/*
 * ui_xform.cpp (wide121) -- TOD_ASPECT_UI: keep the HUD 16:9 inside a wider (TOD_ASPECT) frame.
 *
 * func_00660CD8 runs once per frame, right before the HUD's widget tree is drawn (func_00661400 -> here). It turns the UI's own
 * units (the "screen" at 0x10724BE4, 1251.6 x 704) into guest frame pixels and stores the result at 0x10724BF4:
 *   [0] x offset, [4] x scale, [8] y offset, [12] y scale          (floats; the HUD's quad writers read them: 65B468, 65F5A8, 6612F0)
 * (measured: 0, 1.02273, 0, 1.02273 -> 1251.6 x 704 UI units fill the 1280 x 720 frame.)
 * With TOD_ASPECT the guest still renders 1280 x 720 (x RSX_SCALE) and the present stretches the frame to the wider shape, so a
 * HUD laid out over the whole 1280 columns comes out stretched. The lifted code was renamed func_00660CD8_lifted
 * (tools/ui_xform_ppu_patch.cmake); here it runs, and then x scale and x offset are narrowed by k = (16/9) / aspect around the
 * middle column (640): the HUD occupies the middle 16:9 of the frame and, after the present's stretch, has its true proportions.
 * Off (no TOD_ASPECT, or TOD_ASPECT_UI=0): nothing is touched.
 */
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" float g_tod_ui_k;           /* src/main.cpp: 1 = off, else (16/9)/aspect */
void func_00660CD8_lifted(ppu_context* ctx);

void func_00660CD8(ppu_context* ctx)
{
    func_00660CD8_lifted(ctx);
    const float k = g_tod_ui_k;
    if (!(k < 0.999f && k > 0.1f)) return;
    const uint32_t A = 0x10724BF4u;
    auto rd = [](uint32_t a) { uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; };
    auto wr = [](uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(a, u); };
    const float xo = rd(A), xs = rd(A + 4);
    const float half = 640.0f;
    wr(A, xo * k + half * (1.0f - k));
    wr(A + 4, xs * k);
    static int n;
    if (n < 4) { n++; fprintf(stderr, "[ui-xform] k=%.4f x: off %.3f scale %.5f -> off %.3f scale %.5f (y: off %.3f scale %.5f)\n",
                              (double)k, (double)xo, (double)xs, (double)rd(A), (double)rd(A + 4), (double)rd(A + 8), (double)rd(A + 12)); }
}
