/* What the Discord status says right now, derived from the running game (guest memory). */
#pragma once

struct TodPresence {
    const char* details;    /* "Metropolis, Kerwan"; nullptr when the level is not known */
    const char* state;      /* "Playing", "In the menu"; nullptr when not known */
    bool operator==(const TodPresence& o) const;
};

/* Called from the Discord thread only. Static strings; never blocks, never takes a game lock. */
TodPresence tod_discord_presence();
