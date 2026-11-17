/*
 * tod_overlay_stub.cpp: the settings overlay's entry points on hosts without the D3D12 overlay
 * (src/ui/tod_overlay.cpp draws Dear ImGui over the D3D12 present and is Windows-only).
 *
 * No F1 page and no alert boxes: alerts answer "not shown" (-2, the value the real overlay
 * returns when it cannot show one), so QUIT/restart prompts change nothing. Menu rows still
 * take effect through the environment, as tod_menu_apply_key_win does on Windows; persisting
 * them to tod.ini and the in-game page come with an overlay for the non-D3D12 renderer.
 */
#ifndef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" void tod_fps_apply_live(const char* v);   /* main.cpp */

extern "C" void tod_overlay_init(void)
{
    fprintf(stderr, "[overlay] settings page not available on this platform yet\n");
}
extern "C" void tod_overlay_pad_feed(uint32_t real_buttons) { (void)real_buttons; }
extern "C" int tod_overlay_open(void) { return 0; }
extern "C" float tod_esci_w720(void) { return 0.0f; }   /* 0 = the menu keeps its own layout */
extern "C" int tod_alert(const char* title, const char* sub)
{
    fprintf(stderr, "[alert] %s%s%s (not shown: no overlay)\n", title ? title : "", sub ? " / " : "", sub ? sub : "");
    return -2;
}
extern "C" void tod_restart_open(void) { fprintf(stderr, "[restart] rows that need a restart were changed\n"); }
extern "C" void tod_quit_open(void) { fprintf(stderr, "[quit] QUIT picked in the pause menu (no overlay: ignored)\n"); }
extern "C" void tod_exit_open(void) { fprintf(stderr, "[quit] QUIT picked on the main menu\n"); fflush(stderr); exit(0); }
extern "C" void tod_menu_apply_key_win(const char* key, const char* v)
{
    if (v && *v) setenv(key, v, 1); else unsetenv(key);
    if (!strcmp(key, "TOD_FPS")) tod_fps_apply_live(v);
    fprintf(stderr, "[menu] %s = %s applied (environment only)\n", key, v ? v : "(default)");
}
extern "C" void tod_menu_set_key(const char* key, const char* v) { tod_menu_apply_key_win(key, v); }
#endif
