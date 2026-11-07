#!/usr/bin/env python3
"""Make the patched Options page from the USER's own global_cached.psarc (stdlib only; nothing of the game is
stored in this repository: the patch is our Lua source, compiled here, spliced into the user's data).

  python -I menu_patch.py SRC_global_cached.psarc OUT_MODS_DIR [--lua optHandler.lua] [--entries 40,86] [--no-inplace]

Writes the patched archive TWICE, where the port's mods folder (src/patches/mods.cpp, TOD_MODS_DIR) looks:
  OUT/PS3_GAME/USRDIR/packed/game/global_cached.psarc        (the disc copy, read first)
  OUT/game/BCES00052/USRDIR/packed/game/global_cached.psarc  (the HDD install copy the game then runs from)
The game opens both and reads the table of contents of each; a file that differs from the other breaks the boot,
so both get the same bytes. The original file is only read.

What changes: entry 40 (/built/anark/frontend/built.dat) and entry 86 (/built/anark/pause/built.dat), chunk 0
(= the "optHandler" Lua script) replaced by tools/menu/lua/optHandler.lua compiled with lua50c.py.
Entries are located by their names in the archive's manifest (entry 0), not by number.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lua50_patch as L   # noqa: E402
import lua50c             # noqa: E402
import psarc_extract as X  # noqa: E402
import psarc_repack as R  # noqa: E402

TARGETS = ("/built/anark/frontend/built.dat", "/built/anark/pause/built.dat")


def find_entries(f, ents, sizes, bs):
    names = X.extract(f, ents, sizes, bs, 0).decode("latin1").split("\n")
    names = [n.strip("\r\0 ") for n in names if n.strip("\r\0 ")]
    # manifest line k is the name of TOC entry k+1 (entry 0 is the manifest itself)
    return {names[i]: i + 1 for i in range(len(names)) if names[i] in TARGETS}


def strip_debug(f):
    f.lines, f.locs = [], []
    for q in f.p:
        strip_debug(q)


def fit_chunk(proto, size):
    """The chunk bytes, exactly `size` long (the stock chunk's slot): debug info (line numbers, local names) is dropped and
    an unused string constant pads the rest. A chunk of another length moves every later section of the container and the
    game then fails to load the LAST script ("bad code in InputIntercept", 05/11 menu106); the same size does not."""
    import copy
    q = copy.deepcopy(proto)
    strip_debug(q)
    n = len(L.dump(q))
    if n > size:
        raise SystemExit("chunk is %d bytes without debug info, slot is %d: shrink the script" % (n, size))
    pad = size - n
    if pad:
        if pad < 6:
            raise SystemExit("pad of %d bytes cannot be filled (a constant needs 6)" % pad)
        q.k.append((4, b"P" * (pad - 5 - 1) + b"\0"))
    data = L.dump(q)
    assert len(data) == size, (len(data), size)
    return data


def patch_dat(data, proto):
    si, sec, n, offs, first = L.split_chunks(data)
    old, p = L.undump(data[first + offs[0]:first + offs[1]])
    if old.source != b"optHandler\x00":
        raise SystemExit("chunk 0 is %r, not optHandler: unknown built.dat" % (old.source,))
    out = L.rebuild_container(data, 0, fit_chunk(proto, offs[1] - offs[0]))
    bad = L.verify_container(data, out)
    if bad:
        raise SystemExit("patched container fails its checks: %s" % bad)
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    src, outdir = argv[0], argv[1]
    lua = os.path.join(HERE, "lua", "optHandler.lua")
    inplace = True
    i = 2
    while i < len(argv):
        if argv[i] == "--lua": lua = argv[i + 1]; i += 2
        elif argv[i] == "--no-inplace": inplace = False; i += 1
        else: raise SystemExit("unknown argument " + argv[i])
    proto = lua50c.compile_chunk(open(lua, encoding="latin1").read(), "optHandler")
    tmp = os.path.join(outdir, "_tmp")
    os.makedirs(tmp, exist_ok=True)
    repl = []
    with open(src, "rb") as f:
        hdr, ents, sizes, bs, w = X.read_toc(f)
        found = find_entries(f, ents, sizes, bs)
        if len(found) != len(TARGETS):
            raise SystemExit("anark built.dat entries not found in the manifest: %s" % sorted(found))
        for name in TARGETS:
            idx = found[name]
            new = patch_dat(X.extract(f, ents, sizes, bs, idx), proto)
            p = os.path.join(tmp, "%d.bin" % idx)
            open(p, "wb").write(new)
            repl.append("%d=%s" % (idx, p))
            print("%s: entry %d patched (%d bytes)" % (name, idx, len(new)))
    first = os.path.join(outdir, "PS3_GAME", "USRDIR", "packed", "game", "global_cached.psarc")
    second = os.path.join(outdir, "game", "BCES00052", "USRDIR", "packed", "game", "global_cached.psarc")
    os.makedirs(os.path.dirname(first), exist_ok=True)
    os.makedirs(os.path.dirname(second), exist_ok=True)
    R.main((["--inplace"] if inplace else []) + [src, first] + repl)
    import shutil
    shutil.copyfile(first, second)
    for p in repl:
        os.remove(p.split("=", 1)[1])
    os.rmdir(tmp)
    print("wrote", first, "and", second)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
