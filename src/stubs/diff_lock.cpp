/*
 * diff_lock.cpp (16/11) -- TOD_DIFF_LOCK: lock the game's dynamic difficulty at medium.
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
 * input is still to fix), 0 = the game's own dynamic difficulty. TOD_DIFF_LOG=1 logs "[diff]" when the unlocked rating or multipliers change.
 * Read every 30 calls, so tod.ini / the F1 page change it live.
 */
#include "ppu_recomp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void func_002CDDA8_lifted(ppu_context* ctx);

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

static bool env1(const char* k)
{
    const char* e = getenv(k);
    return e && e[0] == '1';
}

void func_002CDDA8(ppu_context* ctx)
{
    static int n = 0;
    static bool lock = true, log = false;
    static float last_r = -1.f, last_h = -1.f, last_e = -1.f;
    if (n-- <= 0) {
        n = 30;
        const char* le = getenv("TOD_DIFF_LOCK");
        const bool l = !(le && le[0] == '0'); // on unless "0"
        if (l != lock) fprintf(stderr, "[diff] lock medium: %s\n", l ? "on" : "off");
        lock = l;
        log = env1("TOD_DIFF_LOG");
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
    if (log) {
        const float r = fmaxf(0.f, in0 + in4), c = rdf(t + 0xFA8);
        if (fabsf(r - last_r) >= 0.25f) {
            const float ratio = c != 0.f ? (r - c) / c : 0.f;
            const float h = powf(rdf(t + 0xFB8), ratio), e = powf(rdf(t + 0xFBC), ratio);
            if (fabsf(h - last_h) > 0.01f || fabsf(e - last_e) > 0.01f || last_r < 0.f) {
                fprintf(stderr, "[diff] rating %.2f (clock %.2f trapdoor %.2f, centre %.2f) -> hero dmg x%.3f enemy dmg x%.3f%s\n", r, in0, in4, c, h, e,
                        lock ? " (locked to x1)" : "");
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
    wrf(s + 0, rdf(t + 0xFA8));
    wrf(s + 4, 0.f);
    func_002CDDA8_lifted(ctx);
    wrf(s + 0, in0);
    wrf(s + 4, in4);
}
