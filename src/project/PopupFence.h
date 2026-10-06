#pragma once
#include <stdint.h>

// A popup's window (designer docs/device-contract.md 2.5): centred, 80 % of the
// display's area, a circle on a round display given by its bounding square.
// Display pixels. Plain C++ - no Arduino - so the PC tests reach it
// (test/test_popup_fence).
struct PopupFence {
  bool present = false;  // false: the project has no popups
  bool circle = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;

  // Whether a point lies inside the window - the same test as the designer's
  // insideFence (lib/popup.ts), so a tap the preview closes on is a tap the
  // device closes on.
  bool contains(int px, int py) const {
    if (!present) return false;
    if (circle) {
      // In doubled coordinates, to stay on integers for odd diameters.
      const long r2 = width;  // 2r
      const long dx = 2L * px - (2L * x + width);
      const long dy = 2L * py - (2L * y + height);
      return dx * dx + dy * dy <= r2 * r2;
    }
    return px >= x && px < x + width && py >= y && py < y + height;
  }
};

// One RGB565 pixel at half brightness, each channel halved - what an RGB
// device dims the screen underneath an open popup to (a black scrim at 50 %).
inline uint16_t halveRgb565(uint16_t c) { return (c >> 1) & 0x7BEF; }
