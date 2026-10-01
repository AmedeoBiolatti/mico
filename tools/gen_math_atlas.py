#!/usr/bin/env python3
"""Pre-renders the glyphs mico's math renderer composes equations from.

Run once, by hand, when the glyph set changes; the output is committed, so a
build never needs Python, Pillow or the font:

    python3 tools/gen_math_atlas.py [path/to/latinmodern-math.otf]

Writes src/math/atlas.bin:

    "MATL" u32 version  u32 master_px  u32 count
    count x { u32 cp  i16 adv  i16 x0 y0 x1 y1   (1/1000 em, y up)
              u16 bw bh  i16 left top            (bitmap px; top is y down
                                                  from the baseline)
              u32 offset }                       (into the data below)
    RLE coverage: bytes 0x00..0x7F are a literal coverage value << 1,
    0x80 | n is a run of n+1 zeros

Glyphs are drawn at 4x the master size and box-filtered down, so the
coverage is unhinted and exact at the master size. The renderer resamples
from there with a summed-area table, which is exact at any scale.

Latin Modern Math is under the GUST Font License, which allows this.
"""
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/texmf/fonts/opentype/public/lm-math/latinmodern-math.otf"
MASTER = 64       # px per em of the stored bitmaps
OVER = 4          # supersampling factor
UNITS = 1000      # metrics are stored in 1/1000 em


def codepoints():
    cps = set(range(0x21, 0x7F))
    cps |= set(range(0x391, 0x3AA)) - {0x3A2}
    cps |= set(range(0x3B1, 0x3CA)) | {0x3D1, 0x3D5, 0x3D6, 0x3F0, 0x3F1, 0x3F5}
    # Mathematical alphanumerics: italic, bold, bold italic, script, fraktur,
    # double-struck, Greek italic and bold, bold and double-struck digits.
    cps |= set(range(0x1D400, 0x1D4D0))
    cps |= set(range(0x1D504, 0x1D56C))
    cps |= set(range(0x1D6A8, 0x1D71C))
    cps |= set(range(0x1D7CE, 0x1D7E2))
    # The letterlike holes in those blocks.
    cps |= {0x210E, 0x212C, 0x2130, 0x2131, 0x210B, 0x2110, 0x2112, 0x2133, 0x211B,
            0x212F, 0x210A, 0x2134, 0x212D, 0x210C, 0x2111, 0x211C, 0x2128,
            0x2102, 0x210D, 0x2115, 0x2119, 0x211A, 0x211D, 0x2124}
    cps |= set(range(0x2190, 0x2200))   # arrows
    cps |= set(range(0x2200, 0x2300))   # mathematical operators
    cps |= set(range(0x2308, 0x230C))   # ceilings, floors
    cps |= set(range(0x239B, 0x23B0))   # delimiter pieces
    cps |= set(range(0x23DC, 0x23E2))   # over/under braces
    cps |= set(range(0x27E6, 0x27F0))   # angle and white brackets
    cps |= set(range(0x27F5, 0x2800))   # long arrows
    cps |= set(range(0x2A00, 0x2A07))   # n-ary circled operators
    cps |= set(map(ord, "±×÷·¬°′″‴†‡…ℏℓ℘ℵℶℷ∎□△▽◁▷◇○•∙‖ıȷð♠♣♥♦♭♮♯✓"))
    cps |= {0x2212, 0x2215, 0x2044, 0x02C6, 0x02C7, 0x02D8, 0x02D9, 0x02DC,
            0x00AF, 0x00B4, 0x00A8, 0x02DA, 0x2032, 0x2033, 0x2034, 0x2057}
    return sorted(cps)


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else FONT
    big = ImageFont.truetype(font_path, MASTER * OVER)
    units = ImageFont.truetype(font_path, UNITS)

    def raster(ch):
        x0, y0, x1, y1 = big.getbbox(ch, anchor="ls")
        # Whole master pixels around the ink, so the box filter lines up.
        l, t = x0 // OVER - 1, y0 // OVER - 1
        r, b = -(-x1 // OVER) + 1, -(-y1 // OVER) + 1
        w, h = max(1, r - l), max(1, b - t)
        im = Image.new("L", (w * OVER, h * OVER), 0)
        ImageDraw.Draw(im).text((-l * OVER, -t * OVER), ch, font=big, fill=255, anchor="ls")
        a = np.asarray(im, dtype=np.float32).reshape(h, OVER, w, OVER).mean(axis=(1, 3))
        cov = np.round(a / 255.0 * 127.0).astype(np.uint8)
        # Trim blank borders the bbox estimate left.
        ys, xs = np.nonzero(cov)
        if len(xs) == 0:
            return 0, 0, 0, 0, np.zeros((0, 0), np.uint8)
        cov = cov[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
        return cov.shape[1], cov.shape[0], l + xs.min(), t + ys.min(), cov

    missing = raster(chr(0xE000))[4]  # a private-use point: the font's .notdef

    records, blob, skipped = [], bytearray(), []
    for cp in codepoints():
        ch = chr(cp)
        bw, bh, left, top, cov = raster(ch)
        if cov.shape == missing.shape and np.array_equal(cov, missing):
            skipped.append(cp)
            continue
        adv = round(units.getlength(ch))
        x0, y0, x1, y1 = units.getbbox(ch, anchor="ls")
        off = len(blob)
        zeros = 0
        for v in cov.flatten().tolist():
            if v == 0:
                zeros += 1
                if zeros == 128:
                    blob.append(0x80 | 127)
                    zeros = 0
                continue
            if zeros:
                blob.append(0x80 | (zeros - 1))
                zeros = 0
            blob.append(v)
        if zeros:
            blob.append(0x80 | (zeros - 1))
        records.append(struct.pack("<IhhhhhHHhhI", cp, adv, x0, -y1, x1, -y0, bw, bh,
                                   left, top, off))

    out = Path(__file__).resolve().parent.parent / "src" / "math" / "atlas.bin"
    with open(out, "wb") as f:
        f.write(b"MATL" + struct.pack("<III", 1, MASTER, len(records)))
        for r in records:
            f.write(r)
        f.write(blob)
    print(f"{len(records)} glyphs, {len(blob)} bytes of coverage -> {out}")
    if skipped:
        print(f"not in the font ({len(skipped)}):", " ".join(f"{c:04X}" for c in skipped[:40]),
              "..." if len(skipped) > 40 else "")


if __name__ == "__main__":
    main()
