#!/usr/bin/env node
// Builds a firmware release: every board, from one tagged commit, with a
// manifest the designer ships (designer repo, docs/2026-09-15-firmware-ota.md,
// decision 3 and step 4).
//
// Firmware reaches devices with the designer: its repo carries a manifest
// naming, per device, the build, its size and SHA-256, the system generation
// it reads and where to download it, and the images themselves are assets of
// a GitHub release in that repo - public, while this repo is private, so a
// Pekaway system can download them without a login. Not committed to git: at
// about 2 MB per board per release, a clone in a van would grow by hundreds of
// megabytes.
//
// A release is a tag, fw-YYYY.MM.DD.N. The tag goes on first and every board
// is built from it, so each image's FIRMWARE_BUILD (pre_build_firmware_build.py)
// is the tag itself - which is exactly what a device then announces in hello,
// and what the designer compares with the manifest. The tool checks that, and
// that each image carries the marker for the device it is filed under
// (FirmwareImage.h), before anything is written.
//
// Without --publish it is a dry run: the tag is set locally, everything is
// built and checked, the images and manifest land in dist/<tag>/, and the
// tag is removed again. Nothing leaves this machine. With --publish it keeps
// and pushes the tag, creates the release in the designer repo with the
// images attached, and writes firmware/manifest.json into the designer
// checkout for you to commit.
//
//   node tools/release-firmware.js                 # dry run
//   node tools/release-firmware.js --publish       # the real thing
//
// Options: --designer <path> (default ../v0-screenman-editor-design),
// --release-repo <owner/name> (default Matthias-Hess/screenbee-designer),
// --allow-dirty (dry run only - a release is never built from uncommitted
// changes).

const fs = require("fs")
const os = require("os")
const path = require("path")
const crypto = require("crypto")
const { spawnSync } = require("child_process")

const REPO = path.join(__dirname, "..")

// The boards a release carries. The board directory holds DeviceInfo.h, which
// names the device.
const BOARDS = [
  { env: "waveshare-knob-touch-lcd-1v8", dir: "waveshareknob1v8" },
  { env: "waveshare-touch-lcd-4v3b", dir: "waveshare4v3b" },
  { env: "m5stack-papers3", dir: "papers3" },
]

function arg(name, fallback) {
  const i = process.argv.indexOf(name)
  return i !== -1 && process.argv[i + 1] ? process.argv[i + 1] : fallback
}

const publish = process.argv.includes("--publish")
const allowDirty = process.argv.includes("--allow-dirty")
const designer = path.resolve(arg("--designer", path.join(REPO, "..", "v0-screenman-editor-design")))
const releaseRepo = arg("--release-repo", "Matthias-Hess/screenbee-designer")

function fail(message) {
  console.error(`[release] ERROR: ${message}`)
  process.exit(1)
}

function run(cmd, args, opts = {}) {
  const result = spawnSync(cmd, args, { cwd: REPO, encoding: "utf8", ...opts })
  if (result.error) fail(`${cmd} could not be started: ${result.error.message}`)
  return result
}

function git(...args) {
  const r = run("git", args)
  if (r.status !== 0) fail(`git ${args.join(" ")} failed:\n${r.stderr}`)
  return r.stdout.trim()
}

function findPio() {
  const onPath = run(process.platform === "win32" ? "where" : "which", ["pio"])
  if (onPath.status === 0 && onPath.stdout.trim()) return onPath.stdout.trim().split(/\r?\n/)[0]
  const penv = path.join(os.homedir(), ".platformio", "penv", process.platform === "win32" ? "Scripts" : "bin",
    process.platform === "win32" ? "pio.exe" : "pio")
  if (fs.existsSync(penv)) return penv
  fail("pio not found on PATH or in ~/.platformio/penv")
}

function deviceInfo(dir) {
  const text = fs.readFileSync(path.join(REPO, "src", "boards", dir, "DeviceInfo.h"), "utf8")
  const id = text.match(/#define DEVICE_ID "([^"]+)"/)
  const major = text.match(/#define SYSTEM_GENERATION_MAJOR (\d+)/)
  const minor = text.match(/#define SYSTEM_GENERATION_MINOR (\d+)/)
  if (!id || !major || !minor) fail(`DeviceInfo.h in src/boards/${dir} lacks DEVICE_ID or SYSTEM_GENERATION`)
  return { deviceId: id[1], systemGeneration: `${major[1]}.${minor[1]}` }
}

function nextTag() {
  const now = new Date()
  const day = `${now.getFullYear()}.${String(now.getMonth() + 1).padStart(2, "0")}.${String(now.getDate()).padStart(2, "0")}`
  const existing = git("tag", "-l", `fw-${day}.*`).split(/\r?\n/).filter(Boolean)
  const n = existing.map((t) => Number(t.split(".").pop())).filter(Number.isFinite)
  return `fw-${day}.${n.length ? Math.max(...n) + 1 : 1}`
}

// --- preflight -----------------------------------------------------------
const dirty = git("status", "--porcelain")
if (dirty && (publish || !allowDirty)) {
  fail(`the tree has uncommitted changes - a release is built from a commit:\n${dirty}`)
}
if (publish) {
  const gh = run("gh", ["auth", "status"])
  if (gh.status !== 0) fail("gh is not logged in - run gh auth login")
  if (!fs.existsSync(path.join(designer, "package.json"))) fail(`no designer checkout at ${designer}`)
}

const headTags = git("tag", "--points-at", "HEAD", "-l", "fw-*").split(/\r?\n/).filter(Boolean)
const tag = headTags[0] || nextTag()
const createdTag = headTags.length === 0
const commit = git("rev-parse", "HEAD")
console.log(`[release] ${tag} from ${commit.slice(0, 10)}${publish ? "" : " (dry run - nothing is published)"}`)
if (createdTag) git("tag", "-a", tag, "-m", `Firmware release ${tag}`)

const removeTag = () => {
  if (createdTag && !publish) {
    git("tag", "-d", tag)
    console.log(`[release] dry run: local tag ${tag} removed again`)
  }
}

let manifest
try {
  // --- build and check every board ----------------------------------------
  const pio = findPio()
  const outDir = path.join(REPO, "dist", tag)
  fs.mkdirSync(outDir, { recursive: true })
  const devices = {}

  for (const board of BOARDS) {
    const { deviceId, systemGeneration } = deviceInfo(board.dir)
    console.log(`\n[release] building ${board.env} (${deviceId})`)
    const build = run(pio, ["run", "-e", board.env], { stdio: "inherit" })
    if (build.status !== 0) throw new Error(`pio run -e ${board.env} failed`)

    const generated = fs.readFileSync(path.join(REPO, "src", "FirmwareBuild.h"), "utf8").match(/#define FIRMWARE_BUILD "([^"]+)"/)
    if (!generated || generated[1] !== tag) {
      throw new Error(`${board.env} was built as ${generated ? generated[1] : "unknown"}, not ${tag}`)
    }

    const image = fs.readFileSync(path.join(REPO, ".pio", "build", board.env, "firmware.bin"))
    const markers = [...new Set(image.toString("latin1").match(/<<screenbee-image device=[a-z0-9-]+>>/g) || [])]
    if (markers.length !== 1 || markers[0] !== `<<screenbee-image device=${deviceId}>>`) {
      throw new Error(`${board.env} carries ${markers.join(", ") || "no marker"}, expected ${deviceId}'s`)
    }
    if (!image.includes(Buffer.from(`"${tag}"`)) && !image.includes(Buffer.from(tag))) {
      throw new Error(`${board.env}'s image does not contain its build ${tag}`)
    }

    const file = `${deviceId}-${tag}.bin`
    fs.writeFileSync(path.join(outDir, file), image)
    devices[deviceId] = {
      build: tag,
      file,
      size: image.length,
      sha256: crypto.createHash("sha256").update(image).digest("hex"),
      systemGeneration,
      url: `https://github.com/${releaseRepo}/releases/download/${tag}/${file}`,
    }
    console.log(`[release] ${file}: ${image.length} bytes, sha256 ${devices[deviceId].sha256}`)
  }

  manifest = { release: tag, commit, repository: releaseRepo, devices }
  fs.writeFileSync(path.join(outDir, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n")
  console.log(`\n[release] images and manifest in ${outDir}`)
} catch (e) {
  removeTag()
  fail(e.message)
}

if (!publish) {
  removeTag()
  console.log(`[release] dry run complete. --publish would push ${tag}, create the release in ${releaseRepo}` +
    ` with ${Object.keys(manifest.devices).length} images, and write firmware/manifest.json into ${designer}.`)
  process.exit(0)
}

// --- publish ---------------------------------------------------------------
const outDir = path.join(REPO, "dist", tag)
console.log(`\n[release] pushing ${tag}`)
const push = run("git", ["push", "origin", tag], { stdio: "inherit" })
if (push.status !== 0) fail(`git push origin ${tag} failed`)

const notes = [
  `Firmware ${tag}, built from screenbee-firmware ${commit.slice(0, 10)}.`,
  "",
  ...Object.entries(manifest.devices).map(([id, d]) => `- ${id}: ${d.file}, ${d.size} bytes, sha256 ${d.sha256}`),
  "",
  "Installed by the designer: deploy/pekaway-install.sh downloads the images firmware/manifest.json names.",
].join("\n")
const assets = Object.values(manifest.devices).map((d) => path.join(outDir, d.file))
const release = run("gh", ["release", "create", tag, "--repo", releaseRepo, "--title", `Firmware ${tag}`, "--notes", notes, ...assets],
  { stdio: "inherit" })
if (release.status !== 0) fail(`gh release create failed - the tag is pushed; create the release by hand or re-run`)

const target = path.join(designer, "firmware", "manifest.json")
fs.mkdirSync(path.dirname(target), { recursive: true })
fs.writeFileSync(target, JSON.stringify(manifest, null, 2) + "\n")
console.log(`\n[release] ${tag} published. Wrote ${target} - commit and push it in the designer repo.`)
