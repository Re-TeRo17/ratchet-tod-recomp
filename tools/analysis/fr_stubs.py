"""Map import stubs (func_0083xxxx...) to HLE names and list callers of timing/gcm-related ones.
Needs out\\analysis\\fr_index.json (fr_index.py). Output: out\\analysis\\fr_stubs.txt
"""
import re, os, json, glob
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
idx = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'fr_index.json')))
nids = {}
for m in re.finditer(r'ps3_hle_register\(0x([0-9A-F]+)u, "([^"]+)"', open(os.path.join(ROOT, 'build', 'ppu_hle_nids.cpp')).read()):
    nids[int(m.group(1), 16)] = m.group(2)
stubs = {}
stre = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{\s*$')
nre = re.compile(r'ps3_hle_call\(0x([0-9A-F]+)u')
for f in sorted(glob.glob(os.path.join(ROOT, 'recompiled', 'ppu_recomp_*.cpp'))):
    cur = None
    for line in open(f, 'r', encoding='latin-1'):
        m = stre.match(line)
        if m:
            cur = m.group(1)
            continue
        if cur:
            n = nre.search(line)
            if n and 'import stub' in line:
                stubs[cur] = nids.get(int(n.group(1), 16), 'NID_' + n.group(1))
            cur = None
rev = {}
for k, v in stubs.items():
    rev.setdefault(v, []).append(k)
callers = {}
for fn, e in idx.items():
    for c in e[4]:
        if c in stubs:
            callers.setdefault(stubs[c], []).append(fn)
interest = re.compile(r'Gcm.*(Flip|VBlank|Wait|Label|Time|Display|Queue|Timing)|time|Time|usleep|sleep|Rtc|Tick|Vsync|VideoOut|Event|Semaphore|Cond', re.I)
lines = []
for name in sorted(callers):
    if interest.search(name):
        lines.append('%-48s stub=%s callers(%d)=%s' % (name, ','.join(rev[name]), len(callers[name]), ' '.join(sorted(callers[name])[:25])))
open(os.path.join(ROOT, 'out', 'analysis', 'fr_stubs.txt'), 'w').write('\n'.join(lines))
json.dump(stubs, open(os.path.join(ROOT, 'out', 'analysis', 'fr_stubmap.json'), 'w'))
print('\n'.join(lines))
