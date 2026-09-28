// Holds this device's arc rasterizer to the designer's own numbers, on a
// laptop, with no panel connected.
//
//   node tools/arc-raster/run.js
//
// Under tools/ and not test/, because PlatformIO reserves test/ for its own
// runner and would try to cross-compile these two files for the ESP32. They
// are host programs: they build the firmware's own rasterizer for this laptop
// and run it here.
//
// Why this exists. The arc rasterizer is written four times - the designer's
// lib/arc-raster.ts, ArcRaster.cpp here, ArcRaster.cpp in schaltli-eink,
// ArcRaster.kt in the Android app - in integer arithmetic chosen so the
// copies cannot produce different pixels. Everything else this firmware draws
// is checked by a HIL run against the real panel; the rasterizer does not
// have to wait for one, because the designer already records what it produces
// (the designer repo's hil/android/fixtures/build-arc-golden.js writes
// schaltli-android/app/src/test/resources/arc-raster-golden.json). The same
// recording serves any port of it.
//
// And it matters HERE because this repo serves three boards from one copy of
// the algorithm - the two 24-bit Waveshare panels and the 4-bit PaperS3. A HIL
// run puts one of them in front of the designer at a time, and only for the
// dial that happens to be in that fixture; this holds the rasterizer itself to
// all eleven recorded geometries, on every board's behalf, before any of them
// is flashed.
//
// It also exercises the branch none of these boards takes every day. `framed` -
// the track drawn as the band's own one-pixel outline, where the halfway tint
// it would otherwise be lands back on the background - is the 1-bit panel's
// ordinary case and the PaperS3's corner case, and a branch nobody runs is a
// branch that rots.
//
// Two checks run:
//
//   1. golden - every pixel the designer recorded, band counts and blended
//      colour, with the uniform short cut both on and off.
//   2. selftest - over every pixel of every geometry, plus a sweep of framed
//      dials at many fill and handle angles: the uniform short cut agrees
//      with exhaustive sampling, and rowSpan never drops a covered pixel.
//      Those two are firmware-only optimisations with no counterpart in the
//      designer, so the golden cannot see them.
//   3. glow - a ring's gradient step and glow level per recorded pixel, and
//      the colours the two mix (the golden's "glowCases" and "glowColours",
//      2026-09-28); and that rowSpan, widened by the glow, drops none of it.
//
// Needs a desktop C++ compiler. On this machine that is the Visual Studio
// Build Tools; g++ or clang++ on PATH are used instead when present. With
// none of them, or with no golden file, the run says so loudly and exits
// non-zero rather than passing quietly - a check that skips itself in silence
// is a check nobody notices has stopped running.
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync, spawnSync } = require("child_process");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..");
const SRC = path.join(REPO, "src", "project");
// Named after this repo: schaltli-eink carries the same harness, and two
// of them building "golden.exe" into one directory would each be checking the
// other repo's rasterizer without saying so.
const OUT = path.join(os.tmpdir(), "arc-raster-check-schaltli-firmware");
const DEG = 64; // 1/64 degree units, the rasterizer's own scale

const GOLDEN =
  process.env.ARC_GOLDEN ||
  path.resolve(REPO, "..", "schaltli-android", "app", "src", "test", "resources",
    "arc-raster-golden.json");

function die(message) {
  console.error(`\n${message}`);
  process.exit(1);
}

// Exit 2 rather than 1: this check is about the rasterizer, and "there is no
// compiler on this machine" is not an answer about the rasterizer. It is the
// code hil/test-all.js reads as SKIPPED - loudly, in the summary - the same
// way it treats a board that is not plugged in. A real disagreement still
// leaves with 1 and fails the run.
function skip(message) {
  console.error(`\n${message}`);
  process.exit(2);
}

if (!fs.existsSync(GOLDEN)) {
  skip(
    `No golden file at ${GOLDEN}.\n` +
      `It is written by the designer repo:  node hil/android/fixtures/build-arc-golden.js\n` +
      `(that needs the designer's dev server running). Point ARC_GOLDEN at it if it lives elsewhere.`,
  );
}

// --- compiler ---------------------------------------------------------------

fs.mkdirSync(OUT, { recursive: true });

function haveOnPath(exe) {
  const probe = spawnSync(exe, ["--version"], { encoding: "utf8" });
  return probe.status === 0;
}

const VCVARS = [
  "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Auxiliary/Build/vcvars64.bat",
].find((p) => fs.existsSync(p));

function build(name) {
  const exe = path.join(OUT, `${name}.exe`);
  const cpp = path.join(HERE, `${name}.cpp`);
  const raster = path.join(SRC, "ArcRaster.cpp");
  const shim = path.join(HERE, "shim");
  const gpp = ["g++", "clang++"].find(haveOnPath);
  if (gpp) {
    execFileSync(gpp, ["-O2", "-std=c++17", `-I${shim}`, `-I${SRC}`, "-o", exe, cpp, raster], {
      stdio: "inherit",
    });
    return exe;
  }
  if (!VCVARS) {
    skip(
      "No desktop C++ compiler found - no g++ or clang++ on PATH, and no Visual Studio\n" +
        "Build Tools in the usual places. This check compiles src/project/ArcRaster.cpp for\n" +
        "the host; without a compiler it cannot run, and has NOT passed.",
    );
  }
  // Through a batch file rather than `cmd /c "<one long string>"`: the quotes
  // vcvars64.bat's own path needs do not survive the round trip through
  // Node's argument escaping.
  const bat = path.join(OUT, `build-${name}.bat`);
  fs.writeFileSync(
    bat,
    // cd into the output directory rather than passing /Fo: a quoted path
    // ending in a backslash is read by the CRT as an escaped quote, and cl
    // then reports a missing source file.
    `@echo off\r\ncall "${VCVARS.replace(/\//g, "\\")}" >nul\r\n` +
      `cd /d "${OUT}"\r\n` +
      `cl /nologo /EHsc /O2 /I "${shim}" /I "${SRC}" /Fe:"${exe}" "${cpp}" "${raster}"\r\n`,
  );
  const r = spawnSync("cmd", ["/c", bat], { encoding: "utf8" });
  if (r.status !== 0 || !fs.existsSync(exe)) {
    die(`Compiling ${name}.cpp failed:\n${r.stdout || ""}${r.stderr || ""}`);
  }
  return exe;
}

const goldenExe = build("golden");
const selftestExe = build("selftest");
const glowExe = build("glow");

// --- the cases --------------------------------------------------------------

const recorded = JSON.parse(fs.readFileSync(GOLDEN, "utf8"));
const C = recorded.colours;
const hex = (s) => s.replace("#", "");
const CASE_FILE = path.join(OUT, "case.txt");

const geometryOf = (c) => ({
  size: c.size,
  thickness: c.thickness,
  inset: c.inset ?? 0,
  trackStart64: c.trackStart64,
  trackSweep64: c.trackSweep64,
  fillStart64: c.fillStart64,
  fillSweep64: c.fillSweep64,
  handleAt64: c.handleAt64 === undefined ? -1 : c.handleAt64,
  pointerAt64: c.pointerAt64 === undefined ? -1 : c.pointerAt64,
  startCapFilled: c.startCapFilled ? 1 : 0,
  endCapFilled: c.endCapFilled ? 1 : 0,
  framed: c.framed ? 1 : 0,
});

function writeCase(g, pixels) {
  const header = [
    g.size, g.thickness, g.inset, g.trackStart64, g.trackSweep64, g.fillStart64, g.fillSweep64,
    g.handleAt64, g.pointerAt64 ?? -1, g.startCapFilled, g.endCapFilled, g.framed,
    // A recording from before the pointer (2026-09-28) has no colour for it.
    hex(C.track), hex(C.fill), hex(C.handle), hex(C.pointer ?? C.handle), hex(C.background),
    pixels ? pixels.length : 0,
  ].join(" ");
  const body = pixels ? pixels.map((p) => `${p[0]} ${p[1]}`).join("\n") + "\n" : "";
  fs.writeFileSync(CASE_FILE, header + "\n" + body);
}

// --- 1: the designer's recorded pixels --------------------------------------

let failures = 0;

for (const fastPath of ["1", "0"]) {
  console.log(`\nAgainst the designer's recording (uniform short cut ${fastPath === "1" ? "on" : "off"}):`);
  for (const c of recorded.cases) {
    writeCase(geometryOf(c), c.pixels);
    const out = execFileSync(goldenExe, [CASE_FILE, fastPath], {
      encoding: "utf8",
      maxBuffer: 1 << 28,
    });
    const lines = out.trim().split(/\r?\n/);
    if (lines.length !== c.pixels.length) {
      console.log(`  FAIL ${c.name}: ${lines.length} rows for ${c.pixels.length} pixels`);
      failures++;
      continue;
    }
    let bad = 0;
    let first = "";
    for (let i = 0; i < lines.length; i++) {
      const [f, t, h, p, rgb] = lines[i].split(" ").map(Number);
      const wantP = c.pointer ? c.pointer[i] : 0;
      if (f !== c.fill[i] || t !== c.track[i] || h !== c.handle[i] || p !== wantP || rgb !== c.rgb[i]) {
        if (bad === 0) {
          first =
            `px=(${c.pixels[i]}) got ${f}/${t}/${h}/${p} #${rgb.toString(16).padStart(6, "0")}, ` +
            `want ${c.fill[i]}/${c.track[i]}/${c.handle[i]}/${wantP} #${c.rgb[i].toString(16).padStart(6, "0")}`;
        }
        bad++;
      }
    }
    if (bad) {
      console.log(`  FAIL ${c.name}: ${bad} of ${lines.length} pixels differ\n        first ${first}`);
      failures++;
    } else {
      console.log(`  ok   ${c.name}`);
    }
  }
}

// --- 2: the firmware's own short cuts ---------------------------------------

// Every geometry the golden carries, plus a framed dial walked round: the
// branch only this device takes, and one the golden pins at a single angle.
const swept = [];
for (const [size, thickness] of [[78, 12], [61, 5], [120, 22], [40, 8], [31, 1]]) {
  const inset = Math.max(
    0,
    Math.min(Math.ceil((thickness * 7) / 8), Math.floor(size / 2) - thickness - 1),
  );
  for (let fillDeg = 0; fillDeg <= 270; fillDeg += 27) {
    for (let handleDeg = 0; handleDeg <= 270; handleDeg += 45) {
      swept.push({
        size,
        thickness,
        inset,
        trackStart64: 225 * DEG,
        trackSweep64: 270 * DEG,
        fillStart64: 225 * DEG,
        fillSweep64: fillDeg * DEG,
        handleAt64: 225 * DEG + handleDeg * DEG,
        startCapFilled: fillDeg > 0 ? 1 : 0,
        endCapFilled: fillDeg >= 270 ? 1 : 0,
        framed: 1,
      });
    }
  }
}
// A gauge's pointer instead of a handle, framed and not, at every eighth of
// the scale: it stands outside the ring, so rowSpan's widening and the
// uniform short cut are what it can catch out (2026-09-28).
for (const [size, thickness] of [[78, 12], [120, 22], [40, 8]]) {
  const inset = Math.max(
    0,
    Math.min(Math.ceil((thickness * 7) / 8), Math.floor(size / 2) - thickness - 1),
  );
  for (const framed of [0, 1]) {
    for (let pointerDeg = 0; pointerDeg <= 270; pointerDeg += 34) {
      swept.push({
        size,
        thickness,
        inset,
        trackStart64: 225 * DEG,
        trackSweep64: 270 * DEG,
        fillStart64: 225 * DEG,
        fillSweep64: 100 * DEG,
        pointerAt64: 225 * DEG + pointerDeg * DEG,
        startCapFilled: 1,
        endCapFilled: 0,
        framed,
      });
    }
  }
}
// And a framed full ring, which has no caps at all to rim.
for (let fillDeg = 0; fillDeg <= 360; fillDeg += 60) {
  swept.push({
    size: 90,
    thickness: 14,
    inset: 13,
    trackStart64: 0,
    trackSweep64: 360 * DEG,
    fillStart64: 0,
    fillSweep64: fillDeg * DEG,
    handleAt64: fillDeg * DEG,
    startCapFilled: 0,
    endCapFilled: 0,
    framed: 1,
  });
}

console.log("\nThe firmware's own short cuts, over every pixel:");
function selfCheck(g, label, quiet) {
  writeCase(g, null);
  const r = spawnSync(selftestExe, [CASE_FILE], { encoding: "utf8" });
  const text = (r.stdout || "").trim();
  if (r.status !== 0) {
    console.log(`  FAIL ${label}\n${text}`);
    failures++;
    return;
  }
  if (!quiet) console.log(`  ok   ${label.padEnd(34)} ${text}`);
}
for (const c of recorded.cases) selfCheck(geometryOf(c), c.name, false);
for (const g of swept) {
  selfCheck(g, `framed ${g.size}x${g.thickness} fill=${g.fillSweep64 / DEG}`, true);
}
console.log(`  ok   ${swept.length} swept framed geometries`);

// --- 3: gradient and glow ---------------------------------------------------

console.log("\nGradient and glow against the designer's recording:");
if (!recorded.glowCases || !recorded.glowColours) {
  console.log("  FAIL the golden file has no glow cases - regenerate it (build-arc-golden.js)");
  failures++;
} else {
  for (const c of recorded.glowCases) {
    const header = [c.size, c.thickness, c.inset, c.start64, c.sweep64, c.fromEnd ? 1 : 0, c.filled64, c.levels,
      c.pixels.length].join(" ");
    fs.writeFileSync(CASE_FILE, header + "\n" + c.pixels.map((p) => `${p[0]} ${p[1]}`).join("\n") + "\n");
    const r = spawnSync(glowExe, ["ring", CASE_FILE], { encoding: "utf8", maxBuffer: 1 << 28 });
    if (r.status !== 0) {
      console.log(`  FAIL ${c.name}: exit ${r.status} ${(r.stderr || "").trim()}`);
      failures++;
      continue;
    }
    const lines = r.stdout.trim().split(/\r?\n/);
    let bad = 0;
    let first = "";
    for (let i = 0; i < c.pixels.length; i++) {
      const [step, level] = (lines[i] || "").split(" ").map(Number);
      if (step !== c.step[i] || level !== c.level[i]) {
        if (bad === 0) first = `px=(${c.pixels[i]}) got ${step}/${level}, want ${c.step[i]}/${c.level[i]}`;
        bad++;
      }
    }
    if (bad) {
      console.log(`  FAIL ${c.name}: ${bad} of ${c.pixels.length} pixels differ\n        first ${first}`);
      failures++;
    } else {
      console.log(`  ok   ${c.name}`);
    }
  }
  const samples = recorded.glowColours;
  fs.writeFileSync(
    CASE_FILE,
    samples.map((q) => [...q.from, ...q.to, q.step, q.steps, q.alpha, ...q.under].join(" ")).join("\n") + "\n",
  );
  const out = execFileSync(glowExe, ["colour", CASE_FILE], { encoding: "utf8" }).trim().split(/\r?\n/);
  const wrong = samples.filter((q, i) => out[i] !== [...q.gradient, ...q.blended].join(" "));
  if (wrong.length) {
    console.log(`  FAIL ${wrong.length} of ${samples.length} colour samples differ`);
    failures++;
  } else {
    console.log(`  ok   ${samples.length} colour samples`);
  }
}

console.log(
  failures === 0
    ? "\nThe arc rasterizer agrees with the designer, pixel for pixel."
    : `\n${failures} check(s) failed.`,
);
process.exit(failures === 0 ? 0 : 1);
