#pragma once
#include "interfaces/IDisplay.h"
#include "CanvasPresenter.h"

// IDisplay for setup-mode status text (AP instructions, connection status),
// drawn with Adafruit_GFX's own built-in font into the shared canvas and
// pushed through the compositor.
//
// Deliberately NOT going through ColorScreenRenderer/u8g2's pixel-perfect
// BDF pipeline: that exists to match the designer's rendering of a real
// authored project exactly, and this is throwaway setup UI with no
// designer-side counterpart to match. Same split the M5 Dial makes with
// M5DisplayAdapter.
//
// It shares the project canvas rather than drawing straight to the panel
// because LVGL owns the display: a direct gfx-> write would be overwritten
// by the next LVGL flush. Going through CanvasPresenter means setup text
// composes the same way rendered screens do.
class CanvasDisplayAdapter : public IDisplay {
public:
  explicit CanvasDisplayAdapter(CanvasPresenter& presenter) : presenter_(presenter) {}

  void showLines(std::initializer_list<String> lines) override;
  void showQrScreen(const String& qrData, const String& caption,
                    std::initializer_list<String> lines) override;

  // Where the text below the QR code begins, from the last showQrScreen().
  //
  // Exposed because the setup screen's tap-to-cancel band has to match the
  // layout, and hardcoding it in the caller is how they drift: the QR moved
  // to the top of the screen, the band stayed at its old y, and tapping the
  // line that says "tap to cancel" did nothing (2026-08-23). One value, one
  // owner - the code that draws it.
  int16_t qrScreenTextTop() const { return qrScreenTextTop_; }
  void clear() override;

private:
  int16_t qrScreenTextTop_ = 0;
  CanvasPresenter& presenter_;
};
