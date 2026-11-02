"""Puts the fonts of the three typographies into each board's DDF source.

    python tools/build-typography-fonts.py

The designer sets text in four styles - Caption, Label, Title, Display - and
a device's DDF says which font family each style uses, in named
typographies (schaltli-designer docs/2026-09-30-size-scale.md). Three were
chosen on 2026-09-30 from previews on all three boards:

    Standard   Helvetica                 Display: FreeUniversal
    Humanist   Lucida Sans               Display: FreeUniversal
    Technic    Lucida Sans, Title Logisoso, Display: Seven Segment

On the colour boards (Knob, 4", 4.3B) they became 4-bit on 2026-11-02
(schaltli-designer docs/2026-11-01-4bit-fonts.md), under the same names:

    Standard   Roboto
    Humanist   Inter
    Technic    Roboto Condensed, Display: Seven Segment (4-bit)

and each 4-bit font's `replaces` names the 1-bit fonts it stands in for
(add_replaces). The PaperS3 keeps the 1-bit ones above.

This writes their fonts into ddf-source*/fonts, adds them to device.json
with their family and weight, and writes the three typographies. Run it
again after changing the sizes below; it keeps fonts that are already
there. Then regenerate the headers (tools/generate-ddf-header.js, which
every build also does).

Sizes: the designer turns each style's millimetres into a device's pixels
(Caption 2, Label 3, Title 4.5, Display 7 mm) and takes the family's
closest line. The lists below hold the sizes that lands on for the three
boards, and one either side, so the millimetres can be tuned without a new
font. Seven Segment is rasterised at exactly each board's Display line.

Sources:
  - u8g2's fonts, from the U8g2_for_Adafruit_GFX copy PlatformIO keeps in
    .pio/libdeps, through schaltli-designer's scripts/u8g2-font-to-bdf.js -
    the same bitmaps u8g2 ships, unmodified (Lucida's licence asks that).
  - DSEG7 Classic (keshikan, SIL OFL 1.1) from tools/fonts-src/dseg7,
    through tools/ttf-to-bdf.py. Named "Seven Segment" here: DSEG is a
    Reserved Font Name, and a rasterised subset is a modified version.

Needs node, Pillow, a PlatformIO build done once (for .pio/libdeps) and
schaltli-designer next to this repo.
"""
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DESIGNER = os.path.join(os.path.dirname(REPO), "schaltli-designer")
U8G2_TO_BDF = os.path.join(DESIGNER, "scripts", "u8g2-font-to-bdf.js")
TTF_TO_BDF = os.path.join(REPO, "tools", "ttf-to-bdf.py")
DSEG7 = os.path.join(REPO, "tools", "fonts-src", "dseg7")

# Each board's Display line in px: 7 mm at its pixels per millimetre.
BOARDS = {
    "ddf-source": 55,                # Knob 1.8, 7.88 px/mm
    "ddf-source-waveshare4v3b": 61,  # 4.3B, 8.66 px/mm
    "ddf-source-waveshare4": 47,     # 4", 6.78 px/mm (6.73 x 6.83, measured 2026-10-31)
    "ddf-source-papers3": 65,        # PaperS3, 9.26 px/mm
}

# (family, weight, u8g2 prefix, point sizes, display name)
U8G2_FONTS = [
    ("Lucida Sans", "regular", "luRS", ["10", "12", "14", "18", "19", "24"], "Lucida Sans"),
    ("Lucida Sans", "bold", "luBS", ["10", "12", "14", "18", "19", "24"], "Lucida Sans Bold"),
    ("FreeUniversal", "regular", "fur", ["35", "42"], "FreeUniversal"),
    ("FreeUniversal", "bold", "fub", ["35", "42"], "FreeUniversal Bold"),
    ("Logisoso", "regular", "logisoso", ["28", "30", "32", "34", "38"], "Logisoso"),
]

# 4-bit fonts for the colour boards (schaltli-designer docs/2026-11-01-4bit-fonts.md):
# anti-aliased, rasterised from outline fonts by tools/ttf-to-bdf.py --bpp 4.
# The PaperS3 keeps its 1-bit fonts (decision 4) and is not listed.
COLOUR_BOARDS = ["ddf-source", "ddf-source-waveshare4v3b", "ddf-source-waveshare4"]
# The designer's text styles in millimetres (lib/size-scale.ts, TEXT_STYLE_MM)
# and the capital height a 4-bit face is picked by: 0.69 of a style's line,
# today's Helvetica BDFs' own ratio (docs/device-contract.md, «Font depth»).
TEXT_STYLE_MM = {"caption": 2.0, "label": 3.0, "title": 4.5, "display": 7.0}
CAP_PER_LINE = 0.69
# The characters of the Windows code pages for Central and Western Europe
# and the chart's arrow and minus (tools/ttf-to-bdf.py --charset); each
# family is written with as many of them as it has. helvR18's 753 until
# 2026-11-02, when they made a 4-bit Display face half a megabyte of PSRAM
# and three typographies on the 4.3B used it all up; the user chose the code
# pages, French included.
CHARSET = "cp1250+cp1252"
FONTS_SRC = os.path.join(REPO, "tools", "fonts-src")
# (family, file tag, TTF, axes per weight, styles it serves)
FOUR_BIT_FAMILIES = [
    ("Roboto", "roboto", "roboto/Roboto[wdth,wght].ttf",
     {"regular": "wght=400,wdth=100", "bold": "wght=700,wdth=100"}, ["caption", "label", "title", "display"]),
    ("Inter", "inter", "inter/Inter[opsz,wght].ttf",
     {"regular": "wght=400", "bold": "wght=700"}, ["caption", "label", "title", "display"]),
    ("Roboto Condensed", "robotocond", "roboto-condensed/RobotoCondensed[wght].ttf",
     {"regular": "wght=400", "bold": "wght=700"}, ["caption", "label", "title"]),
]

TYPOGRAPHIES = [
    {"name": "Standard", "styles": {"caption": "Helvetica", "label": "Helvetica", "title": "Helvetica", "display": "FreeUniversal"}},
    {"name": "Humanist", "styles": {"caption": "Lucida Sans", "label": "Lucida Sans", "title": "Lucida Sans", "display": "FreeUniversal"}},
    {"name": "Technic", "styles": {"caption": "Lucida Sans", "label": "Lucida Sans", "title": "Logisoso", "display": "Seven Segment"}},
]

# The colour boards' typographies once their fonts are 4-bit (decision 9):
# replaced, not joined (decision 2), under the same names, so a project's
# chosen typography carries over.
TYPOGRAPHIES_4BIT = [
    {"name": "Standard", "styles": {"caption": "Roboto", "label": "Roboto", "title": "Roboto", "display": "Roboto"}},
    {"name": "Humanist", "styles": {"caption": "Inter", "label": "Inter", "title": "Inter", "display": "Inter"}},
    {"name": "Technic", "styles": {"caption": "Roboto Condensed", "label": "Roboto Condensed", "title": "Roboto Condensed", "display": "Seven Segment"}},
]

# Which 4-bit family stands in for each 1-bit one (spec, «Firmware»: a
# project still on a board names old ids, and the firmware draws them with
# the font that `replaces` them; the designer rewrites them on opening). By
# the role the old family had in the old typographies: Helvetica was
# Standard, Lucida Sans Humanist, Logisoso Technic's Title, FreeUniversal the
# Display of Standard (and Humanist - Standard taken, the default). Courier
# has no monospaced successor and goes to Standard's face.
REPLACED_BY_FAMILY = {
    "Helvetica": "Roboto",
    "Courier": "Roboto",
    "FreeUniversal": "Roboto",
    "Lucida Sans": "Inter",
    "Logisoso": "Roboto Condensed",
    "LogiSoSo": "Roboto Condensed",
    "Seven Segment": "Seven Segment",
}

LICENSES = """# Font licences

The fonts in this directory and where they come from. Every one allows
redistribution; the notes say what else each asks for.

| Family | Files | Source | Licence |
|---|---|---|---|
| Helvetica | helvR*, helvB* | X11 via u8g2 (Adobe, Digital Equipment Corporation) | Permission to use, copy, modify, distribute and sell, provided the copyright notices appear in all copies |
| Courier | courR* | X11 via u8g2 (Adobe, DEC) | as Helvetica |
| Lucida Sans | luRS*, luBS* | X11 via u8g2, (c) Bigelow & Holmes 1985, 1986 | Free to use and redistribute; "Lucida" is a trademark of Bigelow & Holmes and may be used only for the unmodified bitmaps, which these are |
| FreeUniversal | fur*, fub* | u8g2, (c) Stephen Wilson 2009, from SIL Sophia (c) SIL International 1994-2008 | SIL Open Font License 1.1 |
| Logisoso | logisoso* | u8g2, (c) Mathieu Gabiot 2009, 2011 | GPL v2 with font exception, also SIL OFL |
| LogiSoSo digits | logisoso62n | as Logisoso | as Logisoso |
| Seven Segment | seg7* | DSEG7 Classic (c) 2017 keshikan, rasterised by tools/ttf-to-bdf.py | SIL Open Font License 1.1 (tools/fonts-src/dseg7/OFL.txt); renamed because "DSEG" is a Reserved Font Name |

u8g2's font list with each group's licence: https://github.com/olikraus/u8g2/wiki/fntgrp
"""

# The rows the colour boards' LICENSES.md adds to the table above, for the
# 4-bit fonts only they carry.
LICENSES_4BIT_ROWS = """| Roboto | roboto-*, robotob-* | Roboto (c) 2011 The Roboto Project Authors, from google/fonts, rasterised at 4 bits by tools/ttf-to-bdf.py | SIL Open Font License 1.1 (tools/fonts-src/roboto/OFL.txt) |
| Roboto Condensed | robotocond-*, robotocondb-* | Roboto Condensed (c) 2011 The Roboto Project Authors, from google/fonts, rasterised at 4 bits | SIL Open Font License 1.1 (tools/fonts-src/roboto-condensed/OFL.txt) |
| Inter | inter-*, interb-* | Inter (c) 2020 The Inter Project Authors, from google/fonts, rasterised at 4 bits | SIL Open Font License 1.1 (tools/fonts-src/inter/OFL.txt) |
"""


def licenses_for(board):
    if board not in COLOUR_BOARDS:
        return LICENSES
    anchor = "\n\nu8g2's font list"
    return LICENSES.replace(anchor, "\n" + LICENSES_4BIT_ROWS.rstrip("\n") + anchor, 1)


def u8g2_source():
    libdeps = os.path.join(REPO, ".pio", "libdeps")
    for env in sorted(os.listdir(libdeps)) if os.path.isdir(libdeps) else []:
        path = os.path.join(libdeps, env, "U8g2_for_Adafruit_GFX", "src", "u8g2_fonts.c")
        if os.path.exists(path):
            return path
    sys.exit("u8g2_fonts.c not found under .pio/libdeps - build any board once first")


def metrics(bdf_path):
    ascent = descent = None
    with open(bdf_path, encoding="ascii") as f:
        for line in f:
            if line.startswith("FONT_ASCENT "):
                ascent = int(line.split()[1])
            elif line.startswith("FONT_DESCENT "):
                descent = int(line.split()[1])
            if ascent is not None and descent is not None:
                return ascent, descent
    sys.exit(f"{bdf_path}: no FONT_ASCENT/FONT_DESCENT")


def add_font(device, fonts_dir, entry, make):
    """Writes the BDF if missing and adds or completes its device.json entry."""
    path = os.path.join(fonts_dir, os.path.basename(entry["file"]))
    if not os.path.exists(path):
        make(path)
    ascent, descent = metrics(path)
    extra = {k: entry[k] for k in ("bpp", "capHeight") if k in entry}
    existing = next((f for f in device["fonts"] if f["id"] == entry["id"]), None)
    if existing:
        existing["family"] = entry["family"]
        existing["weight"] = entry["weight"]
        existing.update(extra)
        return
    device["fonts"].append({
        "id": entry["id"],
        "displayName": entry["displayName"].replace("{line}", str(ascent + descent)),
        "internalName": entry["internalName"],
        "file": entry["file"],
        "size": ascent + descent,
        "ascent": ascent,
        "descent": descent,
        "family": entry["family"],
        "weight": entry["weight"],
        **extra,
    })


def pixels_per_mm(device):
    """As the designer's pixelsPerMmOf: the mean of both axes."""
    s = device["screen"]
    return (s["width"] / s["widthMm"] + s["height"] / s["heightMm"]) / 2


def add_four_bit_fonts(device, fonts_dir, display_px):
    """Each family's faces for the styles it serves: the capital height a
    style asks for, regular and bold (only that one: each face also travels in
    the DDF the board sends over MQTT, 2026-11-01 - the user chose exact sizes
    over 2.3-2.9 MB of description); Seven Segment
    at the Display line."""
    ppmm = pixels_per_mm(device)
    for family, tag, ttf, axes, styles in FOUR_BIT_FAMILIES:
        caps = sorted({round(CAP_PER_LINE * TEXT_STYLE_MM[st] * ppmm) for st in styles})
        for weight, weight_axes in axes.items():
            for cap in caps:
                name = f"{tag}{'b' if weight == 'bold' else ''}-c{cap}"
                axes_arg = weight_axes
                if tag == "inter":
                    # Inter's optical size follows the size it is set at:
                    # its em is about cap / 0.728, between 14 and 32.
                    axes_arg += f",opsz={min(32, max(14, round(cap / 0.728)))}"
                add_font(device, fonts_dir, {
                    "id": f"font-{name}",
                    "displayName": f"{family}{' Bold' if weight == 'bold' else ''} {{line}}px",
                    "internalName": name,
                    "file": f"fonts/{name}.bdf",
                    "family": family,
                    "weight": weight,
                    "bpp": 4,
                    "capHeight": cap,
                }, lambda path, ttf=ttf, cap=cap, name=name, axes_arg=axes_arg: subprocess.run(
                    [sys.executable, TTF_TO_BDF, os.path.join(FONTS_SRC, ttf), str(cap), path, name,
                     "--bpp", "4", "--by", "cap", "--axes", axes_arg, "--charset", CHARSET],
                    check=True, capture_output=True))
    for weight, ttf, tag in [("regular", "DSEG7Classic-Regular.ttf", "seg7"), ("bold", "DSEG7Classic-Bold.ttf", "seg7b")]:
        name = f"{tag}-{display_px}-4bit"
        add_font(device, fonts_dir, {
            "id": f"font-{name}",
            "displayName": f"Seven Segment{' Bold' if weight == 'bold' else ''} {display_px}px",
            "internalName": name,
            "file": f"fonts/{name}.bdf",
            "family": "Seven Segment",
            "weight": weight,
            "bpp": 4,
        }, lambda path, ttf=ttf, name=name: subprocess.run(
            [sys.executable, TTF_TO_BDF, os.path.join(DSEG7, ttf), str(display_px), path, f"Seven-Segment-{name}",
             "--bpp", "4", "--charset", CHARSET],
            check=True, capture_output=True))


def cap_of(font):
    """A 4-bit face's capital height; Seven Segment's digits fill its line."""
    return font.get("capHeight") or font["ascent"]


def add_replaces(device):
    """Each 1-bit font in exactly one 4-bit font's `replaces`: the successor
    family (REPLACED_BY_FAMILY), the same weight, and the face for the text
    style the old font served. The designer took, for each style, the face of
    a family whose line comes closest to the style's line; a font it took for
    several styles counts for the smallest, one it took for none for the
    style whose line is nearest its own. That style's 4-bit face is the one at
    its capital height (decision 10), or the family's closest to it where the
    family has none for that style (Roboto Condensed has no Display).
    Only the styles the old typographies set in that family count: the
    smallest FreeUniversal is not a Caption because it is FreeUniversal's
    smallest, it was only ever a Display.

    By the capitals alone, Lucida Sans 24px - Humanist's Title on the 4.3B,
    a 32 px line against Title's 39 - went to Inter at Label size, and every
    title would have shrunk."""
    ppmm = pixels_per_mm(device)
    style_line = {st: mm * ppmm for st, mm in TEXT_STYLE_MM.items()}
    style_cap = {st: round(CAP_PER_LINE * line) for st, line in style_line.items()}
    order = list(TEXT_STYLE_MM)
    four = [f for f in device["fonts"] if f.get("bpp") == 4]
    for f in four:
        f.pop("replaces", None)
    olds = [f for f in device["fonts"] if f.get("bpp") != 4]
    for old in olds:
        family = REPLACED_BY_FAMILY.get(old.get("family"))
        if family is None:
            sys.exit(f"{old['id']}: no 4-bit family replaces {old.get('family')!r} - add it to REPLACED_BY_FAMILY")
        weight = old.get("weight", "regular")
        siblings = [f for f in olds if f.get("family") == old.get("family") and f.get("weight", "regular") == weight]
        used_for = {st for t in TYPOGRAPHIES for st, fam in t["styles"].items() if fam == old.get("family")}
        served = [st for st in order if st in used_for
                  and min(siblings, key=lambda f: (abs(f["size"] - style_line[st]), f["size"]))["id"] == old["id"]]
        style = served[0] if served else min(order, key=lambda st: abs(style_line[st] - old["size"]))
        faces = [f for f in four if f["family"] == family and f["weight"] == weight] or                 [f for f in four if f["family"] == family and f["weight"] == "regular"]
        if not faces:
            sys.exit(f"{old['id']}: no 4-bit {family} face")
        if family == "Seven Segment":
            best = faces[0]
        else:
            best = min(faces, key=lambda f: (abs(cap_of(f) - style_cap[style]), cap_of(f)))
        best.setdefault("replaces", []).append(old["id"])


def main():
    u8g2 = u8g2_source()
    for board, display_px in BOARDS.items():
        source = os.path.join(REPO, board)
        device_json = os.path.join(source, "device.json")
        with open(device_json, encoding="utf-8") as f:
            device = json.load(f)
        before = json.loads(json.dumps(device))
        fonts_dir = os.path.join(source, "fonts")

        for family, weight, prefix, sizes, display in U8G2_FONTS:
            for size in sizes:
                name = f"{prefix}{size}"
                add_font(device, fonts_dir, {
                    "id": f"font-{name}",
                    "displayName": f"{display} {int(size)}px",
                    "internalName": f"u8g2_font_{name}_tf",
                    "file": f"fonts/{name}.bdf",
                    "family": family,
                    "weight": weight,
                }, lambda path, name=name: subprocess.run(
                    ["node", U8G2_TO_BDF, u8g2, f"u8g2_font_{name}_tf", path], check=True, capture_output=True))

        for weight, ttf, tag in [("regular", "DSEG7Classic-Regular.ttf", "seg7"), ("bold", "DSEG7Classic-Bold.ttf", "seg7b")]:
            name = f"{tag}-{display_px}"
            add_font(device, fonts_dir, {
                "id": f"font-{name}",
                "displayName": f"Seven Segment{' Bold' if weight == 'bold' else ''} {display_px}px",
                "internalName": name,
                "file": f"fonts/{name}.bdf",
                "family": "Seven Segment",
                "weight": weight,
            }, lambda path, ttf=ttf, name=name: subprocess.run(
                [sys.executable, TTF_TO_BDF, os.path.join(DSEG7, ttf), str(display_px), path, f"Seven-Segment-{name}"],
                check=True, capture_output=True))

        if board in COLOUR_BOARDS:
            add_four_bit_fonts(device, fonts_dir, display_px)
            add_replaces(device)
            device["typography"] = TYPOGRAPHIES_4BIT
        else:
            device["typography"] = TYPOGRAPHIES
        # Written only when something changed, so a board this run adds
        # nothing to (the PaperS3) keeps its file as it is, formatting and all.
        if device != before:
            with open(device_json, "w", encoding="utf-8", newline="\n") as f:
                f.write(json.dumps(device, indent=2, ensure_ascii=False) + "\n")
        licenses_path = os.path.join(fonts_dir, "LICENSES.md")
        with open(licenses_path, encoding="utf-8") as f:
            current = f.read().replace("\r\n", "\n")
        if current != licenses_for(board):
            with open(licenses_path, "w", encoding="utf-8", newline="\n") as f:
                f.write(licenses_for(board))
        print(f"{board}: {len(device['fonts'])} fonts, typographies {', '.join(t['name'] + ' (' + t['styles']['label'] + ')' for t in device['typography'])}")


main()
