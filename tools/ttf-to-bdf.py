"""Rasterises a TrueType font to a BDF, 1 bit (default) or 4 bits per pixel.

    python tools/ttf-to-bdf.py <font.ttf> <px> <out.bdf> <bdf font name>
        [--bpp 1|4] [--by line|cap] [--axes wght=700,wdth=100] [--charset-from <font.bdf>]

--by line (default): <px> is a line height - ascent + descent as the TTF
reports them at the chosen pixel size, the largest size whose line still
fits - which is what the designer's text styles compare for 1-bit fonts
(lib/size-scale.ts in schaltli-designer).

--by cap: <px> is a capital height. The size whose CAP_HEIGHT (the font's
OS/2 capital height at that size, rounded) comes closest wins, a tie to the
smaller - how the designer picks 4-bit fonts (docs/device-contract.md,
«Font depth»: 0.69 of a style's line, the ratio today's Helvetica BDFs have).

--bpp 1: glyphs are drawn with anti-aliasing off, so what the BDF holds is
what a 1-bit panel shows. That suits geometric faces - a seven-segment face
above all, made of straight edges - and is why this exists for DSEG7
(2026-09-30); a text face at small sizes looks better hand-pixelled, which is
what the u8g2 fonts are.

--bpp 4: glyphs are drawn anti-aliased and each pixel's coverage quantised to
0-15, written one hex digit per pixel, with the property BITS_PER_PIXEL 4
(docs/2026-11-01-4bit-fonts.md in schaltli-designer). The colour boards blend
it in RGB565; the designer reads the same bytes.

--axes: a variable font's axes by tag (wght, wdth, opsz); unnamed axes keep
their default.

Characters: Latin-1 (32-126, 160-255) like the u8g2 `_tf` fonts, or with
--charset-from every ENCODING of that BDF - for the 4-bit fonts today's
helvR18, whose 753 glyphs the designer has offered since 2026-08-22. With
--bpp 4 or --charset-from only characters the font's cmap maps are written:
a font has what it has, so the designer's "does the device have this
character?" keeps one answer.

Needs Pillow (pip install pillow), which brings FreeType. The cmap and the
capital height are read from the TTF here, no other library.
"""
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

LATIN1 = list(range(32, 127)) + list(range(160, 256))
AXIS_TAGS = {"weight": "wght", "width": "wdth", "optical size": "opsz", "optics": "opsz", "slant": "slnt", "italic": "ital"}


# ---------------------------------------------------------------- TTF tables

def read_tables(path):
    data = open(path, "rb").read()
    count = struct.unpack_from(">H", data, 4)[0]
    tables = {}
    for i in range(count):
        tag, _, offset, length = struct.unpack_from(">4sIII", data, 12 + 16 * i)
        tables[tag.decode("latin-1")] = data[offset:offset + length]
    return tables


def cmap_codes(tables):
    """Every code point the font maps to a glyph other than .notdef."""
    cmap = tables["cmap"]
    count = struct.unpack_from(">H", cmap, 2)[0]
    subtables = {}
    for i in range(count):
        platform, encoding, offset = struct.unpack_from(">HHI", cmap, 4 + 8 * i)
        subtables[(platform, encoding)] = offset
    for key in ((3, 10), (0, 6), (0, 4), (3, 1), (0, 3)):
        if key not in subtables:
            continue
        off = subtables[key]
        fmt = struct.unpack_from(">H", cmap, off)[0]
        if fmt == 12:
            groups = struct.unpack_from(">I", cmap, off + 12)[0]
            codes = set()
            for g in range(groups):
                start, end, glyph = struct.unpack_from(">III", cmap, off + 16 + 12 * g)
                codes.update(c for c in range(start, end + 1) if glyph + (c - start) != 0)
            return codes
        if fmt == 4:
            seg2 = struct.unpack_from(">H", cmap, off + 6)[0]
            ends = off + 14
            starts = ends + seg2 + 2
            deltas = starts + seg2
            ranges = deltas + seg2
            codes = set()
            for s in range(seg2 // 2):
                end = struct.unpack_from(">H", cmap, ends + 2 * s)[0]
                start = struct.unpack_from(">H", cmap, starts + 2 * s)[0]
                delta = struct.unpack_from(">h", cmap, deltas + 2 * s)[0]
                range_offset = struct.unpack_from(">H", cmap, ranges + 2 * s)[0]
                for c in range(start, end + 1):
                    if c == 0xFFFF:
                        continue
                    if range_offset == 0:
                        glyph = (c + delta) & 0xFFFF
                    else:
                        at = ranges + 2 * s + range_offset + 2 * (c - start)
                        glyph = struct.unpack_from(">H", cmap, at)[0]
                        if glyph:
                            glyph = (glyph + delta) & 0xFFFF
                    if glyph:
                        codes.add(c)
            return codes
    raise SystemExit("no Unicode cmap (format 4 or 12) in the font")


def cap_units(tables):
    """OS/2 sCapHeight and head unitsPerEm."""
    units_per_em = struct.unpack_from(">H", tables["head"], 18)[0]
    os2 = tables["OS/2"]
    if struct.unpack_from(">H", os2, 0)[0] < 2:
        raise SystemExit("OS/2 table older than version 2: no capital height")
    return struct.unpack_from(">h", os2, 88)[0], units_per_em


# ---------------------------------------------------------------- choosing a size

def load(path, size, axes):
    font = ImageFont.truetype(path, size)
    if axes:
        values = []
        for axis in font.get_variation_axes():
            name = axis["name"] if isinstance(axis["name"], str) else axis["name"].decode()
            values.append(axes.get(AXIS_TAGS.get(name.lower(), name.lower()), axis["default"]))
        font.set_variation_by_axes(values)
    return font


def font_for_line(path, line, axes=None):
    best = None
    for size in range(4, 400):
        font = load(path, size, axes)
        ascent, descent = font.getmetrics()
        if ascent + descent > line:
            break
        best = font
    if best is None:
        raise SystemExit(f"{path}: no size has a line of {line} px or less")
    return best


def font_for_cap(path, cap, axes, cap_height, units_per_em):
    best = None
    for size in range(4, 400):
        here = round(cap_height * size / units_per_em)
        if best is None or abs(here - cap) < abs(best[1] - cap):
            best = (size, here)
        if here > cap + 1:
            break
    return load(path, best[0], axes), best[1]


# ---------------------------------------------------------------- glyphs

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


def glyph_rows_4bit(font, ch):
    """Coverage 0-15 per pixel, one hex digit each, trimmed to the ink."""
    left, top, right, bottom = font.getbbox(ch, anchor="ls")
    width, height = max(0, right - left), max(0, bottom - top)
    if width == 0 or height == 0:
        return left, bottom, 0, 0, []
    img = Image.new("L", (width, height), 0)
    draw = ImageDraw.Draw(img)
    draw.fontmode = "L"
    draw.text((-left, -top), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    q = [[(px[x, y] * 15 + 127) // 255 for x in range(width)] for y in range(height)]
    ink_rows = [y for y in range(height) if any(q[y])]
    if not ink_rows:
        return left, bottom, 0, 0, []
    ink_cols = [x for x in range(width) if any(q[y][x] for y in range(height))]
    y0, y1, x0, x1 = ink_rows[0], ink_rows[-1], ink_cols[0], ink_cols[-1]
    rows = ["".join("%X" % q[y][x] for x in range(x0, x1 + 1)) for y in range(y0, y1 + 1)]
    # The box's left edge and bottom, relative to the origin on the baseline.
    return left + x0, top + y1 + 1, x1 - x0 + 1, y1 - y0 + 1, rows


# ---------------------------------------------------------------- main

def parse_args(argv):
    if len(argv) < 5:
        raise SystemExit(__doc__)
    args = {"path": argv[1], "px": int(argv[2]), "out": argv[3], "name": argv[4],
            "bpp": 1, "by": "line", "axes": None, "charset": None}
    rest = argv[5:]
    while rest:
        if len(rest) < 2:
            raise SystemExit(f"{rest[0]} needs a value")
        flag, value = rest[0], rest[1]
        if flag == "--bpp":
            args["bpp"] = int(value)
        elif flag == "--by":
            args["by"] = value
        elif flag == "--axes":
            args["axes"] = {k: float(v) for k, v in (p.split("=") for p in value.split(","))}
        elif flag == "--charset-from":
            args["charset"] = value
        else:
            raise SystemExit(f"unknown option {flag}")
        rest = rest[2:]
    if args["bpp"] not in (1, 4) or args["by"] not in ("line", "cap"):
        raise SystemExit(__doc__)
    return args


def charset_from(bdf):
    codes = set()
    for line in open(bdf, encoding="latin-1"):
        if line.startswith("ENCODING "):
            code = int(line.split()[1])
            if 32 <= code < 0x110000:
                codes.add(code)
    return sorted(codes)


def main():
    a = parse_args(sys.argv)
    path, px, out, name = a["path"], a["px"], a["out"], a["name"]
    tables = read_tables(path)
    cap_height = units_per_em = cap = None
    if a["by"] == "cap" or a["bpp"] == 4:
        cap_height, units_per_em = cap_units(tables)
    if a["by"] == "cap":
        font, cap = font_for_cap(path, px, a["axes"], cap_height, units_per_em)
    else:
        font = font_for_line(path, px, a["axes"])
        if cap_height is not None:
            cap = round(cap_height * font.size / units_per_em)
    ascent, descent = font.getmetrics()
    line = ascent + descent
    chars = charset_from(a["charset"]) if a["charset"] else LATIN1
    mapped = None
    if a["bpp"] == 4 or a["charset"]:
        mapped = cmap_codes(tables)
        chars = [c for c in chars if c in mapped]
    rows_of = glyph_rows_4bit if a["bpp"] == 4 else glyph_rows
    glyphs = []
    for code in chars:
        ch = chr(code)
        advance = round(font.getlength(ch))
        left, bottom, width, height, rows = rows_of(font, ch)
        # A glyph the font does not have draws nothing and moves nowhere.
        # Where the cmap says which it has, one that does neither stays - a
        # zero-width space is a character the font has, not one it lacks.
        if width == 0 and advance == 0 and mapped is None:
            continue
        glyphs.append((code, advance, width, height, left, -bottom, rows))
    # SIZE and SWIDTH keep the line the 1-bit fonts were asked for, as before.
    size = px if a["by"] == "line" else line
    with open(out, "w", encoding="ascii", newline="\n") as o:
        o.write("STARTFONT 2.1\n")
        o.write(f"FONT {name}\n")
        o.write(f"SIZE {size} 75 75\n")
        o.write(f"FONTBOUNDINGBOX {max(g[2] for g in glyphs)} {line} 0 {-descent}\n")
        if a["bpp"] == 4:
            o.write(f"STARTPROPERTIES 4\nFONT_ASCENT {ascent}\nFONT_DESCENT {descent}\n"
                    f"CAP_HEIGHT {cap}\nBITS_PER_PIXEL 4\nENDPROPERTIES\n")
        else:
            o.write(f"STARTPROPERTIES 2\nFONT_ASCENT {ascent}\nFONT_DESCENT {descent}\nENDPROPERTIES\n")
        o.write(f"CHARS {len(glyphs)}\n")
        for code, advance, width, height, xoff, yoff, rows in glyphs:
            o.write(f"STARTCHAR U+{code:04X}\nENCODING {code}\n")
            o.write(f"SWIDTH {advance * 1000 // max(1, size)} 0\nDWIDTH {advance} 0\n")
            o.write(f"BBX {width} {height} {xoff} {yoff}\nBITMAP\n")
            for row in rows:
                o.write(row + "\n")
            o.write("ENDCHAR\n")
        o.write("ENDFONT\n")
    extra = f", cap {cap}, 4 bit" if a["bpp"] == 4 else ""
    print(f"{out}: {font.size} px face, line {line} ({ascent}+{descent}){extra}, {len(glyphs)} glyphs")


main()
