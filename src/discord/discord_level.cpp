/* What the Discord status says: the level that is loaded, read from guest memory.
 *
 * The game's level loader keeps the paths of the level's three archives in a static
 * buffer (EBOOT .bss): "<level>/level_cached", "<level>/level_uncached",
 * "<level>/level_textures", 0x114 bytes apart. It is rewritten at every level load
 * (-level on the command line and the in-game transitions alike), so the first one names
 * the level that is loaded now. <level> is the folder under packed/levels/.
 *
 * Read from the Discord thread without any lock: the buffer is plain bytes, a torn read
 * just fails to match a known level and is ignored (a change must be seen twice in a row
 * before it is shown). Nothing is written to the game. */
#include <cstdint>
#include <cstring>

#include "discord_level.h"

#ifdef _WIN32
#include <windows.h>

extern "C" uint8_t* vm_base;

namespace {

constexpr uint32_t kLevelPathAddr = 0x101E7E50u;    /* "<level>/level_cached" (EBOOT .bss) */
const char kLevelPathSuffix[] = "/level_cached";

/* Engine folder name -> what the status shows. The planet is given where the game's own
 * text names it for sure; otherwise the location alone. */
struct Level { const char* folder; const char* shown; };
const Level kLevels[] = {
    { "metropolis",           "Metropolis, Kerwan" },
    { "cobalia",              "Cobalia" },
    { "fastoon",              "Fastoon" },
    { "fastoon_return",       "Return to Fastoon" },
    { "cragmite ruins",       "Cragmite Ruins" },
    { "apogee space station", "Apogee Space Station" },
    { "imperial fight fest",  "Imperial Fight Festival" },
    { "iris",                 "IRIS Supercomputer" },
    { "kerchu city",          "Kerchu City" },
    { "meridian city",        "Meridian City" },
    { "pirate base",          "Pirate Base" },
    { "rykan v",              "Rykan V" },
    { "sargasso",             "Sargasso" },
    { "slags_fleet",          "Slag's Fleet" },
    { "stratus city",         "Stratus City, Kortog" },
    { "zordoom prison",       "Zordoom Prison" },
    { "space combat i",       "Space Combat" },
    { "space combat ii",      "Space Combat" },
    { "space combat iii",     "Space Combat" },
    /* "level_transitions" is the loading level between planets: not shown */
};
constexpr int kNumLevels = (int)(sizeof kLevels / sizeof kLevels[0]);

/* Index in kLevels of the loaded level; -1 not a known level (or the transition level),
 * -2 guest memory not readable yet, -3 the buffer is empty (no level loaded). */
int read_level()
{
    if (!vm_base) return -2;
    const uint8_t* p = vm_base + kLevelPathAddr;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
        (const uint8_t*)mbi.BaseAddress + mbi.RegionSize < p + 48)
        return -2;
    char buf[48];
    memcpy(buf, p, 47);
    buf[47] = 0;
    if (!buf[0]) return -3;                              /* nothing loaded yet: boot / main menu */
    for (int i = 0; i < kNumLevels; i++) {
        const size_t n = strlen(kLevels[i].folder);
        if (!strncmp(buf, kLevels[i].folder, n) && !strcmp(buf + n, kLevelPathSuffix)) return i;
    }
    return -1;
}

}  // namespace

bool TodPresence::operator==(const TodPresence& o) const
{
    auto eq = [](const char* a, const char* b) { return a == b || (a && b && !strcmp(a, b)); };
    return eq(details, o.details) && eq(state, o.state);
}

TodPresence tod_discord_presence()
{
    static int shown = -2, cand = -2, seen = 0;
    const int now = read_level();
    if (now == cand) { if (seen < 2) seen++; } else { cand = now; seen = 1; }
    if (seen >= 2) shown = cand;
    if (shown >= 0) return { kLevels[shown].shown, "Playing" };
    if (shown == -3) return { nullptr, "In the menu" };
    return { nullptr, nullptr };
}

#else

bool TodPresence::operator==(const TodPresence&) const { return true; }
TodPresence tod_discord_presence() { return { nullptr, nullptr }; }

#endif
