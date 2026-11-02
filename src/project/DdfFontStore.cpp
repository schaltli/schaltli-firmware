#include "DdfFontStore.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Through DdfBytes, the one file that includes the board's ddf_zip.h (by
// name, not by relative path: "../ddf_zip.h" pointed at whichever board
// happened to keep its copy in src/, which is how the 4.3B came to serve
// the knob's DDF).
#include "../DdfBytes.h"
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
  uint8_t* data = ProjectInstaller::extractEntryFromMemoryZip(ddfZipBytes(), ddfZipSize(), name, outSize);
  if (data != nullptr) {
    Serial.printf("[ddf-fonts] extracted %s (%u bytes)\n", name, (unsigned)*outSize);
  }
  return data;
}

// A font's BDF read out of the DDF zip in pieces, line by line (BdfFont's
// BdfLineSource): a 4 KB window instead of the whole file, which for a 4-bit
// Display face is over a megabyte (ProjectInstaller.h, MemoryZipEntryReader).
class ZipLineSource : public BdfLineSource {
public:
  bool open(const char* name) {
    buf_ = (char*)heap_caps_malloc(kSize, MALLOC_CAP_SPIRAM);
    if (buf_ == nullptr) return false;
    if (!entry_.open(ddfZipBytes(), ddfZipSize(), name)) return false;
    start_ = end_ = 0;
    eof_ = false;
    return true;
  }
  ~ZipLineSource() override {
    entry_.close();
    if (buf_ != nullptr) free(buf_);
  }
  bool rewind() override {
    if (!eof_ || !entry_.intact()) {
      // A pass that ended before the entry did, or on a bad CRC.
      failed_ = true;
      return false;
    }
    start_ = end_ = 0;
    eof_ = false;
    return entry_.rewind();
  }
  bool next(const char*& line, size_t& length) override {
    for (;;) {
      // Skip line breaks left from the last line.
      while (start_ < end_ && (buf_[start_] == '\n' || buf_[start_] == '\r')) start_++;
      size_t nl = start_;
      while (nl < end_ && buf_[nl] != '\n' && buf_[nl] != '\r') nl++;
      if (nl < end_ || (eof_ && start_ < end_)) {
        line = buf_ + start_;
        length = nl - start_;
        start_ = nl;
        while (length > 0 && (line[length - 1] == ' ' || line[length - 1] == '\t')) length--;
        return true;
      }
      if (eof_) return false;
      // Keep the partial line, fill the rest.
      memmove(buf_, buf_ + start_, end_ - start_);
      end_ -= start_;
      start_ = 0;
      if (end_ == kSize) {
        failed_ = true;  // a line longer than the window: no BDF has one
        return false;
      }
      const size_t got = entry_.read((uint8_t*)buf_ + end_, kSize - end_);
      if (got == 0) eof_ = true;
      end_ += got;
    }
  }
  bool ok() const { return !failed_ && eof_ && entry_.intact(); }

private:
  static constexpr size_t kSize = 4096;
  ProjectInstaller::MemoryZipEntryReader entry_;
  char* buf_ = nullptr;
  size_t start_ = 0;
  size_t end_ = 0;
  bool eof_ = false;
  bool failed_ = false;
};

}  // namespace

bool DdfFontStore::ensureIndex() {
  if (indexed_) return !index_.fileById.empty();
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

  fontindex::read(doc["fonts"].as<JsonArrayConst>(), index_);
  Serial.printf("[ddf-fonts] DDF declares %u font(s), %u replaced id(s)\n", (unsigned)index_.fileById.size(),
                (unsigned)index_.replacedBy.size());
  return !index_.fileById.empty();
}

String DdfFontStore::defaultFontId() {
  ensureIndex();
  return index_.firstId;
}

String DdfFontStore::drawnId(const String& fontId) {
  if (!ensureIndex()) return String();
  return fontindex::resolve(index_, fontId);
}

size_t DdfFontStore::reserveBytes_ = 1024 * 1024;

size_t DdfFontStore::freePsram() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

size_t DdfFontStore::parsedCount() const {
  std::vector<const BdfFont*> seen;
  for (const auto& entry : cache_) {
    if (entry.second == nullptr) continue;
    bool known = false;
    for (const BdfFont* f : seen) known = known || f == entry.second;
    if (!known) seen.push_back(entry.second);
  }
  return seen.size();
}

size_t DdfFontStore::trim(const std::vector<String>& keep) {
  if (!indexed_) return 0;
  // What must stay, as the ids the fonts are parsed under.
  std::vector<String> stay;
  for (const String& id : keep) {
    const String drawn = fontindex::resolve(index_, id);
    if (!drawn.isEmpty()) stay.push_back(drawn);
  }
  stay.push_back(index_.firstId);
  size_t freed = 0;
  while (freePsram() < reserveBytes_) {
    String oldest;
    uint32_t oldestUse = UINT32_MAX;
    for (const auto& entry : cache_) {
      if (entry.second == nullptr || fontindex::resolve(index_, entry.first) != entry.first) continue;
      bool kept = false;
      for (const String& id : stay) kept = kept || id == entry.first;
      if (kept) continue;
      const auto use = lastUse_.find(entry.first);
      const uint32_t when = use == lastUse_.end() ? 0 : use->second;
      if (when < oldestUse) {
        oldestUse = when;
        oldest = entry.first;
      }
    }
    if (oldest.isEmpty()) break;
    BdfFont* font = cache_[oldest];
    // Every id cached as this font - its own and the replaced ones it
    // stands in for - goes with it, to be loaded again when asked.
    for (auto it = cache_.begin(); it != cache_.end();) {
      if (it->second == font) {
        lastUse_.erase(it->first);
        it = cache_.erase(it);
      } else {
        ++it;
      }
    }
    delete font;
    freed++;
    letGo_++;
    Serial.printf("[ddf-fonts] let go of %s, %u KB PSRAM free\n", oldest.c_str(), (unsigned)(freePsram() / 1024));
  }
  return freed;
}

const BdfFont* DdfFontStore::get(const String& fontId) {
  if (!ensureIndex()) return nullptr;

  const auto cached = cache_.find(fontId);
  if (cached != cache_.end()) {
    if (cached->second != nullptr) lastUse_[fontId] = ++uses_;
    return cached->second;
  }

  // A replaced id the DDF dropped: the font standing in for it, cached
  // under both ids (FontIndex.h).
  const String drawn = fontindex::resolve(index_, fontId);
  if (!drawn.isEmpty() && drawn != fontId) {
    const BdfFont* stand = get(drawn);
    cache_[fontId] = const_cast<BdfFont*>(stand);
    Serial.printf("[ddf-fonts] project wants font \"%s\", drawn with \"%s\", which replaces it\n", fontId.c_str(),
                  drawn.c_str());
    return stand;
  }

  const auto entry = index_.fileById.find(fontId);
  if (entry == index_.fileById.end()) {
    // Cached as a miss too, so a project referencing a font this device
    // does not publish costs one lookup rather than one per redraw.
    cache_[fontId] = nullptr;
    Serial.printf("[ddf-fonts] project wants font \"%s\", which this DDF does not declare\n", fontId.c_str());
    return nullptr;
  }

  // Read in pieces, never whole (ZipLineSource): a 4-bit Display face is
  // over a megabyte of text.
  ZipLineSource source;
  if (!source.open(entry->second.c_str())) {
    Serial.printf("[ddf-fonts] could not open %s in the DDF\n", entry->second.c_str());
    cache_[fontId] = nullptr;
    return nullptr;
  }
  BdfFont* font = new BdfFont();
  const bool ok = font->parse(source) && source.ok();
  if (!ok) {
    Serial.printf("[ddf-fonts] %s did not parse\n", entry->second.c_str());
    delete font;
    cache_[fontId] = nullptr;
    return nullptr;
  }

  Serial.printf("[ddf-fonts] %s: %u glyphs, ascent %d, descent %d\n", entry->second.c_str(),
                (unsigned)font->glyphCount(), font->fontAscent(), font->fontDescent());
  cache_[fontId] = font;
  lastUse_[fontId] = ++uses_;
  return font;
}
