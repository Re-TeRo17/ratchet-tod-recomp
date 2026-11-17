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
#include <atomic>
#include <filesystem>
#ifdef _WIN32
#  include "win32_compat.h"
#endif

#include "ppu_recomp.h"
#include "settings.h"
#include "platform/tod_platform.h"   /* _stricmp off Windows */

extern "C" {
extern uint32_t g_rsx_engine_frame;                           /* rsx_draw_engine.c: present counter */
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
int64_t sys_memory_allocate(ppu_context* ctx);
void tod_menu_set_key(const char* key, const char* value);    /* tod_overlay.cpp: a static key / value (NULL = unset) */
void tod_quit_open(void);                                    /* tod_overlay.cpp: quit125 confirmation box */
}
void func_00013080(ppu_context* ctx);   /* tolua_tonumber(L, idx, default) -> f1 */
void func_000141B0(ppu_context* ctx);   /* tolua_pushnumber(L, f1) */
extern "C" void (*volatile g_cellPad_filter)(uint16_t* button);   /* cellPad.c (alert10) */
extern "C" void tod_exit_open(void);   /* src/ui/tod_overlay.cpp: "Quit the game?" box (alert10) */
extern "C" void tod_restart_open(void);   /* src/ui/tod_overlay.cpp: "Restart the game to apply the changes?" (alert10) */
void func_0039D400(ppu_context* ctx);   /* native save_load_is_interface_displayed (frontend table, every frame; alert10) */
void func_0002B960(ppu_context* ctx);   /* the same native in the pause / in-game table (10/11: pause hints) */
void func_00033E38(ppu_context* ctx);   /* native is_weapon_available (in-game table; 10/11: TOD_SHOP_ALL_WEAPONS) */
void func_000337A8(ppu_context* ctx);   /* native purchase_weapon (10/11: diagnostics) */
void func_00030810(ppu_context* ctx);   /* the native get_current_level (pause / in-game table, EBOOT 0x8894BC -> OPD 0x85A770) */
extern "C" int tod_lua_exec_L(uint32_t L, const char* src, uint32_t arg_tv);   /* src/input/tod_cheat.cpp: loadstring + pcall on that state */
extern "C" int tod_lua_exec_L_quiet(uint32_t L, const char* src, uint32_t arg_tv);   /* the same, no log line (alert10) */
extern "C" const char* tod_menu_text(int id);
extern "C" int tod_zurkon_on(void);           /* src/stubs/tod_zurkon.cpp: TOD_ZURKON_WEAPON */
extern "C" float tod_esci_w720(void);        /* src/ui/tod_overlay.cpp: QUIT's width at 720p in the game's wide font, 0 = font not loaded */
extern "C" float tod_fe_gap720(void);
extern "C" float tod_fe_shift720(void);
extern "C" void tod_cheat_enqueue(const char* cmds);   /* tod_cheat.cpp (10/11: the CHEAT page) */
extern "C" int tod_cheat_state(int id);
extern "C" uint32_t tod_hero_handle(void);
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
    /* 17/11 (user): difficulty, 5 steps around medium, applied live (src/stubs/diff_lock.cpp) */
    { "TOD_DIFFICULTY",  95, 135, 5, { "story", "easy", NULL, "hard", "extreme" },        { 52, 53, 54, 55, 56 }, 2, 1 },
    { "TOD_PROMPTS",     75, 115, 7, { NULL, "ps3", "ps4", "ps5", "xbox", "switch", "keyboard" }, { 24, 37, 38, 39, 40, 41, 42 }, 0, 1 },
    { "PPU_AUDIO_BOOST", 76, 116, 2, { NULL, "0" },                                      { 2, 1 },            0, 0 },
    { "PS3_NO_FMV",      77, 117, 2, { NULL, "1" },                                      { 1, 2 },            0, 0 },
    { "TOD_DISCORD",     78, 118, 2, { NULL, "0" },                                      { 2, 1 },            0, 0 },
    { "TOD_DS_TRIGGERS", 79, 119, 2, { NULL, "0" },                                      { 2, 1 },            0, 1 },
    { "TOD_DS_TRIGGER_STRENGTH", 80, 120, 4, { NULL, "75", "50", "25" },                 { 43, 44, 45, 46 },  0, 1 },
    { "TOD_DS_FIRE_R2",  81, 121, 2, { NULL, "0" },                                      { 2, 1 },            0, 1 },
    { "TOD_PAD_SWAP_R1R2", 85, 125, 2, { NULL, "1" },                                    { 1, 2 },            0, 1 },   /* 09/11 issue #26, cellPad.c */
    { "TOD_PAD_SWAP_L1L2", 86, 126, 2, { NULL, "1" },                                    { 1, 2 },            0, 1 },
    { "TOD_ARMOR_LOOK",  82, 122, 2, { NULL, "hidden" },                                 { 47, 48 },          0, 1 },
    { "TOD_REMOVE_HELMET", 83, 123, 2, { NULL, "1" },                                    { 1, 2 },            0, 1 },
    { "TOD_BAREFOOT",    84, 124, 2, { NULL, "1" },                                      { 1, 2 },            0, 0 },   /* 09/11: renderer filter, next start */
    { "TOD_SHOP_ALL_WEAPONS", 87, 127, 3, { NULL, "1", "2" },                             { 1, 2, 49 },        0, 1 },   /* 10/11: host_weapon_available */
    /* 10/11, the CHEAT page (pause > EXTRAS > CHEAT): keys starting with '!' are not settings (never in tod.ini); they run
     * tod_cheat.cpp's commands at once, like the F1 page's Cheats tab: on / off toggles read back from the engine, and actions */
    { "!god",            88, 128, 2, { "0", "1" },                                       { 1, 2 },            0, 1 },
    { "!ammo",           89, 129, 2, { "0", "1" },                                       { 1, 2 },            0, 1 },
    { "!heal",           90, 130, 2, { "0", "1" },                                       { 50, 51 },          0, 1 },
    { "!bolts",          91, 131, 2, { "0", "1" },                                       { 50, 51 },          0, 1 },
    { "!weapons",        92, 132, 2, { "0", "1" },                                       { 50, 51 },          0, 1 },
    /* 11/11 (user): the combat log on screen (src/stubs/combat_log.cpp; 2 = on-screen list + log lines, read every 30 polls) */
    { "TOD_COMBAT_LOG",  93, 133, 2, { NULL, "2" },                                      { 1, 2 },            0, 1 },
    /* Mr. Zurkon as a weapon (TOD_ZURKON_WEAPON, src/stubs/tod_zurkon.cpp) has no row since 1.3.0 (unfinished: icon,
     * equipment, shop); tod.ini can still turn it on for tests. */
};
const int kRows = (int)(sizeof kRowTab / sizeof kRowTab[0]);
int s_idx[64];                                /* current choice of each row */
int s_idx_open[64];                           /* the choices when the page opened (op 5), for the restart box (op 10) */

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
    for (int i = 0; i < kRows; i++) {
        const char* k = kRowTab[i].key;
        if (k[0] == '!') { s_idx[i] = !strcmp(k, "!god") ? (tod_cheat_state(0) > 0 ? 1 : 0) : !strcmp(k, "!ammo") ? (tod_cheat_state(1) > 0 ? 1 : 0) : 0; continue; }
        s_idx[i] = find_choice(kRowTab[i], getenv(k));
    }
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
    if (h.key[0] == '!') {                    /* the CHEAT page: a toggle flips, an action runs and reads DONE until the page reopens */
        const bool action = strcmp(h.key, "!god") && strcmp(h.key, "!ammo");
        if (action) m = 1;
        if (!tod_hero_handle()) { fprintf(stderr, "[menu] cheat %s: no hero yet\n", h.key); return; }
        const char* cmd = !strcmp(h.key, "!god") ? (m ? "god" : "god=0") : !strcmp(h.key, "!ammo") ? (m ? "ammo" : "noammo")
                        : !strcmp(h.key, "!heal") ? "heal" : !strcmp(h.key, "!bolts") ? "bolts=100000" : "weapons";
        tod_cheat_enqueue(cmd);
        fprintf(stderr, "[menu] cheat row %s -> %s\n", h.key, cmd);
        s_idx[r - 1] = m;
        return;
    }
    if (m == s_idx[r - 1]) return;
    s_idx[r - 1] = m;
    tod_menu_set_key(h.key, h.val[m]);
}

/* quit125: QUIT in the pause menu's main list. The list is the game's ListUpdater script: menuData[MAIN_MENU_INDEX] is a table of
 * { text, callback } pairs (text 33 MISSION .. 40 EXTRAS), run as item[2](self); the list draws as many items as isSelectable()
 * finds and calls the global table.getn when the cursor wraps. Its onInitialize builds that table and then calls the native
 * get_current_level; this replacement has the calling Lua state compile and run kQuitHook (the game's own loadstring + pcall),
 * with the caller's register 0 (the list object, an Anark userdata: CallInfo below the native's, L->ci at +0x14, 24-byte
 * CallInfo, base at +0; measured with TOD_QUIT_DUMP=1)
 * as the argument of the function the chunk returns (this Lua's main chunk gets no `arg`; tod_lua_exec_L calls it):
 *   - table.getn is wrapped: the main list gets a ninth item, the label in the game's language, whose callback asks the host for
 *     the confirmation box (bridge op 9 -> tod_quit_open in tod_overlay.cpp);
 *   - the list object gets that item at once, and its onActivate / onMenuBack put the main list on slide 1, the nine-item layout
 *     of the arena variant (slide 3, the stock one, is laid out for eight);
 *   - op 12 tells the host the list was found (until then the hook runs again at the next get_current_level of that state).
 * The game's script is not changed. TOD_PAUSE_QUIT=0 turns it off; TOD_PAUSE_QUIT_SLIDE=0 keeps the stock slide. */
const char* const kQuitHook =
    "if not tqG then tqG=table.getn "
    "table.getn=function(t) if type(t)==\"table\" and t[9]==nil then local a,b=t[1],t[8] "
    "if type(a)==\"table\" and type(b)==\"table\" and a[1]==33 and b[1]==40 then "
    "t[9]={\"%s\",function() pcall(is_rumble_on,9,0,0) end} end end "
    /* 10/11: the Options list (texts 42 GAME OPTIONS .. 47 LOAD) gets GRAPHICS OPTIONS second; both open the game's Options page
     * and tell the host which half to show (op 22, optHandler.lua asks with op 23) */
    "if %d==1 and type(t)==\"table\" and t[7]==nil then local a,b=t[1],t[6] "
    "if type(a)==\"table\" and type(b)==\"table\" and a[1]==42 and b[1]==47 and type(a[2])==\"function\" then local g=a[2] "
    "a[2]=function(x) pcall(is_rumble_on,22,1,0) return g(x) end "
    "tqGO=g table.insert(t,2,{\"%s\",function(x) pcall(is_rumble_on,22,2,0) return g(x) end}) end end "
    /* 10/11: the Extras list (texts 48 .. 52) gets CHEAT last: the same page on its cheat half (op 22 value 3) */
    "if %d==1 and type(t)==\"table\" and t[6]==nil then local a,b=t[1],t[5] "
    "if type(a)==\"table\" and type(b)==\"table\" and a[1]==48 and b[1]==52 then "
    "t[6]={\"%s\",function(x) pcall(is_rumble_on,22,3,0) if tqGO then return tqGO(x) end end} end end "
    "return tqG(t) end end "
    "return function(s) "
    "if (type(s)==\"userdata\" or type(s)==\"table\") and type(s.menuData)==\"table\" and s.MAIN_MENU_INDEX then local first=not s.tqDone s.tqDone=1 "
    /* 11/11 (user): the game rebuilds menuData at EVERY opening of the pause, on the same list object (p2x: get_current_level
     * again at the second opening, same self): the items below are added again each time; the wrappers only the first time */
    "table.getn(s.menuData[s.MAIN_MENU_INDEX]) "
    "if s.OPTIONS_MENU_INDEX and %d==1 then table.getn(s.menuData[s.OPTIONS_MENU_INDEX]) end "
    "if s.EXTRAS_MENU_INDEX and %d==1 then table.getn(s.menuData[s.EXTRAS_MENU_INDEX]) end "
    "if %d==1 then local OS=%d "
    "if first then local oa=s.onActivate s.onActivate=function(x) if x.currentMenuIndex==x.MAIN_MENU_INDEX then x.currentSlide=1 "
    "elseif tqOS and x.currentMenuIndex==x.OPTIONS_MENU_INDEX then x.currentSlide=OS "
    "elseif tqOS and x.currentMenuIndex==x.EXTRAS_MENU_INDEX then x.currentSlide=7 end return oa(x) end "
    "local ob=s.onMenuBack s.onMenuBack=function(x) ob(x) if x.currentMenuIndex==x.MAIN_MENU_INDEX then x.currentSlide=1 x.parent:goToSlide(1) "
    "elseif tqOS and x.currentMenuIndex==x.OPTIONS_MENU_INDEX then x.currentSlide=OS x.parent:goToSlide(OS) "
    "elseif tqOS and x.currentMenuIndex==x.EXTRAS_MENU_INDEX then x.currentSlide=7 x.parent:goToSlide(7) end end end "
    /* 10/11: with GRAPHICS OPTIONS the Options list has 7 items: it opens on slide 5 (a seven-row layout no script uses) instead of 7 (six rows) */
    "if s.OPTIONS_MENU_INDEX and %d==1 then tqOS=1 s.onOptions=function(x) x:doScreenChange(OS,x.setOptionsText) end "
    /* the Extras list with CHEAT has 6 items: slide 7 (the stock Options list's six rows) instead of 9 (five) */
    "s.onExtras=function(x) x:doScreenChange(7,x.setExtrasText) end "
    "local m=s.menuData[s.MAIN_MENU_INDEX] for i=1,9 do if type(m[i])==\"table\" then if m[i][1]==39 then m[i][2]=s.onOptions elseif m[i][1]==40 then m[i][2]=s.onExtras end end end end "   /* the item keeps the function it was built with */
    "end pcall(is_rumble_on,12,1,0) end end";
/* 11/11 (user): Mr. Zurkon as a weapon (TOD_ZURKON_WEAPON, src/stubs/tod_zurkon.cpp) in the pause grids. The pause scene's
 * weaponHandler (ARMI, chunk 9) and itemhandler (DISPOSITIVI, chunk 19) scripts build their grid in initWeaps: self.weapArray =
 * rows of WPN ids (<= 0 skipped; -2 = cell hidden), self.icon[WPN] = the icon model shown in the cell, self.movie[WPN] /
 * self.movie2[WPN] = the preview movie (WIKI 30 "Griglie ARMI / DISPOSITIVI"). The ARMI cell (4,1) exists and holds -2; the
 * DISPOSITIVI grid has Zurkon at (4,1). The scene object (Layer.weapon.weaponHandler) is a "behavior" userdata: the script's
 * self table is NOT reachable through it (pz1 probe 11/11: weapArray / initWeaps nil there). So the native is_weapon_owned
 * (0x33F10), which ARMI's updateCellData and DISPOSITIVI's changeSelection call as methods, runs this chunk once per script
 * table with the caller's register 0 (self; has_owned_item 0x2F778 for DISPOSITIVI's updateCellData): it wraps initWeaps and onActivate, and each time the host says (op 25) whether
 * the option is on, the cells follow (ARMI 4,1 = Zurkon / -2, DISPOSITIVI 4,1 = -2 / Zurkon). The game's scripts are not
 * changed. Op 26 reports (row 1 = wrapped, val 1 ARMI / 0 DISPOSITIVI). */
const char* const kZurkonHook =
    "return function(x) "
    "if type(x)~=\"table\" and type(x)~=\"userdata\" or x.tzDone then return end local A=x.weapArray "
    "if type(A)~=\"table\" or type(A[1])~=\"table\" or type(A[4])~=\"table\" then return end "
    "local isw if A[1][1]==WPN_COMBUSTER then isw=true elseif A[1][1]==WPN_SLINKONATOR then isw=false else return end "
    "x.tzDone=1 "
    "local function zon() local a,v=pcall(is_rumble_on,25,0,0) return a and v==1 end "
    "local function ap(y) local a=y.weapArray if type(a)~=\"table\" or type(a[4])~=\"table\" then return end local on=zon() "
    "if isw then if on then a[4][1]=WPN_ZURKON else a[4][1]=-2 end "
    "if type(y.icon)==\"table\" then y.icon[WPN_ZURKON]=\"mrzurkon\" end "
    "if type(y.movie)==\"table\" then y.movie[WPN_ZURKON]=\"DEVI_MR_ZURKON\" end "
    "if type(y.movie2)==\"table\" then y.movie2[WPN_ZURKON]=\"DEVI_MR_ZURKON\" end "
    "else if on then a[4][1]=-2 else a[4][1]=WPN_ZURKON end end "
    /* the -2 cell is hidden as a whole (cell opacity 0, zka 11/11): show / hide cell (4,1) itself */
    "local c=type(y.hex)==\"table\" and type(y.hex[4])==\"table\" and y.hex[4][1] "
    "if c then local v=0 if (isw and on) or (not isw and not on) then v=100 end pcall(function() c.opacity=v end) end end "
    "local wi=x.initWeaps if type(wi)==\"function\" then x.initWeaps=function(y) wi(y) ap(y) end end "
    "local oa=x.onActivate if type(oa)==\"function\" then x.onActivate=function(y) ap(y) return oa(y) end end "
    "ap(x) pcall(is_rumble_on,26,1,isw and 1 or 0) end";
/* Runs kZurkonHook once per caller script table (register 0 of the calling Lua frame: a table or an Anark object). */
void zurkon_menu_check(ppu_context* ctx)
{
    const uint32_t L = (uint32_t)ctx->gpr[3];
    static uint32_t s_seen[64]; static int s_ns;
    const uint32_t ci = vm_read32(L + 0x14);
    uint32_t self_tv = 0, tab = 0;
    if (ci >= 0x10018u) { const uint32_t base = vm_read32(ci - 24u);
        const uint32_t tt = base >= 0x10000u ? vm_read32(base) : 0;
        if (tt == 5u || tt == 7u) { self_tv = base; tab = vm_read32(base + 4); } }   /* a table, or an Anark object (userdata) */
    bool seen = !tab;
    for (int i = 0; i < 64 && !seen; i++) seen = s_seen[i] == tab;
    if (seen) return;
    s_seen[s_ns++ & 63] = tab;
    const uint64_t r1 = ctx->gpr[1], r2 = ctx->gpr[2], r3 = ctx->gpr[3], r4 = ctx->gpr[4], lr = ctx->lr;
    tod_lua_exec_L_quiet(L, kZurkonHook, self_tv);
    ctx->gpr[1] = r1; ctx->gpr[2] = r2; ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->lr = lr;
}
/* func_00033F10 = native is_weapon_owned, func_0002F778 = native has_owned_item (declared by ppu_recomp.h) */
void host_weapon_owned(ppu_context* ctx) { zurkon_menu_check(ctx); func_00033F10(ctx); }
void host_has_owned_item(ppu_context* ctx) { zurkon_menu_check(ctx); func_0002F778(ctx); }
int s_quit_on = 1, s_quit_slide = 1, s_opt_slide = 5;   /* TOD_PAUSE_OPT_SLIDE: the Options list layout (5 = seven rows, centred; 3 = eight rows, one empty at the bottom) */
int s_opt_page = 0;                          /* 10/11: op 22 / 23 */
uint32_t s_quit_found_L;                     /* op 12: the Lua state whose list got the item */
void host_get_current_level(ppu_context* ctx)
{
    const uint32_t L = (uint32_t)ctx->gpr[3];
    static uint32_t s_seen[32];
    static uint8_t s_tries[32];
    static int s_nd;
    int k = -1;
    for (int i = 0; i < s_nd; i++) if (s_seen[i] == L) k = i;
    if (k < 0) { k = s_nd < 32 ? s_nd++ : 31; s_seen[k] = L; s_tries[k] = 0; }
    if (getenv("TOD_QUIT_DUMP")) {   /* 11/11: how often, and with which caller, the game asks (pause opened twice) */
        static int s_nc; const uint32_t ci0 = vm_read32(L + 0x14), b0 = ci0 >= 0x10018u ? vm_read32(ci0 - 24u) : 0;
        if (s_nc++ < 60) fprintf(stderr, "[quit] get_current_level call %d on %08X: caller tt %u val %08X, found %d, frame %u\n", s_nc, L,
                                 b0 >= 0x10000u ? vm_read32(b0) : 0, b0 >= 0x10000u ? vm_read32(b0 + 4) : 0, s_quit_found_L == L, g_rsx_engine_frame);
    }
    /* 11/11: after the list was found, again at most once a frame: the game rebuilds the pause list at every opening (3 calls,
     * one frame); before that, up to 40 tries per state */
    static uint32_t s_last_frame = ~0u;
    const bool again = s_quit_found_L == L && s_last_frame != g_rsx_engine_frame;
    if (s_quit_on && ((s_quit_found_L != L && s_tries[k] < 40) || again)) {
        s_last_frame = g_rsx_engine_frame;
        if (s_tries[k] < 250) s_tries[k]++;
        std::string lab;
        for (const char* c = tod_menu_text(143); *c; c++) { if (*c == '"' || *c == '\\') lab += '\\'; lab += *c; }
        std::string glab;                    /* 10/11: GRAPHICS OPTIONS (TOD_GFX_PAGE=0: not added) */
        for (const char* c = tod_menu_text(164); *c; c++) { if (*c == '"' || *c == '\\') glab += '\\'; glab += *c; }
        static int s_gfx_item = -1; if (s_gfx_item < 0) { const char* e = getenv("TOD_GFX_PAGE"); s_gfx_item = (e && e[0] == '0') ? 0 : 1; }
        char src[3000];
        std::string clab;                    /* 10/11: CHEAT (in the Extras list) */
        for (const char* c = tod_menu_text(165); *c; c++) { if (*c == '"' || *c == '\\') clab += '\\'; clab += *c; }
        snprintf(src, sizeof src, kQuitHook, lab.c_str(), s_gfx_item, glab.c_str(), s_gfx_item, clab.c_str(), s_gfx_item, s_gfx_item, s_quit_slide, s_opt_slide, s_gfx_item);
        uint32_t self_tv = 0;                /* the caller's register 0 */
        const uint32_t ci = vm_read32(L + 0x14);
        if (ci >= 0x10000u) { const uint32_t base = vm_read32(ci - 24u); if (base >= 0x10000u && (vm_read32(base) == 5u || vm_read32(base) == 7u)) self_tv = base;   /* a table, or an Anark object (userdata, 7) */ }
        if (s_tries[k] <= 12 && getenv("TOD_QUIT_DUMP")) {   /* layout probe: the CallInfos below the native's */
            const uint32_t c0 = vm_read32(L + 0x14), bci = vm_read32(L + 0x28);
            fprintf(stderr, "[quit] run %d ci=%08X base_ci=%08X:", s_tries[k], c0, bci);
            for (uint32_t c = c0; c >= bci && c + 0x60 > c0; c -= 24u) { const uint32_t bs = vm_read32(c);
                fprintf(stderr, " [ci %08X st %u base %08X tt0 %u tt-1 %u]", c, vm_read32(c + 8), bs, vm_read32(bs), vm_read32(bs - 8)); }
            fprintf(stderr, "\n");
        }
        const uint64_t r1 = ctx->gpr[1], r2 = ctx->gpr[2], r3 = ctx->gpr[3], lr = ctx->lr;
        const int ok = tod_lua_exec_L(L, src, self_tv);
        ctx->gpr[1] = r1; ctx->gpr[2] = r2; ctx->gpr[3] = r3; ctx->lr = lr;
        if (s_tries[k] <= 3 || s_quit_found_L == L || getenv("TOD_QUIT_DUMP"))
            fprintf(stderr, "[quit] pause-menu hook run %d on Lua state %08X: %s, caller self %s, list %s\n", s_tries[k], L,
                    ok == 2 ? "ok, called with the caller" : ok ? "ok" : "failed", self_tv ? "table" : "-", s_quit_found_L == L ? "FOUND (QUIT added)" : "not yet");
    }
    func_00030810(ctx);
}

/* alert10: QUIT on the main menu. The bar (NUOVO GIOCO / CARICA / ELIMINA / OPZIONI) is the frontend scene's frontendHandler;
 * its four items are fixed scene objects (no spare, and the script cannot make new ones), so the fifth item is drawn by the
 * overlay (src/ui/tod_overlay.cpp, the game's wide font) and lives in the host:
 *  - every frame the handler's onUpdate calls the native save_load_is_interface_displayed (frontend table, 0x39D400); the
 *    hook runs a small function (cached in the global tqFE) with the handler object, which reports through the bridge
 *    (op 14) the current item and the items' positions;
 *  - the pad filter (g_cellPad_filter, cellPad.c) sees the words handed to the game: on the last item RIGHT selects QUIT
 *    (the game never sees it); while QUIT is selected the game sees an untouched pad, LEFT / CIRCLE go back, CROSS opens
 *    the "Quit the game?" box (tod_exit_open, run from the hook on the guest thread like the pause QUIT);
 *  - the hook greys / lights the game's last item through its own colours while QUIT is selected.
 * TOD_MAIN_QUIT=0 turns it off. */
std::atomic<uint64_t> s_fe_seen{0};          /* GetTickCount64() of the last main-menu frame */
std::atomic<int> s_fe_sel{0}, s_fe_n{0};     /* the game's current item (1-based) and the number of items */
std::atomic<int> s_esci{0};                  /* 1: our QUIT is the selected item */
std::atomic<int> s_esci_req{0};              /* to the hook: 1 grey the game's item, 2 light it again, 3 open the box */
std::atomic<int> s_esci_shown{0};            /* what the hook last applied (1 greyed) */
std::atomic<int> s_fe_dirty{1};              /* a key went down: read the game's item again on the next frame */
std::atomic<int> s_fe_main{0};               /* the handler's onMainMenu: 1 true, 0 false, 2 a function, 3 nil */
std::atomic<int> s_fe_iface{0};              /* the save / load list is on screen (the native's result) */
std::atomic<int> s_fe_alert{0};              /* "Quit the game?" is open: the guest thread waits in it, the bar stays drawn */
float s_fe_x[8], s_fe_y[8];                  /* the items' positions (the scene's units), from op 14 */
int s_fe_main_on = 1;

/* Installed ONCE per Lua state (an exec leaves 2 CallInfos / 1 nCcalls behind, and repeated execs stop the menu's input,
 * 09/11 mq10-mq12): the global save_load_is_interface_displayed, which onUpdate calls every frame, becomes a Lua wrapper that
 * reports the handler's state (op 14: current item, items, onMainMenu) and gets back a request (1 grey the last item, 2 light
 * it again), then calls the native. So the game runs our code as its own, with no exec from the host. */
/* 09/11 mq22: every call of the bridge (is_rumble_on -> host_pc_opt) from Lua leaves 2 CallInfos and 1 nCcalls behind, so
 * calling it every frame ends in "C stack overflow" after ~200 frames. The native this wrapper wraps does not leak: the
 * state rides on it as extra arguments (marker 7071, item, items + 100 * onMainMenu) and the request comes back as an
 * extra result (host_fe_update). */
const char* const kFeHook =   /* globals only: an upvalue of the exec'd function reads garbage later (its frame is never closed, mq17) */
    "return function(s) if tqOrig or not s.listItem then return end tqSelf=s tqOrig=save_load_is_interface_displayed "
    "save_load_is_interface_displayed=function(a,b,c) local s=tqSelf local n=table.getn(s.listItem) local m=s.onMainMenu "
    /* once: the four items move left so that the bar with QUIT stays centred (the game's own moveOption, mq34) */
    "if not tqMoved then tqMoved=1 for i=1,n do pcall(function() local x=s.listItem[i].position.x s:moveOption(i,x+(tqShift or -60)) end) end end "
    "local r,req=tqOrig(7071,s.currentSelection or 0,n+100*((m==true or m==1) and 1 or (type(m)=='function' and 2 or (m==nil and 3 or 0)))) "
    "if type(r)=='number' then req=r r=nil end "
    "if req==1 or req==2 then local col=req==1 and s.normalColor or s.highlightColor "
    "local function paint(o) if o then if not pcall(function() o.textColor:setColor(col) end) then pcall(function() o:setColor(col) end) end end end "
    "paint(s.listItem[n]) if s.mirrorItem then paint(s.mirrorItem[n]) end end "
    "return r end is_rumble_on(17,1,2) end";
uint32_t s_fe_hooked_L;                      /* the Lua state whose global got the wrapper (op 17) */

/* a number argument of the running C function (TValue = {int tt; float v}, tt 3 = number), or def */
float fe_arg(uint32_t L, int idx, float def)
{
    const uint32_t base = vm_read32(L + 0xC), top = vm_read32(L + 8), tv = base + 8u * (uint32_t)(idx - 1);
    if (tv + 8u > top || vm_read32(tv) != 3u) return def;
    const uint32_t bits = vm_read32(tv + 4); float f; memcpy(&f, &bits, 4); return f;
}

/* the state the wrapper reports each frame; returns the request for it (1 grey the game's last item, 2 light it, 0 none) */
int fe_state(int sel, int v)
{
    const int m = v / 100;
    s_fe_sel.store(sel); s_fe_n.store(v % 100);
    if (m != s_fe_main.exchange(m)) fprintf(stderr, "[mainquit] onMainMenu %d, item %d of %d\n", m, sel, v % 100);
    int req = s_esci_req.exchange(0);
    if (req == 3) { s_fe_alert.store(1); tod_exit_open(); s_fe_alert.store(0); s_fe_seen.store(GetTickCount64()); req = 0; }   /* the box blocks here, on the guest thread, like the pause QUIT */
    if (req == 1 || req == 2) { s_esci_shown.store(req == 1 ? 1 : 0); fprintf(stderr, "[mainquit] request %d to the script (item %d of %d)\n", req, sel, v % 100); }
    return req;
}

void host_fe_update(ppu_context* ctx)
{
    const uint32_t L = (uint32_t)ctx->gpr[3];
    if (!s_toc) s_toc = ctx->gpr[2];
    static uint32_t s_triedL; static int s_tries;
    if (s_fe_main_on) s_fe_seen.store(GetTickCount64());
    if (s_fe_main_on && s_fe_hooked_L != L) {
        if (s_triedL != L) { s_triedL = L; s_tries = 0; }
        if (s_tries < 5) {
            s_tries++;
            uint32_t self_tv = 0;
            const uint32_t ci = vm_read32(L + 0x14);
            if (ci >= 0x10000u) { const uint32_t base = vm_read32(ci - 24u); if (base >= 0x10000u && (vm_read32(base) == 5u || vm_read32(base) == 7u)) self_tv = base; }
            if (self_tv) {
                const uint64_t r1 = ctx->gpr[1], r2 = ctx->gpr[2], r3 = ctx->gpr[3], lr = ctx->lr;
                /* 10/11: the shift that centres the bar with QUIT in this language (scene units, see tod_fe_shift720) */
                char pre[48]; snprintf(pre, sizeof pre, "tqShift=%.2f ", (double)(tod_fe_shift720() / 1.0167f));
                const std::string src = std::string(pre) + kFeHook;
                const int ok = tod_lua_exec_L(L, src.c_str(), self_tv);
                ctx->gpr[1] = r1; ctx->gpr[2] = r2; ctx->gpr[3] = r3; ctx->lr = lr;
                fprintf(stderr, "[mainquit] install on %08X try %d: %d, %s\n", L, s_tries, ok, s_fe_hooked_L == L ? "wrapper in place" : "not yet");
            }
        }
    }
    const bool ours = s_fe_main_on && fe_arg(L, 1, 0.0f) == 7071.0f;   /* called by our wrapper with the state */
    const int sel = ours ? (int)fe_arg(L, 2, 0.0f) : 0, v = ours ? (int)fe_arg(L, 3, 0.0f) : 0;
    if (ours) vm_write32(L + 8, vm_read32(L + 0xC));   /* the native takes no argument (tolua: "[no object] expected") */
    const uint64_t lr = ctx->lr;
    func_0039D400(ctx);
    if (ours) {
        const uint64_t nres = ctx->gpr[3];
        { const uint32_t t = vm_read32(L + 8);   /* the native's own result: is the save / load list on screen (bool) */
          const int on = nres >= 1 && vm_read32(t - 8u) == 1u && vm_read32(t - 4u) != 0u ? 1 : 0;
          static uint64_t s_last = ~0ull; const uint64_t key = ((uint64_t)vm_read32(t - 8u) << 32) | vm_read32(t - 4u);
          if (key != s_last) { s_last = key; fprintf(stderr, "[mainquit] native result tt %u val %08X (n %d) -> list %d\n", (unsigned)(key >> 32), (unsigned)key, (int)nres, on); }
          s_fe_iface.store(on); }
        const int req = fe_state(sel, v);
        /* the extra result written straight to the stack (TValue {3, float}): no guest call (push_number leaked, mq25) */
        const uint32_t top = vm_read32(L + 8); const float f = (float)req; uint32_t bits; memcpy(&bits, &f, 4);
        vm_write32(top, 3u); vm_write32(top + 4, bits); vm_write32(L + 8, top + 8u);
        ctx->gpr[3] = nres + 1; ctx->lr = lr;
    }
}

/* cellPad.c, guest thread, once per GetData of port 0 (after PAD_REPLAY): button[2] DIGITAL1, [3] DIGITAL2, [4..7] rx ry lx ly */
void fe_pad_filter(uint16_t* b)
{
    static uint16_t prev1, prev2, prev_lx = 128;
    const uint16_t d1 = b[2], d2 = b[3];
    const uint16_t e1 = d1 & ~prev1, e2 = d2 & ~prev2;
    prev1 = d1; prev2 = d2;
    if ((e1 & 0xF0) || (e2 & 0x60) || ((b[6] > 200 || b[6] < 56) != ((prev_lx > 200) || (prev_lx < 56)))) { s_fe_dirty.store(1); }
    prev_lx = b[6];
    const bool on_menu = GetTickCount64() - s_fe_seen.load() < 300 && s_fe_main.load() == 1 && !s_fe_iface.load();
    if (!on_menu) { if (s_esci.load()) { s_esci.store(0); } return; }
    const int n = s_fe_n.load(), sel = s_fe_sel.load();
    const bool right = (e1 & 0x20) != 0, left = (e1 & 0x80) != 0, cross = (e2 & 0x40) != 0, circle = (e2 & 0x20) != 0;
    const bool stick_r = b[6] > 200, stick_l = b[6] < 56;
    static bool sr_prev, sl_prev;
    const bool sr_e = stick_r && !sr_prev, sl_e = stick_l && !sl_prev;
    sr_prev = stick_r; sl_prev = stick_l;
    /* the key that left (or entered) QUIT stays hidden from the game until it is released, else the game sees it go down
     * on the next frame and moves too (mq28) */
    static uint16_t hide1, hide2; static bool hide_stick;
    hide1 &= d1; hide2 &= d2; if (!stick_l && !stick_r) hide_stick = false;
    b[2] &= (uint16_t)~hide1; b[3] &= (uint16_t)~hide2; if (hide_stick) b[6] = 128;
    if (!s_esci.load()) {
        if (n > 0 && sel == n) {                      /* on the last item: RIGHT is ours (the game must not wrap) */
            if (right || sr_e) { s_esci.store(1); s_esci_req.store(1); hide1 |= 0x20; hide_stick = sr_e; fprintf(stderr, "[mainquit] QUIT selected\n"); }
            b[2] &= (uint16_t)~0x20; if (b[6] > 128) b[6] = 128;
        }
        return;
    }
    /* QUIT selected: the game sees nothing */
    b[2] = 0; b[3] = 0; b[4] = b[5] = b[6] = b[7] = 128;
    for (int i = 8; i < 20; i++) b[i] = 0;
    if (left || sl_e || circle) { s_esci.store(0); s_esci_req.store(2); hide1 |= d1 & 0x80; hide2 |= d2 & 0x60; hide_stick = sl_e; fprintf(stderr, "[mainquit] back to the game's items\n"); }
    else if (cross) { s_esci_req.store(3); hide2 |= 0x40; fprintf(stderr, "[mainquit] QUIT pressed\n"); }
}

/* 10/11, issue #25 point 2: button hints (SELECT / BACK) under the pause menu's lists (main list, Options, Extras), which the
 * game draws without any (MISSION and the other pages have their own). The list's onUpdate (pause scene, ListUpdater) sets
 * self.isActive = listBehavior.hasFocus and then calls the native save_load_is_interface_displayed (pause / in-game table,
 * 0x2B960) every frame. As on the main menu (kFeHook), that global becomes a wrapper, installed once and only in the Lua
 * state where kQuitHook found the list, that passes the state as extra arguments (marker 7072, isActive): no bridge call per
 * frame. The overlay draws the hints while frames keep coming with isActive and the save / load list is not up
 * (tod_pause_hint_state). TOD_PAUSE_HINTS=0 turns it off. */
const char* const kPauseHintHook =
    "return function(s) if tqPO or type(s)==\"nil\" or type(s.menuData)~=\"table\" or not s.MAIN_MENU_INDEX then return end "
    "tqPS=s tqPO=save_load_is_interface_displayed "
    "save_load_is_interface_displayed=function() local s=tqPS return tqPO(7072,(s.isActive and 1 or 0),(s.currentMenuIndex==s.MAIN_MENU_INDEX) and 1 or 0) end "
    "pcall(is_rumble_on,18,1,2) end";
int s_ph_on = 1;
uint32_t s_ph_hooked_L;                      /* op 18: the Lua state whose global got the wrapper */
std::atomic<uint64_t> s_ph_seen{0};          /* GetTickCount64() of the last frame the list was up with the focus */
void host_pause_update(ppu_context* ctx)
{
    const uint32_t L = (uint32_t)ctx->gpr[3];
    if (s_ph_on && s_ph_hooked_L != L && L == s_quit_found_L) {
        static uint32_t s_triedL; static int s_tries;
        if (s_triedL != L) { s_triedL = L; s_tries = 0; }
        if (s_tries < 3) {
            s_tries++;
            uint32_t self_tv = 0;
            const uint32_t ci = vm_read32(L + 0x14);
            if (ci >= 0x10000u) { const uint32_t base = vm_read32(ci - 24u); if (base >= 0x10000u && (vm_read32(base) == 5u || vm_read32(base) == 7u)) self_tv = base; }
            if (self_tv) {
                const uint64_t r1 = ctx->gpr[1], r2 = ctx->gpr[2], r3 = ctx->gpr[3], lr = ctx->lr;
                const int ok = tod_lua_exec_L(L, kPauseHintHook, self_tv);
                ctx->gpr[1] = r1; ctx->gpr[2] = r2; ctx->gpr[3] = r3; ctx->lr = lr;
                fprintf(stderr, "[pausehint] install on %08X try %d: %d, %s\n", L, s_tries, ok, s_ph_hooked_L == L ? "wrapper in place" : "not yet");
            }
        }
    }
    {   /* 10/11 probe (test only): TOD_PAUSE_LUA = a chunk run once in the pause scene's Lua state, 120 calls after the hint
         * wrapper is in place (about 2 s of pause), with the caller object as the argument of the function it returns */
        static int s_pl_n = -1; static const char* s_pl = nullptr;
        if (s_pl_n == -1) { s_pl = getenv("TOD_PAUSE_LUA"); s_pl_n = 0; }
        if (s_pl && L == s_ph_hooked_L && ++s_pl_n == 120) {
            uint32_t self_tv = 0;
            const uint32_t ci = vm_read32(L + 0x14);
            if (ci >= 0x10000u) { const uint32_t base = vm_read32(ci - 24u); if (base >= 0x10000u && (vm_read32(base) == 5u || vm_read32(base) == 7u)) self_tv = base; }
            const uint64_t r1 = ctx->gpr[1], r2 = ctx->gpr[2], r3 = ctx->gpr[3], lr = ctx->lr;
            const int ok = tod_lua_exec_L(L, s_pl, self_tv);
            ctx->gpr[1] = r1; ctx->gpr[2] = r2; ctx->gpr[3] = r3; ctx->lr = lr;
            fprintf(stderr, "[pauselua] ran on %08X: %d (self %s)\n", L, ok, self_tv ? "yes" : "no");
        }
    }
    const bool ours = s_ph_on && fe_arg(L, 1, 0.0f) == 7072.0f;   /* called by our wrapper with the state */
    /* 10/11 (user): not under the main list, where the row runs over the panel's frame; under the Options / Extras lists only */
    const int active = ours ? ((int)fe_arg(L, 2, 0.0f) && (int)fe_arg(L, 3, 0.0f) == 0 ? 1 : 0) : 0;
    if (ours) vm_write32(L + 8, vm_read32(L + 0xC));   /* the native takes no argument */
    func_0002B960(ctx);
    if (ours) {
        const uint32_t t = vm_read32(L + 8);           /* the native's result: the save / load list is on screen (bool) */
        const int iface = ctx->gpr[3] >= 1 && vm_read32(t - 8u) == 1u && vm_read32(t - 4u) != 0u ? 1 : 0;
        static int s_last = -1; const int st = active * 2 + iface;
        if (st != s_last) { s_last = st; fprintf(stderr, "[pausehint] list %s, save/load list %d\n", active ? "has the focus" : "not active", iface); }
        if (active && !iface) s_ph_seen.store(GetTickCount64());
    }
}

/* 10/11, game option "all weapons in every shop" (TOD_SHOP_ALL_WEAPONS: 1 = all but the RYNO, 2 = the RYNO too). The weapon
 * vendor's script (scene weaponvendor, ListUpdater setAvailableWeaps) lists the weapons of its weaponArray (Combuster .. RYNO,
 * enum 1..15) for which the native is_weapon_available (0x33E38, Lua wrapper of func_000265C8) answers true; only that scene
 * calls it. Here the native runs as before and its boolean (L->top - 8, Lua 5.0 TValue {tt 1, value}) is turned true. Read
 * at every call, so the Options row applies the next time a shop opens. */
void host_weapon_available(ppu_context* ctx)
{
    const uint32_t L = (uint32_t)ctx->gpr[3];
    const int w = (int)fe_arg(L, 1, -1.0f);
    func_00033E38(ctx);
    const char* e = getenv("TOD_SHOP_ALL_WEAPONS");
    const int mode = e ? atoi(e) : 0;
    if (mode <= 0 || w < 1 || w > 15 || (w == 15 && mode < 2)) return;
    /* only a weapon the player does not own (slot w * 20 + 0x10 of the player data [0x89F1B8]: 1 for all 15 on a save that owns
     * them all, where the game's is_weapon_owned agrees, s9_shop): an owned one the game does not sell (all levels bought) would
     * be sold again for nothing (s5/s10: -40000 bolts, no change) */
    const uint32_t pd = vm_read32(0x0089F1B8u);
    if (pd < 0x10000u || vm_read8(pd + (uint32_t)w * 20u + 0x10u) != 0) return;
    const uint32_t top = vm_read32(L + 0x08);
    if (top >= 0x10008u && vm_read32(top - 8u) == 1u && vm_read32(top - 4u) == 0u) {
        vm_write32(top - 4u, 1u);
        static int s_n; if (s_n++ < 20) fprintf(stderr, "[shop] weapon %d made available (TOD_SHOP_ALL_WEAPONS=%d)\n", w, mode);
    }
}

/* 10/11 diagnostics while TOD_SHOP_ALL_WEAPONS is on: the weapon bought (native purchase_weapon, 0x337A8) and its slot's
 * slot's +0x10 / +0x11 bytes before / after (player data [0x89F1B8], slot w * 20). NB 10/11: on a save that owns every weapon
 * +0x10 reads 1 for all 15 and the game's is_weapon_owned agrees, so +0x10 is not "owned" alone: mappa 0089F1B8 to re-check */
void host_purchase_weapon(ppu_context* ctx)
{
    const uint32_t L = (uint32_t)ctx->gpr[3];
    const int w = (int)fe_arg(L, 1, -1.0f);
    const uint32_t pd = vm_read32(0x0089F1B8u);
    const int b0 = (w >= 0 && w < 32 && pd >= 0x10000u) ? vm_read8(pd + (uint32_t)w * 20u + 0x10u) : -1;
    func_000337A8(ctx);
    const int b1 = (w >= 0 && w < 32 && pd >= 0x10000u) ? vm_read8(pd + (uint32_t)w * 20u + 0x10u) : -1;
    const uint32_t top = vm_read32(L + 0x08);
    fprintf(stderr, "[shop] purchase_weapon(%d): owned %d -> %d, level %d, result tt %u val %u\n", w, b0, b1,
            (w >= 0 && w < 32 && pd >= 0x10000u) ? vm_read8(pd + (uint32_t)w * 20u + 0x11u) : -1, vm_read32(top - 8u), vm_read32(top - 4u));
}

/* 10/11: the Options page's sections (optHandler.lua: the first row picks one). 1 game (with the game's own rows), 2 graphics,
 * the controller rows with the game ones (user, 10/11). A row not listed here goes to the game section. */
int row_section(int r)
{
    static const char* const kGfx[] = { "RSX_SCALE", "RSX_DISPLAY", "TOD_ASPECT", "RSX_AA", "RSX_ANISO", "RSX_TEX_LOD_BIAS",
        "RSX_SHADOW_SCALE", "RSX_BLOOM_SCALE", "RT_AO", "RT_AO_QUALITY", "RSX_SSR", "RSX_VSYNC", "TOD_FPS", "TOD_FPS_COUNTER" };
    static const char* const kPad[] = { "TOD_PROMPTS", "TOD_DS_TRIGGERS", "TOD_DS_TRIGGER_STRENGTH", "TOD_DS_FIRE_R2",
        "TOD_PAD_SWAP_R1R2", "TOD_PAD_SWAP_L1L2" };
    const char* k = kRowTab[r - 1].key;
    if (k[0] == '!') return 3;                 /* the CHEAT page */
    for (const char* g : kGfx) if (!strcmp(k, g)) return 2;
    for (const char* g : kPad) if (!strcmp(k, g)) return 1;   /* 10/11 (user): the controller rows go with the game page */
    return 1;
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
    /* alert10 (mq22, rs2): a DIRECT Lua call of this bridge leaves 2 CallInfos + 1 nCcalls behind ("C stack overflow" after
     * ~200); through pcall (the Options page, the pause QUIT) nothing is left: >700 calls with ci / nCcalls unchanged. So the
     * scripts call it only through pcall, and the main-menu hook does not call it every frame (see kFeHook). */
    switch (op) {
    case -1: push_number(ctx, L, 7071.0); break;       /* no argument at all: the handshake */
    case 0:  push_number(ctx, L, (double)kRows); break;
    case 1:  push_number(ctx, L, ok ? (double)row_text(row) : 0.0); break;
    case 2:  if (ok) row_step(row, val > 0 ? 1 : val < 0 ? -1 : 0); push_number(ctx, L, ok ? (double)row_text(row) : 0.0); break;
    case 3:  push_number(ctx, L, ok ? (double)kRowTab[row - 1].label : 0.0); break;
    case 4:  push_number(ctx, L, ok ? (double)kRowTab[row - 1].help : 0.0); break;
    case 7:  push_number(ctx, L, (double)tx_char(row, (int)val)); break;   /* char (val) of text (row), see tx_char */
    case 24: {   /* 10/11 probe (TOD_PAUSE_LUA): print the string argument 2 ("print" of the UI states goes nowhere) */
        const uint32_t tv = vm_read32((uint32_t)L + 0xC) + 8u;
        if (tv + 8u <= vm_read32((uint32_t)L + 8) && vm_read32(tv) == 4u) {
            const uint32_t ts = vm_read32(tv + 4), len = vm_read32(ts + 12);
            char msg[400]; uint32_t k = 0;
            for (; k < len && k < sizeof msg - 1; k++) msg[k] = (char)vm_read8(ts + 16 + k);
            msg[k] = 0;
            fprintf(stderr, "[PL] %s\n", msg);
        }
        push_number(ctx, L, 0.0); break; }
    case 6:  push_number(ctx, L, ok && kRowTab[row - 1].key[0] != '!' ? (kRowTab[row - 1].hot ? 141.0 : 142.0) : 0.0); break;   /* help tail: at once / next start (none on the CHEAT page) */
    case 5:  read_env(); memcpy(s_idx_open, s_idx, sizeof s_idx); push_number(ctx, L, s_preselect ? 1.0 : 0.0); break;   /* page opens: re-read; test hook TOD_MENU_PRESELECT=1 */
    case 10: {   /* alert10: the page closes (onBack): a "next start" row changed since it opened -> "Restart the game?" */
        int changed = 0;
        for (int i = 0; i < kRows; i++) if (!kRowTab[i].hot && s_idx[i] != s_idx_open[i]) { changed++; fprintf(stderr, "[restart] %s changed\n", kRowTab[i].key); }
        memcpy(s_idx_open, s_idx, sizeof s_idx);
        if (changed) tod_restart_open();     /* blocks here, on the guest thread, like the pause QUIT; YES does not come back */
        push_number(ctx, L, (double)changed); } break;
    case 9:  tod_quit_open(); push_number(ctx, L, 1.0); break;
    case 12: s_quit_found_L = (uint32_t)L; push_number(ctx, L, 1.0); break;   /* quit125: the hook found the list (see kQuitHook) */
    case 14: push_number(ctx, L, (double)fe_state(row, (int)val)); break;   /* alert10: old channel (leaks, see kFeHook) */
    case 11: push_number(ctx, L, ok ? (double)row_section(row) : 1.0); break;   /* 10/11: the page's section of a row */
    case 22: s_opt_page = row; push_number(ctx, L, 1.0); break;               /* 10/11: the pause item that opens the page (1 game, 2 graphics) */
    case 23: push_number(ctx, L, (double)s_opt_page); s_opt_page = 0; break;   /* handed over once (the main menu's page asks too and gets 0) */
    case 25: push_number(ctx, L, (double)tod_zurkon_on()); break;              /* 11/11: kZurkonHook asks whether Mr. Zurkon is a weapon */
    case 26: { static int n; if (n++ < 20) fprintf(stderr, "[zurkon] menu hook: row %d val %g\n", row, val); push_number(ctx, L, 1.0); } break;   /* 11/11: kZurkonHook reports */
    case 18: s_ph_hooked_L = (uint32_t)L; fprintf(stderr, "[pausehint] wrapper installed\n"); push_number(ctx, L, 1.0); break;   /* 10/11 */
    case 17: s_fe_hooked_L = (uint32_t)L; fprintf(stderr, "[mainquit] wrapper in the script env (%s)\n", val == 1 ? "SAME as globals" : "own env"); push_number(ctx, L, 1.0); break;   /* alert10 */
    case 15: if (row >= 1 && row < 8) s_fe_x[row] = (float)val; push_number(ctx, L, 1.0); break;
    case 16: if (row >= 1 && row < 8) { s_fe_y[row] = (float)val; fprintf(stderr, "[mainquit] item %d at %g %g\n", row, s_fe_x[row], s_fe_y[row]); } push_number(ctx, L, 1.0); break;
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
    { const char* q = getenv("TOD_PAUSE_QUIT"); s_quit_on = (q && q[0] == '0') ? 0 : 1; }
    { const char* q = getenv("TOD_PAUSE_QUIT_SLIDE"); s_quit_slide = (q && q[0] == '0') ? 0 : 1; }
    { const char* q = getenv("TOD_PAUSE_OPT_SLIDE"); if (q && q[0] >= '1' && q[0] <= '9') s_opt_slide = atoi(q); }
    if (s_quit_on) ppu_register_function(0x00030810u, host_get_current_level);   /* quit125 */
    ppu_register_function(0x0002F778u, host_has_owned_item);   /* 11/11: the same, DISPOSITIVI */
    ppu_register_function(0x00033F10u, host_weapon_owned);   /* 11/11: TOD_ZURKON_WEAPON, the ARMI / DISPOSITIVI cells (kZurkonHook) */
    ppu_register_function(0x00033E38u, host_weapon_available);   /* 10/11: TOD_SHOP_ALL_WEAPONS (does nothing while it is 0) */
    if (const char* da = getenv("TOD_SHOP_DUMP_AT")) {   /* 10/11 test only: the weapon slots' bytes every 10 s from that second on */
        const int at = atoi(da);
        std::thread([at] {
            std::this_thread::sleep_for(std::chrono::seconds(at));
            for (int k = 0; k < 6; k++) {
                const uint32_t pd = vm_read32(0x0089F1B8u);
                char b[900]; int n = 0;
                for (uint32_t i = 0; pd >= 0x10000u && i < 24 && n < 860; i++)
                    n += snprintf(b + n, sizeof b - n, " %u:%u/%u/%u", i, vm_read32(pd + i * 20u), vm_read8(pd + i * 20u + 0x10u), vm_read8(pd + i * 20u + 0x11u));
                fprintf(stderr, "[shop] slots at %08X (index:id/+0x10/+0x11):%s\n", pd, b);
                std::this_thread::sleep_for(std::chrono::seconds(10));
            }
        }).detach();
    }
    { const char* sa = getenv("TOD_SHOP_ALL_WEAPONS"); if (sa && atoi(sa) > 0) ppu_register_function(0x000337A8u, host_purchase_weapon); }
    { const char* ph = getenv("TOD_PAUSE_HINTS"); s_ph_on = (ph && ph[0] == '0') ? 0 : 1;
      if (s_ph_on && s_quit_on) ppu_register_function(0x0002B960u, host_pause_update); else s_ph_on = 0; }   /* 10/11: #25 point 2 */
    { const char* mq = getenv("TOD_MAIN_QUIT"); s_fe_main_on = (mq && mq[0] == '0') ? 0 : 1;
      if (s_fe_main_on) { ppu_register_function(0x0039D400u, host_fe_update); g_cellPad_filter = fe_pad_filter; } }   /* alert10: QUIT on the main menu */
    read_env();
    pick_language();
    { const char* ps = getenv("TOD_MENU_PRESELECT"); s_preselect = (ps && *ps == '1') ? 1 : 0; }
    fprintf(stderr, "[menu] bridge pc_opt installed over is_rumble_on at 0x%08X and 0x%08X (%d rows, preselect %d)\n",
            kBridgeAddr[0], kBridgeAddr[1], kRows, s_preselect);
}

struct Starter { Starter() { std::thread(install).detach(); } } s_starter;

}  // namespace

/*
 * 10/11 (the user: the main menu's spacing changes from language to language). The game spreads its four items between two
 * fixed ends (at 1080p always 283..1457, i.e. 720p 189..971 after our shift, or 249.7..1032.3 unshifted) and the gaps follow
 * the words' widths: their median, at 720p, is English 66, Danish 58, Swedish 50, Finnish 48, Italian 45, Dutch 37, Portuguese 34,
 * Norwegian 31, French 26, Spanish 20, German 18 (spaces inside a label, NYTT SPEL, left out)
 * (lg_*_exit_t33.png, ar_169_bar_t33.png). QUIT takes the same gap as the game's items in that language, and the whole bar
 * (four items + gap + QUIT) is centred: the shift below moves the four items (the wrapper's moveOption, 1 scene unit = 1.0167
 * px at 720p) and the overlay draws QUIT at 1032.3 + shift + gap.
 */
extern "C" float tod_fe_gap720(void)
{
    const TxItem* t = s_tx_lang;
    if (t == kTxEn) return 66.0f;
    if (t == kTxIt) return 45.0f;
    if (t == kTxNl) return 37.0f;
    if (t == kTxPt) return 34.0f;
    if (t == kTxFr) return 26.0f;
    if (t == kTxEs) return 20.0f;
    if (t == kTxDe) return 18.0f;
    if (t == kTxSv) return 50.0f;
    if (t == kTxNo) return 31.0f;
    if (t == kTxDa) return 58.0f;
    if (t == kTxFi) return 48.0f;
    return 45.0f;
}

extern "C" float tod_fe_shift720(void)
{
    /* computed once (the first call is the wrapper's install), so the items and QUIT always use the same value */
    static float s = 1e9f;
    if (s > 1e8f) {
        const float L0 = 249.7f, R0 = 1032.3f;
        float we = tod_esci_w720();
        if (!(we > 1.0f)) we = 73.0f;                    /* "ESCI" measured: 73 px at 720p */
        const float total = (R0 - L0) + tod_fe_gap720() - 2.0f + we;   /* -2: see the overlay (QUIT's ink starts 2 px into its box) */
        s = (640.0f - total * 0.5f) - L0;
        fprintf(stderr, "[mainquit] bar: gap %.0f, QUIT %.1f px wide, shift %.1f px (720p)\n", (double)tod_fe_gap720(), (double)we, (double)s);
    }
    return s;
}

/* quit125: a text of tod_menu_text.inc in the game's language (English where it lacks one), for the overlay's quit box. */
extern "C" const char* tod_menu_text(int id)
{
    static bool picked = false;
    if (!picked) { picked = true; pick_language(); }
    const TxItem* it = tx_find(s_tx_lang, id);
    if (!it) it = tx_find(kTxEn, id);
    return it ? it->s : "";
}

/* 10/11: for the overlay. 1 while a pause-menu list is up with the focus (button hints) */
extern "C" int tod_pause_hint_state(void)
{
    return s_ph_on && s_ph_hooked_L && GetTickCount64() - s_ph_seen.load() < 120 ? 1 : 0;
}

/* alert10: for the overlay. 0 = not on the main menu, 1 = on it, 2 = our QUIT selected; *n items, x / y of the last one */
extern "C" int tod_fe_quit_state(int* n, float* x, float* y)
{
    if (s_fe_alert.load()) { *n = s_fe_n.load(); *x = *y = 0.0f; return 2; }
    if (!s_fe_main_on || GetTickCount64() - s_fe_seen.load() > 300 || s_fe_main.load() != 1 || s_fe_iface.load() || s_fe_hooked_L == 0) return 0;
    const int k = s_fe_n.load();
    *n = k; *x = (k >= 1 && k < 8) ? s_fe_x[k] : 0.0f; *y = (k >= 1 && k < 8) ? s_fe_y[k] : 0.0f;
    return s_esci.load() ? 2 : 1;
}
