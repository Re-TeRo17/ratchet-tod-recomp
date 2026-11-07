#!/usr/bin/env python3
"""Extract one or more entries (1-based TOC index, see out/psarc/<name>.manifest.txt) of a zlib PSARC.
Usage: python -I psarc_extract.py SRC.psarc OUTDIR INDEX [INDEX ...]   -> OUTDIR/<INDEX>.bin
The game's own file is only read; extracted data is the USER's and must never be committed."""
import os
import sys
import zlib


def be(b):
    return int.from_bytes(b, "big")


def read_toc(f):
    hdr = f.read(32)
    assert hdr[:4] == b"PSAR" and hdr[8:12] == b"zlib", "not a zlib PSARC"
    toc_len, esz, n, bs = be(hdr[12:16]), be(hdr[16:20]), be(hdr[20:24]), be(hdr[24:28])
    w = 2 if bs <= 65536 else (3 if bs <= 16 * 1024 * 1024 else 4)
    toc = f.read(n * esz)
    tbl = f.read(toc_len - 32 - n * esz)
    ents = [(be(toc[i * esz + 16:i * esz + 20]), be(toc[i * esz + 20:i * esz + 25]),
             be(toc[i * esz + 25:i * esz + 30])) for i in range(n)]
    sizes = [be(tbl[i:i + w]) for i in range(0, len(tbl), w)]
    return hdr, ents, sizes, bs, w


def extract(f, ents, sizes, bs, idx):
    b0, usize, off = ents[idx]
    out = bytearray()
    f.seek(off)
    bi = b0
    while len(out) < usize:
        want = min(bs, usize - len(out))
        z = sizes[bi]
        if z == 0 and want == 65536:
            z = 65536
        raw = f.read(z)
        out += raw if z == want else zlib.decompress(raw)
        bi += 1
    return bytes(out[:usize])


def main(a):
    src, outdir, idxs = a[0], a[1], [int(x) for x in a[2:]]
    os.makedirs(outdir, exist_ok=True)
    with open(src, "rb") as f:
        hdr, ents, sizes, bs, w = read_toc(f)
        for i in idxs:
            d = extract(f, ents, sizes, bs, i)
            open(os.path.join(outdir, "%d.bin" % i), "wb").write(d)
            print("entry %d: %d bytes" % (i, len(d)))


if __name__ == "__main__":
    main(sys.argv[1:])
