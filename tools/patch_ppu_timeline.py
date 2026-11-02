#!/usr/bin/env python3
"""Post-lift patch: TOD_TIMELINE hooks in the lifted PPU code (idempotent; ~no cost when g_tl_on == 0).

 - TlScope guard (B/E span with r3 / lr / r4 at entry) at the top of the wait-type guest functions
 - func_005AC298 (poll-loop backoff): tl_ppu_backoff(lr) coalesces the ~100k calls/frame into wait spans
 - func_007E5828 (poll of a job group counter): tl_ppu_poll(r3) remembers the group pointer
Usage: patch_ppu_timeline.py recompiled/ppu_recomp_*.cpp   (only files that define a target are touched)
"""
import re, sys
SCOPE = [0x4A3038,0x4A2920,0x4A3288,0x4A3E20,0x4A3BF8,0x4A52B0,0x63DEF0,0x63DCE8,0x63DBD8,
         0x534EC0,0x48D980,0x614FF0,0x501980,0x505558,
         0x51FBD8,0x51FB68,0x5F0D78,0x5F00C0,0x5CDAC8,0x557C68,0x2F8AE8]
HDR = ('extern "C" volatile int g_tl_on; extern "C" void tl_ppu_ev(int, uint32_t, uint64_t, uint64_t, uint32_t);\n'
       'extern "C" void tl_ppu_backoff(uint64_t, uint32_t); extern "C" void tl_ppu_poll(uint64_t);\n'
       'struct TlScope { uint32_t fn; uint32_t tid; uint8_t on;\n'
       '  inline TlScope(uint32_t f, ppu_context* c) : fn(f), tid((uint32_t)c->thread_id), on((uint8_t)g_tl_on) { if (on) tl_ppu_ev(0, f, c->gpr[3], (c->lr & 0xFFFFFFFFull) | ((uint64_t)(uint32_t)c->gpr[4] << 32), tid); }\n'
       '  inline ~TlScope() { if (on) tl_ppu_ev(1, fn, 0, 0, tid); } };\n')
for p in sys.argv[1:]:
    s = open(p, encoding='latin1', newline='').read()
    if 'TlScope' in s:
        print(p, 'already patched'); continue
    nl = '\r\n' if '\r\n' in s else '\n'
    n = 0
    def hit(addr, extra):
        global s, n
        d = 'void func_%08X(ppu_context* ctx) {' % addr
        i = s.find(d + nl)
        if i < 0: return
        j = i + len(d) + len(nl)
        s = s[:j] + '        ' + extra + nl + s[j:]; n += 1
    for a in SCOPE: hit(a, 'TlScope _tl(0x%X, ctx);' % a)
    hit(0x5AC298, 'if (g_tl_on) tl_ppu_backoff(ctx->lr, (uint32_t)ctx->thread_id);')
    hit(0x7E5828, 'if (g_tl_on) tl_ppu_poll(ctx->gpr[3]);')
    if n:
        k = s.index('#include "ppu_recomp.h"') + len('#include "ppu_recomp.h"')
        s = s[:k] + nl + HDR.replace('\n', nl).rstrip(nl) + s[k:]
        open(p, 'w', encoding='latin1', newline='').write(s)
    print(p, 'patched', n, 'functions')
