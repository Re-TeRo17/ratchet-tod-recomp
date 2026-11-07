#!/usr/bin/env python3
"""merge_cache.py - unisce piu' file pipemap.bin (le mappe dei descrittori di pipeline, SOLO NUMERI) in uno solo.

Uso:
  python -I merge_cache.py -o shaders/pipemap.bin  A/cache/pipemap.bin B/cache/pipemap.bin ...
  python -I merge_cache.py --stats shaders/pipemap.bin          (solo conteggi)
  python -I merge_cache.py -o out.bin  dir1 dir2 ...            (una cartella = cerca pipemap.bin sotto di essa, profondita' <= 4)

La mappa non contiene HLSL, microcodice ne' traduzioni del gioco: per ogni pipeline l'indirizzo del programma nell'immagine EBOOT,
hash di verifica (FNV del microcodice VP, hash strutturale FP), gli ingressi dei decompilatori (maschere texture, funzioni ombra,
dimensioni non normalizzate, texel-op, SSR, layout vertici), stato di render, formati dei target. Il motore, al primo avvio, rigenera
ogni pipeline dalla memoria del gioco dell'utente (vedi rsx_pipemap.inc). Dedup per chiave di pipeline (primi 8 byte del record).

Header (32 byte): magic 'PMAP' u32, version u32, sz_rec u32, sz_rs u32, sz_layout u32, keyrev u32, pad u64.
Record (map_rec): key u64, vp_fnv u64, fp_struct u64, vs_text u64, ps_text u64, ..."""
import os, struct, sys, argparse

HDR = struct.Struct('<IIIIIIQ')
MAGIC = 0x50414D50

def find_files(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            base = a.rstrip('\\/').count(os.sep)
            for root, dirs, files in os.walk(a):
                if root.count(os.sep) - base >= 4:
                    dirs[:] = []
                if 'pipemap.bin' in files:
                    out.append(os.path.join(root, 'pipemap.bin'))
        elif os.path.isfile(a):
            out.append(a)
    return out

def read_map(path):
    d = open(path, 'rb').read()
    if len(d) < HDR.size:
        return None, []
    magic, ver, sz_rec, sz_rs, sz_lay, keyrev, _ = HDR.unpack_from(d, 0)
    if magic != MAGIC:
        return None, []
    n = (len(d) - HDR.size) // sz_rec
    recs = [d[HDR.size + i * sz_rec: HDR.size + (i + 1) * sz_rec] for i in range(n)]
    return (ver, sz_rec, sz_rs, sz_lay, keyrev), recs

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('-o', '--out')
    ap.add_argument('--stats', action='store_true')
    a = ap.parse_args()
    files = find_files(a.inputs)
    if not files:
        print('nessun file pipemap.bin trovato'); return 1
    hdr = None; seen = {}; total = 0
    for f in files:
        h, recs = read_map(f)
        if h is None:
            print('ignorato (formato non valido):', f); continue
        if hdr is None:
            hdr = h
        elif h != hdr:
            print('ignorato (header diverso: %r vs %r): %s' % (h, hdr, f)); continue
        new = 0
        for r in recs:
            k = struct.unpack_from('<Q', r, 0)[0]
            total += 1
            if k not in seen:
                seen[k] = r; new += 1
        print('%-70s %6d descrittori, %5d nuovi' % (f, len(recs), new))
    vps = set(struct.unpack_from('<Q', r, 8)[0] for r in seen.values())
    fps = set(struct.unpack_from('<Q', r, 16)[0] for r in seen.values())
    size = HDR.size + sum(len(r) for r in seen.values())
    print('TOTALE: %d record letti, %d pipeline uniche, %d vertex program distinti, %d fragment program (struttura) distinti, %.2f MB'
          % (total, len(seen), len(vps), len(fps), size / 1048576.0))
    if a.out and not a.stats:
        magic, ver, sz_rec, sz_rs, sz_lay, keyrev = (MAGIC,) + hdr
        with open(a.out, 'wb') as o:
            o.write(HDR.pack(magic, ver, sz_rec, sz_rs, sz_lay, keyrev, 0))
            for k in sorted(seen):
                o.write(seen[k])
        print('scritto', a.out)
    return 0

if __name__ == '__main__':
    sys.exit(main())
