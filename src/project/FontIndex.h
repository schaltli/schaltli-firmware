#pragma once
// Which font file draws a font id, from the DDF's device.json - pure, so it
// runs on the PC too (test/test_font_index).
//
// A DDF font may list in `replaces` the ids of fonts it stands in for: the
// colour boards' 4-bit faces replace their 1-bit ones (designer
// docs/2026-11-01-4bit-fonts.md, 2026-11-02). A project still on a board names
// the old ids until the next deploy brings it migrated. While the old font
// is still declared it draws as it was made; once a DDF drops it (T14), the
// font that replaces it draws instead, so every text keeps drawing.
//
// Templated on the string type: Arduino's String on the board, std::string in
// the native tests.

#include <ArduinoJson.h>
#include <map>

namespace fontindex {

template <typename Str>
struct Index {
  std::map<Str, Str> fileById;    // declared id -> its file in the DDF zip
  std::map<Str, Str> replacedBy;  // old id -> the declared id replacing it
  Str firstId;                    // the first font declared, the fallback
};

// Reads device.json's `fonts`. A font without id or file is skipped; an old
// id two fonts both claim goes to the first (the build tool gives each one
// exactly one).
template <typename Str>
void read(JsonArrayConst fonts, Index<Str>& out) {
  for (JsonObjectConst font : fonts) {
    const char* id = font["id"] | "";
    const char* file = font["file"] | "";
    if (!*id || !*file) continue;
    out.fileById[Str(id)] = Str(file);
    if (out.firstId.length() == 0) out.firstId = Str(id);
    for (JsonVariantConst old : font["replaces"].as<JsonArrayConst>()) {
      const char* oldId = old | "";
      if (*oldId && out.replacedBy.find(Str(oldId)) == out.replacedBy.end()) out.replacedBy[Str(oldId)] = Str(id);
    }
  }
}

// The declared id that draws `id`: itself when declared, else the font that
// replaces it, else empty (the caller falls back to firstId).
template <typename Str>
Str resolve(const Index<Str>& index, const Str& id) {
  if (index.fileById.find(id) != index.fileById.end()) return id;
  const auto by = index.replacedBy.find(id);
  if (by != index.replacedBy.end() && index.fileById.find(by->second) != index.fileById.end()) return by->second;
  return Str();
}

}  // namespace fontindex
