#!/usr/bin/env node
// Regenerates src/ddf_zip.h from this repo's own ddf-source/ (the DDF's
// real editable source - device.json + adornment.svg + fonts/*.bdf), and
// keeps DeviceInfo.h's DDF_HASH in sync with the bytes it just wrote.
//
// Ported from screenbee-m5dial@d73d0ff on 2026-08-19, unchanged apart from
// this comment block and the flash command below. The designer carries no
// baked-in knowledge of this device: ddf-source/ here is the only source of
// truth, and this script builds the zip rather than importing one.
//
// Why it is checked in rather than being "a one-off script": on the M5 Dial
// it wasn't, and that is precisely why designer and firmware drifted - the
// designer's DDF went 1.4 -> 1.5 -> 1.6 while the firmware kept announcing
// and serving 1.4, so anyone picking that device from "Announced Devices"
// got a DDF missing object types the firmware had actually gained.
//
// Run after any change to ddf-source/, then rebuild and flash - the
// header is compiled in, so a rebuild is what actually publishes it:
//
//   node tools/generate-ddf-header.js
//   pio run -e waveshare-knob-touch-lcd-1v8 -t upload
//
// Pass --check to verify without writing (exit 1 on drift). Worth wiring
// into CI or a pre-commit hook: the output is byte-deterministic by
// construction (fixed DOS timestamps, entries sorted by name), so --check
// only ever fires on a real change - never on wall-clock drift.

const fs = require("fs")
const path = require("path")
const zlib = require("zlib")
const crypto = require("crypto")

const REPO = path.join(__dirname, "..")
const DEFAULT_SOURCE = path.join(REPO, "ddf-source")
const checkOnly = process.argv.includes("--check")
const sourceDir = process.argv.slice(2).find((a) => !a.startsWith("--")) || DEFAULT_SOURCE

// Output paths follow the source directory instead of being fixed, because
// this repo now builds two boards out of one src/ and they cannot share a
// generated header. Running the generator on the 4.3B's source with the old
// hardcoded paths silently overwrote the knob's ddf_zip.h and DeviceInfo.h.
// A firmware would then have served another board's DDF - with a correct
// hash for it, so nothing downstream could tell - which is precisely the
// class of silent staleness the pre-build hook exists to prevent.
//
// "ddf-source" still writes to src/, so the knob is untouched and every
// existing invocation keeps its meaning. Any other source directory writes
// next to its own board's code.
function outputsFor(dir) {
  const name = path.basename(path.resolve(dir))
  if (name === "ddf-source") {
    return {
      header: path.join(REPO, "src", "ddf_zip.h"),
      deviceInfo: path.join(REPO, "src", "DeviceInfo.h"),
    }
  }
  const boardDir = path.join(REPO, "src", "boards", "waveshare" + name.replace(/^ddf-source-/, ""))
  if (!fs.existsSync(boardDir)) {
    console.error(`no board directory for DDF source "${name}": expected ${boardDir}`)
    process.exit(1)
  }
  return { header: path.join(boardDir, "ddf_zip.h"), deviceInfo: path.join(boardDir, "DeviceInfo.h") }
}

const { header: HEADER, deviceInfo: DEVICE_INFO } = outputsFor(sourceDir)

if (!fs.existsSync(sourceDir) || !fs.existsSync(path.join(sourceDir, "device.json"))) {
  console.error(`DDF source not found: ${sourceDir} (expected a device.json in it)`)
  process.exit(1)
}

// --- Minimal in-memory ZIP writer (Node built-ins only - this repo has no
// package.json/node_modules, and the old version of this script already
// hand-rolled zip *reading* the same way, so a hand-rolled *writer* stays
// consistent with that rather than introducing a dependency for one script). ---

// Standard ZIP/PKZIP CRC-32 (same polynomial as gzip/PNG).
const CRC_TABLE = (() => {
  const table = new Uint32Array(256)
  for (let n = 0; n < 256; n++) {
    let c = n
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1
    table[n] = c >>> 0
  }
  return table
})()

function crc32(buf) {
  let c = 0xffffffff
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8)
  return (c ^ 0xffffffff) >>> 0
}

// Fixed DOS date/time (1980-01-01 00:00:00, the format's own epoch) for
// every entry instead of the real mtime - regenerating from unchanged
// source must produce byte-identical output, or --check would spuriously
// report "stale" on every run just from wall-clock drift.
const DOS_TIME = 0
const DOS_DATE = (1 << 5) | 1 // day=1, month=1, year offset=0 -> 1980-01-01

function buildZip(entries) {
  // entries: [{ name, data: Buffer }], already in the exact order to write.
  const localParts = []
  const centralParts = []
  let offset = 0

  for (const { name, data } of entries) {
    const nameBuf = Buffer.from(name, "utf8")
    const crc = crc32(data)
    const compressed = zlib.deflateRawSync(data)
    const useStore = compressed.length >= data.length
    const method = useStore ? 0 : 8
    const payload = useStore ? data : compressed

    const localHeader = Buffer.alloc(30)
    localHeader.writeUInt32LE(0x04034b50, 0)
    localHeader.writeUInt16LE(20, 4) // version needed
    localHeader.writeUInt16LE(0, 6) // flags
    localHeader.writeUInt16LE(method, 8)
    localHeader.writeUInt16LE(DOS_TIME, 10)
    localHeader.writeUInt16LE(DOS_DATE, 12)
    localHeader.writeUInt32LE(crc, 14)
    localHeader.writeUInt32LE(payload.length, 18)
    localHeader.writeUInt32LE(data.length, 22)
    localHeader.writeUInt16LE(nameBuf.length, 26)
    localHeader.writeUInt16LE(0, 28) // extra field length
    localParts.push(localHeader, nameBuf, payload)

    const centralHeader = Buffer.alloc(46)
    centralHeader.writeUInt32LE(0x02014b50, 0)
    centralHeader.writeUInt16LE(20, 4) // version made by
    centralHeader.writeUInt16LE(20, 6) // version needed
    centralHeader.writeUInt16LE(0, 8) // flags
    centralHeader.writeUInt16LE(method, 10)
    centralHeader.writeUInt16LE(DOS_TIME, 12)
    centralHeader.writeUInt16LE(DOS_DATE, 14)
    centralHeader.writeUInt32LE(crc, 16)
    centralHeader.writeUInt32LE(payload.length, 20)
    centralHeader.writeUInt32LE(data.length, 24)
    centralHeader.writeUInt16LE(nameBuf.length, 28)
    centralHeader.writeUInt16LE(0, 30) // extra field length
    centralHeader.writeUInt16LE(0, 32) // comment length
    centralHeader.writeUInt16LE(0, 34) // disk number start
    centralHeader.writeUInt16LE(0, 36) // internal attrs
    centralHeader.writeUInt32LE(0, 38) // external attrs
    centralHeader.writeUInt32LE(offset, 42) // local header offset
    centralParts.push(centralHeader, nameBuf)

    offset += localHeader.length + nameBuf.length + payload.length
  }

  const centralDirStart = offset
  const centralDir = Buffer.concat(centralParts)

  const eocd = Buffer.alloc(22)
  eocd.writeUInt32LE(0x06054b50, 0)
  eocd.writeUInt16LE(0, 4) // disk number
  eocd.writeUInt16LE(0, 6) // disk with central dir
  eocd.writeUInt16LE(entries.length, 8)
  eocd.writeUInt16LE(entries.length, 10)
  eocd.writeUInt32LE(centralDir.length, 12)
  eocd.writeUInt32LE(centralDirStart, 16)
  eocd.writeUInt16LE(0, 20) // comment length

  return Buffer.concat([...localParts, centralDir, eocd])
}

// Walk sourceDir recursively, collecting { name, data } in a fixed,
// deterministic order (sorted by zip-entry name) - required for byte-stable
// output, same reasoning as the fixed timestamp above.
function collectEntries(dir, prefix = "") {
  const out = []
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name)
    const zipName = prefix + entry.name
    if (entry.isDirectory()) {
      out.push(...collectEntries(full, zipName + "/"))
    } else {
      out.push({ name: zipName, data: fs.readFileSync(full) })
    }
  }
  out.sort((a, b) => (a.name < b.name ? -1 : a.name > b.name ? 1 : 0))
  return out
}

const entries = collectEntries(sourceDir)
const version = JSON.parse(fs.readFileSync(path.join(sourceDir, "device.json"), "utf8")).ddfVersion
const zip = buildZip(entries)

// The DDF's identity, and the only thing the designer compares: sha256 of
// the served bytes, first 16 hex characters, lowercase (designer repo's
// lib/ddf-name.ts, docs/device-contract.md §4). Computed here rather than on
// the device precisely because here it hashes the bytes it is about to
// write, so it cannot drift from them - and the designer *verifies* it
// against what it downloads, refusing the fetch on a mismatch. A hash that
// drifts does not degrade discovery, it ends it.
//
// Truncation is deliberate and matches the designer: this identifies a
// build, it is not a security boundary.
const ddfHash = crypto.createHash("sha256").update(zip).digest("hex").slice(0, 16)

const lines = []
for (let i = 0; i < zip.length; i += 16) {
  const row = Array.from(zip.subarray(i, i + 16))
    .map((b) => `0x${b.toString(16).padStart(2, "0")}`)
    .join(", ")
  lines.push(`  ${row},`)
}

const header = `#pragma once
// Built from this repo's own ddf-source/ (device.json + adornment.svg +
// fonts/*.bdf) - GENERATED by tools/generate-ddf-header.js, do not
// hand-edit ddf_zip.h itself, edit ddf-source/ and regenerate. Served at
// GET /ddf.zip (TestInterfaceServer) so the designer's
// device-scan-section.tsx can fetch+cache this device's own DDF directly
// from a live device, the same way the e-paper firmware already does.
//
// Regenerate whenever ddf-source/ changes - the generator also updates
// DeviceInfo.h's DDF_HASH, which is what tells the designer a cached copy
// is stale and needs refetching.
//
// DDF version embedded here: ${version}, content hash ${ddfHash}
static const uint8_t DDF_ZIP[${zip.length}] = {
${lines.join("\n")}
};
`

const existingHeader = fs.existsSync(HEADER) ? fs.readFileSync(HEADER, "utf8") : ""
const deviceInfo = fs.readFileSync(DEVICE_INFO, "utf8")
const hashPattern = /#define DDF_HASH "([^"]*)"/
const currentHash = (deviceInfo.match(hashPattern) || [])[1]

if (checkOnly) {
  const headerStale = existingHeader !== header
  const hashStale = currentHash !== ddfHash
  if (headerStale || hashStale) {
    if (headerStale) console.error(`src/ddf_zip.h is stale (DDF is ${zip.length} bytes, version ${version})`)
    if (hashStale) console.error(`DeviceInfo.h DDF_HASH is "${currentHash}", the DDF bytes hash to "${ddfHash}"`)
    console.error("Run: node tools/generate-ddf-header.js")
    process.exit(1)
  }
  console.log(`Up to date (DDF ${ddfHash}, version ${version}, ${zip.length} bytes)`)
  process.exit(0)
}

fs.writeFileSync(HEADER, header)
if (currentHash !== ddfHash) {
  fs.writeFileSync(DEVICE_INFO, deviceInfo.replace(hashPattern, `#define DDF_HASH "${ddfHash}"`))
  console.log(`DDF_HASH ${currentHash} -> ${ddfHash}`)
}
console.log(`Wrote ${path.relative(REPO, HEADER)} (${zip.length} bytes, ${ddfHash}) from ${path.relative(REPO, sourceDir)}`)

// --zip <path> also drops the raw .ddf.zip somewhere, which is how a board
// that cannot yet serve its own DDF over HTTP still gets one into the
// designer: the same bytes that are compiled in, written out as a file for
// public/ddf/. Precedent is the Android target, whose DDF lives in its own
// repo and whose zip is checked into the designer with a test asserting the
// two match. Deliberately the same bytes rather than a second build, so the
// checked-in copy cannot drift from the compiled one.
const zipArgIndex = process.argv.indexOf("--zip")
if (zipArgIndex !== -1 && process.argv[zipArgIndex + 1]) {
  const zipPath = process.argv[zipArgIndex + 1]
  fs.writeFileSync(zipPath, zip)
  console.log(`Wrote ${zipPath} (${zip.length} bytes, ${ddfHash})`)
}
