// Holds this device's Switch geometry and colours to the designer's own
// numbers, on a laptop, with no panel connected.
//
//   node tools/switch-shape/run.js
//
// Under tools/ and not test/, for the same reason tools/arc-raster/ is:
// PlatformIO reserves test/ for its own runner and would try to cross-compile
// this for the ESP32. It is a host program - it builds the firmware's own
// src/project/SwitchShape.h for this laptop and runs it here.
//
// Why this exists. A Switch's shape is written four times over - the
// designer's lib/switch-shape.ts, SwitchShape.h here, SwitchShape.kt in the
// Android app - in integer arithmetic chosen so the copies cannot produce
// different pixels. Until today SwitchShape.h had never been checked against
// any of them: a HIL run sees a Switch only as a percentage of differing
// pixels, with a panel connected, and only for whatever object happens to be
// in that fixture. The designer already records the answers
// (hil/android/fixtures/build-switch-golden.js writes
// schaltli-android/app/src/test/resources/switch-shape-golden.json), and the
// same recording serves any port of the rules.
//
// It found its first divergence the day it was written: the connected button
// group's two radii and the 2 px gap between its buttons (the designer's
// 2026-09-21 change) had not been brought across, so every group button here
// was a pill tiled edge to edge with its neighbour.
//
// The font metrics are an input rather than an answer, exactly as in the
// recorder: where a label sits depends on how wide it is, and that is the
// BDF's answer on the device and the browser's in the designer. What is
// compared is the layout rule.
//
// Needs a desktop C++ compiler. On this machine that is the Visual Studio
// Build Tools; g++ or clang++ on PATH are used instead when present. With none
// of them, or with no golden file, the run says so loudly and exits non-zero
// rather than passing quietly - a check that skips itself in silence is a
// check nobody notices has stopped running.
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync, spawnSync } = require("child_process");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..");
const SRC = path.join(REPO, "src", "project");
const OUT = path.join(os.tmpdir(), "switch-shape-check-schaltli-firmware");

const GOLDEN =
  process.env.SWITCH_GOLDEN ||
  path.resolve(REPO, "..", "schaltli-android", "app", "src", "test", "resources",
    "switch-shape-golden.json");

// The C++ runtime linked in on Windows. Git for Windows puts its own, older
// libstdc++-6.dll and libwinpthread-1.dll on PATH ahead of the compiler's,
// and a program built by a newer g++ (WinLibs 16 here) loaded those and
// crashed with 0xC0000005 before printing a line - which read as the shape
// rules failing (test:all, 2026-10-06). Linked statically there is no DLL to
// pick wrong.
const STATIC_RUNTIME = process.platform === "win32" ? ["-static"] : [];

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
      `It is written by the designer repo:  node hil/android/fixtures/build-switch-golden.js\n` +
      `(that needs the designer's dev server running). Point SWITCH_GOLDEN at it if it lives elsewhere.`,
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
    execFileSync(gpp, ["-O2", "-std=c++17", ...STATIC_RUNTIME, `-I${shim}`, `-I${SRC}`, "-o", exe, cpp], {
      stdio: "inherit",
    });
    return exe;
  }
  if (!VCVARS) {
    skip(
      "No desktop C++ compiler found - no g++ or clang++ on PATH, and no Visual Studio\n" +
        "Build Tools in the usual places. This check compiles src/project/SwitchShape.h for\n" +
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
  const states = props.states || [];
  const flags = [];
  for (let i = 0; i < input.stateCount; i++) {
    const state = states[i] || {};
    flags.push(state.showAsOn ?? state.showMarker ? 1 : 0);
  }
  fs.writeFileSync(
    CASE_FILE,
    [
      [
        input.type,
        input.x,
        input.y,
        input.width,
        input.height,
        input.stateCount,
        input.activeIndex ?? -1,
        input.askedIndex ?? -1,
        input.pressedIndex ?? -1,
        input.textWidth ?? 0,
        input.hasIcon ? 1 : 0,
      ].join(" "),
      [word(props.switchColor), word(props.switchStyle), word(input.background)].join(" "),
      [metrics.ascent, metrics.descent, metrics.capHeight].join(" "),
      flags.join(" "),
      "",
    ].join("\n"),
  );
}

// What the firmware printed, as a map of the first word to the rest, with the
// repeated lines (seg, knob, quietknob, ringfill) kept as lists.
const REPEATED = ["seg", "knob", "quietknob", "ringfill"];

function parse(text) {
  const out = Object.fromEntries(REPEATED.map((key) => [key, []]));
  for (const line of text.trim().split(/\r?\n/)) {
    const parts = line.trim().split(/\s+/);
    const key = parts.shift();
    if (REPEATED.includes(key)) out[key].push(parts);
    else out[key] = parts;
  }
  return out;
}

let failures = 0;

for (const [name, entry] of Object.entries(recorded.cases)) {
  const want = entry.shape;
  writeCase(entry.input, want.metrics);
  const got = parse(execFileSync(goldenExe, [CASE_FILE], { encoding: "utf8" }));

  const bad = [];
  const num = (v) => Number(v);
  const check = (label, mine, theirs) => {
    if (mine !== theirs) bad.push(`${label}: got ${mine}, want ${theirs}`);
  };
  const checkHex = (label, mine, theirs) => {
    const a = (mine === "-" ? null : String(mine).toLowerCase());
    const b = theirs === null || theirs === undefined ? null : String(theirs).toLowerCase();
    if (a !== b) bad.push(`${label}: got ${a === null ? "none" : a}, want ${b === null ? "none" : b}`);
  };
  // Every rect carries both radii here; the designer's own rect leaves rRight
  // out when both ends are the same.
  const checkRect = (label, mine, theirs) => {
    check(`${label}.x`, num(mine[0]), theirs.x);
    check(`${label}.y`, num(mine[1]), theirs.y);
    check(`${label}.w`, num(mine[2]), theirs.w);
    check(`${label}.h`, num(mine[3]), theirs.h);
    check(`${label}.r`, num(mine[4]), theirs.r);
    check(`${label}.rRight`, num(mine[5]), theirs.rRight ?? theirs.r);
  };
  const checkKnob = (label, mine, theirs) => {
    check(`${label}.cx`, num(mine[0]), theirs.cx);
    check(`${label}.cy`, num(mine[1]), theirs.cy);
    check(`${label}.r`, num(mine[2]), theirs.r);
  };

  check("form", got.form[0], want.form);
  check("corner", num(got.corner[0]), want.corner);
  checkRect("container", got.container, want.container);
  check("segments", got.seg.length, want.segments.length);
  want.segments.forEach((seg, i) => {
    if (got.seg[i]) checkRect(`segment[${i}]`, got.seg[i], seg);
  });
  checkRect("track", got.track, want.track);
  checkRect("labelBox", got.labelbox, want.labelBox);

  // What a ring on each button encloses. The chosen button keeps its OWN
  // colour there; every other one shows the container, or nothing where the
  // container is only an outline. Only a button that is at once the reported
  // state and the one asked for tells the two apart, which the recording had
  // no case for until group-asked-for-what-is-already-chosen.
  if (want.ringFills) {
    check("ringFills", got.ringfill.length, want.ringFills.length);
    want.ringFills.forEach((fill, i) => {
      if (got.ringfill[i]) checkHex(`ringFill[${i}]`, got.ringfill[i][0], fill);
    });
  }
  want.knobs.forEach((knob, i) => {
    if (got.knob[i]) checkKnob(`knob[${i}]`, got.knob[i], knob);
  });
  want.quietKnobs.forEach((knob, i) => {
    if (got.quietknob[i]) checkKnob(`quietKnob[${i}]`, got.quietknob[i], knob);
  });
  checkKnob("pressedKnob", got.pressedknob, want.pressedKnob);
  check("shownIndex", num(got.shown[0]), want.shownIndex);

  const [hasIcon, iconX, iconY, iconW, iconH, textX, baseline, beside] = got.content;
  check("content.hasIcon", hasIcon === "1", want.content.icon !== null);
  if (want.content.icon && hasIcon === "1") {
    checkRect("content.icon", [iconX, iconY, iconW, iconH, 0, 0], { ...want.content.icon, rRight: 0 });
  }
  check("content.textX", num(textX), want.content.textX);
  check("content.baseline", num(baseline), want.content.baseline);
  check("content.beside", beside === "1", want.content.beside);

  const [surface, framed, outline, chosen, onChosen, onSurface, ring] = got.look;
  checkHex("look.surface", surface, want.look.surface);
  check("look.framed", framed === "1", want.look.surfaceOutline !== null);
  if (want.look.surfaceOutline !== null) checkHex("look.surfaceOutline", outline, want.look.surfaceOutline);
  checkHex("look.chosen", chosen, want.look.chosen);
  checkHex("look.onChosen", onChosen, want.look.onChosen);
  checkHex("look.onSurface", onSurface, want.look.onSurface);
  checkHex("look.ring", ring, want.look.ring);

  for (const [key, theirs] of [["knoblook", want.knobLook], ["knoblookoff", want.knobLookOff]]) {
    const [track, trackOutline, knob, onKnob] = got[key];
    checkHex(`${key}.track`, track, theirs.track);
    checkHex(`${key}.trackOutline`, trackOutline, theirs.trackOutline);
    checkHex(`${key}.knob`, knob, theirs.knob);
    checkHex(`${key}.onKnob`, onKnob, theirs.onKnob);
  }
  check("on", got.on[0] === "1", want.on);

  if (bad.length) {
    failures++;
    console.log(`  FAIL ${name}: ${bad.length} value(s) differ`);
    for (const line of bad.slice(0, 8)) console.log(`        ${line}`);
    if (bad.length > 8) console.log(`        ... and ${bad.length - 8} more`);
  } else {
    console.log(`  ok   ${name}`);
  }
}

// A Toggle Button's tap: the next state, whatever x; from none the first
// that shows as on (designer toggleNextIndex, the same cases as its spec).
{
  const want = ["1", "0", "1", "0", "2", "0", "-1"];
  const got = execFileSync(build("toggle-tap"), { encoding: "utf8" }).trim().split(/\s+/);
  if (got.join(" ") !== want.join(" ")) {
    failures++;
    console.log(`  FAIL toggle-tap: got ${got.join(" ")}, want ${want.join(" ")}`);
  } else {
    console.log("  ok   toggle-tap");
  }
}

console.log(
  failures === 0
    ? "\nThe Switch's shape and colours agree with the designer, value for value."
    : `\n${failures} case(s) failed.`,
);
process.exit(failures === 0 ? 0 : 1);
