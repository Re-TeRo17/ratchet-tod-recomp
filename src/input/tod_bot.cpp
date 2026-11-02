/*
 * tod_bot.cpp -- waypoint bot for repeatable benchmarks / test runs (wired: src/input/host_input.cpp).
 *
 * What it does: at every pad poll (host_input -> tod_bot_apply, AFTER keyboard/script filled the pad)
 * it reads Ratchet's world position from guest RAM, steers the LEFT stick toward the current waypoint
 * and ORs R1 periodically (fire). Camera yaw is not read: it is estimated online from "commanded stick
 * angle vs observed velocity angle", so only the position is needed. It only writes the pad state; it
 * never calls guest code (the cheats that do: tod_cheat.cpp, TOD_CHEAT*).
 *
 * Use (all off by default; nothing runs without TOD_BOT or TOD_BOT_DIAG):
 *   TOD_BOT=<file>        waypoints, one per line: "x y [z] [radius]"  ('#' comments). Enables the bot.
 *   TOD_BOT_BASE=follow   (default) get_pos record (0x90 stride, xyz at +0x30) of the handle the level
 *                         passed to set_follow_cam (g_tod_hero_handle, src/main.cpp tod_fntrace)
 *                 hero    object = [ent+0x40] of the hero entity (resolve_hero; TOD_BOT_POSOFF still a GUESS)
 *                 h:<N>   get_pos record N
 *                 a:<hex> absolute guest address of the object
 *   TOD_BOT_POSOFF=<hex>  offset of the float x,y,z triple in the object (default 0x30)
 *   TOD_BOT_PLANE=xy|xz   ground-plane axes (default xy, Z-up). TOD_BOT_FLIP=1 mirrors handedness.
 *   TOD_BOT_RADIUS=<f>    default arrival radius (default 2.0 world units; tune to level scale)
 *   TOD_BOT_FIRE_MS=<n>   R1 period (default 400, held 120 ms); 0 = never fire
 *   TOD_BOT_DELAY=<s>     seconds before the bot takes over (default 0)
 *   TOD_BOT_LOOP=1        restart at waypoint 0 after the last
 *   TOD_BOT_DIAG=1        diagnostic, each second: the hero chain step by step (handle, entity match),
 *                         entity/object float triples that moved (position candidates), get_pos records that moved
 * Do NOT use PAD_NO_HOST=1: cellPad.c returns before pad_poll_host_input(), the bot would never run.
 * Log prefix: "[bot]" / "[bot-diag]".
 *
 * Guest facts (static analysis of recompiled/*.cpp + ELF):
 *   hero handle = u32 (u64[P+8] & [[0x8885D8]]) >> [[0x8885DC]], P = [0x8885B4] = 0x101AEF80 (func_00024380)
 *   handle -> entity (func_00276D18, TOC 0x89FF20): T = [0x898F3C]: base = [T], count = [T+0x10];
 *          idx = handle & 0xFFFF; ent = base + idx*384; VALID iff u32 [ent+0x64] == handle (ALL 32 bits).
 *          object = [ent+0x40] (hero+0x1C8D is a state byte).
 *          Fixed 2026-10: the bot compared [ent+0x64] with the 16-bit idx, so "hero" and the diag never matched.
 *   Lua get_pos (OPD 0x86C3A8 -> func_002C6DC8 -> func_0027B060): handle h -> rec = [[0x898F40]] + (h&0xFFFF)*0x90;
 *          x,y,z floats at rec+0x30/+0x34/+0x38.  CERTAIN for that table.
 *   hero position offset inside the hero object: NOT determined statically -> use TOD_BOT_DIAG.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <string>
#include <vector>
#include <algorithm>

#include "ppu_recomp.h"        /* vm_read32/vm_read64 (BE accessors), as src/main.cpp */
#include "win32_compat.h"      /* GetTickCount64 */
#include "cellPad.h"           /* CellPadHostInput, CELL_PAD_CTRL_* */
extern "C" unsigned g_tod_hero_handle;   /* src/main.cpp tod_fntrace: from set_follow_cam */
extern "C" uint32_t tod_hero_handle(void); /* tod_cheat.cpp: chain or set_follow_cam, validated as func_00276D18 */

namespace {

struct WP { float x, y, z, r; };
std::vector<WP> s_wp;
size_t   s_cur;
bool     s_on, s_diag, s_loop, s_flip, s_xz;
uint32_t s_posoff = 0x30;
int      s_base_mode = 3;          /* 0 hero chain, 1 handle, 2 absolute, 3 hero handle from set_follow_cam */
uint32_t s_base_arg;
float    s_defr = 2.0f;
int      s_fire_ms = 400;
double   s_delay;
uint64_t s_t0;

/* TOC cells (constant pointers in .data, readable from guest RAM) */
const uint32_t kTOC        = 0x88FF38;
const uint32_t kP_slot     = kTOC - 0x7984;   /* -> 0x101AEF80 */
const uint32_t kMask_slot  = kTOC - 0x7960;   /* -> 0x10027BF0 (u64 mask) */
const uint32_t kShift_slot = kTOC - 0x795C;   /* -> 0x10027BD0 (u64 shift) */
const uint32_t kTab_slot   = 0x89FF20 - 0x6FE4; /* 0x898F3C, the slot func_00276D18 reads -> 0x1064FC80 (+0 base, +0x10 count);
                                                * the old [TOC-0x7958] = 0x8885E0 should hold the same value (diag prints both) */
const uint32_t kHnd_slot   = 0x89FF20 - 0x6FE0; /* Lua-side TOC 0x89FF20: 0x898F40 -> 0x10723AC0 -> base of 0x90-byte records */

float rf(uint32_t a) { uint32_t u = vm_read32(a); float f; memcpy(&f, &u, 4); return f; }

uint32_t resolve_hero()
{
    const uint32_t h = tod_hero_handle();       /* 0 = no valid hero (frontend, loading, empty handle 0xFFFF) */
    if (!h) return 0;
    const uint32_t T = vm_read32(kTab_slot), idx = h & 0xFFFF;
    if (!T || vm_read32(T + 0x10) < idx) return 0;
    const uint32_t ent = vm_read32(T) + idx * 384u;
    if (vm_read32(ent + 0x64) != h) return 0;  /* full 32-bit handle, as func_00276D18 */
    return vm_read32(ent + 0x40);
}

uint32_t resolve_obj()
{
    switch (s_base_mode) {
    case 3: { /* the handle the level passed to set_follow_cam (func_0028BD60, src/main.cpp tod_fntrace) */
              if (!g_tod_hero_handle) return 0;
              uint32_t base = vm_read32(vm_read32(kHnd_slot)), h = g_tod_hero_handle & 0xFFFF;
              return base ? base + ((h << 7) & ~0x7Fu) + ((h << 4) & ~0xFu) : 0; }
    case 1: { uint32_t base = vm_read32(vm_read32(kHnd_slot));
              return base ? base + ((s_base_arg << 7) & ~0x7Fu) + ((s_base_arg << 4) & ~0xFu) : 0; }
    case 2: return s_base_arg;
    default: return resolve_hero();
    }
}

bool read_pos(float* x, float* y, float* z)
{
    uint32_t o = resolve_obj();
    if (!o || (int32_t)o < 0) return false;
    *x = rf(o + s_posoff); *y = rf(o + s_posoff + 4); *z = rf(o + s_posoff + 8);
    return isfinite(*x) && isfinite(*y) && isfinite(*z) && fabsf(*x) < 1e6f && fabsf(*y) < 1e6f && fabsf(*z) < 1e6f;
}

void load_wp(const char* path)
{
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[bot] cannot open %s\n", path); return; }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        float a[4] = {0, 0, 0, 0};
        int n = sscanf(line, "%f %f %f %f", &a[0], &a[1], &a[2], &a[3]);
        if (n < 2) continue;
        s_wp.push_back(WP{ a[0], a[1], n >= 3 ? a[2] : 0.f, n >= 4 ? a[3] : 0.f });
    }
    fclose(f);
    fprintf(stderr, "[bot] %zu waypoints from %s\n", s_wp.size(), path);
}

/* Diagnostic: which words of the object behave like a moving position? */
void diag(double t)
{
    static double next = 0; static uint32_t snap[0x600 / 4]; static bool have = false; static uint32_t lastobj = 0;
    if (t < next) return;
    next = t + 1.0;
    uint32_t o = resolve_obj();
    fprintf(stderr, "[bot-diag] t=%.1f obj=%08X P=%08X\n", t, o, vm_read32(kP_slot));
    { /* the hero chain step by step, to see where it breaks */
      uint32_t P = vm_read32(kP_slot), T = vm_read32(kTab_slot);
      uint64_t mask = vm_read64(vm_read32(kMask_slot)), shift = vm_read64(vm_read32(kShift_slot));
      uint64_t v = P ? ((shift & 64) ? 0 : ((vm_read64(P + 8) & mask) >> (shift & 63))) : 0;
      uint32_t idx = (uint32_t)v & 0xFFFF, ent = T ? vm_read32(T) + idx * 384u : 0;
      fprintf(stderr, "[bot-diag]   chain P+8=%016llX mask=%016llX shift=%llu handle=%08X idx=%u T=%08X (old slot %08X) base=%08X count=%u ent+64=%08X%s ent+40=%08X follow_cam=%08X valid=%08X\n",
              P ? (unsigned long long)vm_read64(P + 8) : 0ull, (unsigned long long)mask, (unsigned long long)shift, (uint32_t)v, idx, T,
              vm_read32(kTOC - 0x7958), T ? vm_read32(T) : 0, T ? vm_read32(T + 0x10) : 0, ent ? vm_read32(ent + 0x64) : 0,
              ent && vm_read32(ent + 0x64) == (uint32_t)v ? " (match)" : " (no match)", ent ? vm_read32(ent + 0x40) : 0,
              g_tod_hero_handle, tod_hero_handle()); }
    { /* entity table: for every valid entry, float triples in the entry (384 B) and in its
       * object [ent+0x40] (first 0x100 B) that moved this second; the per-(entry,offset)
       * count of seconds moved is listed every 10 s -- the player moves almost every second. */
      static std::vector<uint32_t> snapE, snapO; static std::vector<uint16_t> movedE, movedO;
      uint32_t T = vm_read32(kTab_slot), base = T ? vm_read32(T) : 0, cnt = T ? vm_read32(T + 0x10) : 0;
      if (base && cnt && cnt < 8192) {
          const uint32_t WE = 96, WO = 64;
          if (snapE.size() != cnt * WE) { snapE.assign(cnt * WE, 0); snapO.assign(cnt * WO, 0); movedE.assign(cnt * WE, 0); movedO.assign(cnt * WO, 0); }
          auto scan = [&](uint32_t a, uint32_t nw, uint32_t* snap, uint16_t* moved) {
              float cur[96]; bool ch[96];
              for (uint32_t k = 0; k < nw; k++) { uint32_t u = vm_read32(a + k * 4); memcpy(&cur[k], &u, 4);
                  float o; memcpy(&o, &snap[k], 4);
                  ch[k] = isfinite(cur[k]) && fabsf(cur[k]) < 1e4f && fabsf(cur[k]) > 1e-2f && fabsf(cur[k] - o) > 0.05f; snap[k] = u; }
              for (uint32_t k = 0; k + 2 < nw; k++) if (ch[k] && ch[k + 1] && ch[k + 2]) moved[k]++;
          };
          for (uint32_t i = 0; i < cnt; i++) {
              const uint32_t ent = base + i * 384u;
              const uint32_t eh = vm_read32(ent + 0x64);   /* live entry: its own full handle, low half = index */
              if (!eh || (eh & 0xFFFF) != i) continue;
              scan(ent, WE, &snapE[i * WE], &movedE[i * WE]);
              const uint32_t ob = vm_read32(ent + 0x40);
              if (ob && ob < 0xF0000000u) scan(ob, WO, &snapO[i * WO], &movedO[i * WO]);
          }
          static double nextp = 10;
          if (t >= nextp) { nextp = t + 10;
              for (int pass = 0; pass < 2; pass++) {
                  std::vector<std::pair<uint16_t, uint32_t>> top;
                  const auto& mv = pass ? movedO : movedE; const uint32_t W = pass ? WO : WE;
                  for (uint32_t j = 0; j < mv.size(); j++) if (mv[j] >= 3) top.push_back({ mv[j], j });
                  std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
                  fprintf(stderr, "[bot-diag] top moving triples in %s:", pass ? "ent+0x40 obj" : "entity");
                  for (size_t q = 0; q < top.size() && q < 12; q++) {
                      const uint32_t i = top[q].second / W, k = top[q].second % W;
                      const uint32_t a = pass ? vm_read32(base + i * 384u + 0x40) : base + i * 384u;
                      fprintf(stderr, " [e%u +0x%X %us: %.1f %.1f %.1f]", i, k * 4, top[q].first, rf(a + k * 4), rf(a + k * 4 + 4), rf(a + k * 4 + 8));
                  }
                  fprintf(stderr, "\n");
              }
          }
      } }
    { /* get_pos records (0x90 stride, x y z at +0x30): the ones that moved since the last second */
      static float last[4096][3]; static bool lv = false;
      uint32_t base = vm_read32(vm_read32(kHnd_slot));
      if (base && base < 0xF0000000u) {
          int shown = 0;
          for (uint32_t h = 0; h < 4096; h++) {
              uint32_t r = base + h * 0x90u;
              float x = rf(r + 0x30), y = rf(r + 0x34), z = rf(r + 0x38);
              if (!isfinite(x) || !isfinite(y) || !isfinite(z) || fabsf(x) > 1e5f || fabsf(y) > 1e5f || fabsf(z) > 1e5f) { last[h][0] = NAN; continue; }
              float d = lv && isfinite(last[h][0]) ? fabsf(x - last[h][0]) + fabsf(y - last[h][1]) + fabsf(z - last[h][2]) : 0.f;
              if (d > 0.5f && shown < 24) { fprintf(stderr, "[bot-diag]   rec h=%u %.2f %.2f %.2f (moved %.2f)\n", h, x, y, z, d); shown++; }
              last[h][0] = x; last[h][1] = y; last[h][2] = z;
          }
          lv = true;
      } }
    if (!o || (int32_t)o < 0) { have = false; return; }
    if (have && o == lastobj) {
        int ch[0x600 / 4], n = 0;
        for (uint32_t i = 0; i < 0x600 / 4; i++) {
            float a = rf(o + i * 4), b; memcpy(&b, &snap[i], 4);
            if (isfinite(a) && fabsf(a) > 1e-3f && fabsf(a) < 1e6f && fabsf(a - b) > 0.05f) ch[n++] = (int)i * 4;
        }
        for (int i = 0; i + 2 < n; i++)
            if (ch[i + 1] == ch[i] + 4 && ch[i + 2] == ch[i] + 8)
                fprintf(stderr, "[bot-diag]   TRIPLE +0x%X: %.3f %.3f %.3f\n", ch[i],
                        rf(o + ch[i]), rf(o + ch[i] + 4), rf(o + ch[i] + 8));
        fprintf(stderr, "[bot-diag]   changed floats:");
        for (int i = 0; i < n && i < 40; i++) fprintf(stderr, " +%X", ch[i]);
        fprintf(stderr, "\n");
    }
    for (uint32_t i = 0; i < 0x600 / 4; i++) snap[i] = vm_read32(o + i * 4);
    have = true; lastobj = o;
}

float wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

}  // namespace

extern "C" void tod_bot_init(void)
{
    const char* e = getenv("TOD_BOT");
    s_diag = getenv("TOD_BOT_DIAG") != nullptr;
    if (!e && !s_diag) return;
    if (e) load_wp(e);
    s_on = true;
    s_loop = getenv("TOD_BOT_LOOP") != nullptr;
    s_flip = getenv("TOD_BOT_FLIP") != nullptr;
    if (const char* p = getenv("TOD_BOT_PLANE")) s_xz = !strcmp(p, "xz");
    if (const char* p = getenv("TOD_BOT_POSOFF")) s_posoff = (uint32_t)strtoul(p, 0, 16);
    if (const char* p = getenv("TOD_BOT_RADIUS")) s_defr = (float)atof(p);
    if (const char* p = getenv("TOD_BOT_FIRE_MS")) s_fire_ms = atoi(p);
    if (const char* p = getenv("TOD_BOT_DELAY")) s_delay = atof(p);
    if (const char* p = getenv("TOD_BOT_BASE")) {
        if (!strncmp(p, "h:", 2)) { s_base_mode = 1; s_base_arg = (uint32_t)strtoul(p + 2, 0, 10); }
        else if (!strcmp(p, "hero")) s_base_mode = 0;
        else if (!strcmp(p, "follow")) s_base_mode = 3;
        else if (!strncmp(p, "a:", 2)) { s_base_mode = 2; s_base_arg = (uint32_t)strtoul(p + 2, 0, 16); }
    }
    fprintf(stderr, "[bot] on: base=%d posoff=0x%X plane=%s diag=%d\n", s_base_mode, s_posoff, s_xz ? "xz" : "xy", (int)s_diag);
}

/* Called from host_input() AFTER it filled *out. Overwrites the left stick and ORs R1.
 * Returns nonzero when the bot drives. */
extern "C" int tod_bot_apply(CellPadHostInput* out)
{
    if (!s_on) return 0;
    if (!s_t0) s_t0 = GetTickCount64();
    const double t = (GetTickCount64() - s_t0) / 1000.0;
    if (s_diag) diag(t);
    if (t < s_delay || s_wp.empty()) return 0;

    float x, y, z;
    if (!read_pos(&x, &y, &z)) {
        static double lw = -9;
        if (t - lw > 2) { lw = t; fprintf(stderr, "[bot] no hero yet\n"); }
        return 0;
    }
    const float gx = x, gy = s_xz ? z : y;

    if (s_cur >= s_wp.size()) { if (s_loop) s_cur = 0; else return 0; }
    const WP& w = s_wp[s_cur];
    const float wx = w.x, wy = s_xz ? w.z : w.y;
    const float rad = w.r > 0 ? w.r : s_defr;
    const float dx = wx - gx, dy = wy - gy, dist = sqrtf(dx * dx + dy * dy);
    if (dist < rad) {
        fprintf(stderr, "[bot] t=%.1f reached wp %zu (%.1f,%.1f) d=%.2f\n", t, s_cur, wx, wy, dist);
        s_cur++;
        return 0;
    }

    /* camera yaw estimate: theta = angle(observed velocity) - angle(commanded stick) */
    static float theta = 0, lcx = 0, lcy = 0, lastphi = 0; static double lt = -1; static bool moving_cmd = false;
    if (lt < 0) { lcx = gx; lcy = gy; lt = t; }
    else if (t - lt >= 0.15) {
        float vx = gx - lcx, vy = gy - lcy, sp = sqrtf(vx * vx + vy * vy) / (float)(t - lt);
        if (moving_cmd && sp > 0.5f) {
            float meas = wrap(atan2f(vy, vx) - lastphi);
            theta = wrap(theta + 0.35f * wrap(meas - theta));     /* low-pass */
        }
        lcx = gx; lcy = gy; lt = t;
    }

    const float psi = atan2f(dy, dx);                             /* desired world heading */
    const float phi = wrap(psi - theta);                          /* stick angle: 0 = right, +pi/2 = forward */
    const float mag = dist > 3 * rad ? 1.0f : 0.6f + 0.4f * dist / (3 * rad);   /* proportional, >= 0.6 (dead zone) */
    float sx = cosf(phi) * mag; const float sf = sinf(phi) * mag;
    if (s_flip) sx = -sx;
    lastphi = phi; moving_cmd = true;
    out->lx = (uint8_t)(128.f + 127.f * sx);
    out->ly = (uint8_t)(128.f - 127.f * sf);                      /* up = small */

    if (s_fire_ms > 0 && (uint64_t)(t * 1000.0) % (uint64_t)s_fire_ms < 120) out->buttons |= CELL_PAD_CTRL_R1;

    static double lg = -1;
    if (t - lg >= 1.0) {
        lg = t;
        fprintf(stderr, "[bot] t=%.1f pos=(%.2f,%.2f,%.2f) wp%zu=(%.1f,%.1f) d=%.2f theta=%.2f stick=(%u,%u)\n",
                t, x, y, z, s_cur, wx, wy, dist, theta, out->lx, out->ly);
    }
    return 1;
}
