#!/usr/bin/env python3
"""tod_spu.py -- SPU extraction / lifting helper for tod-recomp (driven by tools/lift_spu.ps1).

  extract   pull the SPURS policy module (PM) and the WWS job modules out of the EBOOT
  lift-pm   lift the PM image and write spu/spu_workloads.c (registers it by the
            fingerprint cellSpurs computes over the raw PM bytes, as image id 2)
  lift-raw  lift the two RAW-SPU collision programs (igCollBroadSpu, igcollspu; see
            docs/RAW_SPU.md) and write spu/spu_raw_workloads.c, which registers them
            for runtime/spu/spu_raw.c (proxy-DMA'd LS run fingerprint)

Why a custom registry: build_spu_workloads.py fingerprints the *ELF* it wrapped, but
cellSpurs fingerprints the raw policy-module bytes the game hands to AddWorkload, so the
PM is registered here under the raw-bytes FNV-1a-64.  PM image id 2 is load-bearing: the
runtime's WWS job-manager paths (spu_channels.c / spu_dma.h / spu_context.h) key on it.
"""
import argparse, glob, hashlib, json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
TOOLS = os.path.join(ROOT, "ps3recomp", "tools")
sys.path.insert(0, TOOLS)
from wrap_spu_elf import wrap                      # noqa: E402
import build_spu_workloads as bsw                   # noqa: E402

# The Wws_Job policy module, as handed to cellSpursAddWorkloadWithAttribute
# (guest EA, size from the run log).  Loaded at LS 0xA00 by SPURS.
PM_EA, PM_SIZE, PM_BASE = 0x10053D00, 11136, 0xA00
PM_NAME, PM_IMAGE_ID = "spurs_pm_wwsjob", 2
# The PM is entered through an interrupt vector at 0xA2C (the WWS job manager
# writes `bra 0xA2C` at LS 0); nothing branches there, so seed it explicitly.
PM_EXTRA_FUNCS = "0xA2C"
JOBMOD_MAGIC = b"\xc0\xde\xc0\xde"

# Raw-SPU programs (sys_raw_spu_create; docs/RAW_SPU.md).  The PPU copies each blob
# into LS 0x60 by MFC proxy DMA and starts it at NPC 0x100; raw_spu_start then finds
# the lifted code by FNV-1a-64 of the LS run the GETs filled.  Not ELF, not C0DEC0DE:
# header 0x30 B (+0x10 entry, +0x14 LS end, +0x18 memsz, +0x1C code start, +0x20/+0x24
# offset/size of a parameter block, +0x28/+0x2C DEADDEAD), code from LS 0x90.
# (name, guest EA, size, LS base, entry, image id)
# Image ids: 2 = PM, 200.. = job modules; 110/111 are free.
RAW_BASE, RAW_ENTRY = 0x60, 0x100
RAW_IMAGES = [
    ("igCollBroadSpu", 0x10175180, 0x6DA0, 110),
    ("igcollspu",      0x1017BF80, 0x135D0, 111),
]
RAW_NAMES = [r[0] for r in RAW_IMAGES]


def raw_hashed_len(name, blob):
    """Bytes of the LS run that identify the program (FNV prefix length).

    The header declares a parameter block (+0x20 offset / +0x24 size, measured from
    the code start, LS address in +0x1C, i.e. image offset +0x30; for igCollBroadSpu
    that is LS 0x6DB0..0x6E00, matching docs/RAW_SPU.md).  When the PPU fills it in before each DMA the tail differs per
    run, so only the bytes before it are fingerprinted.  An image without a block
    (igcollspu: nothing written) is hashed whole."""
    code_ls = struct.unpack_from(">I", blob, 0x1C)[0]
    poff, psz = struct.unpack_from(">II", blob, 0x20)
    start = (code_ls - RAW_BASE) + poff
    if psz == 0 or poff == 0 or start + psz != len(blob):
        return len(blob)     # no trailing block: hash the whole run
    return start


def load_elf_segments(d):
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phentsize, phnum = struct.unpack_from(">HH", d, 0x36)
    segs = []
    for i in range(phnum):
        t, fl, off, va, pa, fs, ms, al = struct.unpack_from(">IIQQQQQQ", d, phoff + i * phentsize)
        if t == 1 and fs:
            segs.append((va, off, fs))
    return segs


def va_to_off(segs, va, n):
    for sva, off, fs in segs:
        if sva <= va and va + n <= sva + fs:
            return off + (va - sva)
    raise SystemExit(f"tod_spu: EA 0x{va:X}+{n} is not file-backed in the EBOOT")


def find_jobmods(d):
    """Headers: 4 x ila (0x42/0x43..) | entry@0x10 | size@0x14 | 0 | 0 | C0DEC0DE @0x20.
    The size field counts bss, so it overlaps the next module; the initialised extent
    is bounded by the next header (modules are packed back to back, 0x80-aligned)."""
    hdrs, i = [], 0
    while True:
        i = d.find(JOBMOD_MAGIC, i)
        if i < 0:
            break
        h = i - 0x20
        if h >= 0 and d[h] in (0x42, 0x43) and d[h + 4] in (0x42, 0x43):
            hdrs.append(h)
        i += 4
    return hdrs


def cmd_extract(a):
    d = open(a.elf, "rb").read()
    segs = load_elf_segments(d)
    out = os.path.abspath(a.out)
    os.makedirs(os.path.join(out, "images"), exist_ok=True)
    jm = os.path.join(out, "jobmods")
    os.makedirs(jm, exist_ok=True)
    for f in glob.glob(os.path.join(jm, "*.bin")):
        os.remove(f)

    # --- policy module
    off = va_to_off(segs, PM_EA, PM_SIZE)
    pm = d[off:off + PM_SIZE]
    open(os.path.join(out, "images", PM_NAME + ".bin"), "wb").write(pm)
    open(os.path.join(out, "images", PM_NAME + ".elf"), "wb").write(
        wrap(pm, base=PM_BASE, entry=PM_BASE))
    print(f"PM   EA 0x{PM_EA:08X} -> file 0x{off:X}, {PM_SIZE} bytes, "
          f"fp 0x{bsw.fnv1a64(pm):016X}")

    # --- raw-SPU programs
    for name, ea, size, _id in RAW_IMAGES:
        roff = va_to_off(segs, ea, size)
        blob = d[roff:roff + size]
        open(os.path.join(out, "images", name + ".bin"), "wb").write(blob)
        open(os.path.join(out, "images", name + ".elf"), "wb").write(
            wrap(blob, base=RAW_BASE, entry=RAW_ENTRY))
        print(f"RAW  {name}: EA 0x{ea:08X} -> file 0x{roff:X}, {size} bytes, "
              f"full fp 0x{bsw.fnv1a64(blob):016X}, hashed prefix {raw_hashed_len(name, blob)} B")

    # --- job modules
    hdrs = find_jobmods(d)
    seg_end = {}
    for sva, soff, fs in segs:
        for h in hdrs:
            if soff <= h < soff + fs:
                seg_end[h] = soff + fs
    mods = []
    for n, h in enumerate(hdrs):
        entry, size = struct.unpack_from(">II", d, h + 0x10)
        nxt = hdrs[n + 1] if n + 1 < len(hdrs) else seg_end.get(h, len(d))
        end = min(h + size, nxt, seg_end.get(h, len(d)))
        body = d[h:end]
        sha = hashlib.sha1(body).hexdigest()[:12]
        name = f"jobmod_{sha}_e{entry:04X}_at{h:08X}"
        open(os.path.join(jm, name + ".bin"), "wb").write(body)
        mods.append(dict(name=name, file_off=h, entry=entry, size_field=size, extent=len(body)))
        print(f"JOB  file 0x{h:07X} entry 0x{entry:05X} sizefield 0x{size:05X} "
              f"extent 0x{len(body):05X}{'  (clipped by neighbour)' if end == nxt and nxt < h + size else ''}")
    json.dump(mods, open(os.path.join(out, "jobmods_extract.json"), "w"), indent=1)
    print(f"{len(mods)} job module(s)")


def cmd_lift_pm(a):
    out = os.path.abspath(a.out)
    pm = open(os.path.join(out, "images", PM_NAME + ".bin"), "rb").read()
    elf_path = os.path.join(out, "images", PM_NAME + ".elf")
    outdir = os.path.join(out, PM_NAME)
    srcc = os.path.join(outdir, "spu_recomp.c")
    prefix = bsw.sanitize(PM_NAME) + "_"
    if a.relift or not os.path.exists(srcc):
        os.makedirs(outdir, exist_ok=True)
        cmd = [sys.executable, os.path.join(TOOLS, "spu_lifter.py"), elf_path,
               "--auto-functions", elf_path, "--symbol-prefix", prefix, "-o", outdir,
               "--extra-funcs", PM_EXTRA_FUNCS]
        print("LIFT", PM_NAME)
        if subprocess.run(cmd).returncode != 0:
            sys.exit("PM lift failed")
    for f in ("spu_recomp.h", "spu_recomp.c"):
        bsw.debase_includes(os.path.join(outdir, f))
    fp = bsw.fnv1a64(pm)
    ns = argparse.Namespace(out=os.path.join(out, "spu_workloads.c"), title="tod",
                            register_fn="tod_spu_register_all", constructor=True)
    bsw.write_registry(ns, [(PM_NAME, prefix, fp, PM_BASE)], {}, image_ids={PM_NAME: PM_IMAGE_ID})


def cmd_lift_raw(a):
    out = os.path.abspath(a.out)
    entries = []
    for name, ea, size, image_id in RAW_IMAGES:
        blob = open(os.path.join(out, "images", name + ".bin"), "rb").read()
        elf_path = os.path.join(out, "images", name + ".elf")
        outdir = os.path.join(out, name)       # CMake globs spu/*/spu_recomp.c
        srcc = os.path.join(outdir, "spu_recomp.c")
        prefix = bsw.sanitize(name) + "_"
        if a.relift or not os.path.exists(srcc):
            os.makedirs(outdir, exist_ok=True)
            cmd = [sys.executable, os.path.join(TOOLS, "spu_lifter.py"), elf_path,
                   "--auto-functions", elf_path, "--symbol-prefix", prefix, "-o", outdir]
            print("LIFT", name)
            if subprocess.run(cmd).returncode != 0:
                sys.exit(f"{name} lift failed")
        for f in ("spu_recomp.h", "spu_recomp.c"):
            bsw.debase_includes(os.path.join(outdir, f))
        hl = raw_hashed_len(name, blob)
        entries.append((name, prefix, image_id, bsw.fnv1a64(blob), hl, bsw.fnv1a64(blob[:hl]), len(blob)))
    write_raw_registry(os.path.join(out, "spu_raw_workloads.c"), entries)


def write_raw_registry(path, entries):
    L = ["/* spu_raw_workloads.c -- GENERATED by tools/tod_spu.py lift-raw.",
         " * Registers the raw-SPU collision programs for runtime/spu/spu_raw.c.  The PPU",
         " * streams each into LS 0x60 by proxy DMA; the run [0x60, 0x60+size) is hashed.",
         " *  - exact: FNV-1a-64 of the whole run (a run whose parameter block is unchanged)",
         " *  - prefix: FNV-1a-64 of the first <hashed_len> bytes, for the program whose",
         " *    trailing parameter block the PPU rewrites before every DMA load. */",
         '#include "spu_workload.h"', "",
         "extern void spu_begin_image(int image_id);", ""]
    for name, prefix, iid, ffp, hl, pfp, sz in entries:
        L.append(f"extern void {prefix}spu_func_{RAW_ENTRY:08X}(spu_context*);")
        L.append(f"extern void {prefix}spu_recomp_register(void);")
    L += ["", "void tod_spu_register_raw(void)", "{"]
    for name, prefix, iid, ffp, hl, pfp, sz in entries:
        fn = f"{prefix}spu_func_{RAW_ENTRY:08X}"
        L.append(f"    spu_begin_image({iid}); {prefix}spu_recomp_register();")
        L.append(f"    spu_workload_register_img(0x{ffp:016X}ULL, {fn}, {iid}, \"{name}\");")
        if hl != sz:   # parameter block present: also match on the stable prefix
            L.append(f"    spu_workload_register_prefix(0x{pfp:016X}ULL, 0x{hl:X}u, {fn}, {iid}, \"{name}.prefix\");")
    L += ["    spu_begin_image(0);", "}", "",
          "__attribute__((constructor)) static void tod_spu_register_raw_ctor(void)",
          "{", "    tod_spu_register_raw();", "}", ""]
    open(path, "w", encoding="utf-8", newline="\n").write("\n".join(L))
    print(f"wrote {path} ({len(entries)} raw image(s))")


def cmd_debase(a):
    n = 0
    for h in glob.glob(os.path.join(a.out, "*", "spu_recomp.[ch]")):
        bsw.debase_includes(h); n += 1
    print(f"debased {n} file(s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract"); e.add_argument("--elf", required=True); e.add_argument("--out", required=True)
    l = sub.add_parser("lift-pm"); l.add_argument("--out", required=True); l.add_argument("--relift", action="store_true")
    r = sub.add_parser("lift-raw"); r.add_argument("--out", required=True); r.add_argument("--relift", action="store_true")
    g = sub.add_parser("debase"); g.add_argument("--out", required=True)
    a = ap.parse_args()
    {"extract": cmd_extract, "lift-pm": cmd_lift_pm, "lift-raw": cmd_lift_raw,
     "debase": cmd_debase}[a.cmd](a)


if __name__ == "__main__":
    main()
