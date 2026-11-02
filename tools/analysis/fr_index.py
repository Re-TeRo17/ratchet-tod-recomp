"""Index lifted functions: callers/callees, timebase users, import-stub (0x83xxxx+) callers.
Usage: .venv\\Scripts\\python.exe -I tools\\analysis\\fr_index.py
Output: out\\analysis\\fr_index.json  (funcs: name -> [file, startline, endline, n_tb, callees])
"""
import re, os, json, glob
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
defre = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{')
callre = re.compile(r'\b(func_[0-9A-F]{8})\(ctx\)')
funcs = {}
for f in sorted(glob.glob(os.path.join(ROOT, 'recompiled', 'ppu_recomp_*.cpp'))):
    cur = None
    with open(f, 'r', encoding='latin-1') as fh:
        for ln, line in enumerate(fh, 1):
            m = defre.match(line)
            if m:
                cur = m.group(1)
                funcs[cur] = [os.path.basename(f), ln, ln, 0, set()]
                continue
            if cur:
                e = funcs[cur]
                e[2] = ln
                if 'ppu_timebase_now' in line:
                    e[3] += line.count('ppu_timebase_now')
                for c in callre.findall(line):
                    if c != cur:
                        e[4].add(c)
for k, v in funcs.items():
    v[4] = sorted(v[4])
out = os.path.join(ROOT, 'out', 'analysis', 'fr_index.json')
json.dump(funcs, open(out, 'w'))
print(len(funcs), 'funcs')
tb = sorted(((v[3], k) for k, v in funcs.items() if v[3]), reverse=True)
print('timebase users:', len(tb))
for n, k in tb[:60]:
    print(k, n, funcs[k][0], funcs[k][1])
