/*
 * Host side of the port's rows in the game's own Options page (menu104).
 *
 * The page is the Anark scene script "optHandler" (/built/anark/{frontend,pause}/built.dat). With the patched
 * archive (tools/menu/menu_patch.py: our tools/menu/lua/optHandler.lua, compiled, served through the mods folder)
 * the page scrolls and asks this file for extra rows through ONE Lua native, pc_opt:
 *
 *   pc_opt(op, row, value)
 *     (no arguments) -> 7071 (a NUMBER: strings pushed from the host came out empty, 04/11 menu107)  handshake: the host bridge is installed (without it the stock native answers a
 *                         number; it must be called with NO arguments: it is tolua code and raises a Lua error otherwise)
 *     Strings never cross the bridge (they came out empty): every answer is a NUMBER: a text id (the texts themselves are
 *     in tod_menu_text.inc and reach the script character by character, op 7).
 *     0   -> number of rows      1 -> text id of the value     2 -> step the value by `value` (+1 / -1), applied live
 *                                                               (answers the new text id)
 *     3   -> text id of the label      4 -> text id of the help
 *     7   -> (menu112) character `value` of text id `row`, as an index into kChars (the script's self.ch); 0 = past the end. The texts
 *            (tod_menu_text.inc) come in the language the game runs in (PS3_LANG), English where a language lacks one.
 *     5   -> 1 = open on the last row (TOD_MENU_PRESELECT=1, tests); also re-reads the values from the environment
 *
 * The engine registers its Lua natives by fixed tables (EBOOT 0x889524.. for the pause / in-game scenes, 0x8A2B88.. for the
 * frontend), so no new name can be added without a re-lift, and the page only sees the natives of ITS table (probe of
 * 05/11, in the frontend: get_button_layout = nil, is_rumble_on = function). is_rumble_on is a native the Options page
 * never calls and that sits in both tables (guest code 0x0002C848 pause/in-game, 0x0039CA10 frontend), so BOTH are
 * re-pointed at host code with ppu_register_function (the VM reaches a native through its OPD -> code address -> the
 * registry). The patched script aliases it as self.pcopt and calls it ONLY through pcall.
 *
 * ON BY DEFAULT (05/11 menu119): TOD_MENU absent or 1 = on, TOD_MENU=0 = off (nothing installed, the stock native stays),
 * TOD_MENU=force = install even without the patched archive (tests only). The bridge answers 7071 to the no-argument call,
 * which is also how the STOCK game calls is_rumble_on (the vibration option): installed WITHOUT the patched
 * global_cached.psarc in the mods folder it would turn that answer into a number. So it installs only when the mods folder
 * holds the patched archive (both copies, see mods_have_patched_archive); otherwise it stays out and says so in the log.
 *
 * Rows now (the table kRows below; the script holds NO per-row logic): RESOLUTION SCALE (RSX_SCALE), DISPLAY MODE
 * (RSX_DISPLAY), ANTIALIASING (RSX_AA), VSYNC (RSX_VSYNC), FPS LIMIT (TOD_FPS: read once at start, so "next start"),
 * FPS COUNTER (TOD_FPS_COUNTER). Each change goes to tod_menu_set_key in tod_overlay.cpp: environment, renderer
 * globals / display reload (on the window thread, B2 drained: menu108), tod.ini.
 *
 * Threading: the natives run on the guest thread that executes the Lua script; the AA row only touches ints and
 * the ini file. The strings are copied into a private guest buffer and pushed with tolua_pushstring (the VM copies).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <chrono>
#include <thread>
#include <filesystem>
#ifdef _WIN32
#  include "win32_compat.h"
#endif

#include "ppu_recomp.h"
#include "settings.h"

extern "C" {
extern uint32_t g_rsx_engine_frame;                           /* rsx_draw_engine.c: present counter */
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
int64_t sys_memory_allocate(ppu_context* ctx);
void tod_menu_set_key(const char* key, const char* value);    /* tod_overlay.cpp: a static key / value (NULL = unset) */
}
void func_00013080(ppu_context* ctx);   /* tolua_tonumber(L, idx, default) -> f1 */
void func_000141B0(ppu_context* ctx);   /* tolua_pushnumber(L, f1) */
void func_00013F40(ppu_context* ctx);   /* tolua_pushstring(L, const char*) (PLT stub) */

namespace {

#include "tod_menu_text.inc"      /* the texts, one table per language (menu112) */
#include "tod_menu_chars.inc"     /* kChars: the character set the script's table self.ch repeats */

const uint32_t kBridgeAddr[2] = { 0x0002C848u, 0x0039CA10u };   /* is_rumble_on: pause / in-game table, frontend table */
int s_preselect = 0;                         /* TOD_MENU_PRESELECT=1: the page opens on the last row (tests only) */
uint32_t s_buf;                              /* private guest buffer for strings (unused since menu107: numbers only) */

/* the rows: key = tod.ini / environment name; val[i] = its value for choice i (NULL = key unset); text[i] = text id */
struct HRow {
    const char* key;
    int label, help;
    int n;
    const char* val[8];
    int text[8];  
    int dflt;                                /* choice shown when the environment holds something else / nothing */
    int hot;                                 /* 1: applied at once; 0: at the next start (the help text says so) */
};
/* text ids: 1..60 values, 61..100 labels, 101..140 help, 141 / 142 the "applied at once / at the next start" tails
 * (tod_menu_text.inc) */
const HRow kRowTab[] = {
    { "RSX_SCALE",       61, 101, 4, { "1", "1.5", "2", "3" },                           { 7, 8, 9, 10 },     0, 1 },
    { "RSX_DISPLAY",     62, 102, 3, { "windowed", "borderless", "fullscreen" },         { 11, 12, 13 },      0, 1 },
    { "TOD_ASPECT",      63, 103, 4, { NULL, "21:9", "32:9", "auto" },                   { 21, 22, 23, 24 }, 0, 0 },
    { "RSX_AA",          64, 104, 5, { NULL, "fxaa", "msaa2", "msaa4", "msaa8" },        { 1, 3, 4, 5, 6 },   0, 1 },
    { "RSX_ANISO",       65, 105, 5, { "1", "2", "4", "8", "16" },                       { 1, 25, 26, 27, 28 }, 4, 1 },
    { "RSX_TEX_LOD_BIAS",66, 106, 3, { "0", "-0.5", "-1" },                              { 29, 30, 31 },      0, 0 },
    { "RSX_SHADOW_SCALE",67, 107, 3, { "1", "2", "4" },                                  { 32, 33, 34 },      1, 0 },
    { "RSX_BLOOM_SCALE", 68, 108, 3, { "1", "2", "4" },                                  { 32, 33, 34 },      1, 0 },
    { "RT_AO",           69, 109, 2, { NULL, "1" },                                      { 1, 2 },            0, 0 },
    { "RT_AO_QUALITY",   70, 110, 3, { "0", "1", "2" },                                  { 35, 36, 33 },      1, 0 },
    { "RSX_SSR",         71, 111, 2, { NULL, "1" },                                      { 1, 2 },            0, 0 },
    { "RSX_VSYNC",       72, 112, 2, { "1", "0" },                                       { 2, 1 },            0, 1 },
    { "TOD_FPS",         73, 113, 5, { NULL, "30", "60", "120", "1000" },                { 14, 15, 16, 17, 18 }, 0, 0 },
    { "TOD_FPS_COUNTER", 74, 114, 3, { "0", "1", "2" },                                  { 1, 19, 20 },       0, 1 },
    { "TOD_PROMPTS",     75, 115, 7, { NULL, "ps3", "ps4", "ps5", "xbox", "switch", "keyboard" }, { 24, 37, 38, 39, 40, 41, 42 }, 0, 1 },
    { "PPU_AUDIO_BOOST", 76, 116, 2, { NULL, "0" },                                      { 2, 1 },            0, 0 },
    { "PS3_NO_FMV",      77, 117, 2, { NULL, "1" },                                      { 1, 2 },            0, 0 },
    { "TOD_DISCORD",     78, 118, 2, { NULL, "0" },                                      { 2, 1 },            0, 0 },
    { "TOD_DS_TRIGGERS", 79, 119, 2, { NULL, "0" },                                      { 2, 1 },            0, 1 },
    { "TOD_DS_TRIGGER_STRENGTH", 80, 120, 4, { NULL, "75", "50", "25" },                 { 43, 44, 45, 46 },  0, 1 },
    { "TOD_DS_FIRE_R2",  81, 121, 2, { NULL, "0" },                                      { 2, 1 },            0, 1 },
};
const int kRows = (int)(sizeof kRowTab / sizeof kRowTab[0]);
int s_idx[32];                                /* current choice of each row */

int find_choice(const HRow& r, const char* v)
{
    if (!v || !*v) {
        for (int i = 0; i < r.n; i++) if (!r.val[i]) return i;
        return r.dflt;
    }
    if (!strcmp(r.key, "RSX_AA")) {          /* legacy spellings: on, 0/1, sample counts */
        if (!_stricmp(v, "on")) v = "fxaa";
        else if (!strcmp(v, "0") || !_stricmp(v, "off")) return 0;
        else if (!strcmp(v, "1")) v = "fxaa";
        else if (!strcmp(v, "2")) v = "msaa2";
        else if (!strcmp(v, "4")) v = "msaa4";
        else if (!strcmp(v, "8")) v = "msaa8";
    }
    for (int i = 0; i < r.n; i++) {
        if (!r.val[i]) continue;
        if (!_stricmp(r.val[i], v)) return i;
        if (!strcmp(r.key, "RSX_SCALE") && atof(r.val[i]) == atof(v)) return i;
    }
    return r.dflt;
}

void read_env()
{
    for (int i = 0; i < kRows; i++) s_idx[i] = find_choice(kRowTab[i], getenv(kRowTab[i].key));
}

uint64_t s_toc;                             /* caller's r2: the PLT stubs rebase r2 relative to the current value,
                                              * so it must be restored before EVERY guest call (04/11 menu107) */
void run_guest(void (*fn)(ppu_context*), ppu_context* ctx)
{
    ctx->gpr[2] = s_toc;
    fn(ctx);
    while (g_trampoline_fn) { void (*t)(void*) = g_trampoline_fn; g_trampoline_fn = 0; t((void*)ctx); }
}

double lua_arg(ppu_context* ctx, uint64_t L, int idx, double def)
{
    ctx->gpr[3] = L; ctx->gpr[4] = (uint64_t)idx; ctx->fpr[1] = def;
    run_guest(func_00013080, ctx);
    return ctx->fpr[1];
}

void push_number(ppu_context* ctx, uint64_t L, double v)
{
    ctx->gpr[3] = L; ctx->gpr[4] = 0; ctx->fpr[1] = v;
    run_guest(func_000141B0, ctx);
}

void push_string(ppu_context* ctx, uint64_t L, const std::string& s)
{
    if (!s_buf) {                            /* 64 KB granule, once; the out word sits below the caller's stack */
        ppu_context fake;
        memset(&fake, 0, sizeof fake);
        const uint32_t out = ((uint32_t)ctx->gpr[1] - 0x800u) & ~0xFu;
        vm_write32(out, 0);
        fake.gpr[3] = 0x10000; fake.gpr[4] = 0x200; fake.gpr[5] = out;
        sys_memory_allocate(&fake);
        s_buf = vm_read32(out);
        fprintf(stderr, "[menu] string buffer at guest 0x%08X\n", s_buf);
    }
    const size_t n = s.size() < 255 ? s.size() : 255;
    for (size_t i = 0; i < n; i++) vm_write8(s_buf + (uint32_t)i, (uint8_t)s[i]);
    vm_write8(s_buf + (uint32_t)n, 0);
    ctx->gpr[3] = L; ctx->gpr[4] = s_buf;
    run_guest(func_00013F40, ctx);
    { static int ns = 0; if (ns++ < 40) fprintf(stderr, "[menu] pushed string \"%s\" (%u bytes at 0x%08X, first byte now 0x%02X)\n", s.c_str(), (unsigned)n, s_buf, (unsigned)vm_read8(s_buf)); }
}

/* ---- texts (menu112): the language the game runs in (PS3_LANG, as cellSysutil.c reads it), one character at a time ---------- */
const TxItem* s_tx_lang = kTxEn;               /* table of the game's language; English fills the gaps */
const TxItem* tx_find(const TxItem* t, int id) { for (; t->id; t++) if (t->id == id) return t; return NULL; }

void pick_language()
{
    static const struct { const char* name; int v; const TxItem* t; } k[] = {
        { "english", 1, kTxEn }, { "english_us", 1, kTxEn }, { "english_gb", 18, kTxEn },
        { "french", 2, kTxFr }, { "spanish", 3, kTxEs }, { "german", 4, kTxDe }, { "italian", 5, kTxIt }, { "dutch", 6, kTxNl },
        { "portuguese", 7, kTxPt }, { "portuguese_br", 17, kTxPt }, { "finnish", 12, kTxFi }, { "swedish", 13, kTxSv },
        { "danish", 14, kTxDa }, { "norwegian", 15, kTxNo },
    };
    s_tx_lang = kTxEn;
    const char* e = getenv("PS3_LANG");
    if (!e || !*e) return;
    char* end = NULL;
    const long n = strtol(e, &end, 10);
    for (const auto& r : k)
        if ((end && !*end && n == r.v) || !_stricmp(e, r.name)) { s_tx_lang = r.t; return; }
}

/* character i (1-based) of text id as an index into kChars (1-based); 0 = past the end. A character not in kChars shows as a space. */
int tx_char(int id, int i)
{
    const TxItem* it = tx_find(s_tx_lang, id);
    if (!it) it = tx_find(kTxEn, id);
    if (!it || i < 1) return 0;
    const unsigned char* p = (const unsigned char*)it->s;
    for (int n = 1;; n++) {                  /* walk i-1 UTF-8 sequences */
        if (!*p) return 0;
        uint32_t cp; int len;
        if (*p < 0x80) { cp = *p; len = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); len = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); len = 3; }
        else { cp = '?'; len = 1; }
        if (n == i) {
            const unsigned char* c = (const unsigned char*)kChars;
            for (int j = 1; *c; j++) {
                uint32_t k; int l;
                if (*c < 0x80) { k = *c; l = 1; }
                else if ((*c & 0xE0) == 0xC0) { k = ((c[0] & 0x1Fu) << 6) | (c[1] & 0x3Fu); l = 2; }
                else { k = '?'; l = 1; }
                if (k == cp) return j;
                c += l;
            }
            return 1;
        }
        p += len;
    }
}

/* ---- the rows (kRowTab above) ---------------------------------------------------------------------- */
int row_text(int r) { return kRowTab[r - 1].text[s_idx[r - 1]]; }

void row_step(int r, int d)
{
    if (r < 1 || r > kRows || !d) return;
    const HRow& h = kRowTab[r - 1];
    int m = s_idx[r - 1] + (d > 0 ? 1 : -1);
    if (h.n == 2) m = (m + 2) % 2;           /* on / off rows toggle */
    if (m < 0) m = 0;
    if (m > h.n - 1) m = h.n - 1;
    if (m == s_idx[r - 1]) return;
    s_idx[r - 1] = m;
    tod_menu_set_key(h.key, h.val[m]);
}

/* int pc_opt(L): see the header */
void host_pc_opt(ppu_context* ctx)
{
    const uint64_t lr = ctx->lr;
    const uint64_t toc = ctx->gpr[2];
    s_toc = toc;
    const uint64_t L = ctx->gpr[3];
    const int op = (int)lua_arg(ctx, L, 1, -1.0);
    const int row = (int)lua_arg(ctx, L, 2, 0.0);
    const double val = lua_arg(ctx, L, 3, 0.0);
    static int s_calls = 0;
    if (s_calls++ < 60) fprintf(stderr, "[menu] pc_opt call %d: op=%d row=%d val=%g (L=0x%08X)\n", s_calls, op, row, val, (unsigned)L);
    const bool ok = row >= 1 && row <= kRows;
    switch (op) {
    case -1: push_number(ctx, L, 7071.0); break;       /* no argument at all: the handshake */
    case 0:  push_number(ctx, L, (double)kRows); break;
    case 1:  push_number(ctx, L, ok ? (double)row_text(row) : 0.0); break;
    case 2:  if (ok) row_step(row, val > 0 ? 1 : val < 0 ? -1 : 0); push_number(ctx, L, ok ? (double)row_text(row) : 0.0); break;
    case 3:  push_number(ctx, L, ok ? (double)kRowTab[row - 1].label : 0.0); break;
    case 4:  push_number(ctx, L, ok ? (double)kRowTab[row - 1].help : 0.0); break;
    case 7:  push_number(ctx, L, (double)tx_char(row, (int)val)); break;   /* char (val) of text (row), see tx_char */
    case 6:  push_number(ctx, L, ok ? (kRowTab[row - 1].hot ? 141.0 : 142.0) : 0.0); break;   /* help tail: at once / next start */
    case 5:  read_env(); push_number(ctx, L, s_preselect ? 1.0 : 0.0); break;   /* page opens: re-read; test hook TOD_MENU_PRESELECT=1 */
    default: push_number(ctx, L, 0.0); break;
    }
    ctx->gpr[2] = toc;
    ctx->gpr[3] = 1;                         /* one result */
    ctx->lr = lr;
}

/* Same folder rules as src/patches/mods.cpp (TOD_MODS=0 off, TOD_MODS_DIR, mods\ next to tod.exe, mods\ in the cwd):
 * is the patched archive (menu_patch.py output) there, in both places the game opens it? */
bool mods_have_patched_archive()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const char* mo = getenv("TOD_MODS");
    if (mo && *mo == '0') return false;
    fs::path dir;
    const char* d = getenv("TOD_MODS_DIR");
    if (d && *d) dir = d;
#ifdef _WIN32
    if (dir.empty()) {
        wchar_t exe[1024];
        const DWORD n = GetModuleFileNameW(NULL, exe, 1024);
        if (n > 0 && n < 1024) { fs::path b = fs::path(exe).parent_path() / "mods"; if (fs::is_directory(b, ec)) dir = b; }
    }
#endif
    if (dir.empty() && fs::is_directory("mods", ec)) dir = fs::absolute("mods", ec);
    if (dir.empty()) return false;
    const fs::path tail = fs::path("packed") / "game" / "global_cached.psarc";
    const bool disc = fs::is_regular_file(dir / "PS3_GAME" / "USRDIR" / tail, ec) || fs::is_regular_file(dir / "USRDIR" / tail, ec);
    const bool hdd  = fs::is_regular_file(dir / "game" / "BCES00052" / "USRDIR" / tail, ec);
    return disc && hdd;
}

void install()
{
    /* the loader fills the registry from its function table at boot; replace after that (tod.ini is in the
     * environment long before the first frame, so TOD_MENU may live there) */
    while (g_rsx_engine_frame < 1) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const char* on = getenv("TOD_MENU");
    if (on && !strcmp(on, "0")) return;                 /* TOD_MENU=0: stock page */
    if (!(on && !strcmp(on, "force")) && !mods_have_patched_archive()) {
        fprintf(stderr, "[menu] native PC options page NOT installed: no patched global_cached.psarc in the mods folder (stock page and vibration stay)\n");
        return;
    }
    for (uint32_t a : kBridgeAddr) ppu_register_function(a, host_pc_opt);
    read_env();
    pick_language();
    { const char* ps = getenv("TOD_MENU_PRESELECT"); s_preselect = (ps && *ps == '1') ? 1 : 0; }
    fprintf(stderr, "[menu] bridge pc_opt installed over is_rumble_on at 0x%08X and 0x%08X (%d rows, preselect %d)\n",
            kBridgeAddr[0], kBridgeAddr[1], kRows, s_preselect);
}

struct Starter { Starter() { std::thread(install).detach(); } } s_starter;

}  // namespace
