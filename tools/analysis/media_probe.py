#!/usr/bin/env python3
"""Read-only media probe for tod-recomp (stdlib only, run with python -I).

Reads:
  - packed\\game\\global_cached.psarc : header dumps of .xvag / .bnk / .bnkh entries
  - built\\movies\\**\\*.bik            : Bink header fields
  - elf\\EBOOT.ELF                      : keyword strings (codec / Bink / SPU job names)
Writes only out\\analysis\\media_probe.json. Never modifies inputs.
"""
import collections
import json
import os
import re
import sys
import zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
USRDIR = os.path.join(ROOT, "game", "disc", "PS3_GAME", "USRDIR")
ELF = os.path.join(ROOT, "elf", "EBOOT.ELF")
OUT = os.path.join(ROOT, "out", "analysis", "media_probe.json")
MB = 1024 * 1024
AUDIO_EXTS = (".xvag", ".bnk", ".bnkh")


def be(b):
    return int.from_bytes(b, "big")


def read_index(f):
    f.seek(0)
    hdr = f.read(32)
    if len(hdr) < 32 or hdr[:4] != b"PSAR":
        raise ValueError("bad PSAR magic")
    h = {
        "compression": hdr[8:12].decode("ascii", "replace"),
        "toc_length": be(hdr[12:16]),
        "toc_entry_size": be(hdr[16:20]),
        "toc_entries": be(hdr[20:24]),
        "block_size": be(hdr[24:28]),
    }
    n, stride = h["toc_entries"], h["toc_entry_size"]
    f.seek(32)
    toc = f.read(n * stride)
    ents = []
    for i in range(n):
        e = toc[i * stride:(i + 1) * stride]
        ents.append({"block": be(e[16:20]), "usize": be(e[20:25]), "offset": be(e[25:30])})
    tbl_len = h["toc_length"] - 32 - n * stride
    f.seek(32 + n * stride)
    raw = f.read(tbl_len)
    bs = h["block_size"]
    w = 2 if bs <= 65536 else (3 if bs <= 16 * MB else 4)
    blocks = [be(raw[i:i + w]) for i in range(0, len(raw) - w + 1, w)]
    return h, ents, blocks


def read_entry(f, h, blocks, ent, max_bytes=None):
    if h["compression"] != "zlib":
        raise NotImplementedError(h["compression"])
    out = bytearray()
    remaining = ent["usize"]
    bi = ent["block"]
    bs = h["block_size"]
    f.seek(ent["offset"])
    while remaining > 0 and (max_bytes is None or len(out) < max_bytes):
        zs = blocks[bi] or bs
        raw = f.read(zs)
        chunk = raw
        if raw[:1] == b"\x78":
            try:
                chunk = zlib.decompressobj().decompress(raw)
            except zlib.error:
                chunk = raw
        chunk = chunk[:remaining]
        remaining -= len(chunk)
        out += chunk
        bi += 1
    return bytes(out if max_bytes is None else out[:max_bytes])


def u32le(b, o):
    return int.from_bytes(b[o:o + 4], "little") if len(b) >= o + 4 else None


def u32be(b, o):
    return int.from_bytes(b[o:o + 4], "big") if len(b) >= o + 4 else None


def probe_audio():
    arch = os.path.join(USRDIR, "packed", "game", "global_cached.psarc")
    res = {"archive": arch, "entries": [], "by_ext": {}}
    with open(arch, "rb") as f:
        h, ents, blocks = read_index(f)
        names = read_entry(f, h, blocks, ents[0]).decode("utf-8", "replace").split("\n")
        if names and names[-1] == "":
            names.pop()
        names = [x.rstrip("\r") for x in names]
        for i in range(1, h["toc_entries"]):
            nm = names[i - 1] if i - 1 < len(names) else "<unnamed %d>" % i
            ext = os.path.splitext(nm)[1].lower()
            if ext not in AUDIO_EXTS:
                continue
            head = read_entry(f, h, blocks, ents[i], max_bytes=256)
            res["entries"].append({
                "index": i, "name": nm, "ext": ext,
                "usize": ents[i]["usize"], "head_hex": head[:64].hex(),
                "magic4": head[:4].decode("latin1"),
                "head": head,
            })
    # per-extension summary
    by = collections.defaultdict(list)
    for e in res["entries"]:
        by[e["ext"]].append(e)
    for ext, lst in by.items():
        magics = collections.Counter(e["magic4"] for e in lst)
        sizes = [e["usize"] for e in lst]
        # for each 4-byte-aligned LE/BE u32 field in the first 64 bytes, count distinct values
        fields = {}
        for off in range(0, 64, 4):
            vals_le = collections.Counter(u32le(e["head"], off) for e in lst)
            fields["u32le@%02x" % off] = {
                "distinct": len(vals_le),
                "top": [[hex(k) if k is not None else None, v] for k, v in vals_le.most_common(4)],
            }
        res["by_ext"][ext] = {
            "count": len(lst),
            "magic4_counts": dict(magics.most_common(8)),
            "size_min": min(sizes), "size_max": max(sizes), "size_total": sum(sizes),
            "fields_le": fields,
        }
    # strip raw bytes before JSON dump
    for e in res["entries"]:
        e.pop("head", None)
    return res


def probe_bik():
    movies = os.path.join(USRDIR, "built", "movies")
    recs = []
    for dp, _, fns in os.walk(movies):
        for fn in fns:
            if fn.lower().endswith(".bik"):
                p = os.path.join(dp, fn)
                with open(p, "rb") as f:
                    hd = f.read(48)
                tag = hd[:3].decode("latin1")
                rev = chr(hd[3]) if len(hd) > 3 else "?"
                fps_num, fps_den = u32le(hd, 28), u32le(hd, 32)
                frames = u32le(hd, 8)
                fps = (fps_num / fps_den) if fps_num and fps_den else None
                recs.append({
                    "file": os.path.relpath(p, USRDIR),
                    "size": os.path.getsize(p),
                    "tag": tag, "revision": rev, "tag4_hex": hd[:4].hex(),
                    "frames": frames,
                    "width": u32le(hd, 20), "height": u32le(hd, 24),
                    "fps_num": fps_num, "fps_den": fps_den,
                    "fps": round(fps, 3) if fps else None,
                    "duration_s": round(frames / fps, 2) if (fps and frames) else None,
                    "video_flags_hex": hex(u32le(hd, 36) or 0),
                    "audio_tracks": u32le(hd, 40),
                })
    recs.sort(key=lambda r: r["file"])
    combos = collections.Counter(
        (r["tag"], r["revision"], r["width"], r["height"], r["fps"], r["audio_tracks"]) for r in recs)
    return {"count": len(recs), "records": recs,
            "combos": [[list(k), v] for k, v in combos.most_common()]}


KEYWORDS = [b"mp3", b"mpeg", b"atrac", b"at3", b"vag", b"xvag", b"libmp3", b"syn",
            b"bink", b"bik", b"adec", b"cellaudio", b"cellatrac", b"celladec",
            b"cellsync", b"spu job", b"sndlib", b"wws", b"audiojob", b"pcm", b"adpcm"]


def probe_elf():
    with open(ELF, "rb") as f:
        data = f.read()
    hits = collections.defaultdict(list)
    counts = collections.Counter()
    for m in re.finditer(rb"[\x20-\x7e]{4,}", data):
        s = m.group(0)
        low = s.lower()
        for kw in KEYWORDS:
            if kw in low:
                counts[kw.decode()] += 1
                if len(hits[kw.decode()]) < 6:
                    hits[kw.decode()].append({"off": hex(m.start()), "str": s[:120].decode("latin1")})
    return {"size": len(data), "keyword_counts": dict(counts), "examples": dict(hits)}


def main():
    out = {}
    out["audio"] = probe_audio()
    out["bik"] = probe_bik()
    out["elf"] = probe_elf()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump(out, fh, indent=1)
    print("wrote", OUT)
    print("audio entries:", len(out["audio"]["entries"]))
    for ext, v in out["audio"]["by_ext"].items():
        print(ext, v["count"], "magic:", v["magic4_counts"], "size:", v["size_min"], v["size_max"])
    print("bik:", out["bik"]["count"], "combos:", out["bik"]["combos"][:10])
    print("elf keywords:", out["elf"]["keyword_counts"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
