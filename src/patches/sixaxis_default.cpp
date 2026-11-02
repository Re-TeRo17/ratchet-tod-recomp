/*
 * SIXAXIS (motion controls) off by default.
 *
 * The Options page's "SIXAXIS CONTROLS" row calls the Lua natives
 * get_six_axis_controls / set_six_axis_controls (func_0002BBB8 / func_0002BC60,
 * OPDs 0x85A498 / 0x85A4A0, names at 0x10003AD8 / 0x10003AC0), which read/write
 * ONE guest byte: [[TOC(0x88FF38) - 0x7914] = 0x101EFB20] + 0x10000 + 0x14D3
 * = 0x10200FF3 (func_00026BC0 reads it, func_00026BD0 writes it; this is the
 * options block: 0x14C8 camera speed, 0x14D0..0x14D5 the toggles). It lives in
 * .bss; the only direct writers in the recompiled code store 0 (func_001FBF90,
 * func_0021DCE0), so the real default comes from the profile/save that fills it.
 *
 * Since the port has no saved profile of its own, a few seconds after the
 * frontend is up (rsx frame counter) this writes the wanted value ONCE. After
 * that the byte is the game's: the user's changes in the Options page stick.
 *
 *   TOD_SIXAXIS = 0 (default: off) | 1 (on)   -- tod.ini / environment
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <thread>

#include "ppu_recomp.h"        /* vm_read8 / vm_write8 */

extern "C" uint32_t g_rsx_engine_frame;   /* rsx_draw_engine.c: present counter */

namespace {

const uint32_t kSixAxisByte = 0x10200FF3;
const uint32_t kWaitFrames  = 300;   /* ~5 s of frontend at 60 fps */

void run()
{
    while (g_rsx_engine_frame < kWaitFrames)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const char* e = getenv("TOD_SIXAXIS");
    const uint8_t want = (e && atoi(e) != 0) ? 1 : 0;
    const uint8_t cur = vm_read8(kSixAxisByte);
    if (cur != want) vm_write8(kSixAxisByte, want);
    fprintf(stderr, "[sixaxis] default: byte [%08X] was %u -> %u (TOD_SIXAXIS=%s)\n",
            kSixAxisByte, (unsigned)cur, (unsigned)want, e ? e : "unset");
}

struct Starter { Starter() { std::thread(run).detach(); } } s_starter;

}  // namespace
