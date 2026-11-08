/*
 * aspect_hor.cpp (r121u) -- TOD_ASPECT: a real wider view (Hor+) instead of the 16:9 picture stretched.
 *
 * With TOD_ASPECT the guest still renders its 1280x720 frame and the present stretches it to the chosen shape
 * (rsx_present_set_frame_shape); the game's own aspect constant (0x8A73FC) only feeds the display set-up, not the 3D camera.
 * Found 08/11 (scan of guest memory for view-projection matrices + hardware write breakpoints, log\r121u):
 *   - func_00597328 builds a camera's projection at camera+0x110 (column-major floats, P00 at +0, P11 at +0x14, P20 at +0x20)
 *     from near/far and the frustum's left/right/bottom/top, glFrustum style; func_005973E0 is the same builder that also stores
 *     near/far at camera+0/+4. The main camera is the object at 0x1072BB80 (projection 0x1072BC90): P00 = -1.29152,
 *     P11 = 2.29603 (horizontal field of view 75.5 deg, vertical 47.0 deg, |P11/P00| = 16/9). The projection is built when a
 *     camera is set up (and every frame in some scripted shots) and multiplied by the view each frame (func_00599480 ->
 *     camera+0x150), so the culling and every view-projection copy follow it.
 * Both builders are renamed *_lifted (tools/aspect_proj_ppu_patch.cmake); here the lifted code runs and then, for a projection
 * whose |P11/P00| is 16/9, P00 is divided by s = aspect / (16/9): the vertical field of view stays, the horizontal one widens
 * (tan(h'/2) = tan(h/2) * s). Off-centre terms (P20, jitter) are ratios of the same extents and do not change. Projections of
 * another shape (shadow maps, square render targets) are left alone.
 * Off (no TOD_ASPECT, or TOD_ASPECT_HOR=0): nothing is touched; the picture is stretched as in 1.2.0.
 */
#include "ppu_recomp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void func_00597328_lifted(ppu_context* ctx);
void func_005973E0_lifted(ppu_context* ctx);

static float g_hor_s = 1.0f;     /* aspect / (16/9); 1 = off */

static inline float rdf(uint32_t a) { uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; }
static inline void  wrf(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); vm_write32(a, u); }

static void widen(uint32_t cam, int which)
{
    /* The pause menu (game mode word 0x10731D00 != 0) draws its blue band in 3D with the main camera, sized for a 16:9 view: a
     * camera set up while the mode is not 0 keeps its 16:9 projection (the band then reaches the sides of the stretched frame). */
    if (vm_read32(0x10731D00u) != 0) {
        static int np; if (np < 4) { np++; fprintf(stderr, "[aspect-hor] cam 0x%08X set up in pause/menu mode: kept 16:9\n", cam); }
        return;
    }
    const uint32_t m = cam + 0x110u;
    const float p00 = rdf(m), p11 = rdf(m + 0x14);
    if (!(fabsf(p00) > 1e-3f && fabsf(p11) > 1e-3f)) return;
    if (fabsf(fabsf(p11 / p00) - 16.0f / 9.0f) > 0.01f) return;      /* not a 16:9 projection */
    wrf(m, p00 / g_hor_s);
    static int n;
    if (n < 6) { n++; fprintf(stderr, "[aspect-hor] %s cam 0x%08X P00 %.5f -> %.5f (P11 %.5f)\n", which ? "5973E0" : "597328",
                             cam, (double)p00, (double)(p00 / g_hor_s), (double)p11); }
}

void func_00597328(ppu_context* ctx)
{
    const uint32_t cam = (uint32_t)ctx->gpr[3];
    func_00597328_lifted(ctx);
    if (g_hor_s != 1.0f) widen(cam, 0);
}

void func_005973E0(ppu_context* ctx)
{
    const uint32_t cam = (uint32_t)ctx->gpr[3];
    func_005973E0_lifted(ctx);
    if (g_hor_s != 1.0f) widen(cam, 1);
}

extern "C" void tod_aspect_hor_install(float aspect)
{
    const char* m = getenv("TOD_ASPECT_HOR");
    if (m && m[0] == '0') { fprintf(stderr, "[aspect-hor] TOD_ASPECT_HOR=0: the 3D view stays 16:9 (stretched by the present)\n"); return; }
    if (!(aspect > 1.0f && aspect < 5.0f)) return;
    g_hor_s = aspect / (16.0f / 9.0f);
    fprintf(stderr, "[aspect-hor] Hor+: 16:9 projections widened x%.4f for aspect %.4f\n", (double)g_hor_s, (double)aspect);
}
