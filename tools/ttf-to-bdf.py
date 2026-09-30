"""Rasterises a TrueType font to a 1-bit BDF at a given line height.

    python tools/ttf-to-bdf.py <font.ttf> <line_px> <out.bdf> <bdf font name>

The line height is ascent + descent as the TTF reports them at the chosen
pixel size - the largest size whose line still fits - which is what the
designer's text styles compare (lib/size-scale.ts in schaltli-designer).
Latin-1 only (32-126, 160-255), like the u8g2 `_tf` fonts beside it.

Glyphs are drawn with anti-aliasing off, so what the BDF holds is what a
1-bit panel shows. That suits geometric faces - a seven-segment face above
all, made of straight edges - and is why this exists for DSEG7
(2026-09-30); a text face at small sizes looks better hand-pixelled, which
is what the u8g2 fonts are.

Needs Pillow (pip install pillow), which brings FreeType.
"""
import sys

from PIL import Image, ImageDraw, ImageFont

CHARS = list(range(32, 127)) + list(range(160, 256))


def font_for_line(path, line):
    best = None
    for size in range(4, 400):
        font = ImageFont.truetype(path, size)
        ascent, descent = font.getmetrics()
        if ascent + descent > line:
            break
        best = font
    if best is None:
        raise SystemExit(f"{path}: no size has a line of {line} px or less")
    return best


def glyph_rows(font, ch):
    left, top, right, bottom = font.getbbox(ch, anchor="ls")
    width, height = max(0, right - left), max(0, bottom - top)
    if width == 0 or height == 0:
        return left, bottom, 0, 0, []
    img = Image.new("1", (width, height), 0)
    draw = ImageDraw.Draw(img)
    draw.fontmode = "1"
    draw.text((-left, -top), ch, font=font, fill=1, anchor="ls")
    px = img.load()
    rows = []
    for y in range(height):
        bits = "".join("1" if px[x, y] else "0" for x in range(width))
        bits += "0" * ((8 - len(bits) % 8) % 8)
        rows.append("".join(f"{int(bits[i:i + 8], 2):02X}" for i in range(0, len(bits), 8)))
    if not any(int(r, 16) for r in rows):
        return left, bottom, 0, 0, []
    return left, bottom, width, height, rows


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    path, line, out, name = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    font = font_for_line(path, line)
    ascent, descent = font.getmetrics()
    glyphs = []
    for code in CHARS:
        ch = chr(code)
        advance = round(font.getlength(ch))
        left, bottom, width, height, rows = glyph_rows(font, ch)
        # A glyph the font does not have draws nothing and moves nowhere.
        if width == 0 and advance == 0:
            continue
        glyphs.append((code, advance, width, height, left, -bottom, rows))
    with open(out, "w", encoding="ascii", newline="\n") as o:
        o.write("STARTFONT 2.1\n")
        o.write(f"FONT {name}\n")
        o.write(f"SIZE {line} 75 75\n")
        o.write(f"FONTBOUNDINGBOX {max(g[2] for g in glyphs)} {ascent + descent} 0 {-descent}\n")
        o.write(f"STARTPROPERTIES 2\nFONT_ASCENT {ascent}\nFONT_DESCENT {descent}\nENDPROPERTIES\n")
        o.write(f"CHARS {len(glyphs)}\n")
        for code, advance, width, height, xoff, yoff, rows in glyphs:
            o.write(f"STARTCHAR U+{code:04X}\nENCODING {code}\n")
            o.write(f"SWIDTH {advance * 1000 // max(1, line)} 0\nDWIDTH {advance} 0\n")
            o.write(f"BBX {width} {height} {xoff} {yoff}\nBITMAP\n")
            for row in rows:
                o.write(row + "\n")
            o.write("ENDCHAR\n")
        o.write("ENDFONT\n")
    print(f"{out}: {font.size} px face, line {ascent + descent} ({ascent}+{descent}), {len(glyphs)} glyphs")


main()
