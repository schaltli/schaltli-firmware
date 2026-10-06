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
// Two images per board leave here. The OTA image is the app alone, which is
// what a running device pulls into its spare slot. The factory image is that
// same app with the bootloader, the partition table and the OTA selector in
// front of it, in one file written at 0x0 - what a brand-new board needs,
// because it has none of those (designer repo,
// docs/2026-09-18-factory-image.md). The byte layout is defined once, in the
// designer's lib/factory-image.mjs, which the flasher page uses as well, so
// what is built here and what is checked there cannot drift apart.
//
// A release is a tag, fw-YYYY.MM.DD.N. The tag goes on first and every board
// is built from it, so each image's FIRMWARE_BUILD (pre_build_firmware_build.py)
// is the tag itself - which is exactly what a device then announces in hello,
// and what the designer compares with the manifest. The tool checks that, and
// that each image carries the marker for the device it is filed under
// (FirmwareImage.h), before anything is written.
//
// Without --publish it is a dry run: a "-dryrun" tag is set locally, everything is
// built and checked, the images and manifest land in dist/<tag>/, and the
// tag is removed again. Nothing leaves this machine. With --publish it keeps
// and pushes the tag, creates the release in the designer repo with the
// images attached, and writes firmware/manifest.json into the designer
// checkout for you to commit.
//
//   node tools/release-firmware.js                 # dry run
//   node tools/release-firmware.js --publish       # the real thing
//
// A dry run also leaves dist/<tag>/CHANGES.txt: the commits since the previous
// release. It is material, not release notes - this repo is private and the
// release public, so nothing from its history is published unless a person
// writes it into a file and passes it with --notes.
//
//   node tools/release-firmware.js                          # dry run, read CHANGES.txt
//   node tools/release-firmware.js --publish --notes n.md   # publish with those notes
//
// The designer checkout is needed for every run, dry or not: the factory
// layout lives there.
//
// A pre-release is a build for one person to try - a fix for a problem they
// reported - before it becomes a release for everyone:
//
//   node tools/release-firmware.js --prerelease knob_crash                 # dry run
//   node tools/release-firmware.js --prerelease knob_crash --publish --notes n.md
//   node tools/release-firmware.js --drop-prerelease fw-2026.09.27.2-pre.knob_crash
//
// Its tag takes the day's next number and a name, fw-2026.09.27.2-pre.knob_crash,
// so it sorts between the releases (the designer's lib/firmware-build.ts). It
// is published as a GitHub pre-release, never "latest", and its manifest goes
// onto a designer branch pre/<name> instead of main: main, the handbook and
// flasher on Pages, and every installation that updates from main stay as they
// are. Whoever should try it installs exactly that:
//
//   curl -fsSL .../deploy/pekaway-install.sh | bash -s -- --ref fw-2026.09.27.2-pre.knob_crash
//
// The branch is made from the designer's HEAD in a worktree of its own, so a
// designer fix that goes with it is on it too - commit it first; the checkout
// you work in is not touched. --drop-prerelease removes the release, the tag
// in both repos and the branch again.
//
// Options: --designer <path> (default ../schaltli-designer),
// --release-repo <owner/name> (default schaltli/schaltli-designer),
// --notes <file> (markdown, placed in the release body),
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
const designer = path.resolve(arg("--designer", path.join(REPO, "..", "schaltli-designer")))
const releaseRepo = arg("--release-repo", "schaltli/schaltli-designer")
const notesFile = arg("--notes", null)
const prerelease = arg("--prerelease", null)
const dropPrerelease = arg("--drop-prerelease", null)
if (prerelease !== null && !/^[a-z0-9_]+$/.test(prerelease)) {
  // Anything else could run into git's "-3-g..." or "-dirty" after it.
  console.error(`[release] ERROR: --prerelease ${prerelease}: lower case, digits and _ only`)
  process.exit(1)
}
if (notesFile && !fs.existsSync(notesFile)) {
  console.error(`[release] ERROR: --notes ${notesFile} does not exist`)
  process.exit(1)
}

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
  // Releases and pre-releases share the numbers of a day (fw-...27.2-pre.x
  // uses up 2), so the number is read from the front of each tag.
  const existing = git("tag", "-l", `fw-${day}.*`).split(/\r?\n/).filter(Boolean)
  const n = existing.map((t) => Number(/^fw-\d{4}\.\d{2}\.\d{2}\.(\d+)/.exec(t)?.[1])).filter(Number.isFinite)
  return `fw-${day}.${n.length ? Math.max(...n) + 1 : 1}`
}

// --- drop a pre-release ----------------------------------------------------
if (dropPrerelease) {
  const name = /-pre\.([a-z0-9_]+)$/.exec(dropPrerelease)?.[1]
  if (!name) fail(`${dropPrerelease} is not a pre-release tag (fw-YYYY.MM.DD.N-pre.<name>)`)
  const steps = [
    ["gh", ["release", "delete", dropPrerelease, "--repo", releaseRepo, "--cleanup-tag", "--yes"], REPO],
    ["git", ["push", "origin", `:refs/tags/${dropPrerelease}`], REPO],
    ["git", ["tag", "-d", dropPrerelease], REPO],
    ["git", ["push", "origin", "--delete", `pre/${name}`], designer],
    ["git", ["branch", "-D", `pre/${name}`], designer],
    ["git", ["tag", "-d", dropPrerelease], designer],
  ]
  for (const [cmd, args, cwd] of steps) {
    const r = run(cmd, args, { cwd })
    const why = (r.stderr || "").trim().split(/\r?\n/)[0]
    console.log(`[release] ${cmd} ${args.join(" ")}: ${r.status === 0 ? "done" : `nothing to do (${why})`}`)
  }
  process.exit(0)
}

// --- preflight -----------------------------------------------------------
const dirty = git("status", "--porcelain")
if (dirty && (publish || !allowDirty)) {
  fail(`the tree has uncommitted changes - a release is built from a commit:\n${dirty}`)
}
if (publish) {
  const gh = run("gh", ["auth", "status"])
  if (gh.status !== 0) fail("gh is not logged in - run gh auth login")
}
if (!fs.existsSync(path.join(designer, "package.json"))) fail(`no designer checkout at ${designer}`)

// The factory layout, from the designer - the same module the flasher page
// imports in the browser.
const layoutModule = path.join(designer, "lib", "factory-image.mjs")
if (!fs.existsSync(layoutModule)) fail(`${layoutModule} is missing - the factory layout lives in the designer repo`)
const { FACTORY_PARTS, FLASHER_URL, mergeFactoryImage, verifyFactoryImage } = require(layoutModule)

// boot_app0.bin is not a build artifact: it ships with the Arduino core and is
// the same for every board here. It holds ota_seq = 1, which points the
// bootloader at the first app slot - the slot the app is written to.
const pioCore = process.env.PLATFORMIO_CORE_DIR || path.join(os.homedir(), ".platformio")
const bootApp0 = path.join(pioCore, "packages", "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin")
if (!fs.existsSync(bootApp0)) fail(`boot_app0.bin not found at ${bootApp0}`)

// A dry run tags "-dryrun" onto the name. Its images announce that build, and
// the designer places no such build (lib/firmware-build.ts), so it counts as
// older than every release - otherwise a board flashed with a dry-run image
// would announce exactly the name the real release of the same day then
// gets, and the designer would call it up to date (2026-09-15, both test
// boards).
// A pre-release reuses only a tag of its own name on HEAD, a release only a
// release's.
const headTags = git("tag", "--points-at", "HEAD", "-l", "fw-*").split(/\r?\n/)
  .filter((t) => t && !t.endsWith("-dryrun") && (prerelease ? t.endsWith(`-pre.${prerelease}`) : !t.includes("-pre.")))
const fresh = prerelease ? `${nextTag()}-pre.${prerelease}` : nextTag()
const tag = headTags[0] || (publish ? fresh : `${fresh}-dryrun`)
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
    const markers = [...new Set(image.toString("latin1").match(/<<schaltli-image device=[a-z0-9-]+>>/g) || [])]
    if (markers.length !== 1 || markers[0] !== `<<schaltli-image device=${deviceId}>>`) {
      throw new Error(`${board.env} carries ${markers.join(", ") || "no marker"}, expected ${deviceId}'s`)
    }
    if (!image.includes(Buffer.from(`"${tag}"`)) && !image.includes(Buffer.from(tag))) {
      throw new Error(`${board.env}'s image does not contain its build ${tag}`)
    }

    const file = `${deviceId}-${tag}.bin`
    fs.writeFileSync(path.join(outDir, file), image)

    // The factory image: the same app, with what a blank chip is missing in
    // front of it. Its parts come from this board's build directory, except
    // boot_app0.bin, which comes with the core.
    const parts = {
      bootloader: fs.readFileSync(path.join(REPO, ".pio", "build", board.env, "bootloader.bin")),
      partitions: fs.readFileSync(path.join(REPO, ".pio", "build", board.env, "partitions.bin")),
      otaSelect: fs.readFileSync(bootApp0),
      app: image,
    }
    const factoryImage = Buffer.from(mergeFactoryImage(parts))
    const problems = verifyFactoryImage(factoryImage, { deviceId, app: image })
    if (problems.length) throw new Error(`${board.env}'s factory image is not sound:\n  ${problems.join("\n  ")}`)
    const factoryFile = `${deviceId}-factory-${tag}.bin`
    fs.writeFileSync(path.join(outDir, factoryFile), factoryImage)

    devices[deviceId] = {
      build: tag,
      file,
      size: image.length,
      sha256: crypto.createHash("sha256").update(image).digest("hex"),
      systemGeneration,
      url: `https://github.com/${releaseRepo}/releases/download/${tag}/${file}`,
      // Written at 0x0 over USB, never by OTA - OTA writes an app slot and can
      // do nothing with a merged image.
      factory: {
        file: factoryFile,
        size: factoryImage.length,
        sha256: crypto.createHash("sha256").update(factoryImage).digest("hex"),
        url: `https://github.com/${releaseRepo}/releases/download/${tag}/${factoryFile}`,
      },
    }
    console.log(`[release] ${file}: ${image.length} bytes, sha256 ${devices[deviceId].sha256}`)
    console.log(`[release] ${factoryFile}: ${factoryImage.length} bytes, sha256 ${devices[deviceId].factory.sha256}` +
      ` (${FACTORY_PARTS.map((p) => `${p.label} 0x${p.offset.toString(16)}`).join(", ")})`)
  }

  manifest = { release: tag, commit, repository: releaseRepo, devices }
  fs.writeFileSync(path.join(outDir, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n")

  // What changed since the previous release, as material for --notes - kept
  // in dist/, never published by itself: this repo is private and the release
  // is public, so what its history says in public is written by a person.
  const previous = git("tag", "-l", "fw-*", "--sort=-creatordate")
    .split(/\r?\n/)
    .filter((t) => t && t !== tag && !t.endsWith("-dryrun"))[0]
  const range = previous ? `${previous}..${commit}` : commit
  const changes = git("log", "--no-merges", "--format=- %s (%h)", range)
  fs.writeFileSync(path.join(outDir, "CHANGES.txt"),
    `Commits in ${tag}${previous ? ` since ${previous}` : " (first release: the whole history)"}:\n\n${changes}\n`)
  console.log(`\n[release] images, manifest and CHANGES.txt in ${outDir}`)
} catch (e) {
  removeTag()
  fail(e.message)
}

if (!publish) {
  removeTag()
  console.log(`[release] dry run complete. --publish would push ${tag.replace(/-dryrun$/, "")}, create the release in ${releaseRepo}` +
    ` with ${Object.keys(manifest.devices).length} OTA images, ${Object.keys(manifest.devices).length} factory images and the manifest,` +
    (prerelease ? ` as a pre-release, and put its manifest on the designer branch pre/${prerelease}.`
      : ` and write firmware/manifest.json into ${designer}.`))
  process.exit(0)
}

// --- publish ---------------------------------------------------------------
const outDir = path.join(REPO, "dist", tag)
console.log(`\n[release] pushing ${tag}`)
const push = run("git", ["push", "origin", tag], { stdio: "inherit" })
if (push.status !== 0) fail(`git push origin ${tag} failed`)

const notes = [
  `Firmware ${tag}, built from schaltli-firmware ${commit.slice(0, 10)}.`,
  "",
  // Hand-written, from --notes: what a user of the devices should know. The
  // commit list the dry run leaves in dist/<tag>/CHANGES.txt is the material.
  ...(notesFile ? [fs.readFileSync(notesFile, "utf8").trim(), ""] : []),
  ...Object.entries(manifest.devices).map(([id, d]) => `- ${id}: ${d.file}, ${d.size} bytes, sha256 ${d.sha256}`),
  "",
  "Installed by the designer: deploy/pekaway-install.sh downloads the images firmware/manifest.json names.",
  "",
  `A brand-new board has no Schaltli to update, so it is flashed over USB instead: ${FLASHER_URL}`,
  "(pick the board, pick this version, connect the cable - the factory images below are what that page writes,",
  "one file at 0x0; nothing else needs installing.)",
  "",
  ...Object.entries(manifest.devices).map(([id, d]) => `- ${id}: ${d.factory.file}, ${d.factory.size} bytes, sha256 ${d.factory.sha256}`),
].join("\n")
// manifest.json goes along as an asset, not only into the designer checkout:
// the flasher page offers the last three releases, and this is where it reads
// what each one holds.
const assets = [
  ...Object.values(manifest.devices).flatMap((d) => [path.join(outDir, d.file), path.join(outDir, d.factory.file)]),
  path.join(outDir, "manifest.json"),
]
if (prerelease) {
  publishPrerelease()
  process.exit(0)
}
const release = run("gh", ["release", "create", tag, "--repo", releaseRepo, "--title", `Firmware ${tag}`, "--notes", notes, ...assets],
  { stdio: "inherit" })
if (release.status !== 0) fail(`gh release create failed - the tag is pushed; create the release by hand or re-run`)

const target = path.join(designer, "firmware", "manifest.json")
fs.mkdirSync(path.dirname(target), { recursive: true })
fs.writeFileSync(target, JSON.stringify(manifest, null, 2) + "\n")
console.log(`\n[release] ${tag} published. Wrote ${target} - commit and push it in the designer repo.`)

// The designer side of a pre-release: branch pre/<name> from the designer's
// HEAD, in a worktree of its own, with the manifest committed on it - the
// release is then created on that branch, so its tag in the designer repo is
// the commit that ships this firmware.
function publishPrerelease() {
  const branch = `pre/${prerelease}`
  const dg = (...args) => {
    const r = run("git", args, { cwd: designer })
    if (r.status !== 0) fail(`git ${args.join(" ")} in the designer failed:\n${r.stderr}`)
    return r.stdout.trim()
  }
  if (dg("status", "--porcelain", "--untracked-files=no")) {
    console.log("[release] note: the designer has uncommitted changes - they are not part of the pre-release")
  }
  const tree = path.join(os.tmpdir(), `schaltli-designer-${prerelease}-${Date.now()}`)
  dg("worktree", "add", "-B", branch, tree, "HEAD")
  try {
    const target = path.join(tree, "firmware", "manifest.json")
    fs.mkdirSync(path.dirname(target), { recursive: true })
    fs.writeFileSync(target, JSON.stringify(manifest, null, 2) + "\n")
    const wt = (...args) => {
      const r = run("git", args, { cwd: tree })
      if (r.status !== 0) fail(`git ${args.join(" ")} in the pre-release worktree failed:\n${r.stderr}`)
      return r.stdout.trim()
    }
    wt("add", "firmware/manifest.json")
    wt("commit", "-m", `Ship pre-release ${tag}`)
    console.log(`\n[release] pushing designer branch ${branch}`)
    const push = run("git", ["push", "--force", "origin", branch], { cwd: tree, stdio: "inherit" })
    if (push.status !== 0) fail(`git push origin ${branch} failed - the firmware tag is pushed; push the branch by hand or drop the pre-release`)
  } finally {
    run("git", ["worktree", "remove", "--force", tree], { cwd: designer })
  }
  const release = run("gh", ["release", "create", tag, "--repo", releaseRepo, "--title", `Pre-release ${tag}`,
    "--prerelease", "--latest=false", "--target", branch, "--notes", notes, ...assets], { stdio: "inherit" })
  if (release.status !== 0) fail("gh release create failed - tag and branch are pushed; create the release by hand or drop the pre-release")
  console.log(`\n[release] ${tag} published as a pre-release on ${releaseRepo}, designer branch ${branch}.`)
  console.log(`[release] to install it on a Pekaway: curl -fsSL https://raw.githubusercontent.com/${releaseRepo}/main/deploy/pekaway-install.sh | bash -s -- --ref ${tag}`)
  console.log(`[release] to remove it again: node tools/release-firmware.js --drop-prerelease ${tag}`)
}
