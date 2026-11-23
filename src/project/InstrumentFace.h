#pragma once
// A styled gauge's or bar's baked face (device contract §2.15, «S565»):
// read once from LittleFS into PSRAM, kept by path until the project is
// loaded again. A face that is missing, unreadable or does not fit in memory
// is null; the renderer then draws the object's flat look and says so once.

#include <Arduino.h>
#include <stdint.h>

namespace instrumentface {

struct Face {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t* pixels = nullptr;  // width x height RGB565, PSRAM
  uint8_t* mask = nullptr;     // one bit per pixel, row by row, lowest bit first
  bool drawn(int32_t x, int32_t y) const {
    const int32_t i = y * width + x;
    return (mask[i >> 3] >> (i & 7)) & 1;
  }
};

// The face at `path`, loading it on first use; nullptr if it cannot be had.
const Face* get(const String& path);

// Lets every face go - call when a project is (re)loaded: a path names
// content, but a new project may not need the old ones.
void clear();

// What the faces hold, for /api/debug.
size_t bytesHeld();

}  // namespace instrumentface
