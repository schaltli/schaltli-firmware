#pragma once
#include <Wire.h>

// What the shared RGB-board firmware needs from a board's IO expander.
//
// On both Waveshare RGB boards the expander, not a GPIO, owns the backlight
// and the touch controller's reset line: a CH422G on the 4.3B, a CH32V003 on
// the 4". The chips speak different protocols but are asked the same few
// things, so main.cpp and Gt911.h see only this and each board's
// BoardProfile.h names the chip (using BoardExpander = ...).
class PanelExpander {
public:
  virtual ~PanelExpander() {}

  // Brings the expander up with every line in its power-on state. False when
  // the chip does not answer.
  virtual bool begin(TwoWire &wire) = 0;

  virtual bool setBacklight(bool on) = 0;

  // The touch controller's reset line: false holds it in reset, true
  // releases it.
  virtual bool setTouchReset(bool released) = 0;

  // The chip's name, for serial lines.
  virtual const char *name() const = 0;
};
