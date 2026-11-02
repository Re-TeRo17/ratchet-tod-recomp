"""Functions of TOC base B that load TOC slot SLOT (a struct pointer) and also access field offsets.
Usage: fr_field.py 0x89ff20 0x7498 0x150 0x138 ...   -> per field: functions + how (read32 -> float, write)
Heuristic: same-function co-occurrence of 'r2 + SLOT' and '+ 0xFIELD)' (any register).
"""
import os, sys, re, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fr_toc as t
B = int(sys.argv[1], 0)
slot = int(sys.argv[2], 0)
fields = [int(x, 0) for x in sys.argv[3:]]
slotre = re.compile(r'r2 \+ 0x%X\b' % slot)
fre = {f: re.compile(r'(vm_read32|vm_write32|PPU_RR32|PPU_RW32)\(([^;]*?)r\d+ \+ 0x%X[,)]' % f) for f in fields}
defre = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{')
res = {f: {} for f in fields}
for fn_ in sorted(glob.glob(os.path.join(t.ROOT, 'recompiled', 'ppu_recomp_*.cpp'))):
    cur = None
    has = False
    buf = {}
    def flush():
        if cur and has and t.toc_of(t.faddr(cur)) == B:
            for f, v in buf.items():
                res[f][cur] = v
    for line in open(fn_, 'r', encoding='latin-1'):
        m = defre.match(line)
        if m:
            flush()
            cur = m.group(1); has = False; buf = {}
            continue
        if cur:
            if slotre.search(line):
                has = True
            for f, p in fre.items():
                mm = p.search(line)
                if mm:
                    k = 'W' if 'write' in mm.group(1) or 'RW' in mm.group(1) else 'R'
                    buf.setdefault(f, set()).add(k)
    flush()
for f in fields:
    print('field +0x%X:' % f, ' '.join('%s(%s)' % (k, ''.join(sorted(v))) for k, v in sorted(res[f].items())))

