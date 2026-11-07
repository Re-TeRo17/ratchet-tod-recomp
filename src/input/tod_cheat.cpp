/*
 * tod_cheat.cpp -- cheat menu for automatic tests and on demand (god, heal, bolts, ammo, status).
 *
 * OFF by default: with none of TOD_CHEAT / TOD_CHEAT_FILE / TOD_CHEAT_KEYS set, tod_cheat_poll()
 * returns at its first test. Every log line starts with "[cheat]".
 *
 * Env:
 *   TOD_CHEAT=god,ammo,bolts=100000   commands run at start (separators , ; space newline)
 *   TOD_CHEAT_FILE=<path>             command file, read every 0.5 s: one command per line, the file
 *                                     is emptied after reading (echo bolts=5000 > file)
 *   TOD_CHEAT_KEYS=1                  F9 = next menu entry (printed on stderr), F10 = run it (window in front)
 *   TOD_CHEAT_TID=<n>                 guest thread allowed to run guest functions (default 1 = PPU main;
 *                                     -1 = any thread that polls the pad: NOT recommended)
 * Commands:
 *   god | god=0 | nogod   invulnerability: HC+0x34 |= 4 (what health_set_invulnerability, func_002814D8,
 *                         does) + refill HC+4 = HC+8 every poll while on
 *   heal                  once: HC+4 = HC+8 (what hero_heal, func_00289240, does)
 *   bolts=N               once: hero_add_bolts(handle, N) = func_0028A850; if the counter [G+0x41C] did
 *                         not move, falls back to a direct write of the counter
 *   ammo | noammo         infinite-ammo flag [[0x8993A0]] (func_00288DE8 writes only that) + once
 *                         hero_give_all_ammo core func_0028AAD0(handle)
 *   weapons               once: gives Ratchet ALL weapons at max level with full ammo. For weapon slot
 *                         idx = 0..30 whose definition exists (func_00462F38(idx) != 0) it calls the core of
 *                         Lua hero_give_weapon (OPD -> func_002BB748 -> func_0028B4B8(handle, idx, ammo, xp)):
 *                         func_00466918 unlocks the slot (slot = [obj+0x1A68] + idx*20; +0 id, +4 float xp,
 *                         +8 float ammo, +0x10 u8 owned (<0 = not owned), +0x11 u8 level), ammo (r5, >-1) goes
 *                         through func_00465B68 (clamped to the max ammo), xp (r6, >-1) through func_00465E50 (level
 *                         = thresholds passed, capped at the weapon's max level from func_00465CB0). Passing a huge
 *                         xp therefore gives the max level. Then hero_give_all_ammo core (0028AAD0) tops up ammo.
 *                         Usage: TOD_CHEAT=god,weapons,ammo
 *                         Slot 31 (DECRYPTOR) is NOT given: the core asserts idx < 0x1F (cmplwi 0x1F in
 *                         func_0028B4B8 and func_002890D8; the assert is only logged but the slot table has
 *                         31 entries, 0..30, so slot 31 would write out of bounds).
 *   equip=N               once: equips weapon slot N (same N as `weapons`), as the wheel does. Calls
 *                         func_002890D8(handle, N), the core of Lua hero_equip (name at 0x89df8c -> OPD 0x86bdc0
 *                         -> func_002B91B0): checks two hero property flags (0xB9, 0x211), then the virtual
 *                         method [[obj+0x1E00]->vtbl+0x1C](obj+0x1E00, slot, 0). The weapon must be owned
 *                         (run `weapons` first; the order inside one command line is kept).
 *   fx=<tag>              prints "[cheat] fx <tag> frame=<n> t=<s>" (n = RSX present counter g_rsx_engine_frame)
 *                         as a marker for log analysis; queued behind pending one-shots to keep the order
 *                         (equip=3,fx=slot3 prints the marker after the equip). Tag: no , ; or space.
 *   trace | trace=<n>     arms the 4-frame RSX draw trace n presents from now (default 1): the same as F8 with
 *                         RSX_TRACE_ON_F8=1 (rsx_draw_engine_trace_after) but without a key.
 *   bighead | enemybighead | mirror   game cheats HERO_BIGHEAD (1), ENEMY_BIGHEAD (3), MIRROR_LEVEL (11) = state 1
 *   cheat=N | cheat=N:S   game cheat N (0..13, enum CHEAT: 0 E3TRAILER 1 HERO_BIGHEAD 2 CONCEPT_CHAR 3 ENEMY_BIGHEAD
 *                         4 TRAILER01 5 DEV_COMMENTS 6 CONCEPT_ENV 7 WRENCH_REPLACE 8 HOW_TO_DRAW 9 JAMES_ZURKON
 *                         10 CONCEPT_WEAP 11 MIRROR_LEVEL 12 SCRIPTSCREEN 13 CONCEPT_PAINT) to state S (default 1;
 *                         0 = off; valid 1..7, the game stores anything else as 0). Core of Lua set_cheat_state
 *                         (OPD 0x86BEC8 -> func_002BBB48: arg1 = cheat, arg2 = state -> func_0027A618(cheat, state)):
 *                         writes u8 [[TOC-0x6FD0] + 0x86F8 + N] (runtime state array, 14 bytes). Also mirrors it into the
 *                         profile array u8 [[TOC-0x6F2C] + 0x31D0 + N] (the one enable_all_cheats fills, so a
 *                         save/level copy does not undo it) and raises the cheat points u32 [[TOC-0x6FD0] + 0x8708]
 *                         (set_cheat_points core func_0027A608) to >= 840 (HERO_BIGHEAD's reader func_0023EDxx skips
 *                         the effect when points < func_00011C50(1), so this is a precaution).
 *                         HERO_BIGHEAD is read by the code at 0x0023EE58..: state 0 = normal head, 1/2/3 = three
 *                         bigger head scales (TOC+0x7C20 / 0x7C24 / 0x7C1C). Others are read when their screen/level
 *                         asks for them (MIRROR_LEVEL, concept art, ...): may need a level reload; not verified.
 *   allcheats             enable_all_cheats core func_0027A648 (profile: every cheat byte 0 -> 1, points = 840) +
 *                         runtime states 0..13 = 1 via func_0027A618 + points 840.
 *   status                hero handle, entity, object, HC, health/max/flags, bolts, ammo flag, position
 *
 * At run time: tod_cheat_enqueue("god,weapons") (any thread; the F1 settings page's Cheats tab uses it) appends commands to the
 * same queue as TOD_CHEAT_FILE and arms the poller when no env var did; tod_cheat_arm() arms it without a command;
 * tod_cheat_state(id) reads god / infinite ammo / a game cheat back. The cheats are never saved in tod.ini.
 *
 * Where it runs: tod_cheat_poll() is called by host_input() (src/input/host_input.cpp), i.e. inside
 * cellPadGetData's host poll, ON THE GUEST THREAD THAT POLLS THE PAD (the game's main loop), the same
 * place as the TOD_BOT. Guest functions are called only when that thread's id is TOD_CHEAT_TID: a COPY
 * of the thread's own ppu_context (g_active_ctx) with r1 = its stack pointer - 0x1000 (dead space below
 * the frame that is inside the HLE call; back chain written), r2 = 0x89FF20 (gameplay/Lua TOC),
 * trampolines drained, g_active_ctx and g_trampoline_fn restored -- as ppu_guest_call does, but on the
 * caller's own stack instead of the fixed 0xCFFE0000 scratch.
 * One-shot commands (heal, bolts, ammo) wait until the hero has been valid for 2 s.
 *
 * Hero handle, 3 sources (hero_handle()): (1) the chain below; (2) g_tod_hero_handle (set_follow_cam hook);
 * (3) entity-table scan (no hook, no chain), done on the guest thread when 1 and 2 fail: entities of the table of
 * func_00276D18 whose object answers non-zero to func_0024F1D8(obj, 0xB9) (hero property that set_follow_cam
 * func_0028BD60 itself requires before accepting a target) and has a health component; the first one wins.
 *
 * Guest facts (static, recompiled/ppu_recomp_001.cpp):
 *   handle = u32 (u64[P+8] & [[0x8885D8]]) >> [[0x8885DC]], P = [0x8885B4]   (func_00024380)
 *   func_00276D18(r3 = &handle): T = [0x898F3C], idx = handle & 0xFFFF, valid if idx <= [T+0x10]
 *     and u32 [ent+0x64] == handle (FULL 32 bits), ent = [T] + idx*384; object = [ent+0x40]
 *   func_00251F68(obj, 0x6C) = virtual GetComponent (stub, trampoline to 0068FD20) -> health component HC:
 *     +4 float health, +8 float max, +0x34 flags (bit 4 = invulnerable)
 *   bolts = u32 [[0x89F1B8] + 0x41C]   (get_hero_bolts, func_002D1BD0)
 *   position: get_pos record table [[0x898F40]] + (handle&0xFFFF)*0x90 + 0x30 (x y z) -- NOT verified
 *     that the hero handle indexes this table (TOD_BOT_DIAG).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "ppu_recomp.h"        /* ppu_context, vm_*, func_XXXXXXXX declarations */
#include "win32_compat.h"      /* GetTickCount64, GetAsyncKeyState */

extern "C" unsigned g_tod_hero_handle;   /* src/main.cpp tod_fntrace: from set_follow_cam */
extern "C" uint32_t g_rsx_engine_frame;                  /* rsx_draw_engine.c: present counter */
extern "C" void rsx_draw_engine_trace_after(long after);  /* rsx_draw_engine.c: arm the 4-frame draw trace */
extern "C" void rsx_draw_engine_pick_after(int x, int y, long after);
extern "C" void tod_input_script_set(const char* s);   /* host_input.cpp: "inp=" command */
extern "C" void tod_tp_log(const char* fmt, ...);      /* tod_logv2_host.cpp: stderr + "teleport" event */
extern "C" void tod_ds_probe(int tag);                  /* tod_dualsense.cpp: dsprobe=<n> command */
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

namespace {

const uint32_t kTocLua      = 0x89FF20;
const uint32_t kP_slot      = 0x8885B4;
const uint32_t kMask_slot   = 0x8885D8;
const uint32_t kShift_slot  = 0x8885DC;
const uint32_t kEntTab_slot = kTocLua - 0x6FE4;   /* 0x898F3C, func_00276D18 */
const uint32_t kRec90_slot  = kTocLua - 0x6FE0;   /* 0x898F40, get_pos records */
const uint32_t kG_slot      = 0x89F1B8;            /* G: +0x41C bolts, +0x420 raritanium */
const uint32_t kInfAmmo_slot = kTocLua - 0x6B80;  /* 0x8993A0, func_00288DE8 */

std::atomic<bool> s_on{false};   /* armed by an env var, or at run time by tod_cheat_enqueue / tod_cheat_arm (settings page) */
bool     s_keys, s_god, s_ammo;
int      s_tid = 1;
std::string s_file, s_start;   /* TOD_CHEAT: run at the first poll (guest RAM loaded by then) */
std::vector<std::string> s_pending;   /* one-shot commands waiting for the hero */
std::atomic<bool> s_busy{false};
std::mutex s_in_mx;                 /* inbox of tod_cheat_enqueue (any thread) */
std::string s_in;
std::atomic<bool> s_in_has{false};

bool gp(uint32_t a) { return a >= 0x10000 && a < 0xF0000000u; }   /* plausible guest pointer */
float rf(uint32_t a) { uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; }
void  wf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(a, u); }

uint32_t hero_handle_chain()
{
    const uint32_t P = vm_read32(kP_slot), pm = vm_read32(kMask_slot), ps = vm_read32(kShift_slot);
    if (!gp(P) || !gp(pm) || !gp(ps)) return 0;
    const uint64_t mask = vm_read64(pm), shift = vm_read64(ps);
    return (shift & 64) ? 0 : (uint32_t)((vm_read64(P + 8) & mask) >> (shift & 63));
}

/* func_00276D18, host side */
uint32_t entity_of(uint32_t h)
{
    if (!h) return 0;
    const uint32_t T = vm_read32(kEntTab_slot);
    if (!gp(T)) return 0;
    const uint32_t idx = h & 0xFFFF, base = vm_read32(T);
    if (!gp(base) || vm_read32(T + 0x10) < idx) return 0;
    const uint32_t ent = base + idx * 384u;
    return vm_read32(ent + 0x64) == h ? ent : 0;
}

uint32_t s_scan_h;   /* hero found by the entity-table scan (source 3) */

uint32_t hero_handle()
{
    const uint32_t h = hero_handle_chain();
    if (entity_of(h)) return h;
    if (entity_of(g_tod_hero_handle)) return g_tod_hero_handle;
    if (entity_of(s_scan_h)) return s_scan_h;
    return 0;
}

bool guest_ok()
{
    const ppu_context* c = g_active_ctx;
    return c && (s_tid < 0 || (int64_t)c->thread_id == s_tid) && gp((uint32_t)c->gpr[1]) && (uint32_t)c->gpr[1] > 0x2000;
}

/* Call a lifted guest function on this guest thread (see the header). */
uint64_t gcall(void (*fn)(ppu_context*), uint64_t a0, uint64_t a1, uint64_t a2 = 0, uint64_t a3 = 0)
{
    ppu_context* cur = g_active_ctx;
    ppu_context c = *cur;
    const uint32_t sp = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFu;
    vm_write64(sp, (uint32_t)cur->gpr[1]);    /* back chain */
    c.gpr[1] = sp; c.gpr[2] = kTocLua; c.gpr[3] = a0; c.gpr[4] = a1; c.gpr[5] = a2; c.gpr[6] = a3; c.lr = 0; c.cr = 0;
    void (*saved_tf)(void*) = g_trampoline_fn;
    g_trampoline_fn = nullptr;
    g_active_ctx = &c;
    fn(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c); }
    g_active_ctx = cur;
    g_trampoline_fn = saved_tf;
    return c.gpr[3];
}

/* health component, cached per (handle, object); re-fetched every 1 s */
uint32_t s_hc, s_hc_h, s_hc_obj; double s_hc_t = -9;
uint32_t health_comp(uint32_t h, double t)
{
    const uint32_t ent = entity_of(h), obj = ent ? vm_read32(ent + 0x40) : 0;
    if (!gp(obj)) { s_hc = 0; return 0; }
    if (guest_ok() && (h != s_hc_h || obj != s_hc_obj || t - s_hc_t >= 1.0)) {
        const uint32_t hc = (uint32_t)gcall(func_00251F68, obj, 0x6C);
        if (hc != s_hc) fprintf(stderr, "[cheat] health component handle=%08X obj=%08X -> HC=%08X\n", h, obj, hc);
        s_hc = gp(hc) ? hc : 0; s_hc_h = h; s_hc_obj = obj; s_hc_t = t;
    }
    return (h == s_hc_h && obj == s_hc_obj) ? s_hc : 0;
}

/* Source 3: scan the entity table for the object with hero property 0xB9 (as set_follow_cam requires). Guest thread only. */
void scan_hero(double t)
{
    static double next = 0;
    if (t < next || !guest_ok()) return;
    next = t + 1.0;
    if (g_rsx_engine_frame < 1200) return;   /* not during the level load: guest calls on half-built objects */
    const uint32_t T = vm_read32(kEntTab_slot);
    if (!gp(T)) return;
    const uint32_t base = vm_read32(T), maxi = vm_read32(T + 0x10);
    if (!gp(base) || maxi > 0x4000) return;
    int cands = 0;
    for (uint32_t idx = 0; idx <= maxi; idx++) {
        const uint32_t ent = base + idx * 384u, h = vm_read32(ent + 0x64), obj = vm_read32(ent + 0x40);
        if (!h || h == 0xFFFFFFFFu || (h & 0xFFFF) != idx || !gp(obj) || !gp(vm_read32(obj))) continue;
        if (!((uint32_t)gcall(func_0024F1D8, obj, 0xB9) & 0xFF)) continue;
        const uint32_t hc = (uint32_t)gcall(func_00251F68, obj, 0x6C);
        if (cands < 8) fprintf(stderr, "[cheat] scan: candidate handle=%08X ent=%08X obj=%08X HC=%08X\n", h, ent, obj, hc);
        cands++;
        if (gp(hc) && !s_scan_h) { s_scan_h = h; fprintf(stderr, "[cheat] hero found by entity scan: handle=%08X (idx %u of %u)\n", h, idx, maxi); }
    }
}

const uint32_t kCheatBase_slot = kTocLua - 0x6FD0;   /* base B: +0x86F8+N cheat state u8, +0x8708 points u32 */
const uint32_t kProfile_slot   = kTocLua - 0x6F2C;   /* profile P: +0x31D0+N cheat state u8, +0x31E0 points */

void set_cheat(long n, long st)
{
    if (n < 0 || n > 13) { fprintf(stderr, "[cheat] cheat: N=%ld out of range 0..13\n", n); return; }
    if (st < 0 || st > 7) st = 0;
    gcall(func_0027A618, (uint64_t)n, (uint64_t)st);   /* core of Lua set_cheat_state */
    const uint32_t P = vm_read32(kProfile_slot), B = vm_read32(kCheatBase_slot);
    if (gp(P)) vm_write8(P + 0x31D0 + (uint32_t)n, (uint8_t)st);
    if (st && gp(B) && vm_read32(B + 0x8708) < 840) gcall(func_0027A608, 840, 0);   /* core of set_cheat_points */
    fprintf(stderr, "[cheat] cheat %ld = state %ld: runtime u8[%08X]=%u points=%u\n", n, st, gp(B) ? B + 0x86F8 + (uint32_t)n : 0,
            gp(B) ? vm_read8(B + 0x86F8 + (uint32_t)n) : 0, gp(B) ? vm_read32(B + 0x8708) : 0);
}

/* ---- hero memory diff: "snap" then "diff" (cheat commands). Snapshots the hero entity (0x180 B), its object
 * (0x400 B) and, two pointer levels down, 0x100 B behind each plausible pointer; "diff" prints the float
 * triples that all changed (>0.05) with the path to them: how the live position was located. */
struct Reg { uint32_t addr, len, src; int lvl; std::vector<uint32_t> w; };
std::vector<Reg> s_snap;
bool ptr_ok(uint32_t p) { return p >= 0x10000000u && p < 0x40000000u && !(p & 3); }
void snap_hero(uint32_t h)
{
    s_snap.clear();
    const uint32_t ent = entity_of(h), obj = ent ? vm_read32(ent + 0x40) : 0;
    if (!ent) { fprintf(stderr, "[snap] no hero\n"); return; }
    auto add = [&](uint32_t a, uint32_t len, uint32_t src, int lvl) -> bool {
        for (auto& r : s_snap) if (r.addr == a) return false;
        Reg r{ a, len, src, lvl, {} };
        for (uint32_t i = 0; i < len; i += 4) r.w.push_back(vm_read32(a + i));
        s_snap.push_back(std::move(r)); return true;
    };
    add(ent, 0x180, 0, 0);
    if (ptr_ok(obj)) add(obj, 0x400, ent + 0x40, 0);
    for (int lvl = 1; lvl <= 2; lvl++) {
        const size_t n = s_snap.size();
        for (size_t i = 0; i < n && s_snap.size() < 6000; i++) {
            if (s_snap[i].lvl != lvl - 1) continue;
            const Reg r = s_snap[i];
            for (uint32_t k = 0; k < r.w.size(); k++) if (ptr_ok(r.w[k])) add(r.w[k], 0x100, r.addr + k * 4, lvl);
        }
    }
    fprintf(stderr, "[snap] hero %08X ent=%08X obj=%08X: %zu regions\n", h, ent, obj, s_snap.size());
}
void diff_hero()
{
    int shown = 0;
    for (auto& r : s_snap) {
        for (uint32_t k = 0; k + 2 < r.w.size(); k++) {
            float a[3], b[3]; bool ok = true;
            for (int j = 0; j < 3 && ok; j++) {
                const uint32_t u0 = r.w[k + j], u1 = vm_read32(r.addr + (k + j) * 4);
                memcpy(&a[j], &u0, 4); memcpy(&b[j], &u1, 4);
                ok = isfinite(a[j]) && isfinite(b[j]) && fabsf(b[j]) < 1e5f && fabsf(a[j] - b[j]) > 0.05f;
            }
            if (ok && shown++ < 120)
                fprintf(stderr, "[snap] lvl%d region=%08X (from %08X) +0x%X: %.3f %.3f %.3f -> %.3f %.3f %.3f\n", r.lvl, r.addr, r.src, k * 4, a[0], a[1], a[2], b[0], b[1], b[2]);
        }
    }
    fprintf(stderr, "[snap] diff done: %d triples\n", shown);
}

/* "ents": one line per live entity (entity table T: base [T], count [T+0x10], 384 B each): index, handle, obj,
 * position (ent+0x30), obj vtable and any C-strings reachable from ent/obj words (names / classes). */
std::string str_at(uint32_t p)
{
    std::string s;
    if (!ptr_ok(p) && !(p >= 0x10000 && p < 0x1000000)) return s;
    for (int i = 0; i < 48; i++) {
        const unsigned char c = vm_read8(p + i);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7E) return std::string();
        s += (char)c;
    }
    return s.size() >= 4 ? s : std::string();
}
void dump_ents()
{
    const uint32_t T = vm_read32(kEntTab_slot);
    if (!gp(T)) return;
    const uint32_t base = vm_read32(T), cnt = vm_read32(T + 0x10);
    const char* pa = getenv("TOD_ENTS_FILE");
    FILE* f = fopen(pa ? pa : "ents.txt", "w");
    if (!f) return;
    int n = 0;
    for (uint32_t i = 0; i <= cnt && i < 0x4000; i++) {
        const uint32_t ent = base + i * 384u, h = vm_read32(ent + 0x64), obj = vm_read32(ent + 0x40);
        if (!h || (h & 0xFFFF) != i) continue;
        fprintf(f, "e%u h=%08X ent=%08X obj=%08X pos=%.2f,%.2f,%.2f vt=%08X", i, h, ent, obj, rf(ent + 0x30), rf(ent + 0x34), rf(ent + 0x38),
                ptr_ok(obj) ? vm_read32(obj) : 0);
        for (uint32_t k = 0; k < 0x180; k += 4) { const std::string s = str_at(vm_read32(ent + k)); if (!s.empty()) fprintf(f, " ent+%X:\"%s\"", k, s.c_str()); }
        if (ptr_ok(obj)) for (uint32_t k = 0; k < 0x100; k += 4) { const std::string s = str_at(vm_read32(obj + k)); if (!s.empty()) fprintf(f, " obj+%X:\"%s\"", k, s.c_str()); }
        fprintf(f, " w0=%08X,%08X,%08X,%08X,%08X,%08X\n", vm_read32(ent), vm_read32(ent + 4), vm_read32(ent + 8), vm_read32(ent + 0xC), vm_read32(ent + 0x10), vm_read32(ent + 0x14));
        n++;
    }
    fclose(f);
    fprintf(stderr, "[ents] %d live entities of %u -> %s\n", n, cnt, pa ? pa : "ents.txt");
}

/* Live hero position = entity+0x30 (x,y,z; Y up; the entity starts with its 4x4 matrix, rows at +0,+0x10,+0x20, translation +0x30),
 * verified 25/10 by walking (snap/diff). Writing the entity (and the copies at +0x70, [ent+0x80]+0x40/+0x60) does NOT move
 * Ratchet: the controller state lives elsewhere. What works is the game's own Lua hero_warp (OPD 0x86BDF0 -> func_002B99F0 ->
 * core func_0028A070(hero handle, node id, flag)): node id indexes the get_pos record table ([[0x898F40]] + id*0x90, 4x4 matrix
 * at +0, translation +0x30; func_00276DB8) and must be < [[TOC-0x6F18]] (else assert -> sys_process_exit(1): 1st try passed the
 * entity handle 0x02990298 and the game exited). The core copies that matrix and calls the hero object's virtual warp
 * (vtable +0x58). To warp to a point: take an unused (all-zero) record id, write hero matrix + translation there, call, zero it. */
int s_warp_flag = 0;   /* TOD_WARP_FLAG / wflag=N: third argument of the core (Lua bool) */
bool warp_to(uint32_t h, float x, float y, float z)
{
    const uint32_t ent = entity_of(h);
    if (!ent) { fprintf(stderr, "[warp] no hero entity%c", 10); return false; }
    if (!guest_ok()) { fprintf(stderr, "[warp] not on the guest thread%c", 10); return false; }
    const uint32_t pb = vm_read32(kTocLua - 0x6F18), bound = gp(pb) ? vm_read32(pb) : 0;
    const uint32_t rtab = vm_read32(kRec90_slot), rbase = gp(rtab) ? vm_read32(rtab) : 0;
    fprintf(stderr, "[warp] bound=[%08X]=%u records=%08X%c", pb, bound, rbase, 10);
    if (!bound || bound > 0x10000 || !gp(rbase)) { fprintf(stderr, "[warp] bad bound/records: abort%c", 10); return false; }
    uint32_t id = 0;
    for (uint32_t i = bound - 1; i > 0 && i + 4096 > bound; i--) {
        const uint32_t r = rbase + i * 0x90u; bool zero = true;
        for (uint32_t k = 0; k < 0x90 && zero; k += 4) zero = vm_read32(r + k) == 0;
        if (zero) { id = i; break; }
    }
    /* No all-zero record (fastoon: all 340 in use, 30/10): borrow the last one and put it back after the call. */
    uint32_t saved[0x90 / 4]; const bool borrow = !id;
    if (borrow) id = bound - 1;
    const uint32_t rec = rbase + id * 0x90u;
    if (borrow) for (uint32_t k = 0; k < 0x90; k += 4) saved[k / 4] = vm_read32(rec + k);
    const float ox = rf(ent + 0x30), oy = rf(ent + 0x34), oz = rf(ent + 0x38);
    for (uint32_t k = 0; k < 0x30; k += 4) vm_write32(rec + k, vm_read32(ent + k));
    wf(rec + 0x30, x); wf(rec + 0x34, y); wf(rec + 0x38, z); vm_write32(rec + 0x3C, vm_read32(ent + 0x3C));
    gcall(func_0028A070, h, id, (uint64_t)s_warp_flag);
    for (uint32_t k = 0; k < 0x90; k += 4) vm_write32(rec + k, borrow ? saved[k / 4] : 0);
    fprintf(stderr, "[warp] hero %08X ent=%08X (%.2f,%.2f,%.2f) -> (%.2f,%.2f,%.2f) via record %u%s flag=%d, now ent=(%.2f,%.2f,%.2f)%c", h, ent, ox, oy, oz,
            x, y, z, id, borrow ? " (borrowed)" : "", s_warp_flag, rf(ent + 0x30), rf(ent + 0x34), rf(ent + 0x38), 10);
    return true;
}
/* cinedump: state of the cinematic manager T=[[0x88FF38]-0x4E4C]... (same object as the name registry): +0x58 active flag, +0x5C stamp, command copy at +0x64.., per-id record ptr at +0xD80+id*4 */
void cine_dump(const char* tag, int id)
{
    const uint32_t T = vm_read32(kTocLua - 0x10000 + 0x18 - 0x4E4C);
    fprintf(stderr, "[cine-dump] %s T=%08X:", tag, T);
    for (uint32_t k = 0x50; k < 0x80; k += 4) fprintf(stderr, " +%X=%08X", k, vm_read32(T + k));
    fprintf(stderr, " | cmd@+130:"); for (uint32_t k = 0x130; k < 0x170; k += 4) fprintf(stderr, " %08X", vm_read32(T + k));
    fprintf(stderr, " | ring idx=%08X cnt=%08X%c", vm_read32(T + 0x170 + 0x400), vm_read32(T + 0x170 + 0x404), 10);
    { const uint32_t r2 = kTocLua - 0x10000 + 0x18, P = vm_read32(r2 + 0x16FC0), D = vm_read32(r2 - 0x4D80), V = vm_read32(r2 - 0x4D4C);
      fprintf(stderr, "[cine-dump] P=%08X time10=%.4f t14=%.4f t18=%.4f idx1C=%u cnt24=%u 28=%08X 2C=%08X data30=%08X 3C=%08X | D=%08X dt4=%.5f dt8=%.5f B4=%.3f | V=%08X *V=%d%c",
              P, rf(P + 0x10), rf(P + 0x14), rf(P + 0x18), vm_read32(P + 0x1C), vm_read32(P + 0x24), vm_read32(P + 0x28), vm_read32(P + 0x2C), vm_read32(P + 0x30), vm_read32(P + 0x3C),
              D, rf(D + 4), rf(D + 8), rf(D + 0xB4), V, (int)vm_read32(V), 10);
      const uint32_t dp = vm_read32(P + 0x30); if (gp(dp)) fprintf(stderr, "[cine-dump] anim@%08X: %08X %08X %08X %08X(+C frames=%d)%c", dp, vm_read32(dp), vm_read32(dp + 4), vm_read32(dp + 8), vm_read32(dp + 0xC), (int)vm_read32(dp + 0xC), 10); }
    { /* VRAM mirror probe: where the anim container parser (func_004C2CD0) saw its chunks at queue time (0xC4EBD400..0xC4ED9F00) */
      uint32_t nz = 0, nzbig = 0; for (uint32_t a = 0xC4EBD400u; a < 0xC4ED9F00u; a += 4) if (vm_read32(a)) nz++;
      for (uint32_t a = 0xC4000000u; a < 0xC5000000u; a += 4096) if (vm_read32(a) | vm_read32(a + 2048)) nzbig++;
      fprintf(stderr, "[cine-dump] vram C4EBD400..C4ED9F00 nonzero words=%u / %u ; 4K-pages with data in C4000000..C5000000 = %u / 4096 ; C4EBD580: %08X %08X %08X %08X%c", nz, (0xC4ED9F00u - 0xC4EBD400u) / 4,
              nzbig, vm_read32(0xC4EBD580u), vm_read32(0xC4EBD584u), vm_read32(0xC4EBD588u), vm_read32(0xC4EBD58Cu), 10); }
    if (id >= 0) { const uint32_t rp = vm_read32(T + 0xD80 + id * 4);
        fprintf(stderr, "[cine-dump] rec[%d]=%08X:", id, rp);
        if (gp(rp)) for (uint32_t k = 0; k < 0x40; k += 4) fprintf(stderr, " %08X", vm_read32(rp + k));
        fprintf(stderr, "%c", 10); }
}
/* cine=<name>: runs the Lua C function queue_cinematic (Lua table name->OPD 0x86C560 -> func_002CAB78; the Lua wrapper play_cinematic(name,
 * obviate_pod,unobviate_pod,warp_volume) only forwards to it). Lua 5.0.2: lua_State +8 = top, +0xC = base, TValue = 8 bytes {tt,value}. Main L =
 * [[TOC-0x6F80]+4]. Method: lua_pushstring(L,name) (func_002506E8), temporarily point L->base at the pushed string, call the C function (args
 * 2..4 default), restore base/top. Result (bool) = slot at top-8. */
bool cine_run(const std::string& name)
{
    if (!guest_ok()) { fprintf(stderr, "[cine] not on the guest thread%c", 10); return false; }
    const uint32_t sc = vm_read32(kTocLua - 0x6F80), L = gp(sc) ? vm_read32(sc + 4) : 0;
    if (!gp(L)) { fprintf(stderr, "[cine] no lua_State (sc=%08X)%c", sc, 10); return false; }
    const uint32_t otop = vm_read32(L + 8), obase = vm_read32(L + 0xC);
    const uint32_t buf = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
    for (size_t i = 0; i <= name.size() && i < 120; i++) vm_write8(buf + (uint32_t)i, i < name.size() ? (uint8_t)name[i] : 0);
    fprintf(stderr, "[cine] L=%08X top=%08X base=%08X name='%s'%c", L, otop, obase, name.c_str(), 10);
    { /* name registry used by core func_0027B5B8 (stub 0x24F4D8 -> func_0007B3B0): bucket = djb2-like hash 9 bits, slot [[TOC-0x4E4C]+0x580+bucket*4] */
      uint32_t hv = 0, cnt = 0; { uint32_t h = 0x1505; for (size_t i = 0; i < name.size(); i++) h = h * 33u + (uint8_t)name[i]; hv = h & 0x1FF; }
      const uint32_t tb = vm_read32(kTocLua - 0x10000 + 0x18 - 0x4E4C);
      for (uint32_t i = 0; i < 512; i++) if (vm_read32(tb + 0x580 + i * 4)) cnt++;
      for (uint32_t i = 0, k = 0; i < 512 && k < 3; i++) { const uint32_t v = vm_read32(tb + 0x580 + i * 4); if (v) { k++; fprintf(stderr, "[cine] slot %u = %08X%c", i, v, 10); } }
      fprintf(stderr, "[cine] registry tb=%08X nonzero slots=%u, bucket(%s)=%u -> %08X; lookup stub 24F4D8('%s') = %d%c", tb, cnt, name.c_str(), hv,
              vm_read32(tb + 0x580 + hv * 4), name.c_str(), (int)(int32_t)gcall(func_0024F4D8, buf, 0), 10); }
    { const uint32_t cid = (uint32_t)gcall(func_002D1460, 0, 0), tb6 = vm_read32(kTocLua - 0x6FD0), row = tb6 + ((cid << 2) + 0x10000u - 31088u);
      fprintf(stderr, "[cine] ctx id=%u row=%08X mask[+0x14]=%08X (tb=%08X)%c", cid, row, vm_read32(row + 0x14), tb6, 10); }
    gcall(func_002506E8, L, buf);
    vm_write32(L + 0xC, otop);
    const uint64_t r = gcall(func_002CAB78, L, 0);
    const uint32_t ntop = vm_read32(L + 8);
    fprintf(stderr, "[cine] queue_cinematic returned %d, top now %08X (+%u TValues), result tt=%u val=%u%c", (int)(int32_t)r, ntop, (ntop - otop) / 8,
            ntop >= otop + 8 ? vm_read32(ntop - 8) : 99u, ntop >= otop + 8 ? vm_read32(ntop - 4) : 99u, 10);
    vm_write32(L + 8, otop); vm_write32(L + 0xC, obase);
    cine_dump("after-queue", (int)(int32_t)gcall(func_0024F4D8, buf, 0));
    return true;
}


/* ---- lua exec: runs a Lua source chunk inside the game's own Lua 5.0 VM (the one that runs the level scripts, so every global such as
 * Ratchet1, Clank1, B_Checkpoint_Pos_1 and every game function: switch_to_clank, hero_warp, set_segment, checkpoint_volume ... is visible).
 * Method (same hack as cine_run): push the source as a Lua string (func_002506E8 lua_pushstring), point L->base at it, call the base-library
 * C function `loadstring` (luaB_loadstring, ELF reg table 0x840E30 -> OPD 0x87DE90 -> func_0066E378, TOC 0x8AFE5C) which leaves the compiled
 * function on top, then point L->base at that function and call `pcall` (luaB_pcall, OPD 0x87DEC0 -> func_0066E7A8): it runs the chunk in
 * protected mode (errors are caught by the game's setjmp/longjmp). Result: pcall's boolean ends up at L->base. */
const uint32_t kTocLib = 0x8AFE5C;   /* TOC of the Lua base library OPDs */
uint64_t gcall_toc(void (*fn)(ppu_context*), uint32_t toc, uint64_t a0)
{
    ppu_context* cur = g_active_ctx;
    ppu_context c = *cur;
    const uint32_t sp = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFu;
    vm_write64(sp, (uint32_t)cur->gpr[1]);
    c.gpr[1] = sp; c.gpr[2] = toc; c.gpr[3] = a0; c.gpr[4] = 0; c.lr = 0; c.cr = 0;
    void (*saved_tf)(void*) = g_trampoline_fn;
    g_trampoline_fn = nullptr;
    g_active_ctx = &c;
    fn(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c); }
    g_active_ctx = cur;
    g_trampoline_fn = saved_tf;
    return c.gpr[3];
}
bool lua_exec(const std::string& code)
{
    if (!guest_ok()) { fprintf(stderr, "[lua] not on the guest thread%c", 10); return false; }
    const uint32_t sc = vm_read32(kTocLua - 0x6F80), L = gp(sc) ? vm_read32(sc + 4) : 0;
    if (!gp(L)) { fprintf(stderr, "[lua] no lua_State (sc=%08X)%c", sc, 10); return false; }
    if (code.size() > 0x600) { fprintf(stderr, "[lua] chunk too long (%zu > 1536)%c", code.size(), 10); return true; }
    const uint32_t otop = vm_read32(L + 8), obase = vm_read32(L + 0xC);
    const uint32_t buf = ((uint32_t)g_active_ctx->gpr[1] - 0x900u - (uint32_t)code.size() - 16u) & ~0xFu;
    for (size_t i = 0; i <= code.size(); i++) vm_write8(buf + (uint32_t)i, i < code.size() ? (uint8_t)code[i] : 0);
    fprintf(stderr, "[lua] exec L=%08X top=%08X base=%08X: %s%c", L, otop, obase, code.c_str(), 10);
    gcall(func_002506E8, L, buf);                 /* lua_pushstring(L, buf) */
    vm_write32(L + 0xC, otop);                    /* base -> the string = arg 1 */
    const uint32_t nres = (uint32_t)gcall_toc(func_0066E378, kTocLib, L);   /* loadstring */
    const uint32_t t1 = vm_read32(L + 8);
    const uint32_t tt = t1 >= otop + 16 ? vm_read32(t1 - 8) : 99u;
    if (nres != 1 || tt != 6) {
        fprintf(stderr, "[lua] loadstring failed: nres=%u top-slot tt=%u", nres, tt);
        if (t1 >= 8) { const uint32_t sp2 = vm_read32(t1 - 4); for (uint32_t o = 8; o <= 32 && gp(sp2); o += 4) { const std::string m = str_at(sp2 + o); if (!m.empty()) { fprintf(stderr, " msg(+%u)='%s'", o, m.c_str()); break; } } }
        fprintf(stderr, "%c", 10);
        vm_write32(L + 8, otop); vm_write32(L + 0xC, obase);
        return true;
    }
    vm_write32(L + 0xC, t1 - 8);                  /* base -> the compiled function */
    const uint32_t r = (uint32_t)gcall_toc(func_0066E7A8, kTocLib, L);       /* pcall(f) */
    const uint32_t ntop = vm_read32(L + 8), nb = vm_read32(L + 0xC);
    fprintf(stderr, "[lua] pcall returned %u, status slot tt=%u val=%u%c", r, vm_read32(nb), vm_read32(nb + 4), 10);
    vm_write32(L + 8, otop); vm_write32(L + 0xC, obase);
    (void)ntop;
    return true;
}

/* The live Ratchet entity (30/10, fastoon): the one whose object vtable is 0x0084A3F8. The hero handle the cheats
 * use (hero_handle()) names an entity that stays at the spawn point, so warping it or logging it does nothing. */
uint32_t player_ent(uint32_t* hout)
{
    const uint32_t T = vm_read32(kEntTab_slot);
    if (!gp(T)) return 0;
    const uint32_t base = vm_read32(T), cnt = vm_read32(T + 0x10);
    for (uint32_t i = 0; i <= cnt && i < 0x4000; i++) {
        const uint32_t ent = base + i * 384u, h = vm_read32(ent + 0x64), obj = vm_read32(ent + 0x40);
        if (!h || (h & 0xFFFF) != i || !ptr_ok(obj) || vm_read32(obj) != 0x0084A3F8u) continue;
        if (hout) *hout = h;
        return ent;
    }
    return 0;
}

bool warp_cmd(const std::string& c, uint32_t h)   /* warp=x:y:z | warpe=<entity index>[:dx] */
{
    if (!c.compare(0, 5, "warp=")) {
        float v[3] = { 0, 0, 0 };
        if (sscanf(c.c_str() + 5, "%f:%f:%f", &v[0], &v[1], &v[2]) != 3) { fprintf(stderr, "[warp] bad %s%c", c.c_str(), 10); return true; }
        uint32_t ph = 0;
        if (player_ent(&ph) && ph) h = ph;
        warp_to(h, v[0], v[1], v[2]);
        return true;
    }
    unsigned idx = 0; float dist = 2.0f;
    sscanf(c.c_str() + 6, "%u:%f", &idx, &dist);
    const uint32_t T = vm_read32(kEntTab_slot), base = gp(T) ? vm_read32(T) : 0;
    const uint32_t e = base + idx * 384u;
    if (!base || vm_read32(e + 0x64) == 0) { fprintf(stderr, "[warp] entity %u not live%c", idx, 10); return true; }
    warp_to(h, rf(e + 0x30) + dist, rf(e + 0x34), rf(e + 0x38));
    return true;
}
struct Timed { double t; std::string c; };
std::vector<Timed> s_timed;   /* TOD_WARP / TOD_WARP_MOBY: run once the hero is ready */
std::vector<Timed> s_lua_timed;   /* TOD_LUA_EXEC=chunk@sec|chunk@sec : Lua chunks run once the hero is ready */
std::mutex s_luaq_mx; std::vector<std::string> s_luaq;   /* usr123: tod_lua_enqueue (teleport menu): Lua chunks from any thread, run on the guest thread once the hero is ready */
std::string s_lua_file;   /* TOD_LUA_FILE: whole-file chunk, run and emptied when non-empty (polled every 0.5 s) */

uint32_t bolts_addr() { const uint32_t G = vm_read32(kG_slot); return gp(G) ? G + 0x41C : 0; }

void status(double t)
{
    const uint32_t hc_ = hero_handle_chain(), h = hero_handle(), ent = entity_of(h), obj = ent ? vm_read32(ent + 0x40) : 0;
    const uint32_t hc = h ? health_comp(h, t) : 0, ba = bolts_addr(), ia = vm_read32(kInfAmmo_slot);
    fprintf(stderr, "[cheat] status t=%.1f handle=%08X (chain %08X, set_follow_cam %08X) ent=%08X obj=%08X HC=%08X",
            t, h, hc_, g_tod_hero_handle, ent, obj, hc);
    if (hc) fprintf(stderr, " health=%.2f/%.2f flags=%08X%s", rf(hc + 4), rf(hc + 8), vm_read32(hc + 0x34), (vm_read32(hc + 0x34) & 4) ? " (invuln)" : "");
    if (ba) fprintf(stderr, " bolts=%u raritanium=%u", vm_read32(ba), vm_read32(ba + 4));
    if (gp(ia)) fprintf(stderr, " infammo=%u", vm_read32(ia));
    const uint32_t rb = gp(vm_read32(kRec90_slot)) ? vm_read32(vm_read32(kRec90_slot)) : 0;
    if (h && gp(rb)) { const uint32_t r = rb + (h & 0xFFFF) * 0x90u;
        fprintf(stderr, " pos(rec90,?)=%.2f %.2f %.2f", rf(r + 0x30), rf(r + 0x34), rf(r + 0x38)); }
    fprintf(stderr, " god=%d ammo=%d tid=%lld\n", (int)s_god, (int)s_ammo, g_active_ctx ? (long long)g_active_ctx->thread_id : -1ll);
}

/* one-shot command with the hero valid and guest calls allowed; false = keep pending */
bool run_oneshot(const std::string& c, uint32_t h, double t)
{
    if (c == "heal") {
        const uint32_t hc = health_comp(h, t);
        if (!hc) return false;
        fprintf(stderr, "[cheat] heal %.2f -> %.2f\n", rf(hc + 4), rf(hc + 8));
        wf(hc + 4, rf(hc + 8));
        return true;
    }
    if (!c.compare(0, 6, "bolts=")) {
        const long n = strtol(c.c_str() + 6, 0, 10);
        const uint32_t ba = bolts_addr();
        const uint32_t before = ba ? vm_read32(ba) : 0;
        gcall(func_0028A850, h, (uint64_t)(int64_t)n);   /* hero_add_bolts(handle, n) */
        const uint32_t after = ba ? vm_read32(ba) : 0;
        fprintf(stderr, "[cheat] bolts += %ld via 0028A850: [%08X] %u -> %u\n", n, ba, before, after);
        if (ba && after == before && n) {
            vm_write32(ba, before + (uint32_t)n);
            fprintf(stderr, "[cheat] bolts unchanged by the game call: direct write %u -> %u\n", before, vm_read32(ba));
        }
        return true;
    }
    if (c == "weapons") {
        int given = 0, tried = 0;
        for (unsigned idx = 0; idx < 31; idx++) {
            if (!(uint32_t)gcall(func_00462F38, idx, 0)) continue;   /* no weapon definition in this slot */
            tried++;
            /* hero_give_weapon core: r3 handle, r4 slot, r5 ammo (-1 = untouched), r6 xp (-1 = untouched) */
            gcall(func_0028B4B8, h, idx, 99999, 100000000);
            given++;
        }
        gcall(func_0028AAD0, h, 0);   /* hero_give_all_ammo core: refill to the new (max-level) caps */
        fprintf(stderr, "[cheat] weapons: given %d (defs found %d of 31 slots), level max (xp=1e8, capped at each weapon top level), ammo full\n", given, tried);
        return true;
    }
    if (!c.compare(0, 6, "equip=")) {
        const long n = strtol(c.c_str() + 6, 0, 10);
        if (n < 0 || n >= 31) { fprintf(stderr, "[cheat] equip: slot %ld out of range 0..30\n", n); return true; }
        gcall(func_002890D8, h, (uint64_t)(int64_t)n);   /* core of Lua hero_equip */
        fprintf(stderr, "[cheat] equip slot %ld via 002890D8\n", n);
        return true;
    }
    if (!c.compare(0, 3, "fx=")) {
        fprintf(stderr, "[cheat] fx %s frame=%u t=%.1f\n", c.c_str() + 3, g_rsx_engine_frame, t);
        return true;
    }
    if (!c.compare(0, 6, "cheat=")) {
        char* e = nullptr;
        const long n = strtol(c.c_str() + 6, &e, 10);
        const long st = (e && *e == ':') ? strtol(e + 1, 0, 10) : 1;
        set_cheat(n, st);
        return true;
    }
    if (c == "bighead") { set_cheat(1, 1); return true; }
    if (c == "enemybighead") { set_cheat(3, 1); return true; }
    if (c == "mirror") { set_cheat(11, 1); return true; }
    if (c == "allcheats") {
        gcall(func_0027A648, 0, 0);   /* enable_all_cheats core: profile cheat bytes 0 -> 1, points 840 */
        for (long n = 0; n < 14; n++) set_cheat(n, 1);
        fprintf(stderr, "[cheat] allcheats done (func_0027A648 + states 0..13 = 1)\n");
        return true;
    }
    if (!c.compare(0, 8, "dsprobe=")) {   /* tod_dualsense.cpp: where does the hero keep this value (see equip=N) */
        tod_ds_probe((int)strtol(c.c_str() + 8, 0, 10));
        return true;
    }
    if (c == "giveammo") {
        gcall(func_0028AAD0, h, 0);   /* hero_give_all_ammo core */
        fprintf(stderr, "[cheat] give all ammo (0028AAD0) done\n");
        return true;
    }
    fprintf(stderr, "[cheat] unknown command '%s'\n", c.c_str());
    return true;
}

void command(std::string c, double t)
{
    while (!c.empty() && (c.back() == '\r' || c.back() == ' ')) c.pop_back();
    if (c.empty() || c[0] == '#') return;
    for (auto& ch : c) if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    fprintf(stderr, "[cheat] command: %s\n", c.c_str());
    if (c == "god" || c == "god=1") { s_god = true; return; }
    if (c == "nogod" || c == "god=0") {
        s_god = false;
        if (const uint32_t h = hero_handle()) if (const uint32_t hc = health_comp(h, t)) vm_write32(hc + 0x34, vm_read32(hc + 0x34) & ~4u);
        return;
    }
    if (c == "ammo" || c == "ammo=1") { s_ammo = true; s_pending.push_back("giveammo"); return; }
    if (c == "noammo" || c == "ammo=0") {
        s_ammo = false;
        const uint32_t ia = vm_read32(kInfAmmo_slot);
        if (gp(ia)) vm_write32(ia, 0);
        return;
    }
    if (c == "status") { status(t); return; }
    if (c == "snap") { snap_hero(hero_handle()); return; }
    if (c == "diff") { diff_hero(); return; }
    if (c == "ents") { dump_ents(); return; }
    if (!c.compare(0, 5, "cine=")) { cine_run(c.substr(5)); return; }
    if (!c.compare(0, 8, "cinedump")) { cine_dump("poll", c.size() > 9 ? atoi(c.c_str() + 9) : 1); return; }
    if (!c.compare(0, 6, "wflag=")) { s_warp_flag = atoi(c.c_str() + 6); return; }
    if (!c.compare(0, 5, "warp=") || !c.compare(0, 6, "warpe=")) {
        if (const uint32_t h = hero_handle()) warp_cmd(c, h); else fprintf(stderr, "[warp] no hero yet: %s\n", c.c_str());
        return;
    }
    if (!c.compare(0, 4, "inp=")) {   /* presses separated by '/' (a comma would split the command line) */
        std::string sc = c.substr(4); for (auto& ch : sc) if (ch == '/') ch = ',';
        tod_input_script_set(sc.c_str()); return; }
    if (c == "trace" || !c.compare(0, 6, "trace=")) {
        const long n = c.size() > 6 ? strtol(c.c_str() + 6, 0, 10) : 1;
        rsx_draw_engine_trace_after(n > 0 ? n : 1);
        return;
    }
    if (!c.compare(0, 5, "pick=")) {   /* pick=x:y in 1280x720 space: which draws write that pixel (like F8 + mouse) */
        int x = 0, y = 0;
        if (sscanf(c.c_str() + 5, "%d:%d", &x, &y) == 2) {
            fprintf(stderr, "[cheat] pick %d,%d\n", x, y);
            rsx_draw_engine_pick_after(x, y, 1);
            rsx_draw_engine_trace_after(1);
        }
        return;
    }
    if (!c.compare(0, 3, "fx=")) {
        if (s_pending.empty()) fprintf(stderr, "[cheat] fx %s frame=%u t=%.1f\n", c.c_str() + 3, g_rsx_engine_frame, t);
        else s_pending.push_back(c);
        return;
    }
    if (c == "heal" || c == "weapons" || c == "bighead" || c == "enemybighead" || c == "mirror" || c == "allcheats" ||
        !c.compare(0, 6, "cheat=") || !c.compare(0, 6, "bolts=") || !c.compare(0, 6, "equip=") || !c.compare(0, 8, "dsprobe=")) { s_pending.push_back(c); return; }
    fprintf(stderr, "[cheat] unknown command '%s' (god nogod heal bolts=N weapons equip=N bighead enemybighead mirror cheat=N[:S] allcheats fx=tag trace ammo noammo status)\n", c.c_str());
}

void commands(const std::string& s, double t)
{
    std::string cur;
    for (char ch : s) {
        if (ch == ',' || ch == ';' || ch == '\n' || ch == ' ' || ch == '\t') { command(cur, t); cur.clear(); }
        else cur += ch;
    }
    command(cur, t);
}

void read_file(double t)
{
    static double next = 0;
    if (s_file.empty() || t < next) return;
    next = t + 0.5;
    FILE* f = fopen(s_file.c_str(), "rb");
    if (!f) return;
    std::string s; char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    if (s.empty()) return;
    if (FILE* w = fopen(s_file.c_str(), "wb")) fclose(w);   /* empty it: each command runs once */
    commands(s, t);
}

/* Commands queued by tod_cheat_enqueue(): same path as TOD_CHEAT_FILE (parsed and run on the poll). */
void read_inbox(double t)
{
    if (!s_in_has.exchange(false)) return;
    std::string s;
    { std::lock_guard<std::mutex> g(s_in_mx); s.swap(s_in); }
    if (!s.empty()) commands(s, t);
}

void keys(int focused, double t)
{
#ifdef _WIN32
    static const char* const menu[] = { "god", "nogod", "heal", "bolts=10000", "weapons", "ammo", "noammo", "status" };
    static int sel = -1, was9 = 0, was10 = 0;
    const int d9 = focused && (GetAsyncKeyState(VK_F9) & 0x8000), d10 = focused && (GetAsyncKeyState(VK_F10) & 0x8000);
    if (d9 && !was9) {
        sel = (sel + 1) % (int)(sizeof menu / sizeof *menu);
        fprintf(stderr, "[cheat] menu:");
        for (int i = 0; i < (int)(sizeof menu / sizeof *menu); i++) fprintf(stderr, i == sel ? " [%s]" : " %s", menu[i]);
        fprintf(stderr, "   (F9 next, F10 run)\n");
    }
    if (d10 && !was10 && sel >= 0) command(menu[sel], t);
    was9 = d9; was10 = d10;
#else
    (void)focused; (void)t;
#endif
}

}  // namespace

/* Shared with tod_bot.cpp: the hero handle validated the way func_00276D18 does (0 = none). */
extern "C" uint32_t tod_hero_handle(void) { return hero_handle(); }
/* The hero's game object ([entity+0x40]), 0 without a valid hero (tod_dualsense.cpp reads the equipped weapon from it). */
extern "C" uint32_t tod_hero_object(void)
{
    const uint32_t ent = entity_of(hero_handle());
    const uint32_t obj = ent ? vm_read32(ent + 0x40) : 0;
    return gp(obj) ? obj : 0;
}

/* logv2: the hero position from the get_pos record ([[0x898F40]] + (handle&0xFFFF)*0x90 + 0x30, x y z), the same record the
 * autopilot drives with (tod_bot.cpp). 1 = ok, 0 = no hero / record not mapped. */
/* logv2 position. uni127: was hero_handle()'s record, which stays at the spawn point (the log showed 98.0, 6.6, -150.7 all
 * run long); now the live Ratchet entity the autopilot uses (player_ent: object vtable 0x0084A3F8, position at ent+0x30).
 * The entity index found last is checked first, so the full table scan runs only when Ratchet's entity changes. */
extern "C" int tod_hero_pos(float* xyz)
{
    static uint32_t s_last = 0xFFFFFFFFu;
    const uint32_t T = vm_read32(kEntTab_slot);
    if (!gp(T)) return 0;
    const uint32_t base = vm_read32(T);
    if (!gp(base)) return 0;
    uint32_t ent = 0;
    if (s_last < 0x4000) {
        const uint32_t e = base + s_last * 384u, h = vm_read32(e + 0x64), obj = vm_read32(e + 0x40);
        if (h && (h & 0xFFFF) == s_last && ptr_ok(obj) && vm_read32(obj) == 0x0084A3F8u) ent = e;
    }
    if (!ent) { uint32_t h = 0; ent = player_ent(&h); if (!ent) return 0; s_last = h & 0xFFFF; }
    xyz[0] = rf(ent + 0x30); xyz[1] = rf(ent + 0x34); xyz[2] = rf(ent + 0x38);
    return xyz[0] == xyz[0] && xyz[1] == xyz[1] && xyz[2] == xyz[2];   /* not NaN */
}

extern "C" void tod_cheat_init(void)
{
    const char* e = getenv("TOD_CHEAT");
    const char* f = getenv("TOD_CHEAT_FILE");
    s_keys = getenv("TOD_CHEAT_KEYS") != nullptr;
    if (const char* w = getenv("TOD_WARP_FLAG")) s_warp_flag = atoi(w);
    /* TOD_WARP=x,y,z@seconds (once, game seconds from the first pad poll, >= 2 s after the hero exists);
     * TOD_WARP_MOBY=<entity index>[:dist]@seconds = next to that entity (index: "ents" dump), default 2 units on +X. */
    if (const char* w = getenv("TOD_WARP")) {
        float v[3] = { 0, 0, 0 }; double s = 0;
        if (sscanf(w, "%f,%f,%f@%lf", &v[0], &v[1], &v[2], &s) >= 3) {
            char b[96]; snprintf(b, sizeof b, "warp=%.4f:%.4f:%.4f", v[0], v[1], v[2]); s_timed.push_back({ s, b });
        } else fprintf(stderr, "[warp] bad TOD_WARP '%s' (x,y,z@seconds)\n", w);
    }
    if (const char* w = getenv("TOD_WARP_MOBY")) {
        unsigned idx = 0; double s = 0; float dist = 2.0f;
        const char* at = strchr(w, '@');
        if (at) s = atof(at + 1);
        if (sscanf(w, "%u:%f", &idx, &dist) >= 1) { char b[64]; snprintf(b, sizeof b, "warpe=%u:%.3f", idx, dist); s_timed.push_back({ s, b }); }
    }
    /* TOD_CINE=<name>@seconds: cine=<name> once (e.g. l02_s03_wake_rt@30), see cine_run */
    if (const char* w = getenv("TOD_CINE")) {   /* several: name@sec,name@sec */
        std::string all = w; size_t pos = 0;
        while (pos <= all.size()) {
            size_t e2 = all.find(',', pos); if (e2 == std::string::npos) e2 = all.size();
            const std::string one = all.substr(pos, e2 - pos); pos = e2 + 1;
            if (one.empty()) continue;
            const size_t at = one.find('@');
            s_timed.push_back({ at != std::string::npos ? atof(one.c_str() + at + 1) : 20.0, "cine=" + one.substr(0, at) });
        }
    }
    /* TOD_LUA_EXEC="chunk@seconds|chunk@seconds" (Lua source, '|' separates chunks; the LAST '@' of each chunk starts the delay; no '@' = 20 s) */
    if (const char* w = getenv("TOD_LUA_EXEC")) {
        std::string all = w; size_t pos = 0;
        while (pos <= all.size()) {
            size_t e2 = all.find('|', pos); if (e2 == std::string::npos) e2 = all.size();
            std::string one = all.substr(pos, e2 - pos); pos = e2 + 1;
            if (one.empty()) continue;
            const size_t at = one.rfind('@');
            double sec = 20.0;
            if (at != std::string::npos) { sec = atof(one.c_str() + at + 1); one = one.substr(0, at); }
            s_lua_timed.push_back({ sec, one });
        }
    }
    if (const char* w = getenv("TOD_LUA_FILE")) s_lua_file = w;
    if (const char* p = getenv("TOD_CHEAT_TID")) s_tid = atoi(p);   /* also needed when armed later by tod_cheat_enqueue */
    if (!e && !f && !s_keys && s_timed.empty() && s_lua_timed.empty() && s_lua_file.empty()) return;
    s_on = true;
    if (f) s_file = f;
    fprintf(stderr, "[cheat] on: start='%s' file='%s' keys=%d guest tid=%d\n", e ? e : "", f ? f : "", (int)s_keys, s_tid);
    if (e) s_start = e;
}

/* Thread-safe: appends commands (same syntax and separators as TOD_CHEAT_FILE) to the queue the poller drains, and
 * arms the poller if no env var did. They run on the guest main thread at its next poll, once the hero has been valid
 * for 2 s, exactly like the file path. Used by the F1 settings page (src/ui/tod_overlay.cpp). */
extern "C" void tod_cheat_enqueue(const char* cmds)
{
    if (!cmds || !*cmds) return;
    { std::lock_guard<std::mutex> g(s_in_mx); s_in += cmds; s_in += '\n'; }
    s_in_has.store(true);
    if (!s_on.exchange(true)) fprintf(stderr, "[cheat] on: armed at run time (settings page), guest tid=%d\n", s_tid);
}

/* usr123: queues one Lua chunk (e.g. hero_warp(Ratchet1,Name)) for the guest thread; arms the poller. Thread-safe. */
extern "C" void tod_lua_enqueue(const char* chunk)
{
    if (!chunk || !*chunk) return;
    { std::lock_guard<std::mutex> g(s_luaq_mx); s_luaq.push_back(chunk); }
    if (!s_on.exchange(true)) fprintf(stderr, "[cheat] on: armed at run time (teleport menu), guest tid=%d\n", s_tid);
}

/* vram124: IN-PROCESS level change that keeps the inventory: the game's own Lua set_level(LEVEL_X) (what the levels' scripts call when the
 * ship leaves a planet), issued on the guest thread; once the loader names the target level and the hero has been ready 8 s, the
 * optional hero_warp runs. If the level does not change within 25 s the state goes to 3 (the overlay says so; the user can use the
 * relaunch button). State: 0 idle, 1 waiting for the call, 2 waiting for the level, 3 timed out, 4 done. */
extern "C" int (*g_rsx_level_hook)(char* out, size_t n);
namespace { struct LevelGo { int st = 0; std::string lua, folder, spot; double t0 = 0, t_in = 0; } s_go; std::mutex s_go_mx; }
extern "C" void tod_level_go(const char* level_const, const char* folder, const char* spot)
{
    std::lock_guard<std::mutex> g(s_go_mx);
    s_go.lua = std::string("set_level(") + level_const + ")"; s_go.folder = folder; s_go.spot = spot ? spot : "";
    s_go.t0 = 0; s_go.t_in = 0; s_go.st = 1;
    if (!s_on.exchange(true)) fprintf(stderr, "[cheat] on: armed at run time (level go), guest tid=%d\n", s_tid);
}
extern "C" int tod_level_go_state(void) { return s_go.st; }
/* AUTOPILOT (tod_autopilot.cpp): Ratchet's live position (player_ent, ent+0x30); 0 = no Ratchet entity yet. Pad thread. */
extern "C" int tod_cheat_player_pos(float* xyz)
{
    if (!s_on.load(std::memory_order_relaxed)) return 0;
    const uint32_t pe = player_ent(nullptr);
    if (!pe) return 0;
    xyz[0] = rf(pe + 0x30); xyz[1] = rf(pe + 0x34); xyz[2] = rf(pe + 0x38);
    return 1;
}

/* AUTOPILOT (auto119): the live entities for enemy detection: handle, vtable, position. Returns the count (<= max).
 * Pad thread; reads only guest memory. The hero entity (vtable 0x0084A3F8) is skipped. */
struct TodApEnt { uint32_t h, vt; float x, y, z; };
extern "C" int tod_cheat_ents(TodApEnt* out, int max)
{
    if (!s_on.load(std::memory_order_relaxed)) return 0;
    const uint32_t T = vm_read32(kEntTab_slot);
    if (!gp(T)) return 0;
    const uint32_t base = vm_read32(T), cnt = vm_read32(T + 0x10);
    int n = 0;
    for (uint32_t i = 0; i <= cnt && i < 0x4000 && n < max; i++) {
        const uint32_t ent = base + i * 384u, h = vm_read32(ent + 0x64), obj = vm_read32(ent + 0x40);
        if (!h || (h & 0xFFFF) != i || !ptr_ok(obj)) continue;
        const uint32_t vt = vm_read32(obj);
        if (vt == 0x0084A3F8u) continue;
        out[n].h = h; out[n].vt = vt; out[n].x = rf(ent + 0x30); out[n].y = rf(ent + 0x34); out[n].z = rf(ent + 0x38); n++;
    }
    return n;
}
/* AUTOPILOT: Ratchet's matrix rows for the facing: o[0..3] = row0.x,row0.z,row2.x,row2.z, o[4] = row0.y, o[5] = row2.y. 1 = ok */
extern "C" int tod_cheat_player_mat(float* o)
{
    if (!s_on.load(std::memory_order_relaxed)) return 0;
    const uint32_t pe = player_ent(nullptr);
    if (!pe) return 0;
    o[0] = rf(pe + 0x00); o[1] = rf(pe + 0x08); o[2] = rf(pe + 0x20); o[3] = rf(pe + 0x28); o[4] = rf(pe + 0x04); o[5] = rf(pe + 0x24);
    return 1;
}

/* AUTOPILOT (autofix8): binary dump of the player's memory for offline RE of a "grounded / floor" field (TOD_AP_HERODUMP).
 * Record = "HREC" u32 tag, f64 t, u32 tag2(label), f32 pos[3], u32 nreg, then per region: u32 addr, u32 srcoff (offset in ent/obj that
 * held the pointer, 0xFFFFFFFF = root), u32 kind (0 ent, 1 obj, 2 ptr-from-ent, 3 ptr-from-obj), u32 len, len bytes (big-endian as in guest memory,
 * written as numeric u32 words in host order (little-endian file; read with struct "<I").
 * Regions: ent 0x180, obj 0x400, <=40 pointers found in ent and <=64 in obj, 0x80 B each. 1 = ok. Pad thread, reads guest memory only. */
extern "C" int tod_cheat_hero_dump(void* fv, double t, uint32_t label)
{
    FILE* f = (FILE*)fv;
    if (!f || !s_on.load(std::memory_order_relaxed)) return 0;
    const uint32_t pe = player_ent(nullptr);
    if (!pe) return 0;
    const uint32_t obj = vm_read32(pe + 0x40);
    struct R { uint32_t a, src, kind, len; };
    std::vector<R> rs;
    rs.push_back({ pe, 0xFFFFFFFFu, 0, 0x180 });
    if (ptr_ok(obj)) rs.push_back({ obj, 0x40, 1, 0x400 });
    int ne = 0, no = 0;
    for (uint32_t k = 0; k < 0x180 && ne < 40; k += 4) { const uint32_t v = vm_read32(pe + k); if (k != 0x40 && ptr_ok(v) && v != obj) { rs.push_back({ v, k, 2, 0x80 }); ne++; } }
    if (ptr_ok(obj)) for (uint32_t k = 0; k < 0x400 && no < 64; k += 4) { const uint32_t v = vm_read32(obj + k); if (ptr_ok(v)) { rs.push_back({ v, k, 3, 0x80 }); no++; } }
    const uint32_t tag = 0x43455248u, nreg = (uint32_t)rs.size();
    const float pos[3] = { rf(pe + 0x30), rf(pe + 0x34), rf(pe + 0x38) };
    fwrite(&tag, 4, 1, f); fwrite(&t, 8, 1, f); fwrite(&label, 4, 1, f); fwrite(pos, 4, 3, f); fwrite(&nreg, 4, 1, f);
    for (const R& r : rs) {
        fwrite(&r.a, 4, 1, f); fwrite(&r.src, 4, 1, f); fwrite(&r.kind, 4, 1, f); fwrite(&r.len, 4, 1, f);
        for (uint32_t i = 0; i < r.len; i += 4) { const uint32_t w = vm_read32(r.a + i); fwrite(&w, 4, 1, f); }
    }
    return 1;
}

/* AUTOPILOT: a Lua chunk to run on the guest thread at the next poll once the hero is ready (hero_warp, hero_equip...). */
static std::mutex s_ap_mx;
static std::vector<std::string> s_ap_lua;
static std::atomic<unsigned> s_ap_sent{0}, s_ap_done{0};
static std::atomic<bool> s_ap_ready{false};
extern "C" void tod_cheat_lua(const char* code)
{
    if (!code || !*code) return;
    std::lock_guard<std::mutex> g(s_ap_mx);
    s_ap_lua.push_back(code);
    s_ap_sent++;
}
/* 1 when every chunk queued by tod_cheat_lua has run (the queue only drains once the hero is ready) */
extern "C" int tod_cheat_lua_idle(void) { return s_ap_done.load() == s_ap_sent.load(); }
/* 1 once the hero has been valid for 2 s and the guest thread can run Lua (the level is loaded and running) */
extern "C" int tod_cheat_hero_ready(void) { return s_ap_ready.load() ? 1 : 0; }

/* Arms the poller without a command (the Cheats tab does it when first shown, so the entity-scan hero source works). */
extern "C" void tod_cheat_arm(void)
{
    if (!s_on.exchange(true)) fprintf(stderr, "[cheat] on: armed at run time (settings page), guest tid=%d\n", s_tid);
}

/* State for the settings page: 0 god, 1 infinite ammo, 100+N game cheat N (runtime state byte 0..7). -1 unknown.
 * Reads flags / guest memory only (any thread). */
extern "C" int tod_cheat_state(int id)
{
    if (id == 0) return s_god ? 1 : 0;
    if (id == 1) return s_ammo ? 1 : 0;
    if (id >= 100 && id <= 113) {
        const uint32_t B = vm_read32(kCheatBase_slot);
        return gp(B) ? (int)vm_read8(B + 0x86F8 + (uint32_t)(id - 100)) : -1;
    }
    return -1;
}

/* Called by host_input() on every pad poll (guest thread, inside cellPadGetData). */
extern "C" void tod_cheat_poll(int focused)
{
    if (!s_on.load(std::memory_order_relaxed)) return;
    bool exp = false;
    if (!s_busy.compare_exchange_strong(exp, true)) return;   /* two polling threads in the same tick */
    static uint64_t t0 = 0;
    if (!t0) t0 = GetTickCount64();
    const double t = (GetTickCount64() - t0) / 1000.0;

    { static bool told = false; static double lw = -9;
      const long long tid = g_active_ctx ? (long long)g_active_ctx->thread_id : -1;
      if (!told) { told = true; fprintf(stderr, "[cheat] first poll on guest tid %lld\n", tid); }
      if (!guest_ok() && t - lw > 10) { lw = t; fprintf(stderr, "[cheat] poll on tid %lld != TOD_CHEAT_TID %d: no guest calls from here\n", tid, s_tid); } }

    if (!s_start.empty()) { const std::string st = s_start; s_start.clear(); commands(st, t); }
    if (s_keys) keys(focused, t);
    read_file(t);
    read_inbox(t);

    if (!hero_handle()) scan_hero(t);
    const uint32_t h = hero_handle();
    static uint32_t lh = 0; static double hero_since = 0;
    if (h != lh) { fprintf(stderr, "[cheat] hero handle %08X -> %08X\n", lh, h); lh = h; hero_since = t; }
    /* A scan hit can come while the level still loads (found at frame ~430 in
     * the 22/10 sweep, and the weapons call 2 s later killed the process): give
     * it 10 s. The chain and the set_follow_cam hook only appear once loaded. */
    const bool ready = h && t - hero_since >= (h == s_scan_h ? 10.0 : 2.0);

    if (ready && getenv("TOD_POS_LOG")) {   /* hero position every 2 s: entity+0x30 and component block [ent+0x80]+0x40 */
        static double np = 0;
        if (t >= np) { np = t + 2.0;
            const uint32_t ent = entity_of(h), R = ent ? vm_read32(ent + 0x80) : 0;
            if (ent) fprintf(stderr, "[pos] t=%.1f ent=(%.2f,%.2f,%.2f) comp=(%.2f,%.2f,%.2f)\n", t, rf(ent + 0x30), rf(ent + 0x34), rf(ent + 0x38),
                             ptr_ok(R) ? rf(R + 0x40) : 0.f, ptr_ok(R) ? rf(R + 0x44) : 0.f, ptr_ok(R) ? rf(R + 0x48) : 0.f); 
            { const uint32_t pe = player_ent(nullptr);
              if (pe) fprintf(stderr, "[pos] t=%.1f ratchet=(%.2f,%.2f,%.2f)\n", t, rf(pe + 0x30), rf(pe + 0x34), rf(pe + 0x38)); }
        }
    }
    if (ready) for (size_t i = 0; i < s_timed.size();) {
        if (t >= s_timed[i].t) { const std::string c = s_timed[i].c; s_timed.erase(s_timed.begin() + i); command(c, t); }
        else i++;
    }
    if (ready && guest_ok()) for (size_t i = 0; i < s_lua_timed.size();) {
        if (t >= s_lua_timed[i].t) { const std::string c = s_lua_timed[i].c; s_lua_timed.erase(s_lua_timed.begin() + i); lua_exec(c); }
        else i++;
    }
    if (s_go.st && s_go.st < 3 && guest_ok()) {
        std::lock_guard<std::mutex> g(s_go_mx);
        if (s_go.st == 1 && ready) { lua_exec(s_go.lua); s_go.t0 = t; s_go.st = 2; tod_tp_log("[teleport] in-process: %s (target '%s' warp '%s')\n", s_go.lua.c_str(), s_go.folder.c_str(), s_go.spot.c_str()); }
        else if (s_go.st == 2) {
            char cur[64] = {0}; if (g_rsx_level_hook) g_rsx_level_hook(cur, sizeof cur);
            if (!_stricmp(cur, s_go.folder.c_str())) {
                if (!s_go.t_in) { s_go.t_in = t; tod_tp_log("[teleport] level '%s' is loading (t=%.1f)\n", cur, t); }
                if (t - s_go.t_in >= 8.0 && ready) {
                    if (!s_go.spot.empty()) lua_exec("hero_warp(Ratchet1," + s_go.spot + ")");
                    s_go.st = 4; tod_tp_log("[teleport] in-process done\n");
                }
            } else if (t - s_go.t0 > 25.0 && !s_go.t_in) { s_go.st = 3; tod_tp_log("[teleport] in-process: level did not change within 25 s ('%s')\n", cur); }
        }
    }
    if (ready && guest_ok()) {
        std::vector<std::string> q; { std::lock_guard<std::mutex> g(s_luaq_mx); q.swap(s_luaq); }
        for (const std::string& c : q) lua_exec(c);
    }
    s_ap_ready.store(ready && guest_ok());
    if (ready && guest_ok()) {   /* queued by the autopilot */
        std::vector<std::string> q;
        { std::lock_guard<std::mutex> g(s_ap_mx); q.swap(s_ap_lua); }
        for (const auto& c : q) { lua_exec(c); s_ap_done++; }
    }
    if (ready && guest_ok() && !s_lua_file.empty()) {
        static double nl = 0;
        if (t >= nl) { nl = t + 0.5;
            std::string src;
            if (FILE* lf = fopen(s_lua_file.c_str(), "rb")) { char b[512]; size_t n; while ((n = fread(b, 1, sizeof b, lf)) > 0) src.append(b, n); fclose(lf); }
            if (!src.empty()) { if (FILE* w = fopen(s_lua_file.c_str(), "wb")) fclose(w); lua_exec(src); } }
    }
    if (ready && s_god && guest_ok()) {
        if (const uint32_t hc = health_comp(h, t)) {
            const uint32_t fl = vm_read32(hc + 0x34);
            if (!(fl & 4)) { vm_write32(hc + 0x34, fl | 4); fprintf(stderr, "[cheat] god: HC=%08X flags %08X -> %08X\n", hc, fl, fl | 4); }
            const float cur = rf(hc + 4), mx = rf(hc + 8);
            if (isfinite(mx) && mx > 0 && cur < mx) wf(hc + 4, mx);
        }
    }
    if (ready && s_ammo) {
        static double na = 0;
        if (t >= na) { na = t + 1.0;
            const uint32_t ia = vm_read32(kInfAmmo_slot);
            if (gp(ia) && vm_read32(ia) != 1) { vm_write32(ia, 1); fprintf(stderr, "[cheat] infinite ammo flag [%08X] = 1\n", ia); } }
    }
    if (ready && guest_ok() && !s_pending.empty()) {
        std::vector<std::string> keep;
        for (const auto& c : s_pending) if (!run_oneshot(c, h, t)) keep.push_back(c);
        s_pending.swap(keep);
    }
    s_busy = false;
}
