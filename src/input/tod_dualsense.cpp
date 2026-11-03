/*
 * tod_dualsense.cpp -- DualSense adaptive triggers that follow the weapon in Ratchet's hands.
 *
 * The trigger effect blocks (feedback 0x21, weapon 0x25, vibration 0x26, off 0x05), their meaning and the
 * way a weapon trigger is built from them are ported from RACiT (A Crack in Time port), MIT licence:
 *
 *   Copyright (c) 2026 HighDrexler
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 *   associated documentation files (the "Software"), to deal in the Software without restriction, including
 *   without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the
 *   following conditions: The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
 *   KIND (full text: THIRD_PARTY_NOTICES.md).
 *
 * What is ours: RACiT follows the weapon through the game's sound starts; Tools of Destruction is read from
 * the game state instead -- the equipped weapon slot and its ammo in the hero's object, the pause / quick
 * select state and the cutscene player -- so the profile is right the moment a weapon is equipped and needs
 * no hook in the game's code (host_input.cpp calls tod_ds_poll() on the game's pad poll).
 *
 * Settings (tod.ini keys, all ON by default, changed live from the F1 page; with no DualSense connected
 * nothing below does anything):
 *   TOD_DS_TRIGGERS=1|0          adaptive triggers per weapon
 *   TOD_DS_TRIGGER_STRENGTH=25..100   resistance and kick strength in percent
 *   TOD_DS_FIRE_R2=1|0           DualSense: the game's R1 (fire) is the right trigger, the game's R2 the
 *                                right bumper -- the trigger with the motor behind it fires. The button
 *                                prompts still show R1 for fire (the pad layer only swaps the two).
 *   TOD_XB_TRIGGERS=1|0          1.01: on an Xbox One / Series controller (motors in the triggers, "impulse
 *                                triggers"), the right trigger kicks at every shot, as hard and as long as the
 *                                weapon's DualSense kick (automatic weapons: a short tap per shot = a buzz).
 *                                Strength: TOD_DS_TRIGGER_STRENGTH. Needs a controller and driver path where
 *                                SDL can reach the trigger motors (Xbox Wireless / USB; not plain XInput).
 * Diagnostics: TOD_DS_LOG=1 evaluates and logs ("[ds] ...") with no DualSense too (the effect goes nowhere);
 * TOD_DS_WATCH=1 logs changes of the state words; the cheat "dsprobe=<n>" (tod_cheat.cpp) lists where the
 * hero keeps the value <n>.
 *
 * Game state (all in guest memory, found with dsprobe / dswatch -- see RIPARTENZA.md 02/11 ds134):
 *   hero object  = [entity(hero handle)+0x40]  (tod_hero_object(), tod_cheat.cpp)
 *   player data  = [hero object + 0x1A68]
 *   weapon slot = u32 [player data + 0x42C] (the Lua WPN_* enum order, kWeapon[] below); ammo = float
 *   [player data + slot*20 + 8]; pause / quick select wheel = u32 [0x10731D00] != 0; in-engine cutscene =
 *   u32 [0x101B88E0 + 0x74] != 0.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "ppu_recomp.h"

extern "C" {
uint32_t tod_hero_object(void);                                              /* tod_cheat.cpp */
int  cellPad_host_ds_present(uint32_t port);                                 /* ps3recomp libs/input/cellPad.c */
void cellPad_host_ds_triggers(uint32_t port, const uint8_t* r2, const uint8_t* l2);
void cellPad_host_ds_fire_on_r2(int on);
void cellPad_host_ds_r2_threshold(int raw255);
int  cellPad_host_trig_rumble_present(uint32_t port);                        /* 1.01: Xbox impulse triggers */
void cellPad_host_trig_rumble(uint32_t port, uint16_t left, uint16_t right, uint32_t ms);
}

namespace {

bool gp(uint32_t a) { return a >= 0x10000 && a < 0xF0000000u; }
float rf(uint32_t a) { uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; }

/* ---- the effect blocks (RACiT, MIT): 11 bytes, mode byte first ------------------------------------------ */

void fx_off(uint8_t* d) { memset(d, 0, 11); d[0] = 0x05; }

/* Resistance from `position` (0-9) on, strength 1-8, even all the way. */
void fx_feedback(uint8_t* d, int position, int strength)
{
    memset(d, 0, 11);
    if (strength < 1) { d[0] = 0x05; return; }
    const unsigned force = (unsigned)(strength - 1) & 7u;
    unsigned active = 0;
    unsigned long long zones = 0;
    for (int i = position; i < 10; i++) { zones |= (unsigned long long)force << (3 * i); active |= 1u << i; }
    d[0] = 0x21;
    d[1] = (uint8_t)(active & 0xFF); d[2] = (uint8_t)(active >> 8);
    d[3] = (uint8_t)zones; d[4] = (uint8_t)(zones >> 8); d[5] = (uint8_t)(zones >> 16); d[6] = (uint8_t)(zones >> 24);
}

/* A weapon trigger: resistance from zone `start` that gives way (the click) at zone `end` (2-8). */
void fx_weapon(uint8_t* d, int start, int end, int strength)
{
    memset(d, 0, 11);
    const unsigned zones = (1u << start) | (1u << end);
    d[0] = 0x25; d[1] = (uint8_t)(zones & 0xFF); d[2] = (uint8_t)(zones >> 8); d[3] = (uint8_t)(strength < 1 ? 0 : strength - 1);
}

/* Vibration from `position` (0-9) on: amplitude 1-8, frequency in Hz. */
void fx_vibration(uint8_t* d, int position, int amplitude, int frequency)
{
    memset(d, 0, 11);
    if (amplitude < 1 || frequency < 1) { d[0] = 0x05; return; }
    unsigned active = 0;
    unsigned long long zones = 0;
    for (int i = position; i < 10; i++) { zones |= (unsigned long long)((amplitude - 1) & 7) << (3 * i); active |= 1u << i; }
    d[0] = 0x26;
    d[1] = (uint8_t)(active & 0xFF); d[2] = (uint8_t)(active >> 8);
    d[3] = (uint8_t)zones; d[4] = (uint8_t)(zones >> 8); d[5] = (uint8_t)(zones >> 16); d[6] = (uint8_t)(zones >> 24);
    d[9] = (uint8_t)frequency;
}

/* ---- the profiles and the per-weapon table ---------------------------------------------------------------- */

enum Kind { K_NONE, K_WEAPON, K_FEEDBACK, K_VIBRATION };
struct Profile {
    const char* name;
    Kind kind;
    int a, b, c;        /* weapon: start, end, strength; feedback: position, strength, -; vibration: position, amplitude, Hz */
    int kick_amp, kick_hz, kick_ms;   /* a pulse inside the trigger at every shot (ammo going down); amp 0 = none */
};
enum Prof { P_NONE, P_CLICK, P_HEAVY, P_AUTO, P_BEAM, P_HOLD, P_THROW, P_DEPLOY, P_RAPID, P_N };
const Profile kProf[P_N] = {
    { "none",   K_NONE,      0, 0, 0,   0,  0,   0 },   /* trigger free: wrench, gadgets, menus, cutscenes */
    { "click",  K_WEAPON,    4, 6, 6,   7, 38,  60 },   /* single shot: resistance, then a crisp click at mid-travel */
    { "heavy",  K_WEAPON,    2, 7, 8,   8, 18, 160 },   /* launchers, shotguns: long hard pull that snaps late, a deep thump */
    { "auto",   K_VIBRATION, 2, 4, 32,  0,  0,   0 },   /* automatic fire: the trigger buzzes while it is held */
    { "beam",   K_FEEDBACK,  2, 5, 0,   0,  0,   0 },   /* flame, lasers: firm even resistance all the way */
    { "hold",   K_FEEDBACK,  3, 4, 0,   6, 45, 100 },   /* hold to aim / charge, release to fire */
    { "throw",  K_WEAPON,    4, 6, 7,   8, 24, 110 },   /* grenades, mines: a wall at mid-travel */
    { "deploy", K_WEAPON,    3, 5, 7,   6, 32,  70 },   /* summons and gadgets that are placed: a short click */
    { "rapid",  K_VIBRATION, 1, 7, 20,  0,  0,   0 },   /* heavy and automatic (the RYNO): a deep, strong buzz from the first touch */
};

/* Weapon slot (Lua WPN_*, the EBOOT's enum order) -> profile. Slots outside the table: P_CLICK. */
const struct { const char* name; uint8_t prof; } kWeapon[32] = {
    /*  0 */ { "WRENCH", P_NONE },          { "COMBUSTER", P_CLICK },     { "GRENADE", P_THROW },       { "RAVAGER", P_HEAVY },
    /*  4 */ { "TORNADO", P_HEAVY },        { "BUZZ_BLADES", P_AUTO },    { "PREDATOR", P_HOLD },       { "ALPHA_NOVA", P_HEAVY },
    /*  8 */ { "FLAMETHROWER", P_BEAM },    { "GOOP_MINES", P_THROW },    { "REAPER", P_AUTO },         { "ROCKET_LAUNCHER", P_HEAVY },
    /* 12 */ { "ROBO_HIVE", P_DEPLOY },     { "MAG_NET", P_HEAVY },       { "ENERGY_CLAWS", P_CLICK },  { "RYNO", P_RAPID },
    /* 16 */ { "ZURKON", P_DEPLOY },        { "CONFUSION_GAS", P_THROW }, { "SLINKONATOR", P_CLICK },   { "GROOVITRON", P_DEPLOY },
    /* 20 */ { "MINI_LEECH", P_DEPLOY },    { "MAXI_LEECH", P_DEPLOY },   { "MORPH_BOMB", P_THROW },    { "VISICOPTER", P_NONE },
    /* 24 */ { "INFLATOPOD", P_NONE },      { "SWINGSHOT", P_NONE },      { "CUTTING_LASER", P_BEAM },  { "GELANATOR", P_BEAM },
    /* 28 */ { "ROBOWINGS", P_NONE },       { "MAGCYCLE", P_NONE },       { "PIRATEGUISE", P_NONE },    { "DECRYPTOR", P_NONE },
};

/* ---- the game state ---------------------------------------------------------------------------------------- */

const uint32_t kPauseState = 0x10731D00;          /* the game's mode word: 0 play, 2 pause menu or quick-select wheel */
const uint32_t kCinematic  = 0x101B88E0 + 0x74;   /* cinematic player (T, RIPARTENZA 25/10): 1 while an in-engine cutscene plays */

struct State {
    bool play;          /* gameplay: a hero, no pause, quick select or cutscene */
    const char* why;    /* when not */
    int weapon;         /* equipped slot, -1 unknown */
    float ammo;         /* of that slot, <0 unknown */
    int fired;          /* a slot whose ammo went down since the last poll: a shot of that weapon, -1 none */
    float fired_from, fired_to;
};

State read_state()
{
    State s{ false, "no hero", -1, -1.f, -1, 0.f, 0.f };
    const uint32_t obj = tod_hero_object();
    if (!obj) return s;
    if (vm_read32(kPauseState)) { s.why = "pause / quick select"; return s; }       /* D+0: 0 none, 2 pause or the wheel */
    if (vm_read32(kCinematic)) { s.why = "cutscene"; return s; }                  /* T+0x74: 1 while an in-engine cinematic plays */
    const uint32_t inv = vm_read32(obj + 0x1A68);        /* the player's data: slot table, 20 bytes a slot */
    s.play = true; s.why = "";
    /* The equipped weapon is u32 [player data + 0x42C]; +0x430 and +0x434 keep the two before it (every
     * hero_equip, the cheats' and the Lua one, shifts them). It is the slot that fires, also while Ratchet has
     * the weapon holstered. [hero object + 0x1F40] is only the pending equip request (-1 once applied): it
     * is not used. Seen: equip 3, 11, 5, 8 -> +0x42C 3, 11, 5, 8 and the ammo of that same slot going down
     * as it fires (log\ds134\out\p5.err). */
    int cur = -1;
    if (gp(inv)) {
        const uint32_t c = vm_read32(inv + 0x42C);
        if (c < 32) cur = (int)c;
        /* Every slot's ammo: the one that goes down is a shot (the trigger kicks). */
        static float ammo[32]; static bool ammo_init = false;
        int down = 0;
        for (int i = 0; i < 32; i++) {
            const float a = rf(inv + (uint32_t)i * 20 + 8);
            if (ammo_init && a < ammo[i] - 0.4f && ammo[i] > 0.f) { down++; s.fired = i; s.fired_from = ammo[i]; s.fired_to = a; }
            ammo[i] = a;
        }
        if (down > 2) s.fired = -1;                       /* a reload of the save / a refill: not a shot */
        ammo_init = true;
        if (cur >= 0) s.ammo = ammo[cur];
    }
    s.weapon = cur;
    return s;
}

int env_int(const char* k, int def)
{
    const char* e = getenv(k);
    return e && *e ? atoi(e) : def;
}

int scaled(int v, int pct) { const int r = (v * pct + 50) / 100; return r < 1 ? 1 : r; }

/* The block of a profile at the given strength (percent). */
void build(const Profile& p, int pct, uint8_t* d)
{
    switch (p.kind) {
    case K_WEAPON:    fx_weapon(d, p.a, p.b, scaled(p.c, pct)); break;
    case K_FEEDBACK:  fx_feedback(d, p.a, scaled(p.b, pct)); break;
    case K_VIBRATION: fx_vibration(d, p.a, scaled(p.b, pct), p.c); break;
    default:          fx_off(d); break;
    }
}

/* Where a pull counts as the button (0-255): at the click of a weapon effect, at the start of the others. */
int fire_threshold(const Profile& p)
{
    switch (p.kind) {
    case K_WEAPON:    return p.b * 255 / 10 - 10;
    case K_FEEDBACK:
    case K_VIBRATION: return p.a * 255 / 10 - 6;
    default:          return 3000 * 255 / 32767;   /* the pad layer's default */
    }
}

}  // namespace

/* TOD_DS_WATCH / dsprobe: see the header. */
extern "C" void tod_ds_probe(int tag)
{
    const uint32_t obj = tod_hero_object();
    const uint32_t P2 = obj ? vm_read32(obj + 0x1A68) : 0, G = vm_read32(0x89F1B8);
    struct { const char* n; uint32_t base, len; } R[] = { { "obj", obj, 0x4000 }, { "P2", P2, 0x2400 }, { "G", G, 0x2400 } };
    fprintf(stderr, "[dsprobe] tag=%d obj=%08X P2=%08X G=%08X\n", tag, obj, P2, G);
    for (auto& r : R) {
        if (!gp(r.base)) continue;
        for (int w = 0; w < 2; w++) {
            char line[16384]; int n = 0; line[0] = 0;
            for (uint32_t o = 0; o < r.len && n < (int)sizeof line - 16; o += (w ? 4 : 1)) {
                const uint32_t v = w ? vm_read32(r.base + o) : vm_read8(r.base + o);
                if (v == (uint32_t)tag) n += snprintf(line + n, sizeof line - n, "%X ", o);
            }
            fprintf(stderr, "[dsprobe] %s %s==%d: %s\n", r.n, w ? "u32" : "u8", tag, line);
        }
    }
}

/* TOD_DS_WATCH=1 (diagnostic): log every change, with a timestamp, of the words of a few game structures
 * (a word that changes more than 6 times is muted: timers). Used to find the state words this file reads. */
static void watch(ULONGLONG now)
{
    static const struct { const char* n; uint32_t base; uint32_t len; } R[] = {
        { "D", 0x10731D00, 0x80 }, { "P", 0x101AEF80, 0x100 }, { "T", 0x101B88E0, 0x100 }, { "C", 0x10616280, 0x200 } };
    static uint32_t prev[4][0x80]; static uint8_t cnt[4][0x80]; static bool first = true;
    static uint32_t oprev[0x100], gprev[0x180]; static uint8_t ocnt[0x100], gcnt[0x180]; static uint32_t obase, gbase;
    static ULONGLONG t0 = now;
    auto smallv = [](uint32_t v) { return v < 0x100 || v == 0xFFFFFFFFu; };
    for (int r = 0; r < 4; r++)
        for (uint32_t o = 0; o < R[r].len; o += 4) {
            const uint32_t v = vm_read32(R[r].base + o), i = o / 4;
            if (!first && v == prev[r][i]) continue;
            if (!first && smallv(v) && smallv(prev[r][i]) && cnt[r][i] < 6) { cnt[r][i]++; fprintf(stderr, "[dswatch] %6llu ms %s+%X: %08X -> %08X%s\n", now - t0, R[r].n, o, prev[r][i], v, cnt[r][i] == 6 ? " (muted)" : ""); }
            prev[r][i] = v;
        }
    first = false;
    const uint32_t obj = tod_hero_object();
    if (obj) {
        const uint32_t g = vm_read32(obj + 0x1A68);
        if (obj != obase) { obase = obj; memset(oprev, 0, sizeof oprev); fprintf(stderr, "[dswatch] hero object %08X\n", obj); }
        if (g != gbase) { gbase = g; memset(gprev, 0, sizeof gprev); fprintf(stderr, "[dswatch] player data %08X\n", g); }
        for (uint32_t i = 0; i < 0x100; i++) {   /* obj+0x1E00 .. +0x2200 */
            const uint32_t v = vm_read32(obj + 0x1E00 + i * 4);
            if (v != oprev[i] && smallv(v) && smallv(oprev[i]) && ocnt[i] < 30) { ocnt[i]++; fprintf(stderr, "[dswatch] %6llu ms obj+%X: %08X -> %08X\n", now - t0, 0x1E00 + i * 4, oprev[i], v); }
            oprev[i] = v;
        }
        if (gp(g)) for (uint32_t i = 0; i < 0x180; i++) {   /* player data +0 .. +0x600, small ints only (the ammo floats are logged apart) */
            const uint32_t v = vm_read32(g + i * 4);
            if (v != gprev[i] && smallv(v) && smallv(gprev[i]) && gcnt[i] < 12) { gcnt[i]++; fprintf(stderr, "[dswatch] %6llu ms G+%X: %08X -> %08X\n", now - t0, i * 4, gprev[i], v); }
            gprev[i] = v;
        }
        static float ammo[32];   /* every slot's ammo, when it moves */
        if (gp(g)) for (int sl = 0; sl < 32; sl++) {
            const float a = rf(g + sl * 20 + 8);
            if (a != ammo[sl]) { fprintf(stderr, "[dswatch] %6llu ms ammo slot %d: %.1f -> %.1f (equipped %d)\n", now - t0, sl, ammo[sl], a, (int)vm_read32(obj + 0x1F40)); ammo[sl] = a; }
        }
    }
}

/* Called on every pad poll of the game's main thread (host_input.cpp), also while the F1 page is open. */
extern "C" void tod_ds_poll(int ui_open)
{
    static ULONGLONG next = 0, cfg_next = 0;
    const ULONGLONG now = GetTickCount64();
    { static int w = -1; if (w < 0) w = getenv("TOD_DS_WATCH") != nullptr; if (w) watch(now); }
    if (now < next) return;
    next = now + 8;

    static bool trig_on = true, fire_r2 = true, force_log = false, xb_on = true;
    static int pct = 100;
    if (now >= cfg_next) {   /* the F1 page changes these live */
        cfg_next = now + 250;
        trig_on = env_int("TOD_DS_TRIGGERS", 1) != 0;
        fire_r2 = env_int("TOD_DS_FIRE_R2", 1) != 0;
        pct = env_int("TOD_DS_TRIGGER_STRENGTH", 100);
        pct = pct < 10 ? 10 : pct > 100 ? 100 : pct;
        force_log = env_int("TOD_DS_LOG", 0) != 0;
        xb_on = env_int("TOD_XB_TRIGGERS", 1) != 0;
        cellPad_host_ds_fire_on_r2(fire_r2);
    }
    static bool was_present = false;
    const bool present = cellPad_host_ds_present(0) != 0;
    if (present != was_present) {
        was_present = present;
        fprintf(stderr, "[ds] DualSense %s on port 0 (triggers %s, fire on %s, strength %d%%)\n", present ? "connected" : "gone",
                trig_on ? "on" : "off", fire_r2 ? "R2" : "R1", pct);
    }
    static bool xb_was = false;
    const bool xb = xb_on && !present && cellPad_host_trig_rumble_present(0) != 0;   /* a DualSense keeps its own effects */
    if (xb != xb_was) {
        xb_was = xb;
        fprintf(stderr, "[ds] trigger motors (Xbox impulse triggers) %s on port 0\n", xb ? "found" : "gone");
    }
    if (!present && !xb && !force_log) return;   /* no DualSense, no trigger motors: nothing to do, nothing to read */

    /* What the trigger should do now. */
    State st = read_state();
    int prof = P_NONE;
    const char* why = st.why;
    if (!trig_on) { why = "triggers off"; st.play = false; }
    else if (ui_open) { why = "settings page"; st.play = false; }
    if (st.play) {
        prof = st.weapon >= 0 && st.weapon < 32 ? kWeapon[st.weapon].prof : P_NONE;   /* -1: no weapon in hand (Clank, vehicles) */
        if (st.weapon < 0) why = "no weapon";
        if (prof != P_NONE && st.ammo >= 0.f && st.ammo < 0.5f) { prof = P_NONE; why = "out of ammo"; }
    }

    /* Log: one line per change of (weapon, profile). */
    static int l_weapon = -2, l_prof = -2;
    const int key_w = st.play ? st.weapon : -1;
    if (key_w != l_weapon || prof != l_prof) {
        l_weapon = key_w; l_prof = prof;
        if (st.play)
            fprintf(stderr, "[ds] weapon %d %s -> trigger %s (ammo %.0f)\n", st.weapon,
                    st.weapon >= 0 && st.weapon < 32 ? kWeapon[st.weapon].name : "?", kProf[prof].name, st.ammo);
        else
            fprintf(stderr, "[ds] weapon none (%s) -> trigger none\n", why ? why : "?");
        if (prof == P_NONE && st.play) fprintf(stderr, "[ds]   (%s)\n", why && *why ? why : "free trigger");
    }

    /* A shot: the ammo of the weapon went down. A pulse inside the trigger, then back to the resting effect. */
    static ULONGLONG kick_until = 0; static int kick_prof = 0;
    if (st.play && prof != P_NONE && st.fired == st.weapon && kProf[prof].kick_amp > 0) {
        kick_until = now + (ULONGLONG)kProf[prof].kick_ms; kick_prof = prof;
        if (xb) {   /* 1.01: the same kick on Xbox trigger motors (kick_amp is 0..8 on the DualSense scale) */
            const uint16_t r = (uint16_t)(kProf[prof].kick_amp * 8191 * pct / 100);
            cellPad_host_trig_rumble(0, 0, r, (uint32_t)kProf[prof].kick_ms);
        }
        static int nk; if (nk++ < 20) fprintf(stderr, "[ds] shot (ammo %.0f -> %.0f): kick %s\n", st.fired_from, st.fired_to, kProf[prof].name);
    }
    if (xb && st.play && prof != P_NONE && st.fired == st.weapon && kProf[prof].kick_amp == 0) {
        /* the automatic weapons (a DualSense buzz while held, no kick): a short light tap per shot */
        cellPad_host_trig_rumble(0, 0, (uint16_t)(4 * 8191 * pct / 100), 45);
    }

    uint8_t blk[11];
    const bool have = prof != P_NONE;
    if (now < kick_until && have) {
        const Profile& k = kProf[kick_prof];
        fx_vibration(blk, 2, scaled(k.kick_amp, pct), k.kick_hz);
    } else if (have) build(kProf[prof], pct, blk);
    static uint8_t last[11]; static bool last_have = false; static int last_thr = -1;
    if (have != last_have || (have && memcmp(blk, last, 11))) {
        last_have = have; memcpy(last, blk, 11);
        if (force_log && getenv("TOD_DS_LOG")[0] == '2' && have) {
            char hex[40]; for (int i = 0; i < 11; i++) snprintf(hex + i * 3, 4, "%02X ", blk[i]);
            fprintf(stderr, "[ds]   R2 block %s\n", hex);
        }
        cellPad_host_ds_triggers(0, have ? blk : nullptr, nullptr);
    }
    const int thr = have ? fire_threshold(kProf[prof]) : fire_threshold(kProf[P_NONE]);
    if (thr != last_thr) { last_thr = thr; cellPad_host_ds_r2_threshold(thr); }
}
