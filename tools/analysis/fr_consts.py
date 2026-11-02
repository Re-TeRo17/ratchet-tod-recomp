"""Find framerate-related float/double constants in EBOOT.ELF and the TOC slots that point at them.
Usage: .venv\\Scripts\\python.exe -I tools\\analysis\\fr_consts.py
Output: out\\analysis\\fr_consts.json / .txt
"""
import struct, json, os
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
d = open(os.path.join(ROOT, 'elf', 'EBOOT.ELF'), 'rb').read()
segs = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'segs.json')))
# (type, flags, off, va, filesz, memsz)
loads = [s for s in segs if s[0] == 1 and s[4] > 0]
TOC = 0x8afe5c


def va2off(va):
    for t, fl, off, v, fs, ms in loads:
        if v <= va < v + fs:
            return off + va - v
    return None


targets32 = {
    '1/30': 1 / 30, '1/60': 1 / 60, '30.0': 30.0, '60.0': 60.0, '29.97': 29.97,
    '59.94': 59.94, '1/29.97': 1 / 29.97, '1/59.94': 1 / 59.94, '0.05': 0.05, '0.1': 0.1,
    '1/20': 1 / 20, '1000.0': 1000.0, '1/120': 1 / 120, '0.033': 0.033, '0.0167': 0.0167,
}
out = []
pats = {}
for name, v in targets32.items():
    pats[('f32', name)] = struct.pack('>f', v)
    pats[('f64', name)] = struct.pack('>d', v)
hits = {}
for (kind, name), pat in pats.items():
    i = 0
    while True:
        i = d.find(pat, i)
        if i < 0:
            break
        # file off -> va
        va = None
        for t, fl, off, v, fs, ms in loads:
            if off <= i < off + fs:
                va = v + i - off
                break
        if va is not None and (kind == 'f64' or va % 4 == 0) and (kind == 'f32' or va % 8 == 0):
            hits.setdefault((kind, name), []).append(va)
        i += 1
# TOC slots: 32-bit BE words in TOC region [TOC-0x8000, TOC+0x8000) equal to hit addresses
toc_off = va2off(TOC - 0x8000)
slots = {}
allhits = {va: (k, n) for (k, n), vs in hits.items() for va in vs}
for s in range(0, 0x10000, 4):
    off = va2off(TOC - 0x8000 + s)
    if off is None:
        continue
    w = struct.unpack('>I', d[off:off + 4])[0]
    if w in allhits:
        slots.setdefault(w, []).append(s - 0x8000)  # signed offset from r2
lines = []
for (k, n), vs in sorted(hits.items()):
    for va in vs:
        lines.append('%s %-8s va=0x%08X toc_offs=%s' % (k, n, va, [('%+#x' % x) for x in slots.get(va, [])]))
os.makedirs(os.path.join(ROOT, 'out', 'analysis'), exist_ok=True)
open(os.path.join(ROOT, 'out', 'analysis', 'fr_consts.txt'), 'w').write('\n'.join(lines))
print('\n'.join(lines))
