#!/usr/bin/env python3
"""Insert a tod_fntrace() call at the entry of chosen lifted functions.

Usage: python -I tools/fntrace_patch.py HEX [HEX ...]   (e.g. 0062BB48 00636E30)
Edits recompiled/ppu_recomp_*.cpp in place (idempotent). At run time the hook
prints the first calls of each function when TOD_FNTRACE=1 (see src/main.cpp).
recompiled/ is not versioned: re-run the lift to undo.
"""
import glob, re, sys
want = {a.upper().zfill(8) for a in sys.argv[1:]}
decl = 'extern "C" void tod_fntrace(unsigned, ppu_context*);\n'
for fn in sorted(glob.glob('recompiled/ppu_recomp_[0-9][0-9][0-9].cpp')):
    s = open(fn, encoding='utf-8', errors='surrogateescape', newline='').read()
    n = 0
    for a in want:
        pat = 'void func_%s(ppu_context* ctx) {' % a
        call = ' tod_fntrace(0x%s, ctx);' % a
        if pat in s and (pat + call) not in s:
            s = s.replace(pat, decl + pat + call, 1); n += 1
    if n:
        open(fn, 'w', encoding='utf-8', errors='surrogateescape', newline='').write(s)
        print(fn, n)
