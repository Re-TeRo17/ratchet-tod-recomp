"""TOC helper. The EBOOT has THREE TOC bases (OPD tocs 0x88ff38, 0x89ff20, 0x8afe5c); a function's r2 is the toc of the
nearest OPD entry whose code address <= the function (approximation, verified on the functions we looked at).
Usage:
  fr_toc.py of func_00501980                  -> toc base for that function
  fr_toc.py val  func_0038F618 0x2630 ...      -> dump TOC slot (r2+off) of that function's toc as u32/float/ptr
  fr_toc.py scanf TOC lo hi                    -> TOC slots whose float value is in [lo,hi] (TOC = 0x88ff38 etc.)
  fr_toc.py fload func_XXXXXXXX                -> every TOC float constant a function loads (lfs via r2+off, value shown)
"""
import struct, os, sys, json, re, bisect
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
d = open(os.path.join(ROOT, 'elf', 'EBOOT.ELF'), 'rb').read()
segs = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'segs.json')))
loads = [s for s in segs if s[0] == 1 and s[4] > 0]


def rd(va, n=4):
    for t, fl, off, v, fs, ms in loads:
        if v <= va and va + n <= v + fs:
            return d[off + va - v: off + va - v + n]
    return None


def u32(va):
    b = rd(va)
    return struct.unpack('>I', b)[0] if b else None


def f32(w):
    return struct.unpack('>f', struct.pack('>I', w))[0]


_opd = None


def opd():
    global _opd
    if _opd is None:
        b = rd(0x859408, 0x2eb30)
        ents = {}
        for i in range(0, len(b) - 8, 8):
            code, toc = struct.unpack('>II', b[i:i + 8])
            if 0x10000 <= code < 0x83c3c8 and toc in (0x88ff38, 0x89ff20, 0x8afe5c):
                ents[code] = toc
        _opd = (sorted(ents), ents)
    return _opd


def toc_of(addr):
    keys, ents = opd()
    i = bisect.bisect_right(keys, addr) - 1
    return ents[keys[i]]


def faddr(name):
    return int(name.split('_')[1], 16) if name.startswith('func_') else int(name, 0)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'of':
        for n in sys.argv[2:]:
            print(n, hex(toc_of(faddr(n))))
    elif cmd == 'val':
        B = toc_of(faddr(sys.argv[2]))
        print('toc', hex(B))
        for a in sys.argv[3:]:
            o = int(a, 0)
            w = u32(B + o)
            extra = ''
            if w is not None and rd(w):
                p = u32(w)
                extra = ' -> [0x%08X]=0x%08X f=%g' % (w, p, f32(p)) if p is not None else ''
            print('r2+0x%X (va 0x%X): %s f=%s%s' % (o, B + o, hex(w) if w is not None else None, '%g' % f32(w) if w is not None else None, extra))
    elif cmd == 'scanf':
        B = int(sys.argv[2], 0)
        lo, hi = float(sys.argv[3]), float(sys.argv[4])
        for s in range(-0x8000, 0x8000, 4):
            w = u32(B + s)
            if w is None:
                continue
            v = f32(w)
            if lo <= v <= hi:
                print('r2%+#x (va 0x%08X): %r' % (s, B + s, v))
    elif cmd == 'fload':
        idx = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'fr_index.json')))
        pat = re.compile(r'vm_read32\(r2 \+ (0x[0-9A-F]+)\); float ftmp|PPU_RR32\(2, \d+, r2 \+ (0x[0-9A-F]+)\); float ftmp')
        for name in sys.argv[2:]:
            fn, s, e = idx[name][0], idx[name][1], idx[name][2]
            B = toc_of(faddr(name))
            seen = {}
            with open(os.path.join(ROOT, 'recompiled', fn), 'r', encoding='latin-1') as fh:
                for ln, line in enumerate(fh, 1):
                    if ln < s:
                        continue
                    if ln > e:
                        break
                    for m in pat.finditer(line):
                        o = int(m.group(1) or m.group(2), 0)
                        w = u32(B + o)
                        seen[o] = f32(w) if w is not None else None
            print(name, hex(B), {hex(k): v for k, v in sorted(seen.items())})
