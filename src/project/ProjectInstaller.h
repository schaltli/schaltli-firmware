#pragma once
#include <Arduino.h>

// Shared "take a project zip already sitting in a file on LittleFS and turn
// it into the live /PROJECT directory" logic - ported ~verbatim from
// schaltli-eink's ProjectInstaller (same miniz-based extraction, same
// /PROJECT layout convention) so TestInterfaceServer's project-upload
// endpoint doesn't have to reinvent it. Kept as its own namespace (not
// folded into TestInterfaceServer) in case a future MQTT self-deploy path
// for this device needs it too, same reason it's separate on the e-paper
// side.
namespace ProjectInstaller {

// Extracts one entry from a zip that already sits in memory - the embedded
// DDF, in practice - into a freshly allocated buffer the caller owns
// (heap_caps_malloc, PSRAM, one byte longer than the entry and
// NUL-terminated so text can be used as a C string). Returns nullptr and
// leaves *outSize at 0 on any failure.
//
// It lives here, rather than wherever it happens to be needed, because
// DEFLATE extraction on this device is not a matter of calling miniz: it
// needs the padded static dictionary buffer and the patched allocator
// documented at the top of the .cpp, both of which exist because
// tinfl_decompress overruns its dictionary window by 100-270 bytes and one
// upstream miniz path frees a non-heap pointer. Reimplementing "just read a
// file out of a zip" without those cost a boot loop on 2026-08-22 - the
// font loader called mz_zip_reader_extract_to_mem() with plain allocators
// and the overrun corrupted whatever PSRAM followed the dictionary.
uint8_t* extractEntryFromMemoryZip(const uint8_t* zipData, size_t zipSize, const char* entryName,
                                   size_t* outSize);

// One entry of an in-memory zip read in pieces, never held whole - with the
// same padded dictionary and allocator as extractEntryFromMemoryZip(), and
// one entry at a time like it (the dictionary is a single static buffer).
//
// For the 4-bit fonts (designer docs/2026-11-01-4bit-fonts.md): a Display
// face's BDF is over a megabyte of text, and extracting it whole needed that
// much contiguous PSRAM on top of the parsed font. On the 4.3B with three
// typographies loaded it was not there - «no room for fonts/inter-c42.bdf»,
// and Humanist's Display drew in the fallback font (2026-11-02).
class MemoryZipEntryReader {
public:
  ~MemoryZipEntryReader();
  // False when the zip cannot be opened or holds no such entry.
  bool open(const uint8_t* zipData, size_t zipSize, const char* entryName);
  // Up to n bytes into buf; 0 at the end, or after a failure.
  size_t read(uint8_t* buf, size_t n);
  // Back to the entry's first byte, for a second pass.
  bool rewind();
  // Whether everything read so far is the whole entry with its CRC - ask at
  // the end of a pass.
  bool intact() const;
  void close();

private:
  void* zip_ = nullptr;    // mz_zip_archive, opaque so miniz stays out of this header
  void* state_ = nullptr;  // mz_zip_reader_extract_iter_state
  uint32_t index_ = 0;
  uint32_t crc_ = 0;
  uint32_t expectedCrc_ = 0;
  size_t done_ = 0;
  size_t size_ = 0;
};

// Reads just project.json out of the zip at zipPath (without touching
// /PROJECT at all). Returns false when it cannot - no zip, no project.json,
// a bad CRC, not JSON - and the caller must refuse the project:
// until 2026-11-01 that case and "the project names no device" were the same
// empty string, which the gates let through, and the 4.3B under load
// installed a project for another device. Returns true with deviceIdOut set
// to the top-level "deviceId", empty when the project has none (an older
// export, which may install). See DEVICE_ID in DeviceInfo.h for what it gets
// compared against.
bool readProjectDeviceId(const String& zipPath, String& deviceIdOut);

// The deviceId, or "" for "none" and "could not read" alike. Kept for
// callers that only log it; a gate uses readProjectDeviceId().
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
