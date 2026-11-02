"""List functions that issue time-related lv2 syscalls (141 usleep, 142 sleep, 145/146 get time, 147 tb freq)
and functions containing an immediate 79800000 (timebase freq) load. Output: out\\analysis\\fr_sys.txt
"""
import re, os, glob, json
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
defre = re.compile(r'^void (func_[0-9A-F]{8})\(ppu_context\* ctx\) \{')
r11re = re.compile(r'r11 = \(int64_t\)\(int32_t\)\((0x[0-9A-Fa-f]+|\d+)\);')
want = {141: 'usleep', 142: 'sleep', 145: 'get_current_time', 146: 'get_system_time', 147: 'tb_freq'}
res = {}
tb = []
for f in sorted(glob.glob(os.path.join(ROOT, 'recompiled', 'ppu_recomp_*.cpp'))):
    cur = None
    last11 = None
    for line in open(f, 'r', encoding='latin-1'):
        m = defre.match(line)
        if m:
            cur = m.group(1); last11 = None; continue
        if not cur:
            continue
        for mm in r11re.finditer(line):
            last11 = int(mm.group(1), 0)
        if 'lv2_syscall(ctx)' in line:
            # r11 may be set on same line before the call
            ms = list(r11re.finditer(line))
            n = int(ms[-1].group(1), 0) if ms else last11
            if n in want:
                res.setdefault(want[n], {}).setdefault(cur, 0)
                res[want[n]][cur] += 1
        if '0x4C4B' in line and '0x40' in line:  # lis 0x4C4B; ori 0x400 = 5,000,000? placeholder
            pass
lines = []
for k, v in res.items():
    lines.append('%s: %d funcs: %s' % (k, len(v), ' '.join(sorted(v)[:60])))
open(os.path.join(ROOT, 'out', 'analysis', 'fr_sys.txt'), 'w').write('\n'.join(lines))
print('\n'.join(lines))
