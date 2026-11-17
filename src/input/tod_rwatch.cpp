/*
 * tod_rwatch.cpp - research tool (12/11): which host code READS a guest page.
 *
 * rwatch=HEXADDR (cheat command) makes the 4 KB host page under that guest address inaccessible. Every access faults: the
 * vectored handler (registered first, so it runs before the VM commit handler) records the faulting host instruction, opens
 * the page for that one instruction (trap flag) and closes it again on the single-step exception. rwatchdump prints the
 * distinct instruction addresses with their symbols (tod.pdb). After 20000 faults the page stays open (the tool stops).
 * Off unless the command is used.
 */
#define WIN32_LEAN_AND_MEAN
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

extern "C" uint8_t* vm_base;

static volatile LONG s_on = 0;
static uintptr_t s_page = 0;
static volatile LONG s_faults = 0;
static const int kMax = 128;
static volatile uintptr_t s_rip[kMax];
static volatile LONG s_cnt[kMax];
static volatile LONG s_n = 0;
static uintptr_t s_exact = 0;   /* rwatchx=: only reads of these 4 bytes are recorded (the rest of the page still faults) */
static __declspec(thread) int t_pending = 0;
/* wwatchx= (15/11, enemy damage looks doubled): only WRITES of the 4 exact bytes, no fault cap; each write is kept with the
 * value before and after (big-endian float) and a host backtrace (guest callers = func_XXXXXXXX frames), printed at once
 * without symbols and with symbols by rwatchdump. */
static volatile LONG s_wonly = 0;
static const int kEv = 64;
static uintptr_t s_ev_bt[kEv][8];
static uint32_t s_ev_old[kEv], s_ev_new[kEv];
static volatile LONG s_ev_n = 0;
static __declspec(thread) uint32_t t_old = 0;
static __declspec(thread) uintptr_t t_bt[8];

static float be_f(uint32_t v) { v = _byteswap_ulong(v); float f; memcpy(&f, &v, 4); return f; }

static void note(uintptr_t rip)
{
    LONG n = s_n;
    for (LONG i = 0; i < n && i < kMax; i++) if (s_rip[i] == rip) { InterlockedIncrement(&s_cnt[i]); return; }
    LONG k = InterlockedIncrement(&s_n) - 1;
    if (k < kMax) { s_rip[k] = rip; s_cnt[k] = 1; }
}

static LONG WINAPI rwatch_veh(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_pending) {
        if (t_pending == 2) {
            const uint32_t nv = *(volatile uint32_t*)s_exact;
            const LONG k = InterlockedIncrement(&s_ev_n) - 1;
            if (k < kEv) { s_ev_old[k] = t_old; s_ev_new[k] = nv; memcpy(s_ev_bt[k], t_bt, sizeof t_bt); }
            fprintf(stderr, "[wwatch] #%ld write %.2f -> %.2f rip %p caller %p %p%c", (long)k, be_f(t_old), be_f(nv),
                    (void*)t_bt[0], (void*)t_bt[1], (void*)t_bt[2], 10);
        }
        t_pending = 0;
        DWORD old;
        if (s_on) VirtualProtect((void*)s_page, 0x1000, PAGE_NOACCESS, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && s_on) {
        const uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
        if ((fault & ~(uintptr_t)0xFFF) != s_page) return EXCEPTION_CONTINUE_SEARCH;
        const bool isw = ep->ExceptionRecord->ExceptionInformation[0] == 1;
        const bool hit = (!s_exact || (fault >= s_exact && fault < s_exact + 4)) && (!s_wonly || isw);
        if (hit) note((uintptr_t)ep->ContextRecord->Rip);
        bool ev = false;
        if (hit && s_wonly) {
            t_old = *(volatile uint32_t*)s_exact;
            /* frame 0 = faulting instruction, then the guest call chain walked from the faulting context */
            t_bt[0] = (uintptr_t)ep->ContextRecord->Rip;
            CONTEXT cx = *ep->ContextRecord;
            for (int i = 1; i < 8; i++) {
                DWORD64 ib = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(cx.Rip, &ib, NULL);
                if (!rf) { t_bt[i] = 0; continue; }
                void* hd = NULL; DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, cx.Rip, rf, &cx, &hd, &ef, NULL);
                t_bt[i] = (uintptr_t)cx.Rip;
            }
            ev = true;
        }
        DWORD old;
        VirtualProtect((void*)s_page, 0x1000, PAGE_READWRITE, &old);
        if ((!s_wonly && InterlockedIncrement(&s_faults) > (s_exact ? 3000000 : 20000)) || (hit && !s_wonly && s_n >= kMax)) { s_on = 0; return EXCEPTION_CONTINUE_EXECUTION; }
        if (s_wonly) InterlockedIncrement(&s_faults);
        t_pending = ev ? 2 : 1;
        ep->ContextRecord->EFlags |= 0x100;   /* trap flag: one instruction, then EXCEPTION_SINGLE_STEP */
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

extern "C" void tod_rwatch_arm2(uint32_t ea, int exact)
{
    static bool reg = false;
    if (!vm_base) return;
    if (!reg) { AddVectoredExceptionHandler(1, rwatch_veh); reg = true; }
    s_exact = exact ? (uintptr_t)(vm_base + ea) : 0;
    if (!exact) s_wonly = 0;
    s_page = (uintptr_t)(vm_base + ea) & ~(uintptr_t)0xFFF;
    s_faults = 0; s_n = 0;
    DWORD old;
    s_on = 1;
    const BOOL ok = VirtualProtect((void*)s_page, 0x1000, PAGE_NOACCESS, &old);
    fprintf(stderr, "[rwatch] guest %08X host page %p -> %s\n", ea, (void*)s_page, ok ? "watched" : "VirtualProtect failed");
    if (!ok) s_on = 0;
}

extern "C" void tod_rwatch_arm(uint32_t ea) { tod_rwatch_arm2(ea, 0); }

extern "C" void tod_wwatch_arm(uint32_t ea) { s_wonly = 1; s_ev_n = 0; tod_rwatch_arm2(ea, 1); }

extern "C" void tod_rwatch_dump(void)
{
    static bool sym = false;
    if (!sym) {
        char dir[MAX_PATH] = {0}; GetModuleFileNameA(NULL, dir, MAX_PATH);
        char* sl = strrchr(dir, 92); if (sl) *sl = 0;   /* 92 = backslash */
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (!SymInitialize(GetCurrentProcess(), dir, TRUE)) { SymSetSearchPath(GetCurrentProcess(), dir); SymRefreshModuleList(GetCurrentProcess()); }
        sym = true;
    }
    fprintf(stderr, "[rwatch] exe base %p%c", (void*)GetModuleHandleA(NULL), 10);
    const LONG n = s_n < kMax ? s_n : kMax;
    fprintf(stderr, "[rwatch] %ld faults, %ld distinct instructions\n", (long)s_faults, (long)n);
    char buf[sizeof(SYMBOL_INFO) + 256];
    for (LONG i = 0; i < n; i++) {
        SYMBOL_INFO* si = (SYMBOL_INFO*)buf; memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
        DWORD64 d = 0;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)s_rip[i], &d, si))
            fprintf(stderr, "[rwatch]   %p x%ld %s+0x%llX\n", (void*)s_rip[i], (long)s_cnt[i], si->Name, (unsigned long long)d);
        else
            fprintf(stderr, "[rwatch]   %p x%ld ?\n", (void*)s_rip[i], (long)s_cnt[i]);
    }
    const LONG ne = s_ev_n < kEv ? s_ev_n : kEv;
    for (LONG k = 0; k < ne; k++) {
        fprintf(stderr, "[wwatch] #%ld %.2f -> %.2f%c", (long)k, be_f(s_ev_old[k]), be_f(s_ev_new[k]), 10);
        for (int j = 0; j < 8 && s_ev_bt[k][j]; j++) {
            SYMBOL_INFO* si = (SYMBOL_INFO*)buf; memset(buf, 0, sizeof buf); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 255;
            DWORD64 d = 0;
            if (SymFromAddr(GetCurrentProcess(), (DWORD64)s_ev_bt[k][j], &d, si))
                fprintf(stderr, "[wwatch]     %s+0x%llX%c", si->Name, (unsigned long long)d, 10);
            else
                fprintf(stderr, "[wwatch]     %p%c", (void*)s_ev_bt[k][j], 10);
        }
    }
}
#else   /* the page-guard read watch uses Windows vectored exception handling; not available here yet */
#include <stdint.h>
#include <stdio.h>
extern "C" void tod_rwatch_arm2(uint32_t ea, int exact) { (void)ea; (void)exact; fprintf(stderr, "[rwatch] not supported on this platform\n"); }
extern "C" void tod_rwatch_arm(uint32_t ea) { tod_rwatch_arm2(ea, 0); }
extern "C" void tod_wwatch_arm(uint32_t ea) { tod_rwatch_arm2(ea, 1); }
extern "C" void tod_rwatch_dump(void) {}
#endif
