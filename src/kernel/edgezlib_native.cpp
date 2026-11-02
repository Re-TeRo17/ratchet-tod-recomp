/*
 * Edge zlib inflate, completely on the host.
 *
 * Ratchet & Clank ToD streams its packed/game/game.psarc chunks through Sony's
 * Edge zlib SPU task (edgezlib_inflate_task, the only SPU ELF in the EBOOT).
 * The PPU side is edgeZlibAddInflateQueueElement (EBOOT 0x7E3D20, called from
 * 0x616C3C for inflate and 0x61724C for copy), which pushes a 32-byte request
 * on an LFQueue with _cellSpursLFQueuePushBody (its only caller: the `bl` at
 * 0x7E3DE0, so the push returns to lr = 0x7E3DE4). We do the task's work in the
 * push itself and never queue anything.
 *
 * Request (big-endian words, built at r1+0x70 of the 0x7E3D20 frame, read by the
 * task's loop at LS 0x31C8..0x34FC):
 *   +00 compressed EA          +04 output EA
 *   +08 compressed bytes       +0C output bytes       (copy: equal)
 *   +10 work-to-do counter EA | bit 0 (1 inflate, 0 copy)
 *   +14 event flag EA          +18 u16 event flag bits
 *   +1A/+1C: left uninitialised by this build's PPU code and not read by this
 *            task image (no rotqbyi of those bytes), so ignored here.
 * The caller has already stepped over the 2-byte zlib header (it passes
 * stream+2), so the data is a raw deflate stream (optionally followed by the
 * Adler-32).
 *
 * Completion, from the task (0x3494..0x34A8, helper 0xA7F0 = atomic 32-bit
 * decrement returning the OLD value, 0x9308 = cellSpursEventFlagSet): per request
 * the counter is decremented by 1; only when the old value was 1 (it reached 0)
 * is the event flag set with the request's bits. A request without a counter
 * (EA 0) sets the flag unconditionally; without a flag EA nothing is set.
 *
 * TOD_ZLIB_NATIVE=0 disables the handler's work (requests are dropped as before).
 */
#include "ppu_recomp.h"
#include "ps3emu/nid.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <zlib.h>

extern "C" {
typedef void (*hle_ctx_fn)(ppu_context*);
void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
void spurs_ef_set_from_spu(uint32_t flag_ea, uint16_t bits);   /* libs/spurs/cellSpurs.c */
extern uint8_t* vm_base;
}

namespace {

constexpr uint32_t kPushCallerLr = 0x007E3DE4;   /* return address of the bl at 0x7E3DE0 */

std::atomic<unsigned long long> s_n, s_fail, s_bytes_in, s_bytes_out;
std::mutex s_counter_mtx;

bool enabled()
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("TOD_ZLIB_NATIVE");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* Inflate `in` into `out` (capacity `cap`); returns bytes produced or -1. Raw
 * deflate first (what the caller passes), then a zlib-wrapped stream. */
long inflate_any(const uint8_t* in, uint32_t in_size, uint8_t* out, uint32_t cap)
{
    for (int wbits = -15; ; wbits = 15) {
        z_stream z;
        memset(&z, 0, sizeof z);
        if (inflateInit2(&z, wbits) != Z_OK) return -1;
        z.next_in = const_cast<Bytef*>(in);
        z.avail_in = in_size;
        z.next_out = out;
        z.avail_out = cap;
        const int rc = inflate(&z, Z_FINISH);
        const long got = (long)z.total_out;
        inflateEnd(&z);
        if (rc == Z_STREAM_END) return got;
        if (wbits == 15) return -1;
    }
}

/* Italian main menu typo: "NUOVA GIOCO" -> "NUOVO GIOCO" (same length, so no offset
 * moves). The string sits in /built/anark/frontend/anark_loc_text_tags.it.pkg
 * (IGHW, global_cached.psarc entry 44, at 0x18C3 of its single zlib block), which
 * arrives here as one inflated chunk. Replaced only where the exact original bytes
 * are found in an IGHW chunk, so other languages and versions are untouched.
 *   TOD_LOC_FIX=0 disables. */
void loc_fix(uint8_t* p, long n)
{
    static int on = -1;
    if (on < 0) {
        const char* e = getenv("TOD_LOC_FIX");
        on = !(e && e[0] == '0');
    }
    static const char kOld[] = "\0NUOVA GIOCO\0";   /* NUL-delimited record string */
    const size_t len = sizeof kOld - 1;
    if (!on || n < 0x1000 || n > 0x10000 || memcmp(p, "IGHW", 4)) return;
    for (long i = 0; i + (long)len <= n; i++) {
        if (memcmp(p + i, kOld, len)) continue;
        p[i + 1 + 4] = 'O';   /* NUOVA -> NUOVO */
        static std::atomic<bool> s_logged{false};
        if (!s_logged.exchange(true)) fprintf(stderr, "[loc-fix] applied (NUOVA GIOCO -> NUOVO GIOCO, +0x%lX)\n", i + 1);
        return;
    }
}

void push_body(ppu_context* ctx)
{
    const uint32_t q = (uint32_t)ctx->gpr[3];
    const uint32_t src = (uint32_t)ctx->gpr[4];
    const uint32_t lr = (uint32_t)ctx->lr;
    ctx->gpr[3] = 0;   /* CELL_OK */

    static std::atomic<bool> s_unknown_logged{false};
    if (!enabled()) return;
    if (!q || !src || lr != kPushCallerLr) {
        if (!s_unknown_logged.exchange(true))
            fprintf(stderr, "[edgezlib] push to unknown queue q=0x%08X src=0x%08X lr=0x%08X dropped (CELL_OK)\n", q, src, lr);
        return;
    }

    uint32_t w[8];
    for (int i = 0; i < 8; i++) w[i] = vm_read32(src + 4u * i);
    const uint32_t in_ea = w[0], out_ea = w[1], in_size = w[2], out_size = w[3];
    const uint32_t counter_ea = w[4] & ~3u, flag_ea = w[5];
    const bool inflate_mode = (w[4] & 1u) != 0;
    const uint16_t bits = (uint16_t)(w[6] >> 16);

    const unsigned long long n = ++s_n;
    bool ok = true;
    long produced = 0;
    if (in_size > 0x10000 || out_size > 0x10080) ok = false;   /* limits the task enforces */
    else if (inflate_mode) {
        produced = inflate_any(vm_base + in_ea, in_size, vm_base + out_ea, out_size);
        ok = produced >= 0;
        if (ok) loc_fix(vm_base + out_ea, produced);
    } else {
        memmove(vm_base + out_ea, vm_base + in_ea, in_size < out_size ? in_size : out_size);
        produced = (long)(in_size < out_size ? in_size : out_size);
    }
    if (!ok) s_fail++;
    s_bytes_in += in_size;
    s_bytes_out += produced > 0 ? (unsigned long long)produced : 0;

    if (n <= 8 || !ok || n % 500 == 0)
        fprintf(stderr,
                "[edgezlib] #%llu %s in=0x%08X(%u) out=0x%08X(%u) -> %ld%s cnt=0x%08X flag=0x%08X bits=0x%04X lr=0x%08X"
                "  [total %.1f MB -> %.1f MB, %llu failed]\n",
                n, inflate_mode ? "inflate" : "copy", in_ea, in_size, out_ea, out_size, produced,
                ok ? "" : " FAILED (completing anyway)", w[4], flag_ea, bits, lr,
                (double)s_bytes_in.load() / 1048576.0, (double)s_bytes_out.load() / 1048576.0, s_fail.load());

    /* Completion, as the task does it: counter - 1, flag set when it hits 0. */
    bool set_flag = true;
    if (counter_ea) {
        std::lock_guard<std::mutex> g(s_counter_mtx);
        const uint32_t old = vm_read32(counter_ea);
        if (old == 0) {
            static int s_u = 0;
            if (s_u++ < 4) fprintf(stderr, "[edgezlib] counter 0x%08X already 0 (task would assert)\n", counter_ea);
            set_flag = false;
        } else {
            vm_write32(counter_ea, old - 1);
            set_flag = (old == 1);
        }
    }
    if (set_flag && flag_ea) spurs_ef_set_from_spu(flag_ea, bits);
}

}  // namespace

extern "C" void tod_register_edgezlib(void)
{
    ps3_hle_register_ctx(ps3_compute_nid("_cellSpursLFQueuePushBody"), "_cellSpursLFQueuePushBody", push_body);
}
