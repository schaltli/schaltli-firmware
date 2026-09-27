// Checks PillRaster::pixelBandsQuick() against PillRaster::pixelBands() - the
// firmware-only shortcut that decides whole pixels without sub-samples - on a
// laptop, with no board connected.
//
//   node tools/pill-raster/run.js
//
// Under tools/ and not test/ for the reason tools/switch-shape/ gives:
// PlatformIO reserves test/ for its own runner. Compiled against
// tools/switch-shape/shim, whose Arduino.h is all PillRaster.h needs.
//
// Exit 0 on pass, 1 on a failed case, 2 when there is no compiler - the code
// the designer's hil/test-all.js reports as SKIPPED, loudly.
const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync, spawnSync } = require("child_process");

const HERE = __dirname;
const REPO = path.resolve(HERE, "..", "..");
const SRC = path.join(REPO, "src", "project");
const SHIM = path.join(REPO, "tools", "switch-shape", "shim");
const OUT = path.join(os.tmpdir(), "pill-raster-check-schaltli-firmware");
fs.mkdirSync(OUT, { recursive: true });

function skip(message) {
  console.error(`\n${message}`);
  process.exit(2);
}

function haveOnPath(exe) {
  return spawnSync(exe, ["--version"], { encoding: "utf8" }).status === 0;
}

const VCVARS = [
  "C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2019/BuildTools/VC/Auxiliary/Build/vcvars64.bat",
  "C:/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Auxiliary/Build/vcvars64.bat",
].find((p) => fs.existsSync(p));

const exe = path.join(OUT, "check.exe");
const cpp = path.join(HERE, "check.cpp");
const gpp = ["g++", "clang++"].find(haveOnPath);
if (gpp) {
  execFileSync(gpp, ["-O2", "-std=c++17", `-I${SHIM}`, `-I${SRC}`, "-o", exe, cpp], { stdio: "inherit" });
} else if (VCVARS) {
  const bat = path.join(OUT, "build.bat");
  fs.writeFileSync(
    bat,
    `@echo off\r\ncall "${VCVARS.replace(/\//g, "\\")}" >nul\r\n` +
      `cd /d "${OUT}"\r\n` +
      `cl /nologo /EHsc /O2 /std:c++17 /I "${SHIM}" /I "${SRC}" /Fe:"${exe}" "${cpp}"\r\n`,
  );
  const r = spawnSync("cmd", ["/c", bat], { encoding: "utf8" });
  if (r.status !== 0 || !fs.existsSync(exe)) {
    console.error(`Compiling check.cpp failed:\n${r.stdout || ""}${r.stderr || ""}`);
    process.exit(1);
  }
} else {
  skip(
    "No desktop C++ compiler found - no g++ or clang++ on PATH, and no Visual Studio\n" +
      "Build Tools in the usual places. This check has NOT passed.",
  );
}

const run = spawnSync(exe, [], { encoding: "utf8" });
process.stdout.write(run.stdout || "");
process.stderr.write(run.stderr || "");
process.exit(run.status === 0 ? 0 : 1);
