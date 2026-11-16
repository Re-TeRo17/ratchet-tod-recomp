/*
 * The port's settings page: a Dear ImGui overlay drawn into the D3D12 engine's
 * present (rsx_d3d12_set_overlay, ps3recomp libs/video/rsx_d3d12_engine.h), which
 * edits tod.ini and applies what can change while the game runs.
 *
 * Open / close:   F1 on the keyboard, or SELECT held for 2 s on the pad (Esc, Circle
 *                 or a tap of SELECT close it). F8 (screenshot) is not touched.
 * Input:          while it is open the window's keyboard and mouse messages are taken
 *                 here (the engine's window procedure asks us first) and port 0 reads
 *                 as an untouched pad (g_cellPad_input_block, cellPad.c), so nothing
 *                 reaches the game. The pad's own buttons are fed in by
 *                 tod_overlay_pad_feed() from the pad poll (src/input/host_input.cpp).
 * Cost closed:    one atomic load per present. ImGui, its descriptor heap and its font
 *                 are made on the first open.
 * Threads:        the window thread queues events (overlay_message); the thread that
 *                 encodes the frame builds and records the page (overlay_render) and
 *                 is the only one that touches ImGui.
 *
 * Rows are tod.ini keys (RSX_SCALE, RSX_DISPLAY, RSX_WINDOW, RSX_VSYNC, TOD_FPS,
 * RSX_AA, RSX_ANISO, PS3_LANG, TOD_PROMPTS). "Apply" writes the changed ones into
 * tod.ini (comments and order kept, as dod3-recomp does) and applies the live ones: the
 * renderer re-reads its display settings at the window pump (g_rsx_display_reload),
 * anisotropy and anti-aliasing are plain globals, the button prompts re-read their
 * variable four times a second. TOD_FPS and PS3_LANG are read once at start: they say
 * "(requires restart)" until then. The rows under "Coming soon" are placeholders.
 *
 * The "Cheats" tab (tod_cheat.cpp): toggles (God mode, Infinite ammo, the game's Big head / Enemy big head /
 * Mirror level) and one-shot buttons (Heal, +100,000 bolts, All weapons) act at once, no Apply: each press
 * queues commands with tod_cheat_enqueue(), which the game's main thread runs at its next pad poll. They are
 * NOT saved in tod.ini (all off at every start); the toggles show what was sent this session (re-read from the
 * engine when the page opens). Greyed and inert until a level is running (tod_hero_handle() == 0).
 *
 * The "Controls" tab (keys136): one row per pad input (buttons, D-pad, left stick, camera) with the keyboard key or mouse
 * button bound to it (src/input/key_bindings.cpp; tod.ini KEY_CROSS = Space ...), the mouse camera on/off with its
 * sensitivity and button bindings (TOD_MOUSE*), and "Reset to defaults". Enter / a click on a key row waits for the
 * next key ("Press a key..."), Esc cancels, Backspace (or a right click) clears. Rows here apply and are written to
 * tod.ini at once (no Apply): they are Row.cheat == 3 rows, which every Apply / Discard / Defaults loop skips.
 *
 * TOD_OVERLAY=0 turns it off. TOD_OVERLAY_AUTO=<seconds>[:<tab>] opens it that long after
 * the first frame (screenshots of an unattended run). TOD_OVERLAY_SCRIPT="<sec>:<cmd>,<sec>:<cmd>,..." (debug)
 * drives the page without a mouse: sec = seconds since the first present, one entry per present at most;
 * cmd = open | close | tab=<n> | sel=<n> | act | left | right | apply | key=<name> | clear (key=: the key the
 * Controls tab is waiting for, by its tod.ini name; esc = cancel, backspace = clear).
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include <d3d12.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   /* alert10: ImFontAtlasTextureBlockQueueUpload for the game-font glyphs */
#include "backends/imgui_impl_dx12.h"

#include "rsx_d3d12_engine.h"
#include "settings.h"
#include "input/key_bindings.h"
#include "update/update_check.h"    /* 1.01: version, update check */

extern "C" {
extern volatile int g_cellPad_input_block;     /* cellPad.c */
void cellPad_poll(void);                       /* cellPad.c: poll the host pad now */
void rsx_d3d12_engine_repaint(void);           /* rsx_d3d12_engine.c: present the last frame again (+ overlay) */
typedef int (*cellSaveData_pick_hook_t)(int, int, const char* const*, const char* const*, int, int);
void cellSaveData_set_pick_hook(cellSaveData_pick_hook_t fn);   /* cellSaveData.c */
const char* cellSaveData_root(void);                           /* cellSaveData.c: the host save folder */
unsigned char* stbi_load_from_memory(const unsigned char* buf, int len, int* x, int* y, int* comp, int req_comp);   /* ps3recomp cellPngDec.c */
void stbi_image_free(void* p);
int tod_game_archive_read(const char* entry, unsigned char** data, size_t* size);   /* src/patches/menu_archive.cpp (alert10) */
int tod_fe_quit_state(int* n, float* x, float* y);
extern float g_tod_ui_k;                    /* src/main.cpp: 1 = off, else (16/9)/aspect (the HUD narrowing of TOD_ASPECT) */   /* tod_settings_menu.cpp (alert10): 0 off the main menu, 1 on it, 2 our QUIT selected */
const char* tod_menu_text(int id);
float tod_fe_gap720(void);                 /* tod_settings_menu.cpp (10/11): the main menu bar per language */
float tod_fe_shift720(void);
float tod_aspect_frame(void);              /* src/main.cpp (10/11): TOD_ASPECT's frame shape, 0 = off */
int tod_pause_hint_state(void);            /* tod_settings_menu.cpp (10/11): a pause-menu list is up with the focus */
int tod_prompts_glyph(uint32_t cp, uint8_t* rgba, int cap, int* w, int* h);   /* src/patches/prompts.cpp (10/11) */
extern "C" void tod_fps_apply_live(const char* v);   /* main.cpp (usr123): the FPS limit, applied live */
extern "C" void tod_level_go(const char* level_const, const char* folder, const char* spot);   /* vram124 */
extern "C" int tod_level_go_state(void);
extern "C" void tod_lua_enqueue(const char* chunk);
extern "C" void tod_tp_log(const char* fmt, ...);   /* tod_logv2_host.cpp: stderr + "teleport" event */   /* tod_cheat.cpp (usr123): a Lua chunk for the guest thread */
extern "C" void tod_logv2_mark(void);                 /* tod_logv2_host.cpp: F7 "mark" event in the structured log */
extern int (*g_rsx_level_hook)(char* out, size_t n);  /* rsx_draw_engine.c: the level the loader names in guest memory */
extern volatile double g_rsx_gpu_busy_ms_total;      /* rsx_d3d12_engine.c (usr123): cumulative GPU time of the lists (ms) */
extern volatile int g_rsx_gpu_load_ok;
extern int (*g_rsx_vram_hook)(unsigned long long* used, unsigned long long* budget);   /* rsx_draw_engine.c */
extern volatile int g_rsx_display_reload;      /* rsx_draw_engine.c: the renderer re-reads its display settings */
extern int g_rsx_aniso;                        /* rsx_d3d12_engine.c: the live anisotropy level */
extern int g_rsx_aa;                           /* rsx_draw_engine.c: 0 off, 1 FXAA, 2/4/8 MSAA */
extern volatile int g_rsx_aa_pending;          /* rsx_draw_engine.c: taken by the window pump, B2 drained (menu108) */
uint32_t tod_hero_handle(void);                /* tod_cheat.cpp: 0 = no level / hero yet */
int tod_no_level_yet(void);                    /* discord_level.cpp: 1 until the first level has loaded */
void tod_cheat_enqueue(const char* cmds);      /* tod_cheat.cpp: thread-safe, runs on the guest main thread */
void tod_cheat_arm(void);
int tod_cheat_state(int id);                   /* 0 god, 1 infinite ammo, 100+N game cheat N */
void tod_input_reload(void);                   /* host_input.cpp: re-read the KEY_* / TOD_MOUSE* settings (live) */
}

#ifndef CELL_PAD_CTRL_SELECT
#define CELL_PAD_CTRL_SELECT   0x0001
#define CELL_PAD_CTRL_START    0x0008
#define CELL_PAD_CTRL_UP       0x0010
#define CELL_PAD_CTRL_RIGHT    0x0020
#define CELL_PAD_CTRL_DOWN     0x0040
#define CELL_PAD_CTRL_LEFT     0x0080
#define CELL_PAD_CTRL_L1       0x0400
#define CELL_PAD_CTRL_R1       0x0800
#define CELL_PAD_CTRL_TRIANGLE 0x1000
#define CELL_PAD_CTRL_CIRCLE   0x2000
#define CELL_PAD_CTRL_CROSS    0x4000
#define CELL_PAD_CTRL_SQUARE   0x8000
#endif

namespace {

/* ---- the settings ---------------------------------------------------------- */

struct Choice {
    const char* value;   /* NULL: the key unset */
    const char* text;
};
struct Row {
    const char* key;     /* tod.ini / environment name; NULL for a placeholder */
    const char* label;
    const char* desc;
    std::vector<Choice> choices;
    int def;             /* index of the default */
    bool live;           /* applied at once, else at the next start */
    int tab;
    int running = -1, saved = -1, pending = -1;   /* choice indices; -1: a value not in the list */
    std::string other;   /* that value */
    bool soon = false;   /* a placeholder: greyed, "coming soon" */
    bool info = false;   /* 1.2 Credits tab: a read-only line like a placeholder, drawn in normal colour */
    int cheat = 0;       /* 1: toggle, 2: button (Cheats tab), 3: button (an action, any tab): acts at once, never in tod.ini, never "unapplied" */
    const char* cmd_on = NULL;    /* cheat commands (tod_cheat.cpp): toggle on / off, or the button's */
    const char* cmd_off = NULL;
    int state_id = -1;   /* tod_cheat_state() id read back when the page opens */
    const char* group = NULL;     /* a heading drawn above the row */
    bool experimental = false;    /* not re-tested on this version: an "EXPERIMENTAL" tag after the label */
    /* Controls tab (keys136): such a row has cheat == 3 (so the Apply / Discard / Defaults loops skip it: it applies and
     * is written to tod.ini at once). ctl: 1 key binding of kb::Action `bind`, 2 mouse button / wheel binding of
     * kb::MouseSlot `bind` (choices = the pad button it presses), 3 the "Reset to defaults" button, 4 a plain choice
     * (TOD_MOUSE, TOD_MOUSE_SENS, TOD_MOUSE_INVERT_Y; r.pending is the shown choice). */
    int ctl = 0;
    int bind = -1;
};

enum { TAB_VIDEO, TAB_SYSTEM, TAB_CHEATS, TAB_CONTROLS, TAB_SOON, TAB_CREDITS, NTABS };
const char* const kTabName[NTABS] = { "Video", "System", "Cheats", "Controls", "Coming soon", "Credits" };

std::vector<Row> s_rows;
bool s_rows_init;

void add(Row r) { s_rows.push_back(std::move(r)); }

/* ---- Controls tab rows (keys136) ----------------------------------------------- */

std::deque<std::string> s_ctl_text;   /* descriptions built at run time (stable addresses) */

void add_controls_rows()
{
    auto ctl_row = [](int ctl, const char* key, const char* label, const char* desc, std::vector<Choice> ch, int def, const char* group) -> Row& {
        Row r{ key, label, desc, std::move(ch), def, true, TAB_CONTROLS };
        r.cheat = 3; r.ctl = ctl; r.group = group;
        r.running = r.saved = r.pending = def;
        add(std::move(r));
        return s_rows.back();
    };
    ctl_row(3, NULL, "Reset to defaults",
            "Puts every key and mouse setting on this tab back to its default (the original layout) and saves it.",
            { { "1", "Reset" } }, 0, NULL);

    /* the mouse camera: the toggle, then its settings */
    ctl_row(4, "TOD_MOUSE", "Mouse camera",
            "The mouse turns the camera and aims, with the buttons below. Hides the cursor in the window; F1 or Alt+Tab free it.",
            { { "0", "Off" }, { "1", "On" } }, kb::kMouseDefaultOn ? 1 : 0, "Mouse");
    ctl_row(4, "TOD_MOUSE_SENS", "Mouse sensitivity", "How fast the camera follows the mouse. Capped by the game's Camera Speed option.",
            { { "0.5", "0.5x (slow)" }, { "0.75", "0.75x" }, { "1", "1x" }, { "1.5", "1.5x" }, { "2", "2x" }, { "3", "3x (fast)" } }, 2, NULL);
    ctl_row(4, "TOD_MOUSE_INVERT_Y", "Mouse: invert vertical", "Moving the mouse towards you looks up instead of down.",
            { { "0", "Off" }, { "1", "On" } }, 0, NULL);

    /* the keyboard: one row per pad input */
    const char* groups[kb::NACTIONS] = {};
    groups[kb::CROSS] = "Buttons"; groups[kb::DPAD_UP] = "D-pad"; groups[kb::LS_UP] = "Left stick (movement)";
    groups[kb::CAM_UP] = "Camera (right stick)";
    for (int a = 0; a < kb::NACTIONS; a++) {
        s_ctl_text.push_back(std::string("PS3 pad input. Default: ") + kb::key_name(kb::default_vk(a)) +
                             ". Enter or click, then press the new key or mouse button (middle, 4, 5).");
        Row& r = ctl_row(1, kb::ini_key(a), kb::label(a), s_ctl_text.back().c_str(), { { "1", "" } }, 0, groups[a]);
        r.bind = a;
    }

    /* the mouse's buttons while the mouse camera is on: which pad button each presses */
    std::vector<Choice> targets;
    for (int t = 0; t < kb::kNumButtonTargets; t++) targets.push_back({ kb::target_token(t), kb::label(t) });
    targets.push_back({ "None", "None" });
    for (int m = 0; m < kb::NMOUSE; m++) {
        s_ctl_text.push_back(std::string("The pad button this mouse input presses while the mouse camera is on. Default: ") +
                             (kb::mouse_default(m) >= 0 ? kb::label(kb::mouse_default(m)) : "None") + ".");
        Row& r = ctl_row(2, kb::mouse_ini_key(m), kb::mouse_label(m), s_ctl_text.back().c_str(), targets,
                         kb::mouse_default(m) >= 0 ? kb::mouse_default(m) : kb::kNumButtonTargets, m == 0 ? "Mouse buttons (mouse camera on)" : NULL);
        r.bind = m;
    }
}

void init_rows()
{
    if (s_rows_init) return;
    s_rows_init = true;
    add({ "RSX_SCALE", "Internal resolution",
          "The resolution the game is rendered at (the window size does not change). Costs performance: 4K needs a powerful GPU.",
          { { "1", "720p (native)" }, { "1.5", "1080p" }, { "2", "1440p" }, { "3", "4K" } }, 0, true, TAB_VIDEO });
    add({ "RSX_DISPLAY", "Window mode",
          "Windowed, borderless window or exclusive fullscreen. Changing it rebuilds the swap chain: a frame or two of black.",
          { { "windowed", "Windowed" }, { "borderless", "Borderless" }, { "fullscreen", "Fullscreen" } }, 0, true, TAB_VIDEO });
    add({ "RSX_WINDOW", "Window size",
          "Windowed mode only.",
          { { NULL, "1280x720" }, { "1600x900", "1600x900" }, { "1920x1080", "1920x1080" }, { "2560x1440", "2560x1440" } },
          0, true, TAB_VIDEO });
    add({ "TOD_ASPECT", "Aspect ratio",
          "For ultrawide screens: a wider view, the HUD and menus stay 16:9 in the middle. Auto = the shape of the screen (of the window size in windowed mode). Applies at the next launch.",
          { { NULL, "16:9 (original)" }, { "21:9", "21:9" }, { "43:18", "3440x1440 (43:18)" }, { "32:9", "32:9" }, { "auto", "Auto" } },
          0, false, TAB_VIDEO });
    s_rows.back().experimental = true;
    add({ "RSX_CROP_BARS", "Crop black bars",
          "The game draws 704 lines inside a 720-line frame, as on the PS3 (TVs hid the thin bars at the top and bottom). On cuts them and stretches the picture 2.3% taller. Applies at the next launch.",
          { { NULL, "Off" }, { "8", "On" } }, 0, false, TAB_VIDEO });
    add({ "RSX_VSYNC", "VSync", "Syncs presentation to the monitor. Off allows more fps than the refresh rate (with tearing).",
          { { "1", "On" }, { "0", "Off" } }, 0, true, TAB_VIDEO });
    add({ "TOD_FPS", "FPS limit",
          "Frames-per-second cap. Above 60 VSync turns itself off and physics uses a smaller step (docs\\FRAMERATE.md). Unlimited = 1000.",
          { { NULL, "Original" }, { "30", "30" }, { "60", "60" }, { "120", "120" }, { "1000", "Unlimited" } }, 0, true, TAB_VIDEO });
    add({ "RSX_AA", "Anti-aliasing", "FXAA softens the image slightly; MSAA is cleaner but costs a lot of memory and GPU.",
          { { NULL, "Off" }, { "fxaa", "FXAA" }, { "msaa2", "MSAA x2" }, { "msaa4", "MSAA x4" }, { "msaa8", "MSAA x8" } },
          0, true, TAB_VIDEO });
    add({ "RSX_ANISO", "Anisotropic filtering", "Sharper textures on ground and walls seen at an angle.",
          { { "1", "Trilinear (off)" }, { "2", "2x" }, { "4", "4x" }, { "8", "8x" }, { "16", "16x" } }, 4, true, TAB_VIDEO });
    add({ "RSX_TEX_LOD_BIAS", "Texture sharpness", "Negative values pick sharper texture detail levels (more shimmer on fine patterns). Applies at the next launch.",
          { { "0", "Default" }, { "-0.5", "Sharper" }, { "-1", "Sharpest" } }, 0, false, TAB_VIDEO });
    add({ "RSX_SHADOW_SCALE", "Shadow resolution", "Resolution multiplier of the per-object shadow maps on top of the internal resolution. Applies at the next launch.",
          { { "1", "Standard" }, { "2", "High" }, { "4", "Very high" } }, 1, false, TAB_VIDEO });
    add({ "RSX_BLOOM_SCALE", "Bloom resolution", "Resolution multiplier of the bloom blur chain on top of the internal resolution. Applies at the next launch.",
          { { "1", "Standard" }, { "2", "High" }, { "4", "Very high" } }, 1, false, TAB_VIDEO });
    add({ "TOD_FPS_COUNTER", "FPS counter",
          "Shows the frame rate in the top-right corner. Detailed adds the frame time, the 1% low (last 5 s), the GPU and CPU load and the video memory use.",
          { { "0", "Off" }, { "1", "FPS" }, { "2", "Detailed" } }, 0, true, TAB_VIDEO });
    add({ "RT_AO", "Ray-traced ambient occlusion",
          "Contact shadows in corners, under objects and around characters, traced on the GPU (needs an RTX 20 / RDNA2 or newer GPU, ~1 ms). Experimental: not re-tested on this version, can darken dark scenes.",
          { { NULL, "Off" }, { "1", "On" } }, 0, false, TAB_VIDEO });
    s_rows.back().experimental = true;
    add({ "RT_AO_QUALITY", "Ray tracing quality", "Rays per pixel and resolution of the ray-traced effects. High uses the most rays (cleaner, costs more). Experimental: not re-tested on this version.",
          { { "0", "Low" }, { "1", "Medium" }, { "2", "High" } }, 1, false, TAB_VIDEO });
    s_rows.back().experimental = true;
    add({ "RSX_SSR", "Screen-space reflections",
          "Metal, glass and wet surfaces reflect the visible scene (from the previous frame) instead of only the blurry environment picture; the picture is kept where the ray leaves the screen. Applies at the next launch. Experimental: not re-tested on this version.",
          { { NULL, "Off" }, { "1", "On" } }, 0, false, TAB_VIDEO });
    s_rows.back().experimental = true;

    add({ "PS3_LANG", "Language",
          "The console system language the game sees: text, voices, subtitles.",
          { { NULL, "English" }, { "english_gb", "English (UK)" }, { "italian", "Italian" }, { "french", "French" },
            { "german", "German" }, { "spanish", "Spanish" }, { "dutch", "Dutch" }, { "portuguese", "Portuguese" },
            { "finnish", "Finnish" }, { "swedish", "Swedish" }, { "danish", "Danish" }, { "norwegian", "Norwegian" },
            { "korean", "Korean" }, { "chinese_t", "Chinese (Traditional)" }, { "japanese", "Japanese" } },
          0, false, TAB_SYSTEM });
    add({ "TOD_PROMPTS", "Button prompts",
          "The button icons the game shows. Auto follows player 1's controller (the keyboard if it was used last).",
          { { NULL, "Auto" }, { "ps3", "PlayStation 3" }, { "ps4", "PlayStation 4" }, { "ps5", "PlayStation 5" },
            { "xbox", "Xbox" }, { "switch", "Nintendo Switch" }, { "keyboard", "Keyboard" } },
          0, true, TAB_SYSTEM });
    add({ "TOD_DIFFICULTY", "Difficulty",
          "Story: enemies deal half damage and your weapons deal double. Extreme: the reverse. Medium is the game's own balance.",
          { { "story", "Story" }, { "easy", "Easy" }, { NULL, "Medium" }, { "hard", "Hard" }, { "extreme", "Extreme" } },
          2, true, TAB_SYSTEM });
    add({ "TOD_ARMOR_LOOK", "Armor look",
          "Hidden: Ratchet keeps his normal suit whatever armor is equipped (the armor still protects him; saves are unchanged).",
          { { NULL, "Shown" }, { "hidden", "Hidden" } }, 0, true, TAB_SYSTEM });
    add({ "TOD_REMOVE_HELMET", "Remove helmet",
          "On: let Ratchet breathe a little easier. The helmet comes off, the armor stays and still protects him.",
          { { NULL, "Off" }, { "1", "On" } }, 0, true, TAB_SYSTEM });
    add({ "TOD_BAREFOOT", "Barefoot",
          "On: like the old days on Veldin, Ratchet goes barefoot, boots off (with the base suit, or Armor look: Hidden). Applies at the next start.",
          { { NULL, "Off" }, { "1", "On" } }, 0, false, TAB_SYSTEM });
    {
        Row r{ NULL, "Saves folder", "Opens the folder that holds your saves in Explorer. PS3_SAVEDATA_ROOT in tod.ini moves it; "
               "the layout (one BCES00052_SAVE_<n> folder per save) is the one RPCS3 uses.",
               { { "1", "Open" } }, 0, true, TAB_SYSTEM };
        r.cheat = 4;      /* an action button (not a cheat, not a setting) */
        r.running = r.saved = r.pending = 0;
        add(r);
    }
    {   /* 1.01: update_check.cpp */
        Row r{ NULL, "Updates", "This is version " TOD_RELEASE_VERSION ". At start the game asks GitHub whether a newer release exists "
               "(nothing is downloaded; TOD_UPDATE_CHECK = 0 in tod.ini turns it off). Opens the download page in the browser.",
               { { "1", "Open download page" } }, 0, true, TAB_SYSTEM };
        r.cheat = 5;      /* an action button, like "Saves folder" */
        r.running = r.saved = r.pending = 0;
        add(r);
    }

    /* DualSense (tod_dualsense.cpp): read live every quarter second; no effect without a DualSense. */
    add({ "TOD_DS_TRIGGERS", "DualSense adaptive triggers",
          "PS5 controller: the right trigger pushes back, clicks or buzzes like the weapon in Ratchet's hands (nothing in menus and cutscenes). Does nothing without a DualSense.",
          { { NULL, "On" }, { "0", "Off" } }, 0, true, TAB_SYSTEM });
    s_rows.back().group = "Controller";
    add({ "TOD_DS_TRIGGER_STRENGTH", "Trigger strength",
          "How hard the DualSense triggers push back and kick.",
          { { NULL, "Full" }, { "75", "75 %" }, { "50", "50 %" }, { "25", "25 %" } }, 0, true, TAB_SYSTEM });
    add({ "TOD_DS_FIRE_R2", "Fire with R2 (DualSense)",
          "DualSense: the right trigger fires and R1 does what R2 does on the PS3 pad (the on-screen prompts keep saying R1 for fire). Off: the PS3 layout.",
          { { NULL, "On" }, { "0", "Off (R1)" } }, 0, true, TAB_SYSTEM });
    add({ "TOD_PAD_SWAP_R1R2", "Swap R1/R2",
          "On: R1 and R2 trade places on every controller (fire with the trigger). On a DualSense, Fire with R2 already does it. "
          "Any button can also be moved with PAD_MAP in tod.ini (e.g. PAD_MAP=CROSS=SQUARE,SQUARE=CROSS).",
          { { NULL, "Off" }, { "1", "On" } }, 0, true, TAB_SYSTEM });
    add({ "TOD_PAD_SWAP_L1L2", "Swap L1/L2",
          "On: L1 and L2 trade places on every controller (zoom and strafe on the trigger).",
          { { NULL, "Off" }, { "1", "On" } }, 0, true, TAB_SYSTEM });
    add({ "TOD_TELEPORT", "Teleport menu (F5)",
          "EXPERIMENTAL / debug: F5 opens a list of warp points of the current level and a level switcher (the level switch restarts the game on that level). Not for normal play.",
          { { NULL, "Off" }, { "1", "On" } }, 0, true, TAB_SYSTEM });
    s_rows.back().experimental = true;
    add({ "TOD_XB_TRIGGERS", "Xbox impulse triggers",
          "Xbox One / Series controller: the right trigger's motor kicks at every shot, like the weapon in Ratchet's hands. Strength: Trigger strength above. Does nothing on a controller without trigger motors.",
          { { NULL, "On" }, { "0", "Off" } }, 0, true, TAB_SYSTEM });

    /* Cheats: the commands are tod_cheat.cpp's. Toggles: Off/On; buttons: one choice. Greyed until a level runs. */
    auto toggle = [](const char* label, const char* desc, const char* on, const char* off, int state_id, const char* group) {
        Row r{ NULL, label, desc, { { "0", "Off" }, { "1", "On" } }, 0, true, TAB_CHEATS };
        r.cheat = 1; r.cmd_on = on; r.cmd_off = off; r.state_id = state_id; r.group = group;
        r.running = r.saved = r.pending = 0;
        add(r);
    };
    auto button = [](const char* label, const char* desc, const char* cmd, const char* group) {
        Row r{ NULL, label, desc, { { "1", "Apply" } }, 0, true, TAB_CHEATS };
        r.cheat = 2; r.cmd_on = cmd; r.group = group;
        r.running = r.saved = r.pending = 0;
        add(r);
    };
    toggle("God mode", "Ratchet takes no damage (health is kept full).", "god", "god=0", 0, NULL);
    toggle("Infinite ammo", "Weapons never run out of ammo (and the ammo is refilled when you switch it on).", "ammo", "noammo", 1, NULL);
    button("Heal", "Refills Ratchet's health once.", "heal", "Quick actions");
    button("+100,000 bolts", "Adds 100,000 bolts to your wallet.", "bolts=100000", NULL);
    button("Give all weapons (max level)", "Unlocks every weapon at its top level with full ammo.", "weapons", NULL);
    toggle("Big head", "The game's own HERO_BIGHEAD cheat: Ratchet with a big head.", "cheat=1:1", "cheat=1:0", 101, "The game's own cheats");
    toggle("Enemy big head", "The game's own ENEMY_BIGHEAD cheat: enemies with big heads.", "cheat=3:1", "cheat=3:0", 103, NULL);
    toggle("Mirror level", "The game's own MIRROR_LEVEL cheat: the level is mirrored. May need a level reload to take effect.", "cheat=11:1", "cheat=11:0", 111, NULL);

    const char* soon = "Coming in a future version.";
    const char* names[][2] = {
        { "Field of view (FOV)", "" }, { "Shadows", "" }, { "Motion blur", "" }, { "Post-processing", "" },
        { "View distance", "" }, { "Ray tracing: shadows", "" }, { "Ray tracing: reflections", "" },
        { "Ray tracing: global illumination", "" } };
    for (auto& n : names) {
        Row r{ NULL, n[0], soon, { { NULL, "Coming soon" } }, 0, false, TAB_SOON };
        r.soon = true;
        r.running = r.saved = r.pending = 0;
        add(r);
    }
    /* 1.2 Credits tab: the port's own page (never the game's credits). Supporters: one line each, added per release. */
    {
        const char* lines[][3] = {   /* label, description, group heading */
            { "Ratchet & Clank: Tools of Destruction - native PC port", "A static recompilation of the PS3 game. Requires your own copy of the game.", "Recomp project" },
            { "Developed by Re-TeRo17", "github.com/Re-TeRo17/ratchet-tod-recomp", NULL },
            { "Fan project, not affiliated with Insomniac Games or Sony Interactive Entertainment", "All game content belongs to its owners; nothing of the game is included in this download.", NULL },
            { "7GP", "Thank you for supporting the project!", "Supporters" },
            { "IS", "Thank you for supporting the project!", NULL },
            { "S", "Thank you for supporting the project!", NULL },
            { "Support the project: buymeacoffee.com/tero17", "Support goes to the development work of the port.", NULL },
            { "Community: discord.gg/S4jd7643mg", "Help, news and screenshots. News also on X: @Re_TeRo17.", NULL },
            { "ps3recomp by sp00nznet (fork by soyjxck) (MIT)", "The PS3 static recompilation runtime this port is built on.", "Thanks" },
            { "dod3-recomp by soyjxck and contributors (MIT)", "The Drakengard 3 recompilation this port is modelled on (boot path, guest memory, Dear ImGui setup).", NULL },
            { "RACiT by HighDrexler (MIT)", "Discord presence and DualSense trigger effect code.", NULL },
            { "SDL2, Dear ImGui, zlib and the other libraries", "See THIRD_PARTY_NOTICES.md next to tod.exe.", NULL },
            { "Everyone reporting issues on GitHub", "Your reports and logs make every version better.", NULL },
        };
        for (auto& l : lines) {
            Row r{ NULL, l[0], l[1], { { NULL, "" } }, 0, false, TAB_CREDITS };
            r.soon = true; r.info = true; r.group = l[2];
            r.running = r.saved = r.pending = 0;
            add(r);
        }
    }
    add_controls_rows();   /* keys136 */
}

int find_choice(const Row& r, const char* v)
{
    for (size_t i = 0; i < r.choices.size(); i++) {
        const char* c = r.choices[i].value;
        if (!v || !*v) {
            if (!c) return (int)i;
            continue;
        }
        if (!c) continue;
        if (!strcmp(c, v)) return (int)i;
        if (!strcmp(r.key, "RSX_SCALE") && atof(c) == atof(v)) return (int)i;
        if (!strcmp(r.key, "RSX_AA") && !strcmp(v, "0")) continue;
    }
    if (v && (!strcmp(v, "0") || !_stricmp(v, "off")) && !strcmp(r.key, "RSX_AA")) return find_choice(r, NULL);
    if (v && !strcmp(v, "1") && !strcmp(r.key, "RSX_AA")) return find_choice(r, "fxaa");
    if (v && !strcmp(r.key, "RSX_WINDOW") && !strcmp(v, "1280x720")) return find_choice(r, NULL);
    if (v && !strcmp(r.key, "PS3_LANG") && !_stricmp(v, "english")) return find_choice(r, NULL);
    return -1;
}

void read_current(Row& r, int& idx)
{
    r.other.clear();
    const char* v = getenv(r.key);
    idx = find_choice(r, v);
    if (idx < 0) {
        if (!v || !*v) idx = r.def;
        else r.other = v;
    }
}

/* From the environment (where tod.ini was loaded): what is running now. */
void refresh_rows()
{
    for (Row& r : s_rows) {
        if (r.cheat) {      /* not a setting: what the engine has now (god / ammo / the game's cheat bytes) */
            if (r.cheat == 1 && r.state_id >= 0) r.running = r.saved = r.pending = tod_cheat_state(r.state_id) > 0 ? 1 : 0;
            continue;
        }
        if (r.soon) continue;
        read_current(r, r.running);
        r.saved = r.pending = r.running;
    }
}

std::string value_text(const Row& r, int idx)
{
    std::string t = idx >= 0 ? r.choices[idx].text : ("Custom (" + r.other + ")");
    if (!r.soon && !r.live && idx == r.saved && r.saved != r.running) t += "  (requires restart)";
    return t;
}

/* ---- tod.ini --------------------------------------------------------------- */

/* KEY = VALUE on the key's line (uncommenting "# KEY = ..."), or appended; a NULL
 * value comments the line out. Everything else is kept (same rules as dod3's). */
void write_ini(const std::vector<std::pair<std::string, const char*>>& kv)
{
    const char* path = tod_settings_path();
    std::vector<std::string> lines;
    bool crlf = false;
    if (FILE* f = fopen(path, "rb")) {
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) {
            std::string l(buf);
            if (l.size() >= 2 && l[l.size() - 2] == '\r') crlf = true;
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            lines.push_back(l);
        }
        fclose(f);
    }
    for (const auto& p : kv) {
        const std::string& key = p.first;
        int hit = -1, commented = -1;
        for (size_t i = 0; i < lines.size(); i++) {
            const char* s = lines[i].c_str();
            while (*s == ' ' || *s == '\t') s++;
            const bool c = *s == '#';
            if (c) {
                s++;
                while (*s == ' ' || *s == '\t') s++;
            }
            if (strncmp(s, key.c_str(), key.size())) continue;
            const char* e = s + key.size();
            while (*e == ' ' || *e == '\t') e++;
            if (*e != '=') continue;
            if (!c) { hit = (int)i; break; }
            if (commented < 0) commented = (int)i;
        }
        /* keys136: a value starting with '#' = "back to the default": the line is written commented, as "# KEY = value" */
        const bool as_comment = p.second && p.second[0] == '#';
        const std::string line = !p.second ? "# " + key + " = " : as_comment ? "# " + key + " = " + (p.second + 1) : key + " = " + p.second;
        if (hit >= 0) {
            if (p.second) lines[hit] = line;
            else lines[hit] = "# " + lines[hit];
        } else if (p.second) {
            if (commented >= 0) lines[commented] = line;
            else if (!as_comment) lines.push_back(line);
        }
    }
    if (FILE* f = fopen(path, "wb")) {
        for (const std::string& l : lines) fprintf(f, "%s%s", l.c_str(), crlf ? "\r\n" : "\n");
        fclose(f);
        fprintf(stderr, "[overlay] saved %s\n", path);
    } else {
        fprintf(stderr, "[overlay] could not write %s\n", path);
    }
}

std::string s_status;
double s_status_until;
double now_s() { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart; }
void say(const char* t) { s_status = t; s_status_until = now_s() + 4.0; }

bool dirty()
{
    for (const Row& r : s_rows) if (!r.soon && !r.cheat && r.pending != r.saved) return true;
    return false;
}

int s_fps_mode;     /* TOD_FPS_COUNTER: 0 off, 1 FPS, 2 Detailed (read by the render thread) */

void apply_and_save()
{
    std::vector<std::pair<std::string, const char*>> kv;
    bool display = false;
    for (Row& r : s_rows) {
        if (r.soon || r.cheat || r.pending == r.saved) continue;
        const char* v = r.pending >= 0 ? r.choices[r.pending].value : r.other.c_str();
        kv.push_back({ r.key, v });
        r.saved = r.pending;
        if (!r.live) continue;
        _putenv_s(r.key, v ? v : "");       /* "" removes it */
        r.running = r.pending;
        if (!strcmp(r.key, "RSX_ANISO")) g_rsx_aniso = v ? atoi(v) : 16;
        if (!strcmp(r.key, "TOD_FPS_COUNTER")) s_fps_mode = v ? atoi(v) : 0;
        if (!strcmp(r.key, "TOD_FPS")) tod_fps_apply_live(v);   /* usr123: live, main.cpp */
        if (!strcmp(r.key, "RSX_AA")) {
            g_rsx_aa_pending = !v ? 0 : !strcmp(v, "fxaa") ? 1 : !strcmp(v, "msaa2") ? 2 : !strcmp(v, "msaa4") ? 4 : !strcmp(v, "msaa8") ? 8 : 0;
            display = true;
        }
        if (!strcmp(r.key, "RSX_DISPLAY") || !strcmp(r.key, "RSX_VSYNC") || !strcmp(r.key, "RSX_SCALE") ||
            !strcmp(r.key, "RSX_WINDOW"))
            display = true;
        fprintf(stderr, "[overlay] %s = %s%s\n", r.key, v ? v : "(default)", "");
    }
    if (display) g_rsx_display_reload = 1;
    if (kv.empty()) { say("No changes."); return; }
    write_ini(kv);
    bool restart = false;
    for (const Row& r : s_rows) if (!r.soon && !r.live && r.saved != r.running) restart = true;
    say(restart ? "Saved to tod.ini. Some options apply after a restart." : "Saved to tod.ini and applied.");
}

void revert()
{
    bool any = false;
    for (Row& r : s_rows) if (!r.soon && !r.cheat && r.pending != r.saved) { r.pending = r.saved; any = true; }
    say(any ? "Changes discarded." : "No changes to discard.");
}

void defaults()
{
    for (Row& r : s_rows) if (!r.soon && !r.cheat) { r.pending = r.def; r.other.clear(); }
    say("Defaults selected: press Apply and save.");
}

void push_capture(int code);   /* the input section below */

/* ---- Controls tab (keys136) ------------------------------------------------------
 * Key / mouse bindings (src/input/key_bindings.cpp). They apply live (tod_input_reload) and are written to tod.ini at
 * once; a value equal to the default is written back as the commented default line ("# KEY_CROSS = Z"). */

std::atomic<int> s_cap_row{ -1 };    /* the key row waiting for a key (index in s_rows), -1 = none; read by the window thread */
bool s_scroll_follow;                /* the selection moved by key / pad: scroll it into view */

const int kCapCancel = -1, kCapClear = -2, kCapReserved = -3;   /* codes of a capture event besides a virtual key */

bool controls_capturing() { return s_cap_row.load() >= 0; }

/* The shown value of a Controls row. */
std::string controls_value_text(const Row& r)
{
    if (r.ctl == 1) {
        if (s_cap_row.load() >= 0 && &s_rows[s_cap_row.load()] == &r) return "Press a key...";
        const int vk = kb::get(r.bind);
        return vk ? kb::key_name(vk) : "(none)";
    }
    if (r.ctl == 3) return r.choices[0].text;
    return r.pending >= 0 ? r.choices[r.pending].text : "?";
}

/* Writes the rows to tod.ini and puts them in the environment (the game side re-reads it: tod_input_reload). */
void controls_commit(const std::vector<Row*>& rows)
{
    std::deque<std::string> val;
    std::vector<std::pair<std::string, const char*>> kv;
    for (Row* r : rows) {
        std::string v, def;
        if (r->ctl == 1) {
            v = kb::key_name(kb::get(r->bind));
            def = kb::key_name(kb::default_vk(r->bind));
        } else if (r->ctl == 2) {
            const int a = kb::mouse_get(r->bind);
            v = kb::target_token(a);
            def = kb::target_token(kb::mouse_default(r->bind));
        } else if (r->ctl == 4) {
            v = r->choices[r->pending].value;
            def = r->choices[r->def].value;
        } else continue;
        const bool is_def = v == def;
        _putenv_s(r->key, is_def ? "" : v.c_str());     /* "" removes it: the default */
        val.push_back(is_def ? "#" + def : v);
        kv.push_back({ r->key, val.back().c_str() });
        r->saved = r->running = r->pending;
        fprintf(stderr, "[overlay] %s = %s%s\n", r->key, v.c_str(), is_def ? " (default)" : "");
    }
    tod_input_reload();
    write_ini(kv);
}

/* The page just opened: what the environment says (the rows are applied at once, so it is also what is running). */
void controls_refresh()
{
    s_cap_row.store(-1);
    for (Row& r : s_rows) {
        if (r.ctl == 2) r.pending = kb::mouse_get(r.bind) >= 0 ? kb::mouse_get(r.bind) : kb::kNumButtonTargets;
        else if (r.ctl == 4) {
            const char* v = getenv(r.key);
            int idx = -1;
            if (!v || !*v) idx = r.def;
            else if (!strcmp(r.key, "TOD_MOUSE_SENS")) {
                for (size_t i = 0; i < r.choices.size(); i++) if (atof(r.choices[i].value) == atof(v)) idx = (int)i;
                if (idx < 0) idx = r.def;      /* a custom value: shown as the default, left alone until changed */
            } else idx = atoi(v) != 0 ? 1 : 0;
            r.pending = idx;
        } else continue;
        r.saved = r.running = r.pending;
    }
}

void controls_cancel() { if (s_cap_row.exchange(-1) >= 0) say("Cancelled."); }

/* A key row's binding changed to vk (0 = none). */
void controls_bind(Row& r, int vk)
{
    kb::set(r.bind, vk);
    controls_commit({ &r });
    std::string others;
    if (vk)
        for (int a = 0; a < kb::NACTIONS; a++)
            if (a != r.bind && kb::get(a) == vk) others += (others.empty() ? "" : ", ") + std::string(kb::label(a));
    std::string m = std::string(r.label) + ": " + (vk ? kb::key_name(vk) : "unbound") + ".";
    if (!others.empty()) m += " (also " + others + ")";
    say(m.c_str());
}

void controls_reset_all()
{
    std::vector<Row*> all;
    for (Row& r : s_rows) {
        if (r.tab != TAB_CONTROLS) continue;
        if (r.ctl == 1) kb::set(r.bind, kb::default_vk(r.bind));
        else if (r.ctl == 2) { kb::mouse_set(r.bind, kb::mouse_default(r.bind)); r.pending = r.def; }
        else if (r.ctl == 4) r.pending = r.def;
        else continue;
        all.push_back(&r);
    }
    controls_commit(all);
    say("Controls reset to the defaults and saved.");
}

/* Enter / click / arrow on a Controls row. */
void controls_change(Row& r, int dir)
{
    if (controls_capturing()) { controls_cancel(); return; }    /* any other press while waiting for a key: cancel */
    if (r.ctl == 1) {
        s_cap_row.store((int)(&r - s_rows.data()));
        say("Press the new key (or a mouse button). Esc cancels, Backspace clears.");
    } else if (r.ctl == 3) {
        controls_reset_all();
    } else if (r.ctl == 2 || r.ctl == 4) {
        const int n = (int)r.choices.size();
        r.pending = (r.pending + dir + n) % n;
        if (r.ctl == 2) kb::mouse_set(r.bind, r.pending < kb::kNumButtonTargets ? r.pending : -1);
        controls_commit({ &r });
        say((std::string(r.label) + ": " + r.choices[r.pending].text + ".").c_str());
    }
}

void controls_clear(Row& r)
{
    if (r.ctl != 1) return;
    s_cap_row.store(-1);
    controls_bind(r, 0);
}

/* A capture event (render thread): code = a virtual key, or kCap*. Ignored when nothing is waiting. */
void controls_capture_event(int code)
{
    const int ri = s_cap_row.load();
    if (ri < 0) return;
    Row& r = s_rows[ri];
    if (code == kCapCancel) { controls_cancel(); return; }
    if (code == kCapReserved) { say("That key is reserved (F1 page, F8 screenshots, Windows keys). Press another, or Esc."); return; }
    s_cap_row.store(-1);
    controls_bind(r, code == kCapClear ? 0 : code);
}

int controls_code_for_vk(int vk)
{
    if (vk == VK_ESCAPE) return kCapCancel;
    if (vk == VK_BACK) return kCapClear;
    if (vk == VK_F1 || vk == VK_F8 || vk == VK_LWIN || vk == VK_RWIN || vk == VK_SNAPSHOT || vk == VK_APPS) return kCapReserved;
    return vk;
}

/* The window thread, a WM_KEYDOWN while a row waits for a key: the key as a capture event (left / right Shift, Ctrl, Alt apart). */
void controls_wm_key(uintptr_t wp, intptr_t lp)
{
    if (lp & (1 << 30)) return;                        /* auto-repeat of the key that started it */
    int vk = (int)wp;
    if (vk == VK_SHIFT) vk = (int)MapVirtualKeyA((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    else if (vk == VK_CONTROL) vk = (lp & (1 << 24)) ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU) vk = (lp & (1 << 24)) ? VK_RMENU : VK_LMENU;
    if (!vk) vk = (int)wp;
    push_capture(controls_code_for_vk(vk));
}

/* ---- input ------------------------------------------------------------------ */

enum Act { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_ACCEPT, A_BACK, A_TAB_NEXT, A_TAB_PREV, A_APPLY, A_REVERT, A_CLOSE, A_CLEAR };
struct Ev { int kind; float a, b; };   /* kind: 0 act, 1 mouse move, 2 mouse button (a=btn, b=down), 3 wheel, 4 Controls capture (a=code) */

std::atomic<bool> s_open{false};
std::atomic<bool> s_want_toggle{false};
std::atomic<uint32_t> s_pad{0};
CRITICAL_SECTION s_cs;
std::vector<Ev> s_ev;
bool s_cs_init;

void push(const Ev& e) { EnterCriticalSection(&s_cs); if (s_ev.size() < 512) s_ev.push_back(e); LeaveCriticalSection(&s_cs); }
void push_capture(int code) { push({ 4, (float)code, 0 }); }

/* ---- save-slot picker (svpick) ----------------------------------------------------------------------------------------
 * cellSaveDataListLoad2/ListSave2 (cellSaveData.c) call pick_hook() on the guest thread, which blocks there on a condition
 * variable until the user chooses; the render side draws the list and feeds the result. */
uint32_t s_pad_prev;
std::atomic<bool> s_pick_on{false};
std::atomic<uint64_t> s_pick_drawn{0};           /* GetTickCount64() of the last frame that drew the picker */
std::mutex s_pick_m;
std::condition_variable s_pick_cv;
int s_pick_result = -3;                          /* -3 pending, -1 cancel, >= 0 the chosen entry */
std::vector<std::string> s_pick_name, s_pick_info;
int s_pick_new = -1, s_pick_sel;
bool s_pick_save, s_pick_scroll, s_pick_hold;
double s_pick_t0;
uint64_t s_pick_hold_t0;
std::string s_pick_title;                        /* quit125: a title instead of LOAD / SAVE GAME (the quit box) */
bool s_pick_alert;                               /* alert10: draw the picker as the game's own YES / NO box (the vendor's "are you sure") */
std::string s_alert_sub;                         /* alert10: the second line of that box (may be empty) */

void pick_finish(int r)
{
    std::lock_guard<std::mutex> g(s_pick_m);
    if (s_pick_result != -3) return;
    s_pick_result = r;
    s_pick_cv.notify_all();
}

void pick_act(int act)
{
    const int n = (int)s_pick_name.size();
    s_pick_scroll = true;
    switch (act) {
    case A_UP: if (n) s_pick_sel = (s_pick_sel + n - 1) % n; break;
    case A_DOWN: if (n) s_pick_sel = (s_pick_sel + 1) % n; break;
    case A_ACCEPT: pick_finish(s_pick_sel); break;
    case A_BACK: case A_CLOSE: pick_finish(-1); break;
    default: break;
    }
}

int pick_hook(int is_save, int n, const char* const* names, const char* const* infos, int new_idx, int def_idx)
{
    if (!s_cs_init || n < 2 || s_pick_on.load() || s_open.load()) return -2;
    {
        std::lock_guard<std::mutex> g(s_pick_m);
        s_pick_name.assign(names, names + n);
        s_pick_info.assign(infos, infos + n);
        s_pick_new = new_idx; s_pick_save = is_save != 0;
        s_pick_sel = (def_idx >= 0 && def_idx < n) ? def_idx : 0;
        s_pick_result = -3; s_pick_scroll = true;
        s_pick_t0 = now_s();
    }
    EnterCriticalSection(&s_cs); s_ev.clear(); LeaveCriticalSection(&s_cs);
    s_pad_prev = s_pad.load();
    const int prev_block = g_cellPad_input_block;
    s_pick_hold = false;
    g_cellPad_input_block = 1;
    s_pick_drawn.store(0);
    s_pick_on.store(true);
    fprintf(stderr, "[savepick] picker open (%s, %d entries, preselected #%d)\n", is_save ? "save" : "load", n, s_pick_sel);
    const uint64_t t0 = GetTickCount64();
    int r = -3;
    bool no_ui = false;
    for (;;) {
        {   /* the guest thread is stuck here: it does the work the game's own loop would (pad poll, present the frame) */
            std::unique_lock<std::mutex> lk(s_pick_m);
            if (s_pick_cv.wait_for(lk, std::chrono::milliseconds(16), [] { return s_pick_result != -3; })) { r = s_pick_result; break; }
        }
        cellPad_poll();
        rsx_d3d12_engine_repaint();
        const uint64_t now = GetTickCount64(), drawn = s_pick_drawn.load();
        if (!drawn && now - t0 > 4000) { no_ui = true; break; }                 /* never drew: no window / overlay failed */
        if (drawn && now - drawn > 6000) { no_ui = true; break; }               /* stopped drawing: do not hang the game */
    }
    s_pick_on.store(false);
    if (no_ui) {
        fprintf(stderr, "[savepick] picker could not be drawn -> automatic pick\n");
        g_cellPad_input_block = s_open.load() ? 1 : (prev_block ? 1 : 0);
        return -2;
    }
    s_pick_hold = true; s_pick_hold_t0 = GetTickCount64();   /* keep the game's pad cleared until the buttons are released */
    fprintf(stderr, "[savepick] picker closed: %s\n", r < 0 ? "cancel" : "chosen");
    return r;
}

void set_open(bool on)
{
    if (s_open.load() == on) return;
    s_open.store(on);
    s_cap_row.store(-1);      /* a Controls capture ends with the page */
    g_cellPad_input_block = on ? 1 : 0;
    if (on) s_want_toggle.store(true);     /* the render thread refreshes the rows */
    fprintf(stderr, "[overlay] %s (input to the game %s)\n", on ? "open" : "closed", on ? "blocked" : "restored");
}

extern "C" void tod_menu_apply_key_win(const char* key, const char* v);

/* ---- usr123: TELEPORT MENU (EXPERIMENTAL / debug, TOD_TELEPORT=1 or the System tab; F5) ----------------------------------
 * Left: the 19 levels (the current one marked). Right: its warp points (tod_teleport_data.inc, from WIKI_CHEAT.md sec. 4).
 * A warp point of the CURRENT level is a hero_warp(Ratchet1,<volume>) run live on the guest thread (tod_lua_enqueue, same path as
 * TOD_LUA_EXEC). Another level cannot be loaded from inside a running level, so it restarts tod.exe on -level;<name> (same environment, the
 * log keeps going, the warp runs 45 s after the start); the new process id is printed ("[teleport] relaunched pid N") and written to
 * teleport_pid.txt (an agent holding the game lock must -Register that id). */
#include "tod_teleport_data.inc"
std::atomic<bool> s_tp_open{ false };
int s_tp_sel = -1;                  /* level selected in the list (-1: the current one) */
char s_tp_cur[64];                  /* the level the loader names (refreshed every second while open) */
std::string s_tp_msg;
bool tp_enabled() { const char* e = getenv("TOD_TELEPORT"); return e && e[0] == '1'; }
void tp_set_open(bool on)
{
    if (s_tp_open.load() == on) return;
    s_tp_open.store(on);
    if (!s_open.load()) g_cellPad_input_block = on ? 1 : 0;
    if (on) { s_tp_sel = -1; s_tp_msg.clear(); }
    tod_tp_log("[teleport] menu %s\n", on ? "open" : "closed");
}
/* vram124: LEVEL_* constant of the game's Lua for a level folder name */
void tp_relaunch(const char* level, const char* spot);
static std::string tp_level_const(const char* folder)
{
    if (!_stricmp(folder, "fastoon_return")) return "LEVEL_FASTOON_II";
    std::string s = "LEVEL_"; for (const char* p = folder; *p; p++) s += (*p == ' ') ? '_' : (char)toupper((unsigned char)*p);
    return s;
}
void tp_go(const char* level, const char* spot)
{
    const char* e = getenv("TOD_TELEPORT_RELAUNCH");
    if (e && e[0] == '1') { tp_relaunch(level, spot); return; }
    tod_level_go(tp_level_const(level).c_str(), level, spot);
    s_tp_msg = std::string("Going to ") + level + " with your inventory (loads in a few seconds)";
}
void tp_relaunch(const char* level, const char* spot)
{
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string argv = "-no_legal;-no_frontend;";
    const char* fmv = getenv("PS3_NO_FMV"); if (fmv && fmv[0] == '1') argv += "-no_movies;";
    argv += "-level;"; argv += level;
    _putenv_s("PS3_ARGV", argv.c_str());
    std::string lua = spot && *spot ? std::string("hero_warp(Ratchet1,") + spot + ")@45" : std::string();
    _putenv_s("TOD_LUA_EXEC", lua.c_str());
    _putenv_s("TOD_START_DELAY_MS", "1500");
    { /* vram124: a relaunch starts with the default inventory: hand over every weapon and full ammo (the in-process travel keeps the real one) */
      const char* tc = getenv("TOD_CHEAT"); std::string cs = tc && *tc ? tc : ""; if (cs.find("weapons") == std::string::npos) { if (!cs.empty()) cs += ","; cs += "weapons,giveammo"; }
      _putenv_s("TOD_CHEAT", cs.c_str()); }
    _putenv_s("TOD_OVERLAY_SCRIPT", ""); _putenv_s("TOD_OVERLAY_AUTO", "");   /* the new process must not replay this process's script (it would relaunch forever) */
    STARTUPINFOA si = {}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::string cl = GetCommandLineA();
    if (!CreateProcessA(exe, &cl[0], NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { s_tp_msg = "Could not start the new process."; tod_tp_log("[teleport] CreateProcess failed %lu\n", GetLastError()); return; }
    tod_tp_log("[teleport] relaunched pid %lu: PS3_ARGV=%s warp=%s\n", pi.dwProcessId, argv.c_str(), spot ? spot : "-");
    { char pf[MAX_PATH]; snprintf(pf, sizeof pf, "%s", exe); if (char* sl = strrchr(pf, 92)) strcpy(sl + 1, "teleport_pid.txt"); else strcpy(pf, "teleport_pid.txt");
      if (FILE* f = fopen(pf, "w")) { fprintf(f, "%lu\n", pi.dwProcessId); fclose(f); } }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
}
void build_teleport(float W, float H, float k)
{
    static ULONGLONG last = 0;
    const ULONGLONG nowt = GetTickCount64();
    if (!s_tp_cur[0] || nowt - last > 1000) { last = nowt; s_tp_cur[0] = 0; if (g_rsx_level_hook) g_rsx_level_hook(s_tp_cur, sizeof s_tp_cur); }
    int cur = -1;
    const int nl = (int)(sizeof kTpLevels / sizeof kTpLevels[0]);
    for (int i = 0; i < nl; i++) if (!_stricmp(kTpLevels[i].name, s_tp_cur)) cur = i;
    if (s_tp_sel < 0) s_tp_sel = cur >= 0 ? cur : 0;
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760.0f * k, 640.0f * k), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Teleport  -  EXPERIMENTAL / debug##tp", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Current level: %s", s_tp_cur[0] ? s_tp_cur : "(unknown)");
        ImGui::TextDisabled("Click a warp point. Another level restarts the game on it. F5 / Esc closes.");
        ImGui::Separator();
        ImGui::BeginChild("tplv", ImVec2(250.0f * k, 0), true);
        for (int i = 0; i < nl; i++) {
            char lab[96]; snprintf(lab, sizeof lab, "%s%s", kTpLevels[i].name, i == cur ? "  (here)" : "");
            if (ImGui::Selectable(lab, i == s_tp_sel)) s_tp_sel = i;
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("tpsp", ImVec2(0, 0), true);
        const TpLevel& L = kTpLevels[s_tp_sel];
        ImGui::Text("%s", L.name);
        if (s_tp_sel == cur) ImGui::TextDisabled("warp inside this level");
        else ImGui::TextDisabled("restarts the game on this level");
        ImGui::Separator();
        if (ImGui::Button(s_tp_sel == cur ? "Restart this level" : "Go to level start")) tp_go(L.name, NULL);
        ImGui::SameLine();
        if (ImGui::Button("Restart the game on it (all weapons given)")) tp_relaunch(L.name, NULL);
        if (tod_level_go_state() == 3) ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "The level did not change: use the restart button.");
        for (int j = 0; j < 40 && L.warps[j]; j++) {
            if (ImGui::Selectable(L.warps[j])) {
                if (s_tp_sel == cur) {
                    char ch[160]; snprintf(ch, sizeof ch, "hero_warp(Ratchet1,%s)", L.warps[j]);
                    tod_lua_enqueue(ch);
                    s_tp_msg = std::string("Warp: ") + L.warps[j];
                    tod_tp_log("[teleport] warp %s\n", L.warps[j]);
                } else tp_go(L.name, L.warps[j]);
            }
        }
        if (!L.warps[0]) ImGui::TextDisabled("(no warp points known: ship level)");
        ImGui::EndChild();
        if (!s_tp_msg.empty()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", s_tp_msg.c_str());
    }
    ImGui::End();
    if (!open) tp_set_open(false);
}

static volatile HWND s_game_hwnd;   /* menu117: the window that really delivers messages to overlay_message */
int overlay_message(void* hw, unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result)
{
    if (hw && !s_game_hwnd) s_game_hwnd = (HWND)hw;
    if (msg == (WM_APP + 0x108)) { fprintf(stderr, "[menu] message received on the window thread: %s" "\n", (const char*)wp ? (const char*)wp : "?"); tod_menu_apply_key_win((const char*)wp, (const char*)lp); *result = 0; return 1; }
    if (!s_cs_init) return 0;
    const bool open = s_open.load() || s_pick_on.load();
    const bool tp = s_tp_open.load();
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F7 && !(lp & (1 << 30))) { tod_logv2_mark(); }   /* logv2: the user marks "here" (not consumed: the game never used F7) */
        if (wp == VK_F5 && tp_enabled() && !open) { if (!(lp & (1 << 30))) tp_set_open(!tp); *result = 0; return 1; }   /* usr123: teleport menu */
        if (tp && !open) { if (!(lp & (1 << 30)) && wp == VK_ESCAPE) tp_set_open(false); if (wp == VK_F4 && (lp & (1 << 29))) return 0; *result = 0; return 1; }
        if (wp == VK_F1 && s_pick_on.load()) { *result = 0; return 1; }   /* the slot picker owns the input */
        if (wp == VK_F1) {
            if (!(lp & (1 << 30))) set_open(!open);
            *result = 0;
            return 1;
        }
        if (!open) return 0;
        if (wp == VK_F4 && (lp & (1 << 29))) return 0;                  /* Alt+F4 still closes the game, the page open or not */
        if (controls_capturing()) { controls_wm_key(wp, lp); *result = 0; return 1; }   /* Controls tab: the next key is the binding */
        {
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            switch (wp) {
            case VK_UP: push({ 0, (float)A_UP, 0 }); break;
            case VK_DOWN: push({ 0, (float)A_DOWN, 0 }); break;
            case VK_LEFT: push({ 0, (float)A_LEFT, 0 }); break;
            case VK_RIGHT: push({ 0, (float)A_RIGHT, 0 }); break;
            case VK_RETURN: case VK_SPACE: if (!(lp & (1 << 30))) push({ 0, (float)A_ACCEPT, 0 }); break;
            case VK_ESCAPE: if (!(lp & (1 << 30))) push({ 0, (float)A_BACK, 0 }); break;
            case VK_TAB: if (!(lp & (1 << 30))) push({ 0, (float)(shift ? A_TAB_PREV : A_TAB_NEXT), 0 }); break;
            case VK_NEXT: if (!(lp & (1 << 30))) push({ 0, (float)A_TAB_NEXT, 0 }); break;
            case VK_PRIOR: if (!(lp & (1 << 30))) push({ 0, (float)A_TAB_PREV, 0 }); break;
            case VK_F2: if (!(lp & (1 << 30))) push({ 0, (float)A_APPLY, 0 }); break;
            case VK_BACK: if (!(lp & (1 << 30))) push({ 0, (float)A_CLEAR, 0 }); break;   /* Controls tab: clear the binding */
            default: break;
            }
        }
        *result = 0;
        return 1;
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        if (!open && !tp) return 0;
        *result = 0;
        return 1;
    case WM_MOUSEMOVE:
        if (!open && !tp) return 0;
        push({ 1, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp) });
        *result = 0;
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
        if (!open && !tp) return 0;
        push({ 1, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp) });
        push({ 2, 0, msg == WM_LBUTTONUP ? 0.0f : 1.0f });
        *result = 0;
        return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
        if (!open && !tp) return 0;
        push({ 2, 1, msg == WM_RBUTTONUP ? 0.0f : 1.0f });
        *result = 0;
        return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        if (!open) return 0;
        if (msg == WM_MBUTTONDOWN && controls_capturing()) push_capture(VK_MBUTTON);   /* Controls tab: bind the middle button */
        *result = 0;
        return 1;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        if (!open) return 0;
        if (msg == WM_XBUTTONDOWN && controls_capturing()) push_capture(GET_XBUTTON_WPARAM(wp) == XBUTTON2 ? VK_XBUTTON2 : VK_XBUTTON1);
        *result = TRUE;
        return 1;
    case WM_MOUSEWHEEL:
        if (!open && !tp) return 0;
        push({ 3, (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA, 0 });
        *result = 0;
        return 1;
    case WM_SETCURSOR:
        if ((!open && !tp) || LOWORD(lp) != HTCLIENT) return 0;
        SetCursor(LoadCursor(NULL, IDC_ARROW));
        *result = TRUE;
        return 1;
    case WM_KILLFOCUS:
        return 0;
    default:
        return 0;
    }
}

/* ---- ImGui over the engine's command list ------------------------------------ */

const int kSrvHeapSize = 64;
ID3D12DescriptorHeap* s_heap;
std::vector<int> s_free;
D3D12_CPU_DESCRIPTOR_HANDLE s_cpu0;
D3D12_GPU_DESCRIPTOR_HANDLE s_gpu0;
UINT s_inc;
bool s_ready, s_failed;
float s_scale_cur;
ID3D12Device* s_dev_cached;

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g)
{
    const int i = s_free.back();
    s_free.pop_back();
    c->ptr = s_cpu0.ptr + (SIZE_T)i * s_inc;
    g->ptr = s_gpu0.ptr + (UINT64)i * s_inc;
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    s_free.push_back((int)((c.ptr - s_cpu0.ptr) / s_inc));
}

ImFont* s_tag_font;    /* 1.01: Bahnschrift for the version tag, when Windows has it */

/* ---- alert10: the game's own font ---------------------------------------------------------------------------------------
 * Read at start from the player's global_cached.psarc (/built/fonts/test/font1: font.dat, font.tph, font.tp; nothing of it
 * is shipped or written to disk). font.dat (IGHW): section 0x29200 = per font {u32 map offset, u8 line height, ...},
 * section 0x29300 = per font a u16[65536] codepoint -> glyph index, section 0x29400 = glyphs of 16 bytes
 * {u16 cp, u16 x, u16 y, u16 page, u8, u8, u8 w, u8 h, u8 font, pad}. font.tph: per page 32 bytes {u32 offset, u8, u8 mips,
 * u8 fmt (0x86 DXT1), ..., +0x18 u16 w, u16 h}; font.tp: the pages after its IGHW header (data offset at +0x24).
 * Font 0 = the rounded face of the vendor's "Sei sicuro di voler comprare:", font 1 = the wide face of the main menu bar.
 * The glyphs we need (Latin, U+0020..U+017F) are cut from the DXT1 page into ImGui's atlas as custom rectangles (white,
 * alpha = the page's red channel) and drawn as textured quads by gf_text(). Missing file: gf_ok() false, callers fall back. */
struct GfGlyph { ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid; uint8_t w = 0, h = 0; };
struct GfFont { bool ok = false; int lh = 0; GfGlyph g[0x180]; };
GfFont s_gf[2];

uint32_t gf_be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint16_t gf_be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

/* red channel of one texel of a DXT1 page (blocks little-endian, linear, 4x4) */
uint8_t gf_dxt1_red(const uint8_t* pg, int W, int x, int y)
{
    const uint8_t* b = pg + ((size_t)(y >> 2) * (size_t)(W >> 2) + (size_t)(x >> 2)) * 8;
    const uint16_t c0 = (uint16_t)(b[0] | (b[1] << 8)), c1 = (uint16_t)(b[2] | (b[3] << 8));
    const int r0 = ((c0 >> 11) & 31) * 255 / 31, r1 = ((c1 >> 11) & 31) * 255 / 31;
    const uint32_t bits = (uint32_t)b[4] | ((uint32_t)b[5] << 8) | ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24);
    const int i = (bits >> (2 * ((y & 3) * 4 + (x & 3)))) & 3;
    if (i == 0) return (uint8_t)r0;
    if (i == 1) return (uint8_t)r1;
    if (c0 > c1) return (uint8_t)(i == 2 ? (2 * r0 + r1) / 3 : (r0 + 2 * r1) / 3);
    return (uint8_t)(i == 2 ? (r0 + r1) / 2 : 0);
}

void gf_load(ImFontAtlas* atlas)
{
    const char* off = getenv("TOD_GAME_FONT");
    if (off && off[0] == '0') return;
    unsigned char *dat = NULL, *tph = NULL, *tp = NULL; size_t nd = 0, nh = 0, nt = 0;
    const bool got = tod_game_archive_read("/built/fonts/test/font1/font.dat", &dat, &nd) &&
                     tod_game_archive_read("/built/fonts/test/font1/font.tph", &tph, &nh) &&
                     tod_game_archive_read("/built/fonts/test/font1/font.tp", &tp, &nt);
    int made = 0;
    if (got && nd > 0x40 && nh > 0x40 && nt > 0x40) {
        /* IGHW sections of font.dat */
        uint32_t s29200 = 0, s29300 = 0, s29400 = 0, n29400 = 0;
        const uint32_t ns = gf_be32(dat + 8);
        for (uint32_t i = 0; i < ns && 0x20 + i * 16 + 16 <= nd; i++) {
            const uint8_t* e = dat + 0x20 + i * 16;
            const uint32_t id = gf_be32(e), o = gf_be32(e + 4), sz = gf_be32(e + 12);
            if (id == 0x29200) s29200 = o; else if (id == 0x29300) s29300 = o; else if (id == 0x29400) { s29400 = o; n29400 = sz / 16; }
        }
        const uint32_t tbase = gf_be32(tph + 0x24), tcount = gf_be32(tph + 0x28) & 0xFFFF, tdata = gf_be32(tp + 0x24);
        ImTextureData* tex = atlas->TexData;
        for (int f = 0; f < 2 && s29200 && s29300 && s29400; f++) {
            if (s29200 + f * 8 + 8 > nd) break;
            const uint32_t map = gf_be32(dat + s29200 + f * 8);
            GfFont& F = s_gf[f];
            F.lh = dat[s29200 + f * 8 + 4];
            if (map + 0x20000 > nd || !F.lh) continue;
            for (uint32_t cp = 0x20; cp < 0x180; cp++) {
                const uint16_t gi = gf_be16(dat + map + cp * 2);
                if (gi >= n29400) continue;
                const uint8_t* r = dat + s29400 + (size_t)gi * 16;
                if (gf_be16(r) != cp) continue;                       /* not in this font: no glyph */
                const int gx = gf_be16(r + 2), gy = gf_be16(r + 4), page = gf_be16(r + 6), gw = r[10], gh = r[11];
                if ((uint32_t)page >= tcount || !gw || !gh) continue;
                const uint8_t* te = tph + tbase + page * 32;
                if (tbase + page * 32 + 32 > nh || te[6] != 0x86) continue;
                const int PW = gf_be16(te + 0x18), PH = gf_be16(te + 0x1A);
                const size_t po = (size_t)tdata + gf_be32(te);
                if (gx + gw > PW || gy + gh > PH || po + (size_t)PW * PH / 2 > nt) continue;
                ImFontAtlasRect rr;
                const ImFontAtlasRectId id = atlas->AddCustomRect(gw, gh, &rr);
                if (id == ImFontAtlasRectId_Invalid) continue;
                tex = atlas->TexData;
                const int bpp = tex->BytesPerPixel;
                for (int y = 0; y < gh; y++) {
                    unsigned char* px = (unsigned char*)tex->GetPixelsAt(rr.x, rr.y + y);
                    for (int x = 0; x < gw; x++) {
                        const uint8_t a = gf_dxt1_red(tp + po, PW, gx + x, gy + y);
                        if (bpp == 4) { px[x * 4 + 0] = 255; px[x * 4 + 1] = 255; px[x * 4 + 2] = 255; px[x * 4 + 3] = a; }
                        else px[x] = a;
                    }
                }
                ImFontAtlasTextureBlockQueueUpload(atlas, tex, rr.x, rr.y, gw, gh);
                F.g[cp].rect = id; F.g[cp].w = (uint8_t)gw; F.g[cp].h = (uint8_t)gh;
                made++;
            }
            F.ok = F.g['A'].rect != ImFontAtlasRectId_Invalid;
        }
    }
    free(dat); free(tph); free(tp);
    fprintf(stderr, "[overlay] game font: %s (%d glyphs)\n", s_gf[0].ok ? "loaded" : "not found, Windows fonts", made);
}

bool gf_ok(int f) { return f >= 0 && f < 2 && s_gf[f].ok; }

/* next codepoint of a UTF-8 string */
uint32_t gf_utf8(const char*& s)
{
    const unsigned char c = (unsigned char)*s++;
    if (c < 0x80) return c;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = c & (0x3F >> n);
    while (n-- > 0 && (*s & 0xC0) == 0x80) cp = (cp << 6) | (*s++ & 0x3F);
    return cp;
}

/* width of text in face f at size px (= the font's line height scaled to px) */
float gf_width(int f, float px, const char* t)
{
    const GfFont& F = s_gf[f];
    const float k = px / (float)F.lh;
    float w = 0;
    for (const char* s = t; *s;) { const uint32_t cp = gf_utf8(s); w += (cp < 0x180 && F.g[cp].w ? F.g[cp].w : F.g['?'].w) * k; }
    return w;
}

void gf_text(ImDrawList* dl, int f, float px, ImVec2 p, ImU32 col, const char* t)
{
    const GfFont& F = s_gf[f];
    const float k = px / (float)F.lh;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    for (const char* s = t; *s;) {
        uint32_t cp = gf_utf8(s);
        if (cp >= 0x180 || !F.g[cp].w) cp = '?';
        const GfGlyph& g = F.g[cp];
        ImFontAtlasRect rr;
        if (g.rect != ImFontAtlasRectId_Invalid && cp != ' ' && atlas->GetCustomRect(g.rect, &rr))
            dl->AddImage(atlas->TexRef, p, ImVec2(p.x + g.w * k, p.y + g.h * k), rr.uv0, rr.uv1, col);
        p.x += g.w * k;
    }
}

void style_setup(float k)
{
    ImGuiStyle st;
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10.0f;
    st.ChildRounding = 8.0f;
    st.FrameRounding = 6.0f;
    st.WindowPadding = ImVec2(22, 18);
    st.ItemSpacing = ImVec2(10, 8);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.10f, 0.94f);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_Border] = ImVec4(0.95f, 0.55f, 0.10f, 0.55f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.20f, 0.27f, 1.0f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.95f, 0.55f, 0.10f, 0.75f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.55f, 0.10f, 1.0f);
    st.WindowBorderSize = 1.5f;
    st.ScaleAllSizes(k);
    st.FontSizeBase = 20.0f;
    st.FontScaleMain = k;
    ImGui::GetStyle() = st;
}

bool ensure_ready(void* devp, void* queuep, int frames, unsigned w, unsigned h)
{
    if (s_ready) return true;
    if (s_failed) return false;
    ID3D12Device* dev = (ID3D12Device*)devp;
    ID3D12CommandQueue* q = (ID3D12CommandQueue*)queuep;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvHeapSize;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&s_heap)))) { s_failed = true; return false; }
    s_cpu0 = s_heap->GetCPUDescriptorHandleForHeapStart();
    s_gpu0 = s_heap->GetGPUDescriptorHandleForHeapStart();
    s_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int n = kSrvHeapSize; n > 0; n--) s_free.push_back(n - 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    const char* fonts[] = { "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf" };
    bool font = false;
    for (const char* f : fonts)
        if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES && io.Fonts->AddFontFromFileTTF(f, 0.0f)) { font = true; break; }
    if (!font) io.Fonts->AddFontDefault();
    /* 1.01: the version tag of the title screen in Windows' own Bahnschrift (condensed, close to the menu's look;
     * read from the player's Windows, never shipped). Missing (Windows 10 before 1709): the font above. */
    {
        const char* bahn = "C:\\Windows\\Fonts\\bahnschrift.ttf";
        if (GetFileAttributesA(bahn) != INVALID_FILE_ATTRIBUTES) s_tag_font = io.Fonts->AddFontFromFileTTF(bahn, 0.0f);
    }
    gf_load(io.Fonts);
    ImGui::GetStyle().FontSizeBase = 20.0f;
    s_scale_cur = 0.0f;

    ImGui_ImplDX12_InitInfo info;
    info.Device = dev;
    info.CommandQueue = q;
    info.NumFramesInFlight = frames;
    info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = s_heap;
    info.SrvDescriptorAllocFn = srv_alloc;
    info.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&info)) { s_failed = true; return false; }
    s_dev_cached = dev;
    s_ready = true;
    fprintf(stderr, "[overlay] ImGui %s ready (%ux%u)\n", ImGui::GetVersion(), w, h);
    return true;
}

int s_tab, s_sel;        /* s_sel: a row of the tab (visible index), or -1 - n for the footer button n */
double s_pad_next_rep;
bool s_pad_wait_release;
double s_auto_at = -1;
int s_auto_tab = -1;
double s_t_first;

std::vector<int> tab_rows(int tab)
{
    std::vector<int> v;
    for (size_t i = 0; i < s_rows.size(); i++) if (s_rows[i].tab == tab) v.push_back((int)i);
    return v;
}

/* A Cheats row: send its command(s) now. Needs a running level (the hero), else says so and does nothing. */
void cheat_activate(Row& r)
{
    if (!tod_hero_handle()) { say("Load a level first: the cheats need a running game."); return; }
    tod_cheat_arm();
    if (r.cheat == 2) {
        tod_cheat_enqueue(r.cmd_on);
        char b[160]; snprintf(b, sizeof b, "Sent: %s.", r.label);
        say(b);
        return;
    }
    const int on = r.running ? 0 : 1;
    tod_cheat_enqueue(on ? r.cmd_on : r.cmd_off);
    r.running = r.saved = r.pending = on;
    char b[160]; snprintf(b, sizeof b, "%s: %s.", r.label, on ? "on" : "off");
    say(b);
}

/* "Saves folder": the save root cellSaveData.c uses (PS3_SAVEDATA_ROOT, else gamedata\dev_hdd0\home\00000001\savedata
 * under the working directory), created if it is not there yet, opened in Explorer. */
void open_saves_folder()
{
    const char* e = getenv("PS3_SAVEDATA_ROOT");
    char full[MAX_PATH];
    if (!GetFullPathNameA(e && *e ? e : "gamedata\\dev_hdd0\\home\\00000001\\savedata", MAX_PATH, full, NULL)) {
        say("The saves folder could not be found.");
        return;
    }
    for (char* c = full; *c; c++) if (*c == '/') *c = '\\';
    SHCreateDirectoryExA(NULL, full, NULL);
    if ((INT_PTR)ShellExecuteA(NULL, "open", full, NULL, NULL, SW_SHOWNORMAL) <= 32) say("Explorer could not open the saves folder.");
    else say("Opened the saves folder in Explorer.");
}

void change_value(Row& r, int dir)
{
    if (r.soon) return;
    if (r.ctl) { controls_change(r, dir); return; }     /* Controls tab (keys136) */
    if (r.cheat == 4) { open_saves_folder(); return; }
    if (r.cheat == 5) { tod_update_open_page(); say("Opened the download page in the browser."); return; }
    if (r.cheat) { cheat_activate(r); return; }
    const int n = (int)r.choices.size();
    int cur = r.pending >= 0 ? r.pending : (dir > 0 ? -1 : 0);
    cur = (cur + dir + n) % n;
    r.pending = cur;
    r.other.clear();
}

void do_act(int act)
{
    if (s_pick_on.load()) { pick_act(act); return; }
    s_scroll_follow = true;     /* a key / pad move: scroll the selected row into view (Controls tab) */
    if (controls_capturing()) {  /* waiting for a key: only "back" (the pad's circle) does anything: it cancels */
        if (act == A_BACK || act == A_CLOSE) controls_cancel();
        return;
    }
    std::vector<int> rows = tab_rows(s_tab);
    std::vector<int> nav;   /* the rows nav can land on */
    for (int i : rows) if (!s_rows[i].soon) nav.push_back(i);
    const int nrows = (int)nav.size();
    const bool footer = s_sel < 0;
    auto sel_row = [&]() -> Row* { return (!footer && s_sel < nrows) ? &s_rows[nav[s_sel]] : nullptr; };
    switch (act) {
    case A_UP:
        if (footer) s_sel = nrows ? nrows - 1 : -1;
        else if (s_sel > 0) s_sel--;
        else s_sel = -1;      /* wraps to the footer */
        break;
    case A_DOWN:
        if (footer) s_sel = nrows ? 0 : -1;
        else if (s_sel + 1 < nrows) s_sel++;
        else s_sel = -1;
        break;
    case A_LEFT:
        if (footer) s_sel = s_sel == -1 ? -3 : s_sel + 1;
        else if (Row* r = sel_row()) change_value(*r, -1);
        break;
    case A_RIGHT:
        if (footer) s_sel = s_sel == -3 ? -1 : s_sel - 1;
        else if (Row* r = sel_row()) change_value(*r, +1);
        break;
    case A_ACCEPT:
        if (footer) { if (s_sel == -1) apply_and_save(); else if (s_sel == -2) revert(); else defaults(); }
        else if (Row* r = sel_row()) change_value(*r, +1);
        break;
    case A_BACK: case A_CLOSE: set_open(false); break;
    case A_TAB_NEXT: s_tab = (s_tab + 1) % NTABS; s_sel = 0; break;
    case A_TAB_PREV: s_tab = (s_tab + NTABS - 1) % NTABS; s_sel = 0; break;
    case A_APPLY: apply_and_save(); break;
    case A_REVERT: revert(); break;
    case A_CLEAR: if (Row* r = sel_row()) controls_clear(*r); break;     /* Controls tab: unbind the selected key */
    }
}

void pad_actions(double t)
{
    const uint32_t b = s_pad.load();
    if (s_pad_wait_release) {
        if (!(b & CELL_PAD_CTRL_SELECT)) s_pad_wait_release = false;
        s_pad_prev = b;
        return;
    }
    const uint32_t down = b & ~s_pad_prev;
    if (down & CELL_PAD_CTRL_SELECT) { set_open(false); s_pad_prev = b; return; }
    struct { uint32_t bit; int act; bool rep; } map[] = {
        { CELL_PAD_CTRL_UP, A_UP, true }, { CELL_PAD_CTRL_DOWN, A_DOWN, true },
        { CELL_PAD_CTRL_LEFT, A_LEFT, true }, { CELL_PAD_CTRL_RIGHT, A_RIGHT, true },
        { CELL_PAD_CTRL_CROSS, A_ACCEPT, false }, { CELL_PAD_CTRL_CIRCLE, A_BACK, false },
        { CELL_PAD_CTRL_START, A_BACK, false },
        { CELL_PAD_CTRL_L1, A_TAB_PREV, false }, { CELL_PAD_CTRL_R1, A_TAB_NEXT, false },
        { CELL_PAD_CTRL_SQUARE, A_APPLY, false }, { CELL_PAD_CTRL_TRIANGLE, A_REVERT, false },
    };
    uint32_t held_rep = 0;
    for (auto& m : map) {
        if (down & m.bit) {
            do_act(m.act);
            if (m.rep) { held_rep = m.bit; s_pad_next_rep = t + 0.40; }
        } else if (m.rep && (b & m.bit) && t >= s_pad_next_rep && !(down & 0xFFFF)) {
            do_act(m.act);
            s_pad_next_rep = t + 0.09;
        }
    }
    (void)held_rep;
    s_pad_prev = b;
}

void build_page(float W, float H, float k)
{
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 150));

    const float pw = std::min(W - 40.0f * k, 980.0f * k), ph = std::min(H - 40.0f * k, 760.0f * k);
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##tod_settings", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.62f, 0.16f, 1));
    ImGui::TextUnformatted("SETTINGS");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::TextDisabled("  Ratchet & Clank: Tools of Destruction - port %s", TOD_RELEASE_VERSION);
    if (tod_update_state() == 2) {      /* 1.01 */
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1), "   Version %s is available: System > Updates", tod_update_latest());
    }
    ImGui::Spacing();

    /* tabs */
    const float tabw = 118.0f * k;   /* 1.2: six tabs (Credits) plus the hint fit the page */
    for (int t = 0; t < NTABS; t++) {
        if (t) ImGui::SameLine();
        const bool on = t == s_tab;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.55f, 0.10f, 0.9f));
        if (ImGui::Button(kTabName[t], ImVec2(tabw, 0))) { s_tab = t; s_sel = 0; }
        if (on) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("   L1/R1 or Tab");
    ImGui::Separator();

    const float footer_h = 120.0f * k;
    {   /* the Controls tab is long: it scrolls (wheel, scroll bar, or the selection moving); the others never do */
        static int last_tab = -1;
        if (last_tab != s_tab) { ImGui::SetNextWindowScroll(ImVec2(0, 0)); last_tab = s_tab; }
    }
    ImGui::BeginChild("##rows", ImVec2(0, ImGui::GetContentRegionAvail().y - footer_h), false,
                      s_tab == TAB_CONTROLS ? 0 : ImGuiWindowFlags_NoScrollbar);
    std::vector<int> rows = tab_rows(s_tab);
    const float rw = ImGui::GetContentRegionAvail().x;
    const float rh = ImGui::GetFrameHeight() + 10.0f * k;
    int nav_idx = 0;
    const char* desc = "";
    bool mouse_moved = ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0;
    if (s_tab == TAB_SOON) {
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1, 1), "Planned features (not available yet)");
        ImGui::Spacing();
    }
    const bool in_game = s_tab == TAB_CHEATS && tod_hero_handle() != 0;
    if (s_tab == TAB_CHEATS) {
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Cheats affect your save if you save while they are on.");
        if (!in_game) ImGui::TextColored(ImVec4(0.7f, 0.8f, 1, 1), "Load a level first: the cheats need a running game.");
        else ImGui::TextDisabled("Not saved in tod.ini: all cheats are off at every start.");
        ImGui::Spacing();
    }
    if (s_tab == TAB_CONTROLS) {
        ImGui::TextColored(ImVec4(0.7f, 0.8f, 1, 1), "Enter or click a row, then press the new key. Esc cancels, Backspace clears. A key may drive several inputs. Saved at once.");
        ImGui::TextDisabled("Keyboard button prompts (TOD_PROMPTS = keyboard) still show the default keys.");
        ImGui::Spacing();
    }
    for (int ri = 0; ri < (int)rows.size(); ri++) {
        Row& r = s_rows[rows[ri]];
        if (r.group) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1, 1), "%s", r.group);
            ImGui::Spacing();
        }
        const bool grey = r.cheat && r.cheat != 4 && r.cheat != 5 && !r.ctl && !in_game;      /* a cheat with no level running: shown, inert */
        if (s_tab == TAB_SOON && ri == 5) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1, 1), "Ray tracing (coming soon)");
            ImGui::Spacing();
        }
        const int my = r.soon ? -100 : nav_idx++;
        ImGui::PushID(ri);
        ImVec2 p = ImGui::GetCursorScreenPos();
        const bool selected = !r.soon && my == s_sel;
        ImGui::InvisibleButton("##row", ImVec2(rw, rh));
        const bool hov = ImGui::IsItemHovered();
        if (selected && s_scroll_follow && !ImGui::IsRectVisible(p, ImVec2(p.x + rw, p.y + rh))) ImGui::SetScrollHereY(0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov && mouse_moved && !r.soon) s_sel = my;
        if (selected) dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), IM_COL32(240, 140, 25, 70), 6.0f * k);
        else if (hov && !r.soon) dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), IM_COL32(255, 255, 255, 18), 6.0f * k);
        const ImU32 txt = ((r.soon && !r.info) || grey) ? IM_COL32(120, 125, 140, 255) : IM_COL32(235, 238, 245, 255);
        const float ty = p.y + (rh - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(p.x + 14.0f * k, ty), txt, r.label);
        if (r.experimental) {
            const float lx = p.x + 14.0f * k + ImGui::CalcTextSize(r.label).x + 10.0f * k;
            const float fs = ImGui::GetFontSize() * 0.72f;
            const float tw = ImGui::GetFont()->CalcTextSizeA(fs, FLT_MAX, 0.0f, "EXPERIMENTAL").x;
            const float tt = p.y + (rh - fs) * 0.5f;
            dl->AddRect(ImVec2(lx - 4.0f * k, tt - 2.0f * k), ImVec2(lx + tw + 4.0f * k, tt + fs + 2.0f * k), IM_COL32(255, 170, 60, 200), 3.0f * k);
            dl->AddText(ImGui::GetFont(), fs, ImVec2(lx, tt), IM_COL32(255, 170, 60, 255), "EXPERIMENTAL");
        }
        if (selected || hov) desc = r.desc;
        if (selected && r.desc) desc = r.desc;
        if (!r.soon) {
            const std::string vt = r.ctl ? controls_value_text(r) : value_text(r, r.pending);
            const bool changed = r.pending != r.saved;
            const ImU32 vc = changed ? IM_COL32(255, 205, 80, 255)
                                     : (r.cheat == 1 && r.pending == 1 && !grey) ? IM_COL32(130, 225, 130, 255) : txt;
            const float vw = ImGui::CalcTextSize(vt.c_str()).x;
            const float colw = 330.0f * k;
            const float cx = p.x + rw - 20.0f * k - colw * 0.5f;
            dl->AddText(ImVec2(cx - vw * 0.5f, ty), vc, vt.c_str());
            const float ay = p.y + rh * 0.5f, ah = 8.0f * k;
            const ImVec2 lc(cx - colw * 0.5f + 10.0f * k, ay), rc(cx + colw * 0.5f - 10.0f * k, ay);
            if (r.cheat == 2 || r.cheat == 4 || r.cheat == 5 || r.ctl == 1 || r.ctl == 3) {      /* a button (or a key binding): a pill round the word, no arrows */
                const float pad = 14.0f * k;
                dl->AddRect(ImVec2(cx - vw * 0.5f - pad, p.y + 3.0f * k), ImVec2(cx + vw * 0.5f + pad, p.y + rh - 3.0f * k),
                            grey ? IM_COL32(90, 95, 110, 255) : selected ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 155, 170, 255),
                            6.0f * k, 0, 1.5f * k);
            } else {
                dl->AddTriangleFilled(ImVec2(lc.x + ah, lc.y - ah), ImVec2(lc.x + ah, lc.y + ah), ImVec2(lc.x - ah * 0.6f, lc.y),
                                      selected ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 155, 170, 255));
                dl->AddTriangleFilled(ImVec2(rc.x - ah, rc.y - ah), ImVec2(rc.x - ah, rc.y + ah), ImVec2(rc.x + ah * 0.6f, rc.y),
                                      selected ? IM_COL32(255, 200, 90, 255) : IM_COL32(150, 155, 170, 255));
            }
            if (!r.live) {
                const char* tag = "restart";
                dl->AddText(ImVec2(p.x + rw - 20.0f * k - colw - ImGui::CalcTextSize(tag).x - 10.0f * k, ty),
                            IM_COL32(150, 130, 90, 255), tag);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                const float mx = ImGui::GetIO().MousePos.x;
                s_sel = my;
                if (mx < cx - colw * 0.25f) change_value(r, -1);
                else change_value(r, +1);
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && r.cheat != 2 && r.cheat != 4 && r.cheat != 5 && r.ctl != 1 && r.ctl != 3) { s_sel = my; change_value(r, -1); }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && r.ctl == 1) { s_sel = my; controls_clear(r); }   /* unbind */
        } else if (!r.info) {
            const char* t = "coming soon";
            dl->AddText(ImVec2(p.x + rw - 20.0f * k - ImGui::CalcTextSize(t).x - 40.0f * k, ty), IM_COL32(120, 125, 140, 255), t);
            if (hov) desc = r.desc;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    s_scroll_follow = false;

    /* description + status */
    ImGui::Separator();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextDisabled("%s", desc);
    ImGui::PopTextWrapPos();
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 78.0f * k - ImGui::GetStyle().WindowPadding.y);
    if (now_s() < s_status_until) ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.55f, 1), "%s", s_status.c_str());
    else if (dirty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "Unapplied changes");
    else ImGui::TextUnformatted(" ");

    /* footer buttons: selection -1 apply, -2 undo, -3 defaults */
    struct { const char* t; int sel; } fb[] = { { "Apply and save", -1 }, { "Discard changes", -2 }, { "Defaults", -3 } };
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine();
        const bool sel = s_sel == fb[i].sel;
        if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.95f, 0.55f, 0.10f, 0.9f));
        if (ImGui::Button(fb[i].t, ImVec2(210.0f * k, 0))) {
            s_sel = fb[i].sel;
            do_act(A_ACCEPT);
        }
        if (ImGui::IsItemHovered() && mouse_moved) s_sel = fb[i].sel;
        if (sel) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(110.0f * k, 0))) set_open(false);
    ImGui::TextDisabled("Keyboard: arrows, Enter, Tab, Esc/F1 closes   Pad: cross changes, circle closes, square applies, triangle discards");
    ImGui::End();
}

/* ---- the FPS counter ---------------------------------------------------------
 * Fed from overlay_render, i.e. once per real present. A fixed ring of present
 * timestamps (no allocation); the numbers are recomputed 4 times a second from it:
 * fps over the last 1 s, 1% low = fps at the 99th-percentile frame time of the last 5 s
 * (ring-limited to the last kRing frames), and the average frame time of the last 1 s. */
const int kRing = 4096;
double s_ts[kRing];
float s_dt[kRing];
unsigned s_ts_n;            /* frames recorded since the counter was switched on */
double s_fps_next;          /* next text refresh */
char s_fps_l[6][48];
int s_fps_lines;
int s_fps_prev_mode;

/* usr123: GPU / CPU load (and video memory) next to the frame time, sampled twice a second.
 * CPU: GetProcessTimes (kernel + user of this process) over the wall time, divided by the logical cores = the Task Manager figure.
 * GPU: the sum of the GPU time of the submitted command lists (D3D12 timestamps, g_rsx_gpu_busy_ms_total) over the wall time;
 *      100 = the GPU never idles (the frame is GPU-bound). VRAM: DXGI QueryVideoMemoryInfo through g_rsx_vram_hook. */
struct LoadSample { bool have; double wall, cpu_s, gpu_ms; double cpu_pct, gpu_pct; double vram_u, vram_b; } s_load;
void load_sample()
{
    LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
    const double wall = (double)q.QuadPart / (double)f.QuadPart;
    if (s_load.have && wall - s_load.wall < 0.5) return;
    FILETIME c, e, k, u; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    const double cpu_s = (((double)((((unsigned long long)k.dwHighDateTime << 32) | k.dwLowDateTime)) +
                           (double)((((unsigned long long)u.dwHighDateTime << 32) | u.dwLowDateTime))) * 1e-7);
    const double gms = g_rsx_gpu_busy_ms_total;
    static int ncpu = 0; if (!ncpu) { SYSTEM_INFO si; GetSystemInfo(&si); ncpu = si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1; }
    if (s_load.have) {
        const double dw = wall - s_load.wall;
        s_load.cpu_pct = std::min(100.0, 100.0 * (cpu_s - s_load.cpu_s) / (dw * ncpu));
        s_load.gpu_pct = g_rsx_gpu_load_ok ? std::min(100.0, 100.0 * (gms - s_load.gpu_ms) / (dw * 1000.0)) : -1.0;
    }
    unsigned long long vu = 0, vb = 0;
    if (g_rsx_vram_hook && g_rsx_vram_hook(&vu, &vb)) { s_load.vram_u = (double)vu / 1073741824.0; s_load.vram_b = (double)vb / 1073741824.0; }
    s_load.wall = wall; s_load.cpu_s = cpu_s; s_load.gpu_ms = gms; s_load.have = true;
}

void fps_note(double t)
{
    if (s_fps_prev_mode != s_fps_mode) { s_fps_prev_mode = s_fps_mode; s_ts_n = 0; s_fps_next = 0; s_fps_lines = 0; }
    s_ts[s_ts_n % kRing] = t;
    s_ts_n++;
    if (t < s_fps_next || s_ts_n < 2) return;
    s_fps_next = t + 0.25;
    const unsigned avail = std::min<unsigned>(s_ts_n, kRing);
    unsigned n1 = 0, n5 = 0;                       /* frames inside the last 1 s / 5 s (as intervals) */
    for (unsigned i = 1; i < avail; i++) {
        const double ti = s_ts[(s_ts_n - 1 - i) % kRing];
        if (t - ti > 5.0) break;
        n5 = i;
        if (t - ti <= 1.0) n1 = i;
    }
    if (!n1) n1 = 1;
    const double span1 = t - s_ts[(s_ts_n - 1 - n1) % kRing];
    const double fps = span1 > 0 ? n1 / span1 : 0;
    snprintf(s_fps_l[0], sizeof s_fps_l[0], "%.0f FPS", fps);
    s_fps_lines = 1;
    if (s_fps_mode >= 2) {
        for (unsigned i = 0; i < n5; i++)
            s_dt[i] = (float)(s_ts[(s_ts_n - 1 - i) % kRing] - s_ts[(s_ts_n - 2 - i) % kRing]);
        double low = fps;
        if (n5 >= 8) {
            const unsigned k99 = std::min(n5 - 1, (unsigned)(n5 * 0.99));
            std::nth_element(s_dt, s_dt + k99, s_dt + n5);
            if (s_dt[k99] > 0) low = 1.0 / s_dt[k99];
        }
        snprintf(s_fps_l[1], sizeof s_fps_l[1], "%.1f ms", fps > 0 ? 1000.0 / fps : 0.0);
        snprintf(s_fps_l[2], sizeof s_fps_l[2], "1%% low %.0f", low);
        s_fps_lines = 3;
        load_sample();
        if (s_load.gpu_pct >= 0) snprintf(s_fps_l[3], sizeof s_fps_l[3], "GPU %.0f%%", s_load.gpu_pct);
        else snprintf(s_fps_l[3], sizeof s_fps_l[3], "GPU n/a");
        snprintf(s_fps_l[4], sizeof s_fps_l[4], "CPU %.0f%%", s_load.cpu_pct);
        s_fps_lines = 5;
        if (s_load.vram_b > 0) { snprintf(s_fps_l[5], sizeof s_fps_l[5], "VRAM %.1f/%.1f GB", s_load.vram_u, s_load.vram_b); s_fps_lines = 6; }
    }
}

/* Top-right, over the game (its health bar is top-left). Foreground draw list: no window. */
void fps_draw(float W, float k)
{
    if (s_fps_lines <= 0) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float pad = 8.0f * k, margin = 14.0f * k;
    float tw = 0, th = 0;
    ImVec2 sz[6];
    for (int i = 0; i < s_fps_lines; i++) {
        sz[i] = ImGui::CalcTextSize(s_fps_l[i]);
        tw = std::max(tw, sz[i].x); th += sz[i].y;
    }
    const ImVec2 a(W - margin - tw - 2 * pad, margin), b(W - margin, margin + th + 2 * pad);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), 6.0f * k);
    float y = a.y + pad;
    for (int i = 0; i < s_fps_lines; i++) {
        const ImU32 col = i == 0 ? IM_COL32(255, 200, 80, 255) : IM_COL32(230, 230, 230, 255);
        dl->AddText(ImVec2(b.x - pad - sz[i].x, y), col, s_fps_l[i]);
        y += sz[i].y;
    }
}

/* 1.01: "a newer version is available", bottom-left for 25 s from the moment the check (update_check.cpp) says so. */
double s_upd_until = -1;     /* -1: not shown yet; then the end of the notice */

/* r121s: the notice shows 25 s after start anywhere, and all the time on the title screen / main menu (no level
 * loaded) until the settings page has been opened once. */
static bool s_upd_seen = false;
bool update_notice_on(double t)
{
    if (s_upd_seen) return false;
    if (s_upd_until < 0) {
        if (tod_update_state() != 2) return false;
        s_upd_until = t + 25.0;
    }
    return t < s_upd_until || (tod_no_level_yet() && tod_hero_handle() == 0);
}

void update_notice_draw(float H, float k)
{
    char l1[96], l2[160];
    snprintf(l1, sizeof l1, "UPDATE AVAILABLE: version %s (you have %s)", tod_update_latest(), TOD_RELEASE_VERSION);
    snprintf(l2, sizeof l2, "Close the game and run Update.bat in the game folder, or open F1 > System > Updates");
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = ImGui::GetFont();
    const float s1 = ImGui::GetFontSize() * 1.45f, s2 = ImGui::GetFontSize() * 1.05f;
    const ImVec2 z1 = f->CalcTextSizeA(s1, FLT_MAX, 0.0f, l1), z2 = f->CalcTextSizeA(s2, FLT_MAX, 0.0f, l2);
    const float pad = 14.0f * k, gap = 6.0f * k, bw = std::max(z1.x, z2.x) + 2 * pad, bh = z1.y + gap + z2.y + 2 * pad;
    const float W = ImGui::GetIO().DisplaySize.x;
    (void)H; const ImVec2 a((W - bw) * 0.5f, 8.0f * k), b(a.x + bw, a.y + bh);   /* top centre, close to the edge: the main menu items sit at the bottom, the Options panel title just below */
    dl->AddRectFilled(a, b, IM_COL32(10, 16, 28, 235), 8.0f * k);
    dl->AddRect(a, b, IM_COL32(232, 140, 40, 255), 8.0f * k, 0, 2.0f * k);
    dl->AddText(f, s1, ImVec2(a.x + (bw - z1.x) * 0.5f, a.y + pad), IM_COL32(255, 176, 64, 255), l1);
    dl->AddText(f, s2, ImVec2(a.x + (bw - z2.x) * 0.5f, a.y + pad + z1.y + gap), IM_COL32(230, 230, 236, 255), l2);
}

/* 1.01: the port's version, bottom-right, while no level is loaded (title screen, main menu, loading).
 * TOD_VERSION_TAG=0 hides it. */
bool version_tag_on()
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("TOD_VERSION_TAG"); on = !(e && e[0] == '0'); }
    /* not tod_hero_handle() == 0: without cheats armed the hero is often not found in a level, so the tag stayed on in play */
    return on && tod_no_level_yet() && tod_hero_handle() == 0;
}

void version_tag_draw(float W, float H, float k)
{
    static const char kLine[] = "TOOLS OF DESTRUCTION RECOMPILED " TOD_RELEASE_VERSION;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (gf_ok(0)) {   /* alert10: the game's own font (the rounded face of its menus and boxes) */
        const float px = 30.0f * k, margin = 14.0f * k;
        const float w = gf_width(0, px, kLine);
        const float pic_h = W * 704.0f / 1280.0f, band = pic_h < H ? (H - pic_h) * 0.5f : 0.0f;
        const ImVec2 at(W - margin - w, H - band - margin - px * 0.85f);
        gf_text(dl, 0, px, ImVec2(at.x + 1.5f * k, at.y + 1.5f * k), IM_COL32(0, 0, 0, 120), kLine);
        gf_text(dl, 0, px, at, IM_COL32(225, 225, 225, 135), kLine);
        return;
    }
    ImFont* f = s_tag_font ? s_tag_font : ImGui::GetFont();
    const float size = 21.0f * k, margin = 14.0f * k;
    const ImVec2 sz = f->CalcTextSizeA(size, FLT_MAX, 0.0f, kLine);
    /* above the black band of the letterbox (the game's picture is 1280x704, a little wider than 16:9) */
    const float pic_h = W * 704.0f / 1280.0f;
    const float band = pic_h < H ? (H - pic_h) * 0.5f : 0.0f;
    const ImVec2 at(W - margin - sz.x, H - band - margin - sz.y);
    const float b = s_tag_font ? 0.7f * k : 0.0f;      /* a second pass a hair to the right: a bolder weight */
    dl->AddText(f, size, ImVec2(at.x + 1.5f * k, at.y + 1.5f * k), IM_COL32(0, 0, 0, 110), kLine);   /* shadow */
    if (b > 0) dl->AddText(f, size, ImVec2(at.x + 1.5f * k + b, at.y + 1.5f * k), IM_COL32(0, 0, 0, 110), kLine);
    dl->AddText(f, size, at, IM_COL32(225, 225, 225, 125), kLine);      /* see-through: it must not compete with the menu */
    if (b > 0) dl->AddText(f, size, ImVec2(at.x + b, at.y), IM_COL32(225, 225, 225, 125), kLine);
}

/* 10/11, issue #25 point 2: SELECT / BACK under the pause menu's lists, laid out like the game's own hints of the MISSION page
 * (h_hint2_t122, 1080p: one row centred on x 960, text ink y 948..976 in the game's plain face, colour (93,108,132),
 * "INDIETRO" 189 px wide, the button's glyph ~30 px left of its word). The glyphs are the button prompts' own (circle /
 * cross of the set on screen, the keyboard's caps with the keys as bound: src/patches/prompts.cpp). */
struct HintGlyph { ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid; int sig = 0, w = 0, h = 0; double t = -10.0; };
HintGlyph s_hint_g[2];   /* 0 cross (select), 1 circle (back) */
void hint_glyph_refresh(int i, int px)
{
    HintGlyph& G = s_hint_g[i];
    const double t = now_s();
    if (t - G.t < 0.5 && G.rect != ImFontAtlasRectId_Invalid) return;   /* the set / the keys: looked at twice a second */
    G.t = t;
    static std::vector<uint8_t> buf(208 * 208 * 4);
    int w = 0, h = 0;
    const int sig = tod_prompts_glyph(i == 0 ? 0xE003u : 0xE001u, buf.data(), (int)buf.size(), &w, &h);
    const int f = std::max(1, std::min(8, w / std::max(8, 2 * px)));   /* box-shrunk to about twice the size drawn */
    const int key = sig * 16 + f;
    if (!sig || w <= 0 || (key == G.sig && G.rect != ImFontAtlasRectId_Invalid)) return;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (!atlas->TexData || atlas->TexData->BytesPerPixel != 4) return;
    if (G.rect != ImFontAtlasRectId_Invalid) { atlas->RemoveCustomRect(G.rect); G.rect = ImFontAtlasRectId_Invalid; }
    const int ow = w / f, oh = h / f;
    ImFontAtlasRect rr;
    G.rect = atlas->AddCustomRect(ow, oh, &rr);
    if (G.rect == ImFontAtlasRectId_Invalid) return;
    ImTextureData* tex = atlas->TexData;
    for (int y = 0; y < oh; y++) {
        unsigned char* d = (unsigned char*)tex->GetPixelsAt(rr.x, rr.y + y);
        for (int x = 0; x < ow; x++) {
            unsigned sa = 0, sr = 0, sg = 0, sb = 0;
            for (int j = 0; j < f; j++) for (int q = 0; q < f; q++) {
                const uint8_t* s = &buf[((size_t)(y * f + j) * w + x * f + q) * 4];
                sa += s[3]; sr += s[0] * s[3]; sg += s[1] * s[3]; sb += s[2] * s[3];
            }
            d[x * 4 + 0] = (unsigned char)(sa ? sr / sa : 0); d[x * 4 + 1] = (unsigned char)(sa ? sg / sa : 0);
            d[x * 4 + 2] = (unsigned char)(sa ? sb / sa : 0); d[x * 4 + 3] = (unsigned char)(sa / (unsigned)(f * f));
        }
    }
    ImFontAtlasTextureBlockQueueUpload(atlas, tex, rr.x, rr.y, ow, oh);
    G.sig = key; G.w = ow; G.h = oh;
    fprintf(stderr, "[pausehint] glyph %s: set signature %d, %dx%d\n", i == 0 ? "cross" : "circle", sig, ow, oh);
}
/* 10/11: in-game combat log (TOD_COMBAT_LOG=2, combat_log.cpp): the last lines on the left, below the HUD's health, fading out
 * after ~4 s. White = damage dealt, gold = kill, red = damage taken by Ratchet, cyan = XP / level up. Hidden while a menu or the
 * pause is open ([0x10731D00] != 0). */
extern "C" int tod_combat_log_visible(void);
extern "C" int tod_combat_log_lines(int max_age, int max_n, int* kinds, char (*texts)[96], int* ages);
static bool combat_log_on(void)
{
    return tod_combat_log_visible() != 0;
}
static void combat_log_draw(float W, float H)
{
    (void)W;
    int kinds[10], ages[10]; char texts[10][96];
    const int n = tod_combat_log_lines(260, 10, kinds, texts, ages);
    if (n <= 0) return;
    const float k = H / 1080.0f, px = 26.0f * k, step = 30.0f * k;
    const bool gf = gf_ok(0);
    ImFont* wf = ImGui::GetFont();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float y = H * 0.30f;
    for (int i = 0; i < n; i++, y += step) {
        const float a = ages[i] < 200 ? 1.0f : (float)(260 - ages[i]) / 60.0f;
        const int al = (int)(255 * (a < 0 ? 0 : a));
        static const ImU32 rgb[4] = { IM_COL32(235, 235, 235, 0), IM_COL32(255, 90, 80, 0), IM_COL32(110, 220, 255, 0), IM_COL32(255, 200, 60, 0) };
        const ImU32 col = (rgb[kinds[i] & 3] & 0x00FFFFFFu) | ((ImU32)al << 24);
        const ImVec2 pt(40.0f * k, y), sh(pt.x + 2.0f * k, y + 2.0f * k);
        if (gf) { gf_text(dl, 0, px, sh, IM_COL32(0, 0, 0, al * 3 / 4), texts[i]); gf_text(dl, 0, px, pt, col, texts[i]); }
        else { dl->AddText(wf, px * 0.8f, sh, IM_COL32(0, 0, 0, al * 3 / 4), texts[i]); dl->AddText(wf, px * 0.8f, pt, col, texts[i]); }
    }
}

void pause_hint_draw(float W, float H)
{
    if (!gf_ok(0)) return;
    const float u = H / 1080.0f;                                   /* the pause scene: 16:9 picture, H tall, centred */
    static float s_px1 = 0.0f;                                     /* the face's size for 1 px of "INDIETRO" width at 1080p */
    if (s_px1 <= 0.0f) { const float w1 = gf_width(0, 100.0f, "INDIETRO"); s_px1 = w1 > 1.0f ? 189.0f * 100.0f / w1 : 34.0f; }
    const float px = s_px1 * u;
    const float ink = 29.0f * u;                                   /* the words' ink height */
    const float gs = 32.0f * u;                                    /* the glyph's box */
    hint_glyph_refresh(0, (int)gs); hint_glyph_refresh(1, (int)gs);
    const char* w0 = tod_menu_text(157); const char* w1 = tod_menu_text(158);
    const float gap_g = 24.0f * u, gap_w = 26.0f * u;              /* glyph -> word, word -> next glyph (h_hint3: as MISSION's) */
    const float tw0 = gf_width(0, px, w0), tw1 = gf_width(0, px, w1);
    const float total = gs + gap_g + tw0 + gap_w + gs + gap_g + tw1;
    float x = W * 0.5f - total * 0.5f;
    const float cy = 962.0f * u;                                   /* the middle of the row */
    const float ty = cy - ink * 0.5f - (px - ink) * 0.62f;         /* the face's box sits above its ink (calibrated on h_hint3) */
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const ImU32 col = IM_COL32(93, 108, 132, 255);
    auto glyph = [&](int i) {
        ImFontAtlasRect rr;
        if (s_hint_g[i].rect != ImFontAtlasRectId_Invalid && atlas->GetCustomRect(s_hint_g[i].rect, &rr))
            dl->AddImage(atlas->TexRef, ImVec2(x, cy - gs * 0.5f), ImVec2(x + gs, cy + gs * 0.5f), rr.uv0, rr.uv1, IM_COL32(150, 154, 162, 255));   /* the game shows them dimmed */
        x += gs + gap_g;
    };
    glyph(0); gf_text(dl, 0, px, ImVec2(x, ty), col, w0); x += tw0 + gap_w;
    glyph(1); gf_text(dl, 0, px, ImVec2(x, ty), col, w1);
}

/* alert10: the fifth item of the main menu bar (NUOVO GIOCO / CARICA / ELIMINA / OPZIONI / ESCI), drawn like the game's own
 * items: the wide face of the game's font, flat colour (grey-gold, white when selected), and the mirror image fading out
 * below. Measured at 1280x720 in all 11 languages (log\quit125\lang_*): the bar always ends at x 1032, the text's ink
 * spans y 637..652 (16 px, the font's ink at scale 0.51), the reflection starts 2 px lower at ~19 % and fades to ~6 %. */
void main_quit_draw(float W, float H)
{
    int n = 0; float fx = 0, fy = 0;
    const int st = tod_fe_quit_state(&n, &fx, &fy);
    if (!st || !gf_ok(1)) return;
    /* the game's picture is 1280x704, fitted inside the window and centred (mq32: at 2560x1080 the bar spans 682..1882, i.e.
     * 1080 / 704 = 1.534 per unit); below, the 720p measurements minus the 8 px band of 1280x720 */
    const float sc = std::min(W / 1280.0f, H / 704.0f), ox = (W - 1280.0f * sc) * 0.5f, oy = (H - 704.0f * sc) * 0.5f - 8.0f * sc;
    const char* label = tod_menu_text(143);
    const GfFont& F = s_gf[1];
    const float px = 0.51f * (float)F.lh * sc, k = px / (float)F.lh;
    const float ink_top = 14.0f * k;                       /* the face's ink starts 14 units below the glyph box (font_render.py) */
    /* the game's items move 60 scene units (= 61 px at 720p) left when the wrapper installs (mq34): OPZIONI ends at 971, the
     * same 46 px gap, the bar 189..1089 is centred */
    /* ultrawide (TOD_ASPECT, src/stubs/ui_xform.cpp): the game's HUD is narrowed by g_tod_ui_k around the middle; measured
     * (uw21 / uw32, 09/11) the bar lands at centre + (16:9 position - centre) * k * 1251.6/1280 (the UI's own width), so ESCI
     * follows it: position and letter widths */
    /* 09/11 night: since the menus' 2D camera keeps 16:9 (aspect_hor.cpp) the bar is narrowed once, by the renderer's UI viewport,
     * so in a frame wider than 16:9 it is the 16:9 picture at H/720 per unit, centred (cg21/cg32: NUOVO GIOCO starts at 603 =
     * 320 + 189 * 1.5, OPZIONI ends at 1777); sc above is H/704 there (the picture fit), so x distances and letter widths
     * take (H/720)/sc. Heights already matched (rows 965..989 like OPZIONI). */
    /* 10/11 (u_pb21): only with TOD_ASPECT. Without it a wide window shows the 16:9 picture fitted with bars at the sides, the
     * game's bar at H/704 per unit like ESCI's sc, and H/720 put ESCI 9 px left at 2560x1080 */
    const float hx = (tod_aspect_frame() > 0.0f && W / H > 16.0f / 9.0f + 0.001f) ? (H / 720.0f) / sc : 1.0f;
    const float cxw = W * 0.5f;
    /* 10/11: after the game's last item (1032.3 unshifted) + the shift + the language's gap (tod_settings_menu.cpp) */
    /* -2: QUIT's first glyph box starts 2 px before its ink (bl_*: the gap came out 3 px wider at 1080p in all 11 languages) */
    const float qx = 1032.3f + tod_fe_shift720() + tod_fe_gap720() - 2.0f;
    const float x0 = cxw + (ox + qx * sc - cxw) * hx, top = oy + 637.0f * sc - ink_top;
    const float axis = oy + 653.5f * sc;                       /* the mirror line, between the ink's bottom and the reflection */
    const ImU32 col = st == 2 ? IM_COL32(251, 247, 251, 255) : IM_COL32(116, 102, 81, 255);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    {   /* the letters (gf_text with the horizontal factor) */
        ImFontAtlas* at = ImGui::GetIO().Fonts; float xx = x0;
        for (const char* s = label; *s;) {
            uint32_t cp = gf_utf8(s);
            if (cp >= 0x180 || !F.g[cp].w) cp = '?';
            const GfGlyph& g = F.g[cp];
            ImFontAtlasRect rr;
            if (g.rect != ImFontAtlasRectId_Invalid && cp != ' ' && at->GetCustomRect(g.rect, &rr))
                dl->AddImage(at->TexRef, ImVec2(xx, top), ImVec2(xx + g.w * k * hx, top + g.h * k), rr.uv0, rr.uv1, col);
            xx += g.w * k * hx;
        }
    }
    /* the reflection: each glyph upside down about the axis, alpha from 19 % at the ink's bottom to 6 % at its top */
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    const ImU32 rgb = col & 0x00FFFFFFu;
    auto alpha_at = [&](float y) { const float t = (y - (axis + 1.5f * sc)) / (16.0f * sc); float a = 0.19f - 0.13f * t; if (a < 0) a = 0; if (a > 0.19f) a = 0.19f; return rgb | ((ImU32)(a * 255.0f) << 24); };
    dl->PushTexture(atlas->TexRef);
    float x = x0;
    for (const char* s = label; *s;) {
        uint32_t cp = gf_utf8(s);
        if (cp >= 0x180 || !F.g[cp].w) cp = '?';
        const GfGlyph& g = F.g[cp];
        ImFontAtlasRect rr;
        if (g.rect != ImFontAtlasRectId_Invalid && cp != ' ' && atlas->GetCustomRect(g.rect, &rr)) {
            const float yb = 2.0f * axis - (top + g.h * k), ye = 2.0f * axis - top;   /* box bottom mirrored to the top, and back */
            const float x1 = x + g.w * k * hx;
            const ImU32 ct = alpha_at(yb), cb = alpha_at(ye);
            dl->PrimReserve(6, 4);
            const ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
            dl->PrimWriteVtx(ImVec2(x, yb), ImVec2(rr.uv0.x, rr.uv1.y), ct);
            dl->PrimWriteVtx(ImVec2(x1, yb), ImVec2(rr.uv1.x, rr.uv1.y), ct);
            dl->PrimWriteVtx(ImVec2(x1, ye), ImVec2(rr.uv1.x, rr.uv0.y), cb);
            dl->PrimWriteVtx(ImVec2(x, ye), ImVec2(rr.uv0.x, rr.uv0.y), cb);
            dl->PrimWriteIdx(i0); dl->PrimWriteIdx((ImDrawIdx)(i0 + 1)); dl->PrimWriteIdx((ImDrawIdx)(i0 + 2));
            dl->PrimWriteIdx(i0); dl->PrimWriteIdx((ImDrawIdx)(i0 + 2)); dl->PrimWriteIdx((ImDrawIdx)(i0 + 3));
        }
        x += g.w * k * hx;
    }
    dl->PopTexture();
}

/* TOD_OVERLAY_SCRIPT (debug): "<sec>:<cmd>,..." run on the render thread, one per present, seconds since the first present. */
struct ScriptItem { double t; std::string cmd; };
std::vector<ScriptItem> s_script;
double s_script_t0;

void pick_pad(double t)
{
    const uint32_t b = s_pad.load();
    if (t - s_pick_t0 < 0.35) { s_pad_prev = b; return; }      /* the press that opened the dialog */
    const uint32_t down = b & ~s_pad_prev;
    struct { uint32_t bit; int act; bool rep; } map[] = {
        { CELL_PAD_CTRL_UP, A_UP, true }, { CELL_PAD_CTRL_DOWN, A_DOWN, true },
        { CELL_PAD_CTRL_CROSS, A_ACCEPT, false }, { CELL_PAD_CTRL_CIRCLE, A_BACK, false },
    };
    for (auto& m : map) {
        if (down & m.bit) {
            pick_act(m.act);
            if (m.rep) s_pad_next_rep = t + 0.40;
        } else if (m.rep && (b & m.bit) && t >= s_pad_next_rep && !(down & 0xFFFF)) {
            pick_act(m.act);
            s_pad_next_rep = t + 0.09;
        }
    }
    s_pad_prev = b;
}

/* TOD_SAVE_PICKER_SCRIPT="down,down,act,back,...": test hook, one command every 0.8 s starting 2 s after the picker opens. */
void pick_script(double t)
{
    static int idx = 0;
    static double last_t0 = -1;
    if (last_t0 != s_pick_t0) { last_t0 = s_pick_t0; idx = 0; }
    const char* e = getenv("TOD_SAVE_PICKER_SCRIPT");
    if (!e || !*e) return;
    std::string all = e; std::vector<std::string> cmds; size_t pos = 0;
    while (pos <= all.size()) { size_t c = all.find(',', pos); if (c == std::string::npos) c = all.size(); cmds.push_back(all.substr(pos, c - pos)); pos = c + 1; }
    if (idx >= (int)cmds.size() || t - s_pick_t0 < 2.0 + 0.8 * idx) return;
    const std::string& c = cmds[idx++];
    fprintf(stderr, "[savepick] script: %s\n", c.c_str());
    if (c == "down") pick_act(A_DOWN); else if (c == "up") pick_act(A_UP);
    else if (c == "act") pick_act(A_ACCEPT); else if (c == "back") pick_act(A_BACK);
}

/* alert10: the YES / NO box drawn like the weapon vendor's "Sei sicuro di voler comprare:" (log\quit125\conferma_negozio_nativa.png):
 * the screen darkened, a navy panel with cut corners, a light blue frame, orange marks on the sides, grey question, white
 * second line, YES / NO stacked in the middle, the chosen one on an orange bar that fades out at both ends. Sizes follow the
 * window height, so it looks the same at any resolution and stays centred at 21:9 / 32:9. */
/* the vendor box's panel (measurements in build_alert): navy fill, cut corners, side bevels, orange marks */
void game_panel(ImDrawList* dl, float x0, float y0, float x1, float y1, float u)
{
    const float c = 27.0f * u, ph = y1 - y0;
    const ImVec2 poly[8] = { ImVec2(x0 + c, y0), ImVec2(x1 - c, y0), ImVec2(x1, y0 + c), ImVec2(x1, y1 - c),
                             ImVec2(x1 - c, y1), ImVec2(x0 + c, y1), ImVec2(x0, y1 - c), ImVec2(x0, y0 + c) };
    dl->AddConvexPolyFilled(poly, 8, IM_COL32(7, 12, 32, 250));
    dl->AddPolyline(poly, 8, IM_COL32(48, 61, 75, 255), ImDrawFlags_Closed, 2.0f * u);
    for (int s = 0; s < 2; s++) {                    /* the side bevels: light outer edge, darker body, bright inner edge */
        const float a = s ? x1 - 9.0f * u : x0, b = a + 9.0f * u, ya = y0 + c, yb = y1 - c;
        dl->AddRectFilled(ImVec2(a, ya), ImVec2(b, yb), IM_COL32(35, 58, 94, 255));
        const float ox = s ? b - 2.0f * u : a, ix = s ? a : b - 2.0f * u;
        dl->AddRectFilled(ImVec2(ox, ya), ImVec2(ox + 2.0f * u, yb), IM_COL32(56, 81, 112, 255));
        dl->AddRectFilled(ImVec2(ix, ya), ImVec2(ix + 2.0f * u, yb), IM_COL32(98, 115, 131, 255));
        const float mx = s ? x1 + 1.0f * u : x0 - 4.0f * u;   /* the orange mark, 2 px, a 1 px gap from the frame */
        const float mh = std::min(48.0f * u, ph * 0.25f);
        dl->AddRectFilled(ImVec2(mx, y0 + ph * 0.5f - mh), ImVec2(mx + 2.5f * u, y0 + ph * 0.5f + mh), IM_COL32(176, 111, 36, 255));
    }
}

/* the vendor's selection: an orange bar ry..ry+rh, bh either side of cx, plateau pl, edges fading in 10 px */
void game_selbar(ImDrawList* dl, float cx, float ry, float rh, float bh, float pl, float u)
{
    /* 3 bands (edge fade, plateau, edge fade) x 3 columns (fade in, plateau, fade out) */
    const ImVec4 E(198, 120, 22, 255), M(73, 50, 28, 255);
    const float ys[4] = { ry, ry + 10.0f * u, ry + rh - 10.0f * u, ry + rh };
    const ImVec4 top[3] = { E, M, M }, bot[3] = { M, M, E };
    const float xs[4] = { cx - bh, cx - pl, cx + pl, cx + bh };
    const float al[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
    for (int v = 0; v < 3; v++)
        for (int h = 0; h < 3; h++) {
            auto col = [&](const ImVec4& cc, float aa) { return IM_COL32((int)cc.x, (int)cc.y, (int)cc.z, (int)(255 * aa)); };
            dl->AddRectFilledMultiColor(ImVec2(xs[h], ys[v]), ImVec2(xs[h + 1], ys[v + 1]),
                                        col(top[v], al[h]), col(top[v], al[h + 1]), col(bot[v], al[h + 1]), col(bot[v], al[h]));
        }
}

void build_alert(float W, float H)
{
    /* alert10: measured on the weapon vendor's box at 1280x720 (log\quit125\conferma_negozio_nativa.png, cf16_t155.png):
     * panel 650x240 (x 315..965, y 176..415), corners cut 27 px, sides a 9 px bevel, top / bottom a 2 px line, orange marks
     * 2 px wide outside the sides (y 248..343), fill (7,12,32), question (91,111,126) at 40 px, second line (179,188,208)
     * at 33 px, YES / NO rows 36 px tall 40 px apart, the chosen one on an orange bar 370 px wide (plateau 180) with bright
     * edges (198,120,22) fading to (73,50,28) in 10 px; chosen text (134,121,97), the other (91,111,126). The game's own
     * font (gf_*), the Windows one when the game files are not there. */
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = H / 720.0f;
    const bool gf = gf_ok(0);
    ImFont* wf = s_tag_font ? s_tag_font : ImGui::GetFont();
    auto tw = [&](float px, const char* t) { return gf ? gf_width(0, px, t) : wf->CalcTextSizeA(px * 0.8f, FLT_MAX, 0.0f, t).x; };
    auto tx = [&](float px, ImVec2 p, ImU32 c, const char* t) { if (gf) gf_text(dl, 0, px, p, c, t); else dl->AddText(wf, px * 0.8f, ImVec2(p.x, p.y + px * 0.1f), c, t); };
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 4, 12, 170));
    const float fq = 40.0f * u, fs = 33.0f * u, fo = 40.0f * u;
    const bool sub = !s_alert_sub.empty();
    const float need = std::max(tw(fq, s_pick_title.c_str()), sub ? tw(fs, s_alert_sub.c_str()) : 0.0f) + 90.0f * u;
    const float pw = std::min(W - 32.0f * u, std::max(650.0f * u, need)), ph = 256.0f * u;   /* 16 px more than the vendor box: the same space under NO as above the question */
    /* 09/11 night: centred on the screen (the user: it sat high; it was at the vendor box's place, 64 px above the middle) */
    const float cx = W * 0.5f, y0 = H * 0.5f - ph * 0.5f;
    game_panel(dl, cx - pw * 0.5f, y0, cx + pw * 0.5f, y0 + ph, u);
    /* without a second line the question sits in the middle of the space above YES (mq30: a hole under it otherwise) */
    tx(fq, ImVec2(cx - tw(fq, s_pick_title.c_str()) * 0.5f, y0 + (sub ? 16.0f : 58.0f) * u), IM_COL32(91, 111, 126, 255), s_pick_title.c_str());
    if (sub) tx(fs, ImVec2(cx - tw(fs, s_alert_sub.c_str()) * 0.5f, y0 + 95.0f * u), IM_COL32(179, 188, 208, 255), s_alert_sub.c_str());
    const int n = (int)s_pick_name.size();
    const float rh = 36.0f * u, step = 40.0f * u, bh = 185.0f * u, pl = 90.0f * u;
    ImGuiIO& io = ImGui::GetIO();
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    for (int i = 0; i < n; i++) {
        const float ry = y0 + 162.0f * u + step * (float)i;
        const bool hov = io.MousePos.x >= cx - bh && io.MousePos.x < cx + bh && io.MousePos.y >= ry && io.MousePos.y < ry + step;
        if (hov && moved) s_pick_sel = i;
        if (hov && ImGui::IsMouseClicked(0)) { s_pick_sel = i; pick_finish(i); }
        if (i == s_pick_sel) game_selbar(dl, cx, ry, rh, bh, pl, u);
        const char* t = s_pick_name[i].c_str();
        tx(fo, ImVec2(cx - tw(fo, t) * 0.5f, ry + (rh - fo) * 0.5f), i == s_pick_sel ? IM_COL32(134, 121, 97, 255) : IM_COL32(91, 111, 126, 255), t);
    }
}

/* the save / load list in the game's own look (the vendor box, sp3): title on top, one row per save (its number, the
 * planet, the date under it), the chosen row on the orange bar, at most 6 rows with orange arrows when more are hidden. */
int s_pick_top;                                  /* the first row shown */

/* sp5: each save's ICON0.PNG (320x176, the planet picture the PS3's own save list shows), halved into the ImGui atlas the
 * first time a row shows it; cached by path + size + time (a save made since then gets its new picture). */
struct PickIcon { std::string key; ImFontAtlasRectId rect = ImFontAtlasRectId_Invalid; };
std::vector<PickIcon> s_icons;
ImFontAtlasRectId pick_icon(const std::string& dir)
{
    std::string path = std::string(cellSaveData_root()) + "/" + dir + "/ICON0.PNG";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return ImFontAtlasRectId_Invalid;
    char k[64]; snprintf(k, sizeof k, "|%lu|%lu", fa.nFileSizeLow, fa.ftLastWriteTime.dwLowDateTime);
    const std::string key = path + k;
    for (const PickIcon& p : s_icons) if (p.key == key) return p.rect;
    PickIcon pi; pi.key = key;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    if (s_icons.size() < 96 && atlas->TexData && atlas->TexData->BytesPerPixel == 4) {
        std::vector<unsigned char> f;
        if (FILE* fp = fopen(path.c_str(), "rb")) { fseek(fp, 0, SEEK_END); const long n = ftell(fp); fseek(fp, 0, SEEK_SET);
            if (n > 0 && n < (8 << 20)) { f.resize((size_t)n); if (fread(f.data(), 1, (size_t)n, fp) != (size_t)n) f.clear(); } fclose(fp); }
        int w = 0, h = 0, c = 0;
        unsigned char* img = f.empty() ? NULL : stbi_load_from_memory(f.data(), (int)f.size(), &w, &h, &c, 4);
        if (img && w >= 2 && h >= 2) {
            const int ow = w / 2, oh = h / 2;
            ImFontAtlasRect rr;
            pi.rect = atlas->AddCustomRect(ow, oh, &rr);
            if (pi.rect != ImFontAtlasRectId_Invalid) {
                ImTextureData* tex = atlas->TexData;
                for (int y = 0; y < oh; y++) {
                    unsigned char* d = (unsigned char*)tex->GetPixelsAt(rr.x, rr.y + y);
                    for (int x = 0; x < ow; x++)
                        for (int ch = 0; ch < 4; ch++) {
                            const unsigned char* s = img + ((size_t)(2 * y) * w + 2 * x) * 4 + ch;
                            d[x * 4 + ch] = (unsigned char)((s[0] + s[4] + s[(size_t)w * 4] + s[(size_t)w * 4 + 4] + 2) / 4);
                        }
                }
                ImFontAtlasTextureBlockQueueUpload(atlas, tex, rr.x, rr.y, ow, oh);
            }
        }
        if (img) stbi_image_free(img);
    }
    fprintf(stderr, "[savepick] icon %s: %s\n", dir.c_str(), pi.rect != ImFontAtlasRectId_Invalid ? "loaded" : "none");
    s_icons.push_back(pi);
    return pi.rect;
}
void build_picker_game(float W, float H)
{
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float u = H / 720.0f;
    const bool gf = gf_ok(0);
    ImFont* wf = s_tag_font ? s_tag_font : ImGui::GetFont();
    auto tw = [&](float px, const char* t) { return gf ? gf_width(0, px, t) : wf->CalcTextSizeA(px * 0.8f, FLT_MAX, 0.0f, t).x; };
    auto tx = [&](float px, ImVec2 p, ImU32 c, const char* t) { if (gf) gf_text(dl, 0, px, p, c, t); else dl->AddText(wf, px * 0.8f, ImVec2(p.x, p.y + px * 0.1f), c, t); };
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 4, 12, 170));
    /* sp8 (the user's choice): the thumbnails (A), at most 4 rows, tall enough for a big planet picture */
    const int n = (int)s_pick_name.size(), vis = std::min(n, 4);
    if (s_pick_sel < s_pick_top) s_pick_top = s_pick_sel;
    if (s_pick_sel >= s_pick_top + vis) s_pick_top = s_pick_sel - vis + 1;
    s_pick_top = std::max(0, std::min(s_pick_top, n - vis));
    const float ft = 40.0f * u, f1 = 33.0f * u, f2 = 24.0f * u;
    const float rh = 76.0f * u, step = 82.0f * u, head = 86.0f * u, foot = 30.0f * u;
    const float pw = std::min(W - 32.0f * u, 800.0f * u), ph = head + step * (float)vis + foot;
    const float cx = W * 0.5f, x0 = cx - pw * 0.5f, y0 = H * 0.5f - ph * 0.5f - 10.0f * u;
    game_panel(dl, x0, y0, x0 + pw, y0 + ph, u);
    const char* title = !s_pick_title.empty() ? s_pick_title.c_str() : tod_menu_text(s_pick_save ? 155 : 154);
    tx(ft, ImVec2(cx - tw(ft, title) * 0.5f, y0 + 18.0f * u), IM_COL32(91, 111, 126, 255), title);
    const float bh = pw * 0.5f - 40.0f * u, pl = bh - 110.0f * u;
    ImGuiIO& io = ImGui::GetIO();
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    if (io.MouseWheel != 0.0f && n > vis) s_pick_top = std::max(0, std::min(n - vis, s_pick_top - (io.MouseWheel > 0 ? 1 : -1)));
    int num = 0;                                    /* saves are numbered, the new slot is not */
    for (int i = 0; i < s_pick_top; i++) if (i != s_pick_new) num++;
    const float ih = 68.0f * u, iw = ih * 320.0f / 176.0f, ix = x0 + 84.0f * u;   /* the planet picture (ICON0.PNG) */
    const float lx = x0 + 56.0f * u, tx0 = ix + iw + 20.0f * u;
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    for (int r = 0; r < vis; r++) {
        const int i = s_pick_top + r;
        const float ry = y0 + head + step * (float)r;
        const bool hov = io.MousePos.x >= cx - bh && io.MousePos.x < cx + bh && io.MousePos.y >= ry && io.MousePos.y < ry + step;
        if (hov && moved) s_pick_sel = i;
        if (hov && ImGui::IsMouseClicked(0)) { s_pick_sel = i; pick_finish(i); }
        const bool sel = i == s_pick_sel;
        if (sel) game_selbar(dl, cx, ry, rh, bh, pl, u);
        const ImU32 c1 = sel ? IM_COL32(134, 121, 97, 255) : IM_COL32(91, 111, 126, 255);
        const ImU32 c2 = sel ? IM_COL32(179, 188, 208, 255) : IM_COL32(70, 86, 100, 255);
        std::string l1, l2;
        if (i == s_pick_new) {
            l1 = tod_menu_text(156);
            /* 09/11 night (the user: without a picture beside it the row looked like an error): an empty picture box with a "+" */
            const ImVec2 a(ix, ry + (rh - ih) * 0.5f), b(ix + iw, ry + (rh + ih) * 0.5f);
            dl->AddRectFilled(a, b, IM_COL32(10, 16, 28, 200));
            dl->AddRect(a, b, sel ? IM_COL32(198, 120, 22, 255) : IM_COL32(48, 61, 75, 255), 0.0f, 0, 1.5f * u);
            const float pcx = (a.x + b.x) * 0.5f, pcy = (a.y + b.y) * 0.5f, ps = ih * 0.22f, pt = 3.0f * u;
            const ImU32 pc = sel ? IM_COL32(198, 120, 22, 255) : IM_COL32(70, 86, 100, 255);
            dl->AddRectFilled(ImVec2(pcx - ps, pcy - pt * 0.5f), ImVec2(pcx + ps, pcy + pt * 0.5f), pc);
            dl->AddRectFilled(ImVec2(pcx - pt * 0.5f, pcy - ps), ImVec2(pcx + pt * 0.5f, pcy + ps), pc);
        } else {
            num++;
            const std::string& in = s_pick_info[i];
            const size_t d = in.find("   -   ");
            if (d != std::string::npos) { l2 = in.substr(0, d); l1 = in.substr(d + 7); }
            else { l1 = in.empty() ? s_pick_name[i] : in; }
            int yy, mo, dd, hh, mi;   /* "2026-11-03  22:41" -> "03/11/2026  22:41" (a European release: day first) */
            if (sscanf(l2.c_str(), "%d-%d-%d %d:%d", &yy, &mo, &dd, &hh, &mi) == 5) { char b[40]; snprintf(b, sizeof b, "%02d/%02d/%04d   %02d:%02d", dd, mo, yy, hh, mi); l2 = b; }
            char nb[8]; snprintf(nb, sizeof nb, "%d", num);
            tx(f1 * 1.15f, ImVec2(lx - tw(f1 * 1.15f, nb) * 0.5f, ry + (rh - f1 * 1.15f) * 0.5f), c1, nb);
            const ImFontAtlasRectId ic = pick_icon(s_pick_name[i]);
            ImFontAtlasRect rr;
            const ImVec2 a(ix, ry + (rh - ih) * 0.5f), b(ix + iw, ry + (rh + ih) * 0.5f);
            if (ic != ImFontAtlasRectId_Invalid && atlas->GetCustomRect(ic, &rr)) {
                dl->AddImage(atlas->TexRef, a, b, rr.uv0, rr.uv1, sel ? IM_COL32_WHITE : IM_COL32(200, 200, 200, 255));
                dl->AddRect(a, b, sel ? IM_COL32(198, 120, 22, 255) : IM_COL32(48, 61, 75, 255), 0.0f, 0, 1.5f * u);
            } else dl->AddRect(a, b, IM_COL32(48, 61, 75, 255), 0.0f, 0, 1.5f * u);
        }
        if (l2.empty()) tx(f1, ImVec2(tx0, ry + (rh - f1) * 0.5f), c1, l1.c_str());
        else { const float ty = ry + (rh - (f1 * 0.92f + f2 + 1.0f * u)) * 0.5f;   /* the two lines centred in the row */
               tx(f1, ImVec2(tx0, ty), c1, l1.c_str()); tx(f2, ImVec2(tx0, ty + 1.0f * u + f1 * 0.92f), c2, l2.c_str()); }
    }
    const ImU32 oc = IM_COL32(176, 111, 36, 255);   /* more rows above / below: small orange arrows on the right */
    const float ax = x0 + pw - 60.0f * u, aw = 9.0f * u;
    if (s_pick_top > 0) { const float ay = y0 + head - 6.0f * u; dl->AddTriangleFilled(ImVec2(ax - aw, ay), ImVec2(ax + aw, ay), ImVec2(ax, ay - aw), oc); }
    if (s_pick_top + vis < n) { const float ay = y0 + head + step * (float)vis + 4.0f * u; dl->AddTriangleFilled(ImVec2(ax - aw, ay), ImVec2(ax + aw, ay), ImVec2(ax, ay + aw), oc); }
}

void build_picker(float W, float H, float k)
{
    if (s_pick_alert) { build_alert(W, H); return; }
    { const char* e = getenv("TOD_SAVE_PICKER_OLD"); if (!(e && e[0] == '1')) { build_picker_game(W, H); return; } }
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), IM_COL32(0, 0, 0, 170));
    const float pw = std::min(W - 40.0f * k, 760.0f * k);
    const int n = (int)s_pick_name.size();
    const float rowh = 62.0f * k;
    const float ph = std::min(H - 40.0f * k, 150.0f * k + rowh * (float)std::min(n, 8));
    ImGui::SetNextWindowPos(ImVec2(W * 0.5f, H * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##tod_savepick", NULL,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.62f, 0.16f, 1));
    ImGui::TextUnformatted(!s_pick_title.empty() ? s_pick_title.c_str() : s_pick_save ? "SAVE GAME - choose a slot" : "LOAD GAME - choose a save");
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::BeginChild("##pick_list", ImVec2(0, ph - 110.0f * k), false, ImGuiWindowFlags_NoNav);
    ImGuiIO& io = ImGui::GetIO();
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool sel = i == s_pick_sel;
        if (ImGui::Selectable("##row", sel, 0, ImVec2(0, rowh - 6.0f * k))) { s_pick_sel = i; pick_finish(i); }
        if (ImGui::IsItemHovered() && moved) s_pick_sel = i;
        if (sel && s_pick_scroll) ImGui::SetScrollHereY(0.5f);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const bool isnew = i == s_pick_new;
        dl->AddText(ImVec2(p.x + 10.0f * k, p.y + 4.0f * k), isnew ? IM_COL32(140, 230, 140, 255) : IM_COL32(255, 255, 255, 255), s_pick_name[i].c_str());
        dl->AddText(ImVec2(p.x + 10.0f * k, p.y + 4.0f * k + ImGui::GetTextLineHeight() + 2.0f * k), IM_COL32(170, 170, 170, 255), s_pick_info[i].c_str());
        ImGui::PopID();
    }
    s_pick_scroll = false;
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::TextDisabled("Up/Down: choose     Cross / Enter / click: confirm     Circle / Esc: cancel");
    ImGui::End();
}

void script_step()
{
    if (s_script.empty()) return;
    const double t = now_s();
    if (!s_script_t0) s_script_t0 = t;
    if (t - s_script_t0 < s_script.front().t) return;
    const std::string c = s_script.front().cmd;
    s_script.erase(s_script.begin());
    fprintf(stderr, "[overlay] script: %s\n", c.c_str());
    if (!c.compare(0, 3, "tp=")) {   /* usr123: drive the teleport menu from TOD_OVERLAY_SCRIPT: tp=open | tp=close | tp=warp:<volume> (current level) | tp=go:<level>:<volume or -> */
        const std::string a = c.substr(3);
        if (a == "open") tp_set_open(true);
        else if (a == "close") tp_set_open(false);
        else if (!a.compare(0, 5, "warp:")) { char ch[160]; snprintf(ch, sizeof ch, "hero_warp(Ratchet1,%s)", a.c_str() + 5); tod_lua_enqueue(ch); tod_tp_log("[teleport] warp %s\n", a.c_str() + 5); }
        else if (!a.compare(0, 4, "goin")) { const size_t k1 = a.find(':'); const size_t k2 = k1 == std::string::npos ? k1 : a.find(':', k1 + 1); const std::string lv = k1 == std::string::npos ? "" : a.substr(k1 + 1, k2 == std::string::npos ? std::string::npos : k2 - k1 - 1); const std::string sp = k2 == std::string::npos ? "" : a.substr(k2 + 1); if (!lv.empty()) tp_go(lv.c_str(), sp == "-" ? "" : sp.c_str()); }   /* vram124: tp=goin:<level>:<volume or -> in-process, keeps the inventory (TOD_TELEPORT_RELAUNCH=1 relaunches) */
        else if (!a.compare(0, 3, "go:")) { const size_t k2 = a.find(':', 3); const std::string lv = a.substr(3, k2 == std::string::npos ? std::string::npos : k2 - 3); const std::string sp = k2 == std::string::npos ? "" : a.substr(k2 + 1); tp_relaunch(lv.c_str(), sp == "-" ? "" : sp.c_str()); }
        return;
    }
    if (c == "open") { if (!s_pick_on.load()) set_open(true); return; }
    if (!s_open.load()) { fprintf(stderr, "[overlay] script: page not open, '%s' ignored\n", c.c_str()); return; }
    if (c == "close") set_open(false);
    else if (!c.compare(0, 4, "tab=")) { s_tab = std::max(0, std::min(NTABS - 1, atoi(c.c_str() + 4))); s_sel = 0; }
    else if (!c.compare(0, 4, "sel=")) s_sel = atoi(c.c_str() + 4);
    else if (c == "act") do_act(A_ACCEPT);
    else if (c == "left") do_act(A_LEFT);
    else if (c == "right") do_act(A_RIGHT);
    else if (c == "apply") do_act(A_APPLY);
    else if (c == "clear") do_act(A_CLEAR);
    else if (c == "down") do_act(A_DOWN);
    else if (c == "up") do_act(A_UP);
    else if (!c.compare(0, 4, "key=")) {      /* the key a Controls row is waiting for (same path as a window message's) */
        bool ok = false;
        const int vk = kb::parse_key(c.c_str() + 4, &ok);
        if (ok && vk) controls_capture_event(controls_code_for_vk(vk));
        else fprintf(stderr, "[overlay] script: unknown key '%s'\n", c.c_str() + 4);
    }
}

void overlay_render(void* devp, void* queuep, void* listp, uint64_t rtv, unsigned w, unsigned h, int frames)
{
    script_step();
    /* the auto-open of an unattended run */
    if (s_auto_at >= 0 && !s_open.load() && !s_pick_on.load()) {
        const double t = now_s();
        if (!s_t_first) s_t_first = t;
        if (t - s_t_first >= s_auto_at) {
            s_auto_at = -1;
            if (s_auto_tab >= 0) s_tab = s_auto_tab;
            set_open(true);
        }
    }
    const int cm = s_fps_mode;
    if (cm) fps_note(now_s());
    const bool un = update_notice_on(now_s());
    const bool vt = version_tag_on();
    const bool pk = s_pick_on.load();
    int mq_n; float mq_x, mq_y;
    const bool mq = tod_fe_quit_state(&mq_n, &mq_x, &mq_y) != 0;   /* alert10: QUIT on the main menu bar */
    const bool ph = tod_pause_hint_state() != 0;                  /* 10/11: hints under the pause lists (#25) */
    const bool cl = combat_log_on();                              /* 10/11: TOD_COMBAT_LOG=2 */
    if (!s_open.load() && !cm && !un && !vt && !pk && !s_tp_open.load() && !mq && !ph && !cl) return;
    if (!ensure_ready(devp, queuep, frames, w, h)) return;
    ImGuiIO& io = ImGui::GetIO();
    const float k = std::max(0.75f, std::min(3.0f, (float)h / 1080.0f));
    if (k != s_scale_cur) { style_setup(k); s_scale_cur = k; }
    io.DisplaySize = ImVec2((float)w, (float)h);
    if (s_open.load()) init_rows();
    if (s_open.load() && s_want_toggle.exchange(false)) {          /* just opened */
        if (tod_update_state() == 2) s_upd_seen = true;   /* r121s: the notice has done its job */
        refresh_rows();
        controls_refresh();   /* keys136 */
        s_sel = 0;
        EnterCriticalSection(&s_cs); s_ev.clear(); LeaveCriticalSection(&s_cs);
        s_pad_wait_release = (s_pad.load() & CELL_PAD_CTRL_SELECT) != 0;
        s_pad_prev = s_pad.load();
        s_status.clear();
        POINT pt; HWND fw = GetForegroundWindow();
        if (fw && GetCursorPos(&pt) && ScreenToClient(fw, &pt)) io.AddMousePosEvent((float)pt.x, (float)pt.y);
    }
    std::vector<Ev> evs;
    EnterCriticalSection(&s_cs); evs.swap(s_ev); LeaveCriticalSection(&s_cs);
    for (const Ev& e : evs) {
        switch (e.kind) {
        case 0: do_act((int)e.a); break;
        case 1: io.AddMousePosEvent(e.a, e.b); break;
        case 2: io.AddMouseButtonEvent((int)e.a, e.b != 0.0f); break;
        case 3: io.AddMouseWheelEvent(0, e.a); break;
        case 4: controls_capture_event((int)e.a); break;
        }
    }
    if (s_pick_on.load()) { pick_pad(now_s()); pick_script(now_s()); }
    else if (s_open.load()) pad_actions(now_s());
    const bool open = s_open.load() && !s_pick_on.load();     /* may have been closed by an action above */
    const bool pkd = s_pick_on.load();
    const bool tpo = s_tp_open.load() && !open && !pkd;
    if (!open && !cm && !un && !vt && !pkd && !tpo && !mq && !ph && !cl) return;

    ID3D12GraphicsCommandList* list = (ID3D12GraphicsCommandList*)listp;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (open) build_page((float)w, (float)h, k);
    if (tpo) build_teleport((float)w, (float)h, k);
    if (mq && !open) main_quit_draw((float)w, (float)h);   /* first: the picker's dimming covers it like the game's items (sp1) */
    if (ph && !open && !pkd) pause_hint_draw((float)w, (float)h);
    if (cl && !open && !pkd) combat_log_draw((float)w, (float)h);
    if (pkd) { build_picker((float)w, (float)h, k); s_pick_drawn.store(GetTickCount64()); }
    if (cm) fps_draw((float)w, k);
    if (un && !open && !pkd) update_notice_draw((float)h, k);
    if (vt && !open && !pkd) version_tag_draw((float)w, (float)h, k);
    ImGui::Render();
    D3D12_CPU_DESCRIPTOR_HANDLE rt; rt.ptr = (SIZE_T)rtv;
    list->OMSetRenderTargets(1, &rt, FALSE, NULL);
    list->SetDescriptorHeaps(1, &s_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}

}  // namespace

/* 10/11: QUIT's width at 720p in the bar's font (face 1 at 0.51 of its line height), 0 when the game font is not loaded */
extern "C" float tod_esci_w720(void)
{
    if (!gf_ok(1)) return 0.0f;
    return gf_width(1, 0.51f * (float)s_gf[1].lh, tod_menu_text(143));
}

/* ---- entry points ---------------------------------------------------------- */

/* The pad poll (host_input.cpp), once per poll: the real pad's buttons. Opens the page
 * after SELECT is held for 2 s; the buttons feed the page while it is open. */
extern "C" void tod_overlay_pad_feed(uint32_t real_buttons)
{
    if (!s_cs_init) return;
    s_pad.store(real_buttons);
    static ULONGLONG since;
    static bool fired;
    if (s_pick_hold && !s_pick_on.load()) {      /* the picker just closed: the game stays blind until the buttons are up */
        if ((real_buttons & 0xFFFF) == 0 || GetTickCount64() - s_pick_hold_t0 > 1500) {
            s_pick_hold = false;
            g_cellPad_input_block = s_open.load() ? 1 : 0;
        }
    }
    if (s_pick_on.load()) { since = 0; fired = false; return; }
    if (s_open.load()) { since = 0; fired = false; return; }
    if (!(real_buttons & CELL_PAD_CTRL_SELECT)) { since = 0; fired = false; return; }
    const ULONGLONG now = GetTickCount64();
    if (!since) since = now;
    if (!fired && now - since >= 2000) {
        fired = true;
        fprintf(stderr, "[overlay] SELECT held 2 s\n");
        set_open(true);
    }
}

/* quit125: QUIT GAME picked in the pause menu (src/ui/tod_settings_menu.cpp, bridge op 9, on the guest thread that runs the Lua
 * script). The save-slot picker is the confirmation box: it is modal, takes the pad (cross / circle), the keyboard and the mouse,
 * and keeps the game's frame going while the guest thread waits in it. 0 = back to the main menu (tod.exe starts again on the
 * title screen: the game has no way back to it from a level), 1 = exit to Windows, 2 / circle = cancel (the pause menu stays).
 * TOD_QUIT_SCRIPT="down,act" drives it in tests (same commands as TOD_SAVE_PICKER_SCRIPT). */
extern "C" const char* tod_menu_text(int id);
static std::string s_start_argv;           /* PS3_ARGV at start, for the restart */
static void quit_to_title()
{
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    std::string in = s_start_argv, out, tok;      /* drop -level <name> and -no_frontend: the title screen comes up */
    std::vector<std::string> t; size_t pos = 0;
    while (pos <= in.size()) { size_t c = in.find(';', pos); if (c == std::string::npos) c = in.size(); t.push_back(in.substr(pos, c - pos)); pos = c + 1; }
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i].empty() || t[i] == "-no_frontend") continue;
        if (t[i] == "-level" || t[i] == "-capfps") { i++; continue; }   /* -capfps <n>: main.cpp adds it again at start */
        out += (out.empty() ? "" : ";") + t[i];
    }
    if (out.find("-no_legal") == std::string::npos) out = out.empty() ? "-no_legal" : "-no_legal;" + out;
    _putenv_s("PS3_ARGV", out.c_str());
    _putenv_s("TOD_LUA_EXEC", ""); _putenv_s("TOD_WARP", ""); _putenv_s("TOD_OVERLAY_SCRIPT", ""); _putenv_s("TOD_OVERLAY_AUTO", ""); _putenv_s("TOD_QUIT_SCRIPT", "");
    STARTUPINFOA si = {}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi = {};
    std::string cl = GetCommandLineA();
    if (!CreateProcessA(exe, &cl[0], NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) { fprintf(stderr, "[quit] restart failed (%lu)\n", GetLastError()); return; }
    fprintf(stderr, "[quit] back to the main menu: restarted as pid %lu (PS3_ARGV=%s)\n", pi.dwProcessId, out.c_str());
    { char pf[MAX_PATH]; snprintf(pf, sizeof pf, "%s", exe); if (char* sl = strrchr(pf, 92)) strcpy(sl + 1, "teleport_pid.txt"); else strcpy(pf, "teleport_pid.txt");
      if (FILE* f = fopen(pf, "w")) { fprintf(f, "%lu\n", pi.dwProcessId); fclose(f); } }
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 0);
}
/* alert10: the game-style YES / NO box (build_alert). Blocks the calling thread like the slot picker (call it from the guest
 * thread that runs the menu script, or any thread while the game keeps presenting). title / sub are UTF-8. Returns 1 YES,
 * 0 NO or circle / Esc, -2 when it could not be shown. TOD_QUIT_SCRIPT="down,act" drives it in tests. */
extern "C" int tod_alert(const char* title, const char* sub)
{
    const std::string y = tod_menu_text(152), no = tod_menu_text(153);
    const char* names[2] = { y.c_str(), no.c_str() };
    const char* infos[2] = { "", "" };
    s_pick_title = title ? title : ""; s_alert_sub = sub ? sub : ""; s_pick_alert = true;
    const char* qs = getenv("TOD_QUIT_SCRIPT");
    std::string saved; const char* sp = getenv("TOD_SAVE_PICKER_SCRIPT"); if (sp) saved = sp;
    _putenv_s("TOD_SAVE_PICKER_SCRIPT", qs ? qs : "");
    fprintf(stderr, "[alert] %s\n", s_pick_title.c_str());
    const int r = pick_hook(0, 2, names, infos, -1, 0);
    _putenv_s("TOD_SAVE_PICKER_SCRIPT", saved.c_str());
    s_pick_alert = false; s_pick_title.clear(); s_alert_sub.clear();
    fprintf(stderr, "[alert] answer %s\n", r == 0 ? "yes" : r == -2 ? "not shown" : "no");
    return r == 0 ? 1 : r == -2 ? -2 : 0;
}

/* alert10: leaving the Options page after changing a row that applies at the next start (bridge op 10). YES restarts on the
 * title screen (the settings are already in tod.ini); in a level the second line warns about the progress. */
extern "C" void tod_restart_open(void)
{
    const bool in_level = !tod_no_level_yet();
    fprintf(stderr, "[restart] rows that need a restart were changed (%s)\n", in_level ? "in a level" : "main menu");
    if (tod_alert(tod_menu_text(151), in_level ? tod_menu_text(148) : NULL) == 1) quit_to_title();
}

extern "C" void tod_quit_open(void)
{
    fprintf(stderr, "[quit] QUIT picked in the pause menu: back-to-title box\n");
    if (tod_alert(tod_menu_text(149), tod_menu_text(148)) == 1) quit_to_title();
}

/* alert10: QUIT on the main menu: close the game. */
extern "C" void tod_exit_open(void)
{
    fprintf(stderr, "[quit] QUIT picked on the main menu\n");
    if (tod_alert(tod_menu_text(144), NULL) == 1) { fprintf(stderr, "[quit] exit to Windows\n"); fflush(stderr); HWND w = s_game_hwnd; if (w) PostMessageA(w, WM_CLOSE, 0, 0); else ExitProcess(0); }
}

extern "C" int tod_overlay_open(void) { return (s_open.load() || s_pick_on.load()) ? 1 : 0; }
/* menu104: the Options page of the game's own menu (src/ui/tod_settings_menu.cpp) sets RSX_AA through this: the
 * same steps as apply_and_save() for a live row (environment, the renderer's globals, the display reload) and
 * tod.ini. mode: 0 off, 1 fxaa, 2 msaa2, 3 msaa4, 4 msaa8. Safe from any thread (no overlay state is touched;
 * the F1 page re-reads the environment each time it is opened). */
extern "C" void tod_menu_apply_key_win(const char* key, const char* v)
{
    _putenv_s(key, v ? v : "");               /* "" removes it */
    bool display = false;
    if (!strcmp(key, "RSX_AA")) {
        g_rsx_aa_pending = !v ? 0 : !strcmp(v, "fxaa") ? 1 : !strcmp(v, "msaa2") ? 2 : !strcmp(v, "msaa4") ? 4 : !strcmp(v, "msaa8") ? 8 : 0;
        display = true;
    }
    if (!strcmp(key, "RSX_DISPLAY") || !strcmp(key, "RSX_VSYNC") || !strcmp(key, "RSX_SCALE") || !strcmp(key, "RSX_WINDOW")) display = true;
    if (!strcmp(key, "TOD_FPS_COUNTER")) s_fps_mode = v ? atoi(v) : 0;
    if (!strcmp(key, "RSX_ANISO")) g_rsx_aniso = v ? atoi(v) : 16;
    if (!strcmp(key, "TOD_FPS")) tod_fps_apply_live(v);   /* usr123: live (the menu value wins over the start-up environment) */
    if (display) g_rsx_display_reload = 1;
    write_ini({ { key, v } });
    fprintf(stderr, "[menu] %s = %s applied%s\n", key, v ? v : "(default)", "");
}

#define TOD_WM_MENU_KEY (WM_APP + 0x108)
static BOOL CALLBACK menu_find_wnd(HWND h, LPARAM lp)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    char cls[64] = {};
    GetClassNameA(h, cls, sizeof cls);
    /* menu111: only the renderer's own window (another visible window of this process may come first in the Z order) */
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !strcmp(cls, "ps3recomp_d3d12_engine")) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}

/* The game thread (the Lua script) must NOT apply the change itself: 04/11 menu107, doing it there froze the renderer
 * for good (no frame presented after the call, run h2). The F1 page applies on the window thread; so does this: the
 * request is posted to the game window and overlay_message() runs tod_menu_apply_key_win there. key and v point to
 * static strings (the host's row table). */
extern "C" void tod_menu_set_key(const char* key, const char* v)
{
    HWND w = s_game_hwnd;
    if (!w) EnumWindows(menu_find_wnd, (LPARAM)&w);
    if (w && PostMessageA(w, TOD_WM_MENU_KEY, (WPARAM)key, (LPARAM)v)) {
        fprintf(stderr, "[menu] %s = %s request posted to the window thread\n", key, v ? v : "(default)");
        return;
    }
    fprintf(stderr, "[menu] no game window found: applying on the calling thread\n");
    tod_menu_apply_key_win(key, v);
}


extern "C" void tod_overlay_init(void)
{
    const char* off = getenv("TOD_OVERLAY");
    if (off && !strcmp(off, "0")) return;
    if (!s_cs_init) { InitializeCriticalSection(&s_cs); s_cs_init = true; }
    { const char* av = getenv("PS3_ARGV"); s_start_argv = av ? av : ""; }   /* quit125 */
    if (const char* c = getenv("TOD_FPS_COUNTER")) s_fps_mode = std::max(0, std::min(2, atoi(c)));
    if (const char* sc = getenv("TOD_OVERLAY_SCRIPT")) {
        std::string all = sc; size_t pos = 0;
        while (pos <= all.size()) {
            size_t e = all.find(',', pos); if (e == std::string::npos) e = all.size();
            const std::string one = all.substr(pos, e - pos); pos = e + 1;
            const size_t colon = one.find(':');
            if (colon != std::string::npos) s_script.push_back({ atof(one.c_str()), one.substr(colon + 1) });
        }
        std::stable_sort(s_script.begin(), s_script.end(), [](const ScriptItem& a, const ScriptItem& b) { return a.t < b.t; });
    }
    if (const char* a = getenv("TOD_OVERLAY_AUTO")) {
        s_auto_at = atof(a);
        if (const char* c = strchr(a, ':')) s_auto_tab = atoi(c + 1);
    }
    RsxD3D12Overlay o = {};
    o.message = overlay_message;
    o.render = overlay_render;
    rsx_d3d12_set_overlay(&o);
    { const char* sp = getenv("TOD_SAVE_PICKER"); if (!(sp && sp[0] == '0')) cellSaveData_set_pick_hook(pick_hook); }
    fprintf(stderr, "[overlay] settings page: F1 or SELECT held 2 s\n");
}
