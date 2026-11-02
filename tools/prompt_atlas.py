#!/usr/bin/env python3
"""Draw the button-prompt atlases the port shows in place of the game's.

The game's text draws a button as one private-use character, U+E001..U+E012,
taken from a 256x256 DXT5 atlas (/built/fonts/rcf_scea/font.tp, record 3) at
the rectangles /built/fonts/rcf_scea/font.dat lists (16-byte records from
0x40180: u16 BE codepoint, u16 x, u16 y, ..., u8 w at +10, u8 h at +11; see
docs/TEXTURES.md). This draws the same 18 rectangles for other pads at four
times the resolution (1024x1024 RGBA) and writes src/res/prompts/<set>.png,
which the build embeds (CMakeLists.txt -> gen/prompts_data.h) and
src/patches/prompts.cpp serves in place of the game's atlas.

Nothing here comes from the game's art: only the rectangles (numbers) are the
game's; every glyph is drawn from scratch with Pillow shapes and text.

Sets: xbox, ps4, ps5, switch, keyboard. The keyboard set shows the keys of
src/input/host_input.cpp: Z (cross) X (circle) A (square) S (triangle),
Q/W (L1/R1), 1/2 (L2/R2), Enter (START), Tab (SELECT), the arrow keys (D-pad
and the left stick, which that file maps to the same keys).

usage: python -I tools/prompt_atlas.py [--font TTF] [--out DIR] [--font-dat FILE] [SET ...]

Font: the committed PNGs were drawn with Segoe UI Bold
(C:\\Windows\\Fonts\\segoeuib.ttf, the default of --font). Only the rasterised
labels ("L1", "OPTIONS", "Enter"...) end up in the PNGs. Needs Pillow.
"""
import argparse
import math
import os
import struct

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W = H = 256                      # the game's atlas, in its own texels
Z = 8                            # drawing scale; the result is resized to SCALE x
SCALE = 4                        # output 1024x1024
DEFAULT_FONT = r"C:\Windows\Fonts\segoeuib.ttf"

# codepoint -> (x, y, w, h) in the game's atlas texels. Derived from font.dat
# (records at 0x40180, u16 BE codepoint, x, y; w, h = bytes at +10, +11).
RECTS = {
    0xE001: (116, 91, 52, 52),   # circle
    0xE002: (172, 91, 52, 52),   # square
    0xE003: (4, 147, 52, 52),    # cross
    0xE004: (60, 147, 52, 52),   # triangle
    0xE005: (4, 39, 52, 39),     # SELECT
    0xE006: (60, 39, 52, 39),    # START
    0xE007: (4, 4, 52, 31),      # L1
    0xE008: (60, 4, 52, 31),     # L2
    0xE009: (116, 39, 52, 48),   # L3
    0xE00A: (172, 39, 52, 48),   # left stick
    0xE00B: (116, 4, 52, 31),    # R1
    0xE00C: (172, 4, 52, 31),    # R2
    0xE00D: (4, 91, 52, 48),     # R3
    0xE00E: (60, 91, 52, 48),    # right stick
    0xE00F: (116, 147, 52, 52),  # D-pad down
    0xE010: (172, 147, 52, 52),  # D-pad up
    0xE011: (4, 203, 52, 52),    # D-pad right
    0xE012: (60, 203, 52, 52),   # D-pad left
}


def read_font_dat(path):
    """RECTS from a real font.dat (optional; the table above is what it holds)."""
    b = open(path, "rb").read()
    out = {}
    for i in range(18):
        o = 0x40180 + 16 * i
        cp, x, y = struct.unpack_from(">HHH", b, o)
        out[cp] = (x, y, b[o + 10], b[o + 11])
    return out


WHITE = (240, 240, 240, 255)
GREY = (150, 154, 160, 255)
RIM = (150, 154, 160, 255)
LIT = (92, 222, 200, 255)
DARK = (44, 46, 52, 255)
PS_COLOURS = {"cross": (124, 160, 228), "circle": (240, 96, 88), "square": (232, 124, 190), "triangle": (64, 208, 190)}
PS_PLAIN = {k: (226, 230, 238) for k in PS_COLOURS}

# face: PlayStation position -> (glyph, colour); "shape" draws the position's own symbol
SETS = {
    "ps4": dict(face={k: ("shape", c) for k, c in PS_COLOURS.items()},
                l1="L1", l2="L2", r1="R1", r2="R2", start=("oval", "OPTIONS"), select=("oval", "SHARE"),
                ls="L", rs="R", l3="L3", r3="R3"),
    "ps5": dict(face={k: ("shape", c) for k, c in PS_PLAIN.items()},
                l1="L1", l2="L2", r1="R1", r2="R2", start=("menu", None), select=("create", None),
                ls="L", rs="R", l3="L3", r3="R3"),
    "xbox": dict(face={"cross": ("A", (108, 196, 60)), "circle": ("B", (232, 64, 52)),
                       "square": ("X", (56, 132, 232)), "triangle": ("Y", (240, 188, 36))},
                 l1="LB", l2="LT", r1="RB", r2="RT", start=("menu", None), select=("view", None),
                 ls="L", rs="R", l3="LS", r3="RS"),
    "switch": dict(face={"cross": ("B", WHITE[:3]), "circle": ("A", WHITE[:3]), "square": ("Y", WHITE[:3]),
                         "triangle": ("X", WHITE[:3])},
                   l1="L", l2="ZL", r1="R", r2="ZR", start=("plus", None), select=("minus", None),
                   ls="L", rs="R", l3="LS", r3="RS"),
    # keys as bound in src/input/host_input.cpp keyboard_buttons()
    "keyboard": dict(keys=dict(cross="Z", circle="X", square="A", triangle="S", l1="Q", r1="W", l2="1", r2="2",
                               start="Enter", select="Tab")),
}
FACE_GLYPH = {"circle": 0xE001, "square": 0xE002, "cross": 0xE003, "triangle": 0xE004}


class Pen:
    """Drawing in the atlas's texels on a canvas Z times larger."""

    def __init__(self, font):
        self.z = Z
        self.im = Image.new("RGBA", (W * Z, H * Z), (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)
        self.font = font

    def P(self, *v):
        return [c * self.z for c in v]

    def disc(self, cx, cy, r, fill):
        self.d.ellipse(self.P(cx - r, cy - r, cx + r, cy + r), fill=fill)

    def ring(self, cx, cy, r, col, w):
        self.d.ellipse(self.P(cx - r, cy - r, cx + r, cy + r), outline=col, width=max(1, int(w * self.z)))

    def body(self, cx, cy, r, top=(86, 90, 96), bot=(22, 23, 26), rim=RIM):
        """A round button: a rim and a top-lit face."""
        if rim:
            self.disc(cx, cy, r, rim)
            r -= 1.6
        n = int(2 * r * self.z) + 2
        g = Image.new("RGBA", (n, n))
        for y in range(n):
            t = y / (n - 1)
            g.paste(tuple(int(a + (b - a) * t) for a, b in zip(top, bot)) + (255,), (0, y, n, y + 1))
        m = Image.new("L", (n, n), 0)
        ImageDraw.Draw(m).ellipse((0, 0, n - 1, n - 1), fill=255)
        self.im.paste(g, (int((cx - r) * self.z), int((cy - r) * self.z)), m)

    def rrect(self, x0, y0, x1, y1, rad, fill, rim=None, rw=1.4):
        if rim:
            self.d.rounded_rectangle(self.P(x0, y0, x1, y1), radius=rad * self.z, fill=rim)
            x0 += rw; y0 += rw; x1 -= rw; y1 -= rw
            rad = max(0.3, rad - rw)
        self.d.rounded_rectangle(self.P(x0, y0, x1, y1), radius=rad * self.z, fill=fill)

    def text(self, cx, cy, s, size, fill, fit=None):
        f = ImageFont.truetype(self.font, int(size * self.z))
        l, t, r, b = self.d.textbbox((0, 0), s, font=f)
        if fit and (r - l) > fit * self.z:
            f = ImageFont.truetype(self.font, max(4, int(size * self.z * fit * self.z / (r - l))))
            l, t, r, b = self.d.textbbox((0, 0), s, font=f)
        self.d.text((cx * self.z - (l + r) / 2, cy * self.z - (t + b) / 2), s, font=f, fill=fill)

    def line(self, pts, fill, w):
        self.d.line([c * self.z for p in pts for c in p], fill=fill, width=max(1, int(w * self.z)), joint="curve")
        for x, y in pts:
            self.disc(x, y, w / 2, fill)

    def poly(self, pts, fill):
        self.d.polygon([c * self.z for p in pts for c in p], fill=fill)

    def shape(self, name, cx, cy, r, col, w):
        """A PlayStation face button's symbol."""
        if name == "circle":
            self.ring(cx, cy, r, col, w)
        elif name == "cross":
            q = r * 0.8
            self.line([(cx - q, cy - q), (cx + q, cy + q)], col, w)
            self.line([(cx - q, cy + q), (cx + q, cy - q)], col, w)
        elif name == "square":
            q = r * 0.82
            self.line([(cx - q, cy - q), (cx + q, cy - q), (cx + q, cy + q), (cx - q, cy + q), (cx - q, cy - q)], col, w)
        elif name == "triangle":
            q = r * 1.02
            a, b, c = (cx, cy - q), (cx + q * 0.92, cy + q * 0.62), (cx - q * 0.92, cy + q * 0.62)
            self.line([a, b, c, a], col, w)

    def keycap(self, x0, y0, x1, y1, label, size, col=WHITE, lit=False):
        """A key cap; returns its centre (for an arrow) when label is 'arrow'."""
        wd = x1 - x0
        self.rrect(x0, y0, x1, y1, min(5.0, wd / 4), DARK, rim=(176, 180, 188, 255), rw=min(1.6, wd / 9))
        e = min(2.6, wd / 6)
        self.rrect(x0 + e, y0 + e * 0.8, x1 - e, y1 - e * 1.6, min(3.0, wd / 6), (70, 73, 80, 255))
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2 - e * 0.5
        if label == "arrow":
            return cx, cy
        self.text(cx, cy, label, size, LIT if lit else col, fit=wd - 2 * e - 1.5)
        return None

    def arrow(self, cx, cy, dx, dy, q, col):
        self.poly([(cx + dx * q, cy + dy * q), (cx - dx * q + dy * q, cy - dy * q + dx * q),
                   (cx - dx * q - dy * q, cy - dy * q - dx * q)], col)

    def cluster(self, x0, y0, x1, y1):
        """Four arrow key caps in a plus."""
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        k = (x1 - x0) / 3 - 0.3
        for dx, dy in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            kx, ky = cx + dx * (k + 0.4), cy + dy * (k + 0.4)
            c = self.keycap(kx - k / 2, ky - k / 2, kx + k / 2, ky + k / 2, "arrow", k)
            self.arrow(c[0], c[1], dx, dy, k * 0.22, WHITE)


def inset(r, m):
    x, y, w, h = r
    return x + m, y + m, x + w - m, y + h - m


def draw_set(name, font):
    S = SETS[name]
    p = Pen(font)
    kb = "keys" in S

    for pos, cp in FACE_GLYPH.items():
        x0, y0, x1, y1 = inset(RECTS[cp], 2.5)
        cx, cy, r = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2
        if kb:
            p.keycap(x0, y0, x1, y1, S["keys"][pos], 24)
            continue
        label, col = S["face"][pos]
        col = tuple(col) + (255,)
        p.body(cx, cy, r)
        if label == "shape":
            p.shape(pos, cx, cy, r * 0.52, col, r * 0.15)
        else:
            p.text(cx, cy, label, r * 1.25, col)

    for cp, key in ((0xE007, "l1"), (0xE008, "l2"), (0xE00B, "r1"), (0xE00C, "r2")):
        x0, y0, x1, y1 = inset(RECTS[cp], 2.0)
        if kb:
            p.keycap(x0 + 8, y0, x1 - 8, y1, S["keys"][key], 17)
        else:
            p.rrect(x0, y0, x1, y1, 6.0, (58, 61, 66, 255), rim=RIM)
            p.rrect(x0 + 2.4, y0 + 2.2, x1 - 2.4, y1 - 4.0, 4.0, (74, 78, 84, 255))
            p.text((x0 + x1) / 2, (y0 + y1) / 2 - 1.0, S[key], 16, WHITE, fit=x1 - x0 - 8)

    for cp, key in ((0xE006, "start"), (0xE005, "select")):
        x0, y0, x1, y1 = inset(RECTS[cp], 2.0)
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        if kb:
            p.keycap(x0, cy - 12.5, x1, cy + 12.5, S["keys"][key], 15)
            continue
        kind, label = S[key]
        if kind == "oval":      # a small button with its name under it
            p.rrect(cx - 7, y0, cx + 7, y0 + 18, 6.0, (58, 61, 66, 255), rim=RIM)
            p.text(cx, y1 - 7.5, label, 12, WHITE, fit=x1 - x0 - 2)
            continue
        r = 14.0
        p.body(cx, cy, r)
        if kind == "menu":
            for dy in (-4.6, 0, 4.6):
                p.line([(cx - 6.0, cy + dy), (cx + 6.0, cy + dy)], WHITE, 2.0)
        elif kind == "view":
            p.d.rectangle(p.P(cx - 6.5, cy - 6.0, cx + 2.8, cy + 1.5), outline=WHITE, width=int(1.6 * p.z))
            p.d.rectangle(p.P(cx - 2.8, cy - 2.0, cx + 6.5, cy + 5.8), fill=(40, 42, 46, 255), outline=WHITE,
                          width=int(1.6 * p.z))
        elif kind == "create":
            p.line([(cx, cy + 6.5), (cx, cy + 0.8)], WHITE, 2.0)
            for ang in (-52, 0, 52):
                a = math.radians(ang - 90)
                p.line([(cx + 3.8 * math.cos(a), cy - 0.8 + 3.8 * math.sin(a)),
                        (cx + 7.5 * math.cos(a), cy - 0.8 + 7.5 * math.sin(a))], WHITE, 1.8)
        elif kind == "plus":
            p.line([(cx - 6.2, cy), (cx + 6.2, cy)], WHITE, 2.6)
            p.line([(cx, cy - 6.2), (cx, cy + 6.2)], WHITE, 2.6)
        elif kind == "minus":
            p.line([(cx - 6.2, cy), (cx + 6.2, cy)], WHITE, 2.6)

    # sticks: L3 (click), L (direction), R3, R. The keyboard has only the
    # left stick (the arrow keys); the rest are neutral discs.
    for cp, label, is_dir in ((0xE009, "l3", False), (0xE00A, "ls", True), (0xE00D, "r3", False), (0xE00E, "rs", True)):
        x0, y0, x1, y1 = inset(RECTS[cp], 1.5)
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        r = min(x1 - x0, y1 - y0) / 2
        if kb and cp == 0xE00A:
            p.cluster(cx - r, cy - r, cx + r, cy + r)
            continue
        p.body(cx, cy, r, top=(96, 100, 106), bot=(18, 19, 22), rim=(10, 10, 12, 255))
        txt = ("L" if cp in (0xE009, 0xE00A) else "R") + ("3" if not is_dir and kb else "")
        if not kb:
            txt = S[label]
        p.text(cx, cy - 0.5, txt, 18 if len(txt) == 1 else 14, WHITE, fit=2 * r - 6)

    # D-pad: one glyph per direction
    for cp, d in ((0xE00F, "d"), (0xE010, "u"), (0xE011, "r"), (0xE012, "l")):
        x0, y0, x1, y1 = inset(RECTS[cp], 2.0)
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        dx, dy = {"u": (0, -1), "d": (0, 1), "l": (-1, 0), "r": (1, 0)}[d]
        if kb:
            c = p.keycap(cx - 13, cy - 13, cx + 13, cy + 13, "arrow", 20)
            p.arrow(c[0], c[1], dx, dy, 6.5, WHITE)
            continue
        a, edge, arm = 5.5, (12, 12, 14, 255), (58, 61, 66, 255)
        for grow, col in ((0.0, RIM), (-1.1, edge)):
            p.rrect(cx - a - 2.2 - grow, y0 - grow, cx + a + 2.2 + grow, y1 + grow, 3.0, col)
            p.rrect(x0 - grow, cy - a - 2.2 - grow, x1 + grow, cy + a + 2.2 + grow, 3.0, col)
        arms = {"u": (cx - a, y0 + 2.2, cx + a, cy - a + 0.6), "d": (cx - a, cy + a - 0.6, cx + a, y1 - 2.2),
                "l": (x0 + 2.2, cy - a, cx - a + 0.6, cy + a), "r": (cx + a - 0.6, cy - a, x1 - 2.2, cy + a)}
        for n, b in arms.items():
            p.rrect(*b, 1.4, LIT if n == d else arm)
        p.rrect(cx - a, cy - a, cx + a, cy + a, 0.8, (40, 42, 46, 255))
        p.disc(cx, cy, 2.0, (20, 21, 24, 255))

    return finish(p.im, (W * SCALE, H * SCALE))


def finish(im, size):
    """Down to the output size with the alpha weighted in, then the colour of
    each glyph's edge spread into the transparent texels beside it, so that a
    bilinear tap across an edge does not darken it."""
    im = im.convert("RGBa").resize(size, Image.LANCZOS).convert("RGBA")
    a = im.getchannel("A")
    spread = im.convert("RGBa").filter(ImageFilter.GaussianBlur(3)).convert("RGBA")
    spread.putalpha(255)
    rgb = Image.composite(im.convert("RGB"), spread.convert("RGB"), a.point(lambda v: 255 if v else 0))
    rgb.putalpha(a)
    return rgb


def main():
    global RECTS
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sets", nargs="*", metavar="SET", help="default: " + " ".join(sorted(SETS)))
    ap.add_argument("--font", default=DEFAULT_FONT, help="a bold sans-serif TrueType font (default Segoe UI Bold)")
    ap.add_argument("--font-dat", help="a font.dat to read the rectangles from instead of the table in this file")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "res",
                                                  "prompts"))
    a = ap.parse_args()
    if a.font_dat:
        got = read_font_dat(a.font_dat)
        if got != RECTS:
            print("note: font.dat differs from the built-in table; using font.dat")
        RECTS = got
    os.makedirs(a.out, exist_ok=True)
    for name in a.sets or sorted(SETS):
        if name not in SETS:
            ap.error("no set named " + name)
        path = os.path.join(a.out, name + ".png")
        draw_set(name, a.font).save(path, optimize=True)
        print("wrote", os.path.normpath(path))


if __name__ == "__main__":
    main()
