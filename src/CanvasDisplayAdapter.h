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
  void clear() override;

private:
  CanvasPresenter& presenter_;
};
