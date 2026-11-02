#!/usr/bin/env python3
"""Read-only in-memory grep over text entries of PSARC archives (stdlib only).

Usage: python -I grep_psarc.py REGEX EXT_SUFFIX ARCHIVE [ARCHIVE ...]
  e.g. python -I grep_psarc.py "follow_cam" .lua  <archive>
Lists entries whose content matches REGEX, with matching lines (max 3 per entry).
Writes nothing to disk.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCAN = os.path.normpath(os.path.join(HERE, "..", "psarc_scan.py"))


def main(argv):
    spec = importlib.util.spec_from_file_location("psarc_scan", SCAN)
    scan = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(scan)
    rx = re.compile(argv[0])
    ext = argv[1].lower()
    for arch in argv[2:]:
        with open(arch, "rb") as f:
            h, ents, blocks = scan.read_index(f)
            if h["compression"] != "zlib":
                print("skip (compression %r): %s" % (h["compression"], arch))
                continue
            names = [x.rstrip("\r") for x in scan.read_entry(f, h, blocks, ents[0]).decode("utf-8", "replace").split("\n") if x]
            for i in range(1, h["toc_entries"]):
                nm = names[i - 1] if i - 1 < len(names) else ""
                if not nm.lower().endswith(ext):
                    continue
                try:
                    data = scan.read_entry(f, h, blocks, ents[i]).decode("utf-8", "replace")
                except Exception as e:
                    print("error %s: %s" % (nm, e))
                    continue
                lines = [l for l in data.split("\n") if rx.search(l)]
                if lines:
                    print("%s [%d] %s" % (os.path.basename(arch), i, nm))
                    for l in lines[:3]:
                        print("    " + l.strip()[:200])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
