#pragma once
#include <Arduino.h>
#include <map>
#include <vector>

#include "BdfFont.h"
#include "FontIndex.h"

// Hands out the fonts this device publishes in its own DDF, parsed from the
// .bdf files inside the compiled-in DDF_ZIP.
//
// The DDF is the device's self-description, and the designer renders text
// with exactly the fonts it finds there - it fetches them from this device.
// Serving one font and drawing with another is the whole bug this replaces
// (see BdfFont.h): the published helvR18.bdf has 756 glyphs, the u8g2
// array that used to do the drawing has 191, and nothing could notice
// because both were called "helvR18".
//
// Loaded lazily and cached: a project typically uses one or two of the four
// fonts, and parsing one costs a decompression plus a pass over ~150KB.
// Nothing is freed - the set is small, bounded by the DDF, and PSRAM has
// room for all four several times over.
class DdfFontStore {
public:
  // Parsed font for a DDF font id ("font-helvR18"), or nullptr if the DDF
  // has no such font or it could not be read. An id the DDF no longer
  // declares but lists in a font's `replaces` gets that font (FontIndex.h).
  const BdfFont* get(const String& fontId);

  // The declared id that draws `fontId` - itself, or the font replacing it -
  // or "" when there is none. For metrics that must be the drawn font's: an
  // old project's own entry for a replaced font carries the old font's.
  String drawnId(const String& fontId);

  // Free PSRAM below which no more fonts are loaded ahead (preload) and
  // fonts not wanted now are let go (trim). A 4-bit face takes 50-370 KB,
  // and a project mixing typographies once used all of the 4.3B's PSRAM
  // (2026-11-02, designer docs/2026-11-01-4bit-fonts.md); what else lives
  // there - chart history, a popup's pixels - needs room beside them.
  static size_t reserveBytes() { return reserveBytes_; }
  // For the HIL (?set=fontreserve=<KB>): a reserve larger than the PSRAM
  // makes every screen change let go of every font it does not name.
  static void setReserveBytes(size_t bytes) { reserveBytes_ = bytes; }
  static size_t freePsram();

  // How many fonts are parsed now, and how many were let go since boot.
  size_t parsedCount() const;
  uint32_t letGo() const { return letGo_; }

  // Lets go of the least recently used fonts, none of `keep` (ids as a
  // project names them) and never the fallback, until free PSRAM is back
  // above kReserveBytes or nothing more may go. Only between draws - a
  // pointer from get() is good until then. Returns how many went.
  size_t trim(const std::vector<String>& keep);

  // The id of the first font the DDF declares - what to fall back to when a
  // project names a font this device does not publish. Deliberately
  // explicit rather than silently substituting inside get(): a caller that
  // wants the fallback asks for it, and one that wants to know the font is
  // missing still can.
  String defaultFontId();

private:
  bool ensureIndex();

  // Declared ids, their files and what replaces what, read once from
  // device.json.
  fontindex::Index<String> index_;
  std::map<String, BdfFont*> cache_;
  // When each parsed font was last handed out, by the id it is cached under.
  std::map<String, uint32_t> lastUse_;
  uint32_t uses_ = 0;
  uint32_t letGo_ = 0;
  static size_t reserveBytes_;
  bool indexed_ = false;
};
