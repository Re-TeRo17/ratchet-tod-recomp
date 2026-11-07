#!/usr/bin/env python3
"""Rewrite a PSARC (zlib) with some entries replaced -- stdlib only, input untouched.

Why: PS3_VFS_OVERLAY (sys_fs.c ps3_vfs_overlay) works on whole FILES; the game opens
global_cached.psarc as one file and inflates the entries itself, so an entry cannot be
replaced on its own. Serving a repacked archive through the overlay is the whole mechanism.

Usage:
  python -I psarc_repack.py SRC.psarc OUT.psarc INDEX=FILE [INDEX=FILE ...]
  python -I psarc_repack.py --inplace SRC.psarc OUT.psarc INDEX=FILE ...
      (menu104 "in place" variant: the TOC and the block-size table keep their length, no entry moves;
       a replaced entry reuses its own block slots (it may not need more blocks than it had), its data
       is appended at the end of the file and only its offset/size change in the TOC. Entries whose new
       data needs fewer blocks leave the spare slots at 0. No-change file with INDEX=@same keeps the data.)
  python -I psarc_repack.py --overlay OVERLAYDIR SRC.psarc INDEX=FILE ...
      (writes OVERLAYDIR/PS3_GAME/USRDIR/packed/game/<name>.psarc: set
       PS3_VFS_OVERLAY=OVERLAYDIR next to PS3_VFS_ROOT=game\\disc)
INDEX is the 1-based manifest index (out/psarc/<name>.manifest.txt) = the TOC entry number.

Method: the block-size table grows by the new entries' blocks (appended at its end), so the
TOC is longer by that much; every old entry's offset moves by the same amount, the old
data is copied verbatim, the new entries' blocks go at the end of the file. The replaced
entries' old blocks stay in the file as dead data. Nothing is recompressed but the new files.
"""
import os
import struct
import sys
import zlib

MB = 1024 * 1024


def be(b):
    return int.from_bytes(b, "big")


def put(v, n):
    return int(v).to_bytes(n, "big")


def write_inplace(f, out, hdr, toc, tbl, ents, planned, esz, n, bs, w, toc_len, file_size):
    """Same file layout, same TOC length: replaced entries reuse their block slots, data goes at the end."""
    new_tbl = bytearray(tbl)
    toc = bytearray(toc)
    pos = file_size
    blobs = []
    for idx, usize, blocks, chunks in planned:
        b0, old_us, _off = ents[idx]
        old_nb = (old_us + bs - 1) // bs
        assert len(blocks) <= old_nb, "entry %d needs %d blocks, had %d (not in place)" % (idx, len(blocks), old_nb)
        for k in range(old_nb):
            new_tbl[(b0 + k) * w:(b0 + k + 1) * w] = put(blocks[k] if k < len(blocks) else 0, w)
        e = bytearray(toc[idx * esz:(idx + 1) * esz])
        e[20:25] = put(usize, 5)
        e[25:30] = put(pos, 5)
        toc[idx * esz:(idx + 1) * esz] = e
        blob = b"".join(chunks)
        blobs.append(blob)
        pos += len(blob)
    with open(out, "wb") as o:
        o.write(hdr)
        o.write(toc)
        o.write(new_tbl)
        f.seek(toc_len)
        while True:
            buf = f.read(8 * MB)
            if not buf:
                break
            o.write(buf)
        for blob in blobs:
            o.write(blob)
    print("wrote %s  (%d bytes, %d entries replaced IN PLACE, TOC unchanged)" % (out, os.path.getsize(out), len(planned)))
    return 0


def main(argv):
    overlay = None
    inplace = False
    if argv and argv[0] == "--inplace":
        inplace = True
        argv = argv[1:]
    if argv and argv[0] == "--overlay":
        overlay = argv[1]
        argv = argv[2:]
    if len(argv) < 2:
        print(__doc__)
        return 2
    src = argv[0]
    if overlay:
        out = os.path.join(overlay, "PS3_GAME", "USRDIR", "packed", "game", os.path.basename(src))
        os.makedirs(os.path.dirname(out), exist_ok=True)
        rest = argv[1:]
    else:
        out, rest = argv[1], argv[2:]
    repl = {}
    for a in rest:
        i, p = a.split("=", 1)
        repl[int(i)] = p
    with open(src, "rb") as f:
        hdr = f.read(32)
        assert hdr[:4] == b"PSAR" and hdr[8:12] == b"zlib", "not a zlib PSARC"
        toc_len, esz, n, bs = be(hdr[12:16]), be(hdr[16:20]), be(hdr[20:24]), be(hdr[24:28])
        w = 2 if bs <= 65536 else (3 if bs <= 16 * MB else 4)
        f.seek(32)
        toc = bytearray(f.read(n * esz))
        tbl = f.read(toc_len - 32 - n * esz)
        ents = [(be(toc[i * esz + 16:i * esz + 20]), be(toc[i * esz + 20:i * esz + 25]),
                 be(toc[i * esz + 25:i * esz + 30])) for i in range(n)]
        assert min(e[2] for e in ents if e[1]) >= toc_len, "data starts before the end of the TOC?"
        nblocks = len(tbl) // w
        new_tbl = bytearray(tbl)
        new_data = []                      # (bytes) appended after the old data, in order
        patches = {}
        file_size = os.path.getsize(src)
        # first pass: compress the replacements (sizes decide the new TOC length)
        add_total = 0
        planned = []
        for idx, path in sorted(repl.items()):
            assert 0 < idx < n, "index out of range"
            raw = open(path, "rb").read()
            blocks, chunks = [], []
            for o in range(0, max(len(raw), 1), bs):
                c = raw[o:o + bs]
                z = zlib.compress(c, 9)
                if len(z) >= len(c):
                    z = c                  # stored
                chunks.append(z)
                blocks.append(len(z) % 65536 if (w == 2 and len(z) == 65536) else len(z))
            planned.append((idx, len(raw), blocks, chunks))
        if inplace:
            return write_inplace(f, out, hdr, toc, tbl, ents, planned, esz, n, bs, w, toc_len, file_size)
        shift = w * sum(len(p[2]) for p in planned)
        pos = file_size + shift            # where the first new entry's data will start
        bi = nblocks
        for idx, usize, blocks, chunks in planned:
            patches[idx] = (bi, usize, pos)
            for b in blocks:
                new_tbl += put(b, w)
            bi += len(blocks)
            blob = b"".join(chunks)
            new_data.append(blob)
            pos += len(blob)
        new_toc_len = toc_len + shift
        for i in range(n):
            e = bytearray(toc[i * esz:(i + 1) * esz])
            if i in patches:
                b0, us, off = patches[i]
            else:
                b0, us, off = ents[i]
                if us:
                    off += shift
            e[16:20] = put(b0, 4)
            e[20:25] = put(us, 5)
            e[25:30] = put(off, 5)
            toc[i * esz:(i + 1) * esz] = e
        nh = bytearray(hdr)
        nh[12:16] = put(new_toc_len, 4)
        with open(out, "wb") as o:
            o.write(nh)
            o.write(toc)
            o.write(new_tbl)
            f.seek(toc_len)
            while True:
                buf = f.read(8 * MB)
                if not buf:
                    break
                o.write(buf)
            for blob in new_data:
                o.write(blob)
    print("wrote %s  (%d bytes, %d entries replaced, TOC +%d bytes)" % (out, os.path.getsize(out), len(repl), shift))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
