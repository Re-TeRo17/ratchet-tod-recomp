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
#include <math.h>
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
#include "win32_compat.h"
#if defined(__APPLE__)
#include <CoreText/CoreText.h>
#include <CoreGraphics/CoreGraphics.h>
#endif
#include "input/key_bindings.h"

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

/* ---- issue #21: the keyboard set shows the keys as bound (KEY_* in tod.ini / the F1 page), not only the defaults ----
 * The keyboard PNG (tools/prompt_atlas.py) has the default keys printed on its key caps. Here, for every button bound to another
 * key, the cap's face is painted again in its own colour and the key's name written on it (Segoe UI Bold, shrunk to fit), with the
 * geometry prompt_atlas.py uses (keycap(): rim, face inset e = min(2.6, w/6), text centre raised e/2). Applied to a copy of the
 * master; the atlas is rebuilt when a binding changes (poll_once). TOD_PROMPTS_KEYS=0 keeps the printed defaults. */
struct Cap { int action; uint32_t cp; float x0, y0, x1, y1, size; };
const float kFaceRgb[3] = { 70, 73, 80 };

void cap_rect(uint32_t cp, int kind, float* x0, float* y0, float* x1, float* y1, float* size)
{
    static const struct { uint32_t cp; float x, y, w, h; } R[] = {
        { 0xE001, 116, 91, 52, 52 }, { 0xE002, 172, 91, 52, 52 }, { 0xE003, 4, 147, 52, 52 }, { 0xE004, 60, 147, 52, 52 },
        { 0xE005, 4, 39, 52, 39 }, { 0xE006, 60, 39, 52, 39 }, { 0xE007, 4, 4, 52, 31 }, { 0xE008, 60, 4, 52, 31 },
        { 0xE00B, 116, 4, 52, 31 }, { 0xE00C, 172, 4, 52, 31 } };
    for (const auto& r : R) {
        if (r.cp != cp) continue;
        if (kind == 0) { *x0 = r.x + 2.5f; *y0 = r.y + 2.5f; *x1 = r.x + r.w - 2.5f; *y1 = r.y + r.h - 2.5f; *size = 24; }   /* face */
        else if (kind == 1) { *x0 = r.x + 2 + 8; *y0 = r.y + 2; *x1 = r.x + r.w - 2 - 8; *y1 = r.y + r.h - 2; *size = 17; }  /* L/R */
        else { const float cy = r.y + r.h / 2; *x0 = r.x + 2; *y0 = cy - 12.5f; *x1 = r.x + r.w - 2; *y1 = cy + 12.5f; *size = 15; }
        return;
    }
}

std::string cap_label(int vk)
{
    std::string n = kb::key_name(vk);
    if (n == "MouseLeft") return "LMB";
    if (n == "MouseRight") return "RMB";
    if (n == "MouseMiddle") return "MMB";
    if (n == "Mouse4") return "M4";
    if (n == "Mouse5") return "M5";
    if (n == "Backspace") return "Bksp";
    if (n == "PageUp") return "PgUp";
    if (n == "PageDown") return "PgDn";
    if (n == "CapsLock") return "Caps";
    if (n == "LShift" || n == "RShift") return "Shift";
    if (n == "LCtrl" || n == "RCtrl") return "Ctrl";
    if (n == "LAlt" || n == "RAlt") return "Alt";
    return n;
}

/* Text coverage (0..255) of `label` in Segoe UI Bold at `px` pixels, its ink box cropped; w/h out. */
#if defined(_WIN32)
std::vector<uint8_t> text_cov(const std::string& label, int px, int* w, int* h)
{
    const int W = 1024, H = 256;
    std::vector<uint8_t> cov;
    HDC dc = CreateCompatibleDC(NULL);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    *w = *h = 0;
    if (dc && bm && bits && f) {
        HGDIOBJ ob = SelectObject(dc, bm), of = SelectObject(dc, f);
        memset(bits, 0, (size_t)W * H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        std::wstring ws(label.begin(), label.end());
        TextOutW(dc, 16, 16, ws.c_str(), (int)ws.size());
        GdiFlush();
        const uint8_t* b = (const uint8_t*)bits;
        int lx = W, ly = H, hx = -1, hy = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (b[((size_t)y * W + x) * 4 + 1]) { if (x < lx) lx = x; if (x > hx) hx = x; if (y < ly) ly = y; if (y > hy) hy = y; }
        if (hx >= lx) {
            *w = hx - lx + 1; *h = hy - ly + 1;
            cov.resize((size_t)*w * *h);
            for (int y = 0; y < *h; y++)
                for (int x = 0; x < *w; x++) cov[(size_t)y * *w + x] = b[((size_t)(ly + y) * W + lx + x) * 4 + 1];
        }
        SelectObject(dc, of); SelectObject(dc, ob);
    }
    if (f) DeleteObject(f);
    if (bm) DeleteObject(bm);
    if (dc) DeleteDC(dc);
    return cov;
}
#elif defined(__APPLE__)
std::vector<uint8_t> text_cov(const std::string& label, int px, int* w, int* h)
{
    /* macOS: the system UI font, bold, drawn white on black into an 8-bit grey bitmap. */
    const int W = 1024, H = 256;
    std::vector<uint8_t> cov, buf((size_t)W * H, 0);
    *w = *h = 0;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
    CGContextRef cg = CGBitmapContextCreate(buf.data(), W, H, 8, W, cs, kCGImageAlphaNone);
    CGColorSpaceRelease(cs);
    if (!cg) return cov;
    CTFontRef font = CTFontCreateUIFontForLanguage(kCTFontUIFontEmphasizedSystem, (CGFloat)px, NULL);
    CFStringRef str = CFStringCreateWithBytes(NULL, (const UInt8*)label.data(), (CFIndex)label.size(), kCFStringEncodingUTF8, false);
    if (font && str) {
        CGContextSetGrayFillColor(cg, 1.0, 1.0);
        CGContextSetShouldAntialias(cg, true);
        const void* keys[] = { kCTFontAttributeName, kCTForegroundColorFromContextAttributeName };
        const void* vals[] = { font, kCFBooleanTrue };
        CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFAttributedStringRef as = CFAttributedStringCreate(NULL, str, attrs);
        CTLineRef line = CTLineCreateWithAttributedString(as);
        CGContextSetTextPosition(cg, 16, H - 16 - px);   /* CG's origin is bottom-left */
        CTLineDraw(line, cg);
        CGContextFlush(cg);
        CFRelease(line); CFRelease(as); CFRelease(attrs);
        /* the bitmap's first row is the top row */
        const uint8_t* b = buf.data();
        int lx = W, ly = H, hx = -1, hy = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (b[(size_t)y * W + x]) { if (x < lx) lx = x; if (x > hx) hx = x; if (y < ly) ly = y; if (y > hy) hy = y; }
        if (hx >= lx) {
            *w = hx - lx + 1; *h = hy - ly + 1;
            cov.resize((size_t)*w * *h);
            for (int y = 0; y < *h; y++)
                for (int x = 0; x < *w; x++) cov[(size_t)y * *w + x] = b[(size_t)(ly + y) * W + lx + x];
        }
    }
    if (str) CFRelease(str);
    if (font) CFRelease(font);
    CGContextRelease(cg);
    return cov;
}
#else
std::vector<uint8_t> text_cov(const std::string& label, int px, int* w, int* h)
{
    (void)label; (void)px; *w = *h = 0;   /* no font rasteriser on this host yet: the cap keeps its face */
    return {};
}
#endif

/* Paints a key cap's face again and writes `label` on it, on the RGBA master (kMasterScale x the atlas). */
void relabel_cap(std::vector<uint8_t>& m, float x0, float y0, float x1, float y1, float size, const std::string& label)
{
    const float Z = (float)kMasterScale, wd = x1 - x0, e = wd / 6 < 2.6f ? wd / 6 : 2.6f, rad = (wd / 6 < 3 ? wd / 6 : 3) * Z;
    const float fx0 = (x0 + e) * Z, fy0 = (y0 + e * 0.8f) * Z, fx1 = (x1 - e) * Z, fy1 = (y1 - e * 1.6f) * Z;
    const uint32_t mw = kAtlasW * kMasterScale;
    for (int y = (int)fy0; y < (int)fy1; y++)
        for (int x = (int)fx0; x < (int)fx1; x++) {
            /* inside the rounded rectangle, with a one-pixel soft edge */
            const float cx = x + 0.5f < fx0 + rad ? fx0 + rad : (x + 0.5f > fx1 - rad ? fx1 - rad : x + 0.5f);
            const float cy = y + 0.5f < fy0 + rad ? fy0 + rad : (y + 0.5f > fy1 - rad ? fy1 - rad : y + 0.5f);
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d = sqrtf(dx * dx + dy * dy) - rad;
            float a = d <= -1.0f ? 1.0f : (d >= 0.0f ? 0.0f : -d);
            if (a <= 0.0f) continue;
            uint8_t* p = &m[((size_t)y * mw + x) * 4];
            for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * (1 - a) + kFaceRgb[c] * a + 0.5f);
        }
    const float fit = (wd - 2 * e - 1.5f) * Z, tcx = (x0 + x1) / 2 * Z, tcy = ((y0 + y1) / 2 - e * 0.5f) * Z;
    int px = (int)(size * Z), tw = 0, th = 0;
    std::vector<uint8_t> cov = text_cov(label, px, &tw, &th);
    if (tw > fit && tw > 0) { px = (int)(px * fit / tw); if (px < 16) px = 16; cov = text_cov(label, px, &tw, &th); }
    const int ox = (int)(tcx - tw / 2.0f + 0.5f), oy = (int)(tcy - th / 2.0f + 0.5f);
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            const int X = ox + x, Y = oy + y;
            if (X < 0 || Y < 0 || X >= (int)mw || Y >= (int)(kAtlasH * kMasterScale)) continue;
            const float a = cov[(size_t)y * tw + x] / 255.0f;
            if (a <= 0) continue;
            uint8_t* p = &m[((size_t)Y * mw + X) * 4];
            for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * (1 - a) + 255 * a + 0.5f);
        }
}

/* The bindings the keyboard atlas shows ("" = all defaults, or TOD_PROMPTS_KEYS=0). */
std::string keys_signature()
{
    const char* e = getenv("TOD_PROMPTS_KEYS");
    if (e && e[0] == '0') return "";
    std::string sig;
    static const int A[] = { kb::CROSS, kb::CIRCLE, kb::SQUARE, kb::TRIANGLE, kb::L1, kb::R1, kb::L2, kb::R2, kb::START, kb::SELECT };
    for (int a : A) {
        const int vk = kb::get(a);
        if (vk && vk != kb::default_vk(a)) { sig += std::to_string(a) + ":" + std::to_string(vk) + ";"; }
    }
    return sig;
}
std::string s_kb_sig;                      /* what s_texels[kKeyboard] was made with; under s_mu */
std::vector<std::vector<uint8_t>> s_retired;   /* atlases replaced after a rebinding: kept (the renderer may still read one) */

void apply_keys(std::vector<uint8_t>& m, const std::string& sig)
{
    static const struct { int action; uint32_t cp; int kind; } C[] = {
        { kb::CIRCLE, 0xE001, 0 }, { kb::SQUARE, 0xE002, 0 }, { kb::CROSS, 0xE003, 0 }, { kb::TRIANGLE, 0xE004, 0 },
        { kb::L1, 0xE007, 1 }, { kb::L2, 0xE008, 1 }, { kb::R1, 0xE00B, 1 }, { kb::R2, 0xE00C, 1 },
        { kb::START, 0xE006, 2 }, { kb::SELECT, 0xE005, 2 } };
    if (sig.empty()) return;
    for (const auto& c : C) {
        const int vk = kb::get(c.action);
        if (!vk || vk == kb::default_vk(c.action)) continue;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0, size = 0;
        cap_rect(c.cp, c.kind, &x0, &y0, &x1, &y1, &size);
        relabel_cap(m, x0, y0, x1, y1, size, cap_label(vk));
    }
    fprintf(stderr, "[prompts] keyboard prompts follow the bindings (%s)\n", sig.c_str());
}

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
    std::vector<uint8_t> km;   /* issue #21: the keyboard master with the rebound keys written on */
    if (set == kKeyboard) { const std::string sig = keys_signature(); if (!sig.empty()) { km = m; apply_keys(km, sig); } s_kb_sig = sig; }
    const std::vector<uint8_t>& src_m = km.empty() ? m : km;
    const uint32_t f = kMasterScale >> slot;   /* master texels per texel, each way */
    const uint32_t w = kAtlasW * kMasterScale / f, h = kAtlasH * kMasterScale / f, mw = kAtlasW * kMasterScale;
    out.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sa = 0, pr = 0, pg = 0, pb = 0, r = 0, g = 0, b = 0;
            for (uint32_t j = 0; j < f; j++)
                for (uint32_t i = 0; i < f; i++) {
                    const uint8_t* s = &src_m[((size_t)(y * f + j) * mw + (x * f + i)) * 4];
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

    if (set == kKeyboard) {   /* issue #21: a key rebound in the F1 page shows at once */
        const std::string sig = keys_signature();
        bool changed = false;
        { std::lock_guard<std::mutex> lk(s_mu);
          if (sig != s_kb_sig && !(s_texels[kKeyboard][0].empty() && s_texels[kKeyboard][1].empty() && s_texels[kKeyboard][2].empty())) {
              for (auto& t : s_texels[kKeyboard]) if (!t.empty()) { s_retired.push_back(std::move(t)); t.clear(); }
              s_kb_sig = sig; changed = true; } }
        if (changed && set == s_set.load() && scale == s_scale.load()) { rsx_draw_engine_texture_standins_changed(); return; }
    }
    if (set == s_set.load() && scale == s_scale.load()) return;
    const bool said = set != s_set.load();
    s_set.store(set);
    s_scale.store(scale);
    rsx_draw_engine_texture_standins_changed();
    if (said) fprintf(stderr, "[prompts] button prompts: %s\n", kSet[set].name);
}

} // namespace

/* 10/11 (#25 point 2, the overlay's pause hints): one glyph of the set on screen (circle U+E001, cross U+E003), as RGBA
 * at 4x the game's atlas (208x208), with the keyboard's rebound keys written on. The PS3 set (the game's own art, no PNG
 * here) gives the PS4 glyphs. Returns a signature that changes with the set or the key bindings (0 = none). */
extern "C" int tod_prompts_glyph(uint32_t cp, uint8_t* rgba, int cap, int* w, int* h)
{
    static const struct { uint32_t cp; int x, y, w, h; } G[] = { { 0xE001, 116, 91, 52, 52 }, { 0xE003, 4, 147, 52, 52 } };
    int k = -1;
    for (int i = 0; i < 2; i++) if (G[i].cp == cp) k = i;
    if (k < 0) return 0;
    int set = s_set.load();
    if (set == kOriginal) set = kPs4;
    const int gw = G[k].w * (int)kMasterScale, gh = G[k].h * (int)kMasterScale;
    if (cap < gw * gh * 4) return 0;
    std::lock_guard<std::mutex> lk(s_mu);
    const std::vector<uint8_t>* t = texels(set, 4);
    if (!t) return 0;
    const int aw = (int)(kAtlasW * kMasterScale);
    for (int y = 0; y < gh; y++)
        for (int x = 0; x < gw; x++) {
            const uint8_t* s = &(*t)[((size_t)(G[k].y * (int)kMasterScale + y) * aw + G[k].x * (int)kMasterScale + x) * 4];   /* A R G B */
            uint8_t* d = rgba + ((size_t)y * gw + x) * 4;
            d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0];
        }
    *w = gw; *h = gh;
    uint32_t hsh = 2166136261u;
    for (char c : s_kb_sig) hsh = (hsh ^ (uint8_t)c) * 16777619u;
    return (int)((hsh & 0xFFFFF) * 8 + set + 1);
}

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
