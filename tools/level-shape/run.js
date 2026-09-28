// Holds this device's bar geometry and colours to the designer's own numbers,
// on a laptop, with no panel connected.
//
//   node tools/level-shape/run.js
//
// Under tools/ and not test/, for the same reason tools/switch-shape/ and
// tools/arc-raster/ are: PlatformIO reserves test/ for its own runner and would
// try to cross-compile this for the ESP32. It is a host program - it builds the
// firmware's own src/project/LevelShape.h for this laptop and runs it here.
//
// Why this exists. A bar's shape is written four times over - the designer's
// lib/level-shape.ts, LevelShape.h here, LevelShape.kt in the Android app, and
// the reference page - in integer arithmetic chosen so the copies cannot
// produce different pixels. Until today LevelShape.h had never been checked
// against any of them: a HIL run sees a bar only as a count of differing
// pixels, with a panel connected, and only for whatever object happens to be in
// that fixture. The designer already records the answers
// (hil/android/fixtures/build-level-golden.js writes
// schaltli-android/app/src/test/resources/level-shape-golden.json), and the
// same recording serves any port of the rules.
//
// The font metrics are an input rather than an answer, exactly as in the
// recorder: where the header sits depends on the font's own ascent and descent,
// and that is the BDF's answer on the device and the browser's in the designer.
// What is compared is the layout rule.
//
// Needs a desktop C++ compiler. On this machine that is the Visual Studio Build
// Tools; g++ or clang++ on PATH are used instead when present. With none of
// them, or with no golden file, the run says so loudly and exits non-zero
// rather than passing quietly - a check that skips itself in silence is a check
// nobody notices has stopped running.
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync, spawnSync } = require("child_process");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..");
const SRC = path.join(REPO, "src", "project");
const OUT = path.join(os.tmpdir(), "level-shape-check-schaltli-firmware");

const GOLDEN =
  process.env.LEVEL_GOLDEN ||
  path.resolve(REPO, "..", "schaltli-android", "app", "src", "test", "resources",
    "level-shape-golden.json");

function die(message) {
  console.error(`\n${message}`);
  process.exit(1);
}

// Exit 2 rather than 1: this check is about the shape rules, and "there is no
// compiler on this machine" is not an answer about the shape rules. It is the
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
      `It is written by the designer repo:  node hil/android/fixtures/build-level-golden.js\n` +
      `(that needs the designer's dev server running). Point LEVEL_GOLDEN at it if it lives elsewhere.`,
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
  const shim = path.join(HERE, "shim");
  const gpp = ["g++", "clang++"].find(haveOnPath);
  if (gpp) {
    execFileSync(gpp, ["-O2", "-std=c++17", `-I${shim}`, `-I${SRC}`, "-o", exe, cpp], {
      stdio: "inherit",
    });
    return exe;
  }
  if (!VCVARS) {
    skip(
      "No desktop C++ compiler found - no g++ or clang++ on PATH, and no Visual Studio\n" +
        "Build Tools in the usual places. This check compiles src/project/LevelShape.h for\n" +
        "the host; without a compiler it cannot run, and has NOT passed.",
    );
  }
  // Through a batch file rather than `cmd /c "<one long string>"`: the quotes
  // vcvars64.bat's own path needs do not survive the round trip through Node's
  // argument escaping.
  const bat = path.join(OUT, `build-${name}.bat`);
  fs.writeFileSync(
    bat,
    `@echo off\r\ncall "${VCVARS.replace(/\//g, "\\")}" >nul\r\n` +
      `cd /d "${OUT}"\r\n` +
      `cl /nologo /EHsc /O2 /I "${shim}" /I "${SRC}" /Fe:"${exe}" "${cpp}"\r\n`,
  );
  const r = spawnSync("cmd", ["/c", bat], { encoding: "utf8" });
  if (r.status !== 0 || !fs.existsSync(exe)) {
    die(`Compiling ${name}.cpp failed:\n${r.stdout || ""}${r.stderr || ""}`);
  }
  return exe;
}

const goldenExe = build("golden");

// --- the cases --------------------------------------------------------------

const recorded = JSON.parse(fs.readFileSync(GOLDEN, "utf8"));
const CASE_FILE = path.join(OUT, "case.txt");
const word = (s) => (s === undefined || s === null || s === "" ? "-" : String(s));

function writeCase(input, metrics) {
  const props = input.properties || {};
  // The host build of MaterialColors.h has no DEVICE_COLOR_DEPTH_* defined, so
  // applyDepth is the identity here. That is right for a 24-bit recording and
  // wrong for any other, so a case at another depth stops the run rather than
  // comparing unquantised colours and calling it a pass.
  if ((input.colorDepth || "24bit") !== "24bit") {
    die(
      `Case recorded at colorDepth ${input.colorDepth}. This harness compiles LevelShape.h\n` +
        `with no depth defined, which makes materialcolors::applyDepth the identity - so the\n` +
        `derived track colour would not be compared at all. Build golden.cpp with\n` +
        `DEVICE_COLOR_DEPTH_1BIT / _4BIT before adding such a case.`,
    );
  }
  fs.writeFileSync(
    CASE_FILE,
    [
      [input.type, input.x, input.y, input.width, input.height].join(" "),
      [input.percent, input.setpointPercent ?? -1].join(" "),
      [
        word(props.fillColor),
        word(input.background),
        word(props.displayValue),
        word(props.barDirection),
        props.barThickness ?? 0,
        props.iconAssetId ? 1 : 0,
        word(props.writeTopic),
        word(props.setpointTopic),
        props.fontSize ?? 0,
        props.glow ?? 0,
      ].join(" "),
      [metrics.ascent, metrics.descent, metrics.capHeight].join(" "),
      typeof props.label === "string" ? props.label : "",
      "",
    ].join("\n"),
  );
}

// What the firmware printed, as a map of the first word to the rest, with the
// repeated lines (seg, frame) kept as lists.
function parse(text) {
  const out = { seg: [], frame: [] };
  for (const line of text.trim().split(/\r?\n/)) {
    const parts = line.trim().split(/\s+/);
    const key = parts.shift();
    if (key === "seg" || key === "frame") out[key].push(parts);
    else out[key] = parts;
  }
  return out;
}

function run(input, metrics) {
  writeCase(input, metrics);
  return parse(execFileSync(goldenExe, [CASE_FILE], { encoding: "utf8" }));
}

let failures = 0;

for (const [name, entry] of Object.entries(recorded.cases)) {
  const want = entry.shape;
  const got = run(entry.input, want.metrics);

  const bad = [];
  const num = (v) => Number(v);
  const check = (label, mine, theirs) => {
    if (mine !== theirs) bad.push(`${label}: got ${mine}, want ${theirs}`);
  };
  const checkRect = (label, mine, theirs) => {
    check(`${label}.x`, num(mine[0]), theirs.x);
    check(`${label}.y`, num(mine[1]), theirs.y);
    check(`${label}.w`, num(mine[2]), theirs.w);
    check(`${label}.h`, num(mine[3]), theirs.h);
    check(`${label}.r`, num(mine[4]), theirs.r);
  };
  // The designer says "no header" / "no handle" with null; this side says it
  // with a flag, so the absence is compared before the numbers are.
  const checkMaybeRect = (label, mine, theirs) => {
    check(`${label} present`, mine[0] === "1", theirs !== null && theirs !== undefined);
    if (mine[0] === "1" && theirs) checkRect(label, mine.slice(1), theirs);
  };
  const checkSegment = (label, mine, theirs) => {
    checkRect(label, mine, theirs);
    check(`${label}.role`, mine[5], theirs.role);
    check(`${label}.roundStart`, mine[6] === "1", theirs.roundStart);
    check(`${label}.roundEnd`, mine[7] === "1", theirs.roundEnd);
  };

  const [vertical, fromEnd, thickness, hasHandle, showsNumber, headerHeight, valueWidth] = got.flags;
  check("vertical", vertical === "1", want.vertical);
  check("fillsFromEnd", fromEnd === "1", want.fillsFromEnd);
  check("thickness", num(thickness), want.thickness);
  check("hasHandle", hasHandle === "1", want.hasHandle);
  check("showsNumber", showsNumber === "1", want.showsNumber);
  check("headerHeight", num(headerHeight), want.headerHeight);
  check("valueWidth", num(valueWidth), want.valueWidth);

  // An input, echoed back: it only proves the case file reached the program
  // intact, which is worth the three comparisons it costs.
  check("metrics.ascent", num(got.metrics[0]), want.metrics.ascent);
  check("metrics.descent", num(got.metrics[1]), want.metrics.descent);
  check("metrics.capHeight", num(got.metrics[2]), want.metrics.capHeight);

  checkMaybeRect("layout.header", got.header, want.layout.header);
  check("layout.baseline", num(got.baseline[0]), want.layout.baseline);
  checkMaybeRect("layout.icon", got.icon, want.layout.icon);
  checkMaybeRect("layout.text", got.text, want.layout.text);
  checkMaybeRect("layout.value", got.value, want.layout.value);
  checkRect("layout.bar", got.bar, want.layout.bar);
  checkRect("layout.slot", got.slot, want.layout.slot);
  checkRect("layout.track", got.laytrack, want.layout.track);
  checkRect("track", got.track, want.track);
  checkSegment("emptyTrack", got.empty, want.emptyTrack);
  checkMaybeRect("handle", got.handle, want.handle);
  // A bar's pointer (2026-09-28): a box and which way its tip faces. Older
  // recordings have no pointer at all, which reads as none.
  const wantPointer = want.pointer ?? null;
  check("pointer present", got.pointer[0] === "1", wantPointer !== null);
  if (got.pointer[0] === "1" && wantPointer) {
    check("pointer.x", num(got.pointer[1]), wantPointer.x);
    check("pointer.y", num(got.pointer[2]), wantPointer.y);
    check("pointer.w", num(got.pointer[3]), wantPointer.w);
    check("pointer.h", num(got.pointer[4]), wantPointer.h);
    check("pointer.tip", got.pointer[5], wantPointer.tip);
  }

  check("segments", got.seg.length, want.segments.length);
  want.segments.forEach((seg, i) => {
    if (got.seg[i]) checkSegment(`segment[${i}]`, got.seg[i], seg);
  });

  check("trackLook.track", String(got.look[0]).toLowerCase(), String(want.trackLook.track).toLowerCase());
  check("trackLook.framed", got.look[1] === "1", want.trackLook.framed);

  if (bad.length) {
    failures++;
    console.log(`  FAIL ${name}: ${bad.length} value(s) differ`);
    for (const line of bad.slice(0, 10)) console.log(`        ${line}`);
    if (bad.length > 10) console.log(`        ... and ${bad.length - 10} more`);
  } else {
    console.log(`  ok   ${name}`);
  }
}

console.log(
  failures === 0
    ? "\nThe bar's shape and colours agree with the designer, value for value."
    : `\n${failures} case(s) failed.`,
);

// --- the same rule, over a range the recording only samples -------------------
//
// This block was written on 2026-09-22 because the recording had no case for
// the rule it holds. Every recorded slider was an object with room to spare -
// its slot exactly the handle's asked-for length - so a port measuring the
// handle from the slot and a port measuring it from the thickness arrived at
// the same numbers, and no case could tell them apart. The 4.3B's conformance
// bar is NOT such an object: it carries a name, so its slot is only what is
// left under the header, and that is where LevelShape.h was wrong.
//
// The recording has two such cases now - slider-squeezed-by-its-header and
// slider-vertical-squeezed-across - and they catch that defect on their own:
// put it back into LevelShape.h and the plain comparison above fails both,
// before this block runs at all. So this is no longer the only thing standing
// between the defect and a green run.
//
// It is kept anyway, because it says something the two cases cannot: they pin
// two points (18 px across and 34), while this squeezes every recorded slider
// to three widths in turn and demands that nothing along the bar moves at any
// of them. Rather than restate the designer's arithmetic here (which is the
// very drift the recording exists to prevent), it holds the rule the designer
// states in prose:
//
//   Nothing along the bar follows from the room across it.
//
// Take a recorded slider, squeeze the object across the bar until the slot has
// to clamp, and every number along the bar - where the handle stands, how wide
// it is, its radius, and where each run of track is cut for it - must come out
// exactly as recorded. Only the measurements across the bar may move.
//
// Source: lib/level-shape.ts, levelHandleSpan's own comment ("Measured from the
// length the thickness ASKS for, not from the one the object had room for"),
// the designer's 2026-09-22 change. Also LevelShape.kt's levelHandleSpan, which
// carried the same fault until the two squeezed cases were recorded.
console.log("\nAnd nothing along the bar follows from the room across it:");

let invariantFailures = 0;

for (const [name, entry] of Object.entries(recorded.cases)) {
  const want = entry.shape;
  if (!want.hasHandle || !want.handle) continue;
  const vertical = want.vertical;
  // What the handle asks for, which is what the slot is being squeezed below.
  const span = Math.trunc((want.thickness * 11) / 4);
  const barAcross = vertical ? want.layout.bar.w : want.layout.bar.h;
  // What the object spends on things other than the bar - a header, or the
  // number's own row - so that shrinking the object shrinks the bar by as much.
  const overhead = (vertical ? entry.input.width : entry.input.height) - barAcross;
  const targets = [...new Set([span - 4, Math.trunc((span * 2) / 3), Math.trunc(span / 2)])]
    .filter((t) => t >= want.thickness + 2 && t < span)
    .sort((a, b) => b - a);
  if (targets.length === 0) continue;

  const bad = [];
  for (const target of targets) {
    const input = { ...entry.input };
    if (vertical) input.width = overhead + target;
    else input.height = overhead + target;
    const got = run(input, want.metrics);
    const at = `at ${target} px across`;
    // The handle's own numbers along the bar: where it stands, how wide it is,
    // and the radius that follows from that width.
    const [present, hx, hy, hw, hh, hr] = got.handle;
    if (present !== "1") {
      bad.push(`${at}: no handle at all`);
      continue;
    }
    const alongPos = vertical ? Number(hy) : Number(hx);
    const alongSize = vertical ? Number(hh) : Number(hw);
    const wantPos = vertical ? want.handle.y : want.handle.x;
    const wantSize = vertical ? want.handle.h : want.handle.w;
    if (alongPos !== wantPos) bad.push(`${at}: handle stands at ${alongPos}, want ${wantPos}`);
    if (alongSize !== wantSize) bad.push(`${at}: handle is ${alongSize} wide, want ${wantSize}`);
    if (Number(hr) !== want.handle.r) bad.push(`${at}: handle radius ${hr}, want ${want.handle.r}`);
    void hh;

    if (got.seg.length !== want.segments.length) {
      bad.push(`${at}: ${got.seg.length} run(s) of track, want ${want.segments.length}`);
      continue;
    }
    want.segments.forEach((seg, i) => {
      const mine = got.seg[i];
      const a = vertical ? Number(mine[1]) : Number(mine[0]);
      const b = vertical ? Number(mine[3]) : Number(mine[2]);
      const wantA = vertical ? seg.y : seg.x;
      const wantB = vertical ? seg.h : seg.w;
      if (a !== wantA) bad.push(`${at}: run ${i} starts at ${a}, want ${wantA}`);
      if (b !== wantB) bad.push(`${at}: run ${i} is ${b} long, want ${wantB}`);
    });
  }

  if (bad.length) {
    invariantFailures++;
    console.log(`  FAIL ${name}: ${bad.length} value(s) moved when the object was squeezed`);
    for (const line of bad.slice(0, 10)) console.log(`        ${line}`);
    if (bad.length > 10) console.log(`        ... and ${bad.length - 10} more`);
  } else {
    console.log(`  ok   ${name} (${targets.join(", ")} px across, asked for ${span})`);
  }
}

if (invariantFailures) console.log(`\n${invariantFailures} case(s) moved along the bar.`);
process.exit(failures === 0 && invariantFailures === 0 ? 0 : 1);
