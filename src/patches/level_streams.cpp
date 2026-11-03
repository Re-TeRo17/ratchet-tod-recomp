/* Level music/ambience streams: /built/levels/<level>/streaming_sound.dat.
 *
 * FACTS (log\aud101, 03/11): the game opens
 *   /dev_hdd0/game/BCES00052/USRDIR/built/levels/<level>/streaming_sound.dat
 * (twice per level), gets ENOENT and then NEVER reads that entry from
 * packed/levels/<level>/level_uncached.psarc: with the file missing there is no
 * streaming read at all after the level load. When the same bytes (the psarc entry, stored
 * uncompressed) are present at that HDD path the game opens them and streams them at
 * ~50 KB/s for the whole run (XVAG 6 channel 44.1 kHz). This hook makes that file appear: on
 * a failed read-only open of that path, the psarc entry is extracted once into
 * cache\level_streams\<level>\ and the open is retried there.
 * TOD_LEVEL_STREAMS=0 turns it off (1.01: on by default). Read-only on the disc, writes only under cache\. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <filesystem>
#include <zlib.h>

extern "C" int (*ps3_fs_open_fallback)(const char* guest, char* host_path, size_t cap);   /* ps3recomp runtime/ppu/ppu_fs.cpp */

namespace {
namespace fs = std::filesystem;

uint64_t be(const uint8_t* p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = (v << 8) | p[i]; return v; }

/* Offset and length of `want` inside a PSARC when it is stored uncompressed. */
bool psarc_find(const std::string& path, const std::string& want, uint64_t* off, uint64_t* len)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = false;
    do {
        uint8_t h[32];
        if (fread(h, 1, 32, f) != 32 || memcmp(h, "PSAR", 4) != 0) break;
        const uint32_t tocsz = (uint32_t)be(h + 12, 4), entsz = (uint32_t)be(h + 16, 4), nent = (uint32_t)be(h + 20, 4), blk = (uint32_t)be(h + 24, 4);
        if (tocsz < 32 || entsz < 30 || nent == 0 || blk == 0 || tocsz > (64u << 20)) break;
        std::vector<uint8_t> toc(tocsz - 32);
        if (fread(toc.data(), 1, toc.size(), f) != toc.size()) break;
        const int bsz = blk <= 0x10000 ? 2 : blk <= 0x1000000 ? 3 : 4;
        const size_t nblocks = (toc.size() - (size_t)entsz * nent) / bsz;
        auto bs = [&](size_t i) -> uint32_t { return (uint32_t)be(&toc[(size_t)entsz * nent + i * bsz], bsz); };
        auto ent = [&](uint32_t i, uint32_t* z, uint64_t* ul, uint64_t* bo) { const uint8_t* e = &toc[(size_t)entsz * i]; *z = (uint32_t)be(e + 16, 4); *ul = be(e + 20, 5); *bo = be(e + 25, 5); };
        /* entry 0 = manifest: names separated by '\n' */
        uint32_t z; uint64_t ul, bo; ent(0, &z, &ul, &bo);
        if (ul > (16u << 20)) break;
        std::string names; names.reserve((size_t)ul);
        if (fseek(f, (long)bo, SEEK_SET)) break;
        for (size_t b = z; names.size() < ul && b < nblocks; b++) {
            const uint32_t sz = bs(b) ? bs(b) : blk;
            std::vector<uint8_t> c(sz);
            if (fread(c.data(), 1, sz, f) != sz) { names.clear(); break; }
            std::vector<uint8_t> o(blk + 16);
            uLongf dl = (uLongf)o.size();
            if (sz < blk && uncompress(o.data(), &dl, c.data(), sz) == Z_OK) names.append((char*)o.data(), dl);
            else names.append((char*)c.data(), sz);
        }
        if (names.size() < ul) break;
        size_t pos = 0; uint32_t idx = 1;
        while (pos < names.size() && idx < nent) {
            size_t e = names.find('\n', pos); if (e == std::string::npos) e = names.size();
            if (names.compare(pos, e - pos, want) == 0) {
                ent(idx, &z, &ul, &bo);
                uint64_t tot = 0, rem = ul;
                for (size_t b = z; rem > 0 && b < nblocks; b++) { const uint32_t sz = bs(b) ? bs(b) : blk; tot += sz; rem = rem > blk ? rem - blk : 0; }
                if (tot == ul) { *off = bo; *len = ul; ok = true; }   /* stored raw */
                break;
            }
            pos = e + 1; idx++;
        }
    } while (0);
    fclose(f);
    return ok;
}

bool extract_entry(const std::string& psarc, uint64_t off, uint64_t len, const fs::path& out)
{
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    const fs::path tmp = out.string() + ".part";
    FILE* in = fopen(psarc.c_str(), "rb");
    FILE* o = in ? fopen(tmp.string().c_str(), "wb") : nullptr;
    if (!in || !o) { if (in) fclose(in); return false; }
    bool ok = fseek(in, (long)off, SEEK_SET) == 0;
    std::vector<uint8_t> buf(1u << 20);
    for (uint64_t rem = len; ok && rem;) {
        const size_t n = fread(buf.data(), 1, (size_t)(rem < buf.size() ? rem : buf.size()), in);
        if (!n || fwrite(buf.data(), 1, n, o) != n) ok = false; else rem -= n;
    }
    fclose(in);
    if (fclose(o) != 0) ok = false;
    if (!ok) { fs::remove(tmp, ec); return false; }
    fs::rename(tmp, out, ec);
    return !ec;
}

int level_stream_fallback(const char* guest, char* host_path, size_t cap)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_LEVEL_STREAMS"); on = !(e && *e == '0'); }   /* 1.01: default ON (the game reads the files it expects, as on a PS3 with its HDD data); =0 = 1.0 behaviour */
    if (!on) return 0;
    /* /dev_hdd0/game/<title>/USRDIR/built/levels/<level>/streaming_*.dat */
    static const char* mid = "/USRDIR/built/levels/";
    if (strncmp(guest, "/dev_hdd0/game/", 15) != 0) return 0;
    const char* m = strstr(guest, mid);
    if (!m) return 0;
    const char* lvl = m + strlen(mid);
    const char* sl = strchr(lvl, '/');
    if (!sl) return 0;
    const std::string level(lvl, sl - lvl), file(sl + 1);
    if (file.find('/') != std::string::npos || file.compare(0, 10, "streaming_") != 0 || file.size() < 14 || file.compare(file.size() - 4, 4, ".dat") != 0 || level.empty() || level.find("..") != std::string::npos) return 0;
    /* the disc: <root>/PS3_GAME/USRDIR/packed/levels/<level>/level_uncached.psarc; the failed host path is
     * "<root>/game/<title>/USRDIR/...", so the disc root is what precedes "/game/<title>". */
    const std::string hp(host_path);
    const size_t g = hp.find("/game/BCES");
    const std::string root = g == std::string::npos ? "game/disc" : hp.substr(0, g);
    const std::string psarc = root + "/PS3_GAME/USRDIR/packed/levels/" + level + "/level_uncached.psarc";
    const std::string want = "/built/levels/" + level + "/" + file;
    const char* cd = getenv("TOD_LEVEL_STREAMS_DIR");
    const fs::path out = fs::path(cd && *cd ? cd : "cache/level_streams") / level / file;
    std::error_code ec;
    uint64_t off = 0, len = 0;
    if (!psarc_find(psarc, want, &off, &len)) {
        fprintf(stderr, "[level-streams] %s: not in %s (or stored compressed)\n", want.c_str(), psarc.c_str());
        return 0;
    }
    if (!(fs::exists(out, ec) && fs::file_size(out, ec) == len)) {
        if (!extract_entry(psarc, off, len, out)) return 0;
        fprintf(stderr, "[level-streams] %s extracted from the psarc (%llu bytes) to %s\n", want.c_str(), (unsigned long long)len, out.generic_string().c_str());
    }
    snprintf(host_path, cap, "%s", out.generic_string().c_str());
    return 1;
}
}  // namespace

extern "C" void tod_level_streams_init(void) { ps3_fs_open_fallback = level_stream_fallback; }
