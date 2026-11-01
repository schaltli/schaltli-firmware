// Checks tools/ttf-to-bdf.py on a laptop, with no board connected:
//
//   node tools/ttf-to-bdf-check/run.js
//
//  - 1 bit as before: DSEG7 at the 4.3B's Display line comes out byte for
//    byte as the seg7-61.bdf in its DDF, so --bpp 4 changed nothing for the
//    fonts that are already there;
//  - 4 bits reproducible: Roboto twice, the same bytes;
//  - 4 bits well formed (schaltli-designer docs/device-contract.md, «Font
//    depth»): BITS_PER_PIXEL 4, the CAP_HEIGHT asked for, every row one hex
//    digit per pixel of its box, every box trimmed to its ink, an «H» with a
//    full stem;
//  - the glyph set is what Roboto has of today's 753 (helvR18): «ä ß € — …»
//    in, «Ɔ» (U+0186, which Roboto lacks) out, 468 in all.
//
// Exit 0 on pass, 1 on a failure, 2 when there is no Python with Pillow -
// the code the designer's hil/test-all.js reports as SKIPPED, loudly.
const fs = require("fs");
const os = require("os");
const path = require("path");
const { spawnSync } = require("child_process");

const REPO = path.resolve(__dirname, "..", "..");
const TOOL = path.join(REPO, "tools", "ttf-to-bdf.py");
const OUT = fs.mkdtempSync(path.join(os.tmpdir(), "ttf-to-bdf-check-"));
const PYTHON = process.env.PYTHON || "python";

const pillow = spawnSync(PYTHON, ["-c", "import PIL"], { encoding: "utf8" });
if (pillow.status !== 0) {
  console.warn(`SKIPPED - no Python with Pillow (${PYTHON}); pip install pillow`);
  process.exit(2);
}

const failures = [];
const check = (ok, what) => {
  console.log(`  ${ok ? "PASS" : "FAIL"} ${what}`);
  if (!ok) failures.push(what);
};

function tool(args) {
  const r = spawnSync(PYTHON, [TOOL, ...args], { encoding: "utf8" });
  if (r.status !== 0) throw new Error(`ttf-to-bdf.py ${args.join(" ")}: ${r.stderr || r.stdout}`);
  return r.stdout.trim();
}

// 1 bit, unchanged.
const seg7 = path.join(OUT, "seg7-61.bdf");
tool([path.join(REPO, "tools", "fonts-src", "dseg7", "DSEG7Classic-Regular.ttf"), "61", seg7, "Seven-Segment-seg7-61"]);
const shipped = fs.readFileSync(path.join(REPO, "ddf-source-waveshare4v3b", "fonts", "seg7-61.bdf"));
check(Buffer.compare(fs.readFileSync(seg7), shipped) === 0, "1 bit: DSEG7 at line 61 is byte for byte the 4.3B's seg7-61.bdf");

// 4 bits, twice.
const roboto = path.join(REPO, "tools", "fonts-src", "roboto", "Roboto[wdth,wght].ttf");
const charset = path.join(REPO, "ddf-source-waveshare4v3b", "fonts", "helvR18.bdf");
const args = (out) => [roboto, "14", out, "Roboto-Regular-check", "--bpp", "4", "--by", "cap", "--axes", "wght=400,wdth=100", "--charset-from", charset];
const a = path.join(OUT, "a.bdf");
const b = path.join(OUT, "b.bdf");
console.log(`  ${tool(args(a))}`);
tool(args(b));
const text = fs.readFileSync(a, "utf8");
check(text === fs.readFileSync(b, "utf8"), "4 bits: the same input gives the same bytes");

// 4 bits, well formed.
check(/^BITS_PER_PIXEL 4$/m.test(text), "4 bits: BITS_PER_PIXEL 4");
check(/^CAP_HEIGHT 14$/m.test(text), "4 bits: CAP_HEIGHT is the 14 asked for");
const glyphs = {};
let malformed = 0;
let untrimmed = 0;
for (const m of text.matchAll(/ENCODING (\d+)\n[\s\S]*?BBX (\d+) (\d+) (-?\d+) (-?\d+)\nBITMAP\n([\s\S]*?)ENDCHAR/g)) {
  const [, code, w, h] = m.slice(0, 4).map((v, i) => (i ? Number(v) : v));
  const rows = m[6].split("\n").filter(Boolean);
  glyphs[Number(m[1])] = rows;
  if (rows.length !== h || rows.some((r) => r.length !== w || !/^[0-9A-F]*$/.test(r))) malformed++;
  if (rows.length && (/^0+$/.test(rows[0]) || /^0+$/.test(rows[rows.length - 1]) ||
      rows.every((r) => r[0] === "0") || rows.every((r) => r[r.length - 1] === "0"))) untrimmed++;
}
check(malformed === 0, `4 bits: every row one hex digit per pixel of its box (${malformed} glyph(s) off)`);
check(untrimmed === 0, `4 bits: every box trimmed to its ink (${untrimmed} glyph(s) with an empty edge)`);
// A row a quarter down, above the crossbar: the stem is full within its
// first two pixels (the very first may be an anti-aliased edge).
const h = glyphs["H".codePointAt(0)] || [];
const upper = h[Math.floor(h.length / 4)] || "";
check(h.length >= 13 && h.length <= 15 && upper.slice(0, 2).includes("F"), `4 bits: «H» ${h.length} rows tall with a full stem (row ${upper})`);

// The glyph set.
const has = (ch) => glyphs[ch.codePointAt(0)] !== undefined;
check(["ä", "ß", "€", "—", "…", "°"].every(has), "glyphs: ä ß € — … ° are there");
check(!has("Ɔ"), "glyphs: U+0186, which Roboto lacks, is not");
check(Object.keys(glyphs).length === 468, `glyphs: 468 of helvR18's 753 (got ${Object.keys(glyphs).length})`);

fs.rmSync(OUT, { recursive: true, force: true });
if (failures.length) {
  console.log(`FAIL - ${failures.length} check(s)`);
  process.exit(1);
}
console.log("PASS - ttf-to-bdf.py: 1 bit unchanged, 4 bits reproducible and well formed");
