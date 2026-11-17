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
#include <array>
#include <algorithm>
#include <map>
#include <set>

#include "ppu_recomp.h"        /* ppu_context, vm_*, func_XXXXXXXX declarations */
#include "platform/tod_platform.h"   /* win32_compat.h: GetTickCount64, GetAsyncKeyState; _stricmp off Windows */

extern "C" unsigned g_tod_hero_handle;   /* src/main.cpp tod_fntrace: from set_follow_cam */
extern "C" uint32_t g_rsx_engine_frame;                  /* rsx_draw_engine.c: present counter */
extern "C" void rsx_draw_engine_trace_after(long after);  /* rsx_draw_engine.c: arm the 4-frame draw trace */
extern "C" void rsx_draw_engine_pick_after(int x, int y, long after);
extern "C" void tod_input_script_set(const char* s);   /* host_input.cpp: "inp=" command */
extern "C" void tod_tp_log(const char* fmt, ...);      /* tod_logv2_host.cpp: stderr + "teleport" event */
extern "C" void tod_ds_probe(int tag);                  /* tod_dualsense.cpp: dsprobe=<n> command */
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern "C" int64_t sys_memory_allocate(ppu_context* ctx);   /* 10/11: lua_exec_L's source buffer */
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);

namespace {

const uint32_t kTocLua      = 0x89FF20;
const uint32_t kP_slot      = 0x8885B4;
const uint32_t kMask_slot   = 0x8885D8;
const uint32_t kShift_slot  = 0x8885DC;
const uint32_t kEntTab_slot = kTocLua - 0x6FE4;   /* 0x898F3C, func_00276D18 */
extern "C" uint32_t g_tod_moby_mgr, g_tod_spawn_toc, g_tod_spawn_toc2, g_tod_spawn_arg4[0x400], g_tod_inst_rec[0x400], g_tod_inst_mgr, g_tod_inst_toc;   /* combat_log.cpp spawn hooks */
uint32_t s_tod_spawn_posoff = 0x18;   /* spawnpos=OFF: where the record keeps the position (research) */
int s_tod_spawn_nowake = 0;   /* TOD_SPAWN_NOWAKE=1: clones stay asleep (research) */
uint32_t s_xwatch[16][3], s_xwatch_n = 0;   /* foreign spawns (entity, handle, slot) for the [xwatch] log */
double s_xwatch_t = 0;                        /* time of the last spawn= clone (xlua waits 3 s after it) */
std::vector<std::string> s_xlua_wait;         /* xlua= commands waiting for a clone */
uint32_t s_tod_spawn_srcent = 0;
extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" unsigned char g_ppu_vm_page[65536];
uint32_t g_tod_import_main = 0, g_tod_import_vert = 0, g_tod_import_cls = 0, g_tod_import_ncls = 0;   /* import= (research) */
uint32_t g_tod_import_vsize = 0, g_tod_import_gp = 0, g_tod_import_vio = 0xFFFFFFFFu;   /* vertices.dat size, gameplay.dat, IO offset of vertices */
/* foreign classes (xclass): which slots are imported, the code ranges of their update objects and the source level's ART ids
 * translated to host slots; read by combat_log.cpp func_0068EC48 (spawn by ART id from foreign code uses the source ids) */
extern "C" { uint8_t g_tod_xslot[0x400]; uint32_t g_tod_xrange[32][2]; uint32_t g_tod_xrange_n = 0; uint16_t g_tod_xart[9][0x400]; int g_tod_xart_init = 0;
           uint32_t g_tod_xrange_src[32]; uint32_t g_tod_import_n = 0; }   /* 14/11: per import (source level) 1..8 */
std::string g_tod_import_prefix;
std::map<uint32_t, uint32_t> g_tod_xtex_orig;   /* xclass: texture descriptor -> its original textures.dat offset (already moved) */
std::set<uint32_t> g_tod_xsub_done;              /* xclass: submeshes already remapped (classes can share them) */
extern "C" int32_t cellGcmMapMainMemory(uint32_t ea, uint32_t size, uint32_t* offset);
extern "C" uint32_t g_tod_last_spawn, g_tod_inst_list[8192][3], g_tod_inst_n, g_tod_spawn_cloning;
extern "C" uint64_t tod_gcall_toc(void (*fn)(ppu_context*), uint32_t toc, uint64_t a0, uint64_t a1);
extern "C" void tod_lua_enqueue(const char* chunk);
const uint32_t kRec90_slot  = kTocLua - 0x6FE0;   /* 0x898F40, get_pos records */
const uint32_t kG_slot      = 0x89F1B8;            /* G: +0x41C bolts, +0x420 raritanium */
const uint32_t kInfAmmo_slot = kTocLua - 0x6B80;  /* 0x8993A0, func_00288DE8 */

std::atomic<bool> s_on{false};   /* armed by an env var, or at run time by tod_cheat_enqueue / tod_cheat_arm (settings page) */
bool     s_keys, s_god, s_ammo, s_refill;
int      s_tid = 1;
std::string s_file, s_start;   /* TOD_CHEAT: run at the first poll (guest RAM loaded by then) */
std::vector<std::string> s_pending;   /* one-shot commands waiting for the hero */
std::atomic<bool> s_busy{false};
std::mutex s_in_mx;                 /* inbox of tod_cheat_enqueue (any thread) */
std::string s_in;
std::atomic<bool> s_in_has{false};

bool gp(uint32_t a) { return a >= 0x10000 && a < 0xF0000000u; }   /* plausible guest pointer */
extern "C" void tod_rwatch_arm(uint32_t ea);   /* tod_rwatch.cpp */
extern "C" void tod_rwatch_dump(void);
extern "C" void tod_rwatch_arm2(uint32_t ea, int exact);
extern "C" void tod_wwatch_arm(uint32_t ea);   /* tod_rwatch.cpp: writes only, with values and callers */
extern "C" uint32_t g_fx_disp_mgr, g_fx_foreign_root, g_fx_foreign_ncls, g_fx_nroots;   /* combat_log.cpp func_005F5C80 */
extern "C" uint32_t g_fx_host_roots[8], g_fx_foreign_roots[8];
extern "C" uint32_t g_fx_foreign_T, g_fx_Tslot;
extern "C" uint32_t g_fx_foreign_cls[16];
extern "C" uint32_t g_fx_src_T[9], g_fx_src_roots[9][8], g_fx_src_nroots[9], g_fx_src_cls[9][16], g_fx_src_ncls[9];
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
        fprintf(f, "e%u h=%08X ent=%08X obj=%08X cls=%08X pos=%.2f,%.2f,%.2f vt=%08X", i, h, ent, obj, vm_read32(ent + 0x54), rf(ent + 0x30), rf(ent + 0x34), rf(ent + 0x38),
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
    fprintf(stderr, "[lua] pcall returned %u, status slot tt=%u val=%u", r, vm_read32(nb), vm_read32(nb + 4));
    if (vm_read32(nb) == 1 && vm_read32(nb + 4) == 0 && vm_read32(nb + 8) == 4) {   /* 14/11: false + error string: print it */
        const uint32_t ts = vm_read32(nb + 12);
        for (uint32_t o = 8; o <= 32 && gp(ts); o += 4) { const std::string m = str_at(ts + o); if (m.size() > 3) { fprintf(stderr, " err='%s'", m.c_str()); break; } }
    }
    fprintf(stderr, "%c", 10);
    vm_write32(L + 8, otop); vm_write32(L + 0xC, obase);
    (void)ntop;
    return true;
}

/* quit125: the same loadstring + pcall on ANOTHER Lua state (an Anark UI scene's, from inside one of its native calls, on the
 * guest thread that runs it). L->top / L->base are put back afterwards, so the calling native's frame is untouched.
 * Returns 1 when the chunk ran (pcall true), 0 otherwise. */
int s_exec_quiet = 0;   /* alert10: tod_lua_exec_L_quiet */
int lua_exec_L(uint32_t L, const char* src, uint32_t arg_tv)
{
    if (!gp(L) || !g_active_ctx || !src) return 0;
    const size_t n = strlen(src);
    if (n > 0xA00) { fprintf(stderr, "[lua] exec refused: chunk of %zu bytes (max 2560)%c", n, 10); return 0; }   /* 10/11: was 1536, silent */
    const uint32_t otop = vm_read32(L + 8), obase = vm_read32(L + 0xC);
    const uint32_t ostack = vm_read32(L + 0x1C);   /* L->stack (alert10) */
    const uint32_t oci = vm_read32(L + 0x14), obase_ci = vm_read32(L + 0x28);
    const uint32_t occ = vm_read32(L + 0x2C);   /* alert10: size_ci (u16) + nCcalls (u16) of a Lua 5.0 state: each exec left nCcalls
                                                   one higher ("C stack overflow" after ~200 calls), so it is put back below */
    /* 10/11: the source goes into a guest buffer of its own (64 KB, sys_memory_allocate once, as push_string in
     * tod_settings_menu.cpp). It used to sit 0x900 below the stack pointer, where loadstring's own frames (about 0x1080
     * deep) wrote over the start of a 2257-byte chunk (c4_err: 331 bytes differed -> "invalid control char"). */
    static uint32_t s_srcbuf;
    if (!s_srcbuf) {
        ppu_context fake;
        memset(&fake, 0, sizeof fake);
        const uint32_t out = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
        vm_write32(out, 0);
        fake.gpr[3] = 0x10000; fake.gpr[4] = 0x200; fake.gpr[5] = out;
        sys_memory_allocate(&fake);
        s_srcbuf = vm_read32(out);
        fprintf(stderr, "[lua] source buffer at guest 0x%08X%c", s_srcbuf, 10);
    }
    const uint32_t buf = s_srcbuf ? s_srcbuf : (((uint32_t)g_active_ctx->gpr[1] - 0x900u - (uint32_t)n - 16u) & ~0xFu);
    for (size_t i = 0; i <= n; i++) vm_write8(buf + (uint32_t)i, i < n ? (uint8_t)src[i] : 0);
    gcall(func_002506E8, L, buf);                 /* lua_pushstring(L, buf) */
    vm_write32(L + 0xC, otop);
    const uint32_t nres = (uint32_t)gcall_toc(func_0066E378, kTocLib, L);   /* loadstring */
    const uint32_t t1 = vm_read32(L + 8);
    const uint32_t tt = t1 >= otop + 16 ? vm_read32(t1 - 8) : 99u;
    int ok = 0;
    if (nres == 1 && tt == 6) {
        vm_write32(L + 0xC, t1 - 8);
        gcall_toc(func_0066E7A8, kTocLib, L);     /* pcall(f) */
        const uint32_t nb = vm_read32(L + 0xC);
        ok = (vm_read32(nb) == 1 && vm_read32(nb + 4) == 1) ? 1 : 0;
        /* arg_tv: the chunk returned a function (pcall left: true, f); call f(value at arg_tv) the same way (the main chunk of
         * this Lua gets no `arg`) */
        if (gp(arg_tv) && getenv("TOD_QUIT_DUMP"))
            fprintf(stderr, "[lua] after pcall: base %08X top %08X (+%d) tt %u %u %u%c", nb, vm_read32(L + 8), (int)(vm_read32(L + 8) - nb),
                    vm_read32(nb), vm_read32(nb + 8), vm_read32(nb + 16), 10);
        const uint32_t fslot = vm_read32(L + 8) - 8;   /* the returned function is the last value (measured: true, the source, f) */
        if (ok && gp(arg_tv) && fslot > nb && vm_read32(fslot) == 6) {
            vm_write32(fslot + 8, vm_read32(arg_tv)); vm_write32(fslot + 12, vm_read32(arg_tv + 4));
            vm_write32(L + 8, fslot + 16);
            vm_write32(L + 0xC, fslot);
            gcall_toc(func_0066E7A8, kTocLib, L);     /* pcall(f, value) */
            const uint32_t nb2 = vm_read32(L + 0xC);
            ok = (vm_read32(nb2) == 1 && vm_read32(nb2 + 4) == 1) ? 2 : 0;
        }
        if (!ok) {   /* alert10: the error message pcall left after false (a Lua 5.0 TString: len at +12, chars at +16) */
            const uint32_t eb = vm_read32(L + 0xC), et = vm_read32(L + 8);
            uint32_t es = 0;
            for (uint32_t q = eb; q < et && q < eb + 64; q += 8) if (vm_read32(q) == 4u) es = q;   /* the last string = the message */
            static int s_nerr;
            if (es && s_nerr++ < 20) {
                const uint32_t ts = vm_read32(es + 4), len = vm_read32(ts + 12);
                char msg[200]; uint32_t k = 0;
                for (; k < len && k < sizeof msg - 1; k++) msg[k] = (char)vm_read8(ts + 16 + k);
                msg[k] = 0;
                fprintf(stderr, "[lua] error: %s%c", msg, 10);
            }
        }
    } else if (t1 >= otop + 16 && vm_read32(t1 - 8) == 4u) {   /* 10/11: loadstring's own message (nil, message) */
        const uint32_t ts = vm_read32(t1 - 4), len = vm_read32(ts + 12);
        char msg[200]; uint32_t k = 0;
        for (; k < len && k < sizeof msg - 1; k++) msg[k] = (char)vm_read8(ts + 16 + k);
        msg[k] = 0;
        fprintf(stderr, "[lua] loadstring error: %s%c", msg, 10);
        uint32_t bad = 0, first = 0xFFFFFFFFu;   /* is the guest copy still the source? */
        for (size_t i = 0; i < n; i++) if (vm_read8(buf + (uint32_t)i) != (uint8_t)src[i]) { bad++; if (first == 0xFFFFFFFFu) first = (uint32_t)i; }
        fprintf(stderr, "[lua] source %zu bytes at %08X (sp %08X): %u bytes differ, first at %d%c", n, buf, (uint32_t)g_active_ctx->gpr[1], bad, (int)first, 10);
    }
    if (!ok || !s_exec_quiet) fprintf(stderr, "[lua] exec on L=%08X: loadstring %s, pcall %s%c", L, (nres == 1 && tt == 6) ? "ok" : "FAILED", ok ? "ok" : "failed", 10);
    { const uint32_t nst = vm_read32(L + 0x1C);   /* alert10: the stack may have been reallocated by the call: put top / base back by offset */
      vm_write32(L + 8, nst + (otop - ostack)); vm_write32(L + 0xC, nst + (obase - ostack));
      if (nst != ostack) fprintf(stderr, "[lua] stack moved during exec: %08X -> %08X%c", ostack, nst, 10); }
    { static int s_cil = -1; static int s_cin; if (s_cil < 0) s_cil = getenv("TOD_LUA_CI_LOG") ? 1 : 0;
      if (s_cil && s_cin++ < 12) fprintf(stderr, "[lua] exec state: ci %08X -> %08X, nCcalls %u -> %u, size_ci %u%c", oci, vm_read32(L + 0x14),
                                         occ & 0xFFFFu, vm_read32(L + 0x2C) & 0xFFFFu, vm_read32(L + 0x2C) >> 16, 10); }
    /* nCcalls also ends one higher; putting it back hangs the menu too (mq9), so it stays (few execs: see above) */
    /* L->ci ends 2 CallInfos higher after an exec (TOD_LUA_CI_LOG=1); putting it back hangs the menu at once (09/11 mq6-mq8),
     * so it stays: callers keep the number of execs small (the main-menu hook runs only after a key, alert10). */
    (void)oci; (void)obase_ci;
    return ok;
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
    if (!c.compare(0, 7, "import=")) {   /* research (11/11): import=<host path prefix> -> load <prefix>main.dat + <prefix>vertices.dat of
                                             * another level into new guest memory, apply main.dat's IGHW relocations (as func_0051E450) */
        const std::string pre = c.substr(7);
        auto gal = [&](uint32_t size) -> uint32_t {
            ppu_context fake; memset(&fake, 0, sizeof fake);
            const uint32_t out = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
            vm_write32(out, 0);
            fake.gpr[3] = (size + 0xFFFFFu) & ~0xFFFFFu; fake.gpr[4] = 0x400; fake.gpr[5] = out;
            const int64_t rc = sys_memory_allocate(&fake);
            const uint32_t a = vm_read32(out);
            fprintf(stderr, "[import] allocate %u MB -> %08X (rc %lld)\n", (unsigned)(fake.gpr[3] >> 20), a, (long long)rc);
            return a;
        };
        auto loadf = [&](const std::string& path, uint32_t& size) -> uint32_t {
            FILE* f = fopen(path.c_str(), "rb");
            if (!f) { fprintf(stderr, "[import] cannot open %s\n", path.c_str()); return 0; }
            fseek(f, 0, SEEK_END); size = (uint32_t)ftell(f); fseek(f, 0, SEEK_SET);
            std::vector<uint8_t> b(size); size_t got = fread(b.data(), 1, size, f); fclose(f);
            const uint32_t g = gal(size);
            if (!gp(g)) return 0;
            for (size_t i = 0; i < got; i++) vm_write8(g + (uint32_t)i, b[i]);
            fprintf(stderr, "[import] %s: %u bytes at %08X\n", path.c_str(), size, g);
            return g;
        };
        uint32_t msz = 0, vsz = 0;
        const uint32_t m = loadf(pre + "main.dat", msz);
        if (!m) return true;
        const uint32_t rtab = vm_read32(m + 0x10), nrel = vm_read32(m + 0x14);
        uint32_t done = 0;
        for (uint32_t k = 0; k < nrel; k++) {
            const uint32_t w = vm_read32(m + rtab + k * 4), off = w & 0x3FFFFFFFu, t = w >> 30;
            if (t == 0) { vm_write32(m + off, vm_read32(m + off) + m); done++; }
            else if (t == 1) { vm_write64(m + off, vm_read64(m + off) + m); done++; }
        }
        g_tod_import_main = m;
        g_tod_import_vio = 0xFFFFFFFFu;   /* 14/11: each imported level gets its own vertex mapping */
        if (g_tod_import_n < 8) g_tod_import_n++; else fprintf(stderr, "[import] more than 8 levels: effects/ids of the 8th are shared%c", 10);
        const uint32_t v = loadf(pre + "vertices.dat", vsz);
        g_tod_import_vert = v; g_tod_import_vsize = vsz; g_tod_import_prefix = pre;
        {   /* gameplay.dat (instance records + their config data), relocated the same way */
            uint32_t gsz = 0;
            const uint32_t g2 = loadf(pre + "gameplay.dat", gsz);
            if (g2) {
                const uint32_t rt2 = vm_read32(g2 + 0x10), nr2 = vm_read32(g2 + 0x14);
                for (uint32_t k = 0; k < nr2; k++) {
                    const uint32_t w = vm_read32(g2 + rt2 + k * 4), off = w & 0x3FFFFFFFu, t = w >> 30;
                    if (t == 0) vm_write32(g2 + off, vm_read32(g2 + off) + g2);
                    else if (t == 1) vm_write64(g2 + off, vm_read64(g2 + off) + g2);
                }
                g_tod_import_gp = g2;
                fprintf(stderr, "[import] gameplay %08X relocated %u\n", g2, nr2);
            }
        }
        const uint32_t nsec = vm_read32(m + 8);
        for (uint32_t s2 = 0; s2 < nsec; s2++) if (vm_read32(m + 0x20 + s2 * 16) == 0xD100) {
            g_tod_import_cls = m + vm_read32(m + 0x24 + s2 * 16); g_tod_import_ncls = vm_read32(m + 0x28 + s2 * 16) & 0x0FFFFFFFu; }
        fprintf(stderr, "[import] main %08X relocated %u/%u, vertices %08X, class table %08X x %u\n", m, done, nrel, v, g_tod_import_cls, g_tod_import_ncls);
        return true;
    }
    if (!c.compare(0, 7, "xclass=")) {   /* research (11/11 night): xclass=FC[:INST] -> after import=, make class FC of the imported level
                                             * a class of the current one (next free slot S), then spawn=S works. What the level load
                                             * (func_004A9D48) does for a class, redone for this one: indices + textures into free RSX
                                             * local memory, vertices through a new IO mapping, materials appended to the level table,
                                             * submesh material bytes, class slot, then the moby manager's class prep func_0068F570. */
        unsigned fc = 0, inst = 0;
        sscanf(c.c_str() + 7, "%u:%u", &fc, &inst);
        const uint32_t m = g_tod_import_main, v = g_tod_import_vert;
        if (!gp(m) || !gp(v) || fc >= g_tod_import_ncls) { fprintf(stderr, "[xclass] import= first (main %08X vert %08X, %u classes)\n", m, v, g_tod_import_ncls); return true; }
        auto gal = [&](uint32_t size) -> uint32_t {
            ppu_context fake; memset(&fake, 0, sizeof fake);
            const uint32_t out = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
            vm_write32(out, 0);
            fake.gpr[3] = (size + 0xFFFFFu) & ~0xFFFFFu; fake.gpr[4] = 0x400; fake.gpr[5] = out;
            sys_memory_allocate(&fake);
            return vm_read32(out);
        };
        /* 14/11: small pieces (material table, types, prep allocator, record) carved from shared 1 MB blocks instead of 1 MB
         * (4 MB) each: ~6 MB per class pushed the 3rd level of a rissa past 0x50000000 into the overflow window, where the SPU
         * geometry jobs stall and every mesh comes out corrupted. */
        static uint32_t s_pool = 0, s_left = 0;
        auto galp = [&](uint32_t size) -> uint32_t {
            size = (size + 0x7Fu) & ~0x7Fu;
            if (size >= 0x80000u) return gal(size);
            if (size > s_left) { s_pool = gal(0x100000); s_left = gp(s_pool) ? 0x100000u : 0u; if (!s_left) return 0; }
            const uint32_t a = s_pool; s_pool += size; s_left -= size;
            for (uint32_t o = 0; o < size; o += 4) vm_write32(a + o, 0);
            return a;
        };
        auto sec = [&](uint32_t base, uint32_t id, uint32_t* cnt) -> uint32_t {
            const uint32_t ns = vm_read32(base + 8);
            for (uint32_t s2 = 0; s2 < ns; s2++) if (vm_read32(base + 0x20 + s2 * 16) == id) {
                const uint32_t a = vm_read32(base + 0x28 + s2 * 16);
                if (cnt) *cnt = (a & 0x10000000u) ? (a & 0x0FFFFFFFu) : 1;
                return base + vm_read32(base + 0x24 + s2 * 16);
            }
            return 0;
        };
        auto gcopy = [&](uint32_t dst, uint32_t src, uint32_t n) { for (uint32_t o = 0; o < n; o += 4) vm_write32(dst + o, vm_read32(src + o)); };
        const uint32_t cls = g_tod_import_cls + fc * 0xC0;
        uint32_t n5000 = 0, n5200 = 0;
        const uint32_t t5000 = sec(m, 0x5000, &n5000), t5200 = sec(m, 0x5200, &n5200);
        /* the source level's textures.dat (indices + textures, in RSX local memory in the game) */
        std::vector<uint8_t> tex;
        { FILE* f = fopen((g_tod_import_prefix + "textures.dat").c_str(), "rb");
          if (!f) { fprintf(stderr, "[xclass] cannot open %stextures.dat\n", g_tod_import_prefix.c_str()); return true; }
          fseek(f, 0, SEEK_END); tex.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
          if (fread(tex.data(), 1, tex.size(), f) != tex.size()) tex.clear();
          fclose(f); }
        /* submeshes: class +0x28 -> (pointer, count) x (byte +0x19 * (byte +0x1E + 1)) groups, 0x40 each (section 0xDD00):
         * +0 first index, +0x10 index count, u16 +8 material index */
        std::vector<uint32_t> subs; std::vector<uint32_t> mats; uint32_t iend = 0;
        const uint32_t ng = vm_read8(cls + 0x19) * (vm_read8(cls + 0x1E) + 1u);
        for (uint32_t j = 0; j < ng; j++) {
            const uint32_t p = vm_read32(cls + 0x28) + j * 8, q = vm_read32(p), k = vm_read32(p + 4);
            for (uint32_t i = 0; i < k && i < 4096; i++) {
                const uint32_t e = q + i * 0x40, mi = vm_read16(e + 8);
                if (g_tod_xsub_done.count(e)) continue;   /* shared with a class moved before: already done */
                subs.push_back(e);
                bool have = false; for (uint32_t x : mats) if (x == mi) have = true;
                if (!have) mats.push_back(mi);
                const uint32_t ie = vm_read32(e) + vm_read32(e + 0x10); if (ie > iend) iend = ie;
            }
        }
        /* textures: the material pointer slots +0..+0xC -> 0x20-byte RSX texture descriptors (section 0x5200, +0 offset in textures.dat);
         * a texture's size = distance to the next texture offset */
        std::vector<uint32_t> toffs;
        for (uint32_t i = 0; i < n5200; i++) { const uint32_t e = t5200 + i * 0x20; auto it = g_tod_xtex_orig.find(e); toffs.push_back(it != g_tod_xtex_orig.end() ? it->second : vm_read32(e)); }
        std::sort(toffs.begin(), toffs.end());
        auto tsize = [&](uint32_t off) -> uint32_t {
            auto it = std::upper_bound(toffs.begin(), toffs.end(), off);
            return (it == toffs.end() ? (uint32_t)tex.size() : *it) - off; };
        std::vector<uint32_t> tents;
        for (uint32_t mi : mats) for (uint32_t f2 = 0; f2 < 0x10; f2 += 4) {
            const uint32_t p = vm_read32(t5000 + mi * 0x80 + f2);
            if (p >= t5200 && p < t5200 + n5200 * 0x20 && !((p - t5200) & 0x1F)) {
                bool have = g_tod_xtex_orig.count(p) != 0;   /* moved by an earlier xclass: keep */
                for (uint32_t x : tents) if (x == p) have = true;
                if (!have) tents.push_back(p);
            }
        }
        const uint32_t ioff = vm_read32(cls + 0x34), v38 = vm_read32(cls + 0x38);
        /* vertices inside textures.dat (bit 31 clear): they sit right before the indices */
        const uint32_t vlsize = (!(v38 & 0x80000000u) && v38 < ioff && ioff - v38 < 0x2000000u) ? ioff - v38 : 0;
        uint32_t need = ((iend * 2u) + 0xFFFu) & ~0xFFFu;
        need += (vlsize + 0xFFFu) & ~0xFFFu;
        for (uint32_t p : tents) need += (tsize(vm_read32(p)) + 0xFFFu) & ~0xFFFu;
        fprintf(stderr, "[xclass] class %u at %08X: %zu submeshes, %zu materials, %zu textures, indices %X x2 at %08X, %u KB to RSX local memory\n",
                fc, cls, subs.size(), mats.size(), tents.size(), iend, ioff, need >> 10);
        if (tex.empty() || ioff + iend * 2u > tex.size()) { fprintf(stderr, "[xclass] textures.dat missing or too short\n"); return true; }
        /* free RSX local memory: the all-zero runs of 64 KB pages (from 64 MB up); the highest one that fits, 1 MB kept free each side */
        uint32_t top = 0, lo = 0;
        { uint32_t rs = 0;
          for (uint32_t pg = 0xC4000000u; pg <= 0xD0000000u; pg += 0x10000u) {
              bool z = pg < 0xD0000000u;
              for (uint32_t a = pg; a < pg + 0x10000u && z; a += 4) z = vm_read32(a) == 0;
              if (z) { if (!rs) rs = pg; continue; }
              if (rs && pg - rs >= 0x400000u) fprintf(stderr, "[xclass] zero RSX local run %08X..%08X (%u KB)\n", rs, pg, (pg - rs) >> 10);
              if (rs && pg - rs >= need + 0x200000u) { lo = rs; top = pg; }
              rs = 0;
          } }
        if (!top) { fprintf(stderr, "[xclass] no zero RSX local run of %u KB\n", (need + 0x200000u) >> 10); return true; }
        /* TOD_XCLASS_PACK=1: the game fills this run from the bottom up during a fight (wave brawl: up to ~CD53xxxx);
           the 1 MB margin taken under every import pushed the 8th one down there. Keep the margin only at the top of the run. */
        static uint32_t s_xlow = 0;
        if (!(getenv("TOD_XCLASS_PACK") && top == s_xlow)) top -= 0x100000u;
        uint32_t dst = (top - need) & ~0xFFFFu;
        s_xlow = dst;
        fprintf(stderr, "[xclass] RSX local %08X..%08X (zero run from %08X)\n", dst, top, lo);
        auto put = [&](uint32_t off, uint32_t n) -> uint32_t {
            const uint32_t at = dst;
            for (uint32_t i = 0; i < n; i++) vm_write8(at + i, tex[off + i]);
            dst += (n + 0xFFFu) & ~0xFFFu;
            return at - 0xC0000000u;
        };
        vm_write32(cls + 0x34, put(ioff, iend * 2u));
        { std::vector<std::pair<uint32_t, uint32_t>> done;   /* textures.dat offset -> new local offset (shared data copied once) */
          for (uint32_t p : tents) {
              const uint32_t off = vm_read32(p); uint32_t nl = 0xFFFFFFFFu;
              for (auto& d2 : done) if (d2.first == off) nl = d2.second;
              if (nl == 0xFFFFFFFFu) { nl = put(off, tsize(off)); done.push_back({off, nl}); }
              g_tod_xtex_orig[p] = off;
              vm_write32(p, nl);
              vm_write32(p + 4, (vm_read32(p + 4) & ~3u) | 1u);   /* location: local */
          } }
        /* vertices: bit 31 of class +0x38 = in main memory; the load makes it 0x80000000|IO(section 0x9000 of vertices.dat) + low bits */
        /* 14/11: only the vertices of the imported classes are mapped, not the whole vertices.dat: Rykan already uses IO up to
         * 0BC00000, three whole foreign files (80 MB) ran past the 256 MB IO space and the scene broke up when the class mapped
         * past 0x10000000 was drawn. A class's vertices run from its offset to the next class offset of its level. */
        static std::vector<uint32_t> s_voffs; static std::vector<std::array<uint32_t, 3>> s_vmaps;   /* (ea lo, ea hi, io) */
        const uint32_t v9000 = sec(v, 0x9000, nullptr) - v;
        if (g_tod_import_vio == 0xFFFFFFFFu) {
            s_voffs.clear(); s_vmaps.clear();
            for (uint32_t i = 0; i < g_tod_import_ncls; i++) {
                const uint32_t w = vm_read32(g_tod_import_cls + i * 0xC0u + 0x38);
                if (w & 0x80000000u) s_voffs.push_back(w & 0x7FFFFFFFu);
            }
            std::sort(s_voffs.begin(), s_voffs.end());
            s_voffs.erase(std::unique(s_voffs.begin(), s_voffs.end()), s_voffs.end());
            g_tod_import_vio = 0;
        }
        { const uint32_t w = vm_read32(cls + 0x38);
          if (w & 0x80000000u) {
            const uint32_t o = w & 0x7FFFFFFFu;
            uint32_t e = g_tod_import_vsize > v9000 ? g_tod_import_vsize - v9000 : o + 0x100000u;
            for (uint32_t x : s_voffs) if (x > o) { e = x; break; }
            const uint32_t a = v + v9000 + o, lo = a & ~0xFFFFFu, hi = (v + v9000 + e + 0xFFFFFu) & ~0xFFFFFu;
            uint32_t io = 0xFFFFFFFFu;
            for (auto& mp : s_vmaps) if (lo >= mp[0] && hi <= mp[1]) { io = mp[2] + (lo - mp[0]); break; }
            if (io == 0xFFFFFFFFu) {
                const uint32_t out = ((uint32_t)g_active_ctx->gpr[1] - 0x900u) & ~0xFu;
                vm_write32(out, 0xFFFFFFFFu);
                const int32_t rc = cellGcmMapMainMemory(lo, hi - lo, (uint32_t*)(uintptr_t)out);
                io = vm_read32(out);
                s_vmaps.push_back({lo, hi, io});
                fprintf(stderr, "[xclass] vertices %08X..%08X (%u KB of the class) mapped at IO %08X (rc %d)%s\n", lo, hi, (e - o) >> 10, io, rc,
                        (uint64_t)io + (hi - lo) > 0x10000000ull ? " PAST THE 256 MB IO SPACE" : "");
            }
            vm_write32(cls + 0x38, 0x80000000u | (io + (a - lo)));
          }
          else if (vlsize) { vm_write32(cls + 0x38, put(w, vlsize)); fprintf(stderr, "[xclass] vertices in textures.dat: %X bytes copied\n", vlsize); }
          else fprintf(stderr, "[xclass] WARNING: class +0x38 %08X: vertices not found\n", w); }
        /* materials: a new level table = the current one + this class's, in both holders (P = [0x89FF20+0x6DB0]: +0 table, +4/+8 count;
         * a second (table, count) pair elsewhere in the same data block) */
        const uint32_t P = vm_read32(kTocLua + 0x6DB0), oldt = vm_read32(P), oldn = vm_read32(P + 4);
        uint32_t P2 = 0;
        for (uint32_t a = (P & ~0xFFFFu) - 0x40000u; a < (P & ~0xFFFFu) + 0x40000u && !P2; a += 4)
            if (a != P && vm_read32(a) == oldt && vm_read32(a + 4) == oldn) P2 = a;
        const uint32_t nt = galp((oldn + (uint32_t)mats.size()) * 0x80u);
        if (!gp(nt)) { fprintf(stderr, "[xclass] no memory for the material table\n"); return true; }
        gcopy(nt, oldt, oldn * 0x80u);
        for (size_t k = 0; k < mats.size(); k++) {
            const uint32_t d2 = nt + (oldn + (uint32_t)k) * 0x80u;
            gcopy(d2, t5000 + mats[k] * 0x80u, 0x80);
            if (!vm_read32(d2 + 4)) vm_write32(d2 + 4, 0x1064F6CCu);   /* the load's default textures for empty slots */
            if (!vm_read32(d2 + 8)) vm_write32(d2 + 8, 0x1064F68Cu);
            if (!vm_read32(d2 + 0xC)) vm_write32(d2 + 0xC, 0x1064F6ACu);
        }
        vm_write32(P, nt); vm_write32(P + 4, oldn + (uint32_t)mats.size()); vm_write32(P + 8, oldn + (uint32_t)mats.size());
        if (P2) { vm_write32(P2, nt); vm_write32(P2 + 4, oldn + (uint32_t)mats.size()); }
        fprintf(stderr, "[xclass] materials %08X x %u -> %08X x %u (holders %08X %08X)\n", oldt, oldn, nt, oldn + (uint32_t)mats.size(), P, P2);
        uint32_t run = 0;
        for (uint32_t e : subs) {
            const uint32_t mi = vm_read16(e + 8); uint32_t k = 0;
            while (k < mats.size() && mats[k] != mi) k++;
            const uint32_t ni = oldn + k, mt = nt + ni * 0x80u;
            vm_write16(e + 8, (uint16_t)ni);
            g_tod_xsub_done.insert(e);
            vm_write8(e + 0xE, (uint8_t)run++);
            vm_write8(e + 0x24, vm_read8(e + 0x24) & vm_read8(mt + 0x10));
            vm_write8(e + 0x25, vm_read8(mt + 0x11));
            vm_write8(e + 0x26, vm_read8(mt + 0x13));
        }
        vm_write8(cls + 0x18, (uint8_t)run);
        /* class slot: array [0x8AFE5C-0x5B10], count [[0x8AFE5C-0x5B0C]+0x10] (TOC of the moby class module) */
        const uint32_t kTocMoby = 0x8AFE5Cu, arr = vm_read32(kTocMoby - 0x5B10), cntp = vm_read32(kTocMoby - 0x5B0C) + 0x10, S = vm_read32(cntp);
        if (!gp(arr) || S >= 0x180 || vm_read32(arr + S * 4)) { fprintf(stderr, "[xclass] class array %08X count %u: slot not free\n", arr, S); return true; }
        vm_write32(arr + S * 4, cls); vm_write32(cntp, S + 1);
        /* ART class map (global class id -> level class index; what spawn-by-id func_00011B70 -> func_006511D0 reads):
         * M = [[0x8AFE5C-0x62F4]], +8 table of u32 (u16 ?, u16 level index; 0xFFFF = not in this level), +0xC count = the
         * level's gameplay.dat section 0x25500. Every id the source level maps to FC now maps to S (e.g. the Cragmite mother
         * 69 spawns its young by id 0x7C = Cragmite class 61). */
        { const uint32_t M = vm_read32(vm_read32(kTocMoby - 0x62F4)), mt = gp(M) ? vm_read32(M + 8) : 0, mn = gp(M) ? vm_read32(M + 0xC) : 0;
          uint32_t sn = 0; const uint32_t st = gp(g_tod_import_gp) ? sec(g_tod_import_gp, 0x25500, &sn) : 0;
          uint32_t ssz = 0;
          for (uint32_t s2 = 0, ns = gp(g_tod_import_gp) ? vm_read32(g_tod_import_gp + 8) : 0; s2 < ns; s2++)
              if (vm_read32(g_tod_import_gp + 0x20 + s2 * 16) == 0x25500) ssz = vm_read32(g_tod_import_gp + 0x2C + s2 * 16);
          if (!g_tod_xart_init) { for (int q = 0; q < 9; q++) for (int i = 0; i < 0x400; i++) g_tod_xart[q][i] = 0xFFFF; g_tod_xart_init = 1; }
          g_tod_xslot[S] = (uint8_t)(g_tod_import_n ? g_tod_import_n : 1);   /* the source level (import number) of slot S */
          if (gp(mt) && st) for (uint32_t i = 0; i < ssz / 4 && i < mn; i++) {
              if (vm_read16(st + i * 4 + 2) != fc) continue;
              if (i < 0x400) g_tod_xart[g_tod_xslot[S]][i] = (uint16_t)S;   /* the source level's id i -> slot S, for spawns made by foreign code */
              /* the host level's own map only where the host has no class for that id (id 0 and the like stay the host's) */
              if (vm_read16(mt + i * 4 + 2) != 0xFFFF) { fprintf(stderr, "[xclass] ART id %X: host has %04X, only foreign spawns use slot %u%c", i, vm_read16(mt + i * 4 + 2), S, 10); continue; }
              fprintf(stderr, "[xclass] ART id %X: level map %04X:%04X -> %04X:%04X%c", i, vm_read16(mt + i * 4), vm_read16(mt + i * 4 + 2),
                      vm_read16(st + i * 4), S, 10);
              vm_write16(mt + i * 4, vm_read16(st + i * 4)); vm_write16(mt + i * 4 + 2, (uint16_t)S);
          } }
        /* Update TYPES (12/11): an object made by type id (e.g. the Cragmite mother's shot, spawn by ART id 0 with type 0x513) gets
         * its default config from func_006510E8: M+0x1C/+0x20 = map type id -> index (gameplay section 0x25300, -1 = this level
         * has no such type), M+0x14 = config sizes (0x25380), M+0x10 = config block pointers (0x25340), M+0x18 = their count. A type
         * the host level lacks gives an empty base object (vtable 0x856998) that never moves: the shot stays where it was made.
         * Every type the source level has and the host lacks is appended, pointing at the imported gameplay (already relocated). */
        { const uint32_t M = vm_read32(vm_read32(kTocMoby - 0x62F4));
          const uint32_t hmap = gp(M) ? vm_read32(M + 0x1C) : 0, hmn = gp(M) ? vm_read32(M + 0x20) : 0;
          const uint32_t hsz = gp(M) ? vm_read32(M + 0x10) : 0, hptr = gp(M) ? vm_read32(M + 0x14) : 0, hn = gp(M) ? vm_read32(M + 0x18) : 0;
          uint32_t smapsz = 0;
          for (uint32_t s2 = 0, ns = gp(g_tod_import_gp) ? vm_read32(g_tod_import_gp + 8) : 0; s2 < ns; s2++)
              if (vm_read32(g_tod_import_gp + 0x20 + s2 * 16) == 0x25300) smapsz = vm_read32(g_tod_import_gp + 0x2C + s2 * 16);
          const uint32_t smap = gp(g_tod_import_gp) ? sec(g_tod_import_gp, 0x25300, 0) : 0;
          uint32_t spn = 0; const uint32_t sptr = gp(g_tod_import_gp) ? sec(g_tod_import_gp, 0x25340, &spn) : 0;
          const uint32_t ssz2 = gp(g_tod_import_gp) ? sec(g_tod_import_gp, 0x25380, 0) : 0;
          std::vector<uint32_t> add;
          if (gp(hmap) && gp(hsz) && gp(hptr) && smap && sptr && ssz2)
              for (uint32_t T = 0; T < hmn && T < smapsz / 4; T++) {
                  const int32_t hi = (int32_t)vm_read32(hmap + T * 4), si = (int32_t)vm_read32(smap + T * 4);
                  if (hi < 0 && si >= 0 && (uint32_t)si < spn) add.push_back(T);
              }
          if (!add.empty()) {
              const uint32_t nn = hn + (uint32_t)add.size(), nb = galp(((nn * 4 + 0xF) & ~0xFu) * 2);
              if (gp(nb) && nn * 8 < 0x100000) {
                  const uint32_t np = nb, ns2 = nb + ((nn * 4 + 0xF) & ~0xFu);
                  gcopy(np, hptr, hn * 4); gcopy(ns2, hsz, hn * 4);
                  for (size_t j = 0; j < add.size(); j++) {
                      const uint32_t T = add[j], si = vm_read32(smap + T * 4);
                      /* M+0x14 (np) = sizes, M+0x10 (ns2) = block pointers: what func_006510E8 returns in r4 / r5 */
                      vm_write32(np + (hn + (uint32_t)j) * 4, vm_read32(ssz2 + si * 4));
                      vm_write32(ns2 + (hn + (uint32_t)j) * 4, vm_read32(sptr + si * 4));
                      vm_write32(hmap + T * 4, hn + (uint32_t)j);
                  }
                  vm_write32(M + 0x14, np); vm_write32(M + 0x10, ns2); vm_write32(M + 0x18, nn);
                  fprintf(stderr, "[xclass] types: %u added (%u -> %u), first %X%c", (unsigned)add.size(), hn, nn, add[0], 10);
              }
          } else fprintf(stderr, "[xclass] types: none to add (map %08X n %u, source map %08X)%c", hmap, hn, smap, 10);
        }
        /* the moby manager's per-class prep (func_0068FAE0 runs it for every class at load): +0x40 / +0x44 / +0x6C objects, from a
         * bump allocator of our own {+0 base, +4 size, +8 used from the bottom, +0xC used from the top, +0x10 started} */
        const uint32_t abn = 0x40000u, ab = galp(abn);   /* 14/11: 4 MB -> 256 KB (a class uses ~80 KB) */
        if (gp(ab)) {
            for (uint32_t o = 0; o < 0x40; o += 4) vm_write32(ab + o, 0);
            vm_write32(ab, ab + 0x100); vm_write32(ab + 4, abn - 0x100);
            ppu_context* cur = g_active_ctx;
            ppu_context c2 = *cur;
            const uint32_t sp = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFu;
            vm_write64(sp, (uint32_t)cur->gpr[1]);
            for (uint32_t o = 0x40; o <= 0x400; o += 0x10) vm_write64(sp - o + 0x28, kTocMoby);
            c2.gpr[1] = sp; c2.gpr[2] = kTocMoby; c2.gpr[3] = 0; c2.gpr[4] = cls; c2.gpr[5] = ab; c2.lr = 0; c2.cr = 0;
            void (*stf)(void*) = g_trampoline_fn;
            g_trampoline_fn = nullptr;
            g_active_ctx = &c2;
            func_0068F570(&c2);
            while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c2); }
            g_active_ctx = cur;
            g_trampoline_fn = stf;
        }
        fprintf(stderr, "[xclass] class slot %u = %08X; prep: +40 %08X +44 %08X +6C %08X +48 %08X\n", S, cls,
                vm_read32(cls + 0x40), vm_read32(cls + 0x44), vm_read32(cls + 0x6C), vm_read32(cls + 0x48));
        /* an instance record of class FC from the imported gameplay.dat (section 0x7340, 0x48 each, u16 +0x3C class), as the
         * record spawn=S clones: class index -> S, segment masks (+0x10/+0x14) of the placed moby nearest the hero */
        uint32_t ninst = 0; const uint32_t t7340 = gp(g_tod_import_gp) ? sec(g_tod_import_gp, 0x7340, &ninst) : 0;
        uint32_t src = 0, seen = 0;
        for (uint32_t i = 0; i < ninst && !src; i++) if (vm_read16(t7340 + i * 0x48 + 0x3C) == fc && seen++ == inst) src = t7340 + i * 0x48;
        if (!src) { fprintf(stderr, "[xclass] no instance %u of class %u in gameplay.dat (%u records)\n", inst, fc, ninst); return true; }
        const uint32_t rec = galp(0x80);
        if (!gp(rec)) return true;
        gcopy(rec, src, 0x48);
        vm_write16(rec + 0x3C, (uint16_t)S);
        { const uint32_t he = entity_of(h); float bd = 1e30f; uint32_t best = 0;
          if (he) for (uint32_t i = 0; i < g_tod_inst_n; i++) {
              const uint32_t ei = g_tod_inst_list[i][2];
              if (!gp(ei)) continue;
              const float dx = rf(ei + 0x30) - rf(he + 0x30), dz = rf(ei + 0x38) - rf(he + 0x38), d2 = dx * dx + dz * dz;
              if (d2 < bd) { bd = d2; best = g_tod_inst_list[i][1]; } }
          if (best) { vm_write32(rec + 0x10, vm_read32(best + 0x10)); vm_write32(rec + 0x14, vm_read32(best + 0x14));
                      if (!getenv("TOD_XCLASS_KEEP44")) vm_write32(rec + 0x44, vm_read32(best + 0x44)); } }   /* +0x44 high half: zone of the level (Rykan arena 3, Cragmite 10/12) */
        /* the per-instance config block (rec+0: a small IGHW, section 0x25200 at +0x20, size at +0x18): our own copy, and the
         * enemy AREA indices at +0x4F0 / +0x500 (indices into the level's volumes, section 0x7740) taken from the nearest placed
         * moby of this level that has them: the source level's indices point at unrelated volumes here (the moby ran off to one
         * of them, or stood still) */
        { const uint32_t b0 = vm_read32(rec);
          if (gp(b0) && vm_read32(b0) == 0x49474857u) {
              const uint32_t bsz = 0x20 + vm_read32(b0 + 0x18), nb = rec + 0x100;
              if (bsz < 0x10000) {
                  gcopy(nb, b0, (bsz + 3) & ~3u);
                  vm_write32(rec, nb);
                  if (bsz >= 0x520 && !getenv("TOD_XCLASS_KEEPAREA")) {
                      const uint32_t he = entity_of(h); float bd = 1e30f; uint32_t bb = 0;
                      if (he) for (uint32_t i = 0; i < g_tod_inst_n; i++) {
                          const uint32_t ei = g_tod_inst_list[i][2], ri = g_tod_inst_list[i][1], bi = gp(ri) ? vm_read32(ri) : 0;
                          if (!gp(ei) || !gp(bi) || vm_read32(bi) != 0x49474857u || vm_read32(bi + 0x18) + 0x20 < 0x520) continue;
                          const uint32_t a1 = vm_read32(bi + 0x4F0);
                          if (!a1 || a1 == 0xFFFFFFFFu) continue;
                          const float dx = rf(ei + 0x30) - rf(he + 0x30), dz = rf(ei + 0x38) - rf(he + 0x38), d2 = dx * dx + dz * dz;
                          if (d2 < bd) { bd = d2; bb = bi; } }
                      if (const char* av = getenv("TOD_XCLASS_AREA")) {   /* research: force both area indices (FFFFFFFF = none) */
                          const uint32_t v2 = (uint32_t)strtoul(av, 0, 16);
                          fprintf(stderr, "[xclass] areas %X/%X -> %X/%X (TOD_XCLASS_AREA)%c", vm_read32(nb + 0x4F0), vm_read32(nb + 0x500), v2, v2, 10);
                          vm_write32(nb + 0x4F0, v2); vm_write32(nb + 0x500, v2); bb = 0;
                      }
                      if (bb) {
                          fprintf(stderr, "[xclass] areas %X/%X -> %X/%X (block of the placed moby %.0f m away)\n", vm_read32(nb + 0x4F0), vm_read32(nb + 0x500),
                                  vm_read32(bb + 0x4F0), vm_read32(bb + 0x500), sqrtf(bd));
                          vm_write32(nb + 0x4F0, vm_read32(bb + 0x4F0)); vm_write32(nb + 0x500, vm_read32(bb + 0x500));
                          /* +0x514: a PATH index (the walker of Cragmite followed its path 0x21, here a path leading to 83,-82);
                           * +0x524: another level index (0x24..0x40): both from the same placed moby */
                          fprintf(stderr, "[xclass] path %X -> %X, +524 %X -> %X%c", vm_read32(nb + 0x514), vm_read32(bb + 0x514), vm_read32(nb + 0x524), vm_read32(bb + 0x524), 10);
                          vm_write32(nb + 0x514, vm_read32(bb + 0x514));
                          if (!getenv("TOD_XCLASS_KEEP524")) vm_write32(nb + 0x524, vm_read32(bb + 0x524));
                      }
                  }
              }
          } }
        g_tod_inst_rec[S] = rec;
        fprintf(stderr, "[xclass] record %08X (from %08X, instance %u of class %u) -> slot %u: now spawn=%u:N:DIST\n", rec, src, inst, fc, S, S);
        return true;
    }
    if (!c.compare(0, 7, "rwatch=")) {   /* research (12/11): rwatch=HEXADDR -> who READS that guest 4 KB page (host code addresses, see rwatchdump) */
        const uint32_t a = (uint32_t)strtoul(c.c_str() + 7, 0, 16);
        tod_rwatch_arm(a);
        return true;
    }
    /* Effects of other levels (12/11). fxdump=PATH, in the SOURCE level: the effect id table (effect.dat section 0x500, at
     * [[TOC-0x7350]]), every effect descriptor (0x24 bytes at [EM+0x4EC], EM = [TOC-0x7360]) with its definition as the loader
     * built it (position independent: only offsets inside; size at def+0x18), and the effect conduit image (fxconduit_packed.dat
     * as loaded, base + size from its IGHW header, root = [dispatcher manager+8]). fximport=PATH, in the HOST level after the
     * xclass imports: appends the definitions of the effects the host lacks (new descriptor array, new indices in the host id
     * table), loads the conduit image relocated, and makes the dispatcher (combat_log.cpp func_005F5C80) use it for events of
     * mobys of the imported classes. Textures of effect.tp are not handled yet. */
    if (!c.compare(0, 7, "fxdump=") || !c.compare(0, 9, "fximport=")) {
        const bool dump = c[2] == 'd';
        const std::string path = c.substr(dump ? 7 : 9);
        const uint32_t kToc = 0x8AFE5Cu, EM = vm_read32(kToc - 0x7360), T = vm_read32(vm_read32(kToc - 0x7350));
        const uint32_t D = gp(EM) ? vm_read32(EM + 0x4EC) : 0, Dh = vm_read32(kToc - 0x766C);
        if (!gp(EM) || !gp(T) || !gp(D)) { fprintf(stderr, "[fx] no effect manager (EM %08X T %08X D %08X)%c", EM, T, D, 10); return true; }
        const uint32_t nT = vm_read32(T), base4 = vm_read32(T + 4);
        uint32_t N = 0;
        for (uint32_t j = 0; j < nT && j < 0x4000; j++) { const int32_t v = (int32_t)vm_read32(T + 8 + j * 4); if (v >= 0 && (uint32_t)v + 1 > N) N = (uint32_t)v + 1; }
        /* 14/11: a second fximport must append after ALL effects already there: the previous import's source-only effects (the
         * highest indices) are not in the host id table, so the table maximum stopped one short and the next source overwrote the
         * previous source's last effect (indices 747..1118 after 385..747 in rw5) */
        if (!dump && vm_read32(EM + 0x490) > N && vm_read32(EM + 0x490) < 0x4000) N = vm_read32(EM + 0x490);
        auto rd =[](uint32_t a, uint32_t n) { std::vector<uint8_t> b(n); for (uint32_t i = 0; i < n; i++) b[i] = vm_read8(a + i); return b; };
        auto wr = [](uint32_t a, const uint8_t* p, uint32_t n) { for (uint32_t i = 0; i < n; i++) vm_write8(a + i, p[i]); };
        auto galx = [&](uint32_t size) -> uint32_t {
            ppu_context fake; memset(&fake, 0, sizeof fake);
            const uint32_t out = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
            vm_write32(out, 0);
            fake.gpr[3] = (size + 0xFFFFFu) & ~0xFFFFFu; fake.gpr[4] = 0x400; fake.gpr[5] = out;
            sys_memory_allocate(&fake);
            return vm_read32(out);
        };
        if (dump) {
            FILE* f = fopen(path.c_str(), "wb");
            if (!f) { fprintf(stderr, "[fxdump] cannot write %s%c", path.c_str(), 10); return true; }
            auto w32 = [&](uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(b, 1, 4, f); };
            fwrite("FXPK", 1, 4, f); w32(1); w32(nT); w32(base4);
            for (uint32_t j = 0; j < nT; j++) w32(vm_read32(T + 8 + j * 4));
            w32(N);
            uint32_t total = 0;
            for (uint32_t k = 0; k < N; k++) {
                const uint32_t de = D + k * 0x24, def = vm_read32(de);
                uint32_t sz = gp(def) ? vm_read32(def + 0x18) : 0;
                if (sz < 0x20 || sz > 0x40000) sz = 0;
                std::vector<uint8_t> dd = rd(de, 0x24); fwrite(dd.data(), 1, 0x24, f);
                w32(sz);
                if (sz) { std::vector<uint8_t> b = rd(def, sz); fwrite(b.data(), 1, sz, f); total += sz; }
            }
            /* conduit: root from the dispatcher's manager; header found by scanning down from the root's first pointer */
            const uint32_t root = g_fx_disp_mgr ? vm_read32(g_fx_disp_mgr + 8) : 0;
            uint32_t H = 0, size = 0;
            if (gp(root)) for (uint32_t a = vm_read32(root) & ~3u, lim = a - 0x80000; a > lim; a -= 4)
                if (vm_read32(a) == 0x49474857u && vm_read32(a + 4) == 2) { H = a; break; }
            if (H) { const uint32_t ns = vm_read32(H + 8);
                for (uint32_t s = 0; s < ns && s < 0x2000; s++) { const uint32_t o = vm_read32(H + 0x10 + s * 16 + 4), z = vm_read32(H + 0x10 + s * 16 + 8);
                    if (o + z > size && o < 0x200000) size = o + z; } }
            w32(H); w32(size); w32(root);
            if (H && size) { std::vector<uint8_t> b = rd(H, size); fwrite(b.data(), 1, size, f); }
            fclose(f);
            fprintf(stderr, "[fxdump] ids %u (base %u), effects %u (%u KB of definitions), conduit %08X + %X root %08X -> %s%c", nT, base4, N, total >> 10,
                    H, size, root, path.c_str(), 10);
            return true;
        }
        /* fximport: "fximport=PK|TEXMAP". TEXMAP (optional, text "src dst" per line) = the source level's effect texture index ->
         * the host's (both are positions in the level's system.tph; same image found by content, scratchpad systp.py). In the
         * definitions a texture is referenced as u16 0x8000|index: without the map an imported effect shows the host's texture
         * at that position (the Cragmite mother's shot was purple instead of green). */
        std::map<uint32_t, uint32_t> texmap;
        std::string pkpath = path;
        { const size_t bar = path.find('|');
          if (bar != std::string::npos) {
              pkpath = path.substr(0, bar);
              if (FILE* tf = fopen(path.substr(bar + 1).c_str(), "r")) { unsigned a2, b2; while (fscanf(tf, "%u %u", &a2, &b2) == 2) texmap[a2] = b2; fclose(tf); }
              fprintf(stderr, "[fximport] texture map: %u entries%c", (unsigned)texmap.size(), 10);
          } }
        FILE* f = fopen(pkpath.c_str(), "rb");
        if (!f) { fprintf(stderr, "[fximport] cannot read %s%c", path.c_str(), 10); return true; }
        std::vector<uint8_t> fb; { uint8_t buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) fb.insert(fb.end(), buf, buf + n); } fclose(f);
        size_t p = 0;
        auto g32 = [&]() -> uint32_t { if (p + 4 > fb.size()) return 0; const uint32_t v = (uint32_t)fb[p] << 24 | (uint32_t)fb[p + 1] << 16 | (uint32_t)fb[p + 2] << 8 | fb[p + 3]; p += 4; return v; };
        if (fb.size() < 16 || memcmp(fb.data(), "FXPK", 4)) { fprintf(stderr, "[fximport] bad file%c", 10); return true; }
        p = 4; g32();
        const uint32_t snT = g32(); g32();
        std::vector<int32_t> sT(snT); for (uint32_t j = 0; j < snT; j++) sT[j] = (int32_t)g32();
        const uint32_t sN = g32();
        std::vector<size_t> sdesc(sN), sdef(sN); std::vector<uint32_t> ssz(sN);
        for (uint32_t k = 0; k < sN; k++) { sdesc[k] = p; p += 0x24; ssz[k] = g32(); sdef[k] = p; p += ssz[k]; }
        const uint32_t oH = g32(), osz = g32(), oroot = g32(); const size_t pcond = p;
        /* effects the host lacks */
        std::vector<std::pair<uint32_t, uint32_t>> add;   /* (table slot j, source index) */
        for (uint32_t j = 0; j < snT && j < nT; j++)
            if (sT[j] >= 0 && (uint32_t)sT[j] < sN && ssz[sT[j]] && (int32_t)vm_read32(T + 8 + j * 4) < 0) add.push_back({ j, (uint32_t)sT[j] });
        /* ALL source effects are imported as copies of their own (12/11 night: 25 of the 279 ids both levels have are different
         * effects in the two levels, e.g. the children of the Cragmite mother's shot: mapping them to the host's version made
         * the shot purple instead of green). Foreign events look ids up in a source id table (g_fx_foreign_T) */
        uint32_t need = (N + sN) * 0x24 + 0x100;
        std::map<uint32_t, uint32_t> s2k;
        for (uint32_t s = 0; s < sN; s++) if (ssz[s]) { need += (ssz[s] + 0x7F) & ~0x7Fu; s2k[s] = 0; }
        const uint32_t buf = galx(need);
        if (!gp(buf)) { fprintf(stderr, "[fximport] no memory (%u)%c", need, 10); return true; }
        const uint32_t ND = buf; uint32_t cur = (buf + (N + (uint32_t)s2k.size()) * 0x24 + 0x7F) & ~0x7Fu;
        for (uint32_t o = 0; o < N * 0x24; o += 4) vm_write32(ND + o, vm_read32(D + o));
        uint32_t k = N;
        /* source effect index -> host index: the same global id if the host has it, else the new index of the imported copy */
        { uint32_t kk = N; for (auto& e : s2k) e.second = kk++; }
        std::map<uint32_t, uint32_t> s2h;
        for (auto& e : s2k) s2h[e.first] = e.second;
        /* the source id table: same header as the host's, every id the source has -> its imported copy */
        { const uint32_t FT = galx((snT + 2) * 4 + 0x100);
          if (gp(FT)) {
              vm_write32(FT, vm_read32(T)); vm_write32(FT + 4, vm_read32(T + 4));
              for (uint32_t j = 0; j < nT; j++) {
                  const int32_t sv = j < snT ? sT[j] : -1;
                  vm_write32(FT + 8 + j * 4, (sv >= 0 && s2h.count((uint32_t)sv)) ? s2h[(uint32_t)sv] : 0xFFFFFFFFu);
              }
              g_fx_foreign_T = FT; g_fx_Tslot = vm_read32(kToc - 0x7350);
          } }
        unsigned nkid = 0, nkidmiss = 0;
        for (auto& e : s2k) {
            const uint32_t s = e.first, de = ND + k * 0x24;
            wr(de, fb.data() + sdesc[s], 0x24);
            vm_write32(de + 0, cur); vm_write32(de + 4, 0); vm_write32(de + 8, 0); vm_write32(de + 0x10, 0); vm_write32(de + 0x14, 0);
            vm_write32(de + 0x18, 0); vm_write32(de + 0x1C, 0);
            wr(cur, fb.data() + sdef[s], ssz[s]);
            /* the definition's own index (u16 at +0x20) and its texture references (u16 0x8000|t) in the host's numbering */
            vm_write16(cur + 0x20, (uint16_t)k);
            if (!texmap.empty())
                for (uint32_t o = 0x22; o + 2 <= ssz[s]; o += 2) {
                    const uint16_t v = vm_read16(cur + o);
                    if ((v & 0xFF80) == 0x8000 && v != 0x8000) { auto it = texmap.find(v & 0x7F); if (it != texmap.end()) vm_write16(cur + o, (uint16_t)(0x8000 | it->second)); }
                }
            /* child effects (func_005E95B8 spawns them by the u16 index of a block): (1) definitions embedded in this one, header
             * A8,0,0,1C, index at +0x20; (2) 0x80-byte emitter records: flags word (bytes <= 2, 2nd byte 1), a float, 0 or 0x01000000,
             * then index<<16. Both rules found all 60 child indices of the 279 effects Cragmite and Rykan share, no false hit
             * (scratchpad fxkids.py). Indices are rewritten from the source numbering to the host's. */
            { const uint8_t* d = fb.data() + sdef[s]; const uint32_t sz = ssz[s];
              auto U = [&](uint32_t o) -> uint32_t { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
              auto flt = [](uint32_t w) { const uint32_t ex = (w >> 23) & 0xFF; return ex >= 0x70 && ex <= 0x88 && !(w >> 31); };
              std::vector<uint32_t> ko;
              for (uint32_t q = 4; q + 0x24 < sz; q += 4) if (U(q) == 0xA8 && U(q + 4) == 0 && U(q + 8) == 0 && U(q + 0xC) == 0x1C) ko.push_back(q + 0x20);
              for (uint32_t q = 0x30; q + 4 <= sz; q += 4) {
                  const uint32_t w = U(q);
                  if ((w & 0xFFFF) || !((w >> 16) > 0 && (w >> 16) < sN)) continue;
                  const uint32_t fl = U(q - 0x14);
                  const uint32_t b0 = fl >> 24, b1 = (fl >> 16) & 0xFF, b2 = (fl >> 8) & 0xFF, b3 = fl & 0xFF;
                  if (b1 == 1 && b0 <= 2 && b2 <= 2 && b3 <= 2 && flt(U(q - 0x10)) && (U(q - 4) == 0 || U(q - 4) == 0x01000000u)) ko.push_back(q);
              }
              for (uint32_t q : ko) {
                  const uint32_t si = ((uint32_t)d[q] << 8) | d[q + 1];
                  auto it = s2h.find(si);
                  if (it != s2h.end()) { vm_write16(cur + q, (uint16_t)it->second); nkid++; } else nkidmiss++;
              } }
            k++; cur += (ssz[s] + 0x7F) & ~0x7Fu;
        }
        for (auto& a : add) vm_write32(T + 8 + a.first * 4, s2k[a.second]);
        vm_write32(EM + 0x4EC, ND);
        /* effect count: func_005E5B58 rejects an index >= [EM+0x490] */
        fprintf(stderr, "[fximport] effect count [EM+0x490] %u -> %u; child indices rewritten %u, not mapped %u%c", vm_read32(EM + 0x490), k, nkid, nkidmiss, 10);
        if (vm_read32(EM + 0x490) < k) vm_write32(EM + 0x490, k);
        if (gp(Dh) && vm_read32(Dh) == D) vm_write32(Dh, ND);
        /* conduit image, relocated */
        uint32_t nroot = 0;
        if (oH && osz && pcond + osz <= fb.size()) {
            const uint32_t CB = galx(osz + 0x100);
            if (gp(CB)) {
                wr(CB, fb.data() + pcond, osz);
                uint32_t nrel = 0;
                for (uint32_t o = 0; o + 4 <= osz; o += 4) { const uint32_t v = vm_read32(CB + o); if (v >= oH && v < oH + osz) { vm_write32(CB + o, v - oH + CB); nrel++; } }
                nroot = oroot - oH + CB;
                fprintf(stderr, "[fximport] conduit %08X + %X -> %08X (%u pointers relocated), root %08X%c", oH, osz, CB, nrel, nroot, 10);
                /* root lists (IGHW section 0x12000: pointers to the conduit structures, one per manager): host <-> foreign by position */
                auto rootlist = [&](uint32_t H2, uint32_t* out) -> uint32_t {
                    const uint32_t ns = vm_read32(H2 + 8);
                    for (uint32_t s = 0; s < ns && s < 0x2000; s++) if (vm_read32(H2 + 0x10 + s * 16) == 0x12000) {
                        const uint32_t o = vm_read32(H2 + 0x10 + s * 16 + 4), z = vm_read32(H2 + 0x10 + s * 16 + 8);
                        uint32_t n = 0; for (uint32_t q = 0; q < z / 4 && n < 8; q++) out[n++] = vm_read32(H2 + o + q * 4);
                        return n;
                    }
                    return 0;
                };
                const uint32_t hroot = g_fx_disp_mgr ? vm_read32(g_fx_disp_mgr + 8) : 0;
                uint32_t HH = 0;
                if (gp(hroot)) for (uint32_t a = vm_read32(hroot) & ~3u, lim = a - 0x80000; a > lim; a -= 4)
                    if (vm_read32(a) == 0x49474857u && vm_read32(a + 4) == 2) { HH = a; break; }
                const uint32_t nh = HH ? rootlist(HH, g_fx_host_roots) : 0, nf = rootlist(CB, g_fx_foreign_roots);
                g_fx_nroots = nh < nf ? nh : nf;
                fprintf(stderr, "[fximport] host conduit %08X roots %u (manager root %08X), foreign roots %u:", HH, nh, hroot, nf);
                for (uint32_t i = 0; i < g_fx_nroots; i++) fprintf(stderr, " %08X->%08X", g_fx_host_roots[i], g_fx_foreign_roots[i]);
                fprintf(stderr, "%c", 10);
            }
        }
        g_fx_foreign_root = nroot;
        { const uint32_t arr = vm_read32(kToc - 0x5B10); g_fx_foreign_ncls = 0;
          for (uint32_t s = 0; s < 0x400 && g_fx_foreign_ncls < 16; s++) if (g_tod_xslot[s] && gp(arr)) g_fx_foreign_cls[g_fx_foreign_ncls++] = vm_read32(arr + s * 4); }
        {   /* 14/11: the same, per source level, so several imported levels each keep their own effects */
            const uint32_t q = g_tod_import_n < 9 ? g_tod_import_n : 0, arr = vm_read32(kToc - 0x5B10);
            g_fx_src_T[q] = g_fx_foreign_T; g_fx_src_nroots[q] = nroot ? g_fx_nroots : 0;
            memcpy(g_fx_src_roots[q], g_fx_foreign_roots, sizeof g_fx_src_roots[q]);
            g_fx_src_ncls[q] = 0;
            for (uint32_t s = 0; s < 0x400 && g_fx_src_ncls[q] < 16; s++) if (g_tod_xslot[s] == q && gp(arr)) g_fx_src_cls[q][g_fx_src_ncls[q]++] = vm_read32(arr + s * 4);
            fprintf(stderr, "[fximport] source %u: %u classes, %u roots%c", q, g_fx_src_ncls[q], g_fx_src_nroots[q], 10);
        }
        fprintf(stderr, "[fximport] %u table ids -> %u new effects (indices %u..%u), descriptors %08X -> %08X, %u foreign classes%c", (unsigned)add.size(),
                (unsigned)s2k.size(), N, k - 1, D, ND, g_fx_foreign_ncls, 10);
        return true;
    }
    if (!c.compare(0, 8, "rwatchx=")) {   /* like rwatch= but only reads of exactly those 4 bytes are recorded (e.g. one TOC slot) */
        tod_rwatch_arm2((uint32_t)strtoul(c.c_str() + 8, 0, 16), 1);
        return true;
    }
    if (!c.compare(0, 8, "wwatchx=")) { tod_wwatch_arm((uint32_t)strtoul(c.c_str() + 8, 0, 16)); return true; }   /* research 15/11 */
    if (c == "rwatchdump") { tod_rwatch_dump(); return true; }
    if (!c.compare(0, 5, "poke=")) {   /* research (11/11 night): poke=HEXADDR:HEXVAL -> one big-endian 32-bit guest write */
        unsigned a = 0, val = 0;
        if (sscanf(c.c_str() + 5, "%x:%x", &a, &val) == 2 && gp(a)) {
            fprintf(stderr, "[poke] %08X: %08X -> %08X\n", a, vm_read32(a), val);
            vm_write32(a, val);
        }
        return true;
    }
    if (!c.compare(0, 8, "memsave=")) {   /* research (11/11): memsave=HEXADDR:HEXLEN:path -> guest bytes to a host file */
        unsigned a = 0, n = 0; char path[400] = {0};
        if (sscanf(c.c_str() + 8, "%x:%x:%399s", &a, &n, path) != 3) return true;
        FILE* f = fopen(path, "wb");
        if (!f) { fprintf(stderr, "[memsave] cannot write %s\n", path); return true; }
        std::vector<uint8_t> b(n);
        for (uint32_t i = 0; i < n; i++) b[i] = vm_read8(a + i);
        fwrite(b.data(), 1, n, f); fclose(f);
        fprintf(stderr, "[memsave] %08X + %X -> %s\n", a, n, path);
        return true;
    }
    if (!c.compare(0, 8, "memfind=")) {   /* research (11/11): memfind=HEXBYTES -> 16-byte aligned hits in guest RAM and RSX local memory */
        std::string hx = c.substr(8); uint8_t pat[32]; int np = 0; uint32_t step = 16;   /* "memfind=HEX/4": 4-byte aligned (pointers) */
        { size_t sl = hx.find('/'); if (sl != std::string::npos) { step = (uint32_t)strtoul(hx.c_str() + sl + 1, 0, 10); if (step < 1) step = 16; hx = hx.substr(0, sl); } }
        for (size_t i = 0; i + 1 < hx.size() && np < 32; i += 2) pat[np++] = (uint8_t)strtoul(hx.substr(i, 2).c_str(), 0, 16);
        if (np < 4) return true;
        int hits = 0;
        const uint32_t ranges[][2] = { {0x00010000u, 0x40000000u}, {0x40000000u, 0x50000000u}, {0xC0000000u, 0xD0000000u} };
        for (auto& r : ranges) for (uint32_t a = r[0]; a < r[1] && hits < 32; a += step) {
            if (!(a & 0xFFFF) && a < 0xC0000000u && !(g_ppu_vm_page[a >> 16] & 1u)) { a += 0x10000 - step; continue; }   /* only mapped (fast-read) 64 KB pages; RSX local memory (0xC0000000, 256 MB) always */
            if (vm_read8(a) != pat[0] || vm_read8(a + 1) != pat[1] || vm_read8(a + 2) != pat[2] || vm_read8(a + 3) != pat[3]) continue;
            bool ok = true; for (int k = 4; k < np && ok; k++) ok = vm_read8(a + k) == pat[k];
            if (ok) { fprintf(stderr, "[memfind] hit at %08X\n", a); hits++; }
        }
        fprintf(stderr, "[memfind] %d hits\n", hits);
        return true;
    }
    if (!c.compare(0, 6, "edump=")) {   /* research (11/11): edump=HEX entity -> its 0x180 bytes and 0x200 of its update object */
        const uint32_t e = (uint32_t)strtoul(c.c_str() + 6, 0, 16);
        if (!gp(e)) return true;
        const uint32_t o = vm_read32(e + 0x40);
        char line[8192]; int n = snprintf(line, sizeof line, "[edump] ent %08X:", e);
        for (uint32_t k = 0; k < 0x180; k += 4) n += snprintf(line + n, sizeof line - n, " %08X", vm_read32(e + k));
        fprintf(stderr, "%s\n", line);
        if (gp(o)) { n = snprintf(line, sizeof line, "[edump] obj %08X:", o);
            for (uint32_t k = 0; k < 0x200; k += 4) n += snprintf(line + n, sizeof line - n, " %08X", vm_read32(o + k));
            fprintf(stderr, "%s\n", line); }
        return true;
    }
    if (!c.compare(0, 6, "spawn=")) {   /* spawn=IDX[:N[:DIST]] (11/11): the game's full spawn func_0068EA08 of level class index IDX, N in a ring around the hero */
        unsigned idx = 0, n = 1, typ = 0; float dist = 6.f;
        const int na = sscanf(c.c_str() + 6, "%u:%u:%f:%x", &idx, &n, &dist, &typ);
        if (na < 1 || idx >= 0x400) return true;
        /* 12/11: spawn=IDX:N:DIST:TYPE (hex) = no clone of a placed instance: the runtime spawn func_0068EA08 with r4 = TYPE, as the
         * IFF arena does (arena_spawn: r4 = FFFFFFFF = the class's own behaviour; works for classes with no placed instance) */
        const bool force_type = na >= 4;
        /* every spawned moby: [xwatch] / xlua= list, and for an imported (foreign) class the code range of its update object's own
         * methods, so that spawn-by-ART-id calls made from it use the source level's ids (combat_log.cpp func_0068EC48) */
        auto xreg = [&](uint32_t ne, uint32_t nh) {
            if (gp(ne) && s_xwatch_n < 16) { s_xwatch[s_xwatch_n][0] = ne; s_xwatch[s_xwatch_n][1] = nh; s_xwatch[s_xwatch_n][2] = idx; s_xwatch_n++; s_xwatch_t = t; }
            if (idx < 0x400 && g_tod_xslot[idx] && gp(ne)) {
                const uint32_t ob = vm_read32(ne + 0x40), vt = gp(ob) ? vm_read32(ob) : 0;
                uint32_t lo = 0xFFFFFFFFu, hi = 0;
                for (uint32_t e = 0; gp(vt) && e < 0x80; e += 4) {
                    const uint32_t opd = vm_read32(vt + e), f = gp(opd) || (opd >= 0x10000 && opd < 0x900000) ? vm_read32(opd) : 0;
                    if (f >= 0x00100000u && f < 0x00480000u) { if (f < lo) lo = f; if (f > hi) hi = f; }
                }
                if (hi && g_tod_xrange_n < 32) {
                    bool have = false;
                    for (uint32_t r = 0; r < g_tod_xrange_n; r++) have |= g_tod_xrange[r][0] == lo - 0x4000u;
                    if (!have) { g_tod_xrange[g_tod_xrange_n][0] = lo - 0x4000u; g_tod_xrange[g_tod_xrange_n][1] = hi + 0x8000u; g_tod_xrange_src[g_tod_xrange_n] = g_tod_xslot[idx]; g_tod_xrange_n++;
                        fprintf(stderr, "[xclass] slot %u: foreign code %08X..%08X (update vtable %08X)%c", idx, lo - 0x4000u, hi + 0x8000u, vt, 10); }
                }
            }
        };
        const uint32_t he = entity_of(h);
        if (!he) return false;
        const float hx = rf(he + 0x30), hy = rf(he + 0x34), hz = rf(he + 0x38);
        const uint32_t toc = g_tod_spawn_toc2 ? g_tod_spawn_toc2 : (g_tod_spawn_toc ? g_tod_spawn_toc : kTocLua);
        for (unsigned k = 0; k < n && k < 32; k++) {
            const float a = 6.2831853f * (float)k / (float)(n ? n : 1), x = hx + dist * cosf(a), y = hy + 0.5f, z = hz + dist * sinf(a);   /* y is up (enemies on the ground: y ~ 5 on Rykan V) */
            if (!force_type && g_tod_inst_rec[idx] && gp(g_tod_inst_mgr)) {
                /* clone a placed instance: copy its 0x48-byte record into a guest buffer of our own (kept: the game may keep
                 * pointers into it), new position, then the level load's spawn func_0068ED48(manager, record) */
                static uint32_t s_recbuf, s_recn;
                if (!s_recbuf) {
                    ppu_context fake; memset(&fake, 0, sizeof fake);
                    const uint32_t out = ((uint32_t)g_active_ctx->gpr[1] - 0x800u) & ~0xFu;
                    vm_write32(out, 0);
                    fake.gpr[3] = 0x10000; fake.gpr[4] = 0x200; fake.gpr[5] = out;
                    sys_memory_allocate(&fake);
                    s_recbuf = vm_read32(out);
                    fprintf(stderr, "[spawn] record buffer at guest 0x%08X%c", s_recbuf, 10);
                }
                if (!gp(s_recbuf)) return true;
                /* which placed instance to copy: among the instances of this class, the update type (vtable of [ent+0x40]) seen most
                 * often, and the record of one of those (the same model can be placed with other types, e.g. a static one) */
                uint32_t src = g_tod_inst_rec[idx], best_vt = 0, best_n = 0;
                for (uint32_t i = 0; i < g_tod_inst_n; i++) {
                    if (g_tod_inst_list[i][0] != idx) continue;
                    const uint32_t ei = g_tod_inst_list[i][2], oi = gp(ei) ? vm_read32(ei + 0x40) : 0, vt = gp(oi) ? vm_read32(oi) : 0;
                    if (!vt) continue;
                    uint32_t cnt_vt = 0;
                    for (uint32_t j = 0; j < g_tod_inst_n; j++) if (g_tod_inst_list[j][0] == idx) {
                        const uint32_t ej = g_tod_inst_list[j][2], oj = gp(ej) ? vm_read32(ej + 0x40) : 0;
                        if (gp(oj) && vm_read32(oj) == vt) cnt_vt++; }
                    if (cnt_vt > best_n) { best_n = cnt_vt; best_vt = vt; src = g_tod_inst_list[i][1]; }
                }
                /* among the instances with that update type, the one placed nearest to the hero (same zone of the level, so its
                 * segment masks and activation are the ones of where the hero is) */
                float bd = 1e30f;
                for (uint32_t i = 0; i < g_tod_inst_n; i++) {
                    if (g_tod_inst_list[i][0] != idx) continue;
                    const uint32_t ei = g_tod_inst_list[i][2], oi = gp(ei) ? vm_read32(ei + 0x40) : 0;
                    if (!gp(oi) || vm_read32(oi) != best_vt) continue;
                    const float dx = rf(ei + 0x30) - hx, dy = rf(ei + 0x34) - hy, dz = rf(ei + 0x38) - hz, d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 < bd) { bd = d2; src = g_tod_inst_list[i][1]; s_tod_spawn_srcent = ei; }
                }
                const uint32_t rec = s_recbuf + (s_recn++ % 1024u) * 0x50u;
                for (uint32_t o = 0; o < 0x48; o += 4) vm_write32(rec + o, vm_read32(src + o));
                fprintf(stderr, "[spawn] idx=%u: copying record %08X (update type %08X, %u instances of it), source entity %08X at %.1f %.1f %.1f\n", idx, src, best_vt, best_n,
                        s_tod_spawn_srcent, rf(s_tod_spawn_srcent + 0x30), rf(s_tod_spawn_srcent + 0x34), rf(s_tod_spawn_srcent + 0x38));
                if (s_tod_spawn_posoff) { wf(rec + s_tod_spawn_posoff, x); wf(rec + s_tod_spawn_posoff + 4, y); wf(rec + s_tod_spawn_posoff + 8, z); }
                const uint32_t toc2 = g_tod_inst_toc ? g_tod_inst_toc : toc;
                ppu_context* cur2 = g_active_ctx;
                ppu_context c2 = *cur2;
                const uint32_t sp2 = ((uint32_t)cur2->gpr[1] - 0x1000u) & ~0xFu;
                vm_write64(sp2, (uint32_t)cur2->gpr[1]);
                for (uint32_t o = 0x40; o <= 0x400; o += 0x10) vm_write64(sp2 - o + 0x28, toc2);
                c2.gpr[1] = sp2; c2.gpr[2] = toc2; c2.gpr[3] = g_tod_inst_mgr; c2.gpr[4] = rec; c2.lr = 0; c2.cr = 0;
                void (*stf)(void*) = g_trampoline_fn;
                g_trampoline_fn = nullptr;
                g_active_ctx = &c2;
                g_tod_spawn_cloning = 1; func_0068ED48(&c2); g_tod_spawn_cloning = 0;
                while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c2); }
                g_active_ctx = cur2;
                g_trampoline_fn = stf;
                /* wake it up and alert it, as Lua enemy_wakeup / enemy_alert do with a moby handle (cores func_00277918 /
                 * func_0027E3A0: handle -> entity -> its update object) */
                const uint32_t ne = g_tod_last_spawn, nh = gp(ne) ? vm_read32(ne + 0x64) : 0;
                /* start the update object like the runtime spawn func_0068EA08 does right after the manager's +0x2C: its vtable +0x8
                 * (the level load does it later for every placed moby; without it the object stays zeroed and the moby is never
                 * updated nor drawn: sp32) */
                const uint32_t nobj = gp(ne) ? vm_read32(ne + 0x40) : 0, nvt = gp(nobj) ? vm_read32(nobj) : 0, nopd = gp(nvt) ? vm_read32(nvt + 8) : 0;
                if (gp(nopd) && !s_tod_spawn_nowake) {
                    ppu_context* cur4 = g_active_ctx;
                    ppu_context c4 = *cur4;
                    const uint32_t sp4 = ((uint32_t)cur4->gpr[1] - 0x1000u) & ~0xFu;
                    vm_write64(sp4, (uint32_t)cur4->gpr[1]);
                    const uint32_t ntoc = vm_read32(nopd + 4);
                    for (uint32_t o = 0x40; o <= 0x400; o += 0x10) vm_write64(sp4 - o + 0x28, ntoc);
                    c4.gpr[1] = sp4; c4.gpr[2] = ntoc; c4.gpr[3] = nobj; c4.ctr = vm_read32(nopd); c4.lr = 0; c4.cr = 0;
                    void (*stf4)(void*) = g_trampoline_fn;
                    g_trampoline_fn = nullptr;
                    g_active_ctx = &c4;
                    ps3_indirect_call(&c4);
                    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c4); }
                    g_active_ctx = cur4;
                    g_trampoline_fn = stf4;
                }
                if (nh && !s_tod_spawn_nowake) {
                    const uint32_t sp3 = ((uint32_t)g_active_ctx->gpr[1] - 0x1000u) & ~0xFu;
                    /* placed mobys start obviated (hidden, asleep) until their pod is unobviated: Lua unobviate(pod) calls, for
                     * every member, func_0024F878(entity) (stub -> func_0068DE20, r2 = 0x89FF20 like the Lua natives) */
                    for (uint32_t o = 0x40; o <= 0x400; o += 0x10) vm_write64(sp3 - o + 0x28, kTocLua);
                    tod_gcall_toc(func_0024F878, kTocLua, ne, 0);
                    for (uint32_t o = 0x40; o <= 0x400; o += 0x10) vm_write64(sp3 - o + 0x28, toc2);
                    tod_gcall_toc(func_00277918, toc2, nh, 0);
                    for (uint32_t o = 0x40; o <= 0x400; o += 0x10) vm_write64(sp3 - o + 0x28, toc2);
                    tod_gcall_toc(func_0027E3A0, toc2, nh, 0);
                }
                fprintf(stderr, "[spawn] idx=%u #%u clone of record %08X -> %08X ent %08X handle %08X%s at %.1f %.1f %.1f (hero %.1f %.1f %.1f)\n", idx, k, src,
                        rec, ne, nh, s_tod_spawn_nowake ? "" : " woken+alerted", x, y, z, hx, hy, hz);
                /* an imported (foreign) class: the code range of its update object's own methods, so that spawn-by-ART-id calls
                 * made from it use the source level's ids (combat_log.cpp func_0068EC48); base / engine methods are skipped */
                xreg(ne, nh);
                continue;
            }
            ppu_context* cur = g_active_ctx;
            ppu_context cc = *cur;
            const uint32_t sp = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFu, buf = sp - 0x3000u;   /* guest scratch: position + 3 rows */
            vm_write64(sp, (uint32_t)cur->gpr[1]);
            for (uint32_t o = 0x40; o <= 0x400; o += 0x10) vm_write64(sp - o + 0x28, toc);   /* frames reload r2 from +0x28 */
            wf(buf + 0, x); wf(buf + 4, y); wf(buf + 8, z); wf(buf + 12, 1.f);
            for (uint32_t r = 0; r < 3; r++) for (uint32_t q = 0; q < 4; q++) wf(buf + 0x10 + r * 16 + q * 4, (r == q) ? 1.f : 0.f);
            cc.gpr[1] = sp; cc.gpr[2] = toc; cc.gpr[3] = idx; cc.gpr[4] = force_type ? typ : g_tod_spawn_arg4[idx]; cc.gpr[5] = buf; cc.gpr[6] = buf + 0x10;
            cc.fpr[1] = 1.0; cc.lr = 0; cc.cr = 0;
            void (*saved_tf)(void*) = g_trampoline_fn;
            g_trampoline_fn = nullptr;
            g_active_ctx = &cc;
            func_0068EA08(&cc);
            while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&cc); }
            g_active_ctx = cur;
            g_trampoline_fn = saved_tf;
            const uint32_t m = (uint32_t)cc.gpr[3];
            fprintf(stderr, "[spawn] idx=%u #%u r4=%08X -> moby %08X class %08X obj %08X at %.1f %.1f %.1f (hero %.1f %.1f %.1f)\n", idx, k,
                    force_type ? typ : g_tod_spawn_arg4[idx], m, gp(m) ? vm_read32(m + 0x54) : 0, gp(m) ? vm_read32(m + 0x40) : 0, x, y, z, hx, hy, hz);
            if (gp(m)) xreg(m, vm_read32(m + 0x64));
        }
        return true;
    }
    if (c == "armor" || !c.compare(0, 6, "armor=")) {   /* diagnostic (11/11): armor table + protection value; armor=N equips armor N (func_001E2120) */
        const uint32_t ent = entity_of(h), obj = ent ? vm_read32(ent + 0x40) : 0;
        if (!gp(obj)) return false;
        const uint32_t G = vm_read32(kG_slot), pd = vm_read32(obj + 0x1A68);
        const uint32_t tb = vm_read32(0x0088FF38u + 0x55CC);   /* main TOC (OPD 0x859418) */
        if (c.size() > 6) {
            const long n = strtol(c.c_str() + 6, 0, 10);
            /* what func_001E2120 (equip armor) does: index, owned flag, protection = table[n]+0x8 */
            if (n >= 0 && n <= 4 && gp(pd) && gp(tb)) {
                vm_write32(pd + 0x458, (uint32_t)n);
                vm_write32(pd + 0x440 + (uint32_t)n * 4 + 4, 1);
                vm_write32(obj + 0x1798, vm_read32(tb + 0x11C20 + (uint32_t)n * 40 + 8));
            }
        }
        fprintf(stderr, "[armor-dbg] obj=%08X [obj+1A68]=%08X G=%08X idx=%u prot(obj+1798)=%g table=%08X\n", obj, pd, G,
                gp(pd) ? vm_read32(pd + 0x458) : 99u, rf(obj + 0x1798), tb);
        if (gp(tb)) for (unsigned i = 0; i < 5; i++) {
            const uint32_t e = tb + 0x11C20 + i * 40;
            fprintf(stderr, "[armor-dbg]   armor %u @%08X:", i, e);
            for (unsigned k = 0; k < 40; k += 4) fprintf(stderr, " %08X(%g)", vm_read32(e + k), rf(e + k));
            fprintf(stderr, "\n");
        }
        return true;
    }
    if (c == "wdesc" || !c.compare(0, 7, "wflags=") || !c.compare(0, 8, "wlevels=") || !c.compare(0, 5, "wset=")) {   /* diagnostic (10/11): weapon type descriptors (func_00462F38) */
        if (c == "wdesc") {
            for (unsigned idx = 0; idx < 31; idx++) {
                const uint32_t d = (uint32_t)gcall(func_00462F38, idx, 0);
                if (!d) continue;
                char line[12000]; int n = snprintf(line, sizeof line, "[wdesc] %u desc %08X:", idx, d);
                for (uint32_t o = 0; o < 0x80; o += 4) n += snprintf(line + n, sizeof line - n, " %08X", vm_read32(d + o));
                fprintf(stderr, "%s\n", line);
                const uint32_t b = vm_read32(d + 0x48);
                if (b >= 0x10000 && b < 0xF0000000u) {
                    n = snprintf(line, sizeof line, "[wdesc] %u blk %08X +460=%08X:", idx, b, vm_read32(b + 0x460));
                    for (uint32_t o = 0; o < 0x6B0 && n < (int)sizeof line - 12; o += 4) n += snprintf(line + n, sizeof line - n, " %08X", vm_read32(b + o));
                    fprintf(stderr, "%s\n", line);
                }
            }
            /* 11/11: player's weapon slots (G + s*20: +0xC = raritanium mods bitmask, +0x11 level, +4 XP) */
            if (const uint32_t G = vm_read32(kG_slot)) if (gp(G)) for (unsigned s2 = 0; s2 < 32; s2++)
                fprintf(stderr, "[wslot] %u w0=%08X xp=%g w8=%08X mods=%08X w10=%08X lvl=%u\n", s2, vm_read32(G + s2 * 20), rf(G + s2 * 20 + 4),
                        vm_read32(G + s2 * 20 + 8), vm_read32(G + s2 * 20 + 0xC), vm_read32(G + s2 * 20 + 0x10), vm_read8(G + s2 * 20 + 0x11));
        } else if (!c.compare(0, 5, "wset=")) {   /* wset=ID:OFF:VAL writes u32 [desc+OFF] (test, 11/11: Zurkon's model ids) */
            unsigned id = 0, off = 0, v = 0;
            if (sscanf(c.c_str() + 5, "%u:%u:%u", &id, &off, &v) == 3 && id < 31 && off < 0x80 && !(off & 3)) {
                const uint32_t d = (uint32_t)gcall(func_00462F38, id, 0);
                if (d) { const uint32_t old = vm_read32(d + off); vm_write32(d + off, v);
                         fprintf(stderr, "[cheat] wset %u: [%08X+%X] %08X -> %08X\n", id, d, off, old, v); }
            }
        } else if (!c.compare(0, 8, "wlevels=")) {   /* wlevels=ID:N: give a 1-level type N extra levels (test) */
            unsigned id = 0, nl = 0;
            if (sscanf(c.c_str() + 8, "%u:%u", &id, &nl) == 2 && id < 31 && nl >= 1 && nl <= 9) {
                const uint32_t d = (uint32_t)gcall(func_00462F38, id, 0), b = d ? vm_read32(d + 0x48) : 0;
                if (b >= 0x10000 && b < 0xF0000000u) {
                    static const float thr[9] = { 1000, 2200, 3640, 5370, 5500, 20000, 37400, 58300, 83300 };   /* Combuster / Grenade XP */
                    for (uint32_t f = 0; f < 0x460; f += 0x50)   /* 14 tables of 20 words, entry n = level n */
                        for (unsigned n = 1; n <= nl; n++) {
                            const uint32_t at = b + f + n * 4;
                            if (vm_read32(at)) continue;
                            float v;
                            if (f == 0x230) v = thr[n - 1];   /* XP to reach level n (entry 0 stays 0) */
                            else { uint32_t w = vm_read32(b + f); memcpy(&v, &w, 4); if (f == 0) for (unsigned k = 0; k < n; k++) v *= 1.15f; }
                            uint32_t w; memcpy(&w, &v, 4); vm_write32(at, w);
                        }
                    vm_write32(b + 0x460, nl);
                    fprintf(stderr, "[cheat] wlevels %u: blk %08X levels %u\n", id, b, nl);
                }
            }
        } else if (!c.compare(0, 7, "wflags=")) {   /* wflags=ID:VAL writes the low 2 bits of u32 [desc+0x18] (1 = gadget, 2 = device, 0 = weapon) */
            unsigned id = 0, v = 0;
            if (sscanf(c.c_str() + 7, "%u:%u", &id, &v) == 2 && id < 31) {
                const uint32_t d = (uint32_t)gcall(func_00462F38, id, 0);
                if (d) { const uint32_t old = vm_read32(d + 0x18); vm_write32(d + 0x18, (old & ~3u) | (v & 3u));
                         fprintf(stderr, "[cheat] wflags %u: [%08X+18] %08X -> %08X\n", id, d, old, vm_read32(d + 0x18)); }
            }
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
    if (c == "refill") { s_refill = true; return; }
    if (!c.compare(0, 5, "xlua=")) {   /* 12/11: Lua on the last spawn= clone: every "$h" -> moby_handle:new(<its handle>) (no spaces in
                                       * cheat-file commands: use ';' between statements). E.g. xlua=crushto_set_dais($h,some_volume) */
        if (!s_xwatch_n || t - s_xwatch_t < 3.0) { s_xlua_wait.push_back(c); return; }   /* run 3 s after the clone exists */
        char hb[48]; snprintf(hb, sizeof hb, "moby_handle:new(%u)", s_xwatch[s_xwatch_n - 1][1]);
        std::string code = c.substr(5);
        /* the cheat file splits commands at , ; space and lowercases them: write ^ for ',' and # for ' ' */
        for (auto& ch : code) { if (ch == '^') ch = ','; else if (ch == '#') ch = ' '; }
        for (size_t p; (p = code.find("$h")) != std::string::npos;) code.replace(p, 2, hb);
        fprintf(stderr, "[xlua] %s%c", code.c_str(), 10);
        tod_lua_enqueue(code.c_str());
        return;
    }
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
        c == "armor" || !c.compare(0, 6, "armor=") || !c.compare(0, 6, "spawn=") || !c.compare(0, 6, "edump=") || !c.compare(0, 8, "memfind=") || !c.compare(0, 7, "import=") || !c.compare(0, 7, "xclass=") || !c.compare(0, 5, "poke=") || !c.compare(0, 7, "rwatch=") || !c.compare(0, 8, "rwatchx=") || !c.compare(0, 8, "wwatchx=") || !c.compare(0, 7, "fxdump=") || !c.compare(0, 9, "fximport=") || c == "rwatchdump" || !c.compare(0, 8, "memsave=") ||
        !c.compare(0, 6, "cheat=") || !c.compare(0, 6, "bolts=") || !c.compare(0, 6, "equip=") || !c.compare(0, 8, "dsprobe=") ||
        c == "wdesc" || !c.compare(0, 7, "wflags=") || !c.compare(0, 8, "wlevels=") || !c.compare(0, 5, "wset=")) { s_pending.push_back(c); return; }
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

extern "C" int tod_lua_exec_L(uint32_t L, const char* src, uint32_t arg_tv) { return lua_exec_L(L, src, arg_tv); }   /* quit125: see lua_exec_L */
extern "C" int tod_lua_exec_L_quiet(uint32_t L, const char* src, uint32_t arg_tv) { s_exec_quiet = 1; const int r = lua_exec_L(L, src, arg_tv); s_exec_quiet = 0; return r; }   /* alert10: every frame, no log line */

/* Shared with tod_bot.cpp: the hero handle validated the way func_00276D18 does (0 = none). */
extern "C" uint32_t tod_hero_handle(void) { return hero_handle(); }
/* The hero's game object ([entity+0x40]), 0 without a valid hero (tod_dualsense.cpp reads the equipped weapon from it). */
extern "C" uint32_t tod_hero_object(void)
{
    const uint32_t ent = entity_of(hero_handle());
    const uint32_t obj = ent ? vm_read32(ent + 0x40) : 0;
    return gp(obj) ? obj : 0;
}

/* combat_log.cpp (10/11): a guest call on this thread, and the hero's entity (0 = none). */
extern "C" uint64_t tod_gcall(void (*fn)(ppu_context*), uint64_t a0, uint64_t a1) { return gcall(fn, a0, a1); }
extern "C" uint32_t tod_hero_entity(void) { return entity_of(hero_handle()); }
/* gcall for a function of a module with its own TOC (the UI natives' functions: r2 = 0x88FF38). */
extern "C" uint64_t tod_gcall_toc(void (*fn)(ppu_context*), uint32_t toc, uint64_t a0, uint64_t a1)
{
    ppu_context* cur = g_active_ctx;
    ppu_context c = *cur;
    const uint32_t sp = ((uint32_t)cur->gpr[1] - 0x1000u) & ~0xFu;
    vm_write64(sp, (uint32_t)cur->gpr[1]);
    c.gpr[1] = sp; c.gpr[2] = toc; c.gpr[3] = a0; c.gpr[4] = a1; c.lr = 0; c.cr = 0;
    void (*saved_tf)(void*) = g_trampoline_fn;
    g_trampoline_fn = nullptr;
    g_active_ctx = &c;
    fn(&c);
    while (g_trampoline_fn) { void (*tf)(void*) = g_trampoline_fn; g_trampoline_fn = nullptr; tf(&c); }
    g_active_ctx = cur;
    g_trampoline_fn = saved_tf;
    return c.gpr[3];
}
extern "C" int tod_guest_ok(void) { return guest_ok() ? 1 : 0; }
extern "C" uint32_t tod_ent_table(uint32_t* count) { const uint32_t T = vm_read32(kEntTab_slot); if (!gp(T)) return 0; *count = vm_read32(T + 0x10); const uint32_t b = vm_read32(T); return gp(b) ? b : 0; }

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
    if (ready && s_xwatch_n && !s_xlua_wait.empty() && t - s_xwatch_t >= 3.0) {
        std::vector<std::string> w; w.swap(s_xlua_wait);
        for (const auto& c : w) command(c, t);
    }
    if (ready && s_xwatch_n) {   /* 12/11: foreign (xclass) enemies spawned with spawn=: position, distance to Ratchet, alive, every 1 s */
        static double nw = 0;
        if (t >= nw) { nw = t + 1.0;
            const uint32_t pe = player_ent(nullptr);
            for (uint32_t i = 0; i < s_xwatch_n; i++) {
                const uint32_t e = s_xwatch[i][0], hh = s_xwatch[i][1];
                const bool live = vm_read32(e + 0x64) == hh;
                const float x = rf(e + 0x30), y = rf(e + 0x34), z = rf(e + 0x38);
                const float dx = pe ? x - rf(pe + 0x30) : 0, dy = pe ? y - rf(pe + 0x34) : 0, dz = pe ? z - rf(pe + 0x38) : 0;
                fprintf(stderr, "[xwatch] t=%.0f slot %u handle %08X %s pos %.1f %.1f %.1f dist %.1f%c", t, s_xwatch[i][2], hh, live ? "live" : "GONE",
                        x, y, z, sqrtf(dx * dx + dy * dy + dz * dz), 10);
            }
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
    if (ready && s_refill && !s_god && guest_ok()) {   /* 12/11: hits still land (combat log sees who hit), health refilled 0.2 s later */
        if (const uint32_t hc = health_comp(h, t)) {
            static double low_since = -1;
            const float cur = rf(hc + 4), mx = rf(hc + 8);
            if (isfinite(mx) && mx > 0 && cur < mx) { if (low_since < 0) low_since = t; else if (t - low_since >= 0.2) { wf(hc + 4, mx); low_since = -1; } }
            else low_since = -1;
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
