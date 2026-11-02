"""Functions that load a given TOC slot (r2 + OFF, OFF may be negative), filtered to those whose TOC base == TOCBASE.
Usage: fr_users.py 0x89ff20 0x78C8 -0x7fac ...
"""
import os, sys, re, glob
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fr_toc as t
B = int(sys.argv[1], 0)
pats = []
for a in sys.argv[2:]:
    v = int(a, 0)
    s = ('-0x%X' % -v) if v < 0 else ('0x%X' % v)
    pats.append((a, re.compile(r'r2 \+ %s\b' % re.escape(s))))
defre = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{')
for f in sorted(glob.glob(os.path.join(t.ROOT, 'recompiled', 'ppu_recomp_*.cpp'))):
    cur = None
    hit = {}
    for line in open(f, 'r', encoding='latin-1'):
        m = defre.match(line)
        if m:
            cur = m.group(1)
            continue
        if cur:
            for a, p in pats:
                if p.search(line):
                    hit[(a, cur)] = hit.get((a, cur), 0) + 1
    for (a, fn), n in sorted(hit.items()):
        if t.toc_of(t.faddr(fn)) == B:
            print(a, fn, n)
