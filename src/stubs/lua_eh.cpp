/*
 * Lua error handling (setjmp/longjmp) for the lifted PPU code.
 *
 * Why: the engine embeds Lua 5.1. luaD_rawrunprotected = EBOOT 0x671A28
 * (links lj into L->errorJmp @L+0x50, setjmp via PLT 0x59E3D8 -> 0x826790, calls f(L,ud),
 * unlinks), luaD_throw = 0x671AB8 (status -> lj+0x224, longjmp via PLT 0x59DBB8 -> 0x8268B4,
 * else exit(1) at 0x671B1C). The lifted longjmp (0x8268B4) restores r1/lr/r13-r31 and does
 * `blr` = a plain C return to its CALLER, not to the setjmp site, so the first Lua error ever
 * raised (even one the game pcall()s) falls through luaD_throw into
 * "lwz r6,0x10(r3) ... bctrl" with r3 = 1 (null-read at 0x11/0x28, null-call at lr 0x671B14,
 * sys_process_exit(code=1)). Seen in metropolis and cobalia with -level.
 *
 * Fix: natively replace luaD_rawrunprotected (try/catch) and longjmp (C++ throw). Needs the PPU
 * lift redone with   --hook 0x671A28 --hook 0x8268B4 --weak   (ppu_lifter.py: the original
 * bodies become func_XXXXXXXX_lifted, func_XXXXXXXX is then supplied here; --weak is not even
 * needed if the hooked names are just not defined twice -- hooks emit only the _lifted name).
 *
 * Everything the unwind skips must be put back by the catcher: callee-saved GPR/FPR/VR/CR (the
 * lifted callees' epilogues did not run), the guest r1/r2, and the per-thread indirect-call
 * depth counter (ppu_loader.cpp s_icall_depth, manual ++/--).
 */
#include "ppu_recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>

extern "C" {
void ps3_indirect_call(ppu_context* ctx);
int* ps3_ppu_dispatch_depth_slot(void);
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
}
void func_00671A28_lifted(ppu_context* ctx);   /* unused fallback, kept for symmetry */
void func_008268B4_lifted(ppu_context* ctx);

namespace {

struct GuestLongjmp { uint32_t buf; uint64_t val; uint32_t lr; uint32_t sp; };

/* LUA_EH_DEBUG=1: dump every caught Lua error (status, message, top stack slots, guest chain). */
bool lua_eh_debug()
{
    static const int on = [] { const char* e = getenv("LUA_EH_DEBUG"); return (e && *e && *e != '0') ? 1 : 0; }();
    return on != 0;
}

/* TValue (this build) = { int tt; uint32 value } 8 bytes; TString: len @+12, text @+16. */
void dump_tvalue(const char* tag, uint32_t tv)
{
    const int32_t  tt = (int32_t)vm_read32(tv);
    const uint32_t v  = vm_read32(tv + 4);
    if (tt == 4 && v) {
        uint32_t len = vm_read32(v + 12);
        char txt[513];
        uint32_t n = len < 512 ? len : 512;
        for (uint32_t i = 0; i < n; i++) { char c = (char)vm_read8(v + 16 + i); txt[i] = (c == '\n' || c == '\r') ? '|' : c; }
        txt[n] = 0;
        fprintf(stderr, "[lua-eh]   %s @%08X tt=4 str len=%u \"%s\"\n", tag, tv, len, txt);
    } else {
        fprintf(stderr, "[lua-eh]   %s @%08X tt=%d val=0x%08X\n", tag, tv, tt, v);
    }
}

void dump_error(uint32_t L, uint32_t buf, uint32_t status, const GuestLongjmp& e)
{
    const uint32_t top  = vm_read32(L + 8);
    const uint32_t base = vm_read32(L + 12);
    fprintf(stderr, "[lua-eh] caught L=0x%08X buf=0x%08X status=%u L.status=%u top=0x%08X base=0x%08X "
                    "nCcalls=%u ci=0x%08X\n", L, buf, status, vm_read8(L + 6), top, base,
            (unsigned)vm_read16(L + 0x34), vm_read32(L + 0x14));
    if (top >= 0x10000 && top < 0xF0000000u) {
        dump_tvalue("top-1", top - 8);
        for (int i = 2; i <= 6 && top - 8u * i >= base; i++) {
            char tag[16]; snprintf(tag, sizeof tag, "top-%d", i);
            dump_tvalue(tag, top - 8u * i);
        }
    }
    /* guest call chain at the longjmp: lr, then back chain (LR save word @ frame+16, low half @+20) */
    fprintf(stderr, "[lua-eh]   chain: %08X", e.lr);
    uint32_t sp = e.sp;
    for (int i = 0; i < 12 && sp >= 0x10000 && sp < 0xF0000000u; i++) {
        const uint32_t back = vm_read32(sp + 4);   /* 64-bit back chain, low word */
        if (back <= sp) break;
        fprintf(stderr, " %08X", vm_read32(back + 0x14));
        sp = back;
    }
    fprintf(stderr, "\n");
}

/* jmp_buf EAs of the active native setjmp frames on this host thread (innermost last). */
thread_local std::vector<uint32_t> t_frames;

inline uint32_t align16(uint32_t a) { return (a + 0xFu) & ~0xFu; }   /* what 0x826790 does */

struct Saved {
    uint64_t gpr[32];
    double   fpr[32];
    ppu_vr   vr[32];
    uint32_t cr, xer, vrsave;
    void save(const ppu_context* c) {
        memcpy(gpr, c->gpr, sizeof gpr); memcpy(fpr, c->fpr, sizeof fpr); memcpy(vr, c->vr, sizeof vr);
        cr = c->cr; xer = (uint32_t)c->xer; vrsave = c->vrsave;
    }
    void restore_nonvolatile(ppu_context* c) const {
        for (int i = 13; i <= 31; i++) c->gpr[i] = gpr[i];
        for (int i = 14; i <= 31; i++) c->fpr[i] = fpr[i];
        for (int i = 20; i <= 31; i++) c->vr[i] = vr[i];
        c->gpr[1] = gpr[1]; c->gpr[2] = gpr[2];
        c->cr = (c->cr & 0xFF000000u) | (cr & 0x00FFFFFFu);  /* CR2-CR4 are the nonvolatile fields */
        c->vrsave = vrsave;
    }
};

} // namespace

/* int luaD_rawrunprotected(lua_State* L, Pfunc f, void* ud)   r3=L r4=f(OPD ptr) r5=ud -> r3 status */
void func_00671A28(ppu_context* ctx)
{
    const uint32_t L  = (uint32_t)ctx->gpr[3];
    const uint32_t f  = (uint32_t)ctx->gpr[4];
    const uint32_t ud = (uint32_t)ctx->gpr[5];
    const uint64_t sp = ctx->gpr[1];
    const uint64_t lr = ctx->lr;
    const uint32_t nsp = (uint32_t)sp - 0x2B0;            /* same frame the original builds */
    const uint32_t lj  = nsp + 0x70;                      /* struct lua_longjmp { previous; jmp_buf b; ...; status @ +0x224 } */
    const uint32_t buf = align16(lj + 4);

    Saved saved; saved.save(ctx);
    int* depth = ps3_ppu_dispatch_depth_slot();
    const int depth0 = *depth;

    vm_write32(nsp + 0x294, 0);                            /* lj.status = 0 */
    vm_write32(lj, vm_read32(L + 0x50));                   /* lj.previous = L->errorJmp */
    vm_write32(L + 0x50, lj);                              /* L->errorJmp = &lj */

    t_frames.push_back(buf);
    uint32_t status = 0;
    try {
        ctx->gpr[1] = nsp;
        ctx->gpr[3] = L; ctx->gpr[4] = ud;
        ctx->ctr = vm_read32(f);
        ctx->gpr[2] = vm_read32(f + 4);
        ctx->lr = 0x00671A90;
        ps3_indirect_call(ctx);                            /* f(L, ud) */
        status = vm_read32(nsp + 0x294);                   /* normally still 0 */
    } catch (const GuestLongjmp& e) {
        if (e.buf != buf) {                                /* belongs to an outer frame */
            t_frames.pop_back();
            vm_write32(L + 0x50, vm_read32(lj));
            *depth = depth0; g_trampoline_fn = nullptr;
            saved.restore_nonvolatile(ctx);
            ctx->gpr[1] = sp; ctx->lr = lr;
            throw;
        }
        *depth = depth0; g_trampoline_fn = nullptr;
        status = vm_read32(nsp + 0x294);                   /* luaD_throw stored errcode at lj+0x224 */
        if (lua_eh_debug()) dump_error(L, buf, status, e);
    }
    t_frames.pop_back();
    vm_write32(L + 0x50, vm_read32(lj));                   /* L->errorJmp = lj.previous */

    saved.restore_nonvolatile(ctx);
    ctx->gpr[1] = sp;
    ctx->lr = lr;
    ctx->gpr[3] = (uint64_t)(int64_t)(int32_t)status;
}

/* void longjmp(jmp_buf buf, int val)  r3=buf r4=val. Non-local only for buffers a native
 * func_00671A28 frame owns; anything else keeps the lifted (broken-by-design) behaviour. */
void func_008268B4(ppu_context* ctx)
{
    const uint32_t buf = align16((uint32_t)ctx->gpr[3]);
    for (size_t i = t_frames.size(); i-- > 0; ) {
        if (t_frames[i] == buf) {
            static int n;
            if (n < 4) { n++; fprintf(stderr, "[lua-eh] longjmp buf=0x%08X val=%d -> native unwind (%zu frames)\n",
                                      buf, (int)(int32_t)ctx->gpr[4], t_frames.size() - i); }
            throw GuestLongjmp{ buf, ctx->gpr[4], (uint32_t)ctx->lr, (uint32_t)ctx->gpr[1] };
        }
    }
    func_008268B4_lifted(ctx);
}
