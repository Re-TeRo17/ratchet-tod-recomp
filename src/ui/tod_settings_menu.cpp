/*
 * Host natives for the ANTIALIASING row added to the game's own Options page
 * (tools/menu/lua50_patch.py patches the Anark "optHandler" Lua chunk in
 * /built/anark/{frontend,pause}/built.dat; tools/menu/psarc_repack.py serves the
 * patched archive through PS3_VFS_OVERLAY). See work/menu notes: menu_opzioni.md.
 *
 * The Options page calls Lua globals registered by the engine's unrolled
 * registrar (EBOOT 0x2A5xx: lua_register(L, name, OPD) per TOC pair, direct calls,
 * so no new names can be added without a re-lift). Instead three registered
 * wrappers that nothing on the page uses are re-pointed, at run time, at host
 * code with ppu_register_function (the Lua VM reaches a native through its OPD ->
 * code address -> the registry, so replacing the registry entry replaces it):
 *
 *   Lua name (patched row)  guest code  host function
 *   get_button_layout       0x0002C4A8  get_tod_aa       -> number: mode / 4 (0, .25 .. 1)
 *   set_button_layout       0x0002C3E8  set_tod_aa       <- number: the page passes
 *                                                           get() +- 0.1; only the
 *                                                           direction is used
 *   is_rumble_on            0x0002C848  get_tod_aa_text  -> string shown as the value
 *
 * Modes (RSX_AA): 0 off, 1 fxaa, 2 msaa2, 3 msaa4, 4 msaa8. A change writes
 * RSX_AA into tod.ini (applied at the next start; the value text says "(restart)").
 * The value string lives in the guest memory of the (already consumed) name
 * string of set_button_layout, 0x100039D8, 24 bytes.
 *
 * The three originals are dead on this page; if another page ever turns out to
 * use them, change kHooks and the names in tools/menu/lua50_patch.py together.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

#include "ppu_recomp.h"
#include "settings.h"

extern "C" {
extern uint32_t g_rsx_engine_frame;                           /* rsx_draw_engine.c: present counter */
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
}
void func_00013080(ppu_context* ctx);   /* tolua_tonumber(L, idx, default) -> f1 */
void func_000141B0(ppu_context* ctx);   /* tolua_pushnumber(L, f1) */
void func_00013F40(ppu_context* ctx);   /* tolua_pushstring(L, const char*) (PLT stub) */

namespace {

const uint32_t kTextSlot = 0x100039D8;
const int kModes = 5;
const char* const kRsx[kModes]  = { "off", "fxaa", "msaa2", "msaa4", "msaa8" };
const char* const kName[kModes] = { "OFF", "FXAA", "MSAA x2", "MSAA x4", "MSAA x8" };

int s_active = -1, s_pending = -1;

int parse(const char* v)
{
    if (!v || !*v) return 0;
    for (int i = 0; i < kModes; i++) if (!_stricmp(v, kRsx[i])) return i;
    if (!_stricmp(v, "on")) return 1;
    int n = atoi(v);                         /* 0 / 2 / 4 / 8 as sample count */
    return n == 2 ? 2 : n == 4 ? 3 : n == 8 ? 4 : n == 1 ? 1 : 0;
}

void init_modes()
{
    if (s_active >= 0) return;
    s_active = s_pending = parse(getenv("RSX_AA"));
}

/* Rewrite (or add) the RSX_AA line of tod.ini. */
void save_ini(int mode)
{
    const char* path = tod_settings_path();
    std::vector<std::string> lines;
    if (FILE* f = fopen(path, "rb")) {
        std::string cur; int c;
        while ((c = fgetc(f)) != EOF) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += (char)c; }
        if (!cur.empty()) lines.push_back(cur);
        fclose(f);
    }
    const std::string nl = std::string("RSX_AA = ") + kRsx[mode];
    int replaced = -1, tmpl = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& l = lines[i];
        size_t p = l.find_first_not_of(" \t");
        if (p == std::string::npos) continue;
        if (l.compare(p, 6, "RSX_AA") == 0 && l.find_first_not_of(" \t=", p + 6) != p + 6) { replaced = (int)i; break; }
        if (l[p] == '#' && tmpl < 0) {
            size_t q = l.find_first_not_of(" \t#", p);
            if (q != std::string::npos && l.compare(q, 6, "RSX_AA") == 0 && l.find('=', q) != std::string::npos) tmpl = (int)i;
        }
    }
    if (replaced >= 0) lines[replaced] = nl;
    else if (tmpl >= 0) lines.insert(lines.begin() + tmpl + 1, nl);
    else lines.push_back(nl);
    if (FILE* f = fopen(path, "wb")) {
        for (auto& l : lines) { std::string o = l; if (!o.empty() && o.back() == '\r') o.pop_back(); fprintf(f, "%s\r\n", o.c_str()); }
        fclose(f);
        fprintf(stderr, "[menu] RSX_AA = %s written to %s\n", kRsx[mode], path);
    } else fprintf(stderr, "[menu] cannot write %s\n", path);
}

void run_guest(void (*fn)(ppu_context*), ppu_context* ctx)
{
    fn(ctx);
    while (g_trampoline_fn) { void (*t)(void*) = g_trampoline_fn; g_trampoline_fn = 0; t((void*)ctx); }
}

/* int get_tod_aa(L): pushes mode / 4 */
void host_get_tod_aa(ppu_context* ctx)
{
    init_modes();
    const uint64_t lr = ctx->lr;
    ctx->gpr[4] = 0;
    ctx->fpr[1] = (double)s_pending / (double)(kModes - 1);
    run_guest(func_000141B0, ctx);
    ctx->gpr[3] = 1; ctx->lr = lr;
}

/* int set_tod_aa(L, number): direction = sign(arg - current) */
void host_set_tod_aa(ppu_context* ctx)
{
    init_modes();
    const uint64_t lr = ctx->lr;
    ctx->gpr[4] = 1; ctx->fpr[1] = -1.0;
    run_guest(func_00013080, ctx);
    const double v = ctx->fpr[1], cur = (double)s_pending / (double)(kModes - 1);
    int m = s_pending;
    if (v > cur) m++; else if (v < cur) m--;
    if (m < 0) m = 0; if (m > kModes - 1) m = kModes - 1;
    if (m != s_pending) { s_pending = m; save_ini(m); }
    ctx->gpr[3] = 0; ctx->lr = lr;
}

/* string get_tod_aa_text(L) */
void host_get_tod_aa_text(ppu_context* ctx)
{
    init_modes();
    const uint64_t lr = ctx->lr;
    char t[24];
    snprintf(t, sizeof t, "%s%s", kName[s_pending], s_pending != s_active ? " (restart)" : "");
    for (size_t i = 0; i <= strlen(t); i++) vm_write8(kTextSlot + i, (uint8_t)t[i]);
    ctx->gpr[4] = kTextSlot;
    run_guest(func_00013F40, ctx);
    ctx->gpr[3] = 1; ctx->lr = lr;
}

const struct { uint32_t addr; void (*fn)(ppu_context*); const char* what; } kHooks[] = {
    { 0x0002C4A8, host_get_tod_aa,      "get_button_layout -> get_tod_aa" },
    { 0x0002C3E8, host_set_tod_aa,      "set_button_layout -> set_tod_aa" },
    { 0x0002C848, host_get_tod_aa_text, "is_rumble_on -> get_tod_aa_text" },
};

void install()
{
    /* the loader fills the registry from its function table at boot; replace after that */
    /* Only with the patched menu served (PS3_VFS_OVERLAY): without it these three
     * natives are the game's own button-layout / rumble getters and must stay. */
    if (!getenv("PS3_VFS_OVERLAY") || getenv("TOD_MENU_AA") && getenv("TOD_MENU_AA")[0] == '0') return;
    while (g_rsx_engine_frame < 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (auto& h : kHooks) ppu_register_function(h.addr, h.fn);
    init_modes();
    fprintf(stderr, "[menu] AA row natives installed (RSX_AA mode %d)\n", s_active);
}

struct Starter { Starter() { std::thread(install).detach(); } } s_starter;

}  // namespace
