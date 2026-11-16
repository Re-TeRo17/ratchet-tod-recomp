/*
 * tod_zurkon.cpp (11/11, user) -- game option "Mr. Zurkon as a weapon" (TOD_ZURKON_WEAPON=1, OPZIONI DI GIOCO row).
 *
 * Mr. Zurkon (WPN 16) is a device: its type descriptor (func_00462F38(16)) has [+0x18] & 3 == 2 and its data block
 * ([desc+0x48], 14 per-level tables of 0x50 bytes, entry n = level n) has one level ([blk+0x460] = 0). With the option on,
 * tod_zurkon_poll (every pad poll, guest thread, see tod_dualsense.cpp) makes it a weapon, as the test cheats wflags / wlevels /
 * wset did (WIKI 30 "LIVELLI DELLE ARMI"):
 *   - [+0x18] low bits 0 (weapon): it gains XP and levels like the others (max level min(4, [blk+0x460]));
 *   - levels 1..4 filled from level 0: damage (+0x000) x1.15 per level like the grenade / Predator (the game changes only
 *     damage with the level; ammo and the rest stay), XP thresholds (+0x230) of the Combuster / grenade, [blk+0x460] = 4;
 *   - model ids +0x04 / +0x0C = 0x4B (the robot; 0x4A is the egg): the upgrade screen shows the robot (animation 7, see
 *     combat_log.cpp func_00492E68);
 *   - the shop price: the item vendor's get_item_cost (func_002D2028) reads [blk+0x6AC] for a device and [blk+0x6A8] for a
 *     weapon (0 for Zurkon): +0x6A8 gets the device price.
 * Checked once a second (the descriptor is looked up again each time), so whatever reloads it is caught. With the option off
 * again the saved original words are written back (the filled table entries stay: unused with one level). The menus (ARMI /
 * DISPOSITIVI) are handled by the pause hook in tod_settings_menu.cpp (kZurkonHook), which asks tod_zurkon_on().
 */
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void func_00462F38(ppu_context* ctx);
extern "C" uint64_t tod_gcall(void (*fn)(ppu_context*), uint64_t a0, uint64_t a1);   /* tod_cheat.cpp */
extern "C" uint32_t tod_hero_entity(void);                                           /* tod_cheat.cpp */
extern "C" int tod_guest_ok(void);                                                   /* tod_cheat.cpp */

namespace {

bool gp(uint32_t a) { return a >= 0x10000u && a < 0xF0000000u; }
const uint32_t kZurkon = 16, kRobot = 0x4B, kLevels = 4;

struct Saved { bool have; uint32_t desc, blk, flags, m04, m0C, top, price; } s_orig;
int s_applied = 0;   /* 1 while the descriptor is ours */

float f32(uint32_t w) { float f; memcpy(&f, &w, 4); return f; }
uint32_t w32(float f) { uint32_t w; memcpy(&w, &f, 4); return w; }

void apply(uint32_t d, uint32_t b)
{
    if (!s_orig.have || s_orig.desc != d) {
        s_orig = { true, d, b, vm_read32(d + 0x18), vm_read32(d + 0x04), vm_read32(d + 0x0C), vm_read32(b + 0x460), vm_read32(b + 0x6A8) };
    }
    static const float thr[kLevels] = { 1000, 2200, 3640, 5370 };   /* Combuster / grenade XP to reach level n */
    for (uint32_t f = 0; f < 0x460; f += 0x50)
        for (uint32_t n = 1; n <= kLevels; n++) {
            const uint32_t at = b + f + n * 4;
            if (vm_read32(at)) continue;
            float v = f == 0x230 ? thr[n - 1] : f32(vm_read32(b + f));
            if (f == 0) for (uint32_t k = 0; k < n; k++) v *= 1.15f;
            vm_write32(at, w32(v));
        }
    vm_write32(b + 0x460, kLevels);
    vm_write32(d + 0x04, kRobot);
    vm_write32(d + 0x0C, kRobot);
    if (!vm_read32(b + 0x6A8)) vm_write32(b + 0x6A8, vm_read32(b + 0x6AC));
    vm_write32(d + 0x18, vm_read32(d + 0x18) & ~3u);
    s_applied = 1;
    fprintf(stderr, "[zurkon] weapon on: desc %08X blk %08X levels %u model %X price %u\n", d, b, kLevels, kRobot, vm_read32(b + 0x6A8));
}

void revert(uint32_t d, uint32_t b)
{
    if (s_orig.have && s_orig.desc == d) {
        vm_write32(d + 0x18, s_orig.flags);
        vm_write32(d + 0x04, s_orig.m04);
        vm_write32(d + 0x0C, s_orig.m0C);
        vm_write32(b + 0x460, s_orig.top);
        vm_write32(b + 0x6A8, s_orig.price);
    } else {
        vm_write32(d + 0x18, (vm_read32(d + 0x18) & ~3u) | 2u);   /* not seen before: back to a device at least */
        vm_write32(b + 0x460, 0);
    }
    s_applied = 0;
    fprintf(stderr, "[zurkon] weapon off: desc %08X back to a device\n", d);
}

int option_on()
{
    static int v = 0, n = 0;
    if (n-- <= 0) { n = 30; const char* e = getenv("TOD_ZURKON_WEAPON"); v = (e && e[0] == '1') ? 1 : 0; }
    return v;
}

} // namespace

/* 1 when the option is on (the pause menu hook asks through the bridge, op 25) */
extern "C" int tod_zurkon_on(void) { return option_on(); }

extern "C" void tod_zurkon_poll(void)
{
    static int tick = 0;
    if (++tick < 60) return;   /* about once a second */
    tick = 0;
    const int on = option_on();
    if (!on && !s_applied) return;
    if (!tod_guest_ok() || !tod_hero_entity()) return;
    const uint32_t d = (uint32_t)tod_gcall(func_00462F38, kZurkon, 0);
    if (!gp(d)) return;
    const uint32_t b = vm_read32(d + 0x48);
    if (!gp(b)) return;
    const bool weapon = (vm_read32(d + 0x18) & 3u) == 0;
    if (on && (!weapon || vm_read32(b + 0x460) != kLevels || vm_read32(d + 0x04) != kRobot)) apply(d, b);
    else if (!on && s_applied) revert(d, b);
}
