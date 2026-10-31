"""Puts the fonts of the three typographies into each board's DDF source.

    python tools/build-typography-fonts.py

The designer sets text in four styles - Caption, Label, Title, Display - and
a device's DDF says which font family each style uses, in named
typographies (schaltli-designer docs/2026-09-30-size-scale.md). Three were
chosen on 2026-09-30 from previews on all three boards:

    Standard   Helvetica                 Display: FreeUniversal
    Humanist   Lucida Sans               Display: FreeUniversal
    Technic    Lucida Sans, Title Logisoso, Display: Seven Segment

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

TYPOGRAPHIES = [
    {"name": "Standard", "styles": {"caption": "Helvetica", "label": "Helvetica", "title": "Helvetica", "display": "FreeUniversal"}},
    {"name": "Humanist", "styles": {"caption": "Lucida Sans", "label": "Lucida Sans", "title": "Lucida Sans", "display": "FreeUniversal"}},
    {"name": "Technic", "styles": {"caption": "Lucida Sans", "label": "Lucida Sans", "title": "Logisoso", "display": "Seven Segment"}},
]

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
    existing = next((f for f in device["fonts"] if f["id"] == entry["id"]), None)
    if existing:
        existing["family"] = entry["family"]
        existing["weight"] = entry["weight"]
        return
    device["fonts"].append({
        "id": entry["id"],
        "displayName": entry["displayName"],
        "internalName": entry["internalName"],
        "file": entry["file"],
        "size": ascent + descent,
        "ascent": ascent,
        "descent": descent,
        "family": entry["family"],
        "weight": entry["weight"],
    })


def main():
    u8g2 = u8g2_source()
    for board, display_px in BOARDS.items():
        source = os.path.join(REPO, board)
        device_json = os.path.join(source, "device.json")
        with open(device_json, encoding="utf-8") as f:
            device = json.load(f)
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

        device["typography"] = TYPOGRAPHIES
        with open(device_json, "w", encoding="utf-8", newline="\n") as f:
            f.write(json.dumps(device, indent=2, ensure_ascii=False) + "\n")
        with open(os.path.join(fonts_dir, "LICENSES.md"), "w", encoding="utf-8", newline="\n") as f:
            f.write(LICENSES)
        print(f"{board}: {len(device['fonts'])} fonts, typographies {', '.join(t['name'] for t in TYPOGRAPHIES)}")


main()
