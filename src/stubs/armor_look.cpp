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
    { /* TOD_ARMOR_LOOK_FORCE=1..4 (debug, 09/11): the look of that armor whatever is equipped (Clank tests with an armored Ratchet
       * on a level loaded with -level, where the save's inventory is not used). Look only: protection and saves are unchanged. */
      static int f = -1;
      if (f < 0) { const char* e = getenv("TOD_ARMOR_LOOK_FORCE"); f = e ? atoi(e) : 0; if (f < 0 || f > 4) f = 0;
                   if (f) fprintf(stderr, "[armor] TOD_ARMOR_LOOK_FORCE=%d (debug: armor look forced)\n", f); }
      if (f && ret == 0x001E4D24u) ctx->gpr[3] = (uint64_t)f; }
    if (ret == 0x001E4D24u && ctx->gpr[3] != 0 && armor_hidden()) ctx->gpr[3] = 0;
}

/* barefoot (09/11): the hero's visible mesh groups, a 64-bit mask at moby+0x118 built by func_001E4CC0 from the
 * model's group table ([[moby+0x54]+0x6C], one byte per role: +0x3 head, +0x28..+0x2D the whole-body cheat skins, ...).
 * TOD_MESH_LOG=1 prints the table once and every new mask; TOD_MESH_SHOW / TOD_MESH_HIDE = "g,g,..." (group numbers) add /
 * remove groups (tests). */
static uint64_t parse_groups(const char* e)
{
    uint64_t m = 0;
    while (e && *e) { const int g = atoi(e); if (g >= 0 && g < 64) m |= 1ull << g; e = strchr(e, ','); if (e) e++; }
    return m;
}

uint64_t tod_hero_mesh_mask(uint32_t moby, uint64_t mask)
{
    static int n, logon = -1; static uint64_t show, hide, last = ~0ull;
    if (logon < 0) { const char* e = getenv("TOD_MESH_LOG"); logon = e && e[0] == '1'; }
    if (n-- <= 0) { n = 30; show = parse_groups(getenv("TOD_MESH_SHOW")); hide = parse_groups(getenv("TOD_MESH_HIDE")); }
    if (logon && mask != last) {
        last = mask;
        const uint32_t cls = vm_read32(moby + 0x54), tab = cls ? vm_read32(cls + 0x6C) : 0;
        char t[200] = ""; size_t k = 0;
        for (int i = 0; tab && i < 0x30 && k + 4 < sizeof t; i++) k += (size_t)snprintf(t + k, sizeof t - k, "%02X ", vm_read8(tab + (uint32_t)i));
        fprintf(stderr, "[mesh] hero %08X mask %016llX (show %llX hide %llX) table: %s\n", moby, (unsigned long long)mask,
                (unsigned long long)show, (unsigned long long)hide, t);
    }
    { /* TOD_BAREFOOT=1 (09/11): with the suit body (group 1) the striped skin's body (group 47) is drawn too; the renderer
       * keeps only its feet and drops the suit's boots (rsx_draw_engine.c eng_barefoot). Only the suit, no armor (the user's choice, 09/11). Read ONCE, like the renderer does
       * (its filter is baked into the vertex cache): turned on mid-game it would show the whole striped body; the option is
       * "at the next start" in the menus. */
      static int bf = -1;
      if (bf < 0) { const char* e = getenv("TOD_BAREFOOT"); bf = e && e[0] == '1'; fprintf(stderr, "[armor] barefoot: %s\n", bf ? "on" : "off"); }
      if (bf && (mask & 2ull)) mask |= 1ull << 47;
    }
    return (mask | show) & ~hide;
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
