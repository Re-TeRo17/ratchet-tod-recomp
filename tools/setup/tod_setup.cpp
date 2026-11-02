/* tod_setup: the installer on its own (no game code), for the release's install.bat.
 * The same src/install/ code as `tod --install`, in a 300 KB program that starts
 * at once and prints machine-readable progress for the PowerShell front end.
 *
 *   tod_setup --check <disc>                       what the installer makes of <disc>
 *   tod_setup --install [--base <dir>] [--machine] <disc>
 *   tod_setup --status [--base <dir>]              is the game installed in <dir>?
 *
 * <disc> is a DECRYPTED .iso, the folder that holds PS3_GAME, or (--check only)
 * an EBOOT.BIN / EBOOT.ELF. Exit code 0 = success.
 * --machine: lines "PROGRESS <percent> <what>" and "ERROR <text>" instead of the
 * human-readable bar.
 * Build: tools\build_setup.bat */
#include "install/install.h"
#include "setup_util.h"

#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

using namespace todsetup;

static std::string narrow(const wchar_t* w)
{
#ifdef _WIN32
    if (!w || !*w) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    std::string s((size_t)(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, NULL, NULL);
    return s;
#else
    return "";
#endif
}

static int usage()
{
    printf("Ratchet & Clank: Tools of Destruction (BCES00052 v1.00) installer\n\n"
           "  tod_setup --check <disc>\n"
           "  tod_setup --install [--base <dir>] [--machine] <disc>\n"
           "  tod_setup --status [--base <dir>]\n\n"
           "<disc>: a DECRYPTED .iso, or the folder that holds PS3_GAME.\n");
    return 2;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** wargv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> a;
    for (int i = 0; i < argc; i++) a.push_back(narrow(wargv[i]));
#else
int main(int argc, char** argv)
{
    std::vector<std::string> a(argv, argv + argc);
#endif
    if (a.size() < 2) return usage();
    fs::path base = ".", disc;
    bool machine = false;
    for (size_t i = 2; i < a.size(); i++) {
        if (a[i] == "--base" && i + 1 < a.size()) base = from_utf8(a[++i]);
        else if (a[i] == "--machine") machine = true;
        else disc = from_utf8(a[i]);
    }
    if (a[1] == "--status") {
        const Status s = installed(base);
        printf("disc %s, elf %s\n", s.disc ? "yes" : "no", s.elf ? "yes" : "no");
        return s.ready() ? 0 : 1;
    }
    if (a[1] == "--check" && !disc.empty()) return check_cli(disc);
    if (a[1] == "--install" && !disc.empty()) {
        if (!machine) return install_cli(base, disc);
        std::string err;
        int last = -1;
        std::string last_what;
        auto progress = [&](uint64_t d, uint64_t t, const std::string& what) {
            const int pct = t ? (int)(d * 100 / t) : 0;
            if (pct != last || what != last_what) {
                printf("PROGRESS %d %s\n", pct, what.c_str());
                fflush(stdout);
                last = pct;
                last_what = what;
            }
            return true;
        };
        if (!install_disc(base, disc, progress, &err)) {
            printf("ERROR %s\n", err.c_str());
            return 1;
        }
        printf("DONE\n");
        return 0;
    }
    return usage();
}
