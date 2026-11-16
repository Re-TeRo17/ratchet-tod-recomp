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
#include <stdlib.h>

extern "C" float g_tod_ui_k;           /* src/main.cpp: 1 = off, else (16/9)/aspect */
void func_00660CD8_lifted(ppu_context* ctx);
void func_0065B468_lifted(ppu_context* ctx);

static float rdf(uint32_t a) { uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; }
static void wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(a, u); }

/* The transform before the narrowing (this frame), for the full-width rectangles below. */
static float s_xo_full, s_xs_full;
static int s_have_full;

/*
 * 09/11 letterbox: func_0065B468 draws one HUD rectangle, r3 -> (x0, y0, x1, y1) in UI units (the screen is 0x10724BE4 wide,
 * 1251.6). The cutscene bars and full-screen fades are rectangles spanning the whole UI width; narrowed to the middle 16:9
 * they left the sides of an ultrawide frame uncovered (log\rykauto\runs\rd_uw_kerchu_t35.png). Those rectangles are drawn with
 * the transform as it was before the narrowing, so after the present's stretch they reach both edges. TOD_UI_FULLRECT=0 turns
 * it off; TOD_UI_RECT_LOG=1 prints the rectangles.
 */
void func_0065B468(ppu_context* ctx)
{
    static int init, off, lg;
    static unsigned nl;
    if (!init) { init = 1; const char* e = getenv("TOD_UI_FULLRECT"); off = e && e[0] == '0';
                 lg = getenv("TOD_UI_RECT_LOG") != nullptr; }
    if (off || !s_have_full) { func_0065B468_lifted(ctx); return; }
    const uint32_t r = (uint32_t)ctx->gpr[3];
    const float x0 = rdf(r), x1 = rdf(r + 8);
    float W = rdf(0x10724BE4u);
    if (!(W > 100.0f)) W = 1251.6f;   /* the UI screen width (measured); the word reads 0 in game */
    const bool full = W > 100.0f && x0 <= 0.02f * W && x1 >= 0.98f * W;
    if (lg && nl < 3000) { nl++; fprintf(stderr, "[ui-rect] x %.1f..%.1f y %.1f..%.1f W %.1f full %d\n",
                                         (double)x0, (double)x1, (double)rdf(r + 4), (double)rdf(r + 12), (double)W, (int)full); }
    if (!full) { func_0065B468_lifted(ctx); return; }
    const uint32_t A = 0x10724BF4u;
    const float xo = rdf(A), xs = rdf(A + 4);
    wrf(A, s_xo_full); wrf(A + 4, s_xs_full);
    func_0065B468_lifted(ctx);
    wrf(A, xo); wrf(A + 4, xs);
}

void func_00660CD8(ppu_context* ctx)
{
    func_00660CD8_lifted(ctx);
    const float k = g_tod_ui_k;
    if (!(k < 0.999f && k > 0.1f)) { s_have_full = 0; return; }
    const uint32_t A = 0x10724BF4u;
    auto rd = rdf;
    auto wr = wrf;
    const float xo = rd(A), xs = rd(A + 4);
    s_xo_full = xo; s_xs_full = xs; s_have_full = 1;
    const float half = 640.0f;
    wr(A, xo * k + half * (1.0f - k));
    wr(A + 4, xs * k);
    static int n;
    if (n < 4) { n++; fprintf(stderr, "[ui-xform] k=%.4f x: off %.3f scale %.5f -> off %.3f scale %.5f (y: off %.3f scale %.5f)\n",
                              (double)k, (double)xo, (double)xs, (double)rd(A), (double)rd(A + 4), (double)rd(A + 8), (double)rd(A + 12)); }
}
