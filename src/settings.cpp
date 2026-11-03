/* tod.ini loader: see settings.h. Logic follows dod3-recomp's
 * load_settings_file (MIT). */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static std::string s_path = "tod.ini";

static int tod_putenv(const char* name, const char* value)
{
#ifdef _WIN32
    return _putenv_s(name, value) ? -1 : 0;
#else
    return setenv(name, value, 1);
#endif
}

const char* tod_settings_path() { return s_path.c_str(); }

int tod_settings_load(const char* argv0)
{
    std::string beside;
    std::string a = argv0 ? argv0 : "";
#ifdef _WIN32
    /* 1.01: the exe's real path (argv[0] is only "tod" when it was started from PATH or by some launchers) */
    { char m[MAX_PATH]; const DWORD n = GetModuleFileNameA(NULL, m, sizeof m); if (n && n < sizeof m) a = m; }
#endif
    {
        const size_t cut = a.find_last_of("/\\");
        if (cut != std::string::npos) beside = a.substr(0, cut + 1) + "tod.ini";
    }
    /* 1.01: the release ships tod.default.ini (an update unzipped over an older version must not replace the
     * player's tod.ini); the first start makes tod.ini from it. */
    if (!beside.empty()) {
        FILE* t = fopen(beside.c_str(), "r");
        if (t) fclose(t);
        else {
            const std::string def = beside.substr(0, beside.size() - 7) + "tod.default.ini";
            FILE* in = fopen(def.c_str(), "rb");
            FILE* out = in ? fopen(beside.c_str(), "wb") : NULL;
            if (in && out) {
                char buf[8192];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
                fprintf(stderr, "[settings] made %s from tod.default.ini\n", beside.c_str());
            }
            if (out) fclose(out);
            if (in) fclose(in);
        }
    }
    const std::string tried[2] = { beside, "tod.ini" };
    FILE* f = NULL;
    const char* used = NULL;
    for (int i = 0; i < 2 && !f; i++)
        if (!tried[i].empty() && (f = fopen(tried[i].c_str(), "r")) != NULL) used = tried[i].c_str();
    s_path = used ? used : !beside.empty() ? beside : "tod.ini";
    if (!f) return 0;

    char line[512];
    int applied = 0;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || *p == '\n' || *p == '\r' || !*p) continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        char* ke = eq;                                   /* key: trimmed */
        while (ke > p && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = 0;
        char* v = eq + 1;                                /* value: up to a comment, trimmed */
        while (*v == ' ' || *v == '\t') v++;
        for (char* c = v; *c; c++)
            if (*c == '#' || *c == ';' || *c == '\n' || *c == '\r') {
                *c = 0;
                break;
            }
        char* ve = v + strlen(v);
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        *ve = 0;
        if (!*p || !*v) continue;                        /* an empty value is "unset" */
        const char* cur = getenv(p);
        if (cur && *cur) continue;                       /* the environment wins */
        if (tod_putenv(p, v) == 0) applied++;
    }
    fclose(f);
    if (applied) fprintf(stderr, "[settings] %d from %s\n", applied, used);
    return applied;
}
