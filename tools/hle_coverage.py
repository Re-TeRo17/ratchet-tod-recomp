"""Which of the EBOOT's firmware imports ps3recomp's HLE libraries implement.

    python tools/hle_coverage.py <ps3recomp dir> <EBOOT.imports.json>

An import is covered when a non-static cell*/sys*/sce* function in the
runtime's sources hashes to its NID (the rule gen_hle_nids.py registers by).
Names for the NIDs nothing implements come from nid_database.py and
nid_from_protos.json where they know them.
"""
import glob
import json
import os
import sys
from collections import defaultdict


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    root, imports_path = sys.argv[1], sys.argv[2]
    sys.path.insert(0, os.path.join(root, "tools"))
    import gen_hle_nids
    import nid_database

    implemented = {}
    for c in glob.glob(os.path.join(root, "libs", "**", "*.c"), recursive=True) + \
             glob.glob(os.path.join(root, "runtime", "**", "*.c*"), recursive=True):
        for nid, name in gen_hle_nids.scan(c):
            implemented.setdefault(nid, (name, os.path.relpath(c, root)))

    names = {}
    protos = os.path.join(root, "tools", "nid_from_protos.json")
    if os.path.exists(protos):
        data = json.load(open(protos))
        items = data.items() if isinstance(data, dict) else []
        for k, v in items:
            try:
                names[int(k, 16) if isinstance(k, str) else int(k)] = v if isinstance(v, str) else str(v)
            except ValueError:
                names[nid_database.compute_nid(k)] = k
    db = getattr(nid_database, "BUILTIN_NIDS", None) or getattr(nid_database, "KNOWN_NIDS", None) or {}
    for k, v in (db.items() if isinstance(db, dict) else []):
        if isinstance(k, int):
            names.setdefault(k, v if isinstance(v, str) else str(v))
        elif isinstance(k, str):
            names.setdefault(nid_database.compute_nid(k), k)

    by_lib = defaultdict(list)
    for imp in json.load(open(imports_path)):
        nid = int(imp["nid"], 16)
        hit = implemented.get(nid)
        by_lib[imp["library"]].append((nid, hit[0] if hit else names.get(nid, "?"), bool(hit)))

    total = sum(len(v) for v in by_lib.values())
    covered = sum(ok for v in by_lib.values() for _, _, ok in v)
    print(f"{covered}/{total} imports implemented\n")
    for lib in sorted(by_lib):
        rows = by_lib[lib]
        print(f"{lib}: {sum(ok for *_, ok in rows)}/{len(rows)}")
        for nid, name, ok in sorted(rows, key=lambda r: (r[2], r[1])):
            if not ok:
                print(f"    MISSING 0x{nid:08X} {name}")
    print()


if __name__ == "__main__":
    main()
