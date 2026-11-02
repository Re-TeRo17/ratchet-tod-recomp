#!/usr/bin/env python3
"""Read-only single-entry extractor built on tools/psarc_scan.py (stdlib only).

Usage:
  python -I psarc_extract.py ARCHIVE OUTDIR INDEX [INDEX ...] [--head N]

INDEX is the 1-based manifest index (matches out/psarc/<name>.manifest.txt).
--head N writes only the first N bytes of each entry (for headers / big files).
Writes OUTDIR/<index>__<leafname>. Reads the archive only.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCAN = os.path.normpath(os.path.join(HERE, "..", "psarc_scan.py"))


def load_scan():
    spec = importlib.util.spec_from_file_location("psarc_scan", SCAN)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main(argv):
    head = None
    if "--head" in argv:
        k = argv.index("--head")
        head = int(argv[k + 1])
        del argv[k:k + 2]
    if len(argv) < 3:
        print(__doc__)
        return 2
    archive, outdir = argv[0], argv[1]
    indices = [int(x) for x in argv[2:]]
    scan = load_scan()
    os.makedirs(outdir, exist_ok=True)
    with open(archive, "rb") as f:
        h, ents, blocks = scan.read_index(f)
        names = [None] * h["toc_entries"]
        # manifest is entry 0
        text = scan.read_entry(f, h, blocks, ents[0]).decode("utf-8", "replace")
        mf = [x.rstrip("\r") for x in text.split("\n") if x != ""]
        for idx in indices:
            if not 1 <= idx < h["toc_entries"]:
                print("index %d out of range" % idx)
                continue
            nm = mf[idx - 1] if idx - 1 < len(mf) else "<unnamed %d>" % idx
            leaf = nm.replace("\\", "/").split("/")[-1]
            data = scan.read_entry(f, h, blocks, ents[idx], max_bytes=head)
            out = os.path.join(outdir, "%d__%s" % (idx, scan.safe_name(leaf)))
            with open(out, "wb") as of:
                of.write(data)
            print("%d\t%s\t%d bytes written (of %d)\t%s" % (idx, nm, len(data), ents[idx]["usize"], out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
