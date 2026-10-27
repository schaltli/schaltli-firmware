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
// the 2026-09-13 PaperS3 grilling, as revised on 2026-09-14:
//
//   - every redraw takes the partial path, because a tap has to feel answered
//   - once ten partial updates have piled up, the panel cleans itself with a
//     full refresh as soon as nobody has touched it for ten seconds
//   - a screen change is a new picture and is always painted clean
//
// Idle is measured from the last touch, not from the last draw. The first
// version measured it from the draw and applied it lazily to the next paint,
// which put the flash on the first tap after every pause - exactly the tap it
// was meant to spare - and needed a hard ceiling of ten partials so a
// dashboard fed by MQTT, which never stops drawing, would ever get cleaned.
// Counted from the finger, that dashboard goes idle ten seconds after anyone
// last touched it, however busy MQTT is, so there is no ceiling any more: a
// person tapping without a ten-second break is never interrupted by a flash.
//
// None of this is visible to the conformance run: the snapshot it compares
// is read out of the canvas, not off the glass. hil/papers3/refresh-rule.js
// checks the rule itself through /api/debug; whether the glass then actually
// looks clean is checked by hand (decision 11).
class EpdPanel {
public:
  // Partials that have to pile up before a clean-up is worth its flash.
  static const uint32_t PARTIALS_BEFORE_CLEANUP = 10;
  // How long nobody may have touched the panel before it cleans itself.
  static const uint32_t IDLE_AFTER_TOUCH_MS = 10000;

  bool begin()
  {
    auto cfg = M5.config();
    M5.begin(cfg);
    // Landscape, because the DDF says 960x540 and decision 8 says landscape
    // only in v1. M5GFX's rotation 0 on this panel is portrait 540x960 - the
    // long edge is the one the case stands on, not the one the panel counts
    // first - so without this the device renders a project laid out for a
    // screen it does not have. Caught at first boot by main.cpp's check
    // against the DDF's declared size, which is the whole reason that check
    // is there rather than trusting the panel.
    M5.Display.setRotation(1);
    width_ = (int16_t)M5.Display.width();
    height_ = (int16_t)M5.Display.height();
    // Quality mode for the first paint: whatever the panel held before this
    // firmware booted is not ours, and a partial update would keep it.
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.clear();
    return width_ > 0 && height_ > 0;
  }

  int16_t width() const { return width_; }
  int16_t height() const { return height_; }

  // Pushes the whole canvas - partial, unless forceFullNext() asked otherwise.
  void blit(const uint16_t *rgb565)
  {
    const bool full = forceFull_;
    M5.Display.setEpdMode(full ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
    M5.Display.pushImage(0, 0, width_, height_, rgb565, width_);
    M5.Display.display();

    forceFull_ = false;
    partialsSinceFull_ = full ? 0 : partialsSinceFull_ + 1;
    lastWasFull_ = full;
    paints_++;
  }

  // Pushes one rectangle of the canvas, partial - the keyboard of a text
  // field, whose keys must not wait for the whole glass. Counts as a partial
  // like any other, so the clean-up rule below still applies.
  void blitRegion(const uint16_t *rgb565, int16_t x, int16_t y, int16_t w, int16_t h)
  {
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    M5.Display.setClipRect(x, y, w, h);
    M5.Display.pushImage(0, 0, width_, height_, rgb565, width_);
    M5.Display.clearClipRect();
    M5.Display.display();
    partialsSinceFull_++;
    lastWasFull_ = false;
    paints_++;
  }

  // Counts every blit. Lets a caller that wants to update "its" screen in
  // place check that nobody else has painted over it in the meantime.
  uint32_t paints() const { return paints_; }

  // Called by whatever owns the screen when it has decided the next paint
  // must be a clean one - a screen switch, a setup screen, or the clean-up
  // below. Keeps that decision out of this class, which only knows about
  // wear and tear.
  void forceFullNext() { forceFull_ = true; }

  // For a caller that knows its partials left more behind than their count
  // says - the setup hold's countdown, whose black box is drawn over the
  // project up to seven times and then taken away again. The clean-up then
  // comes by the rule below, once nobody has touched the panel for ten
  // seconds, however few partials went before.
  void noteHeavyGhosting()
  {
    if (partialsSinceFull_ < PARTIALS_BEFORE_CLEANUP) partialsSinceFull_ = PARTIALS_BEFORE_CLEANUP;
  }

  // Every touch sample, glass or injected, restarts the idle clock. A finger
  // resting on the panel is someone using it, so a hold never gets flashed.
  void noteTouch()
  {
    lastTouchMs_ = millis();
    touchedSinceBoot_ = true;
  }

  // True when the loop should spend a full refresh now: enough ghosting to be
  // worth it, and nobody there to be interrupted by it.
  bool cleanupDue() const
  {
    return partialsSinceFull_ >= PARTIALS_BEFORE_CLEANUP && msSinceTouch() >= IDLE_AFTER_TOUCH_MS;
  }

  // Diagnostics for hil/papers3/refresh-rule.js and the manual check -
  // neither can see the glass, so the state has to be readable from outside.
  uint32_t partialsSinceFull() const { return partialsSinceFull_; }
  bool lastWasFull() const { return lastWasFull_; }
  bool touchedSinceBoot() const { return touchedSinceBoot_; }
  // Since boot when nothing has touched it yet - a panel nobody has used is
  // as idle as a panel can be.
  uint32_t msSinceTouch() const { return (uint32_t)(millis() - lastTouchMs_); }

private:
  int16_t width_ = 0;
  int16_t height_ = 0;
  uint32_t partialsSinceFull_ = 0;
  uint32_t paints_ = 0;
  uint32_t lastTouchMs_ = 0;
  bool touchedSinceBoot_ = false;
  bool forceFull_ = false;
  bool lastWasFull_ = false;
};
