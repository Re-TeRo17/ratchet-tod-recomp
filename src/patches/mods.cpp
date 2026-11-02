/*
 * Mods folder: a file under mods\ next to tod.exe, at the same path as a game
 * file (relative to the install's game\disc), is read INSTEAD of that file.
 * Nothing of the game is modified; delete the file and the original is back.
 *
 *   mods\PS3_GAME\USRDIR\packed\game\game.psarc   replaces  game\disc\PS3_GAME\USRDIR\packed\game\game.psarc
 *   mods\USRDIR\packed\game\game.psarc            the same (PS3_GAME is optional)
 *   mods\game\BCES00052\USRDIR\...                replaces  game\disc\game\BCES00052\USRDIR\...
 *
 * The lookup itself lives in the runtime (ps3recomp runtime/syscalls/sys_fs.c:
 * ps3_mods_init / ps3_mods_resolve, hooked into cellFs and the sys_fs syscalls,
 * read-only opens and stat only -- saves and every other write are never
 * redirected). This file decides whether it is on and where the folder is.
 *
 *   TOD_MODS = 1 (default) | 0 (off)    -- tod.ini / environment
 *   TOD_MODS_DIR = <folder>             -- test override of the folder (default: mods\ next to tod.exe,
 *                                          else mods\ in the current directory)
 *
 * No folder, or an empty one: nothing is indexed, nothing is printed, and the
 * runtime's hooks return at their first test.
 */
#include <stdlib.h>
#include <string.h>
#include <string>
#include <filesystem>

extern "C" int ps3_mods_init(const char* mods_dir, const char* vfs_root);   /* runtime/syscalls/sys_fs.c */

#ifdef _WIN32
#  include "win32_compat.h"
#endif

extern "C" void tod_mods_init(void)
{
    const char* on = getenv("TOD_MODS");
    if (on && *on == '0') return;

    namespace fs = std::filesystem;
    std::error_code ec;
    std::string dir;
    if (const char* d = getenv("TOD_MODS_DIR")) {
        if (*d) dir = d;
    }
#ifdef _WIN32
    if (dir.empty()) {
        wchar_t exe[1024];
        const DWORD n = GetModuleFileNameW(NULL, exe, 1024);
        if (n > 0 && n < 1024) {
            const fs::path beside = fs::path(exe).parent_path() / "mods";
            if (fs::is_directory(beside, ec)) dir = beside.string();
        }
    }
#endif
    if (dir.empty() && fs::is_directory("mods", ec)) dir = fs::absolute("mods", ec).string();
    if (dir.empty()) return;

    ps3_mods_init(dir.c_str(), getenv("PS3_VFS_ROOT"));
}
