#!/usr/bin/env python3
"""lift_dynovl.py -- lift SPU code regions captured at run time (SPU_DUMP_UNLIFTED).

The runtime (ps3recomp spu_channels.c, spu_dynovl_capture) saves every LS code
region that went to the SPU interpreter as
    <dumps>/ovl_<hash16>_<lsa5>_<size hex>.bin   raw LS bytes of [lsa, lsa+size)
    <dumps>/ovl_<hash16>_<lsa5>_<size hex>.ent   entry pcs seen (one 0x%05X per line)
This tool lifts each one at its LS address into spu/dynovl_<hash16>/ (CMake globs
spu/*/spu_recomp.c) and writes spu/dynovl_register.c, which registers every lifted
image under its own image id (400..) with the content hash, LS range and 16-byte
head (taken at the first entry); the runtime picks it when the live LS bytes still hash the same, and falls
back to the interpreter otherwise.

Dumps are game code: keep them in a git-ignored folder (spu_dumps/).

Usage (from tod-recomp):
  python -I tools/lift_dynovl.py [--dumps spu_dumps] [--out spu] [--force]
"""
import argparse, glob, os, re, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
TOOLS = os.path.join(ROOT, "ps3recomp", "tools")
sys.path.insert(0, TOOLS)
from wrap_spu_elf import wrap                      # noqa: E402
import build_spu_workloads as bsw                   # noqa: E402

IMAGE_BASE = 400
NAME_RE = re.compile(r"^ovl_([0-9A-F]{16})_([0-9A-F]{5})_([0-9A-F]+)\.bin$")
M64 = (1 << 64) - 1


def dyn_hash(b):
    """Same as spu_dynovl_hash() in runtime/spu/spu_channels.c."""
    n = len(b)
    h = 0x9E3779B97F4A7C15
    for i in range(0, n - 7, 8):
        w = struct.unpack_from("<Q", b, i)[0]
        h = ((h ^ w) * 0xFF51AFD7ED558CCD) & M64
        h ^= h >> 29
    h ^= n
    h = (h * 0xC4CEB9FE1A85EC53) & M64
    h ^= h >> 32
    return h


def code_hash(raw, ranges):
    """Same as spu_dynovl_hash_ranges() in spu_channels.c."""
    h = 0x9E3779B97F4A7C15
    tot = 0
    for off, ln in ranges:
        for i in range(off, off + ln, 8):
            w = struct.unpack_from("<Q", raw, i)[0]
            h = ((h ^ w) * 0xFF51AFD7ED558CCD) & M64
            h ^= h >> 29
        tot += ln
    h ^= tot
    h = (h * 0xC4CEB9FE1A85EC53) & M64
    h ^= h >> 32
    return h


def switch_table_targets(raw, base):
    """Targets of position-independent switch tables (see lift_jobmods.py)."""
    ilas = set()
    for off in range(0, len(raw) - 4, 4):
        w = struct.unpack_from(">I", raw, off)[0]
        if (w >> 25) & 0x7F == 0x21:
            ilas.add((w >> 7) & 0x3FFFF)
    out = []
    for off in range(0, len(raw) - 16, 4):
        w = struct.unpack_from(">I", raw, off)[0]
        if (w >> 21) & 0x7FF != 0x1A8 or (w & 0x7F) != 0:
            continue
        tbl = off + 4
        if (base + tbl) not in ilas and tbl not in ilas:
            continue
        targets = []
        while tbl + 4 * len(targets) + 4 <= len(raw):
            e = struct.unpack_from(">I", raw, tbl + 4 * len(targets))[0]
            t = (tbl + e) & 0xFFFFFFFF
            if t % 4 or not (0 <= t < len(raw)):
                break
            targets.append(base + t)
        if len(targets) >= 2:
            out += targets
    return sorted(set(out))


def pic_violations(raw, lsa, ranges):
    """Why a blob cannot run at another LS address with only lqr/stqr rebased
    (--pic-reloc). ranges: absolute [start, end) of the lifted functions."""
    bad = []
    for s0, e0 in ranges:
        for a in range(max(s0, lsa), min(e0, lsa + len(raw)), 4):
            w = struct.unpack_from(">I", raw, a - lsa)[0]
            op11, op9, op7, ra = w >> 21, w >> 23, w >> 25, (w >> 7) & 0x7F
            if op9 in (0x60, 0x62, 0x66):                          bad.append((a, "bra/brasl/brsl"))
            elif op9 in (0x61, 0x41):                              bad.append((a, "lqa/stqa"))
            elif op11 in (0x1A9, 0x1AA, 0x1AB):                    bad.append((a, "bisl/iret/bisled"))
            elif (op11 == 0x1A8 or 0x128 <= op11 <= 0x12B) and ra != 0:   bad.append((a, "bi through $r%d" % ra))
            elif op11 in (0x00D, 0x00F, 0x10D):                    bad.append((a, "channel op"))
            elif op7 == 0x21 and lsa <= ((w >> 7) & 0x3FFFF) < lsa + len(raw):   bad.append((a, "ila of an LS address"))
    return bad


def lift_reloc(a, hexh, group, tmp_elf):
    """A content hash captured at many LS addresses, each .ent holding only its own
    base (the blob is entered at offset 0 only): lift ONE instance at the lowest
    base with --pic-reloc. group: [(lsa, path)]. Returns the register tuple or None."""
    import json
    group = sorted(group)
    lsa0, f0 = group[0]
    raw = open(f0, "rb").read()
    size = len(raw)
    if size % 8 or f"{dyn_hash(raw):016X}" != hexh:
        print(f"skip reloc {hexh}: size/hash mismatch"); return None
    for lsa, f in group:
        ef = f[:-4] + ".ent"
        ents = set(int(l, 0) for l in open(ef).read().split()) if os.path.exists(ef) else set()
        if ents != {lsa} or open(f, "rb").read() != raw:
            print(f"skip reloc {hexh}: {os.path.basename(f)} is not a plain relocated copy (entries {sorted(ents)})")
            return None
    name = f"dynovl_{hexh}_reloc"
    prefix = name + "_"
    outsub = os.path.join(a.out, name)
    extra_s = ",".join(hex(e) for e in sorted({lsa0} | set(switch_table_targets(raw, lsa0))))
    stamp = os.path.join(outsub, "entries.txt")
    stamp_s = "reloc:" + extra_s
    if (not a.force and os.path.exists(os.path.join(outsub, "spu_recomp.c")) and
            os.path.exists(stamp) and open(stamp).read() == stamp_s):
        print(f"keep {name} (canon LS 0x{lsa0:05X}, {size} B, {len(group)} captured bases)")
    else:
        os.makedirs(outsub, exist_ok=True)
        with open(tmp_elf, "wb") as g:
            g.write(wrap(raw, base=lsa0, entry=lsa0))
        cmd = [sys.executable, "-I", os.path.join(TOOLS, "spu_lifter.py"), "--auto-functions", tmp_elf,
               "--base", hex(lsa0), "--symbol-prefix", prefix, "-o", outsub,
               "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
               "--extra-funcs", extra_s, "--pic-reloc", "--report", os.path.join(outsub, "report.json")]
        print(f"LIFT {name} (canon LS 0x{lsa0:05X}, {size} B, --pic-reloc, {len(group)} captured bases)")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-800:], r.stderr[-800:])
            print(f"  lift failed -- reloc blob left out")
            return None
        for fn in ("spu_recomp.h", "spu_recomp.c"):
            bsw.debase_includes(os.path.join(outsub, fn))
        open(stamp, "w").write(stamp_s)
    rep = json.load(open(os.path.join(outsub, "report.json")))
    bad = pic_violations(raw, lsa0, rep["functions"])
    if bad:
        print(f"skip reloc {hexh}: not position independent: " +
              ", ".join(f"0x{ad:X} {why}" for ad, why in bad[:6]))
        shutil.rmtree(outsub)
        return None
    return (name, prefix, int(hexh, 16), lsa0, size, raw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dumps", default=os.path.join(ROOT, "spu_dumps"))
    ap.add_argument("--out", default=os.path.join(ROOT, "spu"))
    ap.add_argument("--max-variants", type=int, default=3,
                    help="skip a content hash captured at more than this many LS addresses (a code cache that "
                         "relocates the same blob everywhere cannot be lifted at one address)")
    ap.add_argument("--force", action="store_true", help="re-lift even if the entry set is unchanged")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dumps, "ovl_*.bin")))
    if not files:
        sys.exit(f"lift_dynovl: no ovl_*.bin in {a.dumps}")
    os.makedirs(a.out, exist_ok=True)
    # drop stale dynovl_* dirs whose dump is gone
    keep = set()
    variants = {}
    groups = {}
    for f in files:
        m = NAME_RE.match(os.path.basename(f))
        if m:
            variants[m.group(1)] = variants.get(m.group(1), 0) + 1
            groups.setdefault(m.group(1), []).append((int(m.group(2), 16), f))
    entries_out = []
    reloc_out = []
    reloc_seen = set()
    tmp_elf = os.path.join(a.out, "_dynovl_tmp.elf")
    for f in files:
        m = NAME_RE.match(os.path.basename(f))
        if not m:
            continue
        hexh, lsa, size = m.group(1), int(m.group(2), 16), int(m.group(3), 16)
        if variants[hexh] > a.max_variants:
            if hexh not in reloc_seen:
                reloc_seen.add(hexh)
                if os.environ.get("LIFT_DYNOVL_RELOC", "1") == "0":
                    print(f"skip {hexh}: hash seen at {variants[hexh]} LS addresses (relocated blob; reloc lift off)")
                else:
                    rl = lift_reloc(a, hexh, groups[hexh], tmp_elf)
                    if rl:
                        keep.add(rl[0]); reloc_out.append(rl)
            continue
        raw = open(f, "rb").read()
        if len(raw) != size or len(raw) % 8:
            print(f"skip {f}: size mismatch / not a multiple of 8")
            continue
        h = dyn_hash(raw)
        if f"{h:016X}" != hexh:
            print(f"skip {f}: hash mismatch (file {hexh}, computed {h:016X})")
            continue
        ents = []
        ef = f[:-4] + ".ent"
        if os.path.exists(ef):
            for l in open(ef):
                l = l.strip()
                if l:
                    ents.append(int(l, 0))
        ents = sorted(set(e for e in ents if lsa <= e < lsa + size and e % 4 == 0))
        if not ents:
            print(f"skip {f}: no entry points")
            continue
        name = f"dynovl_{hexh}"
        prefix = name + "_"
        outsub = os.path.join(a.out, name)
        keep.add(name)
        extra = sorted(set(ents) | set(switch_table_targets(raw, lsa)))
        extra_s = ",".join(hex(e) for e in extra)
        stamp = os.path.join(outsub, "entries.txt")
        if (not a.force and os.path.exists(os.path.join(outsub, "spu_recomp.c")) and
                os.path.exists(stamp) and open(stamp).read() == extra_s):
            print(f"keep {name} (LS 0x{lsa:05X}, {size} B, {len(ents)} entries)")
        else:
            os.makedirs(outsub, exist_ok=True)
            with open(tmp_elf, "wb") as g:
                g.write(wrap(raw, base=lsa, entry=ents[0]))
            cmd = [sys.executable, "-I", os.path.join(TOOLS, "spu_lifter.py"), "--auto-functions", tmp_elf,
                   "--base", hex(lsa), "--symbol-prefix", prefix, "-o", outsub,
                   "--source-name", "spu_recomp.c", "--header-name", "spu_recomp.h",
                   "--extra-funcs", extra_s, "--report", os.path.join(outsub, "report.json")]
            print(f"LIFT {name} (LS 0x{lsa:05X}, {size} B, entries {', '.join(hex(e) for e in ents)})")
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout[-800:], r.stderr[-800:])
                print(f"  lift failed -- overlay left out")
                keep.discard(name)
                continue
            for fn in ("spu_recomp.h", "spu_recomp.c"):
                bsw.debase_includes(os.path.join(outsub, fn))
            open(stamp, "w").write(extra_s)
        ho = (min(ents) - lsa) & ~15
        if ho + 16 > size: ho = 0
        import json
        rep = json.load(open(os.path.join(outsub, "report.json")))
        iv = sorted((max(0, s0 - lsa) & ~15, min(size, (e0 - lsa + 15) & ~15)) for s0, e0 in rep["functions"])
        rr = []
        for s0, e0 in iv:
            if rr and s0 <= rr[-1][1]:
                rr[-1][1] = max(rr[-1][1], e0)
            else:
                rr.append([s0, e0])
        ranges = [(s0, e0 - s0) for s0, e0 in rr if e0 > s0]
        if not ranges:
            print(f"skip {name}: no lifted functions"); continue
        entries_out.append((name, prefix, code_hash(raw, ranges), lsa, size, raw[ho:ho + 16], ho, ranges))
    for d in glob.glob(os.path.join(a.out, "dynovl_*")):
        if os.path.isdir(d) and os.path.basename(d) not in keep:
            import shutil
            shutil.rmtree(d)
    if os.path.exists(tmp_elf):
        os.remove(tmp_elf)

    L = ["/* dynovl_register.c -- GENERATED by tools/lift_dynovl.py.",
         " * Run-time captured SPU code regions, lifted at the LS address they were streamed to.",
         " * Registered with hash + LS range + 16-byte head; the runtime uses one only while the",
         " * live LS bytes still hash the same (spu_dynovl_find), else the interpreter runs. */",
         "#include <stdint.h>", "",
         "extern void spu_begin_image(int image_id);",
         "extern void spu_dynovl_register(uint64_t hash, uint32_t lsa, uint32_t size, const uint8_t head[16], uint32_t head_off, int image_id, uint32_t nranges, const uint32_t* ranges);",
         "extern void spu_dynovl_register_reloc(uint64_t hash, uint32_t canon_lsa, uint32_t size, const uint8_t* ref, int image_id);", ""]
    for name, prefix, h, lsa, size, head, ho, ranges in entries_out:
        L.append(f"extern void {prefix}spu_recomp_register(void);")
    for name, prefix, h, lsa, size, raw in reloc_out:
        L.append(f"extern void {prefix}spu_recomp_register(void);")
    L += ["", "void tod_dynovl_register_all(void)", "{"]
    for i, (name, prefix, h, lsa, size, head, ho, ranges) in enumerate(entries_out):
        img = IMAGE_BASE + i
        arr = ",".join(f"0x{b:02X}" for b in head)
        L.append(f"    {{ static const uint8_t head[16] = {{{arr}}};")
        L.append(f"      static const uint32_t rng[] = {{{','.join(f'0x{x:X}' for r in ranges for x in r)}}};")
        L.append(f"      spu_begin_image({img}); {prefix}spu_recomp_register();")
        L.append(f"      spu_dynovl_register(0x{h:016X}ULL, 0x{lsa:X}u, 0x{size:X}u, head, 0x{ho:X}u, {img}, {len(ranges)}, rng); }}   /* {name} */")
    for j, (name, prefix, h, lsa, size, raw) in enumerate(reloc_out):
        img = IMAGE_BASE + len(entries_out) + j
        arr = ",".join(f"0x{b:02X}" for b in raw)
        L.append(f"    {{ static const uint8_t ref[{size}] = {{{arr}}};")
        L.append(f"      spu_begin_image({img}); {prefix}spu_recomp_register();")
        L.append(f"      spu_dynovl_register_reloc(0x{h:016X}ULL, 0x{lsa:X}u, 0x{size:X}u, ref, {img}); }}   /* {name} */")
    L += ["    spu_begin_image(0);", "}", "",
          "__attribute__((constructor)) static void tod_dynovl_register_ctor(void)",
          "{", "    tod_dynovl_register_all();", "}", ""]
    reg = os.path.join(a.out, "dynovl_register.c")
    open(reg, "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print(f"wrote {reg} ({len(entries_out)} overlay(s), image ids {IMAGE_BASE}..{IMAGE_BASE + len(entries_out) - 1}; "
          f"{len(reloc_out)} relocated blob(s), image ids {IMAGE_BASE + len(entries_out)}..{IMAGE_BASE + len(entries_out) + len(reloc_out) - 1})")


if __name__ == "__main__":
    main()
