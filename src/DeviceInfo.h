#pragma once

// This device's own identity, compiled in. Ported from
// screenbee-m5dial@d73d0ff on 2026-08-19.
//
// DEVICE_ID must match ddf-source/device.json's "device.id" exactly - it is
// how a device tells a project "you weren't built for me" before applying
// it, and how it identifies its own type in its MQTT "hello". A different
// device id is an unconditional hard block, never a version relationship.
#define DEVICE_ID "waveshare-knob-1v8"

// Freeform, only used in the MQTT "hello" payload for humans/tooling to
// see what's running - not parsed or compared against anything.
#define FIRMWARE_VERSION "0.1.0"

// Must match ddf-source/device.json's own "ddfVersion" exactly;
// tools/generate-ddf-header.js keeps them in sync and its --check mode
// fails on drift. Announced in "hello" alongside a GET /ddf.zip URL
// (TestInterfaceServer::handleDdfZip(), serving ddf_zip.h) so the
// designer's device-scan-section.tsx can fetch and cache this device's DDF
// straight from a live device.
//
// Deliberately still the old ddfVersion+url contract rather than the DDF
// content hash the designer is moving to: the designer's auto-discovery
// does not understand a hash yet, and no device should run ahead of a
// designer feature that doesn't exist - that is the exact drift class the
// version rework exists to kill. The hash lands here later, across all
// devices at once. (Decision 3 of the 2026-08-19 port grilling; designer
// repo's docs/version-model-simplification-plan.md.)
#define DDF_VERSION "1.1"

// The system generation this firmware understands - the single version
// number shared by the designer's project file, its device export, and the
// DDF (designer repo's lib/system-generation.ts and
// docs/nested-provenance.md's "Version compatibility"). Replaces the M5
// Dial's EXPORT_SCHEMA_VERSION, which was one of three separately-bumped
// integers.
//
// Only MAJOR gates anything: DeployManager rejects a deploy whose own major
// is higher than this, before touching /PROJECT. A newer *minor* is
// additive by definition and must be accepted - unknown object types are
// skipped and logged, missing fields default - so a device on 1.0 runs a
// project built at 1.7 correctly. An artifact with no systemGeneration
// field at all is treated as 1.0, the implicit generation everything
// written before the field existed carries.
//
// This board implements the correct peek from day one, unlike the M5 Dial,
// which still reads the retired `schemaVersion` field.
#define SYSTEM_GENERATION_MAJOR 1
#define SYSTEM_GENERATION_MINOR 0

// Where DeployManager.cpp promotes a verified deploy download to, and
// TestInterfaceServer.cpp's recovery endpoint (GET /recovery-project) reads
// back from - shared here specifically so both files agree on it. See the
// designer repo's docs/nested-provenance.md, Fall 3.
#define RECOVERY_PROJECT_PATH "/recovery_project.zip"

// The MQTT topic namespace every device/browser participant in the deploy
// flow shares (screenbee/<clientId>/...) - the same shared protocol as
// every other ScreenBee device, not device-specific.
#define TOPIC_PREFIX "screenbee"
