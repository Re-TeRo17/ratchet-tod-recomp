/*
 * Patched Options archive, made by tod.exe itself (release 1.2.1; the Python tools/menu/menu_patch.py makes the same bytes).
 *
 * The port's rows in the game's own Options page (src/ui/tod_settings_menu.cpp) need the user's global_cached.psarc with
 * the "optHandler" Lua chunk of /built/anark/frontend/built.dat and /built/anark/pause/built.dat replaced by ours
 * (tools/menu/lua/optHandler.lua, compiled to Lua 5.0 bytecode and embedded as menu_opt_handler.inc by
 * tools/menu/gen_menu_inc.py). Nothing of the game is stored in this repository: the patch is spliced into the user's
 * own file at start, into the mods folder (src/patches/mods.cpp), twice (disc copy and HDD copy, same bytes):
 *
 *   <mods>\PS3_GAME\USRDIR\packed\game\global_cached.psarc
 *   <mods>\game\BCES00052\USRDIR\packed\game\global_cached.psarc
 *   <mods>\global_cached.menu.stamp         size + hash of the original's TOC + script id + size of the output
 *
 * Same steps as menu_patch.py: entries found by NAME in the manifest (entry 0), chunk 0 of each built.dat replaced by the
 * chunk fitted to the stock slot (debug info dropped, an unused string constant pads the rest: a chunk of another length
 * moves the later sections and the game fails to load the last script), the psarc rewritten IN PLACE (TOC and block-size
 * table keep their length, the new data is appended at the end). The original is only read.
 *
 *   TOD_MENU=0 : nothing is done.  Any error: one "[menu] skipped: <reason>" line, partial output deleted, stock menu.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <zlib.h>

namespace fs = std::filesystem;

namespace {

#if __has_include("menu_opt_handler.inc")
#include "menu_opt_handler.inc"
#define TOD_HAVE_MENU_SCRIPT 1
#else   /* a source tree without the generated script: builds, and the stock Options menu stays */
#define TOD_HAVE_MENU_SCRIPT 0
#define TOD_MENU_SCRIPT_ID "none"
static const unsigned char kOptHandlerA[1] = { 0 };
static const unsigned char kOptHandlerB[1] = { 0 };
#endif

typedef std::vector<uint8_t> Bytes;

struct Fail { std::string why; };
[[noreturn]] void fail(const std::string& s) { throw Fail{s}; }

uint64_t be(const uint8_t* p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | p[i]; return v; }
void put_be(uint8_t* p, uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; } }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

struct Psarc {
    fs::path path;
    Bytes hdr, toc, tbl;
    uint32_t toc_len = 0, esz = 0, n = 0, bs = 0;
    int w = 2;
    uint64_t file_size = 0;
    struct Ent { uint32_t b0; uint64_t usize, off; };
    std::vector<Ent> ents;
    std::vector<uint32_t> sizes;
    std::ifstream f;

    void open(const fs::path& p) {
        path = p;
        std::error_code ec;
        file_size = fs::file_size(p, ec);
        if (ec) fail("cannot stat " + p.string());
        f.open(p, std::ios::binary);
        if (!f) fail("cannot open " + p.string());
        hdr.resize(32);
        f.read((char*)hdr.data(), 32);
        if (!f || memcmp(hdr.data(), "PSAR", 4) != 0 || memcmp(hdr.data() + 8, "zlib", 4) != 0) fail("not a zlib PSARC");
        toc_len = (uint32_t)be(&hdr[12], 4); esz = (uint32_t)be(&hdr[16], 4);
        n = (uint32_t)be(&hdr[20], 4); bs = (uint32_t)be(&hdr[24], 4);
        w = bs <= 65536 ? 2 : (bs <= 16u * 1024 * 1024 ? 3 : 4);
        if (esz < 30 || (uint64_t)n * esz + 32 > toc_len || toc_len > (64u << 20) || bs == 0 || bs > (64u << 20)) fail("bad PSARC header");
        toc.resize((size_t)n * esz);
        f.read((char*)toc.data(), (std::streamsize)toc.size());
        tbl.resize(toc_len - 32 - toc.size());
        f.read((char*)tbl.data(), (std::streamsize)tbl.size());
        if (!f) fail("short PSARC table of contents");
        ents.resize(n);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t* e = &toc[(size_t)i * esz];
            ents[i] = {(uint32_t)be(e + 16, 4), be(e + 20, 5), be(e + 25, 5)};
        }
        sizes.resize(tbl.size() / w);
        for (size_t i = 0; i < sizes.size(); i++) sizes[i] = (uint32_t)be(&tbl[i * w], w);
    }

    Bytes extract(uint32_t idx) {
        if (idx >= n) fail("entry out of range");
        const Ent& e = ents[idx];
        Bytes out;
        out.reserve((size_t)e.usize);
        f.clear();
        f.seekg((std::streamoff)e.off);
        uint32_t bi = e.b0;
        Bytes raw, dec(bs);
        while (out.size() < e.usize) {
            const size_t want = (size_t)std::min<uint64_t>(bs, e.usize - out.size());
            if (bi >= sizes.size()) fail("block index out of range");
            size_t z = sizes[bi];
            if (z == 0 && want == 65536) z = 65536;
            raw.resize(z);
            f.read((char*)raw.data(), (std::streamsize)z);
            if (!f) fail("short read in an entry");
            if (z == want) out.insert(out.end(), raw.begin(), raw.end());
            else {
                uLongf dl = (uLongf)dec.size();
                if (uncompress(dec.data(), &dl, raw.data(), (uLong)z) != Z_OK) fail("zlib error in an entry");
                out.insert(out.end(), dec.begin(), dec.begin() + dl);
            }
            bi++;
        }
        out.resize((size_t)e.usize);
        return out;
    }
};

/* menu_patch.py pad_fill: PAD0PAD1..PADF, restarting at each gap */
void pad_fill(uint8_t* p, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        const size_t k = i / 4, c = i % 4;
        p[i] = c == 0 ? 'P' : c == 1 ? 'A' : c == 2 ? 'D' : (uint8_t)hex[k & 15];
    }
}

/* lua50_patch.py split_chunks + fit_chunk + rebuild_container, for the case the chunk keeps its length (always true here) */
Bytes patch_dat(const Bytes& d)
{
    if (d.size() < 0x40) fail("built.dat too small");
    const uint32_t nsec = (uint32_t)be(&d[8], 4);
    if (nsec == 0 || nsec > 64 || 0x20 + 16u * nsec > d.size()) fail("bad built.dat section table");
    struct Sec { uint32_t id, off, cnt, size; };
    std::vector<Sec> secs(nsec);
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t* s = &d[0x20 + 16 * i];
        secs[i] = {(uint32_t)be(s, 4), (uint32_t)be(s + 4, 4), (uint32_t)be(s + 8, 4), (uint32_t)be(s + 12, 4)};
    }
    int si = -1;
    for (uint32_t i = 0; i < nsec; i++) if (secs[i].id == 0x20730) { si = (int)i; break; }
    if (si < 0 || (uint32_t)si + 1 >= nsec) fail("no script section in built.dat");
    const Sec sec = secs[si];
    if ((uint64_t)sec.off + sec.size > d.size()) fail("script section outside the file");
    const uint32_t nch = (uint32_t)be(&d[sec.off], 4);
    if (nch == 0 || nch > 4096 || (uint64_t)sec.off + 4 + 4 * (nch + 2) > d.size()) fail("bad script section");
    const uint32_t o0 = (uint32_t)be(&d[sec.off + 4], 4), o1 = (uint32_t)be(&d[sec.off + 8], 4);
    const uint32_t first = sec.off + 4 + 4 * (nch + 2);
    if (o1 <= o0 || (uint64_t)first + o1 > sec.off + (uint64_t)sec.size) fail("bad chunk offsets");
    const uint32_t a = first + o0, slot = o1 - o0;
    /* chunk 0 must be the stock "optHandler" (header 14 + test number 4, then the source name) */
    static const uint8_t head[] = {0x1b, 'L', 'u', 'a', 0x50, 0x01, 0x04, 0x04, 0x04, 0x06, 0x08, 0x09, 0x09, 0x04, 0x3b, 0xaf, 0xef, 0x4b};
    static const uint8_t src[] = {11, 0, 0, 0, 'o', 'p', 't', 'H', 'a', 'n', 'd', 'l', 'e', 'r', 0};
    if (memcmp(&d[a], head, sizeof head) != 0 || memcmp(&d[a + 18], src, sizeof src) != 0) fail("chunk 0 is not optHandler: unknown built.dat");
    const size_t base = sizeof kOptHandlerA + sizeof kOptHandlerB;
    if (slot < base + 6) fail("slot too small for the script");
    const size_t pad = slot - base;                       /* a padding constant is 1 + 4 + (pad - 5) bytes */
    Bytes chunk;
    chunk.reserve(slot);
    chunk.insert(chunk.end(), kOptHandlerA, kOptHandlerA + sizeof kOptHandlerA);
    chunk.push_back(4);
    const uint32_t slen = (uint32_t)(pad - 5);
    for (int i = 0; i < 4; i++) chunk.push_back((uint8_t)(slen >> (8 * i)));
    chunk.insert(chunk.end(), slen - 1, 'P');
    chunk.push_back(0);
    chunk.insert(chunk.end(), kOptHandlerB, kOptHandlerB + sizeof kOptHandlerB);
    if (chunk.size() != slot) fail("internal: chunk size");
    const uint32_t nxt_old = secs[si + 1].off;
    const uint32_t body_end = sec.off + sec.size;
    const uint32_t new_nxt = (body_end + 0x7F) & ~0x7Fu;
    if (new_nxt != nxt_old || body_end > nxt_old) fail("unexpected section alignment in built.dat");
    Bytes out(d);
    memcpy(&out[a], chunk.data(), slot);
    pad_fill(&out[body_end], nxt_old - body_end);
    return out;
}

struct Planned { uint32_t idx; uint64_t usize; std::vector<uint32_t> blocks; Bytes blob; };

Planned plan(uint32_t idx, const Bytes& raw, uint32_t bs, int w)
{
    Planned p; p.idx = idx; p.usize = raw.size();
    Bytes z;
    for (size_t o = 0; o < std::max<size_t>(raw.size(), 1); o += bs) {
        const size_t len = std::min<size_t>(bs, raw.size() - o);
        uLongf zl = compressBound((uLong)len);
        z.resize(zl);
        if (compress2(z.data(), &zl, raw.data() + o, (uLong)len, 9) != Z_OK) fail("zlib compress failed");
        const uint8_t* src = z.data();
        size_t n = zl;
        if (n >= len) { src = raw.data() + o; n = len; }     /* stored */
        p.blob.insert(p.blob.end(), src, src + n);
        p.blocks.push_back((w == 2 && n == 65536) ? 0 : (uint32_t)n);
    }
    return p;
}

void write_file(const fs::path& out, Psarc& s, const std::vector<Planned>& planned)
{
    Bytes tbl(s.tbl), toc(s.toc);
    uint64_t pos = s.file_size;
    for (const Planned& p : planned) {
        const Psarc::Ent& e = s.ents[p.idx];
        const uint64_t old_nb = (e.usize + s.bs - 1) / s.bs;
        if (p.blocks.size() > old_nb) fail("entry needs more blocks than it had");
        for (uint64_t k = 0; k < old_nb; k++) put_be(&tbl[(e.b0 + k) * s.w], k < p.blocks.size() ? p.blocks[k] : 0, s.w);
        uint8_t* t = &toc[(size_t)p.idx * s.esz];
        put_be(t + 20, p.usize, 5);
        put_be(t + 25, pos, 5);
        pos += p.blob.size();
    }
    std::ofstream o(out, std::ios::binary | std::ios::trunc);
    if (!o) fail("cannot create " + out.string());
    o.write((const char*)s.hdr.data(), (std::streamsize)s.hdr.size());
    o.write((const char*)toc.data(), (std::streamsize)toc.size());
    o.write((const char*)tbl.data(), (std::streamsize)tbl.size());
    s.f.clear();
    s.f.seekg(s.toc_len);
    Bytes buf(8u << 20);
    for (;;) {
        s.f.read((char*)buf.data(), (std::streamsize)buf.size());
        const std::streamsize g = s.f.gcount();
        if (g <= 0) break;
        o.write((const char*)buf.data(), g);
    }
    for (const Planned& p : planned) o.write((const char*)p.blob.data(), (std::streamsize)p.blob.size());
    o.flush();
    if (!o) fail("write error (disk full?)");
}

const char* const kTargets[2] = {"/built/anark/frontend/built.dat", "/built/anark/pause/built.dat"};

std::string fingerprint(Psarc& s)
{
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, s.hdr.data(), s.hdr.size());
    h = fnv(h, s.toc.data(), s.toc.size());
    h = fnv(h, s.tbl.data(), s.tbl.size());
    char b[128];
    snprintf(b, sizeof b, "script=%s size=%llu toc=%016llx", TOD_MENU_SCRIPT_ID, (unsigned long long)s.file_size, (unsigned long long)h);
    return b;
}

std::string read_text(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s;
}

void run(const fs::path& mods)
{
    std::error_code ec;
    const char* vr = getenv("PS3_VFS_ROOT");
    const fs::path root = (vr && *vr) ? fs::path(vr) : fs::path("game/disc");
    fs::path src = root / "PS3_GAME" / "USRDIR" / "packed" / "game" / "global_cached.psarc";
    if (!fs::is_regular_file(src, ec)) src = root / "USRDIR" / "packed" / "game" / "global_cached.psarc";
    if (!fs::is_regular_file(src, ec)) fail("original global_cached.psarc not found under " + root.string());
    const fs::path tail = fs::path("packed") / "game" / "global_cached.psarc";
    const fs::path first = mods / "PS3_GAME" / "USRDIR" / tail;
    const fs::path second = mods / "game" / "BCES00052" / "USRDIR" / tail;
    const fs::path stamp = mods / "global_cached.menu.stamp";

    Psarc s;
    s.open(src);
    const std::string fp = fingerprint(s);
    const bool had_stamp = fs::is_regular_file(stamp, ec);
    if (had_stamp && fs::is_regular_file(first, ec) && fs::is_regular_file(second, ec)) {
        const uint64_t n1 = fs::file_size(first, ec), n2 = fs::file_size(second, ec);
        const std::string want = fp + " out=" + std::to_string((unsigned long long)n1) + "\n";
        if (n1 == n2 && read_text(stamp) == want) {
            fprintf(stderr, "[menu] up to date\n");
            return;
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const fs::path t1 = first.string() + ".tmp", t2 = second.string() + ".tmp";
    try {
        if (had_stamp) fs::remove(stamp, ec);
        fs::create_directories(first.parent_path(), ec);
        fs::create_directories(second.parent_path(), ec);
        /* entries by name in the manifest (entry 0); manifest line k = TOC entry k+1 */
        const Bytes man = s.extract(0);
        std::vector<std::string> names;
        { std::string cur; for (uint8_t c : man) { if (c == '\n') { names.push_back(cur); cur.clear(); } else cur += (char)c; } names.push_back(cur); }
        std::vector<std::string> clean;
        for (std::string& nm : names) {
            size_t a = nm.find_first_not_of("\r\0 ", 0, 3), b = nm.find_last_not_of("\r\0 ", std::string::npos, 3);
            if (a != std::string::npos) clean.push_back(nm.substr(a, b - a + 1));
        }
        std::vector<Planned> planned;
        for (const char* t : kTargets) {
            uint32_t idx = 0;
            for (size_t i = 0; i < clean.size(); i++) if (clean[i] == t) { idx = (uint32_t)i + 1; break; }
            if (!idx) fail(std::string("entry not found in the manifest: ") + t);
            planned.push_back(plan(idx, patch_dat(s.extract(idx)), s.bs, s.w));
        }
        write_file(t1, s, planned);
        fs::copy_file(t1, t2, fs::copy_options::overwrite_existing, ec);
        if (ec) fail("cannot copy to " + t2.string());
        const uint64_t outsz = fs::file_size(t1, ec);
        fs::remove(first, ec); fs::remove(second, ec);
        fs::rename(t1, first, ec);
        if (ec) fail("cannot rename into " + first.string());
        fs::rename(t2, second, ec);
        if (ec) fail("cannot rename into " + second.string());
        std::ofstream st(stamp, std::ios::binary | std::ios::trunc);
        st << fp << " out=" << (unsigned long long)outsz << "\n";
        st.close();
        if (!st) fail("cannot write the stamp");
        const long ms = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        fprintf(stderr, "[menu] patched archive built in %ld ms\n", ms);
    } catch (...) {
        fs::remove(t1, ec); fs::remove(t2, ec);
        if (had_stamp) { fs::remove(first, ec); fs::remove(second, ec); fs::remove(stamp, ec); }   /* stale copies of ours must not stay */
        throw;
    }
}

}  // namespace

extern "C" void tod_menu_archive_ensure(const char* mods_dir)
{
    const char* on = getenv("TOD_MENU");
    if (on && !strcmp(on, "0")) return;
    if (!mods_dir || !*mods_dir) return;
    if (!TOD_HAVE_MENU_SCRIPT) { fprintf(stderr, "[menu] skipped: built without menu_opt_handler.inc\n"); return; }
    try {
        run(fs::path(mods_dir));
    } catch (const Fail& f) {
        fprintf(stderr, "[menu] skipped: %s\n", f.why.c_str());
    } catch (const std::exception& e) {
        fprintf(stderr, "[menu] skipped: %s\n", e.what());
    }
}
