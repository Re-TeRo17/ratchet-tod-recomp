/*
 * The installer's core, shared by the command line (tod --install <disc>) and,
 * later, a wizard. Modelled on dod3-recomp's src/install/install.h (MIT, see
 * THIRD_PARTY_NOTICES.md), reduced to what this title needs: one decrypted
 * disc, no update, no DLC, a built-in key set.
 *
 * What it does with a disc (a decrypted .iso, or the folder holding PS3_GAME):
 *   1. recognises it: PARAM.SFO TITLE_ID BCES00052 / APP_VER 01.00 and the
 *      SHA-256 of PS3_GAME/USRDIR/EBOOT.BIN (src/install/iso.h);
 *   2. copies PS3_DISC.SFB and PS3_GAME/ into <base>/game/setup.partial/disc;
 *   3. makes the ELF from EBOOT.BIN (src/install/self.h) and checks its
 *      SHA-256, in memory, before anything is moved;
 *   4. moves PS3_DISC.SFB and PS3_GAME into <base>/game/disc and writes
 *      <base>/elf/EBOOT.ELF.
 * A failure or cancel before step 4 leaves the previous install untouched.
 * Anything else in game/disc (the title's own game/ and cache/) is not touched.
 */
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace todsetup {

namespace fs = std::filesystem;

enum class SourceKind { Unknown, Disc, EbootBin, EbootElf };

struct Source {
    SourceKind kind = SourceKind::Unknown;
    std::string what;        /* for the player: one line saying what the file is */
};

/* What is this file or folder? True when it is a supported input; `err` says
 * why not otherwise. `s->what` is filled either way when it can be. */
bool identify(const fs::path& p, Source* s, std::string* err);

/* EBOOT.BIN (SELF) -> EBOOT.ELF with the built-in keys, checked by its hash. */
bool make_elf(const std::vector<uint8_t>& self, std::vector<uint8_t>* elf, std::string* err);

/* What is installed under `base`. */
struct Status {
    bool disc = false, elf = false;
    bool ready() const { return disc && elf; }
};
Status installed(const fs::path& base);

/* progress(done, total, what) returns false to cancel. */
bool install_disc(const fs::path& base, const fs::path& source,
                  const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress, std::string* err);

/* tod --check <file>: print what the installer makes of it. 0 if supported. */
int check_cli(const fs::path& file);

/* tod --install [--base <dir>] <disc>: install, report on stdout. 0 on success. */
int install_cli(const fs::path& base, const fs::path& disc);

/* What to tell a player who starts the game with nothing installed. */
void print_install_instructions(const fs::path& base);

}  // namespace todsetup
