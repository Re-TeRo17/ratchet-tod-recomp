/*
 * combat_log.cpp (10/11) -- in-game combat log: damage sent, damage really applied, XP gained per weapon.
 *
 * Damage applied: tod_combat_poll compares the health of every entity poll by poll (see scan_health) -- any source.
 * Damage messages (research, TOD_COMBAT_LOG=3): the game builds a damage record (func_00250D28 -> func_000BB9A8: +0x18 float amount, +0x1C type, +0x20/+0x24
 * two words, +0x28 float) and hands it to the target with func_000BAE18(r3 = target entity, r4 = record, ...) -- the core of
 * the Lua "damage" native (func_002807B0) goes the same way, and so do the 28 callers of the stub func_0024F058.
 * tools/combat_log_ppu_patch.cmake renames the lifted definition to func_000BAE18_lifted; this file provides func_000BAE18:
 * it logs the record. Seen 10/11 (cl1): only melee-like records (type 1, 3.5 / 5.0) come this way, the grenade's do not, and the
 * health changes later (when the target handles the message), so the applied damage is measured by the scan instead.
 *
 * XP: tod_combat_poll (called from the pad poll, tod_dualsense.cpp) compares every weapon slot's XP float
 * ([0x89F1B8] + slot*20 + 4) with the last poll and logs the gain, with the level byte (+0x11).
 *
 * TOD_COMBAT_LOG = 0 off (default), 1 log lines "[dmg]" / "[xp]" in stderr, 2 also the on-screen list, 3 also "[dmgmsg]" (tod_overlay.cpp,
 * tod_combat_log_lines). Read every 30 calls, so the F1 page / tod.ini change it live.
 */
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>

void func_000BAE18_lifted(ppu_context* ctx);
void func_000BD078_lifted(ppu_context* ctx);
void func_00462F38_lifted(ppu_context* ctx);
void func_00488430_lifted(ppu_context* ctx);
void func_00492E68_lifted(ppu_context* ctx);
void func_00523C00_lifted(ppu_context* ctx);
void func_0068F0A8_lifted(ppu_context* ctx);
extern "C" uint64_t tod_gcall(void (*fn)(ppu_context*), uint64_t a0, uint64_t a1);   /* tod_cheat.cpp */
extern "C" uint32_t tod_hero_entity(void);                                           /* tod_cheat.cpp */
extern "C" uint64_t tod_gcall_toc(void (*fn)(ppu_context*), uint32_t toc, uint64_t a0, uint64_t a1);   /* tod_cheat.cpp */
extern "C" int tod_guest_ok(void);                                                   /* tod_cheat.cpp */
extern "C" uint32_t tod_ent_table(uint32_t* count);                                  /* tod_cheat.cpp */
extern "C" uint32_t g_rsx_engine_frame;

namespace {

bool gp(uint32_t a) { return a >= 0x10000u && a < 0xF0000000u; }
float rf(uint32_t a) { uint32_t w = vm_read32(a); float f; memcpy(&f, &w, 4); return f; }

int mode()
{
    static int v = 0, n = 0;
    if (n-- <= 0) { n = 30; const char* e = getenv("TOD_COMBAT_LOG"); v = e ? atoi(e) : 0; }
    return v;
}

/* on-screen list: the last lines, newest last */
struct Line { uint32_t frame; int kind; char text[96]; };   /* kind 0 damage dealt, 1 damage taken, 2 xp, 3 kill */
Line s_lines[64]; unsigned s_count; std::mutex s_mx;

void push(int kind, const char* text)
{
    std::lock_guard<std::mutex> lk(s_mx);
    Line& l = s_lines[s_count++ % 64];
    l.frame = g_rsx_engine_frame; l.kind = kind;
    snprintf(l.text, sizeof l.text, "%s", text);
}

const char* const kWpn[32] = { "Wrench", "Combuster", "Fusion Grenade", "Shock Ravager", "Tornado", "Buzz Blades", "Predator",
    "Alpha Nova", "Pyro Blaster", "Plasma Beasts", "Shard Reaper", "Negotiator", "Nano-Swarmers", "Mag-Net", "Razor Claws", "RYNO IV",
    "Mr. Zurkon", "Confusion Gas", "Groovitron", "Slinkonator", "Mini Leech", "Maxi Leech", "Morph-o-Ray", "Visicopter",
    "Inflatopod", "Swingshot", "Cutting Laser", "Gelanator", "Robo-Wings", "Magcycle", "Pirate Guise", "Decryptor" };

}  // namespace

extern "C" int tod_combat_log_mode(void) { return mode(); }
/* overlay: show the list (mode 2+, no menu / pause / quick-select wheel: the game mode word [0x10731D00] == 0) */
extern "C" int tod_combat_log_visible(void) { return mode() >= 2 && vm_read32(0x10731D00) == 0; }

/* tod_overlay.cpp: copy the lines newer than max_age frames (newest last); returns the count. */
extern "C" int tod_combat_log_lines(int max_age, int max_n, int* kinds, char (*texts)[96], int* ages)
{
    std::lock_guard<std::mutex> lk(s_mx);
    int n = 0;
    const unsigned first = s_count > 64 ? s_count - 64 : 0;
    for (unsigned i = s_count; i-- > first && n < max_n;) {
        const Line& l = s_lines[i % 64];
        const int age = (int)(g_rsx_engine_frame - l.frame);
        if (age > max_age) break;
        kinds[n] = l.kind; ages[n] = age; memcpy(texts[n], l.text, 96); n++;
    }
    for (int a = 0, b = n - 1; a < b; a++, b--) {   /* oldest first */
        int k = kinds[a]; kinds[a] = kinds[b]; kinds[b] = k; k = ages[a]; ages[a] = ages[b]; ages[b] = k;
        char t[96]; memcpy(t, texts[a], 96); memcpy(texts[a], texts[b], 96); memcpy(texts[b], t, 96);
    }
    return n;
}

void func_000BAE18(ppu_context* ctx)
{
    const int m = mode();
    const uint32_t ent = (uint32_t)ctx->gpr[3], rec = (uint32_t)ctx->gpr[4];
    if (m >= 3 && gp(ent) && gp(rec))   /* research only: the record is applied later, not inside this call */
        fprintf(stderr, "[dmgmsg] f=%u ent=%08X amt=%.2f type=%u w20=%08X w24=%08X\n", g_rsx_engine_frame, ent, rf(rec + 0x18),
                vm_read32(rec + 0x1C), vm_read32(rec + 0x20), vm_read32(rec + 0x24));
    func_000BAE18_lifted(ctx);
}

/* research 15/11: func_000BD078(r3 = health component, r4 = hit record) -- who takes the hero's health, and is one attack counted twice? */
void func_000BD078(ppu_context* ctx)
{
    const int m = mode();
    const uint32_t hc = (uint32_t)ctx->gpr[3], rec = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr;
    const bool on = m >= 3 && gp(hc);
    const float before = on ? rf(hc + 4) : 0.f;
    /* the function walks a list of records (next at +0x4) and subtracts each one's +0x38: keep every record's amount before the call */
    uint32_t lr_[16]; float la[16]; int ln = 0;
    if (on) for (uint32_t q = rec; q && gp(q) && ln < 16; q = vm_read32(q + 0x4)) { lr_[ln] = q; la[ln] = rf(q + 0x38); ln++; }
    func_000BD078_lifted(ctx);
    if (!on) return;
    const float after = rf(hc + 4);
    if (after < before - 0.001f) {
        for (int i = 0; i < ln; i++)
            fprintf(stderr, "[hitrec] f=%u hc=%08X #%d rec=%08X amt %.2f -> %.2f type=%05X fl=%08X src=%08X%08X\n", g_rsx_engine_frame, hc, i, lr_[i],
                    la[i], rf(lr_[i] + 0x38), vm_read32(lr_[i] + 0x3C), vm_read32(lr_[i] + 0x18), vm_read32(lr_[i] + 0x28), vm_read32(lr_[i] + 0x2C));
        const bool r = gp(rec);
        fprintf(stderr, "[hit] f=%u hc=%08X lr=%08X rec=%08X amt=%.2f type=%05X fl=%08X w28=%08X%08X w48=%.2f hp %.2f -> %.2f / %.2f\n",
                g_rsx_engine_frame, hc, lr, rec, r ? rf(rec + 0x38) : 0.f, r ? vm_read32(rec + 0x3C) : 0u, r ? vm_read32(rec + 0x18) : 0u,
                r ? vm_read32(rec + 0x28) : 0u, r ? vm_read32(rec + 0x2C) : 0u, r ? rf(rec + 0x48) : 0.f, before, after, rf(hc + 8));
    }
}

/* Health of every entity, compared poll by poll: the damage really applied (any source), kills, damage taken by Ratchet.
 * Entity table: [0x898F3C] -> base, count; entity = base + idx*384, live when ([ent+0x64] & 0xFFFF) == idx; health component =
 * func_00251F68([ent+0x40], 0x6C) (a guest call: done once per new entity, at most 48 per poll). */
/* Weapon names in the game's language: func_000274F8(id) (core of the UI native get_weapon_name, TOC 0x88FF38) returns a
 * guest string. Read on the guest thread, cached, refreshed every ~600 polls (language change in the Options). Falls back to kWpn. */
char s_name[32][48]; int s_name_poll = 0;
void refresh_names()
{
    if (s_name_poll-- > 0) return;
    s_name_poll = 600;
    for (int w = 0; w < 32; w++) {
        const uint32_t sp = (uint32_t)tod_gcall_toc(func_000274F8, 0x88FF38u, (uint64_t)w, 0);
        char t[48]; int n = 0;
        if (gp(sp)) for (; n < 47; n++) { const uint8_t c = vm_read8(sp + (uint32_t)n); if (!c) break; t[n] = (char)c; }
        t[n] = 0;
        if (n > 0 && strcmp(t, s_name[w])) { memcpy(s_name[w], t, sizeof t); if (mode() >= 3) fprintf(stderr, "[dmg] name %d = '%s'\n", w, t); }
        if (n == 0 && !s_name[w][0]) snprintf(s_name[w], sizeof s_name[w], "%s", kWpn[w]);
    }
}
const char* wname(int w) { return (w >= 0 && w < 32) ? (s_name[w][0] ? s_name[w] : kWpn[w]) : "?"; }

/* Who did it: the target's health component keeps the last damage's type at +0x24 (a bit mask, one per weapon; +0x28 a
 * sub-kind, +0x38 the attacker's handle -- often a projectile or the Zurkon object). Seen 10/11 (cl4/cl5, log\quit125\dmgatt_an.py):
 * 0x1 Wrench, 0x10 Combuster, 0x100 Buzz Blades, 0x200 Predator, 0x400 Shock Ravager (cl6), 0x800 Tornado, 0x10000 Fusion Grenade (direct), 0x20000 Mr. Zurkon; 0x8000 =
 * explosion (grenade splash, Combuster, Negotiator: not one weapon -> the equipped one). Other types are learned at run time: a
 * kill of type T followed (same or next 3 polls) by XP for weapon W -> T means W. Unknown types -> the equipped weapon. */
struct TypeMap { uint32_t type; int wpn; };
TypeMap s_tmap[48] = { { 0x1, 0 }, { 0x10, 1 }, { 0x100, 5 }, { 0x200, 6 }, { 0x400, 3 }, { 0x800, 4 }, { 0x10000, 2 }, { 0x20000, 16 } };
int s_tmap_n = 8;
uint32_t s_kill_type = 0; int s_kill_age = 99;
int type_weapon(uint32_t t)
{
    for (int i = 0; i < s_tmap_n; i++) if (s_tmap[i].type == t) return s_tmap[i].wpn;
    return -1;
}
void learn_type(uint32_t t, int w)
{
    if (!t || t == 0x8000 || type_weapon(t) >= 0 || s_tmap_n >= 48) return;
    s_tmap[s_tmap_n].type = t; s_tmap[s_tmap_n].wpn = w; s_tmap_n++;
    fprintf(stderr, "[dmg] learned: damage type %05X = weapon %d\n", t, w);
}

struct Ent { uint32_t ent, obj, hc; float hp; };
Ent s_ent[4096];

void scan_health(int m)
{
    if (!tod_guest_ok() || g_rsx_engine_frame < 600) return;
    uint32_t count = 0; const uint32_t base = tod_ent_table(&count);
    if (!base) return;
    if (count > 4096) count = 4096;
    const uint32_t hero = tod_hero_entity();
    const uint32_t G = vm_read32(0x89F1B8);
    const int wpn = gp(G) ? (int)vm_read32(G + 0x42C) : -1;
    refresh_names();
    int calls = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t ent = base + i * 384u;
        Ent& e = s_ent[i];
        const uint32_t h = vm_read32(ent + 0x64);
        const uint32_t obj = vm_read32(ent + 0x40);
        if (!h || (h & 0xFFFF) != i || !gp(obj)) { e.ent = 0; continue; }
        if (e.ent != ent || e.obj != obj) {
            if (calls >= 48) continue;
            calls++;
            const uint32_t hc = (uint32_t)tod_gcall(func_00251F68, obj, 0x6C);
            e.ent = ent; e.obj = obj; e.hc = gp(hc) ? hc : 0; e.hp = e.hc ? rf(e.hc + 4) : 0;
            continue;
        }
        if (!e.hc) continue;
        const float hp = rf(e.hc + 4);
        if (hp < e.hp - 0.001f && e.hp > 0) {
            const float lost = e.hp - hp, mx = rf(e.hc + 8);
            const bool kill = hp <= 0, me = ent == hero;
            const uint32_t t24 = vm_read32(e.hc + 0x24);
            const int tw = me ? -1 : type_weapon(t24);
            const int who = tw >= 0 ? tw : wpn;
            const char* wn = wname(who);
            if (kill && !me) { s_kill_type = t24; s_kill_age = 0; }
            fprintf(stderr, "[dmg] f=%u %s ent=%08X lost %.2f hp %.2f -> %.2f / %.2f%s by %s (type %05X%s, equipped %s)\n", g_rsx_engine_frame,
                    me ? "RATCHET" : "target", ent, lost, e.hp, hp, mx, kill ? " KILL" : "", wn, t24, tw >= 0 ? "" : " unknown", wname(wpn));
            if (m >= 3) {   /* research (10/11): who did it? health component words and entity words after the hit */
                char d[1400]; int n = snprintf(d, sizeof d, "[dmgdump] ent=%08X hero=%08X hc:", ent, hero);
                for (uint32_t o = 0; o < 0x80; o += 4) n += snprintf(d + n, sizeof d - n, " %08X", vm_read32(e.hc + o));
                fprintf(stderr, "%s\n", d);
                const uint32_t att = vm_read32(e.hc + 0x38), ai = att & 0xFFFF, ae = ai < count ? base + ai * 384u : 0;
                const uint32_t ao = (ae && vm_read32(ae + 0x64) == att) ? vm_read32(ae + 0x40) : 0;
                char q[600]; int k = snprintf(q, sizeof q, "[dmgatt] att=%08X ent=%08X live=%d obj=%08X vt=%08X hero_ent=%08X hero_h=%08X ent+0..+60:", att, ae,
                                             ae && vm_read32(ae + 0x64) == att, ao, gp(ao) ? vm_read32(ao) : 0, hero, hero ? vm_read32(hero + 0x64) : 0);
                for (uint32_t o = 0; ae && o < 0x70; o += 4) k += snprintf(q + k, sizeof q - k, " %08X", vm_read32(ae + o));
                fprintf(stderr, "%s\n", q);
            }
            if (m >= 2) {
                char t[96];
                if (me) snprintf(t, sizeof t, "Ratchet  -%.1f  (%.1f/%.0f)", lost, hp < 0 ? 0.f : hp, mx);
                else if (kill) snprintf(t, sizeof t, "%s  %.0f  KILL", wn, lost);
                else snprintf(t, sizeof t, "%s  %.0f", wn, lost);
                push(me ? 1 : kill ? 3 : 0, t);
            }
        }
        e.hp = hp;
    }
}

/* Pad poll (main thread): XP gained per weapon slot. */
extern "C" void tod_combat_poll(void)
{
    static float xp[32]; static int lv[32]; static uint32_t lastG;
    const int m = mode();
    const uint32_t G = vm_read32(0x89F1B8);
    if (s_kill_age < 99) s_kill_age++;
    if (m) scan_health(m);
    if (!gp(G)) return;
    const bool fresh = G != lastG; lastG = G;
    for (int s = 0; s < 32; s++) {
        const float x = rf(G + s * 20 + 4); const int l = vm_read8(G + s * 20 + 0x11);
        if (!fresh && m && x > xp[s] && x - xp[s] < 1e7f) {
            if (s_kill_age <= 3) learn_type(s_kill_type, s);
            fprintf(stderr, "[xp] f=%u %s +%.1f (xp %.1f, level %d%s)\n", g_rsx_engine_frame, wname(s), x - xp[s], x, l + 1, l != lv[s] ? ", LEVEL UP" : "");
            if (m >= 2) {
                char t[96];
                if (l != lv[s]) snprintf(t, sizeof t, "%s  LV %d!", wname(s), l + 1);
                else snprintf(t, sizeof t, "%s  +%.0f XP", wname(s), x - xp[s]);
                push(2, t);
            }
        }
        xp[s] = x; lv[s] = l;
    }
}

/* 11/11 research: TOD_WDESC_TRACE=<weapon id> logs who asks for that weapon's descriptor (func_00462F38, r3 = id): the caller's
 * return address, at most once per caller every 120 frames ("[wdesc-call]"). Used to find the code of the upgrade screen. */
void func_00462F38(ppu_context* ctx)
{
    static int trace = -2;
    if (trace == -2) { const char* e = getenv("TOD_WDESC_TRACE"); trace = e ? atoi(e) : -1; }
    const int id = (int)(uint32_t)ctx->gpr[3];
    const uint32_t lr = (uint32_t)ctx->lr;
    func_00462F38_lifted(ctx);
    if (trace < 0 || id != trace) return;
    static uint32_t s_lr[256], s_fr[256]; static int s_n;
    int i = 0;
    for (; i < s_n; i++) if (s_lr[i] == lr) break;
    if (i == s_n) { if (s_n >= 256) return; s_lr[s_n] = lr; s_fr[s_n] = 0; s_n++; }
    if (s_fr[i] && g_rsx_engine_frame - s_fr[i] < 120) return;
    s_fr[i] = g_rsx_engine_frame;
    fprintf(stderr, "[wdesc-call] f=%u id=%d lr=%08X\n", g_rsx_engine_frame, id, lr);
}

/* 11/11 research: the weapon upgrade screen starts with func_00488430(r3 = weapon id, r4 = old level, r5 = new level, f1): it fills
 * the state [[r2+0x68EC]]: +0 id, +4 / +8 the levels, +0xC / +0x10 the old / new model (descriptor +0x04 up to level 3, +0x0C
 * after), +0x14 / +0x18 = -1, except the thrown weapons (grenade 0xD, Nano-Swarmers 0xB / 0xC, Plasma Beasts 0xF / 0x10):
 * NOT the model's animation: a HERO animation (the thrown weapons show Ratchet's arm in that pose; 0 showed Ratchet's body).
 * The weapon model is made by func_004891E0: moby = func_00250BC8(model id, 0x66, ...), then i = func_00492E68(moby, 1, 0) (the
 * animation with keys (1, 0)) and, if i >= 0, func_00494280(moby, [class+0x40] + i*32, ...) plays it. See below. */
static int s_upg_count = 0;   /* upgrade screens started (TOD_UPG_ANIM list: one value per screen) */
static int s_upg_id = -1;     /* weapon id of the current upgrade screen */
void func_00488430(ppu_context* ctx)
{
    const int id = (int)(uint32_t)ctx->gpr[3], lo = (int)(uint32_t)ctx->gpr[4], hi = (int)(uint32_t)ctx->gpr[5];
    const uint32_t toc = (uint32_t)ctx->gpr[2];
    func_00488430_lifted(ctx);
    s_upg_count++;
    s_upg_id = id;
    const uint32_t S = vm_read32(toc + 0x68EC);
    if (mode() >= 3 && gp(S)) fprintf(stderr, "[upg] start id=%d level %d -> %d: models %08X %08X, +14 %d +18 %d\n", id, lo, hi, vm_read32(S + 0xC),
                       vm_read32(S + 0x10), (int)vm_read32(S + 0x14), (int)vm_read32(S + 0x18));
}

/* func_00492E68(r3 = moby, r4 = key a, r5 = key b) -> the index of the moby class's animation whose record (func_004917A0(
 * [class+0x40] + i*32)) has +0 == a and +4 == b, or -1; class = [moby+0x54], count = u16 [class+0x16]. Called by the upgrade
 * screen (func_004891E0, return addresses 0x0048975C / 0x004897F8) with (1, 0): Mr. Zurkon's robot (model 0x4B) has no such
 * animation -> T-pose. TOD_UPG_ANIM=<n>[,<n2>,...]: there, when nothing is found, list the class's animations and answer n
 * (the k-th upgrade screen of the session uses the k-th value). */
void func_00492E68(ppu_context* ctx)
{
    const uint32_t lr = (uint32_t)ctx->lr, moby = (uint32_t)ctx->gpr[3];
    const int ka = (int)(uint32_t)ctx->gpr[4], kb = (int)(uint32_t)ctx->gpr[5];
    func_00492E68_lifted(ctx);
    if (lr < 0x004891E0u || lr >= 0x0048A000u) return;
    const int r = (int)(uint32_t)ctx->gpr[3];
    const uint32_t cls = gp(moby) ? vm_read32(moby + 0x54) : 0;
    const int n = gp(cls) ? (int)vm_read16(cls + 0x16) : 0;
    if (mode() >= 3) fprintf(stderr, "[upg] model anim lookup moby=%08X class=%08X (%d anims) keys (%d,%d) -> %d (lr %08X)\n", moby, cls, n, ka, kb, r, lr);
    if (r != -1 || !gp(cls)) return;
    const char* e = getenv("TOD_UPG_ANIM");
    if (!e) {
        /* default (chosen by the user 11/11): Mr. Zurkon's robot (12 animations, none (1,0)) uses animation 7, the frontal
         * "showcase" pose with the blaster in view; other models are left as the game made them */
        if (s_upg_id == 16 && n == 12) { ctx->gpr[3] = 7; if (mode() >= 3) fprintf(stderr, "[upg]   -> Mr. Zurkon: animation 7\n"); }
        return;
    }
    const uint32_t tab = vm_read32(cls + 0x40);
    for (int i = 0; i < n && i < 64; i++) {
        const uint32_t rec = (uint32_t)tod_gcall(func_004917A0, tab + (uint32_t)i * 32u, 0);
        fprintf(stderr, "[upg]   anim %d: rec %08X keys %d %d  words %08X %08X %08X %08X\n", i, rec, gp(rec) ? (int)vm_read32(rec) : -9,
                gp(rec) ? (int)vm_read32(rec + 4) : -9, gp(rec) ? vm_read32(rec + 8) : 0, gp(rec) ? vm_read32(rec + 12) : 0,
                gp(rec) ? vm_read32(rec + 16) : 0, gp(rec) ? vm_read32(rec + 20) : 0);
    }
    int want = atoi(e);   /* "a,b,c": the n-th upgrade screen uses the n-th value (tests) */
    for (int k = 1; k < s_upg_count; k++) { const char* c = strchr(e, ','); if (!c) break; e = c + 1; want = atoi(e); }
    if (want >= 0 && want < n) { ctx->gpr[3] = (uint64_t)(uint32_t)want; fprintf(stderr, "[upg]   -> using animation %d (TOD_UPG_ANIM)\n", want); }
}

/* func_00523C00(r3 = ?, r4 = class index) -> new entity (spawn research 11/11). TOD_SPAWN_TRACE=1: one line per call with
 * the arguments, the result and the return addresses of 8 stack frames (back chain [sp], saved LR at frame+0x10). */
void func_00523C00(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_SPAWN_TRACE"); on = e && *e && *e != '0'; }
    const uint32_t a3 = (uint32_t)ctx->gpr[3], a4 = (uint32_t)ctx->gpr[4], a5 = (uint32_t)ctx->gpr[5], lr = (uint32_t)ctx->lr;
    uint32_t sp = (uint32_t)ctx->gpr[1];
    func_00523C00_lifted(ctx);
    if (!on) return;
    char bt[160]; int n = 0;
    for (int k = 0; k < 8 && sp >= 0x10000 && sp < 0xF0000000u; k++) { sp = vm_read32(sp); if (sp < 0x10000 || sp >= 0xF0000000u) break;
        n += snprintf(bt + n, sizeof bt - n, " %08X", vm_read32(sp + 0x10)); if (n > 140) break; }
    fprintf(stderr, "[spawn] f=%u alloc r3=%08X class=%u r5=%08X -> %08X lr=%08X bt:%s\n", g_rsx_engine_frame, a3, a4 & 0xFFFF, a5,
            (uint32_t)ctx->gpr[3], lr, bt);
}

/* func_0068F0A8(r3 = moby manager, r4 = level class index) -> new moby (virtual, vtable entry at 0x83EE64; spawn research
 * 11/11). TOD_SPAWN_TRACE=1: manager, index, class pointer, result. The manager is kept for the 'spawn' cheat. */
extern "C" uint32_t g_tod_moby_mgr = 0;
extern "C" uint32_t g_tod_spawn_toc = 0;   /* r2 at the game's calls (the spawn cheat calls with the same TOC) */
extern "C" uint32_t g_tod_last_spawn = 0;  /* the last entity this made (the spawn cheat wakes it up) */
void func_0068F0A8(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_SPAWN_TRACE"); on = e && *e && *e != '0'; }
    const uint32_t mgr = (uint32_t)ctx->gpr[3], idx = (uint32_t)ctx->gpr[4] & 0xFFFF, lr = (uint32_t)ctx->lr;
    g_tod_moby_mgr = mgr;
    g_tod_spawn_toc = (uint32_t)ctx->gpr[2];
    func_0068F0A8_lifted(ctx);
    g_tod_last_spawn = (uint32_t)ctx->gpr[3];
    if (!on) return;
    const uint32_t m = (uint32_t)ctx->gpr[3], cls = (m >= 0x10000 && m < 0xF0000000u) ? vm_read32(m + 0x54) : 0;
    fprintf(stderr, "[spawn] mgr=%08X idx=%u -> moby %08X class %08X lr=%08X\n", mgr, idx, m, cls, lr);
    static uint8_t seen[0x10000 / 8];
    if (cls >= 0x10000 && cls < 0xF0000000u && !(seen[idx >> 3] & (1 << (idx & 7)))) {
        seen[idx >> 3] |= (uint8_t)(1 << (idx & 7));
        fprintf(stderr, "[spawncls] idx=%u class %08X:", idx, cls);
        for (uint32_t k = 0; k < 0x40; k += 2) fprintf(stderr, " %04X", vm_read16(cls + k));
        fprintf(stderr, "\n");
    }
}

/* func_0068EA08(r3 = level class index, r4 = ?, r5 -> position (16 bytes), r6 -> 3 rotation rows (48 bytes), f1 = scale): the
 * game's full spawn (entity via the manager +0x28 = func_0068F0A8, matrix, then the manager +0x2C makes the update object and
 * its +0x8 starts it). Spawn research 11/11: TOD_SPAWN_TRACE=1 logs the arguments; r4 is kept per class index for the cheat. */
void func_0068EA08_lifted(ppu_context* ctx);
extern "C" uint32_t g_tod_spawn_arg4[0x400];
uint32_t g_tod_spawn_arg4[0x400];
extern "C" uint32_t g_tod_spawn_toc2 = 0;
void func_0068EA08(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_SPAWN_TRACE"); on = e && *e && *e != '0'; }
    const uint32_t idx = (uint32_t)ctx->gpr[3] & 0xFFFF, a4 = (uint32_t)ctx->gpr[4], a5 = (uint32_t)ctx->gpr[5], a6 = (uint32_t)ctx->gpr[6];
    const uint32_t lr = (uint32_t)ctx->lr; const double sc = ctx->fpr[1];
    if (idx < 0x400) g_tod_spawn_arg4[idx] = a4;
    g_tod_spawn_toc2 = (uint32_t)ctx->gpr[2];
    func_0068EA08_lifted(ctx);
    if (!on) return;
    const uint32_t m = (uint32_t)ctx->gpr[3];
    float p[4] = {0, 0, 0, 0};
    if (a5 >= 0x10000 && a5 < 0xF0000000u) for (int k = 0; k < 4; k++) { uint32_t w = vm_read32(a5 + k * 4); memcpy(&p[k], &w, 4); }
    fprintf(stderr, "[spawn2] idx=%u r4=%08X pos=%.1f %.1f %.1f %.1f rot@%08X scale=%.3f lr=%08X toc=%08X -> %08X obj=%08X\n", idx, a4, p[0], p[1], p[2], p[3],
            a6, sc, lr, (uint32_t)g_tod_spawn_toc2, m, (m >= 0x10000 && m < 0xF0000000u) ? vm_read32(m + 0x40) : 0);
}

/* func_006511D0(r3 = global ART id) -> level class index (0xFFFF or -1 = not in this level; the level map is the gameplay
 * section 0x25500). Spawn research 12/11: TOD_ART_TRACE=1 prints one line per new (id, caller) pair with the result and 6
 * return addresses, to learn which enemy code asks for which id (dependency catalog for foreign enemies). */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
void func_006511D0_lifted(ppu_context* ctx);
static thread_local int t_art_override = -1;   /* set by func_0068EC48 below for a spawn made by foreign code */
extern "C" uint8_t g_tod_xslot[0x400];
extern "C" uint32_t g_tod_xrange[32][2];
extern "C" uint32_t g_tod_xrange_n;
extern "C" uint16_t g_tod_xart[9][0x400];
extern "C" uint32_t g_tod_xrange_src[32];
extern "C" int g_tod_xart_init;
void func_006511D0(ppu_context* ctx)
{
    if (t_art_override >= 0) { ctx->gpr[3] = (uint64_t)(int64_t)t_art_override; t_art_override = -1; return; }
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_ART_TRACE"); on = e && *e && *e != '0'; }
    if (!on) { func_006511D0_lifted(ctx); return; }
    const uint32_t id = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    uint32_t sp = (uint32_t)ctx->gpr[1];
    func_006511D0_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    const int32_t res = (int32_t)ctx->gpr[3];
    uint32_t bt[6] = {0, 0, 0, 0, 0, 0};
    for (int k = 0; k < 6 && sp >= 0x10000 && sp < 0xF0000000u; k++) { sp = vm_read32(sp); if (sp < 0x10000 || sp >= 0xF0000000u) break; bt[k] = vm_read32(sp + 0x10); }
    static std::mutex mu;
    static uint64_t seen[4096]; static int nseen = 0;
    const uint64_t key = ((uint64_t)(id & 0xFFFF) << 48) ^ ((uint64_t)lr << 16) ^ bt[0] ^ ((uint64_t)bt[1] << 24);
    std::lock_guard<std::mutex> g(mu);
    for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
    if (nseen < 4096) seen[nseen++] = key;
    fprintf(stderr, "[art] f=%u id=%X -> %d lr=%08X bt: %08X %08X %08X %08X %08X %08X\n", g_rsx_engine_frame, id, res, lr,
            bt[0], bt[1], bt[2], bt[3], bt[4], bt[5]);
}

/* func_005F5C80(r3 = conduit manager, r4 = event: +0 moby, +4 event id, +8 type): the effect event dispatcher. The manager's
 * +8 is the level's conduit root (fxconduit_packed.dat: +0 entries of 0x2C sorted by event, +4 count, +0x10 event index).
 * 12/11 (fximport=): an event from a moby of an imported (foreign) class is dispatched with the SOURCE level's conduit root,
 * so its effects are the ones its own level would make (their definitions are appended to the host's effect list). */
extern "C" uint32_t g_fx_disp_mgr = 0, g_fx_foreign_root = 0;
extern "C" uint32_t g_fx_host_roots[8], g_fx_foreign_roots[8];   /* the conduit root lists (section 0x12000), host and foreign */
uint32_t g_fx_host_roots[8], g_fx_foreign_roots[8];
extern "C" uint32_t g_fx_nroots = 0;
extern "C" uint32_t g_fx_foreign_T = 0, g_fx_Tslot = 0;   /* source effect id table, and the word holding the level's id table pointer */
extern "C" uint32_t g_fx_foreign_cls[16]; uint32_t g_fx_foreign_cls[16];
extern "C" uint32_t g_fx_foreign_ncls = 0;
extern "C" uint32_t g_fx_src_T[9], g_fx_src_roots[9][8], g_fx_src_nroots[9], g_fx_src_cls[9][16], g_fx_src_ncls[9];   /* 14/11: per source level */
uint32_t g_fx_src_T[9], g_fx_src_roots[9][8], g_fx_src_nroots[9], g_fx_src_cls[9][16], g_fx_src_ncls[9];
void func_005F5C80_lifted(ppu_context* ctx);
void func_005F5C80(ppu_context* ctx)
{
    const uint32_t mgr = (uint32_t)ctx->gpr[3], ev = (uint32_t)ctx->gpr[4];
    g_fx_disp_mgr = mgr;
    uint32_t saved = 0; bool swap = false;
    uint32_t q = 0;
    if (g_fx_foreign_root && ev >= 0x10000 && ev < 0xF0000000u) {
        const uint32_t mo = vm_read32(ev);
        const uint32_t cl = (mo >= 0x10000 && mo < 0xF0000000u) ? vm_read32(mo + 0x54) : 0;
        for (uint32_t s = 0; cl && s < 9 && !swap; s++)
            for (uint32_t i = 0; i < g_fx_src_ncls[s]; i++) if (g_fx_src_cls[s][i] == cl) { swap = true; q = s; break; }
        if (swap) {
            saved = vm_read32(mgr + 8);
            uint32_t fr = 0;
            for (uint32_t i = 0; i < g_fx_src_nroots[q]; i++) if (g_fx_host_roots[i] == saved) { fr = g_fx_src_roots[q][i]; break; }
            if (!fr) swap = false; else vm_write32(mgr + 8, fr);
        }
        if (swap) {
            /* each distinct (moby, event, type) once, with the frame: which events a foreign enemy sends (idle vs attack) */
            static uint32_t seen[256][3]; static uint32_t nseen = 0;
            const uint32_t e4 = vm_read32(ev + 4), e8 = vm_read32(ev + 8);
            bool old = false;
            for (uint32_t i = 0; i < nseen && !old; i++) old = seen[i][0] == mo && seen[i][1] == e4 && seen[i][2] == e8;
            if (!old && nseen < 256) { seen[nseen][0] = mo; seen[nseen][1] = e4; seen[nseen][2] = e8; nseen++;
                fprintf(stderr, "[fximport] f=%u event %X (type %X) of foreign moby %08X: source conduit%c", g_rsx_engine_frame, e4, e8, mo, 10); }
        }
    }
    uint32_t savedT = 0;
    const bool swapT = swap && g_fx_src_T[q] && g_fx_Tslot >= 0x10000 && g_fx_Tslot < 0xF0000000u;
    if (swapT) { savedT = vm_read32(g_fx_Tslot); vm_write32(g_fx_Tslot, g_fx_src_T[q]); }   /* effect ids -> the source level's copies */
    func_005F5C80_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    if (swapT) vm_write32(g_fx_Tslot, savedT);
    if (swap) vm_write32(mgr + 8, saved);
}

/* func_005F9998 (virtual; r3 = this, r4 -> request list?, ...) -> func_005D4040(r3 = effect id, ...) creates an effect.
 * 12/11, TOD_FX_TRACE=1: one line per new (effect id, outer caller): who asks for the effect and with what arguments. */
static thread_local uint32_t t_fx_outer_lr = 0, t_fx_outer_r[7];
void func_005F9998_lifted(ppu_context* ctx);
void func_005F9998(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_FX_TRACE"); on = e && *e && *e != '0'; }
    if (!on) { func_005F9998_lifted(ctx); return; }
    const uint32_t sl = t_fx_outer_lr; uint32_t sr[7]; memcpy(sr, t_fx_outer_r, sizeof sr);
    t_fx_outer_lr = (uint32_t)ctx->lr;
    for (int k = 0; k < 7; k++) t_fx_outer_r[k] = (uint32_t)ctx->gpr[3 + k];
    func_005F9998_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    t_fx_outer_lr = sl; memcpy(t_fx_outer_r, sr, sizeof sr);
}
void func_005D4040_lifted(ppu_context* ctx);
void func_005D4040(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_FX_TRACE"); on = e && *e && *e != '0'; }
    if (on) {
        const uint32_t id = (uint32_t)ctx->gpr[3] & 0xFFFF, lr = (uint32_t)ctx->lr;
        static std::mutex mu; static uint64_t seen[8192]; static int nseen = 0;
        const uint64_t key = ((uint64_t)id << 32) | t_fx_outer_lr;
        std::lock_guard<std::mutex> g(mu);
        bool dup = false;
        for (int k = 0; k < nseen; k++) if (seen[k] == key) { dup = true; break; }
        if (!dup) {
            if (nseen < 8192) seen[nseen++] = key;
            const uint32_t* R = t_fx_outer_r;
            fprintf(stderr, "[fxmake] f=%u id=%X lr=%08X outer lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r4[0..3]=%08X %08X %08X %08X\n",
                    g_rsx_engine_frame, id, lr, t_fx_outer_lr, R[0], R[1], R[2], R[3], R[4], R[5], R[6],
                    (R[1] >= 0x10000 && R[1] < 0xF0000000u) ? vm_read32(R[1]) : 0, (R[1] >= 0x10000 && R[1] < 0xF0000000u) ? vm_read32(R[1] + 4) : 0,
                    (R[1] >= 0x10000 && R[1] < 0xF0000000u) ? vm_read32(R[1] + 8) : 0, (R[1] >= 0x10000 && R[1] < 0xF0000000u) ? vm_read32(R[1] + 12) : 0);
        }
    }
    func_005D4040_lifted(ctx);
}

/* func_005E5B58(r3 = effect id, r4 -> out index): the effect engine's id -> index (through func_00613080 on the level's effect
 * table; returns 1 when found). 12/11: TOD_FX_TRACE=1 prints one line per new (id, caller): this lr is the real requester. */
void func_005E5B58_lifted(ppu_context* ctx);
void func_005E5B58(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_FX_TRACE"); on = e && *e && *e != '0'; }
    if (!on) { func_005E5B58_lifted(ctx); return; }
    const uint32_t id = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr, out = (uint32_t)ctx->gpr[4];
    func_005E5B58_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    const uint32_t r = (uint32_t)ctx->gpr[3];
    static std::mutex mu; static uint64_t seen[8192]; static int nseen = 0;
    const uint64_t key = ((uint64_t)id << 32) | lr;
    std::lock_guard<std::mutex> g(mu);
    for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
    if (nseen < 8192) seen[nseen++] = key;
    fprintf(stderr, "[fxid] f=%u id=%X -> %u idx %X lr=%08X\n", g_rsx_engine_frame, id, r, (r && out >= 0x10000 && out < 0xF0000000u) ? vm_read32(out) : 0xFFFFFFFFu, lr);
}

/* func_0068EC48(r3 = ART id, r4 = update type, r5 -> position, r6 -> rotation, f1 = scale): spawn by global ART id (translated
 * by func_006511D0 with the HOST level's map, then func_0068EA08). 12/11: when the caller is the code of a foreign class
 * (xclass; ranges recorded by the spawn cheat) and the source level maps that id to an imported class, that class is used:
 * the Cragmite mother's shots (ART id 0) are Cragmite class 48, not Rykan's class 159. */
void func_0068EC48_lifted(ppu_context* ctx);
void func_0068EC48(ppu_context* ctx)
{
    const uint32_t id = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr;
    if (g_tod_xrange_n && g_tod_xart_init && id < 0x400) {
        for (uint32_t r = 0; r < g_tod_xrange_n; r++)
            if (lr >= g_tod_xrange[r][0] && lr < g_tod_xrange[r][1]) {
                const uint32_t q = g_tod_xrange_src[r] < 9 ? g_tod_xrange_src[r] : 0;   /* 14/11: that caller's source level */
                if (g_tod_xart[q][id] == 0xFFFF) {
                    /* foreign code spawning an ART id we did not import: the HOST class (if any) is used. Printed once per id so
                     * the missing projectile / helper classes of a foreign enemy can be added with xclass (log line names the id). */
                    static uint8_t miss[9][0x400];
                    if (!miss[q][id]) { miss[q][id] = 1; fprintf(stderr, "[xclass] foreign spawn MISS: ART id %X from %08X source %u (not imported, host map used)%c", id, lr, q, 10); }
                } else {
                    t_art_override = g_tod_xart[q][id];
                    static int said = 0;
                    if (said++ < 8) fprintf(stderr, "[xclass] foreign spawn: ART id %X from %08X -> slot %u%c", id, lr, g_tod_xart[q][id], 10);
                }
                break;
            }
    }
    func_0068EC48_lifted(ctx);
    t_art_override = -1;
}

/* func_00613080(r3 -> effect id table, r4 = effect id) -> the level's effect index, -1 = this level has no such effect. The table
 * is effect.dat section 0x500 (u32 count 0x644, u32, then one s32 per global id); ids >= 10000 are the level's own (index =
 * id - 10000 + [table+4]). Found 12/11 with rwatch= on the table page. TOD_FX_TRACE=1 prints one line per new (id, caller) pair
 * with the result (do foreign enemies ask for effects the host level lacks?). */
void func_00613080_lifted(ppu_context* ctx);
void func_00613080(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_FX_TRACE"); on = e && *e && *e != '0'; }
    if (!on) { func_00613080_lifted(ctx); return; }
    const uint32_t id = (uint32_t)ctx->gpr[4], lr = (uint32_t)ctx->lr, tab = (uint32_t)ctx->gpr[3];
    func_00613080_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    const uint32_t res = (uint32_t)ctx->gpr[3];
    static std::mutex mu; static uint64_t seen[8192]; static int nseen = 0;
    const uint64_t key = ((uint64_t)id << 32) ^ ((uint64_t)tab << 8) ^ lr;
    std::lock_guard<std::mutex> g(mu);
    for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
    if (nseen < 8192) seen[nseen++] = key;
    fprintf(stderr, "[fx] f=%u tab=%08X id=%X -> %08X lr=%08X\n", g_rsx_engine_frame, tab, id, res, lr);
}

/* Research 12/11 (TOD_TYPE_TRACE=1): the update-object factory func_0068F370 (moby manager vtable +0x2C: r3 = manager, r4 = moby,
 * r5 = type id, -1 = the class default) and the type config lookup func_006510E8 (r3 = type id, r4 -> config, r5 -> size;
 * returns 1 when the level has the type). One line per new (type, caller). */
static int type_trace_on()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_TYPE_TRACE"); on = e && *e && *e != '0'; }
    return on;
}
void func_0068F370_lifted(ppu_context* ctx);
void func_0068F370(ppu_context* ctx)
{
    if (!type_trace_on()) { func_0068F370_lifted(ctx); return; }
    const uint32_t moby = (uint32_t)ctx->gpr[4], ty = (uint32_t)ctx->gpr[5], lr = (uint32_t)ctx->lr;
    func_0068F370_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    const uint32_t o = (uint32_t)ctx->gpr[3];
    static std::mutex mu; static uint64_t seen[4096]; static int nseen = 0;
    const uint64_t key = ((uint64_t)ty << 32) | lr;
    std::lock_guard<std::mutex> g(mu);
    for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
    if (nseen < 4096) seen[nseen++] = key;
    const bool ok = o >= 0x10000 && o < 0xF0000000u;
    fprintf(stderr, "[type] f=%u make type=%X moby=%08X -> obj %08X vt %08X type %X lr=%08X\n", g_rsx_engine_frame, ty, moby, o,
            ok ? vm_read32(o) : 0, ok ? vm_read32(o + 4) : 0, lr);
}
static thread_local int t_build_type = 0;   /* type being built by func_00501018 (the sub-call traces print only inside one) */
#define TOD_TY_SUB(FN) \
    void func_##FN##_lifted(ppu_context* ctx); \
    void func_##FN(ppu_context* ctx) \
    { \
        if (!t_build_type) { func_##FN##_lifted(ctx); return; } \
        const uint32_t a3 = (uint32_t)ctx->gpr[3], a4 = (uint32_t)ctx->gpr[4], a5 = (uint32_t)ctx->gpr[5], lr = (uint32_t)ctx->lr; \
        func_##FN##_lifted(ctx); \
        while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); } \
        fprintf(stderr, "[type]   type %X: " #FN "(%08X, %08X, %08X) -> %08X lr=%08X\n", t_build_type, a3, a4, a5, (uint32_t)ctx->gpr[3], lr); \
    }
TOD_TY_SUB(0051E0B8)
TOD_TY_SUB(0051E110)
TOD_TY_SUB(0051E330)
TOD_TY_SUB(00252588)
void func_00501018_lifted(ppu_context* ctx);
void func_00501018(ppu_context* ctx)
{
    if (!type_trace_on()) { func_00501018_lifted(ctx); return; }
    { const uint32_t c5 = (uint32_t)ctx->gpr[5];
      const uint32_t t0 = (c5 >= 0x10000 && c5 < 0xF0000000u) ? vm_read32(c5 + vm_read32(c5 + 0x14)) : 0;
      static int printed513 = 0, printed403 = 0;
      if ((t0 == 0x513 && printed513++ < 2) || (t0 == 0x403 && printed403++ < 2)) t_build_type = (int)t0; }
    struct Clr { ~Clr() { t_build_type = 0; } } clr;
    const uint32_t a3 = (uint32_t)ctx->gpr[3], a4 = (uint32_t)ctx->gpr[4], a5 = (uint32_t)ctx->gpr[5], a6 = (uint32_t)ctx->gpr[6],
                   a7 = (uint32_t)ctx->gpr[7], lr = (uint32_t)ctx->lr;
    func_00501018_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    const uint32_t r = (uint32_t)ctx->gpr[3];
    const uint32_t ty = (a5 >= 0x10000 && a5 < 0xF0000000u) ? vm_read32(a5 + vm_read32(a5 + 0x14)) : 0;
    static std::mutex mu; static uint32_t seen[4096]; static int nseen = 0;
    std::lock_guard<std::mutex> g(mu);
    for (int k = 0; k < nseen; k++) if (seen[k] == (ty ^ (r ? 0x80000000u : 0))) return;
    if (nseen < 4096) seen[nseen++] = ty ^ (r ? 0x80000000u : 0);
    fprintf(stderr, "[type] f=%u build pool=%08X h=%08X cfg=%08X (type %X) size=%X r7=%08X -> %08X vt %08X lr=%08X\n", g_rsx_engine_frame, a3, a4, a5,
            ty, a6, a7, r, (r >= 0x10000 && r < 0xF0000000u) ? vm_read32(r) : 0, lr);
}
void func_006510E8_lifted(ppu_context* ctx);
void func_006510E8(ppu_context* ctx)
{
    if (!type_trace_on()) { func_006510E8_lifted(ctx); return; }
    const uint32_t ty = (uint32_t)ctx->gpr[3], lr = (uint32_t)ctx->lr, pc = (uint32_t)ctx->gpr[4];
    func_006510E8_lifted(ctx);
    while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f((void*)ctx); }
    const uint32_t r = (uint32_t)ctx->gpr[3];
    static std::mutex mu; static uint64_t seen[4096]; static int nseen = 0;
    const uint64_t key = ((uint64_t)ty << 32) | lr;
    std::lock_guard<std::mutex> g(mu);
    for (int k = 0; k < nseen; k++) if (seen[k] == key) return;
    if (nseen < 4096) seen[nseen++] = key;
    fprintf(stderr, "[type] f=%u config type=%X -> %u cfg %08X lr=%08X\n", g_rsx_engine_frame, ty, r, (pc >= 0x10000 && pc < 0xF0000000u) ? vm_read32(pc) : 0, lr);
}

/* func_0068ED48(r3 = moby manager, r4 -> instance record, 0x48 bytes: +0 uid, +0x38 config (pvars), +0x3C u16 class index):
 * the level load's spawn of a placed moby (the manager +0x28 makes the entity, +0x2C the update object with the record's data).
 * Spawn research 11/11: one record per class index is kept, so the 'spawn' cheat can clone a placed enemy anywhere. */
void func_0068ED48_lifted(ppu_context* ctx);
extern "C" uint32_t g_tod_inst_rec[0x400];
uint32_t g_tod_inst_rec[0x400];
extern "C" uint32_t g_tod_inst_mgr = 0, g_tod_inst_toc = 0;
extern "C" uint32_t g_tod_inst_list[8192][3];
uint32_t g_tod_inst_list[8192][3];
extern "C" uint32_t g_tod_inst_n = 0;
extern "C" uint32_t g_tod_spawn_cloning = 0;   /* set by the spawn cheat around its own calls (not a placed instance) */
void func_0068ED48(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_SPAWN_TRACE"); on = e && *e && *e != '0'; }
    const uint32_t mgr = (uint32_t)ctx->gpr[3], rec = (uint32_t)ctx->gpr[4];
    const uint32_t idx = (rec >= 0x10000 && rec < 0xF0000000u) ? vm_read16(rec + 0x3C) : 0xFFFF;
    g_tod_inst_mgr = mgr; g_tod_inst_toc = (uint32_t)ctx->gpr[2];
    if (idx < 0x400 && !g_tod_inst_rec[idx]) g_tod_inst_rec[idx] = rec;
    func_0068ED48_lifted(ctx);
    /* every placed instance: (class index, record, entity), so the cheat can clone the record of a REAL enemy of a class (the
     * same class can have instances with other update types: sp27, clone of the 1st record of idx 27 = vt 00854728, enemy = 00849378) */

    if (!g_tod_spawn_cloning && g_tod_inst_n < 8192 && idx < 0x400) { g_tod_inst_list[g_tod_inst_n][0] = idx; g_tod_inst_list[g_tod_inst_n][1] = rec;
        g_tod_inst_list[g_tod_inst_n][2] = g_tod_last_spawn; g_tod_inst_n++; }
    if (!on) return;
    static int printed = 0;
    if (printed++ < 40) {
        const uint32_t m = (uint32_t)ctx->gpr[3];
        fprintf(stderr, "[spawn3] idx=%u rec=%08X:", idx, rec);
        for (uint32_t k = 0; k < 0x48; k += 4) { uint32_t w = vm_read32(rec + k); float f; memcpy(&f, &w, 4); fprintf(stderr, " %08X(%.4g)", w, f); }
        fprintf(stderr, " -> ent %08X pos %.2f %.2f %.2f obj %08X\n", m, 0.0, 0.0, 0.0, 0u);
    }
}
