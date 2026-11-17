/*
 * tod_platform.h: the port's portability layer on top of ps3recomp's win32_compat.h.
 *
 * On Windows this is exactly <windows.h> (through win32_compat.h). Elsewhere (macOS, Linux)
 * win32_compat.h supplies the Win32 types, timers, events and interlocked operations the
 * runtime uses; this adds the few CRT names the port's own sources use on top of that.
 * Nothing here changes Windows behaviour.
 */
#pragma once
#include "win32_compat.h"   /* <windows.h> on Windows */

#ifndef _WIN32
#include <strings.h>
#include <limits.h>
#include <unistd.h>
#ifndef _stricmp
#define _stricmp strcasecmp
#endif
#ifndef _strnicmp
#define _strnicmp strncasecmp
#endif
#ifndef MAX_PATH
#define MAX_PATH PATH_MAX
#endif
#include <stdlib.h>
static inline int tod_putenv_s(const char* k, const char* v) { return (v && *v) ? setenv(k, v, 1) : unsetenv(k); }
#ifndef _putenv_s
#define _putenv_s tod_putenv_s
#endif
/* High-resolution waitable timers: not in win32_compat.h. NULL makes the callers fall back to
 * CreateWaitableTimerW, which the shim provides. */
#ifndef TIMER_ALL_ACCESS
#define TIMER_ALL_ACCESS 0x1F0003u
#endif
static inline HANDLE tod_CreateWaitableTimerExW(void* sa, const void* name, unsigned flags, unsigned access)
{ (void)sa; (void)name; (void)flags; (void)access; return NULL; }
#ifndef CreateWaitableTimerExW
#define CreateWaitableTimerExW tod_CreateWaitableTimerExW
#endif

/* Win32 virtual-key codes (the values are the Win32 ones, so tod.ini KEY_* bindings written
 * on Windows mean the same keys here). */
#ifndef VK_LBUTTON
enum {
    VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06,
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12,
    VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22,
    VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
    VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D,
    VK_NUMPAD0 = 0x60, VK_NUMPAD9 = 0x69, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D,
    VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F2 = 0x71, VK_F4 = 0x73, VK_F5 = 0x74,
    VK_F7 = 0x76, VK_F8 = 0x77, VK_F9 = 0x78, VK_F10 = 0x79, VK_F24 = 0x87, VK_NUMLOCK = 0x90,
    VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3,
    VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC,
    VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB,
    VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE, VK_OEM_102 = 0xE2,
};
#endif

/* Keyboard/mouse state off Windows. The host window layer (SDL2 on macOS, once the Vulkan
 * presenter owns a window) installs tod_host_key_down; until then no key reads as held, which
 * is what a headless run wants. Same contract as GetAsyncKeyState: bit 15 = held. */
#ifdef __cplusplus
extern "C" {
#endif
extern int (*tod_host_key_down)(int vk);
#ifdef __cplusplus
}
#endif
static inline short tod_GetAsyncKeyState(int vk) { return (tod_host_key_down && tod_host_key_down(vk)) ? (short)0x8000 : 0; }
#ifndef GetAsyncKeyState
#define GetAsyncKeyState tod_GetAsyncKeyState
#endif
#ifndef GetKeyState
#define GetKeyState tod_GetAsyncKeyState
#endif
#endif
