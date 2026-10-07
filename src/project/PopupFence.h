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

// The round close button a board draws on the fence's edge (designer
// docs/device-contract.md 2.5, lib/popup.ts popupCloseBadge): its centre on
// the top right corner of a rectangle, on a circle where the diagonal to the
// top right meets it. Taps count within hitRadius, wider than the button
// drawn, so a finger finds it. Display pixels.
struct PopupCloseBadge {
  bool present = false;
  int cx = 0;
  int cy = 0;
  int radius = 0;
  int hitRadius = 0;

  bool contains(int px, int py) const {
    if (!present) return false;
    const long dx = px - cx;
    const long dy = py - cy;
    return dx * dx + dy * dy <= (long)hitRadius * hitRadius;
  }
};

// radius 0: the board draws no close button.
inline PopupCloseBadge popupCloseBadgeFor(const PopupFence& fence, int radius) {
  PopupCloseBadge b;
  if (!fence.present || radius <= 0) return b;
  b.present = true;
  b.radius = radius;
  b.hitRadius = radius * 8 / 5;
  if (fence.circle) {
    // Rounded as the designer's Math.round rounds these positive values.
    const double r = fence.width / 2.0;
    const double off = r * 0.70710678;
    b.cx = (int)(fence.x + r + off + 0.5);
    b.cy = (int)(fence.y + r - off + 0.5);
  } else {
    b.cx = fence.x + fence.width;
    b.cy = fence.y;
  }
  return b;
}

// Its colours, on every device and in the designer (lib/popup.ts).
#define POPUP_CLOSE_BADGE_DISC "#303030"
#define POPUP_CLOSE_BADGE_CROSS "#ffffff"

// One RGB565 pixel at half brightness, each channel halved - what an RGB
// device dims the screen underneath an open popup to (a black scrim at 50 %).
inline uint16_t halveRgb565(uint16_t c) { return (c >> 1) & 0x7BEF; }
