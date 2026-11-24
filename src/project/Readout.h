#pragma once
// A styled instrument's digital readout - designer lib/instrument-readout.ts,
// integer for integer (device contract §2.15, «The readout»). The designer
// bakes the characters a readout can show into a strip of alpha (.a8); a
// board writes the value with them, character by character, in the style's
// colour, after the needle. Arduino-free, so the PC test runs it too.
//
// The designer's lib/instrument-readout/vectors.json is the reference
// (test/test_readout).

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "NeedleRaster.h"

namespace readout {

// Per character: where its cell starts in the strip, how wide it is, how far the pen moves. Pixels.
struct Cell {
  int32_t sx = 0, w = 0, advance = 0;
};

// `instrument.readout` without the strip's path.
struct Description {
  std::string chars;
  std::vector<Cell> cells;
  int32_t pad = 0;     // a cell reaches this far left of the pen
  int32_t height = 0;  // pixels
  int32_t x = 0, y = 0;  // the right edge (or the middle) of the text, and its top, in the object's box
  bool center = false;   // false: right-aligned
  uint16_t color = 0;    // RGB565
  int32_t decimals = 0;
};

// The longest text format() writes, its terminator included.
static const size_t TEXT_MAX = 48;

// The text a readout shows for a raw value (formatReadout()): the number
// rounded half away from nought to `decimals` places, a minus only where
// something is left after rounding; "" for none, not a number or not
// finite. `out` holds TEXT_MAX bytes. Returns the length.
size_t format(const char* raw, int32_t decimals, char* out);

// Where the pen starts for `text` (readoutStart()).
int32_t start(const char* text, const Description& d);

// Writes `text` into `t` in d.color with the strip's alpha (`stripWidth` x
// `stripHeight`, one byte a pixel), `opacity` out of 256 (drawReadout()).
// A character the strip lacks is skipped; t's clip holds as for the needle.
void draw(needleraster::Target& t, const Description& d, const uint8_t* alpha, int32_t stripWidth, int32_t stripHeight,
          const char* text, int32_t opacity = 256);

}  // namespace readout
