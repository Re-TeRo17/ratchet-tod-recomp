/*
 * diff_lock.cpp (16/11) -- TOD_DIFF_LOCK: hold the game's dynamic difficulty at the TOD_DIFFICULTY level.
 *
 * The game keeps a difficulty struct S = [r2-0xEA8] (0x101E8D70 seen) and a tuning table T = [r2-0xEA0]+0x10000.
 * func_002CDDA8 rebuilds the multipliers from the rating r = max(0, S+0 + S+4): ratio = (r - T.FA8 centre) / T.FA8,
 * S+8 = pow(T.FB8, ratio) (hero damage), S+0xC = pow(T.FBC, ratio) (enemy damage), S+0x10, S+0x14 the same with T.FC0/FC4,
 * S+0x18 a clamped linear term, S+0x50 = 1 (wiki 31_modello_di_ratchet, "Difficolta' dinamica").
 * S+0 comes from the play clock (func_002CE21C), S+4 from the TrapDoor result (func_002CE5F0).
 *
 * tools/combat_log_ppu_patch.cmake (tod_lift_rename) renames the lifted definition to func_002CDDA8_lifted; this file
 * provides func_002CDDA8. With TOD_DIFF_LOCK=1 it puts the centre rating in S+0 and 0 in S+4 for the call only, so the
 * game itself computes its medium multipliers (ratio 0), then puts the real inputs back: the clock keeps running and
 * turning the lock off returns the real value at the next call.
 *
 * TOD_DIFF_LOCK = 1 medium (default since 1.3.0: the dynamic rating climbs to the top even in slow play; the clock
 * input is still to fix), 0 = the game's own dynamic difficulty. TOD_DIFFICULTY (1.3.0: a 5-step slider in GAME
 * OPTIONS, medium in the middle) picks the locked rating: story 0, easy 2.5, medium 5 (unset), hard 7.5, extreme 10,
 * i.e. enemy damage x0.5 .. x2 and hero damage x2 .. x0.5 with the stock difficulty.csv. TOD_DIFF_LOG=1 logs "[diff]" when the unlocked rating or multipliers change.
 * Read every 30 frames by the func_002CE5F0 wrapper below, which rebuilds the multipliers at once on a change, so the
 * GAME OPTIONS row / tod.ini / the F1 page change it live (applied when the game runs again after the pause menu).
 */
#include "ppu_recomp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include "platform/tod_platform.h"   /* <windows.h> on Windows */

void func_002CDDA8_lifted(ppu_context* ctx);
void func_002CE5F0_lifted(ppu_context* ctx);

static float rdf(uint32_t a)
{
    const uint32_t v = vm_read32(a);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static void wrf(uint32_t a, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    vm_write32(a, v);
}

static bool lock = true, log_on = false;
static float level = 1.f; // the locked rating as a fraction of the centre: 0 story .. 1 medium .. 2 extreme

/* Reads TOD_DIFF_LOCK / TOD_DIFFICULTY / TOD_DIFF_LOG; true when the lock or the level changed. */
static bool read_env()
{
    /* The menu changes these with _putenv_s from the window thread: the CRT getenv pointer can be freed under us, so read the
     * process environment (kept in step by _putenv_s, locked by the OS) into local buffers. */
    char le[8] = "", d[16] = "", lg[8] = "";
    GetEnvironmentVariableA("TOD_DIFF_LOCK", le, sizeof le);
    GetEnvironmentVariableA("TOD_DIFFICULTY", d, sizeof d);
    GetEnvironmentVariableA("TOD_DIFF_LOG", lg, sizeof lg);
    const bool l = le[0] != '0'; // on unless "0"
    float lv = 1.f;
    if (!_stricmp(d, "story")) lv = 0.f;
    else if (!_stricmp(d, "easy")) lv = 0.5f;
    else if (!_stricmp(d, "hard")) lv = 1.5f;
    else if (!_stricmp(d, "extreme")) lv = 2.f;
    log_on = lg[0] == '1';
    const bool changed = l != lock || lv != level;
    if (changed) fprintf(stderr, "[diff] lock: %s, level %.1f\n", l ? "on" : "off", lv);
    lock = l;
    level = lv;
    return changed;
}

/* func_002CE5F0 runs every frame but calls func_002CDDA8 only when its inputs move, so a change made in the menu would wait
 * for the game's next rebuild. Every 30 frames the settings are read again; on a change the multipliers are rebuilt here at
 * once (a change applies without a restart). The call is made at this function's entry with a copy of
 * the context, which is put back afterwards: func_002CDDA8 only reads its globals through r2 and keeps below r1. */
void func_002CE5F0(ppu_context* ctx)
{
    static int n = 0;
    static bool first = true;
    if (n-- <= 0) {
        n = 30;
        const bool changed = read_env();
        if (changed && !first) {
            ppu_context saved = *ctx;
            func_002CDDA8(ctx);
            *ctx = saved;
        }
        first = false;
    }
    func_002CE5F0_lifted(ctx);
}

void func_002CDDA8(ppu_context* ctx)
{
    static bool init = false;
    static float last_r = -1.f, last_h = -1.f, last_e = -1.f;
    if (!init) {
        init = true;
        read_env();
    }
    const uint32_t r2 = (uint32_t)ctx->gpr[2];
    const uint32_t s = vm_read32(r2 - 0xEA8);
    const uint32_t t = vm_read32(r2 - 0xEA0) + 0x10000;
    const bool ok = s >= 0x10000 && s < 0x40000000u && t >= 0x10000 && t < 0x40000000u;
    if (!ok) {
        func_002CDDA8_lifted(ctx);
        return;
    }
    const float in0 = rdf(s + 0), in4 = rdf(s + 4);
    if (log_on) {
        const float c = rdf(t + 0xFA8);
        const float r = lock ? c * level : fmaxf(0.f, in0 + in4); // locked: the rating in effect, not the clock's
        if (fabsf(r - last_r) >= 0.25f) {
            const float ratio = c != 0.f ? (r - c) / c : 0.f;
            const float h = powf(rdf(t + 0xFB8), ratio), e = powf(rdf(t + 0xFBC), ratio);
            if (fabsf(h - last_h) > 0.01f || fabsf(e - last_e) > 0.01f || last_r < 0.f) {
                fprintf(stderr, "[diff] rating %.2f (clock %.2f trapdoor %.2f, centre %.2f) -> hero dmg x%.3f enemy dmg x%.3f%s\n", r, in0, in4, c, h, e,
                        lock ? " (locked)" : "");
                last_h = h;
                last_e = e;
            }
            last_r = r;
        }
    }
    if (!lock) {
        func_002CDDA8_lifted(ctx);
        return;
    }
    wrf(s + 0, rdf(t + 0xFA8) * level); // centre x level: ratio (r - c) / c = level - 1, from -1 (story) to +1 (extreme)
    wrf(s + 4, 0.f);
    func_002CDDA8_lifted(ctx);
    wrf(s + 0, in0);
    wrf(s + 4, in4);
}
