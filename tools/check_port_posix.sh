#!/usr/bin/env bash
# Compile every port source (src/) to an object file on macOS/Linux, without a lift.
#
# The port calls lifted game functions by name (func_<EU address>). This makes a stand-in
# ppu_recomp.h: ps3recomp's smoke-title header (the lifter's real preamble and ABI) plus a
# declaration for every func_ the port references, so the port's own code is compiled exactly
# as in a real build. Nothing is linked. Used to keep the non-renderer port building on macOS
# until the full game links there.
#
#   tools/check_port_posix.sh [BUILD_DIR]     (default build_posix_check)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build_posix_check}"
PY="$ROOT/.venv/bin/python"
[ -x "$PY" ] || PY=python3
mkdir -p "$OUT/hdr" "$OUT/gen" "$OUT/obj"

"$PY" -I ps3recomp/tools/make_smoke_elf.py --out "$OUT/hdr/smoke.elf" --header "$OUT/hdr/ppu_recomp_base.h" >/dev/null
{
  echo '#pragma once'
  echo '#include "ppu_recomp_base.h"'
  echo '#ifdef __cplusplus'
  echo 'extern "C" {'
  echo '#endif'
  grep -rhoE 'func_[0-9A-Fa-f]{8}(_lifted)?' src | sort -u | sed 's/.*/void &(ppu_context* ctx);/'
  echo '#ifdef __cplusplus'
  echo '}'
  echo '#endif'
} > "$OUT/hdr/ppu_recomp.h"

# gen/prompts_data.h as CMakeLists.txt makes it (the PNGs as byte arrays).
{
  echo '/* Generated from src/res/prompts by tools/check_port_posix.sh. */'
  for set in ps4 ps5 xbox switch keyboard; do
    printf 'static const unsigned char kPromptPng_%s[] = {' "$set"
    od -An -v -tx1 "src/res/prompts/$set.png" | tr -s ' \n' ' ' | sed 's/ \([0-9a-f][0-9a-f]\)/0x\1,/g'
    echo '};'
  done
} > "$OUT/gen/prompts_data.h"

P=ps3recomp
INC=(-I"$OUT/hdr" -I"$P/include" -I"$P/runtime/ppu" -I"$P/runtime/spu" -I"$P/runtime/memory"
     -I"$P/runtime/syscalls" -I"$P/runtime/platform" -I"$P/libs/video" -I"$P/libs/input"
     -I. -I"$OUT/gen" -Isrc -Ithird_party/imgui)
[ -d /opt/homebrew/include ] && INC+=(-I/opt/homebrew/include)

SRCS=$(find src -name '*.cpp' ! -name tod_overlay.cpp | sort)
fail=0; n=0
for f in $SRCS; do
  o="$OUT/obj/$(echo "$f" | tr '/' '_').o"
  if c++ -std=c++20 -O1 -w -c "${INC[@]}" -fno-strict-aliasing "$f" -o "$o" 2>"$o.err"; then
    n=$((n+1)); rm -f "$o.err"
  else
    fail=$((fail+1)); echo "FAIL $f"; head -5 "$o.err"
  fi
done
echo "port sources compiled: $n ok, $fail failed ($(uname -s) $(uname -m))"
[ "$fail" -eq 0 ]
