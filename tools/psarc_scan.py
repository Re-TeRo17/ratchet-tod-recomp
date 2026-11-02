#!/usr/bin/env python3
"""Read-only PSARC manifest + SPU / job-module scanner (stdlib only).

Usage:  python -I psarc_scan.py [all|psarc|dat] [USRDIR]

Reads the input tree only. Writes manifests, hit lists and extracted
SPU ELFs / job modules under OUT.
"""
import json
import os
import re
import struct  # noqa: F401 (kept for format debugging)
import sys
import zlib

USRDIR = os.environ.get("TOD_USRDIR", os.path.join("game", "disc", "PS3_GAME", "USRDIR"))
OUT = os.path.join("out", "psarc")
SPU_DIR = os.path.join(OUT, "spu")
MB = 1024 * 1024
NAME_RE = re.compile(r"spu|\.elf|\.self|\.sprx|\.prx|job|coll|igcoll", re.I)
ASSET_EXTS = {
    ".dds", ".tga", ".png", ".jpg", ".jpeg", ".bmp", ".wav", ".at3", ".ogg",
    ".mp3", ".mp4", ".sdf", ".xml", ".txt", ".lua", ".tex", ".mdl", ".xmd",
    ".sgt", ".dat", ".fnt", ".ttf", ".nsf",
}


def be(b):
    return int.from_bytes(b, "big")


def safe_name(s):
    return re.sub(r'[<>:"|?*\x00-\x1f]', "_", s)


# ---------------------------------------------------------------- PSARC ----
def read_index(f):
    f.seek(0)
    hdr = f.read(32)
    if len(hdr) < 32 or hdr[:4] != b"PSAR":
        raise ValueError("bad PSAR magic")
    h = {
        "version": hdr[4:8].hex(),
        "compression": hdr[8:12].decode("ascii", "replace"),
        "toc_length": be(hdr[12:16]),
        "toc_entry_size": be(hdr[16:20]),
        "toc_entries": be(hdr[20:24]),
        "block_size": be(hdr[24:28]),
        "flags": be(hdr[28:32]),
    }
    n, stride = h["toc_entries"], h["toc_entry_size"]
    f.seek(32)
    toc = f.read(n * stride)
    if len(toc) < n * stride:
        raise ValueError("truncated TOC")
    ents = []
    for i in range(n):
        e = toc[i * stride:(i + 1) * stride]
        ents.append({
            "md5": e[:16].hex(),
            "block": be(e[16:20]),
            "usize": be(e[20:25]),
            "offset": be(e[25:30]),
        })
    tbl_len = h["toc_length"] - 32 - n * stride
    if tbl_len < 0:
        raise ValueError("toc_length smaller than TOC")
    f.seek(32 + n * stride)
    raw = f.read(tbl_len)
    bs = h["block_size"]
    w = 2 if bs <= 65536 else (3 if bs <= 16 * MB else 4)
    blocks = [be(raw[i:i + w]) for i in range(0, len(raw) - w + 1, w)]
    return h, ents, blocks


def iter_chunks(f, h, blocks, ent):
    if h["compression"] != "zlib":
        raise NotImplementedError("compression=%r" % h["compression"])
    remaining = ent["usize"]
    bs = h["block_size"]
    bi = ent["block"]
    f.seek(ent["offset"])
    while remaining > 0:
        if bi >= len(blocks):
            raise ValueError("block index %d beyond table (%d)" % (bi, len(blocks)))
        zs = blocks[bi] or bs
        raw = f.read(zs)
        if not raw:
            raise ValueError("unexpected EOF")
        out = raw
        if raw[:1] == b"\x78":
            try:
                out = zlib.decompressobj().decompress(raw)
            except zlib.error:
                out = raw  # stored block that merely starts with 0x78
        out = out[:remaining]
        remaining -= len(out)
        yield out
        bi += 1


def read_entry(f, h, blocks, ent, max_bytes=None):
    buf = bytearray()
    for chunk in iter_chunks(f, h, blocks, ent):
        buf += chunk
        if max_bytes is not None and len(buf) >= max_bytes:
            break
    return bytes(buf[:max_bytes] if max_bytes is not None else buf)


def classify(head):
    if head[:4] == b"\x7fELF" and len(head) >= 20:
        mach = be(head[18:20])
        return "spu-elf" if mach == 23 else "elf-m%d" % mach
    if head[:4] == b"\xC0\xDE\xC0\xDE":
        return "job-module"
    return None


def process_psarc(path, rel, manifest_dir, name_hits, hits_out, used_manifest_names):
    rec = {"archive": rel, "size": os.path.getsize(path), "hits": [], "errors": []}
    with open(path, "rb") as f:
        try:
            h, ents, blocks = read_index(f)
        except Exception as e:  # unparseable header/TOC
            rec["unparseable"] = str(e)
            return rec
        rec.update(compression=h["compression"], version=h["version"],
                   block_size=h["block_size"], toc_entries=h["toc_entries"],
                   blocks=len(blocks), flags=h["flags"])
        if h["compression"] != "zlib":
            rec["note"] = ("compression %r: manifest not decompressed; "
                           "only TOC metadata listed" % h["compression"])
            return rec
        try:
            text = read_entry(f, h, blocks, ents[0]).decode("utf-8", "replace")
        except Exception as e:
            rec["unparseable"] = "manifest: %s" % e
            return rec
        names = text.split("\n")
        if names and names[-1] == "":
            names.pop()
        names = [x.rstrip("\r") for x in names]
        rec["manifest_names"] = len(names)
        if len(names) != h["toc_entries"] - 1:
            rec["warning"] = "manifest has %d names for %d entries" % (
                len(names), h["toc_entries"] - 1)

        # manifest file, <archive>.manifest.txt (basename, folder prefix on clash)
        base = os.path.basename(path) + ".manifest.txt"
        if base in used_manifest_names:
            base = rel.replace("\\", "__") + ".manifest.txt"
        used_manifest_names.add(base)
        with open(os.path.join(manifest_dir, base), "w", encoding="utf-8") as mf:
            for i in range(1, h["toc_entries"]):
                nm = names[i - 1] if i - 1 < len(names) else "<unnamed %d>" % i
                mf.write("%d\t%d\t%s\n" % (i, ents[i]["usize"], nm))

        arch_stem = os.path.splitext(os.path.basename(path))[0]
        for i in range(1, h["toc_entries"]):
            nm = names[i - 1] if i - 1 < len(names) else "<unnamed %d>" % i
            ent = ents[i]
            usize = ent["usize"]
            leaf = nm.replace("\\", "/").split("/")[-1]
            ext = os.path.splitext(leaf)[1].lower()
            is_name = bool(NAME_RE.search(nm))
            if is_name:
                name_hits.append("%s\t%d\t%d\t%s" % (rel, i, usize, nm))
            small_nonasset = usize < 2 * MB and ext not in ASSET_EXTS
            if not (is_name or small_nonasset):
                continue
            try:
                head = read_entry(f, h, blocks, ent, max_bytes=64)
            except Exception as e:
                rec["errors"].append("%s: %s" % (nm, e))
                continue
            kind = classify(head)
            if kind is None:
                continue
            hit = {"name": nm, "size": usize, "kind": kind, "index": i}
            if kind in ("spu-elf", "job-module"):
                try:
                    data = read_entry(f, h, blocks, ent)
                except Exception as e:
                    rec["errors"].append("extract %s: %s" % (nm, e))
                    continue
                os.makedirs(SPU_DIR, exist_ok=True)
                outname = safe_name("%s__%s" % (arch_stem, nm.replace("\\", "/").replace("/", "__")))
                outpath = os.path.join(SPU_DIR, outname)
                with open(outpath, "wb") as of:
                    of.write(data)
                hit["out"] = outpath
                hit["written"] = len(data)
            rec["hits"].append(hit)
            hits_out.append("%s\t%s\t%d\t%s" % (rel, kind, usize, nm))
    return rec


# ------------------------------------------------------------- .dat ELF ----
def elf_extent(f, pos, hdr, fsize):
    cls = hdr[4]
    if cls == 1:
        phoff, shoff = be(hdr[28:32]), be(hdr[32:36])
        phes, phn = be(hdr[42:44]), be(hdr[44:46])
        shes, shn = be(hdr[46:48]), be(hdr[48:50])
    elif cls == 2:
        phoff, shoff = be(hdr[32:40]), be(hdr[40:48])
        phes, phn = be(hdr[54:56]), be(hdr[56:58])
        shes, shn = be(hdr[58:60]), be(hdr[60:62])
    else:
        return None
    end = 0
    if shoff and shn:
        end = max(end, shoff + shes * shn)
    if phoff and phn and phes:
        f.seek(pos + phoff)
        ph = f.read(phes * phn)
        for i in range(min(phn, len(ph) // phes)):
            p = ph[i * phes:(i + 1) * phes]
            if cls == 1:
                off, fsz = be(p[4:8]), be(p[16:20])
            else:
                off, fsz = be(p[8:16]), be(p[32:40])
            end = max(end, off + fsz)
    if shoff and shn and shes:
        f.seek(pos + shoff)
        sh = f.read(shes * shn)
        for i in range(min(shn, len(sh) // shes)):
            s = sh[i * shes:(i + 1) * shes]
            stype = be(s[4:8])
            if cls == 1:
                soff, ssz = be(s[16:20]), be(s[20:24])
            else:
                soff, ssz = be(s[24:32]), be(s[32:40])
            if stype != 8:  # SHT_NOBITS occupies no file space
                end = max(end, soff + ssz)
    if end <= 0:
        return None
    return min(end, fsize - pos)


def scan_dats(usr, hits_out, summary):
    for dp, _dn, fn in os.walk(usr):
        for n in sorted(fn):
            if not n.lower().endswith(".dat"):
                continue
            path = os.path.join(dp, n)
            size = os.path.getsize(path)
            rec = {"file": path, "size": size, "spu": [], "other_elf": 0}
            try:
                with open(path, "rb") as f:
                    if size < 50 * MB:
                        data = f.read()
                        positions = [m.start() for m in re.finditer(rb"\x7fELF", data)]
                        del data
                    else:
                        head = f.read(64 * 1024)
                        positions = [m.start() for m in re.finditer(rb"\x7fELF", head)]
                    covered = -1
                    for pos in positions:
                        if pos < covered:
                            continue
                        f.seek(pos)
                        hdr = f.read(64)
                        # e_ident class/data/version and e_version must look sane
                        if len(hdr) < 52 or hdr[4] not in (1, 2) or hdr[6] != 1 or be(hdr[20:24]) != 1:
                            continue
                        if hdr[5] != 2:  # only big-endian (PS3) ELFs considered
                            continue
                        mach = be(hdr[18:20])
                        if mach != 23:
                            rec["other_elf"] += 1
                            continue
                        ext = elf_extent(f, pos, hdr, size)
                        if not ext:
                            continue
                        f.seek(pos)
                        blob = f.read(ext)
                        os.makedirs(SPU_DIR, exist_ok=True)
                        outname = safe_name("dat__%s__0x%x.elf" % (n, pos))
                        outpath = os.path.join(SPU_DIR, outname)
                        with open(outpath, "wb") as of:
                            of.write(blob)
                        rec["spu"].append({"offset": pos, "size": len(blob), "out": outpath})
                        hits_out.append("%s\tspu-elf@0x%x\t%d\t%s" % (path, pos, len(blob), outname))
                        covered = pos + ext
            except Exception as e:
                rec["error"] = str(e)
            if rec["spu"] or rec.get("error") or rec["other_elf"]:
                summary["dat"].append(rec)
            summary["dat_scanned"] = summary.get("dat_scanned", 0) + 1


# ------------------------------------------------------------------ main ----
def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "all"
    usr = sys.argv[2] if len(sys.argv) > 2 else USRDIR
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(SPU_DIR, exist_ok=True)
    summary = {"archives": [], "dat": []}
    name_hits, hits_out = [], []

    if mode in ("all", "psarc"):
        used = set()
        archives = []
        for dp, _dn, fn in os.walk(usr):
            for n in fn:
                if n.lower().endswith(".psarc"):
                    archives.append(os.path.join(dp, n))
        archives.sort()
        for p in archives:
            rel = os.path.relpath(p, usr)
            try:
                rec = process_psarc(p, rel, OUT, name_hits, hits_out, used)
            except Exception as e:
                rec = {"archive": rel, "unparseable": "exception: %s" % e}
            summary["archives"].append(rec)
            print("%-40s %-6s %6s entries  hits=%d%s" % (
                rel, rec.get("compression", "?"), rec.get("toc_entries", "?"),
                len(rec.get("hits", [])),
                ("  UNPARSEABLE: " + rec["unparseable"]) if "unparseable" in rec else ""))

        with open(os.path.join(OUT, "psarc_name_matches.txt"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(name_hits) + "\n")
        with open(os.path.join(OUT, "psarc_hits.txt"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(hits_out) + "\n")

    if mode in ("all", "dat"):
        scan_dats(usr, hits_out, summary)
        with open(os.path.join(OUT, "dat_hits.txt"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(x for x in hits_out if x.startswith(usr)) + "\n")

    with open(os.path.join(OUT, "scan_summary.json"), "w", encoding="utf-8") as fh:
        json.dump(summary, fh, indent=1)
    print("name-matched manifest entries:", len(name_hits))
    print("SPU/job hits (psarc):", sum(len(a.get("hits", [])) for a in summary["archives"]))
    print("dat files with SPU ELF:", len(summary["dat"]), "| dat files scanned:", summary.get("dat_scanned", 0))


if __name__ == "__main__":
    main()
