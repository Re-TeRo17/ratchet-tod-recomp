"""Raw PPC scan: stores (stw/stfs/stfd/stb/sth/std) of displacement D through a register that was loaded by
'lwz rA, d(r2)' from a TOC slot whose value == ADDR (any of the 3 TOCs) within the previous N instructions.
Usage: fr_stores.py 0x10731D00 0x14 [N=40]
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fr_toc as t
addr = int(sys.argv[1], 0)
disp = int(sys.argv[2], 0)
N = int(sys.argv[3]) if len(sys.argv) > 3 else 40
seg = t.loads[0]
off, va, fs = seg[2], seg[3], seg[4]
words = struct.unpack('>%dI' % (fs // 4), t.d[off:off + fs])
STORES = {36: 'stw', 52: 'stfs', 54: 'stfd', 38: 'stb', 44: 'sth', 62: 'std', 37: 'stwu'}
tocs = (0x88ff38, 0x89ff20, 0x8afe5c)
out = []
for i, w in enumerate(words):
    op = w >> 26
    if op in STORES:
        d = w & 0xFFFF
        if d == disp:
            ra = (w >> 16) & 31
            for j in range(i - 1, max(0, i - N), -1):
                x = words[j]
                if x >> 26 == 32 and ((x >> 21) & 31) == ra and ((x >> 16) & 31) == 2:
                    dd = x & 0xFFFF
                    if dd & 0x8000:
                        dd -= 0x10000
                    if any(t.u32(b + dd) == addr for b in tocs):
                        out.append((va + 4 * i, STORES[op], ra, hex(va + 4 * j)))
                    break
                # register overwritten -> stop
                if (x >> 26) in (14, 15, 24, 32, 34, 40, 58, 31) and ((x >> 21) & 31) == ra:
                    break
for a, n, ra, ld in out:
    print('0x%08X %s r%d (loaded at %s)' % (a, n, ra, ld))
print(len(out), 'hits')
