/*
 * ui_sync.cpp (09/11) -- the weapon vendor / weapons menu flicker.
 *
 * The menus (Anark UI) draw their quads twice a frame, and each pass writes the quads' vertices into the same main-memory bytes
 * (func_00629180 copies them with func_0081A728, return addresses 0x6292F4 / 0x629530) and pushes draws that read them. On the
 * PS3 the RSX has long read the first pass when the second one writes the same bytes again; here the renderer can still be
 * behind. Most of the time the two passes write the same bytes, so nothing shows; when a menu element appears or goes away
 * (the orange glow, every 1.5 s) the quads after it move by a slot and for one frame two quads are drawn from the other pass's
 * bytes: a piece of the menu (a stat bar, the left frame, the weapon icons) vanishes for a frame
 * (log\shop116 r_T83: positions read as texture coordinates; PS3_WATCH_WRITE=202B4780 -> lr 0x629530).
 * func_0081A728 is renamed _lifted (tools/ui_copy_ppu_patch.cmake). Here the UI copies are watched: the passes fill the bytes
 * downwards, so a copy above the last one starts a pass, and in a menu it first waits until the renderer has consumed what
 * was queued (main.cpp tod_ui_pass_start). TOD_MENU_SYNC=0 turns it off.
 */
#include "ppu_recomp.h"

void func_0081A728_lifted(ppu_context* ctx);
extern "C" void tod_ui_pass_start(void);
extern "C" int tod_menu_sync_on(void);

void func_0081A728(ppu_context* ctx)
{
    const uint32_t lr = (uint32_t)ctx->lr;
    if ((lr == 0x006292F4u || lr == 0x00629530u) && tod_menu_sync_on()) {
        static uint32_t last;
        const uint32_t dst = (uint32_t)ctx->gpr[3];
        if (dst > last) tod_ui_pass_start();
        last = dst;
    }
    func_0081A728_lifted(ctx);
}
