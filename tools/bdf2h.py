"""
bdf2h.py - Converts Spleen BDF bitmap fonts into the C header used by the
ELECTGPL EPD Writer firmware (spleen_fonts.h).

Glyph coverage: ASCII 32..126 plus ISO-8859-1 160..255 (191 glyphs per font).
Storage: one integer per row, MSB = leftmost pixel.
Index:   cp < 127 -> cp - 32 ;  cp >= 160 -> cp - 160 + 95

Spleen fonts: https://github.com/fcambus/spleen (BSD 2-Clause).
Run it from Thonny or any Python 3 interpreter; edit the block below.
"""

# ---------------- CONFIGURATION (edit here) ----------------
FONTS = [
    # (C array name,  BDF file path)
    ("spleen12x24", "spleen-12x24.bdf"),
    ("spleen8x16",  "spleen-8x16.bdf"),
]
OUTPUT_FILE = "spleen_fonts.h"
# -----------------------------------------------------------

import re
import sys

CODEPOINTS = list(range(32, 127)) + list(range(160, 256))


def parse_bdf(path):
    txt = open(path, encoding="latin-1").read()
    W, H, FX, FY = map(int, re.search(
        r"FONTBOUNDINGBOX (\d+) (\d+) (-?\d+) (-?\d+)", txt).groups())
    glyphs = {}
    pattern = (r"STARTCHAR.*?ENCODING (-?\d+).*?BBX (\d+) (\d+) (-?\d+) (-?\d+)"
               r".*?BITMAP\n(.*?)ENDCHAR")
    for m in re.finditer(pattern, txt, re.S):
        enc = int(m.group(1))
        bw, bh, bx, by = map(int, m.group(2, 3, 4, 5))
        hexrows = m.group(6).split()
        rows = [int(r, 16) for r in hexrows]
        nbits = len(hexrows[0]) * 4 if hexrows else 8
        cell = [0] * H
        top = H - (by - FY) - bh              # top row of the glyph inside the cell
        for i, r in enumerate(rows):
            y = top + i
            if 0 <= y < H:
                v = r >> (nbits - bw) if nbits >= bw else r
                shift = W - bw - bx
                v = v << shift if shift >= 0 else v >> -shift
                cell[y] = v & ((1 << W) - 1)
        glyphs[enc] = cell
    return W, H, glyphs


def main():
    out = [
        "/* Spleen fonts (c) 2018-2026 Frederic Cambus - BSD 2-Clause license.",
        " * Converted from BDF to row bitmaps (MSB = leftmost pixel) by tools/bdf2h.py.",
        " * Coverage: ASCII 32..126 and ISO-8859-1 160..255 (191 glyphs per font).",
        " * Index: cp<127 -> cp-32 ; cp>=160 -> cp-160+95 */",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    for name, path in FONTS:
        W, H, g = parse_bdf(path)
        ctype = "uint16_t" if W > 8 else "uint8_t"
        fmt = "0x%03X" if W > 8 else "0x%02X"
        out.append(f"#define {name.upper()}_W {W}\n#define {name.upper()}_H {H}")
        out.append(f"static const {ctype} {name}[191][{H}] = {{")
        missing = 0
        for cp in CODEPOINTS:
            cell = g.get(cp)
            if cell is None:
                cell = g[ord("?")]
                missing += 1
            out.append("  {" + ",".join(fmt % v for v in cell) + "}, // " + str(cp))
        out.append("};\n")
        print(f"{name}: {W}x{H}, missing glyphs: {missing}", file=sys.stderr)
    open(OUTPUT_FILE, "w").write("\n".join(out))
    print("written", OUTPUT_FILE, file=sys.stderr)


if __name__ == "__main__":
    main()
