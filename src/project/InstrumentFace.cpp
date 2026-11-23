#include "InstrumentFace.h"

#include <LittleFS.h>
#include <esp_heap_caps.h>

#include <map>

namespace instrumentface {

namespace {

struct Entry {
  Face face;
  bool failed = false;
};

std::map<String, Entry>& faces() {
  static std::map<String, Entry> m;
  return m;
}

size_t held = 0;

void* psram(size_t bytes) {
  void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
}

bool load(const String& path, Face& out) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[instrument] face %s: not found\n", path.c_str());
    return false;
  }
  uint8_t head[9];
  if (f.read(head, 9) != 9 || head[0] != 'S' || head[1] != '5' || head[2] != '6' || head[3] != '5' || head[4] != 1) {
    Serial.printf("[instrument] face %s: not an S565 v1 file\n", path.c_str());
    f.close();
    return false;
  }
  const uint16_t w = head[5] | (head[6] << 8);
  const uint16_t h = head[7] | (head[8] << 8);
  const size_t n = (size_t)w * h;
  const size_t maskBytes = (n + 7) / 8;
  uint16_t* px = (uint16_t*)psram(n * 2);
  uint8_t* mask = (uint8_t*)psram(maskBytes);
  if (!px || !mask) {
    Serial.printf("[instrument] face %s: %u x %u does not fit in memory (%u KB)\n", path.c_str(), w, h,
                  (unsigned)((n * 2 + maskBytes) / 1024));
    if (px) heap_caps_free(px);
    if (mask) heap_caps_free(mask);
    f.close();
    return false;
  }
  // Little-endian u16 on a little-endian core: the bytes are the pixels.
  const bool ok = f.read((uint8_t*)px, n * 2) == n * 2 && f.read(mask, maskBytes) == maskBytes;
  f.close();
  if (!ok) {
    Serial.printf("[instrument] face %s: short file\n", path.c_str());
    heap_caps_free(px);
    heap_caps_free(mask);
    return false;
  }
  out.width = w;
  out.height = h;
  out.pixels = px;
  out.mask = mask;
  held += n * 2 + maskBytes;
  return true;
}

}  // namespace

const Face* get(const String& path) {
  if (path.isEmpty()) return nullptr;
  auto it = faces().find(path);
  if (it != faces().end()) return it->second.failed ? nullptr : &it->second.face;
  Entry e;
  e.failed = !load(path, e.face);
  auto ins = faces().emplace(path, e).first;
  return ins->second.failed ? nullptr : &ins->second.face;
}

void clear() {
  for (auto& kv : faces()) {
    if (kv.second.face.pixels) heap_caps_free(kv.second.face.pixels);
    if (kv.second.face.mask) heap_caps_free(kv.second.face.mask);
  }
  faces().clear();
  held = 0;
}

size_t bytesHeld() { return held; }

}  // namespace instrumentface
