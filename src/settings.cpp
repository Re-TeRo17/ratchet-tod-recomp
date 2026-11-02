/* tod.ini loader: see settings.h. Logic follows dod3-recomp's
 * load_settings_file (MIT). */
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

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
    if (argv0) {
        std::string a = argv0;
        const size_t cut = a.find_last_of("/\\");
        if (cut != std::string::npos) beside = a.substr(0, cut + 1) + "tod.ini";
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
