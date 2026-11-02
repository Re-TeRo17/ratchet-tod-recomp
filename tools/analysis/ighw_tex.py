#!/usr/bin/env python3
"""Read-only IGHW (.tp/.tph) inspector and texture decoder (stdlib + Pillow).

Usage:
  python -I ighw_tex.py dump FILE [--records N]
      Prints the big-endian u32 header, the section table and the 32-byte
      records from 0x80 (hex words, one line each).
  python -I ighw_tex.py decode TP OFFSET FMT W H OUT.png [--swizzle]
      Decodes a single surface starting at byte OFFSET of TP.
      FMT: dxt1 | dxt3 | dxt5 | argb8 | rgb565 | argb4444 | argb1555.
      --swizzle: Morton (RSX/PS3 swizzled) layout for uncompressed formats.
  python -I ighw_tex.py overlay PNG DAT OUT.png
      Draws the U+E001..U+E012 rectangles (font.dat records at 0x40180) on PNG.

Never writes outside the path given on the command line. Reads only.
"""
import struct
import sys


# ---------------------------------------------------------------- container

def u32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def header(b):
    if b[:4] != b"IGHW":
        raise ValueError("not IGHW")
    return {
        "version": "%08x" % u32(b, 4),
        "count_or_flags": u32(b, 8),
        "tab_off": u32(b, 0xC),
        "file_size": u32(b, 0x10),
        "sentinel": "%08x%08x" % (u32(b, 0x18), u32(b, 0x1C)),
    }


def records(b, start=0x80, size=0x20):
    out = []
    o = start
    while o + size <= len(b):
        out.append((o, struct.unpack_from(">8I", b, o)))
        o += size
    return out


# ---------------------------------------------------------------- DXT

def _rgb565(c):
    r = (c >> 11) & 31
    g = (c >> 5) & 63
    bl = c & 31
    return ((r * 255) // 31, (g * 255) // 63, (bl * 255) // 31)


def _dxt_color(block, o, four_color):
    c0, c1 = struct.unpack_from("<HH", block, o)
    idx = struct.unpack_from("<I", block, o + 4)[0]
    r0, g0, b0 = _rgb565(c0)
    r1, g1, b1 = _rgb565(c1)
    if four_color or c0 > c1:
        pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
               ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255),
               ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255)]
    else:
        pal = [(r0, g0, b0, 255), (r1, g1, b1, 255),
               ((r0 + r1) // 2, (g0 + g1) // 2, (b0 + b1) // 2, 255),
               (0, 0, 0, 0)]
    return [pal[(idx >> (2 * i)) & 3] for i in range(16)]


def _dxt_alpha_explicit(block, o):
    bits = int.from_bytes(block[o:o + 8], "little")
    return [((bits >> (4 * i)) & 15) * 17 for i in range(16)]


def _dxt_alpha_interp(block, o):
    a0, a1 = block[o], block[o + 1]
    bits = int.from_bytes(block[o + 2:o + 8], "little")
    if a0 > a1:
        pal = [a0, a1] + [((8 - i) * a0 + (i - 1) * a1) // 7 for i in range(2, 8)]
    else:
        pal = [a0, a1] + [((6 - i) * a0 + (i - 1) * a1) // 5 for i in range(2, 6)] + [0, 255]
    return [pal[(bits >> (3 * i)) & 7] for i in range(16)]


def decode_dxt(data, w, h, kind):
    """Returns RGBA bytes (w*h*4), w and h multiples of 4 handled by cropping."""
    bw, bh = (w + 3) // 4, (h + 3) // 4
    bsz = 8 if kind == "dxt1" else 16
    need = bw * bh * bsz
    if len(data) < need:
        raise ValueError("data too short: %d < %d" % (len(data), need))
    out = bytearray(w * h * 4)
    for by in range(bh):
        for bx in range(bw):
            o = (by * bw + bx) * bsz
            if kind == "dxt1":
                px = _dxt_color(data, o, False)
            else:
                if kind == "dxt3":
                    alpha = _dxt_alpha_explicit(data, o)
                else:
                    alpha = _dxt_alpha_interp(data, o)
                px = _dxt_color(data, o + 8, True)
                px = [(p[0], p[1], p[2], alpha[i]) for i, p in enumerate(px)]
            for i, p in enumerate(px):
                x = bx * 4 + (i & 3)
                y = by * 4 + (i >> 2)
                if x < w and y < h:
                    k = (y * w + x) * 4
                    out[k:k + 4] = bytes(p)
    return bytes(out)


# ---------------------------------------------------------------- uncompressed

def _morton_index(x, y):
    """Interleave bits of x (even) and y (odd)."""
    m = 0
    bit = 0
    while (x >> bit) or (y >> bit):
        m |= ((x >> bit) & 1) << (2 * bit)
        m |= ((y >> bit) & 1) << (2 * bit + 1)
        bit += 1
    return m


def unswizzle(data, w, h, bpp):
    """Morton-order (square power-of-two region, rest linear) to linear."""
    out = bytearray(w * h * bpp)
    for y in range(h):
        for x in range(w):
            src = _morton_index(x, y) * bpp
            dst = (y * w + x) * bpp
            if src + bpp <= len(data):
                out[dst:dst + bpp] = data[src:src + bpp]
    return bytes(out)


def decode_uncompressed(data, w, h, fmt, swizzle):
    bpp = {"argb8": 4, "rgb565": 2, "argb4444": 2, "argb1555": 2}[fmt]
    if swizzle:
        data = unswizzle(data, w, h, bpp)
    out = bytearray(w * h * 4)
    if fmt == "argb8":  # stored B,G,R,A (little-endian ARGB word)
        for i in range(w * h):
            b, g, r, a = data[4 * i:4 * i + 4]
            out[4 * i:4 * i + 4] = bytes((r, g, b, a))
    elif fmt == "rgb565":
        for i in range(w * h):
            c = struct.unpack_from("<H", data, 2 * i)[0]
            r, g, b = _rgb565(c)
            out[4 * i:4 * i + 4] = bytes((r, g, b, 255))
    elif fmt == "argb4444":
        for i in range(w * h):
            c = struct.unpack_from("<H", data, 2 * i)[0]
            a, r, g, b = [((c >> s) & 15) * 17 for s in (12, 8, 4, 0)]
            out[4 * i:4 * i + 4] = bytes((r, g, b, a))
    elif fmt == "argb1555":
        for i in range(w * h):
            c = struct.unpack_from("<H", data, 2 * i)[0]
            a = 255 if c & 0x8000 else 0
            r, g, b = [((c >> s) & 31) * 255 // 31 for s in (10, 5, 0)]
            out[4 * i:4 * i + 4] = bytes((r, g, b, a))
    return bytes(out)


def decode_surface(data, w, h, fmt, swizzle=False):
    if fmt in ("dxt1", "dxt3", "dxt5"):
        return decode_dxt(data, w, h, fmt)
    return decode_uncompressed(data, w, h, fmt, swizzle)


def surface_size(w, h, fmt):
    if fmt == "dxt1":
        return ((w + 3) // 4) * ((h + 3) // 4) * 8
    if fmt in ("dxt3", "dxt5"):
        return ((w + 3) // 4) * ((h + 3) // 4) * 16
    return w * h * {"argb8": 4, "rgb565": 2, "argb4444": 2, "argb1555": 2}[fmt]


# ---------------------------------------------------------------- CLI

def cmd_dump(path, nrec):
    b = open(path, "rb").read()
    h = header(b)
    print("file", path, "size", len(b))
    for k, v in h.items():
        print("  %s = %s" % (k, v if isinstance(v, str) else "0x%x" % v))
    print("  section table @0x20 (u32 words):")
    for o in range(0x20, min(0x80, len(b)), 16):
        ws = struct.unpack_from(">4I", b, o)
        print("    %04x: %s" % (o, " ".join("%08x" % x for x in ws)))
    print("  records @0x80, 32 B each (u32 words):")
    for n, (o, ws) in enumerate(records(b)):
        if n >= nrec:
            break
        print("    %04x: %s" % (o, " ".join("%08x" % x for x in ws)))


FMT_BYTE = {0x86: "dxt1", 0x88: "dxt5", 0xA8: "dxt5-LN", 0x85: "argb8", 0xA5: "argb8-LN"}


def cmd_list(path, base=0x80):
    """Texture table from 32-byte records: w0 = offset from base, w1 byte2 = format,
    w6 = (width, height) as u16 pairs. Size = distance to next offset (or section end)."""
    b = open(path, "rb").read()
    recs = []
    for o, ws in records(b):
        w0, w1, w6 = ws[0], ws[1], ws[6]
        fb = (w1 >> 8) & 0xFF
        recs.append((o, base + w0, FMT_BYTE.get(fb, "0x%02x" % fb),
                     (w6 >> 16) & 0xFFFF, w6 & 0xFFFF, ws[5], ws[3]))
    ends = [r[1] for r in recs[1:]] + [len(b)]
    for (o, off, f, w, h, w5, w3), end in zip(recs, ends):
        print("rec@%04x off=0x%06x %-9s %4dx%-4d span=%7d w5=%08x w3=%08x" % (o, off, f, w, h, end - off, w5, w3))


def hud_surfaces(tph_path, base=0x80):
    """(index, file_offset, fmt, w, h) for every record; fmt is dxt1/dxt5 from FMT_BYTE."""
    b = open(tph_path, "rb").read()
    res = []
    for n, (o, ws) in enumerate(records(b)):
        fb = (ws[1] >> 8) & 0xFF
        fmt = "dxt1" if fb == 0x86 else "dxt5"
        res.append((n, base + ws[0], fmt, (ws[6] >> 16) & 0xFFFF, ws[6] & 0xFFFF))
    return res


def cmd_sheet(tp_path, tph_path, out, cell=96, cols=16):
    """Contact sheet of every surface (base level), thumbnails with record index."""
    from PIL import Image, ImageDraw
    data = open(tp_path, "rb").read()
    surfs = hud_surfaces(tph_path)
    rows = (len(surfs) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell, rows * (cell + 12)), (40, 40, 40, 255))
    dr = ImageDraw.Draw(sheet)
    for n, off, fmt, w, h in surfs:
        if w == 0 or h == 0:
            continue
        size = surface_size(w, h, fmt)
        im = Image.frombytes("RGBA", (w, h), decode_surface(data[off:off + size], w, h, fmt))
        im.thumbnail((cell - 4, cell - 4))
        cx, cy = (n % cols) * cell, (n // cols) * (cell + 12)
        sheet.paste(im, (cx + 2, cy + 12), im)
        dr.text((cx + 2, cy + 1), "%d %dx%d" % (n, w, h), fill=(255, 255, 0, 255))
    sheet.save(out)
    print("wrote", out, len(surfs), "surfaces")


def cmd_gallery(tp_path, tph_path, out, idxs, scale=2):
    """Side-by-side 2x view of selected surfaces (by record index), labelled."""
    from PIL import Image, ImageDraw
    data = open(tp_path, "rb").read()
    surfs = {s[0]: s for s in hud_surfaces(tph_path)}
    ims = []
    for n in idxs:
        _, off, fmt, w, h = surfs[n]
        size = surface_size(w, h, fmt)
        im = Image.frombytes("RGBA", (w, h), decode_surface(data[off:off + size], w, h, fmt))
        ims.append((n, im.resize((w * scale, h * scale), Image.NEAREST)))
    W = sum(im.width for _, im in ims) + 8 * len(ims)
    H = max(im.height for _, im in ims) + 14
    sheet = Image.new("RGBA", (W, H), (40, 40, 40, 255))
    dr = ImageDraw.Draw(sheet)
    x = 0
    for n, im in ims:
        dr.text((x + 2, 1), "rec %d" % n, fill=(255, 255, 0, 255))
        sheet.paste(im, (x, 14), im)
        x += im.width + 8
    sheet.save(out)
    print("wrote", out)


def cmd_decode(tp, off, fmt, w, h, out, swizzle):
    from PIL import Image
    data = open(tp, "rb").read()[off:]
    size = surface_size(w, h, fmt)
    rgba = decode_surface(data[:size], w, h, fmt, swizzle)
    Image.frombytes("RGBA", (w, h), rgba).save(out)
    print("wrote", out, "from", tp, "off", off, "fmt", fmt, "%dx%d" % (w, h), "swizzle", swizzle)


def cmd_overlay(png, dat, out):
    from PIL import Image, ImageDraw
    im = Image.open(png).convert("RGBA")
    d = open(dat, "rb").read()
    dr = ImageDraw.Draw(im)
    for i in range(0x40180, len(d) - 15, 16):
        cp = struct.unpack_from(">H", d, i)[0]
        if not 0xE001 <= cp <= 0xE012:
            continue
        x, y = struct.unpack_from(">HH", d, i + 2)
        w, h = d[i + 10], d[i + 11]  # verified: doc said +8/+9, real bytes are +10/+11
        dr.rectangle([x, y, x + w - 1, y + h - 1], outline=(255, 0, 255, 255), width=1)
        dr.text((x + 2, y + 2), "%X" % (cp - 0xE000), fill=(255, 255, 0, 255))
    im.save(out)
    print("wrote", out)


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(2)
    if a[0] == "dump":
        n = int(a[a.index("--records") + 1]) if "--records" in a else 64
        cmd_dump(a[1], n)
    elif a[0] == "decode":
        cmd_decode(a[1], int(a[2], 0), a[3], int(a[4]), int(a[5]), a[6], "--swizzle" in a)
    elif a[0] == "list":
        cmd_list(a[1])
    elif a[0] == "sheet":
        cmd_sheet(a[1], a[2], a[3])
    elif a[0] == "gallery":
        cmd_gallery(a[1], a[2], a[3], [int(x) for x in a[4:]])
    elif a[0] == "overlay":
        cmd_overlay(a[1], a[2], a[3])
    else:
        print(__doc__)
        sys.exit(2)
