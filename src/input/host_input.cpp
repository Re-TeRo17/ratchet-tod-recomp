/*
 * Port 0's host input beside a real pad (cellPad_set_host_input, libs/input/cellPad.h):
 *
 *   - the keyboard, while the window has the focus (the runtime's built-in
 *     fallback mapping, which registering a host input replaces);
 *   - TOD_INPUT_SCRIPT, scripted presses for unattended test runs, so a run can
 *     get past screens that wait for a button without touching the real
 *     keyboard: "<seconds>:<button>[+<button>...],..." from the first call,
 *     each held 250 ms ("t0-t1:btn" = held from t0 to t1). Buttons: cross circle square triangle start select
 *     up down left right l1 r1 l2 r2, and rleft rright rup rdown (right stick, camera). Example: TOD_INPUT_SCRIPT=30:start,35:cross
 *   - TOD_MOUSE=1 (default off): the mouse as camera (right stick) and buttons, see "mouse look" below.
 *   - the keys and mouse buttons are rebindable (KEY_* / TOD_MOUSE_* in tod.ini, the F1 page's Controls tab):
 *     src/input/key_bindings.cpp.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <thread>

#include "win32_compat.h"
#include "cellPad.h"   /* has its own extern "C" */
#include "input/key_bindings.h"

namespace {

struct Press {
    double t;       /* seconds from the first poll */
    double dur = 0.25;   /* hold time; "t0-t1:btn" in the script = held from t0 to t1 */
    uint16_t btns;
    uint8_t rs = 0;   /* right stick: 1 = rleft, 2 = rright, 4 = rup, 8 = rdown (camera) */
};
std::vector<Press> s_script;
uint64_t s_t0;

uint64_t now_ms() { return GetTickCount64(); }

uint16_t button_bit(const std::string& n)
{
    static const struct { const char* name; uint16_t bit; } k[] = {
        { "cross", CELL_PAD_CTRL_CROSS },   { "circle", CELL_PAD_CTRL_CIRCLE },
        { "square", CELL_PAD_CTRL_SQUARE }, { "triangle", CELL_PAD_CTRL_TRIANGLE },
        { "start", CELL_PAD_CTRL_START },   { "select", CELL_PAD_CTRL_SELECT },
        { "up", CELL_PAD_CTRL_UP },         { "down", CELL_PAD_CTRL_DOWN },
        { "left", CELL_PAD_CTRL_LEFT },     { "right", CELL_PAD_CTRL_RIGHT },
        { "l1", CELL_PAD_CTRL_L1 },         { "r1", CELL_PAD_CTRL_R1 },
        { "l2", CELL_PAD_CTRL_L2 },         { "r2", CELL_PAD_CTRL_R2 },
    };
    for (const auto& e : k)
        if (n == e.name) return e.bit;
    fprintf(stderr, "[input] TOD_INPUT_SCRIPT: unknown button '%s'\n", n.c_str());
    return 0;
}

void parse_script(const char* s)
{
    std::string all(s);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(pos, end - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            Press p{ atof(item.substr(0, colon).c_str()), 0.25, 0 };
            { const size_t dash = item.find('-'); if (dash != std::string::npos && dash < colon) { const double t1 = atof(item.c_str() + dash + 1); if (t1 > p.t) p.dur = t1 - p.t; } }
            std::string names = item.substr(colon + 1);
            size_t q = 0;
            while (q <= names.size()) {
                size_t plus = names.find('+', q);
                if (plus == std::string::npos) plus = names.size();
                const std::string nm = names.substr(q, plus - q);
                if (nm == "rleft") p.rs |= 1; else if (nm == "rright") p.rs |= 2;
                else if (nm == "rup") p.rs |= 4; else if (nm == "rdown") p.rs |= 8;
                else p.btns |= button_bit(nm);
                q = plus + 1;
            }
            s_script.push_back(p);
        }
        pos = end + 1;
    }
    fprintf(stderr, "[input] TOD_INPUT_SCRIPT: %zu presses\n", s_script.size());
}

uint8_t s_script_rs;   /* right-stick bits of the presses active at the last script_buttons() */
uint16_t script_buttons()
{
    s_script_rs = 0;
    if (s_script.empty()) return 0;
    if (!s_t0) s_t0 = now_ms();
    const double t = (now_ms() - s_t0) / 1000.0;
    uint16_t b = 0;
    for (const Press& p : s_script) {
        if (t >= p.t && t < p.t + p.dur) {
            b |= p.btns;
            s_script_rs |= p.rs;
        }
    }
    {   /* log each change of the scripted state (overlapping presses used to flip-flop the log) */
        static uint32_t was = 0xFFFFFFFFu; const uint32_t now = b | ((uint32_t)s_script_rs << 16);
        if (now != was) { was = now; fprintf(stderr, "[input] script state buttons 0x%04X rstick %u at %.1f s\n", b, (unsigned)s_script_rs, t); }
    }
    return b;
}

extern "C" void rsx_draw_engine_trace_after(long after);
extern "C" void rsx_draw_engine_pick_after(int x, int y, long after);

/* RSX_TRACE_ON_F8=1: F8 (the screenshot key of tools_sweep8_catcher.ps1) also
 * traces every draw of the next frames, so each screenshot comes with the
 * textures, shaders and targets of what it shows. */
void f8_trace_poll()
{
#ifdef _WIN32
    static int on = -1, was = 0;
    if (on < 0) on = getenv("RSX_TRACE_ON_F8") ? 1 : 0;
    if (!on) return;
    const int down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (down && !was) {
        rsx_draw_engine_trace_after(1);
        /* the mouse pointer on the defect: which draws write that pixel */
        POINT p; RECT r; HWND w = GetForegroundWindow();
        if (w && GetCursorPos(&p) && ScreenToClient(w, &p) && GetClientRect(w, &r) && r.right > 0 && r.bottom > 0 &&
            p.x >= 0 && p.y >= 0 && p.x < r.right && p.y < r.bottom)
            rsx_draw_engine_pick_after((int)((long long)p.x * 1280 / r.right), (int)((long long)p.y * 720 / r.bottom), 1);
    }
    was = down;
#endif
}

extern "C" int fmv_skip_active(void);
extern "C" void fmv_skip_request(void);
extern "C" void fmv_skip_diag_poll(void);

/* Skip of Bink movies (sys_fs.c fmv_skip_*): START held 2 s on the pad (XInput pad 0, or the
 * keyboard key mapped to Start = Enter, or TOD_INPUT_SCRIPT), fallback Esc / Space held 2 s.
 * Only while a .bik is open (fmv_skip_active); TOD_FMV_SKIP=0 turns it off. Runs on the game's
 * pad-polling thread, so writing guest memory there is safe. Log prefix [fmv-skip]. */
}  // namespace
extern "C" uint32_t cellPad_host_pad0_buttons(void);   /* cellPad.c: port 0 real-pad buttons */
namespace {
void fmv_skip_poll(int focused, uint16_t kbd_script_buttons)
{
    fmv_skip_diag_poll();
    static ULONGLONG since = 0;
    static int fired = 0;
    if (!fmv_skip_active()) { since = 0; fired = 0; return; }
    int down = (kbd_script_buttons & CELL_PAD_CTRL_START) != 0;
    if (cellPad_host_pad0_buttons() & CELL_PAD_CTRL_START) down = 1;   /* the real pad (DualSense/SDL too) */
#ifdef _WIN32
    if (focused && ((GetAsyncKeyState(VK_ESCAPE) | GetAsyncKeyState(VK_SPACE)) & 0x8000)) down = 1;
    {   /* real pad: XInput loaded dynamically, no link dependency */
        typedef struct { DWORD pk; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XS;
        typedef DWORD (WINAPI *XGet)(DWORD, XS*);
        static XGet xget; static int tried;
        if (!tried) {
            tried = 1;
            HMODULE m = LoadLibraryA("xinput1_4.dll");
            if (!m) m = LoadLibraryA("xinput9_1_0.dll");
            if (m) xget = (XGet)GetProcAddress(m, "XInputGetState");
        }
        XS st;
        if (xget && xget(0, &st) == 0 && (st.buttons & 0x0010 /* XINPUT_GAMEPAD_START */)) down = 1;
    }
#endif
    if (!down) { since = 0; fired = 0; return; }
    const ULONGLONG now = now_ms();
    if (!since) since = now;
    if (!fired && now - since >= 2000) {
        fired = 1;
        fprintf(stderr, "[fmv-skip] START/Esc/Space held 2 s -> skip\n");
        fmv_skip_request();
    }
}

/* TOD_R1_SHOTS=<dir>: a screenshot of the game window when R1 (fire) goes down, then one every
 * 2 s while it is held, as <dir>/r1_<n>.bmp at most 1920 wide; each logs "[r1] shot n t= frame="
 * so the picture lines up with the [fx] / [cheat] lines. Captured from the screen on its own
 * thread (the window as DWM shows it), so the pad poll never waits. Max 400 shots. */
extern "C" uint32_t g_rsx_engine_frame;
void r1_capture(HWND w, std::string path)
{
#ifdef _WIN32
    RECT r; if (!GetClientRect(w, &r) || r.right <= 0 || r.bottom <= 0) return;
    POINT o = { 0, 0 }; ClientToScreen(w, &o);
    const int sw = r.right, sh = r.bottom;
    const int dw = sw > 1920 ? 1920 : sw, dh = (int)((long long)sh * dw / sw);
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bi = {}; bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = dw;
    bi.bmiHeader.biHeight = dh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        SetStretchBltMode(mem, HALFTONE);
        StretchBlt(mem, 0, 0, dw, dh, scr, o.x, o.y, sw, sh, SRCCOPY);
        SelectObject(mem, old);
        const int stride = (dw * 3 + 3) & ~3;
        BITMAPFILEHEADER fh = {}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof bi.bmiHeader;
        fh.bfSize = fh.bfOffBits + stride * dh;
        if (FILE* f = fopen(path.c_str(), "wb")) {
            fwrite(&fh, sizeof fh, 1, f); fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
            fwrite(bits, 1, (size_t)stride * dh, f); fclose(f);
        }
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem); ReleaseDC(nullptr, scr);
#endif
}
void r1_shot_poll(int focused, uint16_t kbd_script_buttons)
{
#ifdef _WIN32
    static int on = -1; static std::string dir;
    if (on < 0) { const char* e = getenv("TOD_R1_SHOTS"); on = e && *e; if (on) { dir = e; CreateDirectoryA(e, nullptr); } }
    if (!on) return;
    const int down = ((kbd_script_buttons | cellPad_host_pad0_buttons()) & CELL_PAD_CTRL_R1) != 0;
    static ULONGLONG next = 0; static unsigned n = 0; static int was = 0;
    const ULONGLONG now = now_ms();
    if (down && (!was || now >= next) && n < 400) {
        HWND w = GetForegroundWindow();
        if (w && focused) {
            ++n; next = now + 2000;
            char p[600]; snprintf(p, sizeof p, "%s\\r1_%03u.bmp", dir.c_str(), n);
            fprintf(stderr, "[r1] shot %u t=%.3f frame=%u %s\n", n, now / 1000.0, g_rsx_engine_frame, was ? "held" : "press");
            std::thread(r1_capture, w, std::string(p)).detach();
        }
    }
    was = down;
#endif
}

/* ---- mouse look (TOD_MOUSE=1) -------------------------------------------------------------------
 *
 * The mouse of the game window (rsx_host_mouse.c: raw input, cursor hidden and kept in the window)
 * as port 0's pad, beside the keyboard and a real pad:
 *   motion   -> RIGHT stick (camera / aim). Velocity mapping: the counts of the last ~40 ms, per 60 Hz frame,
 *               times TOD_MOUSE_SENS, are the stick's deflection; a stopped mouse recentres the stick within 40 ms.
 *               The follow camera reads the stick through a dead zone (|x| 0.35 -> 0.70 ramp, docs/CAMERA.md 2.3),
 *               so a moving mouse is mapped onto 0.35 .. 0.70 and the camera turns in proportion to the speed.
 *   left     -> R1 (fire)         right -> L1 (strafe / lock-on, the aim)       middle -> Triangle (quick select)
 *   wheel    -> D-pad left / right (short press)
 *   (the buttons and the wheel are rebindable: TOD_MOUSE_LEFT / _RIGHT / _MIDDLE / _WHEEL_UP / _WHEEL_DOWN,
 *   key_bindings.h; TOD_MOUSE_WHEEL=off still clears both wheel defaults)
 *   Esc      -> Start (pause), only with the mouse on: the held cursor can not click the window's close button
 *               (Shift+Esc and Alt+F4 still quit)
 * Capture is on while the window has the focus and the F1 page is closed; it is let go with either.
 *   TOD_MOUSE=0|1  TOD_MOUSE_SENS (default 1.0)  TOD_MOUSE_INVERT_Y=0|1  TOD_MOUSE_WHEEL=dpad|off
 *   All of these are re-read when the F1 page's Controls tab changes them (tod_input_reload()), so they are live.
 *   TOD_MOUSE_FAKE="dx,dy[,wheel]@sec dx,dy@t0-t1 ..."  debug: injected motion (counts, once at sec, or spread evenly
 *               over t0-t1), seconds since the process started; works without a real mouse or the focus.
 *   TOD_MOUSE_LOG=1  a line per second with the stick values.
 */
extern "C" {
void rsx_host_mouse_capture(int on);
int rsx_host_mouse_captured(void);
void rsx_host_mouse_take_motion(int* dx, int* dy);
int rsx_host_mouse_take_wheel(void);
}

namespace mouse {

bool s_on, s_inited, s_invert_y, s_log;
double s_sens = 1.0;
const double kFullCounts = 20.0;     /* counts per 60 Hz frame (1200 counts/s) for the camera's full speed at SENS 1 */
const double kDeadLo = 0.35, kDeadHi = 0.70;   /* the game's stick ramp (docs/CAMERA.md 2.3) */
const double kWindowMs = 40.0;       /* the stick follows the mouse of the last 40 ms */

struct Fake { double t0, t1; double dx, dy, wh; double sent; bool point; };
std::vector<Fake> s_fake;

uint64_t now_us()
{
    static LARGE_INTEGER f;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e6 / (double)f.QuadPart);
}

/* "dx,dy[,wheel]@when" items separated by spaces or ';' */
void parse_fake(const char* spec)
{
    const std::string all(spec);
    size_t i = 0;
    while (i < all.size()) {
        size_t sep = all.find_first_of("; ", i);
        if (sep == std::string::npos) sep = all.size();
        const std::string item = all.substr(i, sep - i);
        i = sep + 1;
        const size_t at = item.find('@');
        if (at == std::string::npos) continue;
        Fake f{};
        double v[3] = { 0, 0, 0 };
        int n = 0;
        const std::string a = item.substr(0, at);
        size_t q = 0;
        while (q <= a.size() && n < 3) {
            size_t c = a.find(',', q);
            if (c == std::string::npos) c = a.size();
            v[n++] = atof(a.substr(q, c - q).c_str());
            q = c + 1;
        }
        f.dx = v[0]; f.dy = v[1]; f.wh = v[2];
        const std::string w = item.substr(at + 1);
        const size_t dash = w.find('-');
        f.t0 = atof(w.c_str());
        f.point = dash == std::string::npos;
        f.t1 = f.point ? f.t0 : atof(w.c_str() + dash + 1);
        if (!f.point && f.t1 <= f.t0) f.point = true;
        s_fake.push_back(f);
    }
    fprintf(stderr, "[mouse] TOD_MOUSE_FAKE: %zu injections\n", s_fake.size());
}

/* The environment into the settings: at the start and whenever the Controls tab changes one (live). */
void reload()
{
    const char* e = getenv("TOD_MOUSE");
    const bool on = (e && *e) ? atoi(e) != 0 : kb::kMouseDefaultOn;
    s_sens = 1.0;
    if ((e = getenv("TOD_MOUSE_SENS"))) { const double v = atof(e); if (v > 0.01 && v < 100.0) s_sens = v; }
    s_invert_y = (e = getenv("TOD_MOUSE_INVERT_Y")) && atoi(e) != 0;
    if (s_on && !on) rsx_host_mouse_capture(0);   /* switched off from the page: the cursor is the desktop's again */
    if (s_on != on)
        fprintf(stderr, "[mouse] mouse camera %s: sens %.2f invert_y %d\n", on ? "on" : "off", s_sens, (int)s_invert_y);
    s_on = on;
}

void init()
{
    s_inited = true;
    const char* e;
    s_log = getenv("TOD_MOUSE_LOG") != nullptr;
    if ((e = getenv("TOD_MOUSE_FAKE")) && *e) parse_fake(e);
    reload();
    if (s_on)
        fprintf(stderr, "[mouse] TOD_MOUSE=1: sens %.2f invert_y %d (left=%s right=%s middle=%s, Esc=Start)\n", s_sens, (int)s_invert_y,
                kb::target_token(kb::mouse_get(kb::M_LEFT)), kb::target_token(kb::mouse_get(kb::M_RIGHT)),
                kb::target_token(kb::mouse_get(kb::M_MIDDLE)));
}

struct Sample { uint64_t t; int dx, dy; };
Sample s_ring[64];
int s_ring_n;
uint64_t s_wheel_until[2];   /* the press from a notch: [0] wheel away (kb::M_WHEEL_UP), [1] towards you (M_WHEEL_DOWN) */
bool s_esc_wait;             /* Esc held while the page closed: not a Start until released */
uint64_t s_log_next;
double s_ax, s_ay, s_aw;     /* fractions of the injected counts not handed out yet */

/* The stick's byte for a deflection u (-1..1) of the camera's speed: 128 = rest. */
uint8_t stick_byte(double u)
{
    if (u > -1e-3 && u < 1e-3) return 128;
    double a = u < 0 ? -u : u;
    if (a > 1.0) a = 1.0;
    const double m = kDeadLo + (kDeadHi - kDeadLo) * a;   /* onto the game's ramp */
    int v = (int)(m * 127.0 + 0.5);
    v = u < 0 ? 128 - v : 128 + v;
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

void release()
{
    if (!s_on) return;
    rsx_host_mouse_capture(0);
    s_ring_n = 0;
}

/* One pad poll. Returns nonzero while the mouse is in use. Adds its buttons to *btn and gives the right stick. */
int poll(int focused, uint16_t* btn, uint8_t* rx, uint8_t* ry)
{
    if (!s_on) return 0;
    const uint64_t t = now_us();
    rsx_host_mouse_capture(focused ? 1 : 0);
    const bool held = rsx_host_mouse_captured() != 0;
    int dx = 0, dy = 0;
    rsx_host_mouse_take_motion(&dx, &dy);
    int wheel = rsx_host_mouse_take_wheel();
    if (!held) { dx = dy = wheel = 0; }
    if (!s_fake.empty()) {
        static double up0 = -1;   /* seconds since the process started (what a run script's clock shows) */
        if (up0 < 0) {
            FILETIME cr, ex, kr, us, nowft;
            up0 = 0;
            if (GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kr, &us)) {
                GetSystemTimeAsFileTime(&nowft);
                const unsigned long long c = ((unsigned long long)cr.dwHighDateTime << 32) | cr.dwLowDateTime;
                const unsigned long long n = ((unsigned long long)nowft.dwHighDateTime << 32) | nowft.dwLowDateTime;
                up0 = (double)(n - c) / 1e7;
            }
            up0 -= (double)t / 1e6;   /* uptime = up0 + t */
        }
        const double ts = up0 + (double)t / 1e6;
        for (Fake& f : s_fake) {
            if (ts < f.t0) continue;
            double frac = 1.0;
            if (!f.point) { frac = (ts - f.t0) / (f.t1 - f.t0); if (frac > 1.0) frac = 1.0; }
            if (frac > f.sent) {
                const double d = frac - f.sent;
                s_ax += f.dx * d; s_ay += f.dy * d; s_aw += f.wh * d;
                f.sent = frac;
            }
        }
        const int ix = (int)s_ax, iy = (int)s_ay, iw = (int)s_aw;
        s_ax -= ix; s_ay -= iy; s_aw -= iw;
        dx += ix; dy += iy; wheel += iw;
    }
    if ((dx || dy) && s_ring_n < 64) s_ring[s_ring_n++] = { t, dx, dy };
    {   /* what is older than the window is gone: a stopped mouse recentres the stick */
        int k = 0;
        for (int i = 0; i < s_ring_n; i++)
            if ((double)(t - s_ring[i].t) / 1000.0 <= kWindowMs) s_ring[k++] = s_ring[i];
        s_ring_n = k;
    }
    double sx = 0, sy = 0, span = 0;
    if (s_ring_n) {
        uint64_t oldest = s_ring[0].t;
        for (int i = 0; i < s_ring_n; i++) {
            sx += s_ring[i].dx; sy += s_ring[i].dy;
            if (s_ring[i].t < oldest) oldest = s_ring[i].t;
        }
        span = (double)(t - oldest) / 1000.0;
    }
    if (span < 16.7) span = 16.7;   /* counts per 60 Hz frame, never from a slice shorter than one */
    const double k = s_sens / kFullCounts * (16.7 / span);   /* deflection per count */
    const double ux = sx * k, uy = sy * k * (s_invert_y ? -1.0 : 1.0);
    *rx = stick_byte(ux);
    *ry = stick_byte(uy);   /* +y = the mouse towards the player = the stick down */
    if (wheel) s_wheel_until[wheel > 0 ? 0 : 1] = t + 90000;
    uint16_t b = 0;
    uint8_t ls_unused = 0, rs_unused = 0;   /* the mouse bindings are pad buttons (Cross .. D-pad): no stick bits */
    if (t < s_wheel_until[0]) kb::apply(kb::mouse_get(kb::M_WHEEL_UP), &b, &ls_unused, &rs_unused);
    if (t < s_wheel_until[1]) kb::apply(kb::mouse_get(kb::M_WHEEL_DOWN), &b, &ls_unused, &rs_unused);
    if (held) {
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) kb::apply(kb::mouse_get(kb::M_LEFT), &b, &ls_unused, &rs_unused);
        if (GetAsyncKeyState(VK_RBUTTON) & 0x8000) kb::apply(kb::mouse_get(kb::M_RIGHT), &b, &ls_unused, &rs_unused);
        if (GetAsyncKeyState(VK_MBUTTON) & 0x8000) kb::apply(kb::mouse_get(kb::M_MIDDLE), &b, &ls_unused, &rs_unused);
    }
    if (focused) {   /* Esc = Start, but not the Esc that closed the F1 page */
        const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        if (!esc) s_esc_wait = false;
        else if (!s_esc_wait && !(GetAsyncKeyState(VK_SHIFT) & 0x8000)) b |= CELL_PAD_CTRL_START;
    }
    if (s_log && t >= s_log_next) {
        s_log_next = t + 1000000;
        fprintf(stderr, "[mouse] held %d stick rx %u ry %u (counts over %.0f ms: %.0f,%.0f) buttons 0x%04X\n",
                (int)held, (unsigned)*rx, (unsigned)*ry, span, sx, sy, (unsigned)b);
    }
    *btn |= b;
    return (s_ring_n != 0) || (b != 0);
}

}  // namespace mouse

extern "C" void tod_bot_init(void);
extern "C" int tod_bot_apply(CellPadHostInput*);
extern "C" void tod_autopilot_init(void);
extern "C" int tod_autopilot_apply(CellPadHostInput*);   /* TOD_AUTOPILOT (tod_autopilot.cpp) */
extern "C" void tod_cheat_init(void);
extern "C" void tod_cheat_poll(int focused);
extern "C" void tod_ds_poll(int ui_open);   /* tod_dualsense.cpp: DualSense triggers / speaker, no-op without a DualSense */

extern "C" void tod_overlay_pad_feed(uint32_t real_buttons);   /* src/ui/tod_overlay.cpp */
extern "C" int tod_overlay_open(void);

int host_input(void*, int focused, CellPadHostInput* out)
{
    static const bool kb_log = getenv("TOD_KEYS_LOG") != nullptr || (getenv("TOD_KEY_FAKE") && *getenv("TOD_KEY_FAKE"));
    tod_overlay_pad_feed(cellPad_host_pad0_buttons());   /* SELECT held 2 s opens the settings page */
    tod_ds_poll(tod_overlay_open());
    if (tod_overlay_open()) {   /* the page owns the input: nothing from the keyboard or a script, and
                                   not "the keyboard was touched last" (g_cellPad_input_block clears the pad) */
        out->lx = out->ly = out->rx = out->ry = 128;
        mouse::release();   /* TOD_MOUSE: the page has the cursor */
        mouse::s_esc_wait = true;
        return 0;
    }
    f8_trace_poll();
    tod_cheat_poll(focused);   /* TOD_CHEAT*: cheat menu (tod_cheat.cpp), no-op when off */
    if (!mouse::s_inited) mouse::init();
    uint8_t kls = 0, krs = 0;   /* stick directions the bound keys give: 1 left 2 right 4 up 8 down */
    const uint16_t kbd = kb::poll(focused != 0, &kls, &krs);   /* KEY_* bindings (defaults = the old fixed layout) */
    uint16_t b = kbd | script_buttons();
    uint8_t mrx = 128, mry = 128;
    const int mouse_used = mouse::poll(focused, &b, &mrx, &mry);   /* TOD_MOUSE=1: buttons + right stick */
    fmv_skip_poll(focused, b);   /* hold START 2 s while a Bink movie plays */
    r1_shot_poll(focused, b);    /* TOD_R1_SHOTS: screenshots while firing */
    out->buttons = b;
    {   /* the left stick follows the D-pad of the script / the mouse wheel, and the keys bound to it; the D-pad KEYS
         * only press the D-pad (by default the arrows are bound to both) */
        const uint16_t dsrc = b & ~(kbd & (CELL_PAD_CTRL_UP | CELL_PAD_CTRL_DOWN | CELL_PAD_CTRL_LEFT | CELL_PAD_CTRL_RIGHT));
        out->lx = ((dsrc & CELL_PAD_CTRL_LEFT) || (kls & 1)) ? 0 : ((dsrc & CELL_PAD_CTRL_RIGHT) || (kls & 2)) ? 255 : 128;
        out->ly = ((dsrc & CELL_PAD_CTRL_UP) || (kls & 4)) ? 0 : ((dsrc & CELL_PAD_CTRL_DOWN) || (kls & 8)) ? 255 : 128;
    }
    {   /* the camera: TOD_INPUT_SCRIPT rleft/rright/rup/rdown and the KEY_CAMERA_* keys */
        const uint8_t rs = s_script_rs | krs;
        out->rx = (rs & 1) ? 0 : (rs & 2) ? 255 : 128;
        out->ry = (rs & 4) ? 0 : (rs & 8) ? 255 : 128;
    }
    if (mrx != 128 || mry != 128) { out->rx = mrx; out->ry = mry; }   /* the mouse, only while it moves (or just did) */
    out->l2 = (b & CELL_PAD_CTRL_L2) ? 255 : 0;
    out->r2 = (b & CELL_PAD_CTRL_R2) ? 255 : 0;
    const int botdrv = tod_bot_apply(out) | tod_autopilot_apply(out);   /* TOD_BOT: waypoint bot (tod_bot.cpp); TOD_AUTOPILOT: tod_autopilot.cpp */
    if (kb_log) {   /* TOD_KEYS_LOG=1 / TOD_KEY_FAKE: what the bindings gave, at each change */
        static uint64_t was = ~0ull;
        const uint64_t now = (uint64_t)b | ((uint64_t)kls << 16) | ((uint64_t)krs << 24);
        if (now != was) { was = now; fprintf(stderr, "[kb] t=%.1f focused %d buttons 0x%04X lstick bits %u camera bits %u\n", (double)GetTickCount64() / 1000.0, focused, (unsigned)b, (unsigned)kls, (unsigned)krs); }
    }
    return (b != 0) || s_script_rs || botdrv || mouse_used || kls || krs;
}

}  // namespace

/* TOD_CHEAT_FILE "inp=<script>": replace the input script, times counted from now (tod_cheat.cpp,
 * same pad thread), so a test can turn the camera right after a warp whatever the load time was. */
extern "C" void tod_input_script_set(const char* s)
{
    s_script.clear();
    parse_script(s);
    s_t0 = now_ms();
}

/* The F1 page's Controls tab changed a binding or a mouse setting (the environment holds the new values). */
extern "C" void tod_input_reload(void)
{
    kb::reload_from_env();
    mouse::reload();
}

extern "C" void tod_input_init(void)
{
    kb::init();
    if (const char* s = getenv("TOD_INPUT_SCRIPT")) parse_script(s);
    tod_bot_init();
    tod_autopilot_init();
    tod_cheat_init();
    cellPad_set_host_input(host_input, nullptr);
}
