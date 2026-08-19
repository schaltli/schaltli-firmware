#pragma once
#include <Arduino.h>

// Shared "take a project zip already sitting in a file on LittleFS and turn
// it into the live /PROJECT directory" logic - ported ~verbatim from
// MqttEPaperDisplay2's ProjectInstaller (same miniz-based extraction, same
// /PROJECT layout convention) so TestInterfaceServer's project-upload
// endpoint doesn't have to reinvent it. Kept as its own namespace (not
// folded into TestInterfaceServer) in case a future MQTT self-deploy path
// for this device needs it too, same reason it's separate on the e-paper
// side.
namespace ProjectInstaller {

// Reads just project.json out of the zip at zipPath (without touching
// /PROJECT at all) and returns its top-level "deviceId" field, or an empty
// string if the zip/entry/field is missing or unparsable. Callers should
// treat an empty result as "unknown, don't trust it" rather than
// "compatible" - see DEVICE_ID in DeviceInfo.h for what this gets compared
// against.
String peekProjectDeviceId(const String& zipPath);

// Same idea as peekProjectDeviceId() above, but for the export's system
// generation rather than a device's identity - the single version number
// shared by the designer's project file, its device export and the DDF
// (designer repo's lib/system-generation.ts). Returns the MAJOR component
// only; compare against SYSTEM_GENERATION_MAJOR in DeviceInfo.h. Returns -1
// if the zip/entry/field couldn't be read at all (treat as "unknown, don't
// trust it", same convention as peekProjectDeviceId()'s empty string), and
// 1 when the field is simply absent. See the implementation's own comment
// for why only the major is returned.
long peekProjectSystemGenerationMajor(const String& zipPath);

// Wipes and recreates /PROJECT, extracts every entry from the zip at
// zipPath into it, and confirms project.json ended up there. Returns false
// (with errorOut set) on any failure. Not rollback-safe on its own -
// /PROJECT is already wiped by the time this can fail on extraction.
// Callers are expected to have already validated the zip (checksum,
// peekProjectDeviceId()) before calling this, so a failure here is
// expected to be rare (e.g. genuine flash trouble), not the normal way a
// bad upload gets rejected.
bool installProjectZipFromFile(const String& zipPath, String& errorOut);

}
