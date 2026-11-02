/*
 * The installer's disc reader: a decrypted ISO 9660 image or a folder holding
 * PS3_GAME, checked to be Ratchet & Clank: Tools of Destruction (BCES00052, version 01.00) by its
 * PARAM.SFO and the hash of its EBOOT.BIN, then read and copied to game/disc/.
 * The whole install is src/install/install.h.
 */
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <filesystem>

namespace todsetup {

namespace fs = std::filesystem;

/* SHA-256 of the build's inputs, as lowercase hex. */
extern const char* const kEbootElfSha256;      /* the disc's EBOOT.BIN decrypted (1.00) */
extern const char* const kEbootBinSha256;      /* the disc's PS3_GAME/USRDIR/EBOOT.BIN */
extern const char* const kTitleId;             /* BCES00052 */
extern const char* const kAppVer;              /* 01.00 */

std::string sha256_file(const fs::path& p);    /* "" if unreadable */

/* PARAM.SFO's TITLE_ID and APP_VER; false if it is not an SFO. */
bool param_sfo(const std::vector<uint8_t>& sfo, std::string* title_id, std::string* app_ver);

/* Is the disc installed under `base` (game/disc)? */
bool disc_installed(const fs::path& base);

/* A disc to install from: an ISO 9660 image or a folder holding PS3_GAME. */
struct DiscFile {
    std::string path;
    uint64_t offset = 0, size = 0;
    /* A file over 4 GB is stored as several ISO 9660 extents (one directory
     * record each, all but the last flagged "multi-extent"). Then `more` lists
     * ALL the extents in order as (image byte offset, length), `offset` is the
     * first one and `size` the whole file. Empty for an ordinary file. */
    std::vector<std::pair<uint64_t, uint64_t>> more;
};   /* path uses '/', offset is the image byte offset */
struct Disc {
    bool is_iso = false;
    fs::path source;
    std::vector<DiscFile> files;
    uint64_t total = 0;
};

/* List the disc and check it is the right one: PARAM.SFO's title and version,
 * and EBOOT.BIN's hash (a disc image still under disc encryption, or another
 * release, fails here). On failure `err` says what to do. */
bool open_disc(const fs::path& source, Disc* disc, std::string* err);

/* Read one file of the disc whole (PARAM.SFO and the like). */
bool read_disc_file(const Disc& disc, const std::string& path, std::vector<uint8_t>* out);

/* Copy the disc's files into `dest` (created). progress(done, total) returns
 * false to cancel. */
bool copy_disc(const Disc& disc, const fs::path& dest, const std::function<bool(uint64_t, uint64_t)>& progress,
               std::string* err);

}  // namespace todsetup
