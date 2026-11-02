"""Track accesses to a global struct (address ADDR) through TOC-loaded pointers.
For every lifted function: when 'rN = vm_read32(r2 + OFF)' loads a TOC slot whose ELF value == ADDR, remember rN
until rN is reassigned; report accesses '(vm_read32|vm_write32|PPU_RR32|PPU_RW32|vm_read8|...)(... rN + K ...)'.
Usage: fr_gfield.py 0x10731D00 0x4 0x8 0x14 [--show]  -> writes out\\analysis\\gfield_<addr>.txt
"""
import os, sys, re, glob, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fr_toc as t
addr = int(sys.argv[1], 0)
offs = [int(x, 0) for x in sys.argv[2:] if not x.startswith('--')]
loadre = re.compile(r'^\s*(r\d+) = (?:vm_read32|PPU_RR32)\((?:\d+, \d+, )?r2 \+ (-?0x[0-9A-F]+)\);')
asg = re.compile(r'^\s*(r\d+) = ')
accre = re.compile(r'(vm_read32|vm_write32|PPU_RR32|PPU_RW32|vm_read8|vm_write8|vm_read16|vm_write16|vm_read64|vm_write64|PPU_RR64|PPU_RW64|PPU_RR8|PPU_RW8)\(([^;]*?)(r\d+) \+ (-?0x[0-9A-Fa-f]+)\b')
defre = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{')
res = {}
for f in sorted(glob.glob(os.path.join(t.ROOT, 'recompiled', 'ppu_recomp_*.cpp'))):
    cur = None
    B = None
    reg = {}
    for ln, line in enumerate(open(f, 'r', encoding='latin-1'), 1):
        m = defre.match(line)
        if m:
            cur = m.group(1); B = t.toc_of(t.faddr(cur)); reg = {}
            continue
        if not cur:
            continue
        if 'loc_' in line[:12]:
            pass  # labels: keep mapping (approximation)
        for mm in accre.finditer(line):
            kind, _, r, k = mm.groups()
            if reg.get(r) and int(k, 0) in offs:
                rw = 'W' if ('write' in kind or 'RW' in kind) else 'R'
                res.setdefault((cur, int(k, 0)), set()).add(rw)
        if 'ctx->lr = 0x' in line or 'lv2_syscall' in line:
            for r in ['r0'] + ['r%d' % i for i in range(3, 13)]:
                reg.pop(r, None)
        m2 = loadre.match(line)
        if m2:
            r, off = m2.groups()
            ws = [t.u32(b + int(off, 0)) for b in ((B,) if '--strict' in sys.argv else (0x88ff38, 0x89ff20, 0x8afe5c))]
            if addr in ws:
                reg[r] = True
            else:
                reg.pop(r, None)
        else:
            m3 = asg.match(line)
            if m3 and m3.group(1) in reg and 'vm_read32(r2' not in line:
                # reassigned (but skip 'rN = rN | rN' copies: treat as a copy keeps the mapping)
                if not re.match(r'^\s*r\d+ = (r\d+) \| \1;', line):
                    reg.pop(m3.group(1), None)
out = os.path.join(t.ROOT, 'out', 'analysis', 'gfield_%X.txt' % addr)
lines = ['%s +0x%X %s' % (fn, k, ''.join(sorted(v))) for (fn, k), v in sorted(res.items())]
open(out, 'w').write('\n'.join(lines))
print(len(lines), 'entries ->', out)
by = {}
for (fn, k), v in res.items():
    by.setdefault(k, []).append('%s(%s)' % (fn, ''.join(sorted(v))))
for k in offs:
    print('+0x%X:' % k, ' '.join(sorted(by.get(k, []))[:80]))

