/* logv2 host side (06/11): the fields of the structured log that only the outer program knows.
 * - a sampler thread (1 s) measures process CPU %, GPU % (D3D12 timestamps), private RAM, and the CPU % of every thread
 *   (GetThreadTimes + GetThreadDescription), so nothing heavy runs on the present thread;
 * - g_v2_ext_hook: hero position, CPU/GPU/RAM (copied from the sampler);
 * - g_v2_threads_hook: the busiest threads (tick: top 6; fps_drop anomaly: every thread above 2 %);
 * - g_v2_settings_hook: the settings that change live (read with GetEnvironmentVariable: the menu writes the environment);
 * - tod_logv2_mark(): F7 = "mark" event (the user says "here"), with position and fps.
 * Registered at start-up by a static constructor; all of it is skipped when TOD_LOG_V2=0. */
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <mutex>

extern "C" {
#include "rsx_logv2.h"
extern volatile double g_rsx_gpu_busy_ms_total;   /* rsx_d3d12_engine.c (usr123) */
extern volatile int g_rsx_gpu_load_ok;
int tod_hero_pos(float* xyz);                      /* tod_cheat.cpp */
}

namespace {

std::mutex s_mx;
std::string s_ext, s_thr_tick, s_thr_full;

struct Th { DWORD tid; unsigned long long t100ns; };
std::vector<Th> s_prev;

std::string thread_name(HANDLE h, DWORD tid)
{
    typedef HRESULT(WINAPI * GTD)(HANDLE, PWSTR*);
    static GTD gtd = (GTD)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    std::string out;
    if (gtd) {
        PWSTR w = nullptr;
        if (SUCCEEDED(gtd(h, &w)) && w) {
            char b[96]; int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
            if (n > 1) out = b;
            LocalFree(w);
        }
    }
    if (out.empty()) {
        /* uni127: a thread nobody named (driver, OS pool, a library): its start address as module+offset, so the log says whose it is.
         * Cached per thread id: the lookup runs once per thread. */
        static std::vector<std::pair<DWORD, std::string>> cache;
        for (const auto& c : cache) if (c.first == tid) return c.second;
        typedef LONG(NTAPI * NQIT)(HANDLE, int, PVOID, ULONG, PULONG);
        static NQIT nq = (NQIT)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
        void* start = nullptr; std::string where;
        if (nq && nq(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof start, nullptr) == 0 && start) {
            HMODULE m = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)start, &m) && m) {
                char p[MAX_PATH]; const DWORD l = GetModuleFileNameA(m, p, sizeof p);
                const char* b = l ? strrchr(p, '\\') : nullptr;
                char w[160]; snprintf(w, sizeof w, "@%s+0x%llX", b ? b + 1 : (l ? p : "?"), (unsigned long long)((const char*)start - (const char*)m));
                where = w;
            } else { char w[40]; snprintf(w, sizeof w, "@0x%p", start); where = w; }
        }
        out = "tid" + std::to_string(tid) + where;
        char e[200]; v2_esc(e, sizeof e, out.c_str());
        if (cache.size() < 512) cache.push_back({ tid, e });
        return e;
    }
    char e[200]; v2_esc(e, sizeof e, out.c_str());
    return e;
}

void sample_once(double dt, double& last_cpu_s, double& last_gpu_ms, int ncpu)
{
    /* process CPU, GPU, RAM */
    FILETIME c, x, k, u; GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u);
    const double cpu_s = (double)((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime) +
                                  (((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime)) * 1e-7;
    const double gms = g_rsx_gpu_busy_ms_total;
    const double cpu_pct = last_cpu_s > 0 ? (std::min)(100.0, 100.0 * (cpu_s - last_cpu_s) / (dt * ncpu)) : 0.0;
    const double gpu_pct = (g_rsx_gpu_load_ok && last_gpu_ms > 0) ? (std::min)(100.0, 100.0 * (gms - last_gpu_ms) / (dt * 1000.0)) : -1.0;
    last_cpu_s = cpu_s; last_gpu_ms = gms;
    PROCESS_MEMORY_COUNTERS_EX pm{}; pm.cb = sizeof pm;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);

    /* per-thread CPU */
    struct Row { std::string nm; double pct; DWORD tid; };
    std::vector<Row> rows; std::vector<Th> now;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{}; te.dwSize = sizeof te;
        const DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);   /* uni127: full query right, for the start address of unnamed threads */
            if (!h) continue;
            FILETIME a, b, kk, uu;
            if (GetThreadTimes(h, &a, &b, &kk, &uu)) {
                const unsigned long long t = (((unsigned long long)kk.dwHighDateTime << 32) | kk.dwLowDateTime) +
                                             (((unsigned long long)uu.dwHighDateTime << 32) | uu.dwLowDateTime);
                now.push_back({ te.th32ThreadID, t });
                for (const Th& p : s_prev) if (p.tid == te.th32ThreadID) {
                    const double pct = 100.0 * (double)(t - p.t100ns) * 1e-7 / dt;
                    if (pct >= 2.0) rows.push_back({ thread_name(h, te.th32ThreadID), pct, te.th32ThreadID });
                    break;
                }
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
    }
    s_prev.swap(now);
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.pct > b.pct; });
    std::string tick = "\"thr\":[", full = "\"threads\":[";   /* [name, cpu % of one core] */
    for (size_t i = 0; i < rows.size(); i++) {
        char r[200]; snprintf(r, sizeof r, "%s[\"%s\",%.0f]", i ? "," : "", rows[i].nm.c_str(), rows[i].pct);
        if (i < 6) tick += r;
        if (i < 40) full += r;
    }
    tick += "]"; full += "]";
    char e[256];
    snprintf(e, sizeof e, "\"cpu\":%.0f,\"gpu\":%.0f,\"ram_mb\":%llu,\"nthreads\":%zu", cpu_pct, gpu_pct,
             (unsigned long long)(pm.PrivateUsage >> 20), s_prev.size());
    std::lock_guard<std::mutex> g(s_mx);
    s_ext = e; s_thr_tick = tick; s_thr_full = full;
}

DWORD WINAPI sampler(LPVOID)
{
    SetThreadDescription(GetCurrentThread(), L"logv2 sampler");
    SYSTEM_INFO si; GetSystemInfo(&si); const int ncpu = si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
    double lc = 0, lg = 0; LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (;;) {
        Sleep(1000);
        QueryPerformanceCounter(&b);
        const double dt = (double)(b.QuadPart - a.QuadPart) / (double)f.QuadPart; a = b;
        if (dt > 0.2) sample_once(dt, lc, lg, ncpu);
    }
    return 0;
}

int ext_hook(char* out, size_t n)
{
    float p[3]; std::string e;
    { std::lock_guard<std::mutex> g(s_mx); e = s_ext; }
    int o = 0;
    if (tod_hero_pos(p)) o = snprintf(out, n, "\"pos\":[%.1f,%.1f,%.1f]%s%s", p[0], p[1], p[2], e.empty() ? "" : ",", e.c_str());
    else o = snprintf(out, n, "%s", e.c_str());
    return o > 0;
}

int threads_hook(int full, char* out, size_t n)
{
    std::lock_guard<std::mutex> g(s_mx);
    const std::string& s = full ? s_thr_full : s_thr_tick;
    snprintf(out, n, "%s", s.c_str());
    return !s.empty();
}

int settings_hook(char* out, size_t n)
{
    static const char* const keys[] = { "RSX_SCALE", "RSX_AA", "RSX_LANES", "TOD_FPS", "RSX_VSYNC", "RSX_DISPLAY", "RSX_ANISO", "TOD_ASPECT" };
    size_t o = 0; out[0] = 0;
    for (const char* k : keys) {
        char v[64]; DWORD r = GetEnvironmentVariableA(k, v, sizeof v);
        if (!r || r >= sizeof v) continue;
        char e[96]; v2_esc(e, sizeof e, v);
        int w = snprintf(out + o, n - o, "%s\"%s\":\"%s\"", o ? "," : "", k, e);
        if (w > 0) o += (size_t)w;
        if (o >= n - 1) break;
        if (!strcmp(k, "TOD_FPS")) g_v2_target_fps = atof(v);
    }
    return 1;
}

struct Reg {
    Reg()
    {
        const char* e = getenv("TOD_LOG_V2");
        if (e && e[0] == '0') return;
        g_v2_ext_hook = ext_hook; g_v2_threads_hook = threads_hook; g_v2_settings_hook = settings_hook;
        const char* f = getenv("TOD_FPS"); g_v2_target_fps = f ? atof(f) : 60.0;
        HANDLE t = CreateThread(nullptr, 0, sampler, nullptr, 0, nullptr);
        if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
    }
} s_reg;

}  // namespace

/* "[teleport] ..." lines: printed to stderr as before and copied into the structured log as a "teleport" event. */
extern "C" void tod_tp_log(const char* fmt, ...)
{
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, stderr);
    if (!v2_enabled()) return;
    size_t l = strlen(b); while (l && (b[l - 1] == '\n' || b[l - 1] == '\r')) b[--l] = 0;
    char e[700]; v2_esc(e, sizeof e, b);
    float p[3];
    if (tod_hero_pos(p)) v2_event("teleport", "\"msg\":\"%s\",\"pos\":[%.1f,%.1f,%.1f]", e, p[0], p[1], p[2]);
    else v2_event("teleport", "\"msg\":\"%s\"", e);
}

/* F7: the user marks "here" (a stutter, a glitch...). The event carries the position and is never rate-limited. */
extern "C" void tod_logv2_mark(void)
{
    if (!v2_enabled()) return;
    static unsigned n;
    float p[3]; const int hp = tod_hero_pos(p);
    if (hp) v2_event("mark", "\"n\":%u,\"pos\":[%.1f,%.1f,%.1f]", ++n, p[0], p[1], p[2]);
    else v2_event("mark", "\"n\":%u", ++n);
    fprintf(stderr, "[logv2] mark %u\n", n);
}
#endif
