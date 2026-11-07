#!/usr/bin/env python3
"""rz109: generalizza il bracket RZ di wid102 a QUALUNQUE modulo job SPU lifted (spu_recomp.c).

Per ogni funzione (variante piana e `_p`) cerca i cicli (label `loc_X:` + salto all'indietro `goto loc_X;`), tiene quelli
"puri di calcolo" con abbastanza operazioni float e li racchiude in RZ109_ENTER / RZ109_LEAVE:
 - ENTER imposta MXCSR = RZ|DAZ|FTZ (0xFFC0) e g_spu_rz = 1 (solo se non gia' attivo); LEAVE ripristina.
 - il TU va compilato con -DSPU_RZ_FP_TLS (spu_helpers.h): fa/fs/fm/fma/fms/fnms diventano le operazioni float dell'host
   (bit-esatte: RZ+DAZ+FTZ = troncamento SPU con denormali azzerati; esp.255/overflow -> percorso esatto, -0 -> +0).
Ciclo ammesso solo se: ogni istruzione e' un'assegnazione `rN = spu_xxx(...)` (nessun DP: dfa/dfm/dfma/frds/fesd/dfc*..),
 spu_ls_wr/spu_ls_rd, SPU_LOOP_BARRIER, `if (cond) [{ SPU_LOOP_BARRIER(); ] goto L; [}]` con L dentro il ciclo; niente
 return, chiamate, spu_wrch/rdch, SPU_TAIL*, goto fuori dal ciclo (salterebbe LEAVE); nessun salto dall'esterno dentro il ciclo.
Soglia: >= MINOPS operazioni float statiche (default 6).
MODO FUNZIONE (default attivo, --no-func lo spegne): una funzione con >= FMINOPS (default 10) op float, senza chiamate
 (spu_link/spu_drain_call), senza DP, senza canali/stop/salti indiretti, viene racchiusa per intero: ENTER dopo il prologo,
 LEAVE davanti a OGNI uscita (SPU_RET, SPU_RET_REG, SPU_TAIL, SPU_TAILP). Ha la precedenza sui cicli della stessa funzione.
Toggle a runtime: RZ109=0 (tutto spento), RZ109_SKIP=<id[,id]> (id = parte del nome modulo, es. 62d6c136e85b).
--check: ogni operazione float del TU in modalita' RZ e' confrontata col percorso ESATTO (spu__rz_exact): stampa
 [rz109-chk] DIFF ... e ogni 2^20 op "ops=.. diff=..". (anche a fine processo: contatore globale per modulo, atexit)

uso: rz109_apply.py [--check] [--min N] [--survey] <spu_recomp.c in> [out]
  --survey: stampa solo i cicli trovati/scartati (nessun output file)."""
import re, sys, os, collections

DP_RE = re.compile(r'\bspu_(df\w*|frds|fesd|dftsv)\s*\(')
FLOAT_RE = re.compile(r'\bspu_(fa|fs|fm|fma|fms|fnms)\(')
FUNC_START = re.compile(r'^(?:SPU_PN\s+)?void\s+(\w+)\(spu_context\* ctx\)\s*\{\s*$')
LABEL_RE = re.compile(r'^\s*(loc_\w+):\s*$')
GOTO_RE = re.compile(r'goto\s+(loc_\w+)\s*;')
ASSIGN_OK = re.compile(r'^\s*r\d+\s*=\s*spu_\w+\(.*\);\s*$')
STMT_OK = [
    re.compile(r'^\s*spu_ls_wr\(.*\);\s*$'),
    re.compile(r'^\s*SPU_LOOP_BARRIER\(\);\s*$'),
    re.compile(r'^\s*if\s*\(.*\)\s*\{\s*(SPU_LOOP_BARRIER\(\);\s*)?goto\s+loc_\w+;\s*\}\s*$'),
    re.compile(r'^\s*if\s*\(.*\)\s*goto\s+loc_\w+;\s*$'),
    re.compile(r'^\s*goto\s+loc_\w+;\s*$'),
    re.compile(r'^\s*(loc_\w+):\s*$'),
]

HEAD = r'''
/* ---- rz109: bracket RZ attorno ai cicli/funzioni di calcolo (vedi tools/rz109_apply.py). NIENTE -DSPU_RZ_FP_TLS: le operazioni
 * fa/fs/fm/fma/fms/fnms sono ridefinite SOLO dentro le funzioni con bracket; il resto del TU tiene il percorso esatto inline. ---- */
#ifdef SPU_RZ_FP_TLS
#error "rz109: non compilare con SPU_RZ_FP_TLS"
#endif
#include <immintrin.h>
#include <stdlib.h>
#include <string.h>
extern SPU_THREAD_LOCAL int g_spu_rz;
void spu_rz_slow_note(void);
static int g_rz109_on = -1;
static void rz109_init_(void) {
    const char* e_ = getenv("RZ109"); int on = !(e_ && e_[0] == '0');
    const char* s_ = getenv("RZ109_SKIP");
    if (on && s_ && s_[0]) { const char* me_ = "__MODID__"; const char* p_ = s_;
        while (*p_) { const char* q_ = p_; while (*q_ && *q_ != ',' && *q_ != ';') q_++;
            size_t n_ = (size_t)(q_ - p_); if (n_ >= 4 && n_ <= 12 && (size_t)n_ <= strlen(me_)) { char t_[16]; memcpy(t_, p_, n_); t_[n_] = 0; if (strstr(me_, t_)) on = 0; }
            p_ = *q_ ? q_ + 1 : q_; } }
    g_rz109_on = on;
}
#define RZ109_ENTER(K) unsigned rz109_csr##K = 0; int rz109_act##K = 0; \
    do { if (__builtin_expect(g_rz109_on < 0, 0)) rz109_init_(); \
         if (g_rz109_on && !g_spu_rz) { rz109_csr##K = _mm_getcsr(); _mm_setcsr(0xFFC0u); g_spu_rz = 1; rz109_act##K = 1; } } while (0)
#define RZ109_LEAVE(K) do { if (rz109_act##K) { g_spu_rz = 0; _mm_setcsr(rz109_csr##K); rz109_act##K = 0; } } while (0)
/* percorso esatto originale (inline) e versione RZ: stessi bit (spu_helpers.h SPU_RZ_FP_TLS, copiata qui con prefisso rz109_) */
static inline u128 rz109_o_fa(u128 a, u128 b) { return spu_fa(a, b); }
static inline u128 rz109_o_fs(u128 a, u128 b) { return spu_fs(a, b); }
static inline u128 rz109_o_fm(u128 a, u128 b) { return spu_fm(a, b); }
static inline u128 rz109_o_fma(u128 a, u128 b, u128 c) { return spu_fma(a, b, c); }
static inline u128 rz109_o_fms(u128 a, u128 b, u128 c) { return spu_fms(a, b, c); }
static inline u128 rz109_o_fnms(u128 a, u128 b, u128 c) { return spu_fnms(a, b, c); }
/* percorso esatto chiamato da un thread in RZ: MXCSR di default attorno (l'esatto usa double a round-to-nearest) */
static inline __attribute__((noinline, cold)) u128 rz109_redo(int op, u128 a, u128 b, u128 c) {
    spu_rz_slow_note(); unsigned old = _mm_getcsr(); _mm_setcsr(0x1F80u); u128 r;
    switch (op) { case 0: r = rz109_o_fa(a, b); break; case 1: r = rz109_o_fs(a, b); break; case 2: r = rz109_o_fm(a, b); break;
                  case 3: r = rz109_o_fma(a, b, c); break; case 4: r = rz109_o_fms(a, b, c); break; default: r = rz109_o_fnms(a, b, c); break; }
    _mm_setcsr(old); return r;
}
static inline __attribute__((always_inline)) __m128 rz109_ld(u128 a) { return _mm_loadu_ps((const float*)a._u32); }
static inline __attribute__((always_inline)) u128 rz109_fin(__m128 rf, int op, u128 a, u128 b, u128 c) {
    __m128i r = _mm_castps_si128(rf);
    const __m128i t = _mm_and_si128(r, _mm_set1_epi32(0x7FFFFFFF));
    if (__builtin_expect(_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(t, _mm_set1_epi32(0x7F7FFFFE)))) != 0, 0)) {
        u128 h_; _mm_storeu_si128((__m128i*)h_._u32, r);
        return spu__rz_redo_lanes(op, a, b, c, h_);          /* rissa128: only the out-of-range lanes, no MXCSR switch */
    }
    r = _mm_andnot_si128(_mm_cmpeq_epi32(t, _mm_setzero_si128()), r);      /* -0 -> +0 */
    u128 v; _mm_storeu_si128((__m128i*)v._u32, r); return v;
}
@@CHECK@@
#ifndef RZ109_FIN
#define RZ109_FIN(rf, op, a, b, c) rz109_fin((rf), (op), (a), (b), (c))
#endif
#define RZ109_FA(a, b)      (__builtin_expect(g_spu_rz, 1) ? RZ109_FIN(_mm_add_ps(rz109_ld(a), rz109_ld(b)), 0, (a), (b), (b)) : rz109_o_fa((a), (b)))
#define RZ109_FS(a, b)      (__builtin_expect(g_spu_rz, 1) ? RZ109_FIN(_mm_sub_ps(rz109_ld(a), rz109_ld(b)), 1, (a), (b), (b)) : rz109_o_fs((a), (b)))
#define RZ109_FM(a, b)      (__builtin_expect(g_spu_rz, 1) ? RZ109_FIN(_mm_mul_ps(rz109_ld(a), rz109_ld(b)), 2, (a), (b), (b)) : rz109_o_fm((a), (b)))
#define RZ109_FMA(a, b, c)  (__builtin_expect(g_spu_rz, 1) ? RZ109_FIN(_mm_fmadd_ps(rz109_ld(a), rz109_ld(b), rz109_ld(c)), 3, (a), (b), (c)) : rz109_o_fma((a), (b), (c)))
#define RZ109_FMS(a, b, c)  (__builtin_expect(g_spu_rz, 1) ? RZ109_FIN(_mm_fmsub_ps(rz109_ld(a), rz109_ld(b), rz109_ld(c)), 4, (a), (b), (c)) : rz109_o_fms((a), (b), (c)))
#define RZ109_FNMS(a, b, c) (__builtin_expect(g_spu_rz, 1) ? RZ109_FIN(_mm_fnmadd_ps(rz109_ld(a), rz109_ld(b), rz109_ld(c)), 5, (a), (b), (c)) : rz109_o_fnms((a), (b), (c)))
'''

USE_BLOCK = '''#define spu_fa(a, b)      RZ109_FA(a, b)
#define spu_fs(a, b)      RZ109_FS(a, b)
#define spu_fm(a, b)      RZ109_FM(a, b)
#define spu_fma(a, b, c)  RZ109_FMA(a, b, c)
#define spu_fms(a, b, c)  RZ109_FMS(a, b, c)
#define spu_fnms(a, b, c) RZ109_FNMS(a, b, c)'''
UNUSE_BLOCK = '''#undef spu_fa
#undef spu_fs
#undef spu_fm
#undef spu_fma
#undef spu_fms
#undef spu_fnms'''

CHECK = r'''
/* ---- rz109 --check: ogni operazione float in modalita' RZ e' confrontata col percorso ESATTO (rz109_redo) ---- */
#include <stdio.h>
static long g_rz109_n, g_rz109_bad;
static void rz109_atexit_(void) { fprintf(stderr, "[rz109-chk] __MODID__ ops=%ld diff=%ld\n", g_rz109_n, g_rz109_bad); }
static inline __attribute__((noinline)) u128 rz109_chk(int op, __m128 rf, u128 a, u128 b, u128 c) {
    u128 r = rz109_fin(rf, op, a, b, c);      /* esegue anche il redo esatto se fuori intervallo (allora il confronto e' banale) */
    u128 e = rz109_redo(op, a, b, c);
    long n = __atomic_add_fetch(&g_rz109_n, 1, __ATOMIC_RELAXED);
    if (__builtin_expect(n == 1, 0)) atexit(rz109_atexit_);
    if (memcmp(&e, &r, 16)) {
        long k = __atomic_fetch_add(&g_rz109_bad, 1, __ATOMIC_RELAXED);
        if (k < 20) fprintf(stderr, "[rz109-chk] DIFF __MODID__ op=%d a=%08X %08X %08X %08X b=%08X %08X %08X %08X c=%08X %08X %08X %08X exact=%08X %08X %08X %08X rz=%08X %08X %08X %08X\n", op,
            a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], c._u32[0], c._u32[1], c._u32[2], c._u32[3],
            e._u32[0], e._u32[1], e._u32[2], e._u32[3], r._u32[0], r._u32[1], r._u32[2], r._u32[3]);
    }
    if ((n & 0x3FFFF) == 0) fprintf(stderr, "[rz109-chk] __MODID__ ops=%ld diff=%ld\n", n, __atomic_load_n(&g_rz109_bad, __ATOMIC_RELAXED));
    return r;
}
#define RZ109_FIN(rf, op, a, b, c) rz109_chk((op), (rf), (a), (b), (c))
'''


def split_funcs(lines):
    """-> lista di (nome, inizio_riga, fine_riga) (la riga di fine e' la `}` a colonna 0)"""
    out = []
    i = 0
    n = len(lines)
    while i < n:
        m = FUNC_START.match(lines[i])
        if m:
            j = i + 1
            while j < n and not lines[j].startswith('}'):
                j += 1
            out.append((m.group(1), i, j))
            i = j
        i += 1
    return out


def analyse_function(lines, f0, f1, minops):
    """-> (loops_ok, loops_rejected). loops_ok = lista (a, b, nflt) con a = riga label, b = riga dell'ultimo goto all'indietro."""
    labels = {}
    for i in range(f0, f1):
        m = LABEL_RE.match(lines[i])
        if m:
            labels[m.group(1)] = i
    gotos = []   # (riga, label)
    for i in range(f0, f1):
        for m in GOTO_RE.finditer(lines[i]):
            gotos.append((i, m.group(1)))
    back = collections.defaultdict(list)
    for (i, L) in gotos:
        if L in labels and labels[L] < i:
            back[L].append(i)
    cand = []
    for L, bl in back.items():
        a = labels[L]
        b = max(bl)
        cand.append((a, b, L))
    cand.sort(key=lambda t: (t[0], -t[1]))
    ok, rej = [], []
    for (a, b, L) in cand:
        reason = None
        nf = 0
        for i in range(a, b + 1):
            s = lines[i]
            if FLOAT_RE.search(s):
                nf += len(FLOAT_RE.findall(s))
            if DP_RE.search(s):
                reason = 'DP:' + s.strip()[:60]; break
            if ASSIGN_OK.match(s):
                # assegnazioni: nessuna chiamata a funzioni non spu_ (es. spu_wrch e' un'istruzione, non un'assegnazione)
                continue
            if any(p.match(s) for p in STMT_OK):
                continue
            if not s.strip():
                continue
            reason = 'stmt:' + s.strip()[:70]; break
        if reason is None:
            inside = set(l for l, ln in labels.items() if a <= ln <= b)
            for (i, T) in gotos:
                if a <= i <= b and T not in inside:
                    reason = 'goto fuori: ' + T; break
                if not (a <= i <= b) and T in inside:
                    reason = 'salto dall esterno in ' + T; break
        if reason is None and nf < minops:
            reason = 'pochi float (%d)' % nf
        (rej if reason else ok).append((a, b, nf) if not reason else (a, b, nf, reason))
    # tieni solo il piu' esterno tra i cicli ok annidati
    ok.sort(key=lambda t: (t[0], -t[1]))
    final = []
    for t in ok:
        if final and t[0] >= final[-1][0] and t[1] <= final[-1][1]:
            continue
        final.append(t)
    return final, rej


DENY_FUNC = re.compile(r'\b(spu_link|spu_drain_call|spu_img_restore|spu_indirect_branch|SPU_TAIL_IBP?|spu_stop|spu_wrch|spu_rdch|spu_rchcnt|'
                       r'spu_df\w*|spu_frds|spu_fesd|spu_dftsv|host_depth|stop_code|spu_halt|spu_sync|spu_syncc|spu_dsync)\b')
EXIT_RE = re.compile(r'\bSPU_(TAILP|TAIL|RET_REG|RET)\(')


def func_mode(lines, f0, f1, fminops):
    """-> (ok, motivo). ok = (riga_ins_ENTER, [riga uscite], nflt)"""
    nf = 0
    enter_at = f0 + 1
    exits = []
    for i in range(f0 + 1, f1):
        s = lines[i]
        nf += len(FLOAT_RE.findall(s))
        if 'spu_pcheck_enter' in s:
            enter_at = i + 1
            continue
        if DENY_FUNC.search(s):
            return None, 'vieta:' + DENY_FUNC.search(s).group(1)
        if re.match(r'\s*return\s*;', s) or re.search(r'[;{}\s]return\s*;', s):
            return None, 'return nudo'
        if EXIT_RE.search(s):
            exits.append(i)
        if re.match(r'\s*uint8_t\* const ls = ctx->ls;', s):
            enter_at = i + 1
    if nf < fminops:
        return None, 'pochi float (%d)' % nf
    if enter_at is None:
        return None, 'nessun punto ENTER'
    if not exits:
        return None, 'nessuna uscita'
    return (enter_at, exits, nf), None


def wrap_exits(line, K):
    out = []; pos = 0
    for m in EXIT_RE.finditer(line):
        if m.start() < pos:
            continue
        j = m.end(); depth = 1
        while j < len(line) and depth:
            depth += (line[j] == '(') - (line[j] == ')'); j += 1
        k = j
        while k < len(line) and line[k] in ' 	': k += 1
        if k < len(line) and line[k] == ';':
            j = k + 1
        else:
            return None
        out.append(line[pos:m.start()]); out.append('do { RZ109_LEAVE(%d); %s } while (0);' % (K, line[m.start():j]))
        pos = j
    out.append(line[pos:])
    return ''.join(out)


def main():
    args = [a for a in sys.argv[1:]]
    check = '--check' in args
    survey = '--survey' in args
    minops = 6
    fminops = 10
    nofunc = '--no-func' in args
    if '--fmin' in args:
        k = args.index('--fmin'); fminops = int(args[k + 1]); del args[k:k + 2]
    if '--min' in args:
        k = args.index('--min'); minops = int(args[k + 1]); del args[k:k + 2]
    args = [a for a in args if not a.startswith('--')]
    src = open(args[0], encoding='latin1').read()
    modid = os.path.basename(os.path.dirname(os.path.abspath(args[0]))).replace('jobmod_', '')
    if 'RZ109_ENTER' in src:
        if not survey:
            open(args[1], 'w', encoding='latin1', newline='').write(src)
        print('gia applicato'); return
    nl = '\r\n' if '\r\n' in src else '\n'
    lines = src.split('\n')
    lines = [l.rstrip('\r') for l in lines]
    funcs = split_funcs(lines)
    ins = {}   # riga -> testo da inserire PRIMA
    aft = {}   # riga -> testo da inserire DOPO
    K = 0
    tot_ok = tot_rej = 0
    nfl_total = 0
    repl = {}
    wrapf = set()
    nfun_ok = 0
    for (name, f0, f1) in funcs:
        if not nofunc:
            fm, why = func_mode(lines, f0, f1, fminops)
            if fm:
                K += 1; nfun_ok += 1; wrapf.add((f0, f1))
                enter_at, exits, nf = fm
                ins.setdefault(enter_at, []).append('    RZ109_ENTER(%d);' % K)
                bad = False
                for ei in exits:
                    w = wrap_exits(lines[ei], K)
                    if w is None: bad = True; break
                    repl[ei] = w
                if bad:
                    raise SystemExit('uscita non racchiudibile in %s riga %d' % (name, ei + 1))
                nfl_total += nf
                print('FUN %s lines %d-%d floatops=%d exits=%d' % (name, f0 + 1, f1 + 1, nf, len(exits)))
                continue
            elif why and not why.startswith('pochi'):
                nf2 = sum(len(FLOAT_RE.findall(lines[i])) for i in range(f0, f1))
                if nf2 >= fminops:
                    print('FREJ %s floatops=%d  %s' % (name, nf2, why))
        ok, rej = analyse_function(lines, f0, f1, minops)
        for t in ok:
            K += 1; wrapf.add((f0, f1))
            ins.setdefault(t[0], []).append('    RZ109_ENTER(%d);' % K)
            aft.setdefault(t[1], []).append('    RZ109_LEAVE(%d);' % K)
            nfl_total += t[2]
            print('OK  %s lines %d-%d floatops=%d' % (name, t[0] + 1, t[1] + 1, t[2]))
        for t in rej:
            if t[2] >= minops:
                print('REJ %s lines %d-%d floatops=%d  %s' % (name, t[0] + 1, t[1] + 1, t[2], t[3]))
        tot_ok += len(ok); tot_rej += len([t for t in rej if t[2] >= minops])
    print('%s: %d funzioni, funzioni intere ok=%d, cicli ok=%d (float statici %d), scartati con >=%d float=%d' % (modid, len(funcs), nfun_ok, tot_ok, nfl_total, minops, tot_rej))
    if survey or (tot_ok == 0 and nfun_ok == 0):
        if tot_ok == 0 and nfun_ok == 0 and not survey:
            print('nessun ciclo: file NON scritto')
        return
    # un ciclo deve avere il LEAVE dopo l'istruzione di salto all'indietro; se la riga e' `if (c) {..goto..}` il LEAVE sta dopo.
    out = []
    starts = {a: b for (a, b) in wrapf}
    ends = set(b for (a, b) in wrapf)
    for i, l in enumerate(lines):
        if i in repl:
            l = repl[i]
        if i in starts:
            out.extend(USE_BLOCK.splitlines())
        if i in ins:
            out.extend(ins[i])
        out.append(l)
        if i in aft:
            out.extend(aft[i])
        if i in ends:
            out.extend(UNUSE_BLOCK.splitlines())
    text = nl.join(out)
    m = re.search(r'#include "spu_helpers.h"\r?\n', text)
    assert m, 'include spu_helpers.h non trovato'
    pre = HEAD.replace('@@CHECK@@', CHECK if check else '').replace('__MODID__', modid).replace(chr(10), nl)
    text = text[:m.end()] + pre + text[m.end():]
    open(args[1], 'w', encoding='latin1', newline='').write(text)
    print('ok: scritto', args[1], '(%d bracket)' % K)


main()
