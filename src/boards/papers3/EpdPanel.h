#pragma once
#include <Arduino.h>
#include <M5Unified.h>

// The panel, and the refresh discipline that goes with it.
//
// This is the first e-ink board in this repo, and e-ink is the one display
// technology where *when* you push matters as much as what. A partial update
// is fast and leaves a faint trace of what was there before; enough of them
// stacked up and the screen looks dirty. A full update wipes that trace and
// costs over a second of black-and-white flashing. Neither is right always,
// so the rule has to be written down rather than felt out - decision 6 of
// the 2026-09-13 PaperS3 grilling:
//
//   - a redraw takes the partial path, because a tap has to feel answered
//   - after ten partial updates the next one is full, no matter what
//   - and sooner than that whenever nothing has been drawn for ten seconds,
//     because a quiet moment is free and the tenth tap is not
//
// The ceiling is what makes the idle rule safe. Without it, a dashboard fed
// by MQTT redraws all day without anyone touching it, never reaches an idle
// moment, and ghosts forever.
//
// None of this is visible to the conformance run: the snapshot it compares
// is read out of the canvas, not off the glass, so ghosting and refresh
// strategy stay green whatever they do. They are checked by hand instead
// (decision 11), which is exactly why the numbers live here as named
// constants rather than scattered through the call sites.
class EpdPanel {
public:
  // Ten partial updates, then a full one. See the class comment.
  static const uint32_t PARTIALS_BEFORE_FULL = 10;
  // Ten seconds without a redraw counts as idle, and idle is when a full
  // refresh is cheapest.
  static const uint32_t IDLE_MS = 10000;

  bool begin()
  {
    auto cfg = M5.config();
    M5.begin(cfg);
    width_ = (int16_t)M5.Display.width();
    height_ = (int16_t)M5.Display.height();
    // Quality mode for the first paint: whatever the panel held before this
    // firmware booted is not ours, and a partial update would keep it.
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.clear();
    lastDrawMs_ = millis();
    return width_ > 0 && height_ > 0;
  }

  int16_t width() const { return width_; }
  int16_t height() const { return height_; }

  // Pushes the whole canvas. Picks its own refresh mode by the rule above.
  void blit(const uint16_t *rgb565)
  {
    pushRect(rgb565, 0, 0, width_, height_, width_);
  }

  // Pushes one rectangle out of a full-width canvas - what an object redraw
  // uses, and the reason partial updates exist at all here.
  void blitRect(const uint16_t *rgb565, int16_t x, int16_t y, int16_t w, int16_t h)
  {
    if (w <= 0 || h <= 0) return;
    pushRect(rgb565 + (int32_t)y * width_ + x, x, y, w, h, width_);
  }

  // Called by whatever owns the screen when it has decided the next paint
  // must be a clean one - a screen switch, or leaving setup mode. Keeps that
  // decision out of this class, which only knows about wear and tear.
  void forceFullNext() { partialsSinceFull_ = PARTIALS_BEFORE_FULL; }

  // The setup-mode countdown draws once a second in one corner and must not
  // spend the budget: it would hit the ceiling exactly as the hold completes
  // and flash the whole panel white immediately before setup mode appears.
  // Reaching setup mode repaints everything anyway, so the trace it leaves
  // is cleared regardless (decision 7).
  void blitRectUncounted(const uint16_t *rgb565, int16_t x, int16_t y, int16_t w, int16_t h)
  {
    if (w <= 0 || h <= 0) return;
    const uint32_t before = partialsSinceFull_;
    pushRect(rgb565 + (int32_t)y * width_ + x, x, y, w, h, width_);
    partialsSinceFull_ = before;
  }

  // Diagnostics for the manual check decision 11 asks for - no automated
  // test can see this, so it has to be readable from outside.
  uint32_t partialsSinceFull() const { return partialsSinceFull_; }
  bool lastWasFull() const { return lastWasFull_; }

private:
  void pushRect(const uint16_t *src, int16_t x, int16_t y, int16_t w, int16_t h, int16_t stride)
  {
    const bool idle = (uint32_t)(millis() - lastDrawMs_) >= IDLE_MS;
    const bool full = idle || partialsSinceFull_ >= PARTIALS_BEFORE_FULL;

    M5.Display.setEpdMode(full ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
    M5.Display.pushImage(x, y, w, h, src, stride);
    M5.Display.display();

    partialsSinceFull_ = full ? 0 : partialsSinceFull_ + 1;
    lastWasFull_ = full;
    lastDrawMs_ = millis();
  }

  int16_t width_ = 0;
  int16_t height_ = 0;
  uint32_t partialsSinceFull_ = 0;
  uint32_t lastDrawMs_ = 0;
  bool lastWasFull_ = false;
};
