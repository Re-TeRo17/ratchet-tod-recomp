/* Keyboard / mouse bindings of port 0: see key_bindings.h. */
#include "input/key_bindings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "win32_compat.h"
#include "cellPad.h"   /* CELL_PAD_CTRL_*, has its own extern "C" */

namespace kb {

namespace {

struct Def { const char* ini; const char* label; int def_vk; };
const Def kDefs[NACTIONS] = {
    { "KEY_CROSS",    "Cross",    'Z' },
    { "KEY_CIRCLE",   "Circle",   'X' },
    { "KEY_SQUARE",   "Square",   'A' },
    { "KEY_TRIANGLE", "Triangle", 'S' },
    { "KEY_L1",       "L1",       'Q' },
    { "KEY_R1",       "R1",       'W' },
    { "KEY_L2",       "L2",       '1' },
    { "KEY_R2",       "R2",       '2' },
    { "KEY_L3",       "L3 (left stick click)",  0 },
    { "KEY_R3",       "R3 (right stick click)", 0 },
    { "KEY_START",    "Start",    VK_RETURN },
    { "KEY_SELECT",   "Select",   VK_TAB },
    { "KEY_DPAD_UP",    "D-pad Up",    VK_UP },
    { "KEY_DPAD_DOWN",  "D-pad Down",  VK_DOWN },
    { "KEY_DPAD_LEFT",  "D-pad Left",  VK_LEFT },
    { "KEY_DPAD_RIGHT", "D-pad Right", VK_RIGHT },
    { "KEY_LSTICK_UP",    "Left stick Up",    VK_UP },
    { "KEY_LSTICK_DOWN",  "Left stick Down",  VK_DOWN },
    { "KEY_LSTICK_LEFT",  "Left stick Left",  VK_LEFT },
    { "KEY_LSTICK_RIGHT", "Left stick Right", VK_RIGHT },
    { "KEY_CAMERA_UP",    "Camera Up",    0 },
    { "KEY_CAMERA_DOWN",  "Camera Down",  0 },
    { "KEY_CAMERA_LEFT",  "Camera Left",  0 },
    { "KEY_CAMERA_RIGHT", "Camera Right", 0 },
};

struct MDef { const char* ini; const char* label; int def_action; };
const MDef kMDefs[NMOUSE] = {
    { "TOD_MOUSE_LEFT",       "Left button",  R1 },
    { "TOD_MOUSE_RIGHT",      "Right button", L1 },
    { "TOD_MOUSE_MIDDLE",     "Middle button", TRIANGLE },
    { "TOD_MOUSE_WHEEL_UP",   "Wheel away from you", DPAD_LEFT },
    { "TOD_MOUSE_WHEEL_DOWN", "Wheel towards you",   DPAD_RIGHT },
};

const char* const kTargetTokens[kNumButtonTargets] = {
    "Cross", "Circle", "Square", "Triangle", "L1", "R1", "L2", "R2", "L3", "R3", "Start", "Select",
    "DpadUp", "DpadDown", "DpadLeft", "DpadRight",
};

volatile int s_vk[NACTIONS];
volatile int s_mouse[NMOUSE];
bool s_inited;

struct KeyName { int vk; const char* name; };
const KeyName kNames[] = {
    { VK_SPACE, "Space" }, { VK_RETURN, "Enter" }, { VK_TAB, "Tab" }, { VK_ESCAPE, "Esc" }, { VK_BACK, "Backspace" },
    { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
    { VK_SHIFT, "Shift" }, { VK_LSHIFT, "LShift" }, { VK_RSHIFT, "RShift" },
    { VK_CONTROL, "Ctrl" }, { VK_LCONTROL, "LCtrl" }, { VK_RCONTROL, "RCtrl" },
    { VK_MENU, "Alt" }, { VK_LMENU, "LAlt" }, { VK_RMENU, "RAlt" },
    { VK_INSERT, "Insert" }, { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" },
    { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" }, { VK_CAPITAL, "CapsLock" }, { VK_PAUSE, "Pause" },
    { VK_SCROLL, "ScrollLock" }, { VK_NUMLOCK, "NumLock" },
    { VK_MULTIPLY, "NumMultiply" }, { VK_ADD, "NumPlus" }, { VK_SUBTRACT, "NumMinus" },
    { VK_DECIMAL, "NumDecimal" }, { VK_DIVIDE, "NumDivide" },
    { VK_OEM_1, "Semicolon" }, { VK_OEM_PLUS, "Equals" }, { VK_OEM_COMMA, "Comma" }, { VK_OEM_MINUS, "Minus" },
    { VK_OEM_PERIOD, "Period" }, { VK_OEM_2, "Slash" }, { VK_OEM_3, "Backquote" }, { VK_OEM_4, "LeftBracket" },
    { VK_OEM_5, "Backslash" }, { VK_OEM_6, "RightBracket" }, { VK_OEM_7, "Quote" }, { VK_OEM_102, "Oem102" },
    { VK_LBUTTON, "MouseLeft" }, { VK_RBUTTON, "MouseRight" }, { VK_MBUTTON, "MouseMiddle" },
    { VK_XBUTTON1, "Mouse4" }, { VK_XBUTTON2, "Mouse5" },
};

/* case-insensitive compare ignoring spaces and underscores ("Left Shift" = "LeftShift" = "left_shift") */
bool same_name(const char* a, const char* b)
{
    for (;;) {
        while (*a == ' ' || *a == '_') a++;
        while (*b == ' ' || *b == '_') b++;
        if (!*a || !*b) return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
}

bool key_down_real(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

/* TOD_KEY_FAKE (debug): "<sec>[-<sec>]:<key>[+<key>],..." virtual key presses, seconds from the first poll
 * (a single time = held 0.25 s). Goes through the same bindings as a real key, with or without the focus. */
struct Fake { double t0, t1; std::vector<int> vks; };
std::vector<Fake> s_fake;
uint64_t s_fake_t0;

void parse_fake(const char* spec)
{
    const std::string all(spec);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(pos, end - pos);
        pos = end + 1;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) continue;
        Fake f{};
        f.t0 = atof(item.c_str());
        f.t1 = f.t0 + 0.25;
        const size_t dash = item.find('-');
        if (dash != std::string::npos && dash < colon) { const double t1 = atof(item.c_str() + dash + 1); if (t1 > f.t0) f.t1 = t1; }
        const std::string names = item.substr(colon + 1);
        size_t q = 0;
        while (q <= names.size()) {
            size_t plus = names.find('+', q);
            if (plus == std::string::npos) plus = names.size();
            bool ok = false;
            const std::string nm = names.substr(q, plus - q);
            const int vk = parse_key(nm.c_str(), &ok);
            if (ok && vk) f.vks.push_back(vk); else fprintf(stderr, "[kb] TOD_KEY_FAKE: unknown key '%s'\n", nm.c_str());
            q = plus + 1;
        }
        s_fake.push_back(f);
    }
    fprintf(stderr, "[kb] TOD_KEY_FAKE: %zu presses\n", s_fake.size());
}

bool key_down_fake(int vk)
{
    if (s_fake.empty()) return false;
    if (!s_fake_t0) s_fake_t0 = GetTickCount64();
    const double t = (double)(GetTickCount64() - s_fake_t0) / 1000.0;
    for (const Fake& f : s_fake)
        if (t >= f.t0 && t < f.t1)
            for (int v : f.vks) if (v == vk) return true;
    return false;
}

void ensure_init()
{
    if (s_inited) return;
    s_inited = true;
    reload_from_env();
    if (const char* e = getenv("TOD_KEY_FAKE")) if (*e) parse_fake(e);
}

}  // namespace

void init() { ensure_init(); }
const char* ini_key(int a) { return kDefs[a].ini; }
const char* label(int a) { return kDefs[a].label; }
int default_vk(int a) { return kDefs[a].def_vk; }
int get(int a) { ensure_init(); return s_vk[a]; }
void set(int a, int vk) { s_vk[a] = vk; }

std::string key_name(int vk)
{
    if (!vk) return "None";
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Num" + std::to_string(vk - VK_NUMPAD0);
    for (const KeyName& k : kNames) if (k.vk == vk) return k.name;
    char b[16]; snprintf(b, sizeof b, "Key0x%02X", vk & 0xFF);
    return b;
}

int parse_key(const char* s, bool* ok)
{
    if (ok) *ok = true;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s || same_name(s, "None") || same_name(s, "Unbound") || !strcmp(s, "-")) return 0;
    if (!s[1] && ((toupper((unsigned char)*s) >= 'A' && toupper((unsigned char)*s) <= 'Z') || (*s >= '0' && *s <= '9')))
        return toupper((unsigned char)*s);
    if ((s[0] == 'F' || s[0] == 'f') && s[1] >= '1' && s[1] <= '9') {
        char* e; const long n = strtol(s + 1, &e, 10);
        if (!*e && n >= 1 && n <= 24) return VK_F1 + (int)n - 1;
    }
    if (!_strnicmp(s, "Num", 3) && s[3] >= '0' && s[3] <= '9' && !s[4]) return VK_NUMPAD0 + (s[3] - '0');
    if (!_strnicmp(s, "Key0x", 5)) { char* e; const long n = strtol(s + 5, &e, 16); if (!*e && n > 0 && n < 256) return (int)n; }
    for (const KeyName& k : kNames) if (same_name(s, k.name)) return k.vk;
    /* a few aliases */
    if (same_name(s, "Return")) return VK_RETURN;
    if (same_name(s, "Escape")) return VK_ESCAPE;
    if (same_name(s, "Mouse3")) return VK_MBUTTON;
    if (same_name(s, "PgUp")) return VK_PRIOR;
    if (same_name(s, "PgDn")) return VK_NEXT;
    if (same_name(s, "LeftShift")) return VK_LSHIFT;
    if (same_name(s, "RightShift")) return VK_RSHIFT;
    if (same_name(s, "LeftCtrl") || same_name(s, "LeftControl")) return VK_LCONTROL;
    if (same_name(s, "RightCtrl") || same_name(s, "RightControl")) return VK_RCONTROL;
    if (same_name(s, "LeftAlt")) return VK_LMENU;
    if (same_name(s, "RightAlt")) return VK_RMENU;
    if (ok) *ok = false;
    return 0;
}

const char* mouse_ini_key(int slot) { return kMDefs[slot].ini; }
const char* mouse_label(int slot) { return kMDefs[slot].label; }
int mouse_default(int slot)
{
    if (slot == M_WHEEL_UP || slot == M_WHEEL_DOWN) {   /* the old TOD_MOUSE_WHEEL=off still turns the wheel off */
        const char* w = getenv("TOD_MOUSE_WHEEL");
        if (w && (!strcmp(w, "off") || !strcmp(w, "0"))) return -1;
    }
    return kMDefs[slot].def_action;
}
int mouse_get(int slot) { ensure_init(); return s_mouse[slot]; }
void mouse_set(int slot, int action) { s_mouse[slot] = action; }

const char* target_token(int a) { return a >= 0 && a < kNumButtonTargets ? kTargetTokens[a] : "None"; }
int parse_target(const char* s, bool* ok)
{
    if (ok) *ok = true;
    if (!s || !*s || same_name(s, "None")) return -1;
    for (int i = 0; i < kNumButtonTargets; i++) if (same_name(s, kTargetTokens[i])) return i;
    if (same_name(s, "Up")) return DPAD_UP;
    if (same_name(s, "Down")) return DPAD_DOWN;
    if (same_name(s, "Left")) return DPAD_LEFT;
    if (same_name(s, "Right")) return DPAD_RIGHT;
    if (ok) *ok = false;
    return -1;
}

void reload_from_env()
{
    for (int a = 0; a < NACTIONS; a++) {
        const char* v = getenv(kDefs[a].ini);
        int vk = kDefs[a].def_vk;
        if (v && *v) {
            bool ok = false;
            const int p = parse_key(v, &ok);
            if (ok) vk = p; else fprintf(stderr, "[kb] %s = '%s': unknown key, keeping %s\n", kDefs[a].ini, v, key_name(vk).c_str());
        }
        s_vk[a] = vk;
    }
    for (int m = 0; m < NMOUSE; m++) {
        const char* v = getenv(kMDefs[m].ini);
        int act = mouse_default(m);
        if (v && *v) {
            bool ok = false;
            const int p = parse_target(v, &ok);
            if (ok) act = p; else fprintf(stderr, "[kb] %s = '%s': unknown button, keeping %s\n", kMDefs[m].ini, v, target_token(act));
        }
        s_mouse[m] = act;
    }
}

uint16_t button_bit(int a)
{
    static const uint16_t bits[kNumButtonTargets] = {
        CELL_PAD_CTRL_CROSS, CELL_PAD_CTRL_CIRCLE, CELL_PAD_CTRL_SQUARE, CELL_PAD_CTRL_TRIANGLE,
        CELL_PAD_CTRL_L1, CELL_PAD_CTRL_R1, CELL_PAD_CTRL_L2, CELL_PAD_CTRL_R2, CELL_PAD_CTRL_L3, CELL_PAD_CTRL_R3,
        CELL_PAD_CTRL_START, CELL_PAD_CTRL_SELECT,
        CELL_PAD_CTRL_UP, CELL_PAD_CTRL_DOWN, CELL_PAD_CTRL_LEFT, CELL_PAD_CTRL_RIGHT,
    };
    return a >= 0 && a < kNumButtonTargets ? bits[a] : 0;
}

void apply(int a, uint16_t* btn, uint8_t* ls, uint8_t* rs)
{
    if (a < 0) return;
    if (a < kNumButtonTargets) { *btn |= button_bit(a); return; }
    switch (a) {
    case LS_LEFT: *ls |= 1; break;   case LS_RIGHT: *ls |= 2; break;
    case LS_UP: *ls |= 4; break;     case LS_DOWN: *ls |= 8; break;
    case CAM_LEFT: *rs |= 1; break;  case CAM_RIGHT: *rs |= 2; break;
    case CAM_UP: *rs |= 4; break;    case CAM_DOWN: *rs |= 8; break;
    default: break;
    }
}

uint16_t poll(bool focused, uint8_t* ls, uint8_t* rs)
{
    ensure_init();
    uint16_t b = 0;
    *ls = *rs = 0;
    for (int a = 0; a < NACTIONS; a++) {
        const int vk = s_vk[a];
        if (!vk) continue;
        if ((focused && key_down_real(vk)) || key_down_fake(vk)) apply(a, &b, ls, rs);
    }
    return b;
}

}  // namespace kb
