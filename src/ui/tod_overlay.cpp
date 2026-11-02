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
#include <deque>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#include "rsx_d3d12_engine.h"
#include "settings.h"
#include "input/key_bindings.h"

extern "C" {
extern volatile int g_cellPad_input_block;     /* cellPad.c */
extern volatile int g_rsx_display_reload;      /* rsx_draw_engine.c: the renderer re-reads its display settings */
extern int g_rsx_aniso;                        /* rsx_d3d12_engine.c: the live anisotropy level */
extern int g_rsx_aa;                           /* rsx_draw_engine.c: 0 off, 1 FXAA, 2/4/8 MSAA */
uint32_t tod_hero_handle(void);                /* tod_cheat.cpp: 0 = no level / hero yet */
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

enum { TAB_VIDEO, TAB_SYSTEM, TAB_CHEATS, TAB_CONTROLS, TAB_SOON, NTABS };
const char* const kTabName[NTABS] = { "Video", "System", "Cheats", "Controls", "Coming soon" };

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
    add({ "RSX_VSYNC", "VSync", "Syncs presentation to the monitor. Off allows more fps than the refresh rate (with tearing).",
          { { "1", "On" }, { "0", "Off" } }, 0, true, TAB_VIDEO });
    add({ "TOD_FPS", "FPS limit",
          "Frames-per-second cap. Above 60 VSync turns itself off and physics uses a smaller step (docs\\FRAMERATE.md). Unlimited = 1000.",
          { { NULL, "Original" }, { "30", "30" }, { "60", "60" }, { "120", "120" }, { "1000", "Unlimited" } }, 0, false, TAB_VIDEO });
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
          "Shows the frame rate in the top-right corner. Detailed adds the frame time and the 1% low (last 5 s).",
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
    {
        Row r{ NULL, "Saves folder", "Opens the folder that holds your saves in Explorer. PS3_SAVEDATA_ROOT in tod.ini moves it; "
               "the layout (one BCES00052_SAVE_<n> folder per save) is the one RPCS3 uses.",
               { { "1", "Open" } }, 0, true, TAB_SYSTEM };
        r.cheat = 4;      /* an action button (not a cheat, not a setting) */
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
        if (!strcmp(r.key, "RSX_AA")) {
            g_rsx_aa = !v ? 0 : !strcmp(v, "fxaa") ? 1 : !strcmp(v, "msaa2") ? 2 : !strcmp(v, "msaa4") ? 4 : !strcmp(v, "msaa8") ? 8 : 0;
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

void set_open(bool on)
{
    if (s_open.load() == on) return;
    s_open.store(on);
    s_cap_row.store(-1);      /* a Controls capture ends with the page */
    g_cellPad_input_block = on ? 1 : 0;
    if (on) s_want_toggle.store(true);     /* the render thread refreshes the rows */
    fprintf(stderr, "[overlay] %s (input to the game %s)\n", on ? "open" : "closed", on ? "blocked" : "restored");
}

int overlay_message(void*, unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* result)
{
    if (!s_cs_init) return 0;
    const bool open = s_open.load();
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
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
        if (!open) return 0;
        *result = 0;
        return 1;
    case WM_MOUSEMOVE:
        if (!open) return 0;
        push({ 1, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp) });
        *result = 0;
        return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
        if (!open) return 0;
        push({ 1, (float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp) });
        push({ 2, 0, msg == WM_LBUTTONUP ? 0.0f : 1.0f });
        *result = 0;
        return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
        if (!open) return 0;
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
        if (!open) return 0;
        push({ 3, (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA, 0 });
        *result = 0;
        return 1;
    case WM_SETCURSOR:
        if (!open || LOWORD(lp) != HTCLIENT) return 0;
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
uint32_t s_pad_prev;
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
    if (r.cheat) { cheat_activate(r); return; }
    const int n = (int)r.choices.size();
    int cur = r.pending >= 0 ? r.pending : (dir > 0 ? -1 : 0);
    cur = (cur + dir + n) % n;
    r.pending = cur;
    r.other.clear();
}

void do_act(int act)
{
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
    ImGui::TextDisabled("  Ratchet & Clank: Tools of Destruction - port");
    ImGui::Spacing();

    /* tabs */
    const float tabw = 136.0f * k;
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
        const bool grey = r.cheat && r.cheat != 4 && !r.ctl && !in_game;      /* a cheat with no level running: shown, inert */
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
        const ImU32 txt = (r.soon || grey) ? IM_COL32(120, 125, 140, 255) : IM_COL32(235, 238, 245, 255);
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
            if (r.cheat == 2 || r.cheat == 4 || r.ctl == 1 || r.ctl == 3) {      /* a button (or a key binding): a pill round the word, no arrows */
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
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && r.cheat != 2 && r.cheat != 4 && r.ctl != 1 && r.ctl != 3) { s_sel = my; change_value(r, -1); }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && r.ctl == 1) { s_sel = my; controls_clear(r); }   /* unbind */
        } else {
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
char s_fps_l[3][48];
int s_fps_lines;
int s_fps_prev_mode;

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
    }
}

/* Top-right, over the game (its health bar is top-left). Foreground draw list: no window. */
void fps_draw(float W, float k)
{
    if (s_fps_lines <= 0) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float pad = 8.0f * k, margin = 14.0f * k;
    float tw = 0, th = 0;
    ImVec2 sz[3];
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

/* TOD_OVERLAY_SCRIPT (debug): "<sec>:<cmd>,..." run on the render thread, one per present, seconds since the first present. */
struct ScriptItem { double t; std::string cmd; };
std::vector<ScriptItem> s_script;
double s_script_t0;

void script_step()
{
    if (s_script.empty()) return;
    const double t = now_s();
    if (!s_script_t0) s_script_t0 = t;
    if (t - s_script_t0 < s_script.front().t) return;
    const std::string c = s_script.front().cmd;
    s_script.erase(s_script.begin());
    fprintf(stderr, "[overlay] script: %s\n", c.c_str());
    if (c == "open") { set_open(true); return; }
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
    if (s_auto_at >= 0 && !s_open.load()) {
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
    if (!s_open.load() && !cm) return;
    if (!ensure_ready(devp, queuep, frames, w, h)) return;
    ImGuiIO& io = ImGui::GetIO();
    const float k = std::max(0.75f, std::min(3.0f, (float)h / 1080.0f));
    if (k != s_scale_cur) { style_setup(k); s_scale_cur = k; }
    io.DisplaySize = ImVec2((float)w, (float)h);
    if (s_open.load()) init_rows();
    if (s_open.load() && s_want_toggle.exchange(false)) {          /* just opened */
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
    if (s_open.load()) pad_actions(now_s());
    const bool open = s_open.load();     /* may have been closed by an action above */
    if (!open && !cm) return;

    ID3D12GraphicsCommandList* list = (ID3D12GraphicsCommandList*)listp;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    if (open) build_page((float)w, (float)h, k);
    if (cm) fps_draw((float)w, k);
    ImGui::Render();
    D3D12_CPU_DESCRIPTOR_HANDLE rt; rt.ptr = (SIZE_T)rtv;
    list->OMSetRenderTargets(1, &rt, FALSE, NULL);
    list->SetDescriptorHeaps(1, &s_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
}

}  // namespace

/* ---- entry points ---------------------------------------------------------- */

/* The pad poll (host_input.cpp), once per poll: the real pad's buttons. Opens the page
 * after SELECT is held for 2 s; the buttons feed the page while it is open. */
extern "C" void tod_overlay_pad_feed(uint32_t real_buttons)
{
    if (!s_cs_init) return;
    s_pad.store(real_buttons);
    static ULONGLONG since;
    static bool fired;
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

extern "C" int tod_overlay_open(void) { return s_open.load() ? 1 : 0; }

extern "C" void tod_overlay_init(void)
{
    const char* off = getenv("TOD_OVERLAY");
    if (off && !strcmp(off, "0")) return;
    if (!s_cs_init) { InitializeCriticalSection(&s_cs); s_cs_init = true; }
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
    fprintf(stderr, "[overlay] settings page: F1 or SELECT held 2 s\n");
}
