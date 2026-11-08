/*
 * armor_look.cpp (1.2.1) -- Ratchet's look with armor: TOD_ARMOR_LOOK = hidden and TOD_REMOVE_HELMET = 1.
 *
 * func_001E2110(r3 = hero) returns the equipped armor ([hero+0x1A68]+0x458, 0 = none .. 4). The per-frame hero update
 * func_001E4CC0 calls it (return address 0x001E4D24) and switches the armor's mesh groups of Ratchet's model on or off from the
 * answer. The armor's protection is a separate value (hero+0x1798, set by func_001E2120 when the armor is equipped), and the
 * vendor, the scripts and the save read the field directly or through other callers. So only that one caller is told "no
 * armor": the look changes, nothing else does.
 *
 * The same update picks the head group from the hero byte +0x21CD (set to 1 when the hero is created, func_001E3398): not 0 ->
 * the equipped armor's helmet, 0 -> Ratchet's own head. TOD_REMOVE_HELMET = 1 makes that one read answer 0, the game's own
 * "Remove Helmet" look: the armor body stays, the helmet goes, nothing is written to the hero.
 *
 * Both hooks come from tools/armor_look_ppu_patch.cmake. Both settings are read again every 30 frames, so a change made in the
 * F1 page or in the Options menu shows at once.
 */
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void func_001E2110_lifted(ppu_context* ctx);

static int env_is(const char* key, const char* const* on)
{
    const char* e = getenv(key);
    if (!e || !*e) return 0;
    for (; *on; on++) if (!strcmp(e, *on)) return 1;
    return 0;
}

static int armor_hidden(void)
{
    static int v, n;
    static const char* const on[] = { "hidden", "none", "0", NULL };
    if (n-- <= 0) {
        n = 30;
        const int nv = env_is("TOD_ARMOR_LOOK", on);
        if (nv != v) fprintf(stderr, "[armor] armor look: %s\n", nv ? "hidden (normal suit; armor and protection unchanged)" : "shown");
        v = nv;
    }
    return v;
}

void func_001E2110(ppu_context* ctx)
{
    const uint32_t ret = (uint32_t)ctx->lr;
    func_001E2110_lifted(ctx);
    if (ret == 0x001E4D24u && ctx->gpr[3] != 0 && armor_hidden()) ctx->gpr[3] = 0;
}

/* the head-group read in func_001E4CC0: r9 = byte hero+0x21CD */
uint64_t tod_helmet_flag(uint64_t v)
{
    static int off, n;
    static const char* const on[] = { "1", "on", "yes", NULL };
    if (n-- <= 0) {
        n = 30;
        const int nv = env_is("TOD_REMOVE_HELMET", on);
        if (nv != off) fprintf(stderr, "[armor] remove helmet: %s\n", nv ? "on (Ratchet's own head with any armor)" : "off");
        off = nv;
    }
    return off ? 0 : v;
}
