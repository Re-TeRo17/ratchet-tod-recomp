/*
 * Button prompts in the glyphs of the pad the player holds (Xbox, PS4, PS5,
 * Switch, keyboard), in place of the DualShock 3's.
 *
 * The game's text draws a button as a private-use character U+E001..U+E012
 * (circle, square, cross, triangle, SELECT, START, L1, L2, L3, L stick, R1, R2,
 * R3, R stick, D-pad down/up/right/left), taken from one 256x256 DXT5 atlas with
 * a mip chain: /built/fonts/rcf_scea/font.tp, record 3 (docs/TEXTURES.md). The
 * glyphs' rectangles come from font.dat in texels, so a larger atlas drawn at
 * the same rectangles is only sharper.
 *
 * The renderer asks the port about every texture it uploads
 * (rsx_draw_engine_set_texture_standin). For that atlas -- recognised by the
 * FNV-1a 64 of its first mip's bytes -- the answer is another atlas
 * (src/res/prompts/<set>.png, 1024x1024, drawn by tools/prompt_atlas.py, built
 * in as gen/prompts_data.h) averaged down to 1x/2x/4x the game's size
 * according to RSX_SCALE, as one uncompressed level (the game's mips are
 * dropped: at one level a 4x atlas shown smaller than its size aliases a
 * little, hence the scale step).
 *
 *   TOD_PROMPTS = auto (default: player 1's pad; the keyboard's keys when no
 *                 pad is plugged in or the keyboard was touched last)
 *               | ps3 (the game's own art) | ps4 | ps5 | xbox | switch | keyboard
 *   TOD_PROMPTS_CYCLE=<seconds>  step through every set, to look at them
 *
 * Not done (TODO): the HUD's own sprites in hud.tp (records 98 = L1, 103..106 =
 * circle/cross/square/triangle, 187 = R1; 48x44 / 32x32 DXT5 sprites, not
 * glyphs of this atlas) and the pad pictures (hud.tp records 99..102, 175).
 * They need their own hashes and a second atlas drawn at their sizes.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>
#include <zlib.h>
#include "rsx_draw_engine.h"
#include "cellPad.h"
#include "prompts_data.h"   /* gen/: kPromptPng_<set>, from src/res/prompts */

namespace {

/* font.tp record 3: 256x256 CELL_GCM_TEXTURE_COMPRESSED_DXT45 (0x88), mip chain. */
const uint32_t kAtlasW = 256, kAtlasH = 256, kMip0Bytes = 256 * 256;   /* DXT5: 1 byte a texel */
/* FNV-1a 64 of the first mip's 65536 bytes, taken from the disc's
 * /built/fonts/rcf_scea/font.tp (global_cached.psarc entry 1132) at 0x6D680
 * (0x80 + font.tph record 3's offset 448000). Its SHA-1 was
 * 2f1bacd682cbdf5bef46525281372af8c91b5d6d. */
const uint64_t kAtlasHash = 0xCAE974B95105F38Full;
const uint32_t kMasterScale = 4;   /* the PNGs: 1024x1024 */

enum Set { kOriginal, kPs4, kPs5, kXbox, kSwitch, kKeyboard, kSets };
const struct {
    const char* value;   /* TOD_PROMPTS */
    const char* name;
    const unsigned char* png;
    size_t png_bytes;
} kSet[kSets] = {
    { "ps3", "PlayStation 3", nullptr, 0 },
    { "ps4", "PlayStation 4", kPromptPng_ps4, sizeof kPromptPng_ps4 },
    { "ps5", "PlayStation 5", kPromptPng_ps5, sizeof kPromptPng_ps5 },
    { "xbox", "Xbox", kPromptPng_xbox, sizeof kPromptPng_xbox },
    { "switch", "Nintendo Switch", kPromptPng_switch, sizeof kPromptPng_switch },
    { "keyboard", "keyboard", kPromptPng_keyboard, sizeof kPromptPng_keyboard },
};

std::atomic<int> s_set{ kOriginal };
std::atomic<int> s_scale{ 1 };   /* 1, 2 or 4 times the game's atlas */
std::mutex s_mu;                 /* the pixels below */
std::vector<uint8_t> s_master[kSets];
bool s_bad[kSets];
std::vector<uint8_t> s_texels[kSets][3];   /* guest A8R8G8B8 at 1x, 2x, 4x: made once, never moved */

uint64_t fnv1a64(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* An 8-bit RGBA, non-interlaced PNG (what tools/prompt_atlas.py writes). */
bool png_rgba(const uint8_t* p, size_t n, std::vector<uint8_t>& out, uint32_t& w, uint32_t& h)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    if (n < 8 || memcmp(p, sig, 8)) return false;
    std::vector<uint8_t> idat;
    w = h = 0;
    for (size_t o = 8; o + 12 <= n;) {
        const uint32_t len = be32(p + o);
        const uint8_t* type = p + o + 4;
        const uint8_t* data = p + o + 8;
        if (len > n - o - 12) return false;
        if (!memcmp(type, "IHDR", 4) && len >= 13) {
            w = be32(data);
            h = be32(data + 4);
            if (data[8] != 8 || data[9] != 6 || data[12] != 0) return false;   /* depth, colour type, interlace */
        } else if (!memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), data, data + len);
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        o += 12 + (size_t)len;
    }
    if (!w || !h || w > 4096 || h > 4096) return false;
    const size_t stride = (size_t)w * 4;
    std::vector<uint8_t> raw((stride + 1) * h);
    uLongf got = (uLongf)raw.size();
    if (uncompress(raw.data(), &got, idat.data(), (uLong)idat.size()) != Z_OK || got != raw.size()) return false;
    out.assign(stride * h, 0);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* src = raw.data() + (stride + 1) * y;
        uint8_t* row = out.data() + stride * y;
        const uint8_t* up = y ? row - stride : nullptr;
        const int filter = *src++;
        for (size_t x = 0; x < stride; x++) {
            const int a = x >= 4 ? row[x - 4] : 0, b = up ? up[x] : 0, c = (up && x >= 4) ? up[x - 4] : 0;
            int pred = 0;
            switch (filter) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: {
                const int q = a + b - c, pa = abs(q - a), pb = abs(q - b), pc = abs(q - c);
                pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                break;
            }
            default: break;
            }
            row[x] = (uint8_t)(src[x] + pred);
        }
    }
    return true;
}

/* The set's atlas at 1, 2 or 4 times the game's, as guest A8R8G8B8 texels
 * (bytes A,R,G,B in memory order): the master averaged down with its alpha
 * weighted in; a texel with nothing opaque under it keeps the plain average,
 * the edge colour the PNG carries there for bilinear taps. Under s_mu. */
const std::vector<uint8_t>* texels(int set, uint32_t scale)
{
    const int slot = scale >= 4 ? 2 : scale >= 2 ? 1 : 0;
    std::vector<uint8_t>& out = s_texels[set][slot];
    if (!out.empty()) return &out;
    if (s_bad[set]) return nullptr;
    std::vector<uint8_t>& m = s_master[set];
    if (m.empty()) {
        uint32_t w = 0, h = 0;
        if (!kSet[set].png || !png_rgba(kSet[set].png, kSet[set].png_bytes, m, w, h) ||
            w != kAtlasW * kMasterScale || h != kAtlasH * kMasterScale) {
            fprintf(stderr, "[prompts] the %s atlas does not decode\n", kSet[set].name);
            m.clear();
            s_bad[set] = true;
            return nullptr;
        }
    }
    const uint32_t f = kMasterScale >> slot;   /* master texels per texel, each way */
    const uint32_t w = kAtlasW * kMasterScale / f, h = kAtlasH * kMasterScale / f, mw = kAtlasW * kMasterScale;
    out.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sa = 0, pr = 0, pg = 0, pb = 0, r = 0, g = 0, b = 0;
            for (uint32_t j = 0; j < f; j++)
                for (uint32_t i = 0; i < f; i++) {
                    const uint8_t* s = &m[((size_t)(y * f + j) * mw + (x * f + i)) * 4];
                    sa += s[3];
                    pr += s[0] * s[3];
                    pg += s[1] * s[3];
                    pb += s[2] * s[3];
                    r += s[0];
                    g += s[1];
                    b += s[2];
                }
            uint8_t* d = &out[((size_t)y * w + x) * 4];
            d[0] = (uint8_t)(sa / (f * f));
            d[1] = (uint8_t)(sa ? pr / sa : r / (f * f));
            d[2] = (uint8_t)(sa ? pg / sa : g / (f * f));
            d[3] = (uint8_t)(sa ? pb / sa : b / (f * f));
        }
    return &out;
}

int standin(void*, uint32_t fmt, uint32_t w, uint32_t h, uint32_t levels, const uint8_t* bytes, uint32_t span,
            rsx_texture_standin* out)
{
    (void)levels;
    const int set = s_set.load(std::memory_order_relaxed);
    if (set == kOriginal) return 0;
    if (w != kAtlasW || h != kAtlasH || span < kMip0Bytes || (fmt & 0x9Fu) != 0x88u) return 0;
    if (fnv1a64(bytes, kMip0Bytes) != kAtlasHash) return 0;
    const uint32_t scale = (uint32_t)s_scale.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(s_mu);
    const std::vector<uint8_t>* t = texels(set, scale);
    if (!t) return 0;
    out->bytes = t->data();
    out->fmt = 0x85u | 0x20u;   /* CELL_GCM_TEXTURE_A8R8G8B8, linear rows */
    out->w = kAtlasW * scale;
    out->h = kAtlasH * scale;
    out->levels = 1;
    out->pitch = out->w * 4;
    return 1;
}

/* The set TOD_PROMPTS asks for; unset, "auto" or unknown: the one for player 1's pad. */
int wanted_set()
{
    if (const char* e = getenv("TOD_PROMPTS"))
        for (int i = 0; i < kSets; i++)
            if (!strcmp(e, kSet[i].value)) return i;
    switch (cellPad_host_kind(0)) {
    case CELL_PAD_HOST_PS3: return kOriginal;
    case CELL_PAD_HOST_PS4: return kPs4;
    case CELL_PAD_HOST_PS5: return kPs5;
    case CELL_PAD_HOST_SWITCH: return kSwitch;
    case CELL_PAD_HOST_XBOX:
    case CELL_PAD_HOST_OTHER: return kXbox;   /* what a pad of no known family is laid out as */
    default: return kKeyboard;                /* none plugged in, or the keyboard touched last */
    }
}

void poll_once(uint64_t seconds)
{
    int set = wanted_set();
    static int cycle = -1;
    if (cycle < 0) {
        const char* e = getenv("TOD_PROMPTS_CYCLE");
        cycle = e ? atoi(e) : 0;
    }
    if (cycle > 0) set = (int)(seconds / (uint64_t)cycle % kSets);

    /* the atlas is sampled without mip levels: no larger than the picture needs */
    const char* e = getenv("RSX_SCALE");
    const double rs = e ? atof(e) : 1.0;
    const int scale = rs > 2.0 ? 4 : rs > 1.0 ? 2 : 1;

    if (set == s_set.load() && scale == s_scale.load()) return;
    const bool said = set != s_set.load();
    s_set.store(set);
    s_scale.store(scale);
    rsx_draw_engine_texture_standins_changed();
    if (said) fprintf(stderr, "[prompts] button prompts: %s\n", kSet[set].name);
}

} // namespace

/* Called once from main(), after tod_input_init(). */
extern "C" void tod_prompts_init(void)
{
    rsx_draw_engine_set_texture_standin(standin, nullptr);
    poll_once(0);
    std::thread([] {
        for (uint64_t s = 0;; s++) {
            for (int i = 0; i < 4; i++) {   /* four looks a second at the pad kind */
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                poll_once(s);
            }
        }
    }).detach();
}
