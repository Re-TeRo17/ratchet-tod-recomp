#!/usr/bin/env python3
"""Analisi dei Chrome-Trace prodotti da TOD_TIMELINE (stdlib): scomposizione del PPU main e percorso critico per frame.

Uso: timeline_analyze.py fast.json [slow.json ...] [-v] [--frames]
 - frame = intervallo tra due eventi 'ppuflip' (cellGcmSetFlip del main); fine frame = flip k+1.
 - Il percorso critico si ricostruisce all'indietro dalla fine del frame:
     main in calcolo  -> segmento 'PPU calcolo'
     main in attesa W -> produttore = ultimo PUT di un job SPU (o label del walker per le attese RSX) prima della fine di W;
                         segmento 'latenza' = fine W - istante del produttore; si continua sulla lane del produttore
     SPU in run       -> calcolo del run; se il run ha aspettato una linea (llwait), produttore = ultimo write
                         sulla linea (PPU o altra lane) -> si continua nella sua traccia; se la linea e' poi cambiata
                         'in ritardo' la differenza e' latenza di wake-up/polling
     inizio run       -> se la lane era occupata da un run precedente (gap < 150 us) catena sulla lane
                         (saturazione lane / maxContention), altrimenti 'kick' dal main.
 Tutto in ms. I collegamenti per indirizzo sono esatti solo per le linee GETLLAR (eventi write); per i flag PPU
 (job group / slot) il produttore e' scelto per prossimita' temporale (ultimo PUT prima del fine attesa): IPOTESI.
"""
import json, sys, bisect, collections

WAIT_FN = {0x4A3038: 'flag4A3038', 0x4A2920: 'flag4A2920', 0x4A3288: 'flag4A3288', 0x4A3E20: 'flag4A3E20',
           0x4A3BF8: 'flag4A3BF8', 0x4A52B0: 'flag4A52B0', 0x534EC0: 'spin534EC0', 0x48D980: 'spin48D980', 0x614FF0: 'spin614FF0',
           0x501980: 'rsx501980'}
CTX_FN = {0x63DEF0, 0x63DCE8, 0x63DBD8, 0x51FBD8, 0x51FB68, 0x5F0D78, 0x5F00C0, 0x5CDAC8, 0x557C68, 0x2F8AE8, 0x505558}
U = 1e-3  # us -> ms


def load(path):
    d = json.load(open(path))
    hz = d['tl_meta']['tsc_hz']
    tracks = {}
    ev = []
    for e in d['traceEvents']:
        if e['ph'] == 'M':
            if e['name'] == 'thread_name':
                tracks[e['tid']] = e['args']['name']
            continue
        a = e.get('args', {})
        ev.append(dict(ph=e['ph'], tid=e['tid'], ts=e['ts'] * U, dur=e.get('dur', 0) * U, k=a.get('k'), c=a.get('c', 0),
                       a=a.get('a', 0), b=a.get('b', 0), x=a.get('x', 0)))
    ev.sort(key=lambda e: e['ts'])
    return tracks, ev


def spans(evs, tid, k):
    """B/E nested pairing -> list of (start, end, c, a, b, depth)."""
    out, st = [], []
    for e in evs:
        if e['tid'] != tid or e['k'] != k:
            continue
        if e['ph'] == 'B':
            st.append(e)
        elif e['ph'] == 'E' and st:
            # match the innermost with same c (fn) when possible
            for i in range(len(st) - 1, -1, -1):
                if st[i]['c'] == e['c'] or k != 'ppufn':
                    b = st.pop(i)
                    out.append(dict(s=b['ts'], e=e['ts'], c=b['c'], a=b['a'], b=b['b'], depth=i))
                    break
        elif e['ph'] == 'X':
            out.append(dict(s=e['ts'], e=e['ts'] + e['dur'], c=e['c'], a=e['a'], b=e['b'], depth=0))
    return out


class Trace:
    def __init__(self, path):
        self.path = path
        self.tracks, self.ev = load(path)
        self.t0 = self.ev[0]['ts'] if self.ev else 0
        by = collections.defaultdict(lambda: collections.Counter())
        for e in self.ev:
            by[e['tid']][e['k']] += 1
        # main PPU = PPU track with the most flip events
        flips = [(sum(1 for e in self.ev if e['tid'] == t and e['k'] == 'ppuflip'), t) for t in self.tracks]
        flips = [x for x in flips if x[0]]
        self.main = max(flips)[1] if flips else None
        self.walker = next((t for t, n in self.tracks.items() if n.startswith('RSX walker')), None)
        self.lanes = {t: n for t, n in self.tracks.items() if n.startswith('SPURS')}
        # graphics instance = the SPURS ea with more lanes
        eas = collections.Counter(n.split()[1] for n in self.lanes.values())
        self.gea = eas.most_common(1)[0][0] if eas else None
        self.glanes = {t: n for t, n in self.lanes.items() if self.gea and n.split()[1] == self.gea}
        self.runs = {t: sorted(spans(self.ev, t, 'run'), key=lambda r: r['s']) for t in self.lanes}
        self.llw = {t: sorted(spans(self.ev, t, 'llwait'), key=lambda r: r['s']) for t in self.lanes}
        self._wts = {}
        self.writes = collections.defaultdict(list)       # line -> [(ts, tid, wid, lane, x)]
        self.puts = []                                      # (ts, tid, wid, ea, size)
        self.labels = []                                    # (ts, ea, val)
        self.flips = []
        self.presents = []
        for e in self.ev:
            if e['k'] == 'write':
                self.writes[e['a']].append((e['ts'], e['tid'], e['c'], e['b'] - 1, e['x']))
            elif e['k'] == 'put':
                self.puts.append((e['ts'], e['tid'], e['c'], e['a'], e['b']))
            elif e['k'] == 'label':
                self.labels.append((e['ts'], e['a'], e['b']))
            elif e['k'] == 'ppuflip' and e['tid'] == self.main:
                self.flips.append(e['ts'])
            elif e['k'] == 'present':
                self.presents.append(e['ts'])
        self.put_ts = [p[0] for p in self.puts]
        self.label_ts = [l[0] for l in self.labels]
        # main waits (leaf): poll spans + WAIT_FN spans + usleep
        self.mwait = []
        if self.main:
            for s in spans(self.ev, self.main, 'pollwait'):
                s['cat'] = 'poll'; s['lab'] = 'poll lr=%08X grp=%08X' % (s['c'], s['a'] & 0xFFFFFFFF); self.mwait.append(s)
            for s in self.ppu_spans(self.main):
                if s['c'] in WAIT_FN:
                    s['cat'] = 'rsx' if s['c'] == 0x501980 else 'flag'; s['lab'] = WAIT_FN[s['c']]; self.mwait.append(s)
            for s in self.ppu_spans(self.main):
                if s['c'] & 0x80000000 and (s['c'] & 0xFFFF) not in (141, 142) and s['e'] - s['s'] > 0.01:
                    s['cat'] = 'syscall'; s['lab'] = 'syscall %d lr=%08X' % (s['c'] & 0xFFFF, s['b'] & 0xFFFFFFFF); self.mwait.append(s)
            for s in spans(self.ev, self.main, 'usleep'):
                s['cat'] = 'usleep'; s['lab'] = 'usleep lr=%08X %dus' % (s['c'], s['a']); self.mwait.append(s)
            self.mwait.sort(key=lambda s: s['s'])
            self.mwait = self.leafify(self.mwait)
        self.mw_s = [w['s'] for w in self.mwait]

    def ppu_spans(self, tid):
        if not hasattr(self, '_ps'): self._ps = {}
        if tid not in self._ps: self._ps[tid] = spans(self.ev, tid, 'ppufn')
        return self._ps[tid]

    @staticmethod
    def leafify(ws):
        """remove spans fully inside another wait span (keep the outermost wait; rsx501980 contains usleeps)."""
        out = []
        for w in ws:
            if out and w['s'] >= out[-1]['s'] and w['e'] <= out[-1]['e'] + 1e-6:
                continue
            if out and w['s'] < out[-1]['e']:
                w = dict(w); w['s'] = out[-1]['e']
                if w['e'] <= w['s']:
                    continue
            out.append(w)
        return out

    # ---- helpers
    def run_at(self, tid, t):
        rs = self.runs.get(tid, [])
        i = bisect.bisect_right([r['s'] for r in rs], t + 1e-6) - 1
        if i >= 0 and rs[i]['s'] - 1e-6 <= t <= rs[i]['e'] + 0.05:
            return rs[i], i
        return None, i

    def main_state(self, t):
        """(wait span containing/just ending at t) or None"""
        i = bisect.bisect_left(self.mw_s, t - 1e-9) - 1
        if i >= 0 and self.mwait[i]['s'] < t - 1e-9 and t <= self.mwait[i]['e'] + 1e-6:
            return self.mwait[i]
        return None

    def last_put(self, t, window=0.4, only_graphics=True):
        i = bisect.bisect_right(self.put_ts, t) - 1
        while i >= 0 and t - self.put_ts[i] <= window:
            p = self.puts[i]
            if not only_graphics or p[1] in self.glanes:
                return p
            i -= 1
        return None

    def last_label(self, t, window=1.0):
        i = bisect.bisect_right(self.label_ts, t) - 1
        if i >= 0 and t - self.label_ts[i] <= window:
            return self.labels[i]
        return None

    def writer_of(self, line, t0, t1, waiter_tid):
        """last write to `line` by another thread with ts <= t1 (+2us) and >= t0-0.5 ms"""
        ws = self.writes.get(line)
        if not ws:
            return None
        ts = self._wts.get(line)
        if ts is None:
            ts = self._wts[line] = [w[0] for w in ws]
        i = bisect.bisect_right(ts, t1 + 0.002) - 1
        while i >= 0 and ts[i] >= t0 - 0.5:
            if ws[i][1] != waiter_tid:
                return ws[i]
            i -= 1
        return None

    def puts_between(self, t0, t1):
        i = bisect.bisect_left(self.put_ts, t0); j = bisect.bisect_right(self.put_ts, t1)
        return self.puts[i:j]

    # ---- critical path of one frame (strictly backwards in time; every step either consumes time or ends)
    def critical_path(self, f0, f1):
        segs = []
        def add(lab, a, b):
            if b > a + 1e-9:
                segs.append((lab, b - a))
        t, tid = f1, self.main
        guard = 0
        while t > f0 + 1e-6 and guard < 300:
            guard += 1
            if tid == self.main:
                w = self.main_state(t)
                if w is None:
                    i = bisect.bisect_right(self.mw_s, t) - 1
                    prev_end = f0
                    while i >= 0:
                        if self.mwait[i]['e'] < t - 1e-9:
                            prev_end = max(f0, self.mwait[i]['e']); break
                        i -= 1
                    add('PPU main calcolo', prev_end, t); t = prev_end; continue
                ws = max(w['s'], f0); te = min(t, w['e'])
                nt, tr_, lab = None, None, None
                if w['cat'] in ('rsx', 'usleep'):
                    lb = self.last_label(te, 1.0)
                    if lb and lb[0] > ws:
                        add('latenza wake main (%s)' % w['cat'], lb[0], te)
                        st = self.walker_span(lb[0])
                        s0 = max(f0, st[0]) if st else f0
                        add('RSX walker elabora fifo (fino al label)', s0, lb[0]); t = s0
                        if st: continue
                        break
                # latest PUT inside the wait window from any traced SPU thread
                ps = self.puts_between(ws, te)
                if ps:
                    p = max(ps, key=lambda q: q[0])
                    add('latenza wake main dopo ultimo PUT [%s]' % w['cat'], p[0], te)
                    t, tid = p[0], p[1]
                    continue
                add('ATTESA main %s [nessun PUT SPU nella finestra: produttore PPU/altro]' % w['lab'], ws, te)
                t = ws; continue
            # ---- SPU thread
            run, _ = self.run_at(tid, t)
            nm = self.tracks[tid]; lname = nm.split()[-1] if 'SPURS' in nm else nm
            if run is None:
                add('SPU %s [fuori da un run]' % lname, f0, t); t = f0; continue
            wid = run['c']
            lw = [x for x in self.llw.get(tid, []) if x['s'] >= run['s'] - 1e-6 and x['e'] <= t + 0.01 and x['s'] < t]
            if lw:
                x = max(lw, key=lambda y: y['e'])
                if t - x['e'] > 0.005:
                    add('SPU wid%d %s calcolo (dopo attesa linea)' % (wid, lname), x['e'], t)
                wr = self.writer_of(x['a'], x['s'], x['e'], tid)
                if wr and wr[0] < x['e'] + 1e-9 and wr[0] > f0:
                    add('wid%d %s: latenza polling/wake dopo write L=%08X' % (wid, lname, x['a']), wr[0], x['e'])
                    wn = self.tracks.get(wr[1], '?')
                    if wr[1] == tid or not (wr[1] == self.main or wn.startswith('SPURS') or 'thr' in wn):
                        t = wr[0]; continue
                    t, tid = wr[0], wr[1]
                    if tid != self.main and self.run_at(tid, t)[0] is None and not wn.startswith('SPURS'):
                        add('writer = thread PPU/altro (%s) [fine ricostruzione]' % wn, f0, t); break
                    continue
                add('SPU wid%d %s ATTESA L=%08X [writer ignoto]' % (wid, lname, x['a']), x['s'], x['e'])
                t = x['s']; continue
            s0 = max(run['s'], f0)
            add('SPU wid%d %s calcolo' % (wid, lname), s0, t)
            t = run['s']
            if t <= f0:
                break
            rs = self.runs[tid]; i = rs.index(run)
            prev = rs[i - 1] if i > 0 else None
            if prev is not None and run['s'] - prev['e'] < 0.15:
                add('hand-off lane (wid%d->wid%d %s)' % (prev['c'], wid, lname), prev['e'], run['s'])
                t = prev['e']
            else:
                gap = run['s'] - (prev['e'] if prev else f0)
                t2 = max(f0, run['s'] - 0.08)
                add('kick dal main -> inizio run (lane libera da %.2f ms)' % gap, t2, run['s'])
                t, tid = t2, self.main
        return segs

    def walker_span(self, t):
        best = None
        if not self.walker:
            return None
        st = None
        for e in self.ev:
            if e['tid'] != self.walker or e['k'] != 'fifo':
                continue
            if e['ph'] == 'B' and e['ts'] <= t:
                st = e['ts']
            elif e['ph'] == 'E' and e['ts'] < t:
                st = None
            if e['ts'] > t:
                break
        return (st,) if st is not None else None


def classify_main(tr, f0, f1):
    """ms of the main thread in [f0,f1] per category."""
    cat = collections.Counter()
    sites = collections.Counter()
    tot = f1 - f0
    waited = 0.0
    for w in tr.mwait:
        s, e = max(w['s'], f0), min(w['e'], f1)
        if e > s:
            cat[w['cat']] += e - s; waited += e - s; sites[w['lab']] += e - s
    cat['calcolo'] = tot - waited
    return cat, sites


def lane_stats(tr, f0, f1):
    out = {}
    for t, n in sorted(tr.glanes.items(), key=lambda x: x[1]):
        busy = collections.Counter(); runs = collections.Counter(); wait = 0.0
        for r in tr.runs[t]:
            s, e = max(r['s'], f0), min(r['e'], f1)
            if e > s:
                busy[r['c']] += e - s; runs[r['c']] += 1
        for x in tr.llw[t]:
            s, e = max(x['s'], f0), min(x['e'], f1)
            if e > s:
                wait += e - s
        out[n.split()[-1]] = (sum(busy.values()), wait, dict(busy), dict(runs))
    return out


def report(tr, verbose, perframe):
    print('=' * 100)
    print(tr.path, '| main:', tr.tracks.get(tr.main), '| walker:', tr.tracks.get(tr.walker), '| gfx SPURS ea:', tr.gea)
    nf = len(tr.flips) - 1
    if nf < 1:
        print('troppo pochi flip', len(tr.flips)); return None
    agg_c = collections.Counter(); agg_s = collections.Counter(); agg_p = collections.Counter(); agg_p_n = collections.Counter()
    agg_l = collections.defaultdict(lambda: [0.0, 0.0]); tot = 0.0
    for k in range(nf):
        f0, f1 = tr.flips[k], tr.flips[k + 1]
        d = f1 - f0; tot += d
        cat, sites = classify_main(tr, f0, f1)
        for a, b in cat.items(): agg_c[a] += b
        for a, b in sites.items(): agg_s[a] += b
        ls = lane_stats(tr, f0, f1)
        for ln, (b, w, _, _) in ls.items():
            agg_l[ln][0] += b; agg_l[ln][1] += w
        segs = tr.critical_path(f0, f1)
        sc = collections.Counter()
        for lab, ms in segs:
            key = lab
            agg_p[key] += ms; agg_p_n[key] += 1; sc[key] += ms
        if perframe:
            print('--- frame %d: %.2f ms | main: %s' % (k, d, ' '.join('%s=%.1f' % (a, b) for a, b in sorted(cat.items(), key=lambda x: -x[1]))))
            if verbose:
                for lab, ms in reversed(segs):
                    print('     %6.2f  %s' % (ms, lab))
    print('\nFrame medio: %.2f ms (%d frame, %.1f fps)' % (tot / nf, nf, 1000.0 * nf / tot))
    print('PPU main (ms/frame): ' + ', '.join('%s=%.2f' % (a, b / nf) for a, b in sorted(agg_c.items(), key=lambda x: -x[1])))
    print('Attese del main per sito (ms/frame, prime 12):')
    for a, b in sorted(agg_s.items(), key=lambda x: -x[1])[:12]:
        print('   %6.2f  %s' % (b / nf, a))
    print('Lane grafiche: busy ms/frame (di cui in attesa-linea llwait):')
    for ln, (b, w) in sorted(agg_l.items()):
        print('   %s busy=%.2f  llwait=%.2f' % (ln, b / nf, w / nf))
    print('PERCORSO CRITICO (ms/frame, aggregato per etichetta):')
    tot_p = sum(agg_p.values())
    for a, b in sorted(agg_p.items(), key=lambda x: -x[1])[:25]:
        print('   %6.2f  (%4.1f%%)  %s' % (b / nf, 100 * b / tot_p, a))
    print('   --- somma %.2f ms/frame' % (tot_p / nf))
    # coarse buckets
    buck = collections.Counter()
    for a, b in agg_p.items():
        if a.startswith('PPU main calcolo'): buck['PPU calcolo'] += b
        elif a.startswith('SPU wid') and 'ATTESA' in a: buck['SPU attesa (writer ignoto)'] += b
        elif a.startswith('SPU wid'): buck['SPU calcolo job'] += b
        elif 'polling/wake' in a or 'latenza' in a or 'hand-off' in a or 'kick' in a: buck['latenze wake/hand-off'] += b
        elif 'walker' in a: buck['RSX walker'] += b
        elif a.startswith('wid'): buck['latenze wake/hand-off'] += b
        else: buck['attese senza produttore'] += b
    print('   bucket: ' + ', '.join('%s=%.2f' % (a, b / nf) for a, b in sorted(buck.items(), key=lambda x: -x[1])))
    return dict(fps=1000.0 * nf / tot, main=dict((a, b / nf) for a, b in agg_c.items()), path=dict((a, b / nf) for a, b in agg_p.items()), buck=dict((a, b / nf) for a, b in buck.items()))


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    res = {}
    for p in args:
        res[p] = report(Trace(p), '-v' in sys.argv, '--frames' in sys.argv or '-v' in sys.argv)


# ----------------------------------------------------------------------------------------------------
# Scomposizione delle attese a gruppo job (poll): produttore ESATTO = ultimo write sulla linea del gruppo
# ----------------------------------------------------------------------------------------------------
def poll_decomp(tr):
    """Per ogni attesa poll del main >= 0.1 ms: produttore (ultimo write su linea grp +-1 prima della fine),
    decomposizione  [attesa prima dell'inizio del run produttore] + [run: calcolo | attesa linea llwait] + [latenza finale]."""
    nf = max(1, len(tr.flips) - 1)
    f0, f1 = tr.flips[0], tr.flips[-1]
    agg = collections.defaultdict(float); cnt = collections.Counter()
    unresolved = 0.0
    for w in tr.mwait:
        if w['cat'] != 'poll' or w['e'] < f0 or w['s'] > f1 or w['e'] - w['s'] < 0.1:
            continue
        g = w['a'] & 0xFFFFFFFF
        best = None
        for ln in (g & ~127, (g & ~127) + 128):
            wr = tr.writer_of(ln, w['s'], w['e'], tr.main)
            if wr and w['s'] - 0.3 <= wr[0] <= w['e'] + 0.01 and (best is None or wr[0] > best[0]):
                best = wr
        dur = w['e'] - w['s']
        if not best:
            unresolved += dur; continue
        tn = tr.tracks[best[1]]
        if not tn.startswith('SPURS'):
            agg[('writer %s' % tn[:10], 'attesa')] += dur; continue
        wid = best[2] & 0xFFFF
        run, _ = tr.run_at(best[1], best[0])
        key = 'wid%d' % wid
        if run is None:
            agg[(key, 'run non trovato')] += dur; continue
        pre = max(0.0, min(run['s'], w['e']) - w['s'])                     # job non ancora partito
        lw = sum(max(0.0, min(x['e'], best[0]) - max(x['s'], run['s'], w['s'])) for x in tr.llw.get(best[1], []))
        runpart = max(0.0, best[0] - max(run['s'], w['s']))
        lat = max(0.0, w['e'] - best[0])
        agg[(key, 'prima dell\'inizio del run (coda/lane occupata/kick)')] += pre
        agg[(key, 'run: calcolo')] += max(0.0, runpart - lw)
        agg[(key, 'run: polling linea (llwait)')] += lw
        agg[(key, 'latenza fine write->uscita poll')] += lat
        cnt[key] += 1
    print('  ATTESE POLL del main scomposte per produttore (ms/frame):')
    tot = 0.0
    for (k, c), v in sorted(agg.items(), key=lambda kv: -kv[1]):
        print('     %6.2f  %-8s %s' % (v / nf, k, c)); tot += v
    print('     %6.2f  produttore non risolto' % (unresolved / nf))
    print('     (totale poll risolto %.2f ms/frame; n attese: %s)' % (tot / nf, dict(cnt)))


def flag_producers(tr):
    nf = max(1, len(tr.flips) - 1); f0, f1 = tr.flips[0], tr.flips[-1]
    agg = collections.defaultdict(float); n = collections.Counter()
    for w in tr.mwait:
        if not w['lab'].startswith('flag') or w['e'] < f0 or w['s'] > f1 or w['e'] - w['s'] < 0.1:
            continue
        # ultimo writer di qualunque linea nei 30 us prima della fine, da thread != main
        i = None; best = None
        for ln, ws in tr.writes.items():
            ts = tr._wts.get(ln) or tr._wts.setdefault(ln, [x[0] for x in ws])
            j = bisect.bisect_right(ts, w['e'] + 0.002) - 1
            while j >= 0 and ts[j] > w['e'] - 0.03:
                if ws[j][1] != tr.main and (best is None or ts[j] > best[0]):
                    best = (ts[j], ws[j], ln)
                j -= 1
        k = (tr.tracks[best[1][1]][:14] + ' L=%08X' % best[2]) if best else 'nessuno'
        agg[k] += w['e'] - w['s']; n[k] += 1
    print('  ATTESE FLAG (4A*) del main per ultimo writer non-main nei 30us prima della fine (ms/frame):')
    for k, v in sorted(agg.items(), key=lambda kv: -kv[1])[:6]:
        print('     %6.2f  n=%d  %s' % (v / nf, n[k], k))


if __name__ == '__main__' and '--poll' in sys.argv:
    for p in [a for a in sys.argv[1:] if not a.startswith('-')]:
        tr = Trace(p); print('#', p); poll_decomp(tr); flag_producers(tr)
