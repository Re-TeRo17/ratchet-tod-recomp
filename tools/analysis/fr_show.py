"""Print a lifted function with the register save/restore boilerplate stripped.
Usage: fr_show.py func_0038F618 [more...]   (needs out\\analysis\\fr_index.json)
"""
import re, os, sys, json
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
idx = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'fr_index.json')))
stubs = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'fr_stubmap.json')))
sv = re.compile(r'ctx->gpr\[\d+\] = [^;]+;|r\d+ = ctx->gpr\[\d+\];|f\d+ = ctx->fpr\[\d+\];|cr\d = \(ctx->cr >> \d+\) & 0xFu;|ctx->cr = \([^;]+;|ctx->fpr\[\d+\] = [^;]+;|DRAIN_TRAMPOLINE\(ctx\);|PPU_RUN\([^)]*\);|PPU_RUN_VARS\(\d+\)|PPU_FRAME_INIT\([^)]*\)|ctr = ctx->ctr;|xca = ctx->xer & 0x20000000u;|r2 = vm_read64\(r1 \+ 0x28\);|uint64_t r\d+ = ctx->gpr\[\d+\];|uint32_t cr\d = [^;]+;|double f\d+ = ctx->fpr\[\d+\];|uint64_t ctr = ctx->ctr;|uint32_t xca = [^;]+;|uint64_t _cs_\d+ = [^;]+;|r\d+ = _cs_\d+;')
for name in sys.argv[1:]:
    fn, s, e = idx[name][0], idx[name][1], idx[name][2]
    print('=== %s (%s:%d-%d) callees=%s' % (name, fn, s, e, [c + ('[' + stubs[c] + ']' if c in stubs else '') for c in idx[name][4]]))
    with open(os.path.join(ROOT, 'recompiled', fn), 'r', encoding='latin-1') as fh:
        for ln, line in enumerate(fh, 1):
            if ln < s:
                continue
            if ln > e:
                break
            t = sv.sub('', line).strip()
            t = re.sub(r'\s+', ' ', t)
            for k, v in stubs.items():
                if k + '(ctx)' in t:
                    t = t.replace(k + '(ctx)', k + '[' + v + '](ctx)')
            if t and not t.startswith('uint64_t _cs_') and 'uint64_t r0' not in t and t != '/* nop */;':
                print('%d: %s' % (ln, t))
