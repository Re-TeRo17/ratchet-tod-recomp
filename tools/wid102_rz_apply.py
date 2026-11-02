#!/usr/bin/env python3
"""wid102: applica a una COPIA di un modulo job SPU lifted (spu_recomp.c) il bracket RZ attorno al ciclo
moltiplicazione-matrici (funzione LS 0x4600 del job SPURS SharedJobList1 / wid1, firma jobmod_9f077839ea6c).

Il TU va compilato con -DSPU_RZ_FP_TLS (CMakeLists.txt: TOD_WID1_RZ). Il bracket imposta, SOLO per la durata del
ciclo, MXCSR = RZ|DAZ|FTZ e g_spu_rz = 1: le operazioni fa/fs/fm/fma diventano le operazioni float dell'host
(bit-esatte per costruzione, vedi spu_helpers.h SPU_RZ_FP_TLS; overflow/exp255 rifanno il percorso esatto).
Env WID102_RZ=0 spegne il bracket (percorso esatto di prima) per A/B.

uso: wid102_rz_apply.py [--check] <spu_recomp.c in> <spu_recomp.c out>   (--check: confronta ogni op RZ col percorso esatto)
      (idempotente; non tocca il file in ingresso)"""
import re, sys

HEAD = r'''
/* ---- wid102: bracket RZ attorno al ciclo 0x4600 (vedi tools/wid102_rz_apply.py) ---- */
#include <immintrin.h>
#include <stdlib.h>
extern SPU_THREAD_LOCAL int g_spu_rz;
static int g_wid102_on = -1;
#define WID102_ENTER() unsigned w102_csr = 0; int w102_act = 0; \
    do { if (__builtin_expect(g_wid102_on < 0, 0)) { const char* e_ = getenv("WID102_RZ"); g_wid102_on = !(e_ && e_[0] == '0'); } \
         if (g_wid102_on && !g_spu_rz) { w102_csr = _mm_getcsr(); _mm_setcsr(0xFFC0u); g_spu_rz = 1; w102_act = 1; } } while (0)
#define WID102_LEAVE() do { if (w102_act) { g_spu_rz = 0; _mm_setcsr(w102_csr); w102_act = 0; } } while (0)
'''

CHECK = r'''
/* ---- wid102 --check: ogni operazione float del TU in modalita' RZ viene confrontata con il percorso ESATTO ---- */
#include <stdio.h>
#include <string.h>
static long g_w102_n, g_w102_bad;
static inline u128 w102_fa_(u128 a, u128 b) { return spu_fa(a, b); }
static inline u128 w102_fs_(u128 a, u128 b) { return spu_fs(a, b); }
static inline u128 w102_fm_(u128 a, u128 b) { return spu_fm(a, b); }
static inline u128 w102_fma_(u128 a, u128 b, u128 c) { return spu_fma(a, b, c); }
static inline u128 w102_chk(int op, u128 r, u128 a, u128 b, u128 c) {
    if (!g_spu_rz) return r;
    u128 e = spu__rz_exact(op, a, b, c, 1);
    long n = __atomic_add_fetch(&g_w102_n, 1, __ATOMIC_RELAXED);
    if (memcmp(&e, &r, 16)) {
        long k = __atomic_fetch_add(&g_w102_bad, 1, __ATOMIC_RELAXED);
        if (k < 20) fprintf(stderr, "[wid102-chk] DIFF op=%d a=%08X %08X %08X %08X b=%08X %08X %08X %08X c=%08X %08X %08X %08X exact=%08X %08X %08X %08X rz=%08X %08X %08X %08X\n", op,
            a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3], c._u32[0], c._u32[1], c._u32[2], c._u32[3],
            e._u32[0], e._u32[1], e._u32[2], e._u32[3], r._u32[0], r._u32[1], r._u32[2], r._u32[3]);
    }
    if ((n & 0xFFFFF) == 0) fprintf(stderr, "[wid102-chk] ops=%ld diff=%ld\n", n, __atomic_load_n(&g_w102_bad, __ATOMIC_RELAXED));
    return r;
}
#undef spu_fa
#undef spu_fs
#undef spu_fm
#undef spu_fma
#define spu_fa(a, b)     w102_chk(0, w102_fa_((a), (b)), (a), (b), (b))
#define spu_fs(a, b)     w102_chk(1, w102_fs_((a), (b)), (a), (b), (b))
#define spu_fm(a, b)     w102_chk(2, w102_fm_((a), (b)), (a), (b), (b))
#define spu_fma(a, b, c) w102_chk(3, w102_fma_((a), (b), (c)), (a), (b), (c))
'''

def main():
    check = '--check' in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    src = open(args[0], encoding='latin1').read()
    if 'WID102_ENTER' in src:
        open(args[1], 'w', encoding='latin1', newline='').write(src); print('gia applicato'); return
    # dopo gli include
    m = re.search(r'#include "spu_helpers.h"\r?\n', src)
    assert m, 'include spu_helpers.h non trovato'
    src = src[:m.end()] + HEAD + (CHECK if check else '') + src[m.end():]
    n = 0
    # versione piana: label subito dopo '{' della funzione 00004600 (non _p)
    pat_plain = re.compile(r'(_spu_func_00004600\(spu_context\* ctx\) \{\r?\n)(loc_00004600:)')
    src, k = pat_plain.subn(r'\1        WID102_ENTER();\n\2', src); n += k
    src, k = re.subn(r'(\n\s*if \(ctx->gpr\[37\]\._u32\[0\] != 0\) goto loc_00004600;)', r'\1\n        WID102_LEAVE();', src); n += k
    # versione _p: label dopo le load dei registri
    pat_p = re.compile(r'(    r87 = ctx->gpr\[87\];\r?\n)(loc_00004600:)')
    src, k = pat_p.subn(r'\1    WID102_ENTER();\n\2', src); n += k
    src, k = re.subn(r'(\n\s*if \(r37\._u32\[0\] != 0\) \{ SPU_LOOP_BARRIER\(\); goto loc_00004600; \})', r'\1\n    WID102_LEAVE();', src); n += k
    assert n == 4, 'attese 4 sostituzioni, fatte %d' % n
    open(args[1], 'w', encoding='latin1', newline='').write(src)
    print('ok, %d sostituzioni' % n)

main()
