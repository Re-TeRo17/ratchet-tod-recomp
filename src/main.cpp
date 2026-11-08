/*
 * Ratchet & Clank: Tools of Destruction -- recompiled: the runner.
 *
 * Minimal boot path, the same as dod3-recomp/src/main.cpp and the SDK's
 * templates/project/main.cpp: allocate the flat guest VM, load the EBOOT's ELF,
 * register the lifted function table and the HLE handlers, start the frame
 * clock, dispatch the entry OPD. The scaffold (ppu_load_elf, ppu_recomp_register,
 * ppu_hle_init, ppu_sysprx_register, ppu_fs_register, lv2_init_syscalls, ppu_run)
 * is ps3recomp's; no installer, no settings, no title patches yet.
 *
 * Usage:  tod.exe [path\to\EBOOT.ELF]       (default: elf/EBOOT.ELF)
 * Env:    PS3_VFS_ROOT (default game/disc), PS3_TITLE (see TODO below).
 */

#include <atomic>
#include <mutex>
#include "ppu_recomp.h"                 /* the lifter's: ppu_context and the function table */
#include "ps3emu/vm_watch.h"             /* texture write-watch (vm_watch_init, vm_commit_reserved) */
#include "ps3emu/beacon.h"               /* walker / presenter phase beacons */
#include "rsx_logv2.h"                   /* logv2: the structured second log (stall events) */
#include "win32_compat.h"                /* Sleep, CreateThread, VirtualAlloc... (<windows.h> on Windows) */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <signal.h>
#include <stdarg.h>
#include <exception>
#include <filesystem>
#include "settings.h"                    /* tod.ini */
#include "install/install.h"             /* --install / --check */
#include "update/update_check.h"         /* TOD_RELEASE_VERSION, the update check */

#ifdef _WIN32
#include <timeapi.h>                     /* timeBeginPeriod */
#include <intrin.h>                      /* __cpuidex, _xgetbv */
#include <direct.h>
#include <dbghelp.h>                     /* beacon stack dump (stall_monitor) */

/* PPU_HINT_PAUSE=<n>: the guest's priority hints (`or r31,r31,r31`, its delay/spin loops) execute n x
 * _mm_pause (read by recompiled/ppu_recomp_002.cpp after tools/patch_ppu_hints.py). Default 0 = off. */
extern "C" int g_ppu_hint_pause = [] { const char* e = getenv("PPU_HINT_PAUSE"); return e ? atoi(e) : 0; }();
#define mkdir(path, mode) _mkdir(path)
#endif

/* ---------------------------------------------------------------------------
 * TODO(tod): title-specific defaults. Nothing below is verified against the
 * game yet; these are placeholders so the first boot has a window title and a
 * disc root. Replace once the real values are known.
 * -----------------------------------------------------------------------*/
#define TOD_DEFAULT_TITLE     "Ratchet & Clank: Tools of Destruction"   /* TODO: window title */
#define TOD_DEFAULT_VFS_ROOT  "game/disc"                              /* TODO: disc root (holds PS3_GAME/) */
#define TOD_DEFAULT_ELF       "elf/EBOOT.ELF"                          /* decrypted EBOOT (tools/self2elf.py) */

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file (declarations as in the template)
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     cellGame_init_from_paramsfo(const char* sfo_path);   /* libs/system/cellGame.c */
void     tod_overlay_init(void);              /* src/ui/tod_overlay.cpp: F1 / SELECT held settings page */
void     tod_input_init(void);                /* src/input/host_input.cpp */
void     tod_prompts_init(void);              /* src/patches/prompts.cpp */
void     tod_level_streams_init(void);        /* src/patches/level_streams.cpp */
void     tod_mods_init(void);                 /* src/patches/mods.cpp: mods\ folder (TOD_MODS) */
void     tod_discord_start(void);             /* src/discord/discord_rpc.cpp: Rich Presence thread */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* src/stubs/missing_hle.cpp: the port's own HLE handlers; the toolkit's
 * ppu_hle_init runs after it, so these win. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

uint8_t* vm_base = nullptr;                   /* host symbol the runtime links against */
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

extern const char* g_hle_inflight[];                /* ppu_hle.cpp: the HLE each guest tid is inside (64 slots) */
void     ppu_report_guest_lrs(void);               /* ppu_loader.cpp: every thread's lr/r3/sp */
void     ppu_report_guest_chains(int depth);        /* ppu_loader.cpp: lr/cia + back-chain return addresses */
extern "C" void (*g_ppu_chain_hook)(unsigned tid, const uint64_t* gpr, uint64_t lr, uint64_t ctr, uint32_t cia);   /* frz107: per-thread extra dump in the [stall] probe */
extern "C" void anim_cap_init(void);                   /* capture_patch.c: reads the SPU_HLE_WID0 / ANIM_* environment (frz107: called from main after tod.ini) */
extern "C" void anim_hle_dump_stall(void);        /* anim_hle.c: the SPU anim consumer (wid0) state */
extern "C" void spurs_policy_dump_stall(void);    /* spurs_policy.c: resident policy-module contexts (image_id, pc, reservation) */
extern uint32_t ppu_hle_inject_base;                /* cellGcmSys.c: the GCM window (labels, put/get/ref) */
extern void (*g_gcm_fifo_kick_hook)(void);          /* cellGcmSys.c: ring-full recycle wakes the walker */
extern int  (*g_lv2_usleep_hook)(uint32_t lr, uint64_t usec);   /* sys_timer.c: every guest usleep */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);
unsigned cellGcm_flip_request_count(void);
}

/* RSX present backend: D3D12 on Windows, null (headless) elsewhere. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" void rsx_draw_engine_trace_after(long after);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void rsx_host_window_escape_closes(int on);   /* rsx_host_mouse.h */

/* TOD_FNTRACE=1: entry log of the functions patched by tools/fntrace_patch.py
 * (first 25 calls of each, then every 2000th). */
static long long s_syn_sleep_q, s_syn_sleep_us;   /* QPC at the synth tick's usleep and what it asked (tod_usleep_pre) */
extern "C" unsigned g_tod_hero_handle = 0;   /* tod_bot.cpp: the hero, from the level's set_follow_cam */
extern "C" unsigned ppu_boot_frames_presented(void);

/* AUDIO_TICK_DIAG=<ms> (aud104; 1 = threshold 25 ms): per synth tick (func_006F17EC, called from syn_tick_timer_thread at lr 0x6F1BA0; the
 * loop is: tick; usleep(1e6/rate); repeat). Independent of TOD_FNTRACE (the older block in tod_fntrace needs it, which is why [syn-late] never
 * printed in aud103). One line per tick whose period (entry to entry) exceeds the threshold, splitting it into
 *   slept   = tod_usleep_pre -> next tick entry (over = slept - asked: the OS wake-up delay),
 *   work    = previous tick entry -> its usleep, with cpu = the thread's own CPU time in it (work - cpu = not running: preempted or blocked),
 *   blk     = lv2 syscalls + HLE calls the thread made inside the work (count, sum, longest with kind / number-or-NID / guest lr),
 * and the guest state (rate = what the previous tick asked: 60 normal, 240 = job still running; inflight; producer lag). Plus a 5 s summary line. */
extern "C" volatile unsigned g_blk_diag_tid;
extern "C" volatile unsigned g_syn_boost_tid;
extern "C" volatile double g_blk_diag_cpus;
extern "C" void (*g_blk_diag_fn)(unsigned kind, unsigned num, uint32_t lr, long long q0, long long q1);
struct SynBlk { double sum_ms, max_ms, hle_ms; unsigned n, max_kind, max_num; uint32_t max_lr; };
static SynBlk s_synblk;                 /* written by the synth thread only */
static long long s_syn_sleep_cyc;       /* the thread's cycle counter at its usleep */
static int s_syn_diag_on = -1;
static void syn_blk_note(unsigned kind, unsigned num, uint32_t lr, long long q0, long long q1)
{
    if (kind == 0 && lr == 0x006F1BA0u && (num == 141 || num == 142)) return;   /* the tick's own sleep */
    static double f = 0; if (f == 0) { LARGE_INTEGER x; QueryPerformanceFrequency(&x); f = (double)x.QuadPart; }
    const double ms = (q1 - q0) * 1000.0 / f;
    SynBlk& b = s_synblk; b.n++; b.sum_ms += ms; if (kind) b.hle_ms += ms;
    if (ms > b.max_ms) { b.max_ms = ms; b.max_kind = kind; b.max_num = num; b.max_lr = lr; }
}
static void syn_tick_diag(ppu_context* ctx)
{
    static double thr = 25.0;
    if (s_syn_diag_on < 0) {
        const char* e = getenv("AUDIO_TICK_DIAG");
        const int on = (e && e[0] && e[0] != '0') ? 1 : 0;
        const double v = e ? atof(e) : 0; if (v > 1.0) thr = v;
        s_syn_diag_on = on;
    }
    { /* AUDIO_SYN_LOCK_BOOST=1 (aud104, default off): the guest lwmutexes this thread waits on are held at THREAD_PRIORITY_HIGHEST by their holders
       * (priority inheritance), see ppu_sysprx.cpp lwmd_taken. */
      static int bst = -1;
      if (bst < 0) { const char* e = getenv("AUDIO_SYN_LOCK_BOOST"); bst = (e && e[0] == '1') ? 1 : 0; }
      if (bst > 0 && (uint32_t)ctx->lr == 0x006F1BA0u && g_syn_boost_tid == 0) { g_syn_boost_tid = (unsigned)ctx->thread_id; fprintf(stderr, "[syn-diag] lock boost on for thread %u\n", g_syn_boost_tid); } }
    if (s_syn_diag_on <= 0 || (uint32_t)ctx->lr != 0x006F1BA0u) return;
    static double f = 0, cpus = 1;          /* QPC ticks/s, thread cycles per us */
    static long long tl = 0, tcyc = 0;      /* previous tick entry */
    static unsigned nt = 0, hov[5], hwk[5]; static double t5 = 0, dtmax = 0, ovmax = 0, wkmax = 0, cpumax = 0, blkmax = 0;
    LARGE_INTEGER q; ULONG64 cyc; QueryPerformanceCounter(&q); QueryThreadCycleTime(GetCurrentThread(), &cyc);
    if (f == 0) {
        LARGE_INTEGER x; QueryPerformanceFrequency(&x); f = (double)x.QuadPart;
        LARGE_INTEGER a, b; ULONG64 c0, c1;          /* 4 ms spin once: cycles per us of this thread's own counter */
        QueryPerformanceCounter(&a); QueryThreadCycleTime(GetCurrentThread(), &c0);
        do { QueryPerformanceCounter(&b); } while ((b.QuadPart - a.QuadPart) * 1000.0 < 4.0 * f);
        QueryThreadCycleTime(GetCurrentThread(), &c1);
        cpus = (double)(c1 - c0) / ((b.QuadPart - a.QuadPart) * 1e6 / f);
        g_blk_diag_cpus = cpus; g_blk_diag_fn = syn_blk_note; g_blk_diag_tid = (unsigned)ctx->thread_id;
        fprintf(stderr, "[syn-diag] on: thread %u, %.0f thread cycles/us, late threshold %.0f ms\n", (unsigned)ctx->thread_id, cpus, thr);
        QueryPerformanceCounter(&q); QueryThreadCycleTime(GetCurrentThread(), &cyc);
        tl = q.QuadPart; tcyc = (long long)cyc; s_synblk = SynBlk{}; return;
    }
    const double dt = (q.QuadPart - tl) * 1000.0 / f;
    const bool sl = s_syn_sleep_q > tl;
    const double work = sl ? (s_syn_sleep_q - tl) * 1000.0 / f : -1, slept = sl ? (q.QuadPart - s_syn_sleep_q) * 1000.0 / f : -1;
    const double asked = s_syn_sleep_us / 1000.0, over = sl ? slept - asked : -1;
    const double cpu = sl ? (s_syn_sleep_cyc - tcyc) / cpus / 1000.0 : -1;
    const SynBlk bl = s_synblk; s_synblk = SynBlk{};
    nt++; if (dt > dtmax) dtmax = dt; if (over > ovmax) ovmax = over; if (work > wkmax) wkmax = work; if (cpu > cpumax) cpumax = cpu; if (bl.sum_ms > blkmax) blkmax = bl.sum_ms;
    if (sl) {
        hov[over < 1 ? 0 : over < 2 ? 1 : over < 5 ? 2 : over < 10 ? 3 : 4]++;
        hwk[work < 1 ? 0 : work < 2 ? 1 : work < 5 ? 2 : work < 10 ? 3 : 4]++;
    }
    const double tms = q.QuadPart * 1000.0 / f;
    if (dt > thr) {
        const unsigned toc = (unsigned)ctx->gpr[2];
        auto G = [&](int off) { return vm_read32(vm_read32(toc + off)); };
        const unsigned ri = vm_read32(vm_read32(vm_read32(toc - 0x4994)) + 4) & 7, w = G(-0x497C), rate = G(-0x4970), fl = vm_read8(vm_read32(toc - 0x48EC));
        fprintf(stderr, "[syn-late] qpc %.1f ms: dt=%.1f | slept=%.1f asked=%.1f over=%.1f | work=%.1f cpu=%.1f notrunning=%.1f | blk n=%u sum=%.1f max=%.1f (%s %u lr=%08X) hle=%.1f | rate=%u inflight=%u lag=%u frame=%u\n",
                tms, dt, slept, asked, over, work, cpu, work - cpu, bl.n, bl.sum_ms, bl.max_ms, bl.max_kind ? "hle" : "sc", bl.max_num, bl.max_lr, bl.hle_ms,
                rate, fl, (ri - w) & 7, ppu_boot_frames_presented());
    }
    if (tms - t5 >= 5000.0) {
        if (t5) fprintf(stderr, "[syn-diag] 5s: %u ticks, dt max %.1f, over max %.1f, work max %.1f, cpu max %.1f, blk sum max %.1f ms; over hist <1,<2,<5,<10,more: %u %u %u %u %u; work hist: %u %u %u %u %u\n",
                        nt, dtmax, ovmax, wkmax, cpumax, blkmax, hov[0], hov[1], hov[2], hov[3], hov[4], hwk[0], hwk[1], hwk[2], hwk[3], hwk[4]);
        t5 = tms; nt = 0; dtmax = ovmax = wkmax = cpumax = blkmax = 0; memset(hov, 0, sizeof hov); memset(hwk, 0, sizeof hwk);
    }
    /* printing can block on stderr for ms: keep it out of the next tick's work */
    QueryPerformanceCounter(&q); QueryThreadCycleTime(GetCurrentThread(), &cyc);
    tl = q.QuadPart; tcyc = (long long)cyc;
}
extern "C" void tod_fntrace(unsigned addr, ppu_context* ctx)
{
    if (addr == 0x0028BD60) {   /* set_follow_cam(handle, ...): always, the bot needs it */
        if ((unsigned)ctx->gpr[3] != g_tod_hero_handle)
            fprintf(stderr, "[bot] set_follow_cam handle=%08X\n", (unsigned)ctx->gpr[3]);
        g_tod_hero_handle = (unsigned)ctx->gpr[3];
        return;
    }
    if (addr == 0x006F17EC) {   /* TOD_SND49_FIX (default ON): REALTIME_MOVIE (group 49) cue voices (645C48: +0x5C=10 = no auto-end, vtbl 855E00 +0x34 stub
                                 * returns 0) only end by an explicit stop (5B2150 -> flags+0x64 |= 4) that never arrives with -no_movies: the voice keeps the
                                 * ducking rule 0 (x0.45 on all SFX groups) forever. After TOD_SND49_SECS (default 120) in st=4, set the kill flag so
                                 * 5B9850 goes 4->7->0 and the REAPER (5B29B8) frees it. */
        static int fix = -1; static double secs = 120.0;
        if (fix < 0) {
            const char* e = getenv("TOD_SND49_FIX"); fix = (e && e[0] == '0') ? 0 : 1;
            if (const char* s = getenv("TOD_SND49_SECS")) secs = atof(s);
        }
        if (fix) {
            static unsigned seen = 0; static double since = 0, last = 0;
            const double tn = (double)GetTickCount64() / 1000.0;
            if (tn - last >= 1.0) {
                last = tn;
                const unsigned base = vm_read32(vm_read32(0x008A80C8));
                const unsigned lst = base ? vm_read32(base + 49 * 0xB0 + 0x20) : 0;
                unsigned cur = 0;
                for (unsigned s = 0; lst && s < 16; s++) {
                    const unsigned v = vm_read32(lst + s * 4);
                    if (v && vm_read16(v + 0x5A) == 4 && !(vm_read32(v + 0x64) & 4)) { cur = v; break; }
                }
                if (cur != seen) { seen = cur; since = tn; }
                else if (cur && tn - since >= secs) {
                    vm_write32(cur + 0x64, vm_read32(cur + 0x64) | 4);
                    fprintf(stderr, "[sndfix] t=%.3f group49 voice=%08X id=%08X in st=4 for %.0fs -> kill flag (+0x64|=4)\n", tn, cur, vm_read32(cur + 0x8C), tn - since);
                    seen = 0;
                }
            }
        }
    }
    if (addr == 0x005E9150 || addr == 0x005F4198 || addr == 0x005E8A78 || addr == 0x005E5C08) {
        /* TOD_FXLOG=1: outcome of every FX spawn, per effect id (notes: fx_spawn_hook.md). The core 5E9150
         * calls the cull/LOD test 5F4198, then the pool alloc 5E8A78, then 5E5C08 only on success, all on
         * the caller's thread: a spawn whose next stage never comes was dropped at that stage. "lod" = culled
         * with CFG+0x70C > def+0x99 (detail level too low), "cull" = the frustum / distance tests or FX off.
         * Logs each id's first spawn with its caller, then every ~2 s the ids that changed. */
        /* TOD_FX_DETAIL=<0..2>: pin the FX level CFG+0x70C (written once a frame by 5EF498 from
         * the CPU time the FX code took vs a budget) before the cull test reads it (notes:
         * fx_quality.md). OFF by default: pinned to 0 (22/10) nearly every weapon effect
         * vanished -- beams, trails, the Groovitron's lights -- so 0 is not "full detail". */
        static int pin = -2;
        if (pin == -2) { const char* e = getenv("TOD_FX_DETAIL"); pin = e ? atoi(e) : -1; }
        if (addr == 0x005F4198 && pin >= 0) {
            const unsigned cfg = vm_read32(0x106EE7FC);
            if (cfg >= 0x10000000u && cfg < 0x40000000u) vm_write32(cfg + 0x70C, (unsigned)pin);
        }
        static int fx = -1; if (fx < 0) fx = getenv("TOD_FXLOG") ? 1 : 0;
        if (!fx) return;
        const unsigned G = 0x106EE368, NID = 4096;
        static std::atomic<unsigned> c_ok[NID], c_lod[NID], c_cull[NID], c_alloc[NID], c_seen[NID];
        static thread_local unsigned stage = 0, cur = 0, lod = 0;
        static std::atomic<unsigned> detail{ ~0u };
        auto settle = [&]() {   /* the pending spawn of this thread never reached the next stage */
            if (stage == 1) (lod ? c_lod : c_cull)[cur]++;
            else if (stage == 2) c_alloc[cur]++;
            stage = 0;
        };
        if (addr == 0x005E9150) {
            settle();
            cur = (unsigned)ctx->gpr[3] & 0xFFFF; lod = 0;
            if (cur >= NID) return;
            stage = 1;
            if (c_seen[cur]++ == 0) {
                const unsigned p = (unsigned)ctx->gpr[5], ent = vm_read32(G + 0x4EC) + cur * 0x24;
                auto F = [](unsigned a) { unsigned u = vm_read32(a); float f; memcpy(&f, &u, 4); return (double)f; };
                fprintf(stderr, "[fx] t=%.3f new id=%u/%u def=%08X lr=%08X pos=(%.1f %.1f %.1f) live=%u\n",
                        (double)GetTickCount64() / 1000.0, cur, vm_read32(G + 0x490), vm_read32(ent), (unsigned)ctx->lr,
                        F(p), F(p + 4), F(p + 8), vm_read32(G + 0x4DC));
            }
        } else if (!stage) {
            return;
        } else if (addr == 0x005F4198) {   /* r4 = def */
            const unsigned cfg = vm_read32(0x106EE7FC);
            if (cfg >= 0x10000000u && cfg < 0x40000000u) {
                const unsigned d = vm_read32(cfg + 0x70C);
                detail = d;
                lod = d > vm_read8((unsigned)ctx->gpr[4] + 0x99);
            }
        } else if (addr == 0x005E8A78) {
            stage = 2;
        } else {
            c_ok[cur]++; stage = 0;
        }
        static std::mutex m; static double last = 0; static unsigned p_ok[NID], p_lod[NID], p_cull[NID], p_alloc[NID];
        const double tn = (double)GetTickCount64() / 1000.0;
        if (tn - last >= 2.0 && m.try_lock()) {
            if (tn - last >= 2.0) {
                last = tn;
                std::string s;
                char b[96];
                for (unsigned i = 0; i < NID; i++) {
                    const unsigned o = c_ok[i] - p_ok[i], l = c_lod[i] - p_lod[i], c = c_cull[i] - p_cull[i], a = c_alloc[i] - p_alloc[i];
                    if (!(o | l | c | a)) continue;
                    p_ok[i] += o; p_lod[i] += l; p_cull[i] += c; p_alloc[i] += a;
                    snprintf(b, sizeof b, " %u:", i); s += b;
                    if (o) { snprintf(b, sizeof b, "ok%u", o); s += b; }
                    if (l) { snprintf(b, sizeof b, "%sLOD%u", o ? "," : "", l); s += b; }
                    if (c) { snprintf(b, sizeof b, "%scull%u", (o | l) ? "," : "", c); s += b; }
                    if (a) { snprintf(b, sizeof b, "%sALLOC%u", (o | l | c) ? "," : "", a); s += b; }
                }
                if (!s.empty())
                    fprintf(stderr, "[fx] t=%.3f detail=%d live=%u off=%u |%s\n", tn, (int)detail.load(),
                            vm_read32(G + 0x4DC), vm_read8(G), s.c_str());
            }
            m.unlock();
        }
        return;
    }
    if (addr == 0x006F17EC) syn_tick_diag(ctx);
    static int on = -1;
    if (on < 0) on = getenv("TOD_FNTRACE") ? 1 : 0;
    if (!on) return;
    static std::atomic<unsigned> cnt[64]; static std::atomic<unsigned> ids[64];
    unsigned k = 0;
    for (; k < 64; k++) {
        unsigned v = ids[k].load();
        if (v == addr) break;
        if (v == 0 && ids[k].compare_exchange_strong(v, addr)) break;
        if (ids[k].load() == addr) break;
    }
    if (k >= 64) return;
    unsigned n = ++cnt[k];
    double t = (double)GetTickCount64()/1000.0;
    unsigned r3 = (unsigned)ctx->gpr[3];
    auto F = [](unsigned a) { unsigned u = vm_read32(a); float f; memcpy(&f, &u, 4); return (double)f; };
    if (addr == 0x005AD700 || addr == 0x005AD7A8) {   /* voice add/remove to its group's list (voice+0x20 = group idx): group 49 REALTIME_MOVIE only */
        const unsigned w20 = vm_read32(r3 + 0x20);
        if (w20 == 49 || (w20 >> 16) == 49 || (w20 >> 24) == 49 || (w20 & 0xFF) == 49) {
            static int lines = 0;
            if (lines++ < 100) {
                fprintf(stderr, "[sndvoice] %s t=%.3f voice=%08X w20=%08X lr=%08X flags64=%08X words:", addr == 0x5AD700 ? "ADD" : "REMOVE", t, r3, w20, (unsigned)ctx->lr, vm_read32(r3 + 0x64));
                for (int q = 0; q < 16; q++) fprintf(stderr, " %08X", vm_read32(r3 + q * 4));
                fprintf(stderr, "\n");
            }
        }
        return;
    }
    if (addr == 0x005ADB78 || addr == 0x005ADBF8 || addr == 0x005AE0A8 || addr == 0x005AE1C0 || addr == 0x005ADC70 ||
        addr == 0x005ADCB8 || addr == 0x005ADD00 || addr == 0x005AF458 || addr == 0x005AF170 || addr == 0x005AE798 ||
        addr == 0x005AEBE8 || addr == 0x005AED20 || addr == 0x005AF320 || addr == 0x005B4B10 || addr == 0x005AE558) {
        /* SCREAM sound-group API (5ADB78 fade, 5ADBF8 fade from, 5AE0A8/5AE1C0 mask fades, 5ADC70/CB8/D00 set +0/+4/+8,
         * 5AF458/5AF170 solo/unsolo, 5AE798/5AEBE8 mute, 5AED20/5AF320 unmute, 5B4B10 reset, 5AE558 reset fades) */
        if (n <= 400)
            fprintf(stderr, "[sndapi] %08X #%u t=%.3f r3=%08X r4=%08X r5=%08X r6=%08X f1=%g f2=%g f3=%g lr=%08X\n", addr, n, t, r3,
                    (unsigned)ctx->gpr[4], (unsigned)ctx->gpr[5], (unsigned)ctx->gpr[6], ctx->fpr[1], ctx->fpr[2], ctx->fpr[3], (unsigned)ctx->lr);
        return;
    }
    if (addr == 0x002500F8) {   /* sound group fade(group, kind 0 vol/1 lowpass/2 pitch, target, seconds); 277C80 = Lua group_fade_vol */
        fprintf(stderr, "[sndfade] 2500F8 #%u t=%.3f group=%d kind=%d target=%g time=%g lr=%08X\n", n, t, (int)r3, (int)ctx->gpr[4], ctx->fpr[1], ctx->fpr[2], (unsigned)ctx->lr);
        return;
    }
    if (addr == 0x005A78F8) {   /* pad module: r3 = module base (r25) */
        static unsigned last = 0xFFFFFFFF; static int lines = 0;
        r3 = vm_read32((unsigned)ctx->gpr[2] - 0x7EAC); unsigned d2 = vm_read16(r3 + 0x41C + 0x92), d1 = vm_read16(r3 + 0x41C + 0x90);
        unsigned key = (d1 << 16) | d2;
        if ((n <= 3 || key != last) && lines < 120) {
            last = key; lines++;
            fprintf(stderr, "[whud] 5A78F8 #%u t=%.3f r3=%08X lr=%08X d1=%04X d2=%04X press(+9C..)=", n, t, r3, (unsigned)ctx->lr, d1, d2);
            for (int q = 0; q < 6; q++) fprintf(stderr, "%04X ", vm_read16(r3 + 0x41C + 0x94 + q*2));
            fprintf(stderr, " fl[+00..1C]=");
            for (int q = 0; q < 8; q++) fprintf(stderr, "%g ", F(r3 + 0x288 + q*4));
            fprintf(stderr, " fl[+20..4C]=");
            for (int q = 0; q < 12; q++) fprintf(stderr, "%g ", F(r3 + 0x288 + 0x20 + q*4));
            fprintf(stderr, " W[2E0..]=%08X %08X %08X %08X\n", vm_read32(r3+0x2D8), vm_read32(r3+0x2DC), vm_read32(r3+0x2E0), vm_read32(r3+0x2E4));
        }
        return;
    }
    if (addr == 0x00263120) {   /* HUD object G = r3 */
        static unsigned lst = 0xFFFFFFFF; static unsigned l8 = 0;
        unsigned st = vm_read32(r3 + 0x6C); float f8 = (float)F(r3 + 0x3D8);
        unsigned b8; memcpy(&b8, &f8, 4);
        bool chg = (st != lst) || ((b8 == 0) != (l8 == 0));
        if (n <= 3 || n % 30 == 0 || chg) {
            lst = st; l8 = b8;
            fprintf(stderr, "[whud] 263120 #%u t=%.3f G=%08X S=%08X state6C=%u ts70=%g f3D8=%g f3DC=%g b3E0=%u b3E1=%u w3D0=%08X w3D4=%08X lr=%08X\n", n, t, r3, r3 - 0x5AF40, st,
                    F(r3 + 0x70), F(r3 + 0x3D8), F(r3 + 0x3DC), vm_read8(r3 + 0x3E0), vm_read8(r3 + 0x3E1), vm_read32(r3 + 0x3D0), vm_read32(r3 + 0x3D4), (unsigned)ctx->lr);
        }
        return;
    }
    if (addr == 0x0069B590) {
        { unsigned lr_ = (unsigned)ctx->lr;
          if (lr_ == 0x226008 || lr_ == 0x226020) {
            unsigned r4e = (unsigned)ctx->gpr[4]; double p = F(r4e), dt = ctx->fpr[1] * F(r3 + 4);
            static double lastp[2] = {0, 0}; static unsigned cc[2] = {0, 0}; int w = (lr_ == 0x226020);
            cc[w]++;
            if (p > lastp[w] + 0.3) fprintf(stderr, "[whud] HUDT REARM lr=%08X r4=%08X t=%.3f from=%g to=%g dt=%g%c", lr_, r4e, t, lastp[w], p, dt, 10);
            else if (p > 0 && dt > p) fprintf(stderr, "[whud] HUDT EXPIRE lr=%08X r4=%08X t=%.3f was=%g dt=%g%c", lr_, r4e, t, p, dt, 10);
            else if (cc[w] % 25 == 0) fprintf(stderr, "[whud] HUDT val lr=%08X t=%.3f p=%g dt=%g%c", lr_, t, p, dt, 10);
            lastp[w] = p; } }
        if (n <= 20 || n % 600 == 0) {
            unsigned r4 = (unsigned)ctx->gpr[4];
            fprintf(stderr, "[whud] 69B590 #%u t=%.3f f1=%g ts[+4]=%g *p=%g r3=%08X r4=%08X lr=%08X\n", n, t, ctx->fpr[1], F(r3 + 4), F(r4), r3, r4, (unsigned)ctx->lr);
        }
        return;
    }
    if (addr == 0x006F17EC) {   /* every 2 s: SCREAM sound-group fade state (table [[0x8A80C8]], 0xB0 a group;
                                    * 3 fades at +0x50 + kind*24: vol, low-pass, pitch; +0x10 -> live value) */
        static double tg = 0;
        if (t - tg >= 2.0) {
            tg = t;
            const unsigned base = vm_read32(vm_read32(0x008A80C8));
            if (base) {   /* per named group: name eff(+1C) base(+0) f(+C) f(+14) flags(+24) parent(+26) */
                fprintf(stderr, "[sndgrp] t=%.3f", t);
                for (unsigned g = 0; g < 64; g++) {
                    const unsigned gp = base + g * 0xB0;
                    char nm[17]; for (int i = 0; i < 16; i++) { nm[i] = (char)vm_read8(gp + 0x40 + i); if (nm[i] && (nm[i] < 32 || nm[i] > 126)) nm[i] = '?'; } nm[16] = 0;
                    if (!nm[0]) continue;
                    fprintf(stderr, " %u:%s=%.3g(%.3g,%.3g,%.3g,f%X,p%d)", g, nm, F(gp + 0x1C), F(gp), F(gp + 0xC), F(gp + 0x14),
                            vm_read16(gp + 0x24), (int)(short)vm_read16(gp + 0x26));
                }
                fprintf(stderr, "\n");
                {   /* group 49 (REALTIME_MOVIE) voices: state +0x5A (1 pend,2 start,3,4 play,6 pause,0 free), type +0x5E, grp flags +0x64 */
                    const unsigned gp49 = base + 49 * 0xB0, lst = vm_read32(gp49 + 0x20);
                    fprintf(stderr, "[sndvoice49] t=%.3f n=%u", t, vm_read8(gp49 + 0x25));
                    for (unsigned s = 0; lst && s < 16; s++) {
                        const unsigned v = vm_read32(lst + s * 4);
                        if (v) fprintf(stderr, " v%08X(st=%u,ty=%u,vt=%08X,f64=%08X,f68=%08X,id=%08X,w34=%08X)", v, vm_read16(v + 0x5A), vm_read8(v + 0x5E),
                                       vm_read32(v), vm_read32(v + 0x64), vm_read32(v + 0x68), vm_read32(v + 0x8C), vm_read32(v + 0xD0));
                    }
                    fprintf(stderr, "\n");
                }
                /* ducking rules (5AD3E0): table at [0x8A80B4] (+4 count, +8 array of 0x30): +0 duck target, +4 attack s, +8 release s,
                 * +0x10 u64 trigger mask, +0x18 u64 ducked mask, +0x20 unused marker, +0x24 state */
                const unsigned rt = vm_read32(0x008A80B4);
                if (rt) {
                    const unsigned nr = vm_read32(rt + 4), ra = vm_read32(rt + 8);
                    for (unsigned i = 0; i < nr && i < 32; i++) {
                        const unsigned rp = ra + i * 0x30;
                        const unsigned long long tm = ((unsigned long long)vm_read32(rp + 0x10) << 32) | vm_read32(rp + 0x14);
                        const unsigned long long dm = ((unsigned long long)vm_read32(rp + 0x18) << 32) | vm_read32(rp + 0x1C);
                        fprintf(stderr, "[sndduck] t=%.3f rule%u duck=%g att=%g rel=%g unused=%08X state=%u trig=%016llX ducked=%016llX active:", t, i,
                                F(rp), F(rp + 4), F(rp + 8), vm_read32(rp + 0x20), vm_read8(rp + 0x24), tm, dm);
                        for (unsigned g = 0; g < 64; g++) {
                            if (!((tm >> g) & 1)) continue;
                            const unsigned gp = base + g * 0xB0;
                            if (vm_read8(gp + 0x25) && !(vm_read8(gp + 0x24) & 1))
                                fprintf(stderr, " g%u(n=%u,eff=%.3g)", g, vm_read8(gp + 0x25), F(gp + 0x1C));
                        }
                        fprintf(stderr, "\n");
                    }
                }
            }
        }
    }
    if (addr == 0x006F17EC) {   /* NextSynth audio tick (syn_tick_timer_thread): lag of the producer behind the reader */
        const unsigned toc = (unsigned)ctx->gpr[2];
        auto G = [&](int off) { return vm_read32(vm_read32(toc + off)); };
        const unsigned ri = vm_read32(vm_read32(vm_read32(toc - 0x4994)) + 4) & 7, w = G(-0x497C), Nq = G(-0x48E8);
        const unsigned per = G(-0x4970), fl = vm_read8(vm_read32(toc - 0x48EC));
        static LARGE_INTEGER f0; static long long tl = 0; LARGE_INTEGER q; QueryPerformanceCounter(&q);
        if (!f0.QuadPart) QueryPerformanceFrequency(&f0);
        const double dt = tl ? (q.QuadPart - tl) * 1000.0 / f0.QuadPart : 0; tl = q.QuadPart;
        const unsigned lag = (ri - w) & 7;
        static unsigned ticks, skips, lagh[8]; static double t0s = 0, dtmax = 0;
        ticks++; if (per == 0xF0) skips++; lagh[lag]++; if (dt > dtmax) dtmax = dt;
        /* Round trip of a kick: per (set by the previous tick) 0x3C = it kicked, 0xF0 =
         * the job was still running and it re-polls. From a kick to the tick that
         * kicks again: the repolls it took and the time, i.e. job + detection. */
        static long long tkick = 0; static unsigned rp = 0, rph[6]; static double rtsum = 0, rtmax = 0; static unsigned rtn = 0;
        /* The last gap split: work = previous entry -> its usleep, over = slept minus asked. */
        static long long tentry = 0; static double wkmax = 0, ovmax = 0;
        if (tentry && s_syn_sleep_q > tentry) {
            const double wk = (s_syn_sleep_q - tentry) * 1000.0 / f0.QuadPart;
            const double ov = (q.QuadPart - s_syn_sleep_q) * 1000.0 / f0.QuadPart - s_syn_sleep_us / 1000.0;
            if (wk > wkmax) wkmax = wk; if (ov > ovmax) ovmax = ov;
        }
        tentry = q.QuadPart;
        if (per == 0xF0) rp++;
        else {
            if (tkick) { const double rt = (q.QuadPart - tkick) * 1000.0 / f0.QuadPart; rtsum += rt; rtn++; if (rt > rtmax) rtmax = rt; rph[rp < 5 ? rp : 5]++; }
            tkick = q.QuadPart; rp = 0;
        }
        static unsigned lines = 0;
        if ((per == 0xF0 || lag >= 6) && lines < 400) { lines++;
            fprintf(stderr, "[syn] t=%.3f dt=%.2fms ri=%u w=%u lag=%u N=%d inflight=%u period=%u\n", t, dt, ri, w, lag, (int)Nq, fl, per); }
        if (t - t0s >= 2.0) {
            fprintf(stderr, "[syn] 2s: %u ticks, %u after a skip (job not done), dt max %.1f ms, lag hist", ticks, skips, dtmax);
            for (int k = 0; k < 8; k++) fprintf(stderr, " %d:%u", k, lagh[k]);
            fprintf(stderr, "; kick->kick avg %.1f max %.1f ms, repolls/kick", rtn ? rtsum / rtn : 0.0, rtmax);
            for (int k = 0; k < 6; k++) fprintf(stderr, " %d%s:%u", k, k == 5 ? "+" : "", rph[k]);
            fprintf(stderr, "; tick work max %.1f ms, oversleep max %.1f ms\n", wkmax, ovmax); wkmax = ovmax = 0; t0s = t; ticks = skips = 0; dtmax = 0; memset(lagh, 0, sizeof lagh);
            rtsum = rtmax = 0; rtn = 0; memset(rph, 0, sizeof rph);
            /* the fprintf above can block on stderr for ms: keep it out of the next gap */
            LARGE_INTEGER q2; QueryPerformanceCounter(&q2); tl = q2.QuadPart; tentry = q2.QuadPart; tkick = tkick ? q2.QuadPart : 0;
        }
        return;
    }
    if (addr == 0x0026D000 || addr == 0x002511F8) {   /* HUD "triangle pressed?" + its press query (h5142.md) */
        static std::atomic<unsigned> lastH{0};
        if (addr == 0x0026D000) {
            lastH = r3;
            unsigned toc = (unsigned)ctx->gpr[2], ctr = vm_read32(vm_read32(toc - 0x7340) + 0xD0);
            /* the triangle binding at H+0x1108: state mask +0xC (bit 0 = pressed), manager slot +0x44 (-1 = not registered) */
            const unsigned msk = vm_read32(r3 + 0x1114), slot = vm_read32(r3 + 0x114C);
            static unsigned lmsk = 0xDEADBEEF, lslot = 0xDEADBEEF; static unsigned chg = 0;
            if ((msk != lmsk || slot != lslot) && chg < 200) { chg++;
                fprintf(stderr, "[whud] 26D000 binding t=%.3f mask=%08X slot=%08X (was %08X/%08X)\n", t, msk, slot, lmsk, lslot); }
            lmsk = msk; lslot = slot;
            if (n <= 20 || n % 300 == 0)
                fprintf(stderr, "[whud] 26D000 #%u t=%.3f H=%08X b5142=%u ctr=%u mask=%08X slot=%08X lr=%08X\n", n, t, r3, vm_read8(r3 + 0x5142), ctr, msk, slot, (unsigned)ctx->lr);
            /* weapon-wheel gates of its caller 247E48/248130 (ui_ruota_gate.md): held bit [H+0x1110], wheel state
             * byte B at H+0x57E20 and timer ts at +0x57E24 vs threshold, open state [H+0x5140] */
            {   static unsigned lk = 0xFFFFFFFF, cnt = 0; const unsigned H = r3;
                unsigned held = vm_read32(H + 0x1110) & 1, edge = msk & 1;
                unsigned B = vm_read8(H + 0x57E20), st = vm_read8(H + 0x5140), b42 = vm_read8(H + 0x5142);
                unsigned tsb = vm_read32(H + 0x57E24), P = vm_read32(H + 0x57E1C);
                unsigned numb = vm_read32(vm_read32(toc + 0x7EB0)), denb = vm_read32(toc + 0x7E9C);
                float ts, num, den; memcpy(&ts, &tsb, 4); memcpy(&num, &numb, 4); memcpy(&den, &denb, 4);
                unsigned gl = vm_read8(vm_read32(toc + 0x6AD4));
                unsigned key = held | edge << 1 | B << 2 | st << 10 | (unsigned)(ts > num / den) << 18 | gl << 19;
                /* TOD_WHEEL_TRACE=1: trace the draws 20 frames after the wheel opens (st 0 -> 4) */
                {   static int wt = -1; static unsigned lst = 0;
                    if (wt < 0) wt = getenv("TOD_WHEEL_TRACE") ? 1 : 0;
                    if (wt && st == 4 && lst != 4) rsx_draw_engine_trace_after(20);
                    lst = st; }
                if ((key != lk || (n % 120) == 0) && cnt < 400) { cnt++; lk = key;
                    fprintf(stderr, "[wgate] t=%.3f held=%u edge=%u B=%u st5140=%u b5142=%u ts=%.3f thr=%.3f (num=%.3f den=%.3f) glob6AD4=%u ctr=%u P=%08X\n",
                            t, held, edge, B, st, b42, ts, den != 0 ? num / den : 0.f, num, den, gl, ctr, P); } }
        } else if (lastH && r3 == lastH + 0x1108) {
            static std::atomic<unsigned> m{0}; unsigned k2 = ++m;
            if (k2 <= 20 || k2 % 300 == 0)
                fprintf(stderr, "[whud] 2511F8 from 26D000 #%u t=%.3f r3=%08X r4=%08X lr=%08X\n", k2, t, r3, (unsigned)ctx->gpr[4], (unsigned)ctx->lr);
        }
        return;
    }
    /* Anark UI tree (anark_hud.md): 65CA80 = recursive walker, skips a node's
     * whole subtree when its [vtbl+0x10] (default: state [+0x34] != 0) says
     * inactive; 65B468 = the quad draw. Per root: each child panel's vptr,
     * state, request, alpha, children -- printed when it changes. Per 2 s: the
     * quad draws by caller. */
    if (addr == 0x0065CA80) {
        const unsigned node = r3;
        if (vm_read32(node + 0x54) != 0) return;   /* only roots */
        char sig[1024]; int o = 0;
        const unsigned toc = (unsigned)ctx->gpr[2];
        o += snprintf(sig + o, sizeof sig - o, "root %08X vt %08X st %u walkflag %u kids %u:",
                      node, vm_read32(node), vm_read32(node + 0x34),
                      vm_read32(vm_read32(toc - 0x61FC) + 4), vm_read32(node + 0x50));
        unsigned c = vm_read32(node + 0x48);
        for (int i = 0; c && i < 24 && o < (int)sizeof sig - 80; i++, c = vm_read32(c + 0x44)) {
            const unsigned vt = vm_read32(c);
            o += snprintf(sig + o, sizeof sig - o, " [%08X vt%08X f10=%08X st%u rq%u a%08X n%u]", c, vt,
                          vm_read32(vm_read32(vt + 0x10)), vm_read32(c + 0x34), vm_read32(c + 0x3C),
                          vm_read32(c + 0x60), vm_read32(c + 0x50));
        }
        static std::string last[8]; static unsigned roots[8], printed;
        int r = 0; for (; r < 8 && roots[r] && roots[r] != node; r++) {}
        if (r < 8 && !roots[r]) roots[r] = node;
        if (r < 8 && last[r] != sig && printed < 400) { printed++; last[r] = sig;
            fprintf(stderr, "[anark] t=%.3f %s\n", t, sig); }
        return;
    }
    if (addr == 0x0065B468) {
        static std::atomic<unsigned> lrs[16], cnts[16]; static double t0 = 0;
        const unsigned lr = (unsigned)ctx->lr;
        for (int i = 0; i < 16; i++) {
            unsigned v = lrs[i].load();
            if (v == lr || (v == 0 && lrs[i].compare_exchange_strong(v, lr)) || lrs[i].load() == lr) { cnts[i]++; break; }
        }
        /* its culls (65B468 head): width or height*K < K, or style alpha
         * [r4+0x34] * group alpha (stack [[toc-0x6260]+8+4*idx]) < [toc-0x6238] */
        static std::atomic<unsigned> cw, ca, ok; static float amin = 9, amax = -9, gmin = 9, gmax = -9;
        {   const unsigned toc = (unsigned)ctx->gpr[2], rc = r3, st = (unsigned)ctx->gpr[4];
            auto rf = [](unsigned a) { unsigned b = vm_read32(a); float f; memcpy(&f, &b, 4); return f; };
            const unsigned A = vm_read32(toc - 0x6260), idx = vm_read32(A + 4);
            const float w = rf(rc + 8) - rf(rc), h = rf(rc + 0xC) - rf(rc + 4);
            const float sa = rf(st + 0x34), ga = rf(A + 8 + 4 * idx), thr = rf(toc - 0x6238);
            if (w < 1.f || h < 1.f) cw++;
            else if (sa * ga < thr) { ca++; if (sa < amin) amin = sa; if (sa > amax) amax = sa;
                                      if (ga < gmin) gmin = ga; if (ga > gmax) gmax = ga; }
            else ok++;
            /* where its commands go: [toc-0x6254] -> a context whose +0 is the write pointer */
            static unsigned cmin = 0xFFFFFFFF, cmax = 0; static unsigned cx = 0;
            if (!(w < 1.f || h < 1.f) && !(sa * ga < thr)) {
                cx = vm_read32(toc - 0x6254); const unsigned cur = vm_read32(cx);
                if (cur < cmin) cmin = cur; if (cur > cmax) cmax = cur; }
            if (t - t0 >= 2.0) {
                fprintf(stderr, "[anark] t=%.3f quads drawn %u, culled size %u alpha %u (style a %.3f..%.3f group a %.3f..%.3f thr %.4f idx %u) cmd ctx %08X [%08X %08X %08X %08X] cur %08X..%08X\n",
                        t, ok.exchange(0), cw.exchange(0), ca.exchange(0), amin, amax, gmin, gmax, thr, idx,
                        cx, cx ? vm_read32(cx) : 0, cx ? vm_read32(cx + 4) : 0, cx ? vm_read32(cx + 8) : 0, cx ? vm_read32(cx + 12) : 0,
                        cmin, cmax);
                cmin = 0xFFFFFFFF; cmax = 0; }
            if (t - t0 >= 2.0) { amin = gmin = 9; amax = gmax = -9; } }
        if (t - t0 >= 2.0) { t0 = t;
            fprintf(stderr, "[anark] t=%.3f quads/2s by lr:", t);
            for (int i = 0; i < 16 && lrs[i]; i++) fprintf(stderr, " %08X:%u", lrs[i].load(), cnts[i].exchange(0));
            fprintf(stderr, "\n"); }
        return;
    }
    if (addr == 0x0028D4E8) {
        if (n <= 300)
            fprintf(stderr, "[whud] 28D4E8 #%u t=%.3f state=%u secs=%d lr=%08X\n", n, t, r3, (int)ctx->gpr[4], (unsigned)ctx->lr);
        return;
    }
    if (n <= 12 || n % 1000 == 0)
        fprintf(stderr, "[whud] func_%08X #%u t=%.3f tid=%u r3=%08X r4=%08X r5=%08X lr=%08X\n", addr, n, t,
                (unsigned)ctx->thread_id, r3, (unsigned)ctx->gpr[4], (unsigned)ctx->gpr[5], (unsigned)ctx->lr);
}
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock (the template's: vblank/flip ticks off real time, FIFO drains,
 * present on guest flips)
 * -----------------------------------------------------------------------*/
static volatile LONG g_frames_presented = 0;

/* The FIFO walker (frame_clock's cellGcm_rsx_process_fifo) sleeps between
 * drains. Two things want it NOW instead of up to 4 ms later: the ring-full
 * recycle in cellGcmSys.c (it waits for the walker to drain the tail) and a
 * guest thread polling with usleep while put != get (it is waiting on
 * something only a drain writes). Both go through this event. */
static HANDLE s_kick_event;
static void fifo_kick(void)
{
    if (s_kick_event) SetEvent(s_kick_event);
}
static int guest_usleep_hook(uint32_t, uint64_t)
{
    if (vm_read32(ppu_hle_inject_base + 0x2000u) != vm_read32(ppu_hle_inject_base + 0x2004u))
        fifo_kick();
    return 0;                            /* 0: still sleep as asked */
}

extern "C" unsigned ppu_boot_frames_presented(void) { return (unsigned)g_frames_presented; }

static void present_guest_frame(void)
{
    BEACON_PUSH("present_guest_frame");
    rsx_backend_present();
    BEACON_POP();
    InterlockedIncrement(&g_frames_presented);
}

/* Where the walker / presenter are, from the phase beacons (ps3emu/beacon.h):
 * "outer > inner (ms in it)". Read-only, racy by design. */
static void beacon_report(void)
{
    static const char* const nm[BEACON_SLOTS] = { "walker", "presenter" };
    const unsigned long long now = ps3_beacon_ms();
    for (int s = 0; s < BEACON_SLOTS; s++) {
        const ps3_beacon_t* b = &g_ps3_beacon[s];
        int d = b->depth;
        char line[512]; int n = snprintf(line, sizeof line, "[STALL] beacon %-9s tid=%lu:", nm[s], b->tid);
        if (d <= 0) n += snprintf(line + n, sizeof line - n, " idle / not in an instrumented phase");
        if (d > BEACON_DEPTH) d = BEACON_DEPTH;
        for (int i = 0; i < d && n < (int)sizeof line - 80; i++) {
            const char* name = b->name[i];
            n += snprintf(line + n, sizeof line - n, "%s %s (%llu ms)", i ? " >" : "", name ? name : "?", now - b->t0[i]);
        }
        fprintf(stderr, "%s\n", line);
    }
    const unsigned long long rw = g_ps3_recycle_wait_since;
    if (rw) fprintf(stderr, "[STALL] a guest thread is in the FIFO ring-full recycle wait for %llu ms\n", now - rw);
}

#ifdef _WIN32
/* Host stack of another thread, once: suspended only long enough to unwind with the
 * RtlVirtualUnwind tables (no allocation while it is stopped), symbolised after it runs again. */
static int beacon_unwind(HANDLE h, DWORD64* fr, int max)
{
    CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(h, &ctx)) return 0;
    int n = 0;
    __try {
        while (n < max && ctx.Rip) {
            fr[n++] = ctx.Rip;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (!rf) { ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8; }
            else { PVOID hd = NULL; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &ef, NULL); }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return n;
}
static void beacon_dump_stack(const char* label, DWORD tid)
{
    if (!tid) { fprintf(stderr, "[STALL] stack %s: no thread\n", label); return; }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { fprintf(stderr, "[STALL] stack %s: OpenThread(%lu) failed\n", label, (unsigned long)tid); return; }
    DWORD64 fr[48]; int n = 0;
    if (SuspendThread(h) != (DWORD)-1) { n = beacon_unwind(h, fr, 48); ResumeThread(h); }
    CloseHandle(h);
    static bool symok = false, symtried = false;
    if (!symtried) {
        symtried = true;
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, sizeof dir);
        char* sl = strrchr(dir, '\\'); if (sl) *sl = 0;
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        symok = SymInitialize(GetCurrentProcess(), dir, TRUE) != FALSE;
    }
    fprintf(stderr, "[STALL] host stack %s (tid %lu, %d frames):\n", label, (unsigned long)tid, n);
    for (int i = 0; i < n; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
        memset(si, 0, sizeof *si); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 disp = 0; DWORD ldisp = 0; IMAGEHLP_LINE64 ln; memset(&ln, 0, sizeof ln); ln.SizeOfStruct = sizeof ln;
        IMAGEHLP_MODULE64 mi; memset(&mi, 0, sizeof mi); mi.SizeOfStruct = sizeof mi;
        const bool haveMod = symok && SymGetModuleInfo64(GetCurrentProcess(), fr[i], &mi);
        if (symok && SymFromAddr(GetCurrentProcess(), fr[i], &disp, si)) {
            if (SymGetLineFromAddr64(GetCurrentProcess(), fr[i], &ldisp, &ln))
                fprintf(stderr, "[STALL]   #%02d %016llX %s!%s+0x%llX (%s:%lu)\n", i, (unsigned long long)fr[i],
                        haveMod ? mi.ModuleName : "?", si->Name, (unsigned long long)disp, ln.FileName, (unsigned long)ln.LineNumber);
            else
                fprintf(stderr, "[STALL]   #%02d %016llX %s!%s+0x%llX\n", i, (unsigned long long)fr[i],
                        haveMod ? mi.ModuleName : "?", si->Name, (unsigned long long)disp);
        } else
            fprintf(stderr, "[STALL]   #%02d %016llX %s\n", i, (unsigned long long)fr[i], haveMod ? mi.ModuleName : "?");
    }
}
#endif

/* Stall monitor. When no guest flip has been requested for TOD_STALL_SECS
 * (default 6; 0 disables) print what the guest is doing: FIFO put/get/ref
 * from the GCM control block (ppu_hle_inject_base+0x2000/4/8), the label
 * area, and every guest thread's lr/cia and back-chain return addresses.
 * Repeats every TOD_STALL_SECS while the stall lasts. Read-only. */
extern "C" void ps3_scring_dump(const char* path);
static DWORD WINAPI stall_monitor(LPVOID)
{
    const char* e = getenv("TOD_STALL_SECS");
    const int secs = e ? atoi(e) : 6;
    if (secs <= 0) return 0;
    unsigned last = ~0u;
    ULONGLONG since = GetTickCount64();
    for (;;) {
        Sleep(500);
        const unsigned fc = cellGcm_flip_request_count();
        const ULONGLONG now = GetTickCount64();
        static ULONGLONG s_status = 0;   /* a heartbeat every 10 s: the flip count is the measure of progress */
        if (now - s_status >= 10000) {
            s_status = now;
            fprintf(stderr, "[STATUS] flips=%u presented=%ld\n", fc, g_frames_presented);
        }
        if (fc != last) { last = fc; since = now; continue; }
        if (now - since < (ULONGLONG)secs * 1000) continue;
        since = now;
        const uint32_t w = ppu_hle_inject_base;
        fprintf(stderr, "[STALL] no flip for %d s (flips=%u presented=%ld)  FIFO put=%08X get=%08X ref=%08X\n",
                secs, fc, g_frames_presented, vm_read32(w + 0x2000u), vm_read32(w + 0x2004u), vm_read32(w + 0x2008u));
        V2EV("stall", "\"no_flip_s\":%d,\"flips\":%u,\"presented\":%ld,\"fifo\":[\"%08X\",\"%08X\",\"%08X\"]",
             secs, fc, g_frames_presented, vm_read32(w + 0x2000u), vm_read32(w + 0x2004u), vm_read32(w + 0x2008u));
        fprintf(stderr, "[STALL] labels:");
        for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", vm_read32(w + (uint32_t)i * 0x10u));
        fprintf(stderr, "\n");
        /* TOD_STALL_PEEK=addr[,*addr...] (hex): dump 16 guest words at each
         * address; each leading '*' reads the word there first and dumps from it; +off adds. */
        if (const char* pk = getenv("TOD_STALL_PEEK")) {
            char tmp[256]; strncpy(tmp, pk, sizeof tmp - 1); tmp[sizeof tmp - 1] = 0;
            for (char* tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
                int nd = 0; while (tok[nd] == '*') nd++;
                uint32_t a = (uint32_t)strtoul(tok + nd, NULL, 16);
                for (int k = 0; k < nd; k++) a = vm_read32(a);
                if (const char* pl = strchr(tok, '+')) a += (uint32_t)strtoul(pl + 1, NULL, 16);   /* *addr+off */
                fprintf(stderr, "[STALL] peek %s -> %08X:", tok, a);
                for (int i = 0; i < 16; i++) fprintf(stderr, " %08X", vm_read32(a + 4u * i));
                fprintf(stderr, "\n");
            }
        }
        for (unsigned t = 0; t < 64u; t++)
            if (g_hle_inflight[t]) fprintf(stderr, "[STALL] guest-tid %-3u inside HLE %s\n", t, g_hle_inflight[t]);
        beacon_report();
#ifdef _WIN32
        { static bool stacks_done = false;   /* once, at the first stall */
          if (!stacks_done) {
              stacks_done = true;
              beacon_dump_stack("walker", g_ps3_beacon[BEACON_WALKER].tid);
              beacon_dump_stack("presenter", g_ps3_beacon[BEACON_PRESENTER].tid);
          } }
#endif
        ppu_report_guest_chains(12);
        /* PS3_SCRING=1: dump the syscall ring once, at the first stall. */
        { static bool dumped = false;
          if (!dumped && getenv("PS3_SCRING")) { dumped = true; ps3_scring_dump("out\\scring.txt"); } }
    }
    return 0;
}

#ifdef _WIN32
/* TOD_STALL_MS=<ms> (perf115; default off): millisecond stall probe for the unexplained 50-60 ms frame stalls (hitch104: 66 frames
 * with no event attached). Adapted from RACiT's runtime patch (MIT, (c) 2026 HighDrexler; ps3recomp-0001-racacit-runtime-fixes
 * P:24911-25030, ppu_stall_probe / ppu_stall_host_stacks). When no frame has been PRESENTED for <ms> ms (after at least 20
 * steady frames, so loading screens and menus are not reported) it prints once per stall:
 *   [stall]       the gap so far, flip/present counters
 *   [stall-host]  every host thread: RIP + the return addresses found on its stack, as RVAs into tod.exe (or module+offset),
 *                 and the thread name when it has one (spu img N, raw spu N, ...). A thread is stopped only for
 *                 GetThreadContext and a copy of 8 KB of stack; no lock is taken while any thread is stopped.
 *   [STALL] beacon / [CHAIN]  the walker / presenter phase beacons and every guest PPU thread's cia/lr/back-chain (the old monitor's).
 * Then "[stall] ended" with the real length. At most TOD_STALL_MAX (default 16) dumps per session. The dump itself costs a few ms.
 * TOD_STALL_SYM=1 also symbolises with dbghelp (loads the 300 MB PDB: seconds, hundreds of MB; off by default, use
 * log\perf115\resolve.py offline instead). */
#include <tlhelp32.h>
namespace {
struct stall_thr { DWORD tid; HANDLE h; int got; DWORD64 rip, rsp; unsigned nw; uint64_t stk[1024]; };
stall_thr s_sth[256];
void stall_exe_range(uintptr_t* base, uintptr_t* lo, uintptr_t* hi)
{
    static uintptr_t b, l, h;
    if (!b) {
        b = (uintptr_t)GetModuleHandleA(NULL);
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(b + ((const IMAGE_DOS_HEADER*)b)->e_lfanew);
        const IMAGE_SECTION_HEADER* sh = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (sh[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                const uintptr_t a = b + sh[i].VirtualAddress, e = a + sh[i].Misc.VirtualSize;
                if (!l || a < l) l = a;
                if (e > h) h = e;
            }
    }
    *base = b; *lo = l; *hi = h;
}
void stall_addr_str(uintptr_t a, uintptr_t base, uintptr_t lo, uintptr_t hi, char* out, size_t n)
{
    if (a >= lo && a < hi) { snprintf(out, n, "exe+%llX", (unsigned long long)(a - base)); return; }
    HMODULE m = NULL; char mn[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m) {
        GetModuleFileNameA(m, mn, sizeof mn);
        const char* s = strrchr(mn, '\\'); if (s) memmove(mn, s + 1, strlen(s));
    }
    snprintf(out, n, "%s+%llX", mn, (unsigned long long)(a - (uintptr_t)m));
}
void stall_dump_host_threads(void)
{
    uintptr_t base, lo, hi; stall_exe_range(&base, &lo, &hi);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    int nt = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && nt < 256; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        s_sth[nt].tid = te.th32ThreadID; s_sth[nt].h = h; s_sth[nt].got = 0; s_sth[nt].nw = 0; nt++;
    }
    CloseHandle(snap);
    /* The critical part: nothing but the kernel calls below runs while a thread is stopped. */
    for (int i = 0; i < nt; i++) {
        stall_thr& t = s_sth[i];
        CONTEXT c; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (SuspendThread(t.h) != (DWORD)-1) {
            if (GetThreadContext(t.h, &c)) {
                t.got = 1; t.rip = c.Rip; t.rsp = c.Rsp;
                MEMORY_BASIC_INFORMATION mi;
                if (VirtualQuery((const void*)c.Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                    const uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
                    size_t n = (end - c.Rsp) / 8; if (n > 1024) n = 1024;
                    memcpy(t.stk, (const void*)c.Rsp, n * 8); t.nw = (unsigned)n;
                }
            }
            ResumeThread(t.h);
        }
    }
    typedef HRESULT (WINAPI *pfnGTD)(HANDLE, PWSTR*);
    static pfnGTD gtd = (pfnGTD)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
    static const bool sym = getenv("TOD_STALL_SYM") != NULL;
    static bool symok = false, symtried = false;
    if (sym && !symtried) {
        symtried = true;
        char dir[MAX_PATH]; GetModuleFileNameA(NULL, dir, sizeof dir);
        char* sl = strrchr(dir, '\\'); if (sl) *sl = 0;
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        symok = SymInitialize(GetCurrentProcess(), dir, TRUE) != FALSE;
    }
    for (int i = 0; i < nt; i++) {
        stall_thr& t = s_sth[i];
        if (t.got) {
            char line[1400]; int o = 0;
            char nm[64] = "";
            if (gtd) { PWSTR w = NULL; if (SUCCEEDED(gtd(t.h, &w)) && w) { if (*w) { snprintf(nm, sizeof nm, " \"%.40ls\"", w); } LocalFree(w); } }
            char a[300]; stall_addr_str((uintptr_t)t.rip, base, lo, hi, a, sizeof a);
            o += snprintf(line + o, sizeof line - o, "[stall-host] tid %lu%s: %s", (unsigned long)t.tid, nm, a);
            if (symok) {
                char buf[sizeof(SYMBOL_INFO) + 256]; SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
                memset(si, 0, sizeof *si); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255; DWORD64 d = 0;
                if (SymFromAddr(GetCurrentProcess(), t.rip, &d, si)) o += snprintf(line + o, sizeof line - o, " (%s+0x%llX)", si->Name, (unsigned long long)d);
            }
            int ret = 0;
            for (unsigned k = 0; k < t.nw && ret < 12 && o < (int)sizeof line - 40; k++) {
                const uintptr_t w = (uintptr_t)t.stk[k];
                if (w < lo + 8 || w >= hi) continue;
                const uint8_t* p = (const uint8_t*)w;   /* a return address follows a call */
                if (p[-5] == 0xE8 || (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10) || (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10) ||
                    (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10) || (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)) {
                    o += snprintf(line + o, sizeof line - o, " %llX", (unsigned long long)(w - base)); ret++;
                }
            }
            fprintf(stderr, "%s\n", line);
        }
        CloseHandle(t.h);
    }
    fflush(stderr);
}
/* TOD_STALL_LITE=1 (cheap path): the hot threads are found once (and again every 5 s from the probe's idle loop), their handles and
 * names kept; a snapshot is then only SuspendThread + GetThreadContext + a 8 KB stack copy + ResumeThread per thread, the stack walk
 * (return addresses into the exe, at most 12) after every thread is running again, and ONE fwrite. Phase times go to the "[stall] lite:" line. */
static const char* const s_lite_names[] = { "ppu main", "rsx walker", "rsx lane", "rsx draw B2", "rsx encode", "spurs worker", "spurs kernel", "raw spu" };
struct lite_thr { DWORD tid; HANDLE h; char name[48]; };
struct lite_cap { DWORD64 rip, rsp; unsigned nw; int got; uint64_t stk[1024]; };
struct lite_mod { uintptr_t ab; char name[40]; };
lite_thr s_lt[64]; int s_nlt = 0; ULONGLONG s_lt_tick = 0; double s_lt_names_ms = 0.0;
lite_cap s_lc[64];
lite_mod s_lm[48]; int s_nlm = 0;
double s_lite_ph[4]; double s_lite_susp_max = 0.0;   /* suspend, walk, (unused), print: ms summed over the snapshots of one dump */
double lite_now_ms(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
void stall_lite_refresh(void)
{
    const double t0 = lite_now_ms();
    for (int i = 0; i < s_nlt; i++) CloseHandle(s_lt[i].h);
    s_nlt = 0;
    typedef HRESULT (WINAPI *pfnGTDl)(HANDLE, PWSTR*);
    static pfnGTDl gtdl = (pfnGTDl)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te; te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok && s_nlt < 64; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            char nb[96] = ""; PWSTR w = NULL;
            if (gtdl && SUCCEEDED(gtdl(h, &w)) && w) { snprintf(nb, sizeof nb, "%.80ls", w); LocalFree(w); }
            bool keep = false;
            for (size_t k = 0; k < sizeof s_lite_names / sizeof s_lite_names[0]; k++) if (strstr(nb, s_lite_names[k])) { keep = true; break; }
            if (!keep) { CloseHandle(h); continue; }
            s_lt[s_nlt].tid = te.th32ThreadID; s_lt[s_nlt].h = h; snprintf(s_lt[s_nlt].name, sizeof s_lt[s_nlt].name, "%s", nb); s_nlt++;
        }
        CloseHandle(snap);
    }
    s_lt_tick = GetTickCount64();
    s_lt_names_ms = lite_now_ms() - t0;
}
static void lite_modstr(uintptr_t a, char* out, size_t n)
{
    MEMORY_BASIC_INFORMATION mi; uintptr_t ab = 0;
    if (VirtualQuery((const void*)a, &mi, sizeof mi)) ab = (uintptr_t)mi.AllocationBase;
    if (!ab) { snprintf(out, n, "?+%llX", (unsigned long long)a); return; }
    for (int i = 0; i < s_nlm; i++)
        if (s_lm[i].ab == ab) { snprintf(out, n, "%s+%llX", s_lm[i].name, (unsigned long long)(a - ab)); return; }
    char mn[MAX_PATH] = "?";
    GetModuleFileNameA((HMODULE)ab, mn, sizeof mn);
    const char* s = strrchr(mn, '\\'); s = s ? s + 1 : mn;
    if (s_nlm < 48) { s_lm[s_nlm].ab = ab; snprintf(s_lm[s_nlm].name, sizeof s_lm[s_nlm].name, "%s", s); s_nlm++; }
    snprintf(out, n, "%s+%llX", s, (unsigned long long)(a - ab));
}
void stall_lite_snapshot(int k, double since_ms)
{
    uintptr_t base, lo, hi; stall_exe_range(&base, &lo, &hi);
    const double t0 = lite_now_ms();
    double smax = 0.0;
    for (int i = 0; i < s_nlt; i++) {
        lite_cap& c = s_lc[i]; c.got = 0; c.nw = 0;
        const double a0 = lite_now_ms();
        CONTEXT cx; cx.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(s_lt[i].h) != (DWORD)-1) {
            if (GetThreadContext(s_lt[i].h, &cx)) {
                c.got = 1; c.rip = cx.Rip; c.rsp = cx.Rsp;
                MEMORY_BASIC_INFORMATION mi;
                if (VirtualQuery((const void*)cx.Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                    const uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
                    size_t n = (end - cx.Rsp) / 8; if (n > 1024) n = 1024;
                    memcpy(c.stk, (const void*)cx.Rsp, n * 8); c.nw = (unsigned)n;
                }
            }
            ResumeThread(s_lt[i].h);
        }
        const double d = lite_now_ms() - a0; if (d > smax) smax = d;
    }
    const double t1 = lite_now_ms();
    static char buf[65536]; int o = 0;
    for (int i = 0; i < s_nlt; i++) {
        const lite_cap& c = s_lc[i];
        if (!c.got || o > (int)sizeof buf - 1800) continue;
        o += snprintf(buf + o, sizeof buf - o, "[stall-host] snap %d +%.0f ms tid %lu%s%s%s: ", k, since_ms, (unsigned long)s_lt[i].tid,
                      s_lt[i].name[0] ? " \"" : "", s_lt[i].name, s_lt[i].name[0] ? "\"" : "");
        if ((uintptr_t)c.rip >= lo && (uintptr_t)c.rip < hi) o += snprintf(buf + o, sizeof buf - o, "exe+%llX", (unsigned long long)(c.rip - base));
        else { char a[120]; lite_modstr((uintptr_t)c.rip, a, sizeof a); o += snprintf(buf + o, sizeof buf - o, "%s", a); }
        int ret = 0;
        for (unsigned j = 0; j < c.nw && ret < 12; j++) {
            const uintptr_t w = (uintptr_t)c.stk[j];
            if (w < lo + 8 || w >= hi) continue;
            const uint8_t* q = (const uint8_t*)w;   /* a return address follows a call */
            if (q[-5] == 0xE8 || (q[-2] == 0xFF && (q[-1] & 0x38) == 0x10) || (q[-3] == 0xFF && (q[-2] & 0x38) == 0x10) ||
                (q[-6] == 0xFF && (q[-5] & 0x38) == 0x10) || (q[-7] == 0xFF && (q[-6] & 0x38) == 0x10)) {
                o += snprintf(buf + o, sizeof buf - o, " %llX", (unsigned long long)(w - base)); ret++;
            }
        }
        buf[o++] = '\n';
    }
    const double t2 = lite_now_ms();
    if (o) { fwrite(buf, 1, (size_t)o, stderr); fflush(stderr); }
    const double t3 = lite_now_ms();
    s_lite_ph[0] += t1 - t0; s_lite_ph[1] += t2 - t1; s_lite_ph[3] += t3 - t2;
    if (smax > s_lite_susp_max) s_lite_susp_max = smax;
}
/* frz107: guided read for a PPU thread parked in func_0048DA58 (waits for the completion flag of an animation command; frz106 report).
 * Everything is read through VirtualQuery + SEH, so a wild guest address cannot crash the dump. Only runs inside the [stall] dump. */
bool stall_rd32(uint32_t ea, uint32_t* out)
{
    if (!vm_base || (ea & 3u) || ea < 0x10000u || ea > 0xE0000000u - 4u) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(vm_base + ea, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    __try { *out = __builtin_bswap32(*(volatile const uint32_t*)(vm_base + ea)); } __except (1) { return false; }
    return true;
}
void stall_words(const char* tag, uint32_t ea, int nwords)
{
    char b[400]; int o = snprintf(b, sizeof b, "[anim-wait]   %s @%08X:", tag, ea);
    for (int k = 0; k < nwords && o < (int)sizeof b - 12; k++) {
        uint32_t v; if (stall_rd32(ea + 4u * k, &v)) o += snprintf(b + o, sizeof b - o, " %08X", v); else o += snprintf(b + o, sizeof b - o, " ????????");
    }
    fprintf(stderr, "%s\n", b);
}
void stall_chain_hook(unsigned tid, const uint64_t* gpr, uint64_t lr, uint64_t ctr, uint32_t cia)
{
    (void)ctr;
    static const bool test_all = getenv("TOD_STALL_HOOKTEST") != NULL;   /* test only: guided read for EVERY thread (garbage regs = robustness test) */
    const bool in_wait = test_all || (lr >= 0x48DA58 && lr < 0x48DB58) || (cia >= 0x48DA58 && cia < 0x48DB58);
    if (!in_wait) return;
    const uint32_t toc = (uint32_t)gpr[2], sp = (uint32_t)gpr[1], fl = (uint32_t)gpr[31];
    uint32_t base = 0, gen = 0, put = 0, w = 0, v = 0, h = 0;
    const bool ob = stall_rd32(toc + 0x6B68u, &base);
    fprintf(stderr, "[anim-wait] tid %u parked in func_0048DA58 wait loop: flag@%08X toc=%08X sp=%08X base=[toc+6B68]=%s%08X\n", tid, fl, toc, sp, ob ? "" : "(unreadable) ", base);
    if (ob && base) {
        const bool og = stall_rd32(base + 0x210u, &gen), op = stall_rd32(base + 0x214u, &put), ow = stall_rd32(base + 0x8220u, &w);
        fprintf(stderr, "[anim-wait]   ring: gen[base+210]=%s%08X put[base+214]=%s%08X W[base+8220]=%s%08X\n", og ? "" : "(?) ", gen, op ? "" : "(?) ", put, ow ? "" : "(?) ", w);
        if (ow && w) {
            if (stall_rd32(w + 4u, &v)) fprintf(stderr, "[anim-wait]   W+4 (published put-index) = %08X\n", v);
            const uint32_t ln = w & ~127u;
            for (int r = 0; r < 4; r++) { char t[24]; snprintf(t, sizeof t, "line W row%d", r); stall_words(t, ln + 32u * r, 8); }
        }
    }
    /* handle passed to func_0048DA58 by func_004C0C50: frame of 48DA58 is sp (0x90 bytes), the caller's frame follows */
    const bool oh = stall_rd32(sp + 0x90u + 0x1C0u, &h);
    fprintf(stderr, "[anim-wait]   handle [sp+0x90+0x1C0]=%s%08X -> gen=%u idx=%u; expected slot flag = %08X (waiting on %08X)%s\n", oh ? "" : "(?) ", h, h >> 10, h & 0x3FFu,
            base ? base + 0x230u + ((h & 0x3FFu) << 5) : 0u, fl, (base && base + 0x230u + ((h & 0x3FFu) << 5) == fl) ? " MATCH" : " (no match: handle slot differs)");
    if (fl >= 0x10020u) {
        if (stall_rd32(fl, &v)) fprintf(stderr, "[anim-wait]   flag word = %08X (bit0 %s)\n", v, (v & 1u) ? "SET" : "clear");
        stall_words("descriptor (flag-0x10, 8 words)", fl - 0x10u, 8);
        for (int k = 1; k <= 4; k++) { char t[40]; snprintf(t, sizeof t, "descriptor -%d", k); stall_words(t, fl - 0x10u - 32u * k, 8); }
    }
}
DWORD WINAPI stall_probe_ms(LPVOID)
{
    g_ppu_chain_hook = stall_chain_hook;
    const char* e = getenv("TOD_STALL_MS");
    const long lim = e ? atol(e) : 0;
    if (lim <= 0) return 0;
    const char* mx = getenv("TOD_STALL_MAX");
    const int maxd = mx ? atoi(mx) : 16;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    auto now_ms = [&]() { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)fq.QuadPart; };
    long last = g_frames_presented; double last_t = now_ms(); unsigned steady = 0; int dumps = 0, reported = 0;
    fprintf(stderr, "[stall] probe on: TOD_STALL_MS=%ld, at most %d dumps\n", lim, maxd);
    const bool lite = getenv("TOD_STALL_LITE") && getenv("TOD_STALL_LITE")[0] == '1';   /* 3 snapshots of the hot threads only (+0, +20, +50 ms) */
    if (lite) stall_lite_refresh();
    for (;;) {
        Sleep(2);
        if (lite && !reported && GetTickCount64() - s_lt_tick > 5000) stall_lite_refresh();   /* thread list kept fresh while nothing is stalled */
        const long f = g_frames_presented; const double t = now_ms();
        if (f != last) {
            const double gap = t - last_t;
            if (reported) { fprintf(stderr, "[stall] ended: %.1f ms without a presented frame (dump #%d)\n", gap, dumps); V2EV("stall_end", "\"ms\":%.1f", gap); reported = 0; steady = 0; }
            else if (gap > 2.0 * lim) steady = 0;
            else steady += (unsigned)(f - last);
            last = f; last_t = t;
            continue;
        }
        if (reported || dumps >= maxd || steady < 20u || t - last_t < (double)lim) continue;
        reported = 1; dumps++;
        fprintf(stderr, "[stall] #%d no presented frame for %.1f ms (limit %ld; presented=%ld flips=%u, %u steady frames before)\n",
                dumps, t - last_t, lim, f, cellGcm_flip_request_count(), steady);
        const double t0 = now_ms();
        double lite_cost = 0.0;
        if (lite) { memset(s_lite_ph, 0, sizeof s_lite_ph); s_lite_susp_max = 0.0; stall_lite_snapshot(0, 0.0); }
        else stall_dump_host_threads();
        lite_cost = now_ms() - t0;
        beacon_report();
        for (unsigned g = 0; g < 64u; g++)
            if (g_hle_inflight[g]) fprintf(stderr, "[STALL] guest-tid %-3u inside HLE %s\n", g, g_hle_inflight[g]);
        ppu_report_guest_chains(6);
        anim_hle_dump_stall();
        spurs_policy_dump_stall();
        if (lite) {
            static const double at[2] = { 20.0, 50.0 };
            for (int k = 0; k < 2; k++) {
                while (now_ms() - t0 < at[k]) Sleep(1);
                const double ts = now_ms();
                stall_lite_snapshot(k + 1, ts - t0);
                lite_cost += now_ms() - ts;
            }
            fprintf(stderr, "[stall] lite: host snapshots cost %.1f ms in total: suspend %.1f ms (slowest thread %.1f), walk %.1f, print %.1f; thread list %d threads, last refresh %.1f ms (off the dump); the rest of the dump time is the waits between snapshots\n",
                    lite_cost, s_lite_ph[0], s_lite_susp_max, s_lite_ph[1], s_lite_ph[3], s_nlt, s_lt_names_ms);
        }
        fprintf(stderr, "[stall] dump took %.1f ms\n", now_ms() - t0);
        fflush(stderr);
    }
    return 0;
}
}  // namespace
#endif

/* The window was closed: end the process (the guest threads have no exit path). */
extern "C" void cellSaveData_flush_async(void);   /* hitch116: PS3_SAVEDATA_ASYNC: a deferred autosave must reach the disk before TerminateProcess */
[[noreturn]] static void window_closed_exit(void)
{
    cellSaveData_flush_async();
    fprintf(stderr, "[rsx] window closed -- exiting\n");
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), 0);
#else
    _exit(0);
#endif
}

/* ---------------------------------------------------------------------------
 * Frame rate (TOD_FPS) -- docs/FRAMERATE.md. The game paces itself: Timer
 * object at guest 0x10731D00, +4 = min dt (cap, default 1/75 s), +8 = max dt
 * (0.08 s); its own flags -capfps N / -lockfps N / -60fps (func_0048A018) set
 * them from argv. Approach: TOD_FPS=<n> appends "-capfps;<n>" to PS3_ARGV,
 * which ppu_run (ppu_loader.cpp) turns into guest argv[1..] -- no relift, no
 * guest write at a racy point. TOD_FPS=lock<n> uses -lockfps (fixed dt).
 * For n > 75 func_0069AEC0 re-clamps TS+4 to >= the float at 0x8AA558
 * (1/75), so that constant (static .rodata, patched right after ppu_load_elf,
 * before any guest code runs) is lowered to 1/n. Unset: nothing changes.
 * -----------------------------------------------------------------------*/
#define TOD_A_DT_FLOOR 0x8AA558u
static int g_target_fps = 0;           /* 0 = the game's own pacing */
static volatile long long s_vblank_period = 0;   /* QPC ticks per host vblank (frame_clock); usr123: changed live by tod_fps_apply_live */
static long long s_qpf = 0;
#define TOD_A_TIMER 0x10731D00u        /* the game's Timer object: +4 min dt (cap), +8 max dt */

/* TOD_SYN_REPOLL_US (default 1000; 0 = the game's own): the NextSynth tick
 * (func_006F1B34) kicks one SPU job, and when the job has not finished at its next
 * tick it re-polls after usleep(1e6/240) = 4166 us (lr 0x6F1BA0). The poll is a
 * memory read (func_007E5980). On the PS3 the job is done by then; here it often
 * is not, so every round trip lost ~2 ms on average waiting for the re-poll and
 * the synth fell behind the audio ring in heavy fights (explosion crackle).
 * Re-poll after 1 ms instead. */
extern "C" void (*g_lv2_usleep_pre)(ppu_context* ctx, uint64_t* usec);
static uint64_t s_syn_repoll_us = 1000;
/* TOD_WAITDIAG=1: wait episodes of func_00534EC0 (usleep(30us) polls of a job flag): per (caller lr, flag address, slot)
 * polls, episodes, wall ms; [waitdiag] line every 5 s. Off by default. */
struct WaitKey { uint32_t lr, flag, slot; unsigned long long ms_us, n, eps; };
extern "C" unsigned long long ps3_qpc_us(void);
static void waitdiag_note(ppu_context* ctx, uint64_t usec)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_WAITDIAG"); on = (e && e[0] == '1'); }
    if (!on || usec != 30) return;
    static WaitKey keys[64]; static int nk = 0;
    static unsigned long long last = 0, t0 = 0, total_us = 0; static int cur = -1;
    const unsigned long long now = ps3_qpc_us();
    const uint32_t lr = (uint32_t)ctx->lr, flag = (uint32_t)ctx->gpr[31], slot = (uint32_t)ctx->gpr[27];
    int i = 0;
    for (; i < nk; i++) if (keys[i].lr == lr && keys[i].flag == flag && keys[i].slot == slot) break;
    if (i == nk) { if (nk >= 64) return; keys[nk] = WaitKey{ lr, flag, slot, 0, 0, 0 }; nk++; }
    if (last == 0 || now - last > 300 || cur != i) keys[i].eps++;
    else { keys[i].ms_us += now - last; total_us += now - last; }
    keys[i].n++; cur = i; last = now;
    if (!t0) t0 = now;
    if (now - t0 >= 5000000ull) {
        fprintf(stderr, "[waitdiag] 5s window, wait wall %.1f ms\n", total_us / 1000.0);
        for (int k = 0; k < 6; k++) {
            int b = -1;
            for (int j = 0; j < nk; j++) if (keys[j].n && (b < 0 || keys[j].ms_us > keys[b].ms_us)) b = j;
            if (b < 0) break;
            fprintf(stderr, "  lr=%08X flag=%08X slot=%u  %llu polls %llu eps  %.1f ms (%.2f ms/ep)\n", keys[b].lr, keys[b].flag, keys[b].slot,
                    keys[b].n, keys[b].eps, keys[b].ms_us / 1000.0, keys[b].eps ? keys[b].ms_us / 1000.0 / keys[b].eps : 0.0);
            keys[b].n = 0;
        }
        for (int j = 0; j < nk; j++) { keys[j].n = 0; keys[j].ms_us = 0; keys[j].eps = 0; }
        total_us = 0; t0 = now;
    }
}
static void tod_usleep_pre(ppu_context* ctx, uint64_t* usec)
{
    waitdiag_note(ctx, *usec);
    if ((uint32_t)ctx->lr != 0x006F1BA0u) return;
    { /* AUDIO_SYN_PERIOD_LOG=<ms>: the synth tick's own cadence (aud103). One line per gap between two of its sleeps longer than <ms>,
       * on the same clock as the [audio-gap] lines (QPC ms), with the sleep it asked for and the RSX frame. */
      static int thr = -1; static long long last;
      if (thr < 0) { const char* e = getenv("AUDIO_SYN_PERIOD_LOG"); thr = e ? atoi(e) : 0; }
      if (thr > 0) {
          LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
          if (last) { const double per = (q.QuadPart - last) * 1000.0 / f.QuadPart;
              if (per > thr) fprintf(stderr, "[syn-per] qpc %.1f ms: period %.1f ms (asked %llu us) frame %u\n", q.QuadPart * 1000.0 / f.QuadPart, per, (unsigned long long)*usec, ppu_boot_frames_presented()); }
          last = q.QuadPart; } }
    if (s_syn_repoll_us && *usec >= 4000 && *usec <= 4200) *usec = s_syn_repoll_us;
    LARGE_INTEGER q; QueryPerformanceCounter(&q); s_syn_sleep_q = q.QuadPart; s_syn_sleep_us = (long long)*usec;
    if (s_syn_diag_on > 0) { ULONG64 c; QueryThreadCycleTime(GetCurrentThread(), &c); s_syn_sleep_cyc = (long long)c; }
    /* AUDIO_SYN_SPIN=<us> (aud104, default off): the synth's long periodic sleep (not the 1 ms re-poll) is done here on a high-resolution
     * timer that ends <us> before the deadline, then spins to the deadline (QPC); the runtime's usleep then gets 0. Cuts the OS wake-up delay
     * of a plain timer wait at the price of <us> of one core per tick. */
    { static int spin = -1; static double qf = 0; static HANDLE tm = NULL;
      if (spin < 0) { const char* e = getenv("AUDIO_SYN_SPIN"); spin = e ? atoi(e) : 0; LARGE_INTEGER x; QueryPerformanceFrequency(&x); qf = (double)x.QuadPart; }
      if (spin > 0 && *usec > (uint64_t)spin + 2000) {
          const long long deadline = q.QuadPart + (long long)((double)*usec * qf / 1e6);
          if (!tm) tm = CreateWaitableTimerExW(NULL, NULL, 0x2 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
          if (tm) { LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((*usec - (uint64_t)spin) * 10); SetWaitableTimer(tm, &due, 0, NULL, NULL, FALSE); WaitForSingleObject(tm, INFINITE); }
          LARGE_INTEGER n; do { YieldProcessor(); QueryPerformanceCounter(&n); } while (n.QuadPart < deadline);
          *usec = 0;
      } }
}

#ifdef _WIN32
/* TOD_PCORES (default 1; 0 = off): on a hybrid CPU (Intel P + E cores) the process runs on the fastest core class only.
 * Issue #10 (TodeX, 07/11): the frequent/indefinite freezes went away completely with `start "" /affinity FFFF tod.exe`
 * (P-cores only); on E-cores the game's spin/handshake timing between PPU, SPU and RSX threads breaks down. A CPU with one
 * core class (every AMD Ryzen, older Intel) is left alone. SetProcessAffinityMask also constrains threads already running. */
static void pcores_setup(void)
{
    const char* e = getenv("TOD_PCORES");
    if (e && e[0] == '0') return;
    ULONG len = 0;
    GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len) return;
    std::vector<unsigned char> buf(len);
    if (!GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf.data(), len, &len, GetCurrentProcess(), 0)) return;
    int lo = 255, hi = -1, n_all = 0, n_hi = 0;
    for (ULONG off = 0; off < len;) {
        const SYSTEM_CPU_SET_INFORMATION* s = (const SYSTEM_CPU_SET_INFORMATION*)(buf.data() + off);
        if (s->Type == CpuSetInformation) {
            const int c = s->CpuSet.EfficiencyClass;
            if (c < lo) lo = c;
            if (c > hi) hi = c;
        }
        off += s->Size;
    }
    if (hi < 0 || hi == lo) { fprintf(stderr, "[pcores] one core class: affinity left alone\n"); return; }
    DWORD_PTR mask = 0;
    for (ULONG off = 0; off < len;) {
        const SYSTEM_CPU_SET_INFORMATION* s = (const SYSTEM_CPU_SET_INFORMATION*)(buf.data() + off);
        if (s->Type == CpuSetInformation) {
            n_all++;
            if (s->CpuSet.EfficiencyClass == hi && s->CpuSet.Group == 0 && s->CpuSet.LogicalProcessorIndex < sizeof(DWORD_PTR) * 8) {
                mask |= (DWORD_PTR)1 << s->CpuSet.LogicalProcessorIndex; n_hi++;
            }
        }
        off += s->Size;
    }
    DWORD_PTR pm = 0, sm = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) mask &= pm;
    if (n_hi < 4 || !mask) { fprintf(stderr, "[pcores] hybrid CPU but only %d fast logical CPUs: affinity left alone\n", n_hi); return; }
    if (SetProcessAffinityMask(GetCurrentProcess(), mask))
        fprintf(stderr, "[pcores] hybrid CPU: running on the %d fast logical CPUs of %d (mask %llX); TOD_PCORES=0 turns this off\n", n_hi, n_all, (unsigned long long)mask);
    else
        fprintf(stderr, "[pcores] SetProcessAffinityMask failed (%lu)\n", GetLastError());
}
#endif

static void set_default_env(const char* name, const char* value)
{
    const char* cur = getenv(name);
    if (cur && *cur) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

static void fps_setup_argv(void)
{
    const char* e = getenv("TOD_FPS");
    if (!e || !*e) return;
    const bool lock = !_strnicmp(e, "lock", 4);
    const double nd = atof(lock ? e + 4 : e);
    const int n = (int)ceil(nd);   /* a decimal value (59.8): the game gets the next whole number, the host's flip pace the exact one */
    if (n < 10 || n > 1000) { fprintf(stderr, "[fps] TOD_FPS=%s ignored (want 10..1000 or lock<n>)\n", e); return; }
    if (nd != (double)n && nd <= 60.0 && !lock) {
        char hz[32]; snprintf(hz, sizeof hz, "%.4f", nd);
        set_default_env("GCM_FLIP_PACE_HZ", hz);
        fprintf(stderr, "[fps] decimal TOD_FPS=%s: GCM_FLIP_PACE_HZ=%s\n", e, hz);
    }
    g_target_fps = n;
    char arg[256];
    const char* cur = getenv("PS3_ARGV");
    snprintf(arg, sizeof arg, "%s%s%s;%d", (cur && *cur) ? cur : "", (cur && *cur) ? ";" : "", lock ? "-lockfps" : "-capfps", n);
    _putenv_s("PS3_ARGV", arg);
    if (n > 60) set_default_env("RSX_VSYNC", "0");   /* presenting above 60: do not wait for the monitor */
    /* iss117 (issue #13): rsx_flip_pace holds the PPU to GCM_FLIP_PACE_HZ (default 60), which capped TOD_FPS=120 at 60 fps.
     * Above 60 the pace follows TOD_FPS, unless GCM_FLIP_PACE_HZ is set (environment or tod.ini; 0 = no pace). */
    if (n > 60) { char hz[16]; snprintf(hz, sizeof hz, "%d", n); set_default_env("GCM_FLIP_PACE_HZ", hz); }
    fprintf(stderr, "[fps] TOD_FPS=%s -> PS3_ARGV=%s\n", e, arg);
}

/* ---------------------------------------------------------------------------
 * Ultrawide (TOD_ASPECT, opt-in, default off).
 * TOD_ASPECT = 16:9 | 21:9 | 32:9 | <w>:<h> | <ratio> | auto (the window's / screen's own shape).
 * The game picks its screen aspect from the video-out state (func_00501C40: aspect 2 -> the float at
 * guest 0x8A73FC = 16/9, else the 4/3 at 0x8A73F8) and stores it in the global graphics config
 * (0x1064F430 + 0x128), from which the 3D cameras build their projection (func_00231BB8 reads both
 * constants too). Writing the chosen ratio over the 16/9 constant, right after the ELF is loaded and
 * before any guest code runs, makes every 3D view wider (the vertical field of view is unchanged).
 * The guest still renders its 1280x720 frame (x RSX_SCALE), so that frame comes out horizontally
 * squeezed; the present stretches it back to the chosen shape (rsx_present_set_frame_shape,
 * rsx_draw_engine.c; a movie keeps its own 16:9, a window of another shape gets bars).
 * The 2D parts of the frame (HUD, menus) are stretched with it: known limitation.
 * -----------------------------------------------------------------------*/
#define TOD_A_ASPECT_16_9 0x8A73FCu
extern "C" void rsx_present_set_frame_shape(float aspect);
extern "C" void tod_aspect_hor_install(float aspect);   /* src/stubs/aspect_hor.cpp */
static float g_tod_aspect = 0.0f;       /* 0 = off */
extern "C" float g_tod_ui_k = 1.0f;     /* TOD_ASPECT_UI: the HUD's width as a fraction of the frame ((16/9)/aspect), 1 = off; read by src/stubs/ui_xform.cpp */
static bool parse_ratio(const char* s, float* out)
{
    char* end = NULL;
    double a = strtod(s, &end);
    if (end == s || a <= 0.0) return false;
    if (*end == ':' || *end == '/' || *end == 'x' || *end == 'X') {
        char* e2 = NULL;
        const double b = strtod(end + 1, &e2);
        if (e2 == end + 1 || b <= 0.0) return false;
        a /= b;
    }
    *out = (float)a;
    return true;
}
static void aspect_setup(void)
{
    const char* e = getenv("TOD_ASPECT");
    if (!e || !*e) return;
    float a = 0.0f;
    if (!_stricmp(e, "auto")) {
        unsigned ww = 0, wh = 0;
        const char* d = getenv("RSX_DISPLAY");
        const bool windowed = !(d && (!_stricmp(d, "borderless") || !_stricmp(d, "fullscreen")));
        const char* w = getenv("RSX_WINDOW");
        if (windowed) { if (!(w && sscanf(w, "%ux%u", &ww, &wh) == 2)) { ww = 1280; wh = 720; } }
#ifdef _WIN32
        else { ww = (unsigned)GetSystemMetrics(SM_CXSCREEN); wh = (unsigned)GetSystemMetrics(SM_CYSCREEN); }
#endif
        if (ww && wh) a = (float)ww / (float)wh;
    } else if (!parse_ratio(e, &a)) a = 0.0f;
    if (!(a >= 1.2f && a <= 4.0f)) { fprintf(stderr, "[aspect] TOD_ASPECT=%s ignored (want 16:9, 21:9, 32:9, <w>:<h>, a ratio 1.2..4 or auto)\n", e); return; }
    if (fabsf(a - 16.0f / 9.0f) < 0.01f) { fprintf(stderr, "[aspect] TOD_ASPECT=%s is 16:9: nothing changed\n", e); return; }
    g_tod_aspect = a;
}
/* After ppu_load_elf, before any guest code: the present takes the chosen shape and the HUD transform is set. The game's own 16/9
 * constant (0x8A73FC) is no longer changed (r121u): it does not reach the 3D projection (Hor+ is done at the frustum builder,
 * src/stubs/aspect_hor.cpp) and the game's UI lays itself out from it; TOD_ASPECT_CONST=1 writes it as in 1.2.0 (comparison). */
static void aspect_patch_guest(void)
{
    if (g_tod_aspect <= 0.0f) return;
    uint32_t w; memcpy(&w, &g_tod_aspect, 4);
    { const char* c = getenv("TOD_ASPECT_CONST"); if (c && c[0] == '1') vm_write32(TOD_A_ASPECT_16_9, w); }
    rsx_present_set_frame_shape(g_tod_aspect);
    { /* TOD_ASPECT_UI (default 1 with TOD_ASPECT): the HUD stays 16:9 in the middle of the wide frame (src/stubs/ui_xform.cpp); 0 = it is stretched with the frame */
        const char* u = getenv("TOD_ASPECT_UI");
        if (!(u && u[0] == '0')) g_tod_ui_k = (16.0f / 9.0f) / g_tod_aspect;
        fprintf(stderr, "[aspect] TOD_ASPECT_UI=%s -> HUD k=%.4f\n", u ? u : "(default 1)", (double)g_tod_ui_k);
    }
    fprintf(stderr, "[aspect] TOD_ASPECT=%s -> %.4f (frame shown at that shape; 3D view widened at the camera, src/stubs/aspect_hor.cpp)\n", getenv("TOD_ASPECT"), (double)g_tod_aspect);
}

/* After ppu_load_elf: the dt floor for n > 75 (big-endian float 1/n). */
extern "C" volatile double g_rsx_flip_pace_hz;   /* rsx_draw_engine.c (usr123): >= 0 overrides GCM_FLIP_PACE_HZ live */
extern "C" volatile int g_rsx_display_reload;
static void fps_patch_guest(void)
{
    if (g_target_fps <= 75) return;
    const float f = 1.0f / (float)g_target_fps;
    uint32_t w; memcpy(&w, &f, 4);
    vm_write32(TOD_A_DT_FLOOR, w);
    fprintf(stderr, "[fps] dt floor 0x%08X = 1/%d (0x%08X)\n", TOD_A_DT_FLOOR, g_target_fps, w);
}

/* usr123 (user report 05/11: "the 120 fps limit does not work"): the FPS-limit row of the F1 page and of the game's Options page
 * is applied here, live, on the window thread. What the start-up path (fps_setup_argv) does once, this redoes at run time:
 * the Timer's min dt (TS+4, and TS+8 for lock<n>) = 1/n, the dt floor constant, the host vblank period of frame_clock, the
 * flip pace of the RSX (g_rsx_flip_pace_hz) and VSync off above 60. v NULL/"" = original pacing (cap 1/75, 60 Hz pace).
 * The guest floats are plain aligned 4-byte writes (the guest only reads them each frame). */
extern "C" void tod_fps_apply_live(const char* v)
{
    bool lock = false; double nd = 0.0;
    if (v && *v) { lock = !_strnicmp(v, "lock", 4); nd = atof(lock ? v + 4 : v); }
    int n = nd > 0.0 ? (int)ceil(nd) : 0;
    if (n && (n < 10 || n > 1000)) { fprintf(stderr, "[fps] live TOD_FPS=%s ignored (want 10..1000 or lock<n>)\n", v); return; }
    auto wf = [](uint32_t ea, float f) { uint32_t w; memcpy(&w, &f, 4); vm_write32(ea, w); };
    const double hz = n > 60 ? (double)n : 60.0;
    if (n) {
        wf(TOD_A_TIMER + 4, 1.0f / (float)n);
        wf(TOD_A_TIMER + 8, lock ? 1.0f / (float)n : 0.08f);
    } else {
        wf(TOD_A_TIMER + 4, 1.0f / 75.0f);
        wf(TOD_A_TIMER + 8, 0.08f);
    }
    wf(TOD_A_DT_FLOOR, n > 75 ? 1.0f / (float)n : 1.0f / 75.0f);
    g_target_fps = n;
    if (s_qpf) s_vblank_period = (long long)((double)s_qpf / hz);
    g_rsx_flip_pace_hz = (n && nd != (double)n && nd <= 60.0 && !lock) ? nd : hz;
    if (n > 60) {
        const char* vs = getenv("RSX_VSYNC");
        if (!(vs && vs[0] == '0')) { _putenv_s("RSX_VSYNC", "0"); g_rsx_display_reload = 1; }
    }
    fprintf(stderr, "[fps] live TOD_FPS=%s: Timer cap 1/%d%s, dt floor 1/%d, vblank+flip pace %.2f Hz\n", (v && *v) ? v : "(original)",
            n ? n : 75, lock ? " (locked)" : "", n > 75 ? n : 75, g_rsx_flip_pace_hz);
}

/* Frame clock: vblank/flip ticks on QueryPerformanceCounter, sleeping on a
 * high-resolution waitable timer + the FIFO kick event. Vblank rate 60 Hz, or
 * TOD_FPS when above 60; TOD_VBLANK_HZ=<hz> overrides. */
static double vblank_hz(void)
{
    double hz = 60.0;
    if (g_target_fps > 60) hz = (double)g_target_fps;
    if (const char* e = getenv("TOD_VBLANK_HZ")) { double v = atof(e); if (v >= 10.0 && v <= 1000.0) hz = v; }
    return hz;
}

extern "C" void ps3_sampler_tag_thread(void);   /* ppu_loader.cpp */
extern "C" void ps3_ccx_role(int); extern "C" void ps3_cpu_note(void);
extern "C" unsigned long long ps3_qpc_us(void);
extern "C" { volatile long long g_pipe_idle_us; }   /* TOD_PIPE_DIAG: walker thread idle time */
extern "C" volatile int g_tl_on; extern "C" void tl_rsx(int, int, unsigned long long, unsigned long long); extern "C" void tl_name(const char*, int);
#define TL_RSX(w, ph) do { if (g_tl_on) tl_rsx((w), (ph), 0, 0); } while (0)   /* TOD_TIMELINE */
static DWORD WINAPI frame_clock(LPVOID)
{
    ps3_ccx_role(2);
    ps3_beacon_thread(BEACON_WALKER);
    tl_name("RSX walker", 1);
    SetThreadDescription(GetCurrentThread(), L"rsx walker (frame clock)");   /* uni127: log v2 showed it unnamed (tid@tod.exe+0xC1D0), 43% of a core */
    /* PS3_SAMPLE_RSX=1 (with PS3_SAMPLE_SPU=1): profile this thread -- the FIFO
     * walker and draw translation -- instead of the SPURS workers. */
    if (getenv("PS3_SAMPLE_RSX")) ps3_sampler_tag_thread();
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    BEACON_PUSH("backend_init");
    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    BEACON_POP();
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME, rsx_ok ? "OK -- window open" : "FAILED");

    LARGE_INTEGER qf, q;
    QueryPerformanceFrequency(&qf);
    const long long freq = qf.QuadPart;
    const double hz = vblank_hz();
    s_qpf = freq; s_vblank_period = (long long)((double)freq / hz);
    fprintf(stderr, "[fps] vblank %.2f Hz\n", hz);

    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                          TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    HANDLE waits[2] = { s_kick_event, timer };

    unsigned  last_flip = 0;
    QueryPerformanceCounter(&q);
    long long next_tick = q.QuadPart;

    for (;;) {
        /* sleep until the next vblank, a kick, or at most 4 ms (the FIFO poll) */
        ps3_cpu_note();
        BEACON_PUSH("wait_vblank");
        QueryPerformanceCounter(&q);
        long long wait100ns = (next_tick - q.QuadPart) * 10000000ll / freq;
        if (wait100ns > 40000) wait100ns = 40000;
        if (wait100ns > 0 && timer) {
            LARGE_INTEGER due; due.QuadPart = -wait100ns;
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            { const LONGLONG w0 = ps3_qpc_us(); TL_RSX(2, 'B'); WaitForMultipleObjects(2, waits, FALSE, 10); TL_RSX(2, 'E'); g_pipe_idle_us += (LONGLONG)ps3_qpc_us() - w0; }
        } else if (wait100ns > 0) {
            WaitForSingleObject(s_kick_event, 4);
        }
        BEACON_POP();
        BEACON_PUSH("tick");
        QueryPerformanceCounter(&q);
        const long long now = q.QuadPart;

        int fired = 0;
        while (now - next_tick >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            if (rsx_ok && cellGcm_take_flip_pending()) {
                TL_RSX(1, 'B'); present_guest_frame(); TL_RSX(1, 'E');
                last_flip = cellGcm_flip_request_count();
            }
            if (rsx_ok) { TL_RSX(0, 'B'); cellGcm_rsx_process_fifo(); TL_RSX(0, 'E'); }
            next_tick += s_vblank_period;
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */
        BEACON_POP();

        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                TL_RSX(1, 'B'); present_guest_frame(); TL_RSX(1, 'E');
                last_flip = cellGcm_flip_request_count();
            }
            TL_RSX(0, 'B'); cellGcm_rsx_process_fifo(); TL_RSX(0, 'E');

            BEACON_PUSH("backend_pump");
            const int pump_rc = rsx_backend_pump();
            BEACON_POP();
            if (pump_rc != 0) window_closed_exit();
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                TL_RSX(1, 'B'); present_guest_frame(); TL_RSX(1, 'E');
                last_flip = fc;
            } else if (fc == 0) {
                BEACON_PUSH("backend_present0");
                rsx_backend_present();   /* before the first flip: keep the window alive */
                BEACON_POP();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root. Release layout: game/disc holds PS3_GAME/ (PS3_VFS_ROOT overrides).
 * -----------------------------------------------------------------------*/
/* ---------------------------------------------------------------------------
 * Guest VM (same as dod3: reserve 4 GB, commit 64 KB pages on first access;
 * vm_watch for the draw engine's texture write-watch)
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_watch_fault(fault, ep->ExceptionRecord->ExceptionInformation[0] == 1))
            return EXCEPTION_CONTINUE_EXECUTION;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery((void*)fault, &mi, sizeof mi)) return EXCEPTION_CONTINUE_SEARCH;
            if (mi.State == MEM_RESERVE)
                return vm_commit_reserved(page, 0x10000) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
            if (mi.State == MEM_COMMIT) {
                if (mi.Protect & PAGE_READWRITE) return EXCEPTION_CONTINUE_EXECUTION;
                DWORD old;
                if (VirtualProtect((void*)(fault & ~(uintptr_t)0xFFF), 0x1000, PAGE_READWRITE, &old))
                    return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* PS3_VM_BASE=<hex>: reserve the guest space at that fixed host address.
     * PS3_VM_ALIGN=<bytes>: otherwise align the (random) base to that. Default: OS choice. */
    {
        const char* eb = getenv("PS3_VM_BASE"); const char* ea = getenv("PS3_VM_ALIGN");
        if (eb && *eb) vm_base = (uint8_t*)VirtualAlloc((LPVOID)(uintptr_t)strtoull(eb, NULL, 16), VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
        if (!vm_base && ea && *ea) {
            const uintptr_t al = (uintptr_t)strtoull(ea, NULL, 0);
            for (int t = 0; t < 64 && al >= 0x10000 && !vm_base; t++) {
                void* p = VirtualAlloc(NULL, VM_SIZE + al, MEM_RESERVE, PAGE_READWRITE);
                if (!p) break;
                uintptr_t a = ((uintptr_t)p + al - 1) & ~(al - 1);
                VirtualFree(p, 0, MEM_RELEASE);
                vm_base = (uint8_t*)VirtualAlloc((LPVOID)a, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
            }
        }
        if (eb || ea) fprintf(stderr, "[vm] guest base %p (PS3_VM_BASE=%s PS3_VM_ALIGN=%s)\n", (void*)vm_base, eb ? eb : "-", ea ? ea : "-");
    }
    if (!vm_base) vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;            /* the whole space is backed; no OOB guard needed */
    if (vm_base) vm_watch_init(vm_base, VM_SIZE);
#else
    vm_base = (uint8_t*)calloc(1, 0xE0000000u);
    ppu_vm_size = 0xE0000000u;
#endif
    return vm_base != nullptr;
}

static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

#if defined(TOD_X86_V3) && defined(_WIN32)
/* The lifted code is built for x86-64-v3 (CMakeLists.txt, TOD_X86_LEVEL): say
 * so at start rather than crash on an illegal instruction. CPUID directly, as
 * dod3-recomp does: __builtin_cpu_supports needs compiler-rt's __cpu_model,
 * which clang-cl does not link. */
__attribute__((target("xsave"))) static bool x86_v3_cpu(void)
{
    int r1[4], r7[4];
    __cpuidex(r1, 1, 0);
    __cpuidex(r7, 7, 0);
    const bool os_avx = (r1[2] & (1 << 27)) && ((_xgetbv(0) & 6) == 6);
    return os_avx && (r1[2] & (1 << 12)) /* FMA */ && (r1[2] & (1 << 22)) /* MOVBE */ &&
           (r7[1] & (1 << 5)) /* AVX2 */ && (r7[1] & (1 << 3)) && (r7[1] & (1 << 8)) /* BMI1, BMI2 */;
}
#endif

#ifdef _WIN32
/* Windows 11: timeBeginPeriod is ignored for a process whose window is occluded / invisible / on a switched-off
 * monitor (and under EcoQoS), so every Sleep(1) / timed wait inside the process rounds up to the 15.6 ms clock
 * tick. Opt out: ProcessPowerThrottling with ControlMask = IGNORE_TIMER_RESOLUTION | EXECUTION_SPEED and
 * StateMask = 0 ("do not throttle"), then ask for 1 ms. TOD_TIMER_FIX=0 skips the opt-out (A/B).
 * TOD_TIMER_PROBE=1 prints, every 5 s, how long Sleep(1), a 1 ms event wait, a 1 ms SleepConditionVariableSRW and
 * a 1 ms high-resolution waitable timer really take in this process (histogram: <1.5, <3, <8, <20, >=20 ms). */
struct TodPowerThrottle { ULONG Version, ControlMask, StateMask; };
static DWORD WINAPI tod_timer_probe(LPVOID)
{
    static const double edge[4] = { 1.5, 3.0, 8.0, 20.0 };
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    HANDLE hr = CreateWaitableTimerExW(NULL, NULL, 0x00000002, TIMER_ALL_ACCESS);
    SRWLOCK lk = SRWLOCK_INIT; CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    typedef LONG (NTAPI *QTR)(PULONG, PULONG, PULONG);
    QTR qtr = (QTR)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryTimerResolution");
    for (;;) {
        unsigned h[4][5] = {{0}}; double sum[4] = {0}; int n = 0;
        ULONGLONG t0 = GetTickCount64();
        while (GetTickCount64() - t0 < 5000) {
            for (int k = 0; k < 4; k++) {
                LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
                if (k == 0) Sleep(1);
                else if (k == 1) WaitForSingleObject(ev, 1);
                else if (k == 2) { AcquireSRWLockExclusive(&lk); SleepConditionVariableSRW(&cv, &lk, 1, 0); ReleaseSRWLockExclusive(&lk); }
                else { LARGE_INTEGER due; due.QuadPart = -10000; SetWaitableTimer(hr, &due, 0, NULL, NULL, FALSE); WaitForSingleObject(hr, INFINITE); }
                QueryPerformanceCounter(&b);
                double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
                int bin = 0; while (bin < 4 && ms >= edge[bin]) bin++;
                h[k][bin]++; sum[k] += ms;
            }
            n++; Sleep(20);
        }
        ULONG mx = 0, mn = 0, cur = 0; if (qtr) qtr(&mx, &mn, &cur);
        fprintf(stderr, "[timer-probe] res cur=%.2f ms (min %.2f); n=%d  Sleep(1): avg %.2f ms [%u/%u/%u/%u/%u]  Wait1ms: avg %.2f [%u/%u/%u/%u/%u]  CondVar1ms: avg %.2f [%u/%u/%u/%u/%u]  HRtimer1ms: avg %.2f [%u/%u/%u/%u/%u]\n",
                cur / 10000.0, mn / 10000.0, n,
                sum[0] / n, h[0][0], h[0][1], h[0][2], h[0][3], h[0][4], sum[1] / n, h[1][0], h[1][1], h[1][2], h[1][3], h[1][4],
                sum[2] / n, h[2][0], h[2][1], h[2][2], h[2][3], h[2][4], sum[3] / n, h[3][0], h[3][1], h[3][2], h[3][3], h[3][4]);
    }
    return 0;
}
static void tod_timer_fix(void)
{
    const char* e = getenv("TOD_TIMER_FIX");
    const int fix = !(e && e[0] == '0');
    if (fix) {
        typedef BOOL (WINAPI *SPI)(HANDLE, int, LPVOID, DWORD);
        SPI f = (SPI)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessInformation");
        TodPowerThrottle st = { 1u, 0x1u | 0x4u, 0u };
        BOOL ok = f ? f(GetCurrentProcess(), 4 /* ProcessPowerThrottling */, &st, sizeof st) : FALSE;
        fprintf(stderr, "[timer-fix] ProcessPowerThrottling opt-out (execution speed + timer resolution): %s\n", ok ? "ok" : "not available");
    }
    timeBeginPeriod(1);                 /* 1 ms timer resolution */
    const char* p = getenv("TOD_TIMER_PROBE");
    if (p && p[0] == '1') CreateThread(NULL, 0, tod_timer_probe, NULL, 0, NULL);
}
#endif

/* 1.01: a start that fails must say why. Double-clicking tod.exe gives it a console that closes with the
 * process, so the message also goes into a message box -- only when stdout is that console (Play.bat and the
 * test scripts redirect it to a file and show or read the text themselves). TOD_ERROR_DIALOG=0: never. */
static void startup_fail(const char* msg)
{
    fprintf(stderr, "ERROR: %s\n", msg);
#ifdef _WIN32
    const char* e = getenv("TOD_ERROR_DIALOG");
    if (e && e[0] == '0') return;
    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_CHAR) return;
    MessageBoxA(NULL, msg, "Ratchet & Clank: Tools of Destruction Recompiled", MB_OK | MB_ICONERROR);
#endif
}

/* The game files that tod.exe needs, seen from `dir` (the disc root PS3_VFS_ROOT, absolute or relative to `dir`,
 * and elf\EBOOT.ELF). */
static bool tod_files_in(const std::filesystem::path& dir, std::string* missing)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const char* r = getenv("PS3_VFS_ROOT");
    fs::path root = (r && *r) ? fs::path(r) : fs::path(TOD_DEFAULT_VFS_ROOT);
    if (root.is_relative()) root = dir / root;
    if (!fs::exists(root / "PS3_GAME/PARAM.SFO", ec)) {
        if (missing) *missing = (root / "PS3_GAME").make_preferred().string();
        return false;
    }
    if (!fs::exists(dir / TOD_DEFAULT_ELF, ec)) {
        if (missing) *missing = (dir / TOD_DEFAULT_ELF).make_preferred().string();
        return false;
    }
    return true;
}

#ifdef _WIN32
/* ---------------------------------------------------------------------------
 * crash116: crash reporter (ON by default; it only writes a file when the process is dying).
 * Writes log\tod_crash.txt (next to log\tod.log; the working folder is the game's) with the code, the thread, the
 * address as module+offset and 32 frames as module+offset. Nothing here changes the normal run:
 *  - exceptions that the runtime handles itself (guest-pointer / commit VEHs, __try/__except) are NOT reported:
 *    access violations etc. are reported only from SetUnhandledExceptionFilter (they reach it only when nobody handled
 *    them); the vectored handler (last in the chain) looks only at 0xC0000409 and 0xC0000374, which nobody resumes;
 *  - SPU_NOUNWIND / longjmp paths do not raise exceptions, so they never reach these handlers.
 * Limit: a raw __fastfail (int 29h) is not seen by any handler; abort() and invalid-parameter calls are, because
 * _set_abort_behavior clears _CALL_REPORTFAULT (so they no longer end in __fastfail: the exit code is 3).
 * TOD_MINIDUMP=1 also writes log\tod_crash.dmp (small: stacks only). TOD_CRASH_TEST=abort|av|terminate|invalid|fastfail|stackoverflow
 * provokes a crash ~TOD_CRASH_TEST_MS (default 6000) ms after the start, to check the report.
 * -------------------------------------------------------------------------*/
static volatile LONG s_crash_count = 0;
static volatile LONG s_crash_in_terminate = 0;
static int s_crash_minidump = 0;

static const char* crash_module_of(const void* addr, char* out, size_t cap, uintptr_t* off)
{
    HMODULE m = NULL;
    out[0] = 0; *off = (uintptr_t)addr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(m, path, MAX_PATH);
        const char* b = strrchr(path, '\\');
        strncpy(out, b ? b + 1 : path, cap - 1); out[cap - 1] = 0;
        *off = (uintptr_t)addr - (uintptr_t)m;
    } else strncpy(out, "?", cap);
    return out;
}

static void crash_put(HANDLE f, const char* fmt, ...)
{
    char buf[512];
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)sizeof buf - 1;
    buf[n] = 0;
    DWORD w; WriteFile(f, buf, (DWORD)n, &w, NULL);
}

static void crash_put_addr(HANDLE f, const char* label, const void* addr)
{
    char mod[64]; uintptr_t off;
    crash_module_of(addr, mod, sizeof mod, &off);
    crash_put(f, "%s%s+0x%llx\r\n", label, mod, (unsigned long long)off);
}

static void crash_thread_name(DWORD tid, char* out, size_t cap)
{
    out[0] = 0;
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (h) {
        PWSTR d = NULL;
        if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
            WideCharToMultiByte(CP_UTF8, 0, d, -1, out, (int)cap - 1, NULL, NULL);
            out[cap - 1] = 0;
            LocalFree(d);
        }
        CloseHandle(h);
    }
    if (!out[0]) strncpy(out, "(no name)", cap);
}

static void crash_write_minidump(EXCEPTION_POINTERS* ep, DWORD tid)
{
    if (!s_crash_minidump) return;
    CreateDirectoryA("log", NULL);
    HANDLE f = CreateFileA("log\\tod_crash.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei; mei.ThreadId = tid; mei.ExceptionPointers = ep; mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                      (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
                      ep ? &mei : NULL, NULL, NULL);
    CloseHandle(f);
}

/* kind = what caught it; ep = exception info (NULL for abort/terminate/invalid parameter); tid = the crashing thread */
static void crash_report(const char* kind, EXCEPTION_POINTERS* ep, DWORD tid, const char* extra)
{
    if (InterlockedIncrement(&s_crash_count) > 6) return;   /* cap (a crash loop must not fill the disk) */
    CreateDirectoryA("log", NULL);
    HANDLE f = CreateFileA("log\\tod_crash.txt", FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    char tn[128]; crash_thread_name(tid, tn, sizeof tn);
    crash_put(f, "=== tod crash report %s (%04d-%02d-%02d %02d:%02d:%02d) ===\r\n", TOD_RELEASE_VERSION,
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    crash_put(f, "kind: %s\r\n", kind);
    if (extra && extra[0]) crash_put(f, "info: %s\r\n", extra);
    crash_put(f, "thread: %lu \"%s\"\r\n", (unsigned long)tid, tn);
    if (ep && ep->ExceptionRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        crash_put(f, "code: 0x%08lX  flags: 0x%lX\r\n", (unsigned long)er->ExceptionCode, (unsigned long)er->ExceptionFlags);
        crash_put_addr(f, "address: ", er->ExceptionAddress);
        if ((er->ExceptionCode == 0xC0000005 || er->ExceptionCode == 0xC0000006) && er->NumberParameters >= 2)
            crash_put(f, "access: %s of 0x%llx\r\n", er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute",
                      (unsigned long long)er->ExceptionInformation[1]);
        if (ep->ContextRecord) {
            const CONTEXT* c = ep->ContextRecord;
            crash_put(f, "rip=%llx rsp=%llx rbp=%llx\r\nrax=%llx rcx=%llx rdx=%llx rbx=%llx\r\n",
                      (unsigned long long)c->Rip, (unsigned long long)c->Rsp, (unsigned long long)c->Rbp,
                      (unsigned long long)c->Rax, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx, (unsigned long long)c->Rbx);
            /* unwind from the context of the fault */
            crash_put(f, "stack (unwound from the fault context):\r\n");
            CONTEXT cc = *c;
            __try {
                for (int i = 0; i < 32; i++) {
                    DWORD64 rip = cc.Rip;
                    if (!rip) break;
                    crash_put_addr(f, "  ", (const void*)rip);
                    DWORD64 base = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(rip, &base, NULL);
                    if (!fe) { cc.Rip = *(DWORD64*)cc.Rsp; cc.Rsp += 8; }
                    else {
                        PVOID hd = NULL; DWORD64 ef = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, rip, fe, &cc, &hd, &ef, NULL);
                    }
                }
            } __except (1) { crash_put(f, "  (unwind failed)\r\n"); }
        }
    } else {
        crash_put(f, "code: (no exception record)\r\n");
        void* fr[32]; USHORT n = CaptureStackBackTrace(1, 32, fr, NULL);
        crash_put(f, "stack (%u frames):\r\n", (unsigned)n);
        for (USHORT i = 0; i < n; i++) crash_put_addr(f, "  ", fr[i]);
    }
    crash_put(f, "\r\n");
    FlushFileBuffers(f);
    CloseHandle(f);
    crash_write_minidump(ep, tid);
    fflush(stdout); fflush(stderr);
    fprintf(stderr, "[crash] %s: see log\\tod_crash.txt\n", kind);
    fflush(stderr);
}

struct CrashJob { EXCEPTION_POINTERS* ep; DWORD tid; const char* kind; };
static DWORD WINAPI crash_job_thread(LPVOID p)
{
    CrashJob* j = (CrashJob*)p;
    crash_report(j->kind, j->ep, j->tid, NULL);
    return 0;
}

static LONG WINAPI crash_unhandled_filter(EXCEPTION_POINTERS* ep)
{
    const DWORD tid = GetCurrentThreadId();
    if (ep && ep->ExceptionRecord && ep->ExceptionRecord->ExceptionCode == 0xC00000FD) {
        /* stack overflow: no room to format on this stack, report from a helper thread */
        CrashJob j = { ep, tid, "unhandled exception (stack overflow)" };
        HANDLE h = CreateThread(NULL, 256 * 1024, crash_job_thread, &j, 0, NULL);
        if (h) { WaitForSingleObject(h, 10000); CloseHandle(h); }
    } else {
        crash_report("unhandled exception", ep, tid, NULL);
    }
    return EXCEPTION_CONTINUE_SEARCH;   /* the normal Windows handling (error report / exit code) follows */
}

static LONG CALLBACK crash_veh(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord) {
        const DWORD c = ep->ExceptionRecord->ExceptionCode;
        if (c == 0xC0000409 || c == 0xC0000374)
            crash_report("exception 0xC0000409/0xC0000374 (vectored)", ep, GetCurrentThreadId(), NULL);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void crash_sigabrt(int)
{
    if (!s_crash_in_terminate) crash_report("abort() / SIGABRT", NULL, GetCurrentThreadId(), NULL);
}

static void crash_terminate_handler()
{
    InterlockedExchange(&s_crash_in_terminate, 1);
    crash_report("std::terminate (uncaught C++ exception or noexcept violation)", NULL, GetCurrentThreadId(), NULL);
    abort();
}

static void crash_invalid_param(const wchar_t* expr, const wchar_t* func, const wchar_t* file, unsigned line, uintptr_t)
{
    char info[400];
    _snprintf(info, sizeof info - 1, "%ls in %ls (%ls:%u)", expr ? expr : L"?", func ? func : L"?", file ? file : L"?", line);
    info[sizeof info - 1] = 0;
    InterlockedExchange(&s_crash_in_terminate, 1);
    crash_report("CRT invalid parameter", NULL, GetCurrentThreadId(), info);
    abort();   /* same outcome as the CRT's default (a fatal exit) */
}

static void tod_crash_install(void)
{
    SetUnhandledExceptionFilter(crash_unhandled_filter);
    AddVectoredExceptionHandler(0, crash_veh);   /* 0 = last: the runtime's own handlers (first=1) run before it */
    signal(SIGABRT, crash_sigabrt);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    std::set_terminate(crash_terminate_handler);
    _set_invalid_parameter_handler(crash_invalid_param);
}

static int crash_so_rec(int n)
{
    volatile char b[4096];
    b[0] = (char)n;
    return crash_so_rec(n + 1) + b[0];
}

static DWORD WINAPI crash_test_thread(LPVOID p)
{
    const char* what = (const char*)p;
    const char* ms = getenv("TOD_CRASH_TEST_MS");
    SetThreadDescription(GetCurrentThread(), L"crash test");
    Sleep(ms ? (DWORD)atol(ms) : 6000);
    fprintf(stderr, "[crash-test] provoking '%s'\n", what);
    if (!strcmp(what, "abort")) abort();
    else if (!strcmp(what, "av")) { *(volatile int*)(uintptr_t)0x10 = 1; }
    else if (!strcmp(what, "terminate")) std::terminate();
    else if (!strcmp(what, "invalid")) { char b[2]; strcpy_s(b, 0, "x"); }
    else if (!strcmp(what, "fastfail")) __fastfail(7);
    else if (!strcmp(what, "stackoverflow")) crash_so_rec(0);
    return 0;
}

static void tod_crash_config(void)
{
    const char* d = getenv("TOD_MINIDUMP");
    s_crash_minidump = (d && d[0] == '1');
    const char* t = getenv("TOD_CRASH_TEST");
    if (t && t[0]) CreateThread(NULL, 0, crash_test_thread, (LPVOID)t, 0, NULL);
}
#endif

int main(int argc, char** argv)
{
    if (const char* d = getenv("TOD_START_DELAY_MS")) { const int ms = atoi(d); if (ms > 0 && ms < 20000) Sleep((DWORD)ms); }   /* usr123: a teleport-menu relaunch waits for the old process to go */
#ifdef _WIN32
    tod_crash_install();   /* crash116: before any thread is created */
    SetThreadDescription(GetCurrentThread(), L"ppu main");   /* the guest's main PPU thread runs here */
#endif
#if defined(TOD_X86_V3) && defined(_WIN32)
    if (!x86_v3_cpu()) {
        startup_fail("This build needs a CPU with AVX2, FMA and BMI2 (Intel 4th generation / AMD Ryzen or newer). "
                     "Rebuild with -DTOD_X86_LEVEL= for an older CPU.");
        return 1;
    }
#endif
    /* tod.ini first: it sets the environment variables everything below reads
     * (a variable already set in the environment wins over the file). */
    tod_settings_load(argc >= 1 ? argv[0] : NULL);
#ifdef _WIN32
    pcores_setup();       /* TOD_PCORES: hybrid Intel CPUs run on the P-cores only (issue #10); may come from tod.ini */
    tod_crash_config();   /* crash116: TOD_MINIDUMP / TOD_CRASH_TEST (may come from tod.ini) */
#endif
    /* frz107: the SPU anim hook reads SPU_HLE_WID0 / ANIM_HLE_WATCH / ANIM_HLE_DIAG / ANIM_FAST / ANIM_CAPTURE. It used to run from a static
     * constructor, before main() and so before the line above: tod.ini never reached it. Now it is called here, after the .ini is in the environment. */
    anim_cap_init();

    /* Installer: tod --install [--base <dir>] <disc>, tod --check <file>.
     * Runs and exits before any of the runtime is touched. */
    if (argc >= 2 && argv[1][0] == '-' && argv[1][1] == '-') {
        if (!strcmp(argv[1], "--check") && argc >= 3) return todsetup::check_cli(argv[2]);
        if (!strcmp(argv[1], "--install")) {
            int i = 2;
            std::filesystem::path base = ".";
            if (i + 1 < argc && !strcmp(argv[i], "--base")) { base = argv[i + 1]; i += 2; }
            if (i < argc) return todsetup::install_cli(base, argv[i]);
        }
        todsetup::print_install_instructions(".");
        return 2;
    }
    if (argc < 2) {
        /* 1.01: started from a shortcut or a launcher whose working folder is not the game's: the game files,
         * the saves (gamedata\) and the shader cache are next to tod.exe, so use that folder. A working folder
         * that has the files itself (the development runs) is kept. */
        std::string missing;
        if (!tod_files_in(".", &missing)) {
            wchar_t m[MAX_PATH];
            const DWORD n = GetModuleFileNameW(NULL, m, MAX_PATH);
            const std::filesystem::path exe_dir = (n && n < MAX_PATH) ? std::filesystem::path(m).parent_path() : std::filesystem::path();
            if (!exe_dir.empty() && tod_files_in(exe_dir, NULL) && SetCurrentDirectoryW(exe_dir.c_str()))
                fprintf(stderr, "[boot] working folder set to the game's: %s\n", exe_dir.string().c_str());
        }
        if (!tod_files_in(".", &missing)) {
            /* TODO(tod): an ImGui first-run wizard (dod3-recomp src/ui/ + third_party/imgui) instead of text. */
            todsetup::print_install_instructions(".");
            const std::string msg = "The game is not installed in this folder (or not completely): " + missing +
                                    " is missing.\n\nRun Install.bat and choose your decrypted disc (European release "
                                    "BCES00052). If you moved the game files, set PS3_VFS_ROOT in tod.ini.";
            startup_fail(msg.c_str());
            return 1;
        }
    }
    const char* elf = argc >= 2 ? argv[1] : TOD_DEFAULT_ELF;
#ifdef _WIN32
    tod_timer_fix();                    /* opt out of Windows 11 timer-resolution throttling + 1 ms timer resolution */
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif
    printf("=== Tools of Destruction Recompiled %s (BCES00052 1.00) ===\n", TOD_RELEASE_VERSION);
    tod_update_check_start();           /* background, a few seconds at most; TOD_UPDATE_CHECK=0 = off */

    /* TODO(tod): runtime defaults for the title go here (SPURS/GCM tweaks found
     * while booting). dod3 sets a dozen; none is known to apply yet. */
    /* The runtime's GCM window (labels, reports, put/get/ref control block,
     * IO offset tables) defaults to 0x20000000..+0x6000. The game's
     * _cellGcmInitBody is called with ioAddr=0x20000000, ioSize=0xBC00000 and
     * its command buffer and flip/label data live there, so the default
     * overlaps it (same trap as dod3, which moves the window to 0x8F000000). */
    ppu_hle_inject_base = 0x8F000000u;
    if (const char* w = getenv("TOD_GCM_WINDOW")) ppu_hle_inject_base = (uint32_t)strtoul(w, NULL, 16);   /* A/B: 20000000 = the runtime default */
    s_kick_event = CreateEventA(NULL, FALSE, FALSE, NULL);   /* auto-reset */
    g_gcm_fifo_kick_hook = fifo_kick;
    g_lv2_usleep_hook = guest_usleep_hook;
    CreateThread(NULL, 0, stall_monitor, NULL, 0, NULL);
    tod_discord_start();     /* Discord Rich Presence: own background thread, inactive without an Application ID or Discord */
#ifdef _WIN32
    if (const char* sm = getenv("TOD_STALL_MS")) { if (atol(sm) > 0) CreateThread(NULL, 0, stall_probe_ms, NULL, 0, NULL); }   /* perf115 */
#endif
    fps_setup_argv();        /* TOD_FPS -> PS3_ARGV (and RSX_VSYNC default above 60) */
    aspect_setup();          /* TOD_ASPECT (ultrawide, opt-in) */
    /* Esc alone must not quit: Steam Input's desktop layout sends Esc for a
     * pad's menu button (DualSense Options -> the game closed on "Start").
     * Shift+Esc, the window's close button or Alt+F4 still quit. */
#ifdef _WIN32
    rsx_host_window_escape_closes(0);
#endif
    set_default_env("PS3_TITLE", TOD_DEFAULT_TITLE);
    set_default_env("PS3_VFS_ROOT", TOD_DEFAULT_VFS_ROOT);
    /* compiled shaders: cache/dxbc next to the exe (rsx_d3d12_engine.c), not relative to the cwd */
    /* SPURS readyCount as a level (real kernel semantics, cellSpurs.c): without
     * it the WWS geomcullclip jobs start before the PPU kicks them, spin for
     * their "go" state and hold the SPU lane DrawSort needs -> black screen
     * when gameplay starts. SPURS_READY_LEVEL=0 in the environment turns it off. */
    set_default_env("SPURS_READY_LEVEL", "1");
    /* Release 1.2 performance defaults: the levers of the user's test launcher F (07/11: user play test at Imperial Fight Fest and
     * Sargasso, four other levels, 20 min soak, check mode with 0 mismatches), plus the SPURS lane-starvation yield and the faster
     * anim watchdog (3.6 s freeze at Sargasso). Each one stays overridable from the environment or tod.ini (set_default_env never
     * overwrites); TOD_PERF_DEFAULTS=0 turns the whole set off (1.1 behaviour). */
    { const char* e = getenv("TOD_PERF_DEFAULTS");
      if (!(e && e[0] == '0')) {
          static const char* const kPerf[][2] = {
              {"RSX_LANES", "3"}, {"RSX_REL_DEFER", "1"}, {"RSX_JOIN_LITE", "1"}, {"RSX_LANES_LABELQ", "1"}, {"RSX_LANES_ZOWN", "1"},
              {"RSX_LANES_ZLEAN", "1"}, {"RSX_LANES_ZOWN_LRU", "1"}, {"RSX_VPC_BLOCK", "1"}, {"RSX_WALK_PARSE2", "1"}, {"RSX_WALK_SNAP2", "1"},
              {"RSX_SURF_POOL", "1"}, {"RSX_SURF_POOL2", "1"}, {"RSX_PACE_VBLANK", "1"}, {"RSX_VRAM_GOV", "1"}, {"RSX_ZPRE_RECENT", "120"},
              {"RSX_FBDEPTH_LRU", "1"}, {"RSX_MS_FEWDRAW", "1"}, {"RSX_MS_TWIN_POOL", "1"}, {"RSX_MS_TWIN_PARK", "1"}, {"RSX_MS_TWIN_FIT", "1"},
              {"RSX_MS_TWIN_PARK_AGE", "216000"}, {"RSX_MS_TWIN_PARK_PRESS", "200"}, {"RSX_LANES_ZPRE_SAMP", "2"}, {"RSX_LANES_ARENA_MB", "4"},
              {"RSX_LANES_VCPRO", "1"}, {"RSX_RT_HEAP", "1"}, {"RSX_RT_HEAP_SPARE", "1"}, {"RSX_RT_HEAP_NOJOIN", "1"}, {"RSX_MS_NO_SHADOW", "28"},
              {"RSX_DISP_DIRECT", "1"}, {"RSX_LANES_K", "32"}, {"SPU_COLL_RING", "3"}, {"SPU_COLL_RING_IGN12", "1"}, {"TOD_FFA8_HAND", "1"},
              {"TOD_D647_HAND", "1"}, {"SPURS_PM_STARVE_MS", "40"}, {"ANIM_HLE_WD_FAST", "250"},
          };
          for (const auto& kv : kPerf) set_default_env(kv[0], kv[1]);
      } }
    /* RSX subchannel 1 is NV0039 (memory-to-memory copy) in libgcm's layout, which this title uses; the runtime now routes
     * each subchannel by the object bound to it (cellGcmSys.c gcm_sub1_observe) instead of a per-title switch, so nothing
     * to set here. GCM_SUBCH1_2D=1/0 in the environment pins subchannel 1 to NV0039 / NV4097. */
    /* The synth's kick thread at THREAD_PRIORITY_HIGHEST (sys_ppu_thread.c): at
     * normal priority Windows woke it late now and then in heavy scenes, the SPU
     * mix missed a whole ring and explosions crackled (sweep: 7 [audio-gap] -> 0).
     * PPU_AUDIO_BOOST=0 leaves it; PPU_QOS_INTERACTIVE overrides the list. */
    { const char* e = getenv("PPU_AUDIO_BOOST");
      if (!(e && e[0] == '0')) set_default_env("PPU_QOS_INTERACTIVE", "syn_tick_timer_thread,snd_stream_service_thread"); }
    { const char* e = getenv("TOD_SYN_REPOLL_US");
      if (e) s_syn_repoll_us = strtoull(e, NULL, 10);
      g_lv2_usleep_pre = tod_usleep_pre; }
    mkdir("cache", 0755);

    if (!alloc_guest_vm()) {
        startup_fail("Could not reserve the game's memory (the guest address space). Close other programs and try "
                     "again; a 64-bit Windows with enough free memory is needed.");
        return 1;
    }

    uint32_t entry = ppu_load_elf(elf);
    if (!entry) {
        const std::string msg = std::string("Could not load ") + elf + ". Run Install.bat again to remake it from your disc.";
        startup_fail(msg.c_str());
        return 1;
    }

    fps_patch_guest();       /* TOD_FPS > 75: lower the guest dt floor */
    aspect_patch_guest();    /* TOD_ASPECT: the 16/9 constant -> the chosen ratio */
    ppu_vfs_root = getenv("PS3_VFS_ROOT");
    printf("[boot] VFS root: %s\n", ppu_vfs_root);
    tod_level_streams_init();
    tod_mods_init();         /* mods\ folder: indexed once here (silent when there is none) */
    /* The title id (BCES00052) from the disc's PARAM.SFO: cellGame's default is
     * a placeholder (BLES00000), which the title saw in BootCheck/DataCheck and
     * the save directory prefix. */
    {
        std::string sfo = std::string(ppu_vfs_root) + "/PS3_GAME/PARAM.SFO";
        cellGame_init_from_paramsfo(sfo.c_str());
    }

    ppu_recomp_register();   /* lifted function table -> address map */
    if (g_tod_aspect > 0.0f) tod_aspect_hor_install(g_tod_aspect);   /* TOD_ASPECT: widen the render camera (src/stubs/aspect_hor.cpp) */
    ps3_load_prx_modules();  /* the port's own HLE (the six imports the runtime lacks) */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    { extern void ps3_sampler_start(void); ps3_sampler_start(); }   /* PS3_SAMPLE=<ms> guest sampling profiler (no-op unless set) */
    tod_overlay_init();      /* settings overlay over the D3D12 present (TOD_OVERLAY=0: off) */
    tod_input_init();        /* keyboard + TOD_INPUT_SCRIPT into port 0 (src/input/host_input.cpp) */
    tod_prompts_init();      /* button prompts per pad kind (TOD_PROMPTS) */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n", entry, STACK_TOP);

    ps3_ccx_role(1);
    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
