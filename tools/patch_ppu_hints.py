#!/usr/bin/env python3
"""Post-lift patch: PPU priority hints `or r31,r31,r31` (guest spin/delay loops) -> PPU_HINT_PAUSE().

The lifter turns `or rX,rX,rX` into `rX = rX | rX;`, which the compiler deletes, so the guest's
delay loops (func_005A6E28 and ~25 siblings: 16/32 hints per loop body) become empty and the
poll loops that call them hammer the polled line and the SMT sibling. Each maximal run of
consecutive hint lines is replaced by ONE PPU_HINT_PAUSE() (first line) + no-ops; the macro
executes g_ppu_hint_pause x _mm_pause (env PPU_HINT_PAUSE, default 0 = unchanged behaviour,
global defined in src/main.cpp). Idempotent. Usage: patch_ppu_hints.py recompiled/ppu_recomp_002.cpp
"""
import re, sys
HDR = ('extern "C" int g_ppu_hint_pause;\n'
       '#define PPU_HINT_PAUSE() do { for (int _i = 0; _i < g_ppu_hint_pause; _i++) __builtin_ia32_pause(); } while (0)\n')
rx = re.compile(r'^(\s*)r(\d+) = r\2 \| r\2;[ \t]*$')
for p in sys.argv[1:]:
    s = open(p, encoding='latin1', newline='').read()
    if 'PPU_HINT_PAUSE' in s:
        print(p, 'already patched'); continue
    nl = '\r\n' if '\r\n' in s else '\n'
    lines = s.split(nl); out = []; prev = False; runs = 0; n = 0
    for l in lines:
        m = rx.match(l)
        if m:
            n += 1
            out.append(m.group(1) + ('/* or-hint */;' if prev else 'PPU_HINT_PAUSE();'))
            if not prev: runs += 1
            prev = True
        else:
            prev = False; out.append(l)
    s2 = nl.join(out)
    k = s2.index('#include "ppu_recomp.h"') + len('#include "ppu_recomp.h"')
    s2 = s2[:k] + nl + HDR.replace('\n', nl).rstrip(nl) + s2[k:]
    open(p, 'w', encoding='latin1', newline='').write(s2)
    print(p, 'patched', n, 'hint lines,', runs, 'runs')
