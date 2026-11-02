/* The installer's disc reader: see install/iso.h. */
#include "install/iso.h"
#include "install/crypto.h"
#include "setup_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>

namespace todsetup {

const char* const kEbootElfSha256 = "cd2c37351e840fb31810f1be5fe9d7d03860d2c1d9b621f8ee55cdf67a8889ba";
const char* const kEbootBinSha256 = "209adbf717391bd223941993526895f0e03bb5f98436cd67eef92f593c08f7dd";
const char* const kTitleId = "BCES00052";
const char* const kAppVer = "01.00";

static std::string utf8_generic(const fs::path& p)
{
    const auto s = p.generic_u8string();
    return std::string(s.begin(), s.end());
}

std::string sha256_file(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    Sha256 s;
    std::unique_ptr<char[]> buf(new char[1 << 20]);
    while (f) {
        f.read(buf.get(), 1 << 20);
        if (f.gcount() > 0) s.update((const uint8_t*)buf.get(), (size_t)f.gcount());
    }
    return s.hex();
}

/* ---- PARAM.SFO -------------------------------------------------------------- */
bool param_sfo(const std::vector<uint8_t>& d, std::string* title_id, std::string* app_ver)
{
    auto le32 = [&](size_t o) {
        return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24;
    };
    auto le16 = [&](size_t o) { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8; };
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4) != 0) return false;
    const uint32_t kt = le32(8), dt = le32(12), n = le32(16);
    if ((uint64_t)20 + (uint64_t)n * 16 > d.size()) return false;
    for (uint32_t i = 0; i < n; i++) {
        const size_t e = 20 + (size_t)i * 16;
        const uint32_t ko = le16(e), len = le32(e + 4), dof = le32(e + 12);
        if (kt + ko >= d.size() || (uint64_t)dt + dof + len > d.size()) continue;
        const char* key = (const char*)&d[kt + ko];
        std::string val((const char*)&d[dt + dof], len);
        val = val.c_str();                                   /* to the first NUL */
        if (!strncmp(key, "TITLE_ID", 9) && title_id) *title_id = val;
        if (!strncmp(key, "APP_VER", 8) && app_ver) *app_ver = val;
    }
    return true;
}

bool disc_installed(const fs::path& base)
{
    std::error_code ec;
    return fs::exists(base / "game/disc/PS3_GAME/PARAM.SFO", ec) &&
           fs::exists(base / "game/disc/PS3_GAME/USRDIR/EBOOT.BIN", ec);
}

/* ---- ISO 9660 ------------------------------------------------------------------
 * The primary volume descriptor's tree, or Joliet's when the image has one (its
 * names are not truncated). A file over 4 GB is stored as several records flagged
 * "multi-extent"; they are joined into one DiscFile with its extents in `more`. */
namespace {
constexpr uint32_t kSector = 2048;

struct Iso {
    std::ifstream f;
    bool read(uint64_t off, void* dst, size_t n)
    {
        f.clear();
        f.seekg((std::streamoff)off);
        f.read((char*)dst, (std::streamsize)n);
        return (size_t)f.gcount() == n;
    }
};

uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

std::string iso_name(const uint8_t* p, size_t n, bool joliet)
{
    std::string s;
    if (joliet) {
        for (size_t i = 0; i + 1 < n; i += 2) {
            const uint32_t c = (uint32_t)p[i] << 8 | p[i + 1];
            if (c < 0x80)
                s += (char)c;
            else if (c < 0x800) {
                s += (char)(0xC0 | (c >> 6));
                s += (char)(0x80 | (c & 0x3F));
            } else {
                s += (char)(0xE0 | (c >> 12));
                s += (char)(0x80 | ((c >> 6) & 0x3F));
                s += (char)(0x80 | (c & 0x3F));
            }
        }
    } else {
        s.assign((const char*)p, n);
    }
    const size_t semi = s.find(';');
    if (semi != std::string::npos) s.resize(semi);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

bool walk(Iso& iso, uint32_t lba, uint32_t len, const std::string& prefix, bool joliet, std::vector<DiscFile>* out,
          int depth, std::string* err)
{
    if (depth > 32) {
        *err = "the disc image's folders nest too deep (not an ISO 9660 image?)";
        return false;
    }
    std::vector<uint8_t> dir(len);
    if (!iso.read((uint64_t)lba * kSector, dir.data(), len)) {
        *err = "the disc image is truncated";
        return false;
    }
    DiscFile pend;                       /* a multi-extent file being collected */
    bool pending = false;
    for (uint32_t o = 0; o < len;) {
        const uint8_t rl = dir[o];
        if (rl == 0) {
            o = (o / kSector + 1) * kSector;
            continue;
        }      /* records do not span sectors */
        if (o + rl > len || rl < 34) break;
        const uint8_t* r = &dir[o];
        const uint32_t ext = le32(r + 2), size = le32(r + 10);
        const uint8_t flags = r[25], nl = r[32];
        o += rl;
        if (nl == 1 && (r[33] == 0 || r[33] == 1)) continue;            /* . and .. */
        const std::string name = iso_name(r + 33, nl, joliet);
        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (flags & 0x02) {
            if (!walk(iso, ext, size, path, joliet, out, depth + 1, err)) return false;
            continue;
        }
        const std::pair<uint64_t, uint64_t> e((uint64_t)ext * kSector, size);
        if (pending && pend.path != path) {
            *err = "the disc image has a broken multi-extent file: " + pend.path;
            return false;
        }
        if (!pending && (flags & 0x80)) {
            pend = DiscFile();
            pend.path = path;
            pend.offset = e.first;
            pending = true;
        }
        if (pending) {
            pend.more.push_back(e);
            pend.size += e.second;
            if (!(flags & 0x80)) {
                out->push_back(pend);
                pending = false;
            }
        } else {
            out->push_back({ path, e.first, e.second, {} });
        }
    }
    if (pending) {
        *err = "the disc image has a broken multi-extent file: " + pend.path;
        return false;
    }
    return true;
}

bool list_iso(const fs::path& p, std::vector<DiscFile>* files, std::string* err)
{
    Iso iso;
    iso.f.open(p, std::ios::binary);
    if (!iso.f) {
        *err = "the disc image could not be opened";
        return false;
    }
    uint8_t vd[kSector];
    const uint8_t* root = nullptr;
    bool joliet = false;
    uint8_t pvd_root[34] = {}, svd_root[34] = {};
    bool have_pvd = false, have_svd = false;
    for (uint32_t s = 16; s < 64; s++) {
        if (!iso.read((uint64_t)s * kSector, vd, kSector)) break;
        if (memcmp(vd + 1, "CD001", 5) != 0) break;
        if (vd[0] == 1 && !have_pvd) {
            memcpy(pvd_root, vd + 156, 34);
            have_pvd = true;
        }
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            memcpy(svd_root, vd + 156, 34);
            have_svd = true;
        }
        if (vd[0] == 255) break;
    }
    if (!have_pvd) {
        *err = "this is not an ISO 9660 disc image";
        return false;
    }
    if (have_svd) {
        root = svd_root;
        joliet = true;
    } else
        root = pvd_root;
    return walk(iso, le32(root + 2), le32(root + 10), "", joliet, files, 0, err);
}

bool list_folder(const fs::path& p, std::vector<DiscFile>* files, std::string* err)
{
    std::error_code ec;
    for (fs::recursive_directory_iterator it(p, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = utf8_generic(fs::relative(it->path(), p, ec));
        files->push_back({ rel, 0, (uint64_t)it->file_size(ec) });
    }
    if (ec) {
        *err = "the folder could not be read: " + ec.message();
        return false;
    }
    return true;
}

/* A redump-style .iso still under disc encryption starts with a region table in
 * sector 0 (a big-endian region count, then ascending sector boundaries); a
 * decrypted image has zeros there. */
bool iso_has_region_table(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    uint8_t s[kSector];
    if (!f || !f.read((char*)s, sizeof s)) return false;
    const uint32_t n = tod_be32(s);
    if (n == 0 || n > 100) return false;
    uint32_t prev = 0;
    for (uint32_t i = 0; i < 2 * n; i++) {
        const uint32_t v = tod_be32(s + 8 + 4 * i);
        if (v < prev) return false;
        prev = v;
    }
    return prev > 0;
}

const DiscFile* find(const Disc& d, const std::string& path)
{
    for (const auto& f : d.files) {
        if (f.path.size() != path.size()) continue;
        bool same = true;                                   /* ISO names are upper case; folders may not be */
        for (size_t i = 0; i < path.size() && same; i++)
            same = toupper((unsigned char)f.path[i]) == toupper((unsigned char)path[i]);
        if (same) return &f;
    }
    return nullptr;
}
}  // namespace

bool read_disc_file(const Disc& disc, const std::string& path, std::vector<uint8_t>* out)
{
    const DiscFile* f = find(disc, path);
    if (!f || f->size > (256u << 20)) return false;
    out->resize((size_t)f->size);
    if (disc.is_iso) {
        std::ifstream in(disc.source, std::ios::binary);
        if (!f->more.empty()) {
            uint64_t at = 0;
            for (const auto& x : f->more) {
                in.clear();
                in.seekg((std::streamoff)x.first);
                in.read((char*)out->data() + at, (std::streamsize)x.second);
                if ((uint64_t)in.gcount() != x.second) return false;
                at += x.second;
            }
            return true;
        }
        in.seekg((std::streamoff)f->offset);
        in.read((char*)out->data(), (std::streamsize)f->size);
        return (uint64_t)in.gcount() == f->size;
    }
    std::ifstream in(disc.source / todsetup::from_utf8(f->path), std::ios::binary);
    in.read((char*)out->data(), (std::streamsize)f->size);
    return (uint64_t)in.gcount() == f->size;
}

bool open_disc(const fs::path& source, Disc* disc, std::string* err)
{
    std::error_code ec;
    disc->source = source;
    disc->files.clear();
    disc->is_iso = fs::is_regular_file(source, ec);
    if (!disc->is_iso) {
        /* The folder that holds PS3_GAME; accept PS3_GAME itself too. */
        if (!fs::exists(source / "PS3_GAME", ec) && fs::exists(source / "PARAM.SFO", ec) &&
            fs::exists(source / "USRDIR", ec))
            disc->source = source.parent_path();
    }
    const bool encrypted_hint = disc->is_iso && iso_has_region_table(disc->source);
    static const char* const kEncMsg =
        "this disc image is still disc-encrypted (a redump .iso as dumped). The installer needs a DECRYPTED image: "
        "decrypt it first (for example with PS3 Disc Dumper or ps3dec and the disc's key), or point --install at the "
        "folder that holds PS3_GAME.";
    if (!(disc->is_iso ? list_iso(disc->source, &disc->files, err) : list_folder(disc->source, &disc->files, err)))
        return false;
    /* What the game reads from the disc: PS3_DISC.SFB and PS3_GAME/. Not an
     * image's PS3_UPDATE (system software), nor, in a folder that is an
     * installed copy, its game/ (the update and DLC) and cache/. */
    auto on_disc = [](const std::string& p) {
        auto starts = [&](const char* s) {
            for (size_t i = 0; s[i]; i++)
                if (i >= p.size() || toupper((unsigned char)p[i]) != s[i]) return false;
            return true;
        };
        return starts("PS3_GAME/") || (starts("PS3_DISC.SFB") && p.size() == 12);
    };
    disc->files.erase(
        std::remove_if(disc->files.begin(), disc->files.end(), [&](const DiscFile& f) { return !on_disc(f.path); }),
        disc->files.end());
    disc->total = 0;
    for (const auto& f : disc->files) disc->total += f.size;

    std::vector<uint8_t> sfo;
    std::string tid, ver;
    if (encrypted_hint && !(read_disc_file(*disc, "PS3_GAME/PARAM.SFO", &sfo) && param_sfo(sfo, &tid, &ver))) {
        *err = kEncMsg;
        return false;
    }
    if (!read_disc_file(*disc, "PS3_GAME/PARAM.SFO", &sfo) || !param_sfo(sfo, &tid, &ver)) {
        *err =
            disc->is_iso
                ? "no PS3_GAME/PARAM.SFO in this disc image: it is not a PS3 game disc, or it is still disc-encrypted"
                : "no PS3_GAME/PARAM.SFO in this folder: choose the folder that contains PS3_GAME";
        return false;
    }
    if (tid != kTitleId) {
        *err = "this is " + tid + ", not Ratchet & Clank: Tools of Destruction (Europe) " + kTitleId + ". Only that release is supported.";
        return false;
    }
    if (ver != kAppVer) {
        *err = "this disc is version " + ver + "; only version " + kAppVer + " is supported.";
        return false;
    }
    std::vector<uint8_t> eboot;
    if (!read_disc_file(*disc, "PS3_GAME/USRDIR/EBOOT.BIN", &eboot)) {
        *err = "the disc has no PS3_GAME/USRDIR/EBOOT.BIN";
        return false;
    }
    Sha256 s;
    s.update(eboot.data(), eboot.size());
    if (s.hex() != kEbootBinSha256) {
        if (encrypted_hint) {
            *err = kEncMsg;
            return false;
        }
        *err = disc->is_iso
                   ? "this disc image's EBOOT.BIN does not match the supported release. If the image came "
                     "straight from a disc drive it is still disc-encrypted: make a decrypted one "
                     "(for example with PS3 Disc Dumper)."
                   : "this folder's EBOOT.BIN does not match the supported release (BCES00052 v01.00).";
        return false;
    }
    return true;
}

bool copy_disc(const Disc& disc, const fs::path& dest, const std::function<bool(uint64_t, uint64_t)>& progress,
               std::string* err)
{
    std::error_code ec;
    fs::create_directories(dest, ec);
    std::ifstream iso;
    if (disc.is_iso) {
        iso.open(disc.source, std::ios::binary);
        if (!iso) {
            *err = "the disc image could not be opened";
            return false;
        }
    }
    std::unique_ptr<char[]> buf(new char[4 << 20]);
    uint64_t done = 0;
    for (const auto& f : disc.files) {
        const fs::path to = dest / todsetup::from_utf8(f.path);
        fs::create_directories(to.parent_path(), ec);
        std::ofstream out(to, std::ios::binary | std::ios::trunc);
        if (!out) {
            *err = "could not write " + utf8(to);
            return false;
        }
        std::ifstream in;
        if (disc.is_iso) {
            iso.clear();
            iso.seekg((std::streamoff)f.offset);
        } else {
            in.open(disc.source / todsetup::from_utf8(f.path), std::ios::binary);
            if (!in) {
                *err = "could not read " + f.path;
                return false;
            }
        }
        std::istream& src = disc.is_iso ? (std::istream&)iso : (std::istream&)in;
        size_t next_ext = 1;                                    /* multi-extent: the extent after the first */
        uint64_t ext_left = (disc.is_iso && !f.more.empty()) ? f.more[0].second : f.size;
        for (uint64_t left = f.size; left;) {
            if (ext_left == 0 && next_ext < f.more.size()) {    /* jump to the next extent */
                iso.clear();
                iso.seekg((std::streamoff)f.more[next_ext].first);
                ext_left = f.more[next_ext].second;
                next_ext++;
            }
            uint64_t want = left < ext_left ? left : ext_left;
            const size_t n = (size_t)(want < (4u << 20) ? want : (4u << 20));
            src.read(buf.get(), (std::streamsize)n);
            if ((size_t)src.gcount() != n) {
                *err = "a read failed in " + f.path + " (the disc image is truncated?)";
                return false;
            }
            out.write(buf.get(), (std::streamsize)n);
            if (!out) {
                *err = "a write failed for " + utf8(to) + " (disk full?)";
                return false;
            }
            left -= n;
            if (ext_left >= n) ext_left -= n;
            done += n;
            if (!progress(done, disc.total)) {
                *err = "cancelled";
                return false;
            }
        }
    }
    return true;
}

}  // namespace todsetup
