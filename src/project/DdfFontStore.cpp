#include "DdfFontStore.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// By name, not by relative path: this file is shared by every board, and
// the header it wants is the one the current build's include path
// provides. "../ddf_zip.h" pointed at whichever board happened to keep
// its copy in src/, which is how the 4.3B came to serve the knob's DDF.
#include "ddf_zip.h"
#include "ProjectInstaller.h"

namespace {

// Reads one entry out of the compiled-in DDF zip.
//
// Delegated to ProjectInstaller rather than calling miniz here. Doing it
// here directly is what boot-looped this device on 2026-08-22: DEFLATE
// extraction on this board needs a padded static dictionary buffer,
// because tinfl_decompress overruns its 32KB window by 100-270 bytes and
// corrupts whatever follows. That was found and fixed once already, in
// 2026-08-09's investigation; a second call site with plain allocators
// simply re-broke it, and the crash looked nothing like its cause - a
// garbage pointer inside miniz, four frames from anything of ours.
uint8_t* extractFromDdf(const char* name, size_t* outSize) {
  uint8_t* data = ProjectInstaller::extractEntryFromMemoryZip(DDF_ZIP, sizeof(DDF_ZIP), name, outSize);
  if (data != nullptr) {
    Serial.printf("[ddf-fonts] extracted %s (%u bytes)\n", name, (unsigned)*outSize);
  }
  return data;
}

}  // namespace

bool DdfFontStore::ensureIndex() {
  if (indexed_) return !fileByFontId_.empty();
  indexed_ = true;

  size_t size = 0;
  uint8_t* json = extractFromDdf("device.json", &size);
  if (json == nullptr) return false;

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, (const char*)json, size);
  free(json);
  if (err) {
    Serial.printf("[ddf-fonts] device.json in the DDF did not parse: %s\n", err.c_str());
    return false;
  }

  for (JsonObject font : doc["fonts"].as<JsonArray>()) {
    const String id = font["id"] | "";
    const String file = font["file"] | "";
    if (id.isEmpty() || file.isEmpty()) continue;
    fileByFontId_[id] = file;
    if (defaultFontId_.isEmpty()) defaultFontId_ = id;
  }
  Serial.printf("[ddf-fonts] DDF declares %u font(s)\n", (unsigned)fileByFontId_.size());
  return !fileByFontId_.empty();
}

String DdfFontStore::defaultFontId() {
  ensureIndex();
  return defaultFontId_;
}

const BdfFont* DdfFontStore::get(const String& fontId) {
  if (!ensureIndex()) return nullptr;

  const auto cached = cache_.find(fontId);
  if (cached != cache_.end()) return cached->second;

  const auto entry = fileByFontId_.find(fontId);
  if (entry == fileByFontId_.end()) {
    // Cached as a miss too, so a project referencing a font this device
    // does not publish costs one lookup rather than one per redraw.
    cache_[fontId] = nullptr;
    Serial.printf("[ddf-fonts] project wants font \"%s\", which this DDF does not declare\n", fontId.c_str());
    return nullptr;
  }

  size_t size = 0;
  uint8_t* text = extractFromDdf(entry->second.c_str(), &size);
  if (text == nullptr) {
    cache_[fontId] = nullptr;
    return nullptr;
  }

  BdfFont* font = new BdfFont();
  const bool ok = font->parse((const char*)text, size);
  free(text);
  if (!ok) {
    Serial.printf("[ddf-fonts] %s did not parse\n", entry->second.c_str());
    delete font;
    cache_[fontId] = nullptr;
    return nullptr;
  }

  Serial.printf("[ddf-fonts] %s: %u glyphs, ascent %d, descent %d\n", entry->second.c_str(),
                (unsigned)font->glyphCount(), font->fontAscent(), font->fontDescent());
  cache_[fontId] = font;
  return font;
}
