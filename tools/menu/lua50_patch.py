#!/usr/bin/env python3
"""Lua 5.0 (little endian, int/size_t/Instruction 4 bytes, number = float32)
undump/dump + Anark built.dat (IGHW) script-section patcher. stdlib only.

Usage:
  python -I lua50_patch.py roundtrip BUILT.DAT          # undump->dump identical?
  python -I lua50_patch.py dis BUILT.DAT [CHUNK_INDEX]  # disassemble (chunk 0 = optHandler)
  python -I lua50_patch.py patch BUILT.DAT OUT.DAT      # add the ANTIALIASING row to optArray
Container: IGHW, 16-byte section entries {id, offset, count, size} from 0x20; the script
section is id 0x20730: [u32 nchunks][u32 rel offsets...][Lua chunks]; offsets are relative
to the first chunk (section start + 0x30 for 9 chunks). Following sections are 0x80-aligned.
"""
import struct
import sys

HDR = bytes.fromhex("1b4c75615001040404060809" "09" "04")  # 1b Lua 50 01 04 04 04 06 08 09 09 04
TESTNUM = bytes.fromhex("3baf ef4b".replace(" ", ""))  # float32 test number as stored by the game tools
OPS = ("MOVE LOADK LOADBOOL LOADNIL GETUPVAL GETGLOBAL GETTABLE SETGLOBAL SETUPVAL SETTABLE "
       "NEWTABLE SELF ADD SUB MUL DIV POW UNM NOT CONCAT JMP EQ LT LE TEST CALL TAILCALL RETURN "
       "FORLOOP TFORLOOP TFORPREP SETLIST SETLISTO CLOSE CLOSURE").split()
OP = {n: i for i, n in enumerate(OPS)}


class R:
    def __init__(s, b, p=0): s.b, s.p = b, p
    def u8(s): v = s.b[s.p]; s.p += 1; return v
    def u32(s): v = struct.unpack_from("<I", s.b, s.p)[0]; s.p += 4; return v
    def raw(s, n): v = s.b[s.p:s.p + n]; s.p += n; return v
    def string(s):
        n = s.u32()
        if n == 0: return None
        return s.raw(n)  # includes trailing NUL


def w32(v): return struct.pack("<I", v)
def wstr(x): return w32(0) if x is None else w32(len(x)) + x


class Proto:
    pass


def load_func(r):
    f = Proto()
    f.source = r.string(); f.linedef = r.u32()
    f.nups, f.nparams, f.vararg, f.maxstack = r.u8(), r.u8(), r.u8(), r.u8()
    f.lines = [r.u32() for _ in range(r.u32())]
    f.locs = [(r.string(), r.u32(), r.u32()) for _ in range(r.u32())]
    f.upvals = [r.string() for _ in range(r.u32())]
    f.k = []
    for _ in range(r.u32()):
        t = r.u8()
        if t == 3: f.k.append((3, r.raw(4)))
        elif t == 4: f.k.append((4, r.string()))
        elif t == 0: f.k.append((0, None))
        else: raise ValueError("bad const type %d" % t)
    f.p = [load_func(r) for _ in range(r.u32())]
    f.code = [r.u32() for _ in range(r.u32())]
    return f


def undump(b):
    r = R(b)
    if r.raw(len(HDR)) != HDR or r.raw(4) != TESTNUM:
        raise ValueError("not Lua 5.0 LE float32 chunk")
    f = load_func(r)
    return f, r.p


def dump_func(f):
    o = [wstr(f.source), w32(f.linedef), bytes((f.nups, f.nparams, f.vararg, f.maxstack))]
    o.append(w32(len(f.lines))); o += [w32(x) for x in f.lines]
    o.append(w32(len(f.locs)))
    for n, a, b in f.locs: o += [wstr(n), w32(a), w32(b)]
    o.append(w32(len(f.upvals))); o += [wstr(x) for x in f.upvals]
    o.append(w32(len(f.k)))
    for t, v in f.k:
        o.append(bytes((t,)))
        if t == 3: o.append(v)
        elif t == 4: o.append(wstr(v))
    o.append(w32(len(f.p))); o += [dump_func(p) for p in f.p]
    o.append(w32(len(f.code))); o += [w32(x) for x in f.code]
    return b"".join(o)


def dump(f): return HDR + TESTNUM + dump_func(f)


# ---- instructions: this build REORDERS the 5.0 fields: op:6 | C:9 @6 | B:9 @15 | A:8 @24 ; Bx = 18 bits @6 ----
def dec(i): return i & 63, i >> 24, (i >> 15) & 511, (i >> 6) & 511  # op,A,B,C
def decbx(i): return (i >> 6) & 0x3FFFF
def enc(op, a, b=0, c=0): return op | (c << 6) | (b << 15) | (a << 24)
def encbx(op, a, bx): return op | (bx << 6) | (a << 24)


def kstr(f, i):
    t, v = f.k[i]
    return v[:-1].decode("latin1") if t == 4 else ("%r" % struct.unpack("<f", v)[0] if t == 3 else "nil")


def disasm(f, depth=0):
    pad = "  " * depth
    print("%sfunction line=%d params=%d stack=%d nk=%d ncode=%d nprotos=%d" %
          (pad, f.linedef, f.nparams, f.maxstack, len(f.k), len(f.code), len(f.p)))
    for pc, ins in enumerate(f.code):
        op, a, b, c = dec(ins); bx = decbx(ins)
        n = OPS[op] if op < len(OPS) else "?%d" % op
        extra = ""
        if n in ("LOADK", "GETGLOBAL", "SETGLOBAL"): extra = "K[%d]=%s" % (bx, kstr(f, bx))
        elif n == "CLOSURE": extra = "proto %d" % bx
        elif n in ("JMP", "FORLOOP", "TFORPREP"): extra = "->%d" % (pc + 1 + (bx - 131071))
        elif n in ("SETLIST", "SETLISTO"): extra = "bx=%d" % bx
        print("%s %4d %-9s A=%-3d B=%-3d C=%-3d %s" % (pad, pc, n, a, b, c, extra))
    for i, p in enumerate(f.p):
        print("%s-- proto %d" % (pad, i)); disasm(p, depth + 1)


# ---- container ----
def sections(d):
    n = struct.unpack_from(">I", d, 8)[0]
    return [list(struct.unpack_from(">4I", d, 0x20 + 16 * i)) for i in range(n)]  # id,off,count,size


def find_script(d):
    for i, s in enumerate(sections(d)):
        if s[0] == 0x20730: return i, s
    raise ValueError("no 0x20730 section")


def split_chunks(d):
    i, s = find_script(d)
    base = s[1]
    n = struct.unpack_from(">I", d, base)[0]
    # NOTE: header words are big endian (0x00000009 ...) -- see below
    offs = list(struct.unpack_from(">%dI" % (n + 2), d, base + 4))
    first = base + 4 + 4 * (n + 2)
    return i, s, n, offs, first


def chunk_bounds(d):
    i, s, n, offs, first = split_chunks(d)
    # offs[0] is 0; chunk k spans [first+offs[k], first+offs[k+1]); the last runs to a tail
    return [(first + offs[k], first + offs[k + 1]) for k in range(n)], first, offs


def ann(p):
    """Annotated disassembly (RK operands resolved)."""
    out = []
    for pc, c in enumerate(p.code):
        op, a, b, cc = dec(c); bx = decbx(c); n = OPS[op] if op < len(OPS) else "?%d" % op
        def rk(x): return ("K[%d]=%s" % (x - 250, kstr(p, x - 250))) if x >= 250 else "R%d" % x
        if n in ("LOADK", "GETGLOBAL", "SETGLOBAL"): s = "%s R%d <- K[%d]=%s" % (n, a, bx, kstr(p, bx))
        elif n in ("GETTABLE", "SELF"): s = "%s A=%d B=R%d C=%s" % (n, a, b, rk(cc))
        elif n == "SETTABLE": s = "SETTABLE R%d[%s]=%s" % (a, rk(b), rk(cc))
        elif n in ("EQ", "LT", "LE", "ADD", "SUB", "MUL", "DIV", "POW", "CONCAT"): s = "%s A=%d B=%s C=%s" % (n, a, rk(b), rk(cc))
        elif n in ("JMP", "FORLOOP", "TFORPREP"): s = "%s ->%d" % (n, pc + 1 + bx - 131071)
        elif n == "CLOSURE": s = "CLOSURE R%d proto%d" % (a, bx)
        else: s = "%s A=%d B=%d C=%d" % (n, a, b, cc)
        out.append("%3d %s" % (pc, s))
    return "\n".join(out)


# ---- the optArray patch ----
# The AA row reuses three natives the Options page does not use (see src/ui/tod_settings_menu.cpp).
AA_LABEL = b"ANTIALIASING:\x00"
AA_HELP = (b"Select the antialiasing mode: OFF, FXAA or MSAA x2/x4/x8. "
           b"The change takes effect the next time the game starts.\x00")
AA_GET = b"get_button_layout\x00"       # -> host get_tod_aa
AA_SET = b"set_button_layout\x00"       # -> host set_tod_aa
AA_TXT = b"is_rumble_on\x00"            # -> host get_tod_aa_text
AA_TYPE = b"aa\x00"
ROW_KEY = 14.0


def kfind(f, t, v, add=False):
    for i, (tt, vv) in enumerate(f.k):
        if tt == t and vv == v:
            return i
    if not add:
        raise KeyError((t, v))
    f.k.append((t, v))
    return len(f.k) - 1


def kstr_i(f, s, add=True):
    return kfind(f, 4, s if s.endswith(b"\0") else s + b"\0", add)


def knum_i(f, x, add=True):
    return kfind(f, 3, struct.pack("<f", x), add)


def RK(i):
    assert i + 250 <= 511, "constant index too large for RK"
    return 250 + i


def row_code(f, label, help_, getn, setn, typ, key, enabled):
    """The instruction sequence the compiler emitted for one optArray row (the encoder is
    verified against the stock 'surround' row in patch_opthandler)."""
    K = lambda s: RK(kstr_i(f, s))
    return [
        enc(OP["GETTABLE"], 1, 0, K(b"optArray")),
        enc(OP["NEWTABLE"], 2, 0, 3),
        enc(OP["SETTABLE"], 2, K(b"optName"), label),
        enc(OP["SETTABLE"], 2, K(b"desc"), help_),
        encbx(OP["GETGLOBAL"], 3, kstr_i(f, getn)),
        enc(OP["SETTABLE"], 2, K(b"get"), 3),
        encbx(OP["GETGLOBAL"], 3, kstr_i(f, setn)),
        enc(OP["SETTABLE"], 2, K(b"set"), 3),
        enc(OP["SETTABLE"], 2, K(b"type"), typ),
        enc(OP["LOADBOOL"], 3, 1 if enabled else 0, 0),
        enc(OP["SETTABLE"], 2, K(b"enabled"), 3),
        enc(OP["SETTABLE"], 1, key, 2),
    ]


def insert_code(f, at, new):
    """Insert instructions at index `at`, fixing every relative jump and the line info.
    A jump whose target is exactly `at` keeps landing on the first inserted instruction."""
    n = len(new)
    mp = lambda t: t if t <= at else t + n
    out = []
    for pc, c in enumerate(f.code):
        op = c & 63
        if OPS[op] in ("JMP", "FORLOOP", "TFORPREP"):
            tgt = pc + 1 + decbx(c) - 131071
            npc = pc if pc < at else pc + n
            c = encbx(op, c >> 24, mp(tgt) - (npc + 1) + 131071)
        out.append(c)
    f.code = out[:at] + list(new) + out[at:]
    f.lines = f.lines[:at] + [f.lines[max(at - 1, 0)]] * n + f.lines[at:]
    return n


def patch_opthandler(f):
    """f = the optHandler chunk root. Adds the ANTIALIASING row and the value-text branch."""
    root = f
    top = next(p for p in root.p if b"set_six_axis_controls\x00" in [v for t, v in p.k if t == 4])
    # 1. row 14 after the last stock row (13 = surround): same shape as that row
    sc = top.code
    stock = row_code(top, (sc[166] >> 6) & 511, (sc[167] >> 6) & 511,
                     b"get_surround_on", b"set_surround_on", (sc[172] >> 6) & 511, (sc[175] >> 15) & 511, True)
    assert stock == sc[164:176], "row encoder mismatch"
    label = RK(kstr_i(top, AA_LABEL))
    help_ = RK(kstr_i(top, AA_HELP))
    typ = RK(kstr_i(top, AA_TYPE))
    key = RK(knum_i(top, ROW_KEY))
    row = row_code(top, label, help_, AA_GET, AA_SET, typ, key, True)
    at = 176
    assert OPS[sc[at] & 63] == "NEWTABLE" and OPS[sc[at - 1] & 63] == "SETTABLE"
    insert_code(top, at, row)
    # 2. updateItemData: value text for type 'aa', ahead of its numeric 'bar' branch
    upd = None
    for p in root.p:
        ks = [v for t, v in p.k if t == 4]
        if b"localizeAxisKey\x00" in ks and b"surroundKey\x00" in ks and b"SetLocalizedText\x00" in ks:
            upd = p
            break
    assert upd is not None, "updateItemData not found"
    at = None
    for pc in range(len(upd.code) - 1):
        if (OPS[upd.code[pc] & 63] == "CALL" and (upd.code[pc] >> 24) == 7
                and OPS[upd.code[pc + 1] & 63] == "GETGLOBAL"):
            at = pc + 1
            break
    assert at is not None
    g = lambda s: kstr_i(upd, s)
    tx = kstr_i(upd, AA_TXT)
    ka = RK(kstr_i(upd, AA_TYPE))
    # R4 = loop index, R7 = get() value (unused here), R8.. scratch
    blk = [
        enc(OP["GETTABLE"], 8, 0, RK(g(b"option"))),
        enc(OP["GETTABLE"], 8, 8, 4),
        enc(OP["GETTABLE"], 8, 8, RK(g(b"type"))),
        enc(OP["EQ"], 0, 8, ka),
        encbx(OP["JMP"], 0, 8 + 131071),                 # not 'aa': skip the 8 instrs below
        encbx(OP["GETGLOBAL"], 8, g(b"SetLocalizedText")),
        enc(OP["GETTABLE"], 9, 0, RK(g(b"row"))),
        enc(OP["GETTABLE"], 9, 9, 4),
        enc(OP["GETTABLE"], 9, 9, RK(g(b"optValue"))),
        encbx(OP["GETGLOBAL"], 10, tx),
        enc(OP["CALL"], 10, 1, 2),
        enc(OP["CALL"], 8, 3, 1),
        encbx(OP["JMP"], 0, 131071),                     # -> loop end (patched below)
    ]
    blk[4] = encbx(OP["JMP"], 0, 8 + 131071)
    insert_code(upd, at, blk)
    fl = next(pc for pc in range(at + len(blk), len(upd.code)) if OPS[upd.code[pc] & 63] == "FORLOOP")
    j = at + len(blk) - 1
    upd.code[j] = encbx(OP["JMP"], 0, fl - (j + 1) + 131071)
    return True


def pad_fill(n):
    """The IGHW tools fill section alignment gaps with the text PAD0PAD1PAD2... (restarting at each gap)."""
    return b"".join(b"PAD%X" % (k & 15) for k in range((n + 3) // 4))[:n]


def rebuild_container(d, chunk_idx, new_chunk):
    """Replace chunk `chunk_idx` of the script section; return the new file bytes."""
    si, sec, n, offs, first = split_chunks(d)
    a, b = first + offs[chunk_idx], first + offs[chunk_idx + 1]
    delta = len(new_chunk) - (b - a)
    sec_off, sec_size = sec[1], sec[3]
    body = bytearray(d[sec_off:a]) + new_chunk + d[b:sec_off + sec_size]
    for k in range(chunk_idx + 1, n + 2):          # offsets table (big endian, after the count)
        v = struct.unpack_from(">I", body, 4 + 4 * k)[0]
        struct.pack_into(">I", body, 4 + 4 * k, v + delta)
    new_size = len(body)
    secs = sections(d)
    nxt_old = secs[si + 1][1]
    new_nxt = (sec_off + new_size + 0x7F) & ~0x7F
    shift = new_nxt - nxt_old
    out = bytearray(d[:sec_off]) + body + pad_fill(new_nxt - sec_off - new_size) + d[nxt_old:]
    for i in range(len(secs)):                     # section table
        base = 0x20 + 16 * i
        if i == si:
            struct.pack_into(">I", out, base + 12, new_size)
        elif i > si:
            struct.pack_into(">I", out, base + 4, secs[i][1] + shift)
    struct.pack_into(">I", out, 16, struct.unpack_from(">I", d, 16)[0] + shift)   # total size
    last = secs[-1]                                # 0x20800: holds the offset of the 0x5200 section
    if last[0] == 0x20800:
        lo = last[1] + shift
        v = struct.unpack_from(">I", out, lo + 4)[0]
        if any(v == s[1] for s in secs):
            struct.pack_into(">I", out, lo + 4, v + shift)
    # File footer: the very last u32 is the offset of the 2nd word of the 0x20800 section
    # (= header total size - 4). It was left stale (old value) and the loader reads the
    # fixup table through it -> frontend rendered black. Always recompute it.
    if last[0] == 0x20800 and struct.unpack_from(">I", d, len(d) - 4)[0] == last[1] + 4:
        struct.pack_into(">I", out, len(out) - 4, last[1] + shift + 4)
    return bytes(out)


def verify_container(orig, new):
    """Structural checks of a patched IGHW against the original; returns a list of problems."""
    bad = []
    so, sn = sections(orig), sections(new)
    if len(so) != len(sn): bad.append("section count")
    if struct.unpack_from(">I", new, 16)[0] != sn[-1][1] + sn[-1][3]: bad.append("hdr total size")
    if struct.unpack_from(">I", new, len(new) - 4)[0] != sn[-1][1] + 4: bad.append("footer stale")
    for a, b in zip(so, sn):
        if a[0] != b[0]: bad.append("id %x" % a[0])
        if b[1] % 0x80: bad.append("unaligned %x" % b[0])
    si = find_script(orig)[0]
    for i, (a, b) in enumerate(zip(so, sn)):
        if i != si and orig[a[1]:a[1] + a[3]] != new[b[1]:b[1] + b[3]] and a[0] != 0x20800:
            bad.append("section %x content changed" % a[0])
    if new[sn[-2][1]:sn[-2][1] + sn[-2][3]] != orig[so[-2][1]:so[-2][1] + so[-2][3]]: bad.append("0x5200 changed")
    if sn[-1][1] != sn[-2][1] + 0x200 and False: pass
    return bad


def patch_file(src, dst):
    d = open(src, "rb").read()
    si, sec, n, offs, first = split_chunks(d)
    ch = d[first + offs[0]: first + offs[1]]
    f, p = undump(ch)
    assert p == len(ch) and dump(f) == ch, "round trip failed"
    patch_opthandler(f)
    out = rebuild_container(d, 0, dump(f))
    bad = verify_container(d, out)
    assert not bad, bad
    open(dst, "wb").write(out)
    return len(d), len(out)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd, path = argv[0], argv[1]
    d = open(path, "rb").read()
    si, sec, n, offs, first = split_chunks(d)
    if cmd == "roundtrip":
        bad = 0
        for k in range(n):
            ch = d[first + offs[k]: first + offs[k + 1]]
            f, p = undump(ch)
            ok = (p == len(ch) and dump(f) == ch)
            bad += not ok
            print("chunk %2d  %6d bytes  %s" % (k, len(ch), "OK" if ok else "MISMATCH"))
        print("%s: %d chunks, %d mismatches" % (path, n, bad))
        return 1 if bad else 0
    if cmd == "dis":
        k = int(argv[2]) if len(argv) > 2 else 0
        f, _ = undump(d[first + offs[k]: first + offs[k + 1]])

        def walk(p, dep=0):
            print("%sfunction ncode=%d nk=%d nprotos=%d" % ("  " * dep, len(p.code), len(p.k), len(p.p)))
            print("\n".join("  " * dep + l for l in ann(p).split("\n")))
            for i, q in enumerate(p.p):
                print("%s-- proto %d" % ("  " * dep, i))
                walk(q, dep + 1)
        walk(f)
        return 0
    if cmd == "patch":
        a, b = patch_file(path, argv[2])
        print("%s -> %s  (%d -> %d bytes)" % (path, argv[2], a, b))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
