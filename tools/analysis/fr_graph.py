"""Call-graph helper over out\\analysis\\fr_index.json.
  fr_graph.py up FUNC [depth]    -> callers tree (reverse), depth default 4
  fr_graph.py down FUNC [depth]  -> callees tree
  fr_graph.py path A B           -> shortest call path A -> B
"""
import os, sys, json
from collections import deque
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
idx = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'fr_index.json')))
stubs = json.load(open(os.path.join(ROOT, 'out', 'analysis', 'fr_stubmap.json')))
rev = {}
for k, v in idx.items():
    for c in v[4]:
        rev.setdefault(c, []).append(k)


def tag(n):
    e = idx.get(n)
    return '%s%s' % (n, ('[%s]' % stubs[n]) if n in stubs else ('(tb%d,%d lines)' % (e[3], e[2] - e[1]) if e else ''))


def tree(start, depth, graph, seen=None, ind=0):
    seen = seen if seen is not None else set()
    print('  ' * ind + tag(start) + (' ...' if start in seen else ''))
    if start in seen or depth == 0:
        return
    seen.add(start)
    for n in sorted(graph(start))[:14]:
        tree(n, depth - 1, graph, seen, ind + 1)


cmd = sys.argv[1]
if cmd == 'up':
    tree(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 4, lambda n: rev.get(n, []))
elif cmd == 'down':
    tree(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2, lambda n: idx[n][4] if n in idx else [])
elif cmd == 'path':
    a, b = sys.argv[2], sys.argv[3]
    prev = {a: None}
    q = deque([a])
    while q:
        n = q.popleft()
        if n == b:
            break
        for c in idx.get(n, [0, 0, 0, 0, []])[4]:
            if c not in prev:
                prev[c] = n
                q.append(c)
    if b in prev:
        p = []
        n = b
        while n:
            p.append(n)
            n = prev[n]
        print(' -> '.join(tag(x) for x in reversed(p)))
    else:
        print('no path')
