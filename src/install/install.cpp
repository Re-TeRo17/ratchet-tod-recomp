/* The installer's core: see install/install.h. Structure after dod3-recomp's
 * src/install/install.cpp (MIT); simplified to a single disc. */
#include "install/install.h"
#include "install/crypto.h"
#include "install/iso.h"
#include "install/self.h"
#include "install/keys_builtin.h"
#include "setup_util.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace todsetup {

namespace {

const char* kEbootElfRel = "elf/EBOOT.ELF";

bool read_file(const fs::path& p, uint64_t max, std::vector<uint8_t>* out)
{
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff n = f.tellg();
    if (n < 0 || (uint64_t)n > max) return false;
    out->resize((size_t)n);
    f.seekg(0);
    f.read((char*)out->data(), n);
    return (std::streamoff)f.gcount() == n;
}

bool write_file(const fs::path& p, const std::vector<uint8_t>& d)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write((const char*)d.data(), (std::streamsize)d.size());
    return (bool)f;
}

bool hex_to(const char* hex, uint8_t* out, size_t n)
{
    std::vector<uint8_t> v;
    if (!from_hex(hex, &v) || v.size() != n) return false;
    memcpy(out, v.data(), n);
    return true;
}

std::string size_text(uint64_t b)
{
    char t[32];
    snprintf(t, sizeof t, "%.2f GB", (double)b / (1024.0 * 1024.0 * 1024.0));
    return t;
}

}  // namespace

bool make_elf(const std::vector<uint8_t>& self, std::vector<uint8_t>* elf, std::string* err)
{
    SelfInfo info;
    if (!self_info(self, &info, err)) return false;
    SelfKeys keys;
    bool found = false;
    for (const auto& k : k_builtin_app_keys)
        if (k.key_revision == info.key_revision && hex_to(k.erk, keys.erk, 32) && hex_to(k.riv, keys.riv, 16)) found = true;
    if (!found) {
        char t[96];
        snprintf(t, sizeof t, "no built-in keys for this EBOOT.BIN's key revision 0x%04X", info.key_revision);
        *err = t;
        return false;
    }
    bool wrong_keys = false;
    if (!self_decrypt(self, keys, elf, &wrong_keys, err)) return false;
    const std::string h = sha256_hex(elf->data(), elf->size());
    if (h != kEbootElfSha256) {
        *err = "the EBOOT.ELF made from this EBOOT.BIN has the wrong SHA-256 (" + h + "); expected " + kEbootElfSha256;
        return false;
    }
    return true;
}

Status installed(const fs::path& base)
{
    Status s;
    std::error_code ec;
    s.disc = disc_installed(base);
    s.elf = fs::exists(base / kEbootElfRel, ec);
    return s;
}

bool identify(const fs::path& p, Source* s, std::string* err)
{
    *s = Source();
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        *err = "no such file or folder";
        return false;
    }
    if (fs::is_directory(p, ec)) {
        Disc d;
        if (!open_disc(p, &d, err)) return false;
        s->kind = SourceKind::Disc;
        s->what = "a disc folder: " + std::string(kTitleId) + " v" + kAppVer + ", " + std::to_string(d.files.size()) +
                  " files, " + size_text(d.total);
        return true;
    }
    uint8_t head[4] = {};
    {
        std::ifstream f(p, std::ios::binary);
        if (f) f.read((char*)head, 4);
    }
    if (!memcmp(head, "SCE\0", 4)) {
        std::vector<uint8_t> self;
        if (!read_file(p, 64u << 20, &self)) {
            *err = "could not read the file";
            return false;
        }
        const std::string h = sha256_hex(self.data(), self.size());
        if (h != kEbootBinSha256) {
            *err = "a SELF, but not this game's 1.00 EBOOT.BIN (SHA-256 " + h + ")";
            return false;
        }
        s->kind = SourceKind::EbootBin;
        s->what = "the supported EBOOT.BIN (encrypted SELF); the installer makes EBOOT.ELF from it";
        return true;
    }
    if (!memcmp(head, "\x7F" "ELF", 4)) {
        const std::string h = sha256_file(p);
        if (h != kEbootElfSha256) {
            *err = "an ELF, but not this game's decrypted EBOOT.ELF (SHA-256 " + h + ")";
            return false;
        }
        s->kind = SourceKind::EbootElf;
        s->what = "the supported decrypted EBOOT.ELF";
        return true;
    }
    Disc d;
    if (!open_disc(p, &d, err)) return false;
    s->kind = SourceKind::Disc;
    s->what = "a decrypted disc image: " + std::string(kTitleId) + " v" + kAppVer + ", " + std::to_string(d.files.size()) +
              " files, " + size_text(d.total);
    return true;
}

bool install_disc(const fs::path& base, const fs::path& source,
                  const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress, std::string* err)
{
    std::error_code ec;
    Disc disc;
    progress(0, 1, "checking the disc");
    if (!open_disc(source, &disc, err)) return false;

    /* the free space: the disc's files plus the ELF and some slack */
    const auto sp = fs::space(base.empty() ? fs::path(".") : base, ec);
    const uint64_t need = disc.total + (64ull << 20);
    if (!ec && sp.available < need) {
        *err = "not enough free space: " + size_text(need) + " needed, " + size_text(sp.available) + " available";
        return false;
    }

    const fs::path game = base / "game";
    const fs::path stage = game / "setup.partial";
    const fs::path stage_disc = stage / "disc";
    fs::remove_all(stage, ec);
    fs::create_directories(stage_disc, ec);
    if (ec) {
        *err = "could not create " + utf8(stage) + ": " + ec.message();
        return false;
    }
    auto cleanup = [&]() { std::error_code e; fs::remove_all(stage, e); };

    if (!copy_disc(disc, stage_disc, [&](uint64_t d, uint64_t t) { return progress(d, t, "copying the disc"); }, err)) {
        cleanup();
        return false;
    }

    /* the executable, from the copy just made (so what is installed is what was checked) */
    progress(0, 1, "making EBOOT.ELF");
    std::vector<uint8_t> self, elf;
    if (!read_file(stage_disc / "PS3_GAME/USRDIR/EBOOT.BIN", 256u << 20, &self)) {
        *err = "the copied disc has no PS3_GAME/USRDIR/EBOOT.BIN";
        cleanup();
        return false;
    }
    if (!make_elf(self, &elf, err)) {
        cleanup();
        return false;
    }
    fs::create_directories(stage / "elf", ec);
    if (!write_file(stage / "elf/EBOOT.ELF", elf)) {
        *err = "could not write the EBOOT.ELF (disk full?)";
        cleanup();
        return false;
    }

    /* into place: replace only what the disc owns; game/disc/game and cache stay */
    progress(1, 1, "moving into place");
    const fs::path dest = game / "disc";
    fs::create_directories(dest, ec);
    fs::create_directories(base / "elf", ec);
    fs::remove_all(dest / "PS3_GAME", ec);
    fs::remove(dest / "PS3_DISC.SFB", ec);
    fs::remove(base / kEbootElfRel, ec);
    fs::rename(stage_disc / "PS3_GAME", dest / "PS3_GAME", ec);
    if (ec) {
        *err = "could not move PS3_GAME into place: " + ec.message();
        return false;
    }
    if (fs::exists(stage_disc / "PS3_DISC.SFB", ec)) fs::rename(stage_disc / "PS3_DISC.SFB", dest / "PS3_DISC.SFB", ec);
    fs::rename(stage / kEbootElfRel, base / kEbootElfRel, ec);
    if (ec) {
        *err = "could not move EBOOT.ELF into place: " + ec.message();
        return false;
    }
    cleanup();
    return true;
}

int check_cli(const fs::path& file)
{
    Source s;
    std::string err;
    if (identify(file, &s, &err)) {
        printf("OK: %s\n", s.what.c_str());
        return 0;
    }
    printf("NOT SUPPORTED: %s\n", err.c_str());
    return 1;
}

void print_install_instructions(const fs::path& base)
{
    printf(
        "Ratchet & Clank: Tools of Destruction (BCES00052, v1.00) is not installed here.\n"
        "This program does not include the game. Install it from your own DECRYPTED disc:\n\n"
        "    tod --install <path to the decrypted .iso, or to the folder that holds PS3_GAME>\n\n"
        "It copies the disc into %s and makes elf\\EBOOT.ELF from its EBOOT.BIN.\n"
        "A raw redump .iso is still disc-encrypted: decrypt it first (PS3 Disc Dumper, ps3dec).\n"
        "'tod --check <file>' tells you what the installer makes of a file.\n",
        utf8(base / "game/disc").c_str());
}

int install_cli(const fs::path& base, const fs::path& disc)
{
    std::string err;
    int last = -1;
    std::string last_what;
    auto progress = [&](uint64_t d, uint64_t t, const std::string& what) {
        const int pct = t ? (int)(d * 100 / t) : 0;
        if (what != last_what) {
            if (!last_what.empty()) printf("\n");
            last_what = what;
            last = -1;
        }
        if (pct / 5 != last / 5 || last < 0) {
            printf("\r%-24s %3d%%", what.c_str(), pct);
            fflush(stdout);
            last = pct;
        }
        return true;
    };
    if (!install_disc(base, disc, progress, &err)) {
        printf("\nInstall failed: %s\n", err.c_str());
        return 1;
    }
    printf("\nInstalled. Start the game with: tod\n");
    return 0;
}

}  // namespace todsetup
