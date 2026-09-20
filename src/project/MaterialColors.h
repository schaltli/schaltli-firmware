#pragma once

// The colour rules every Material-styled control shares: what a label on a
// colour has to be, and how a quieter shade of a colour is made.
//
// A line-for-line mirror of lib/material-colors.ts and the depth quantisers of
// lib/color-depth.ts in the designer repo. The two have to produce the same
// bytes or conformance finds the difference, so this file is deliberately
// boring: the same names, the same order, the same arithmetic. When one
// changes, the other changes in the same commit.
//
// Why: docs/2026-09-19-button-look.md (decision 2 and 5) and
// docs/2026-09-19-slider-look.md (decision 12) in the designer repo.

#include <Arduino.h>

#include <cmath>

// What this panel can actually show, compiled in per board - the same value
// the board's DDF declares as device.colorDepth. It decides nothing about how
// pixels are pushed; it decides which colours a derived one is allowed to be,
// so the designer's preview and this render agree on a mixed track.
#ifndef DEVICE_COLOR_DEPTH_4BIT
#define DEVICE_COLOR_DEPTH_4BIT 0
#endif
#ifndef DEVICE_COLOR_DEPTH_1BIT
#define DEVICE_COLOR_DEPTH_1BIT 0
#endif

namespace materialcolors {

struct Rgb {
  int r = 0;
  int g = 0;
  int b = 0;
  bool valid = false;
};

// A hex colour as three whole channels. Anything else - a named colour, a
// gradient, "transparent" - comes back invalid, and every caller then leaves
// the colour it was given alone rather than guessing at it.
inline Rgb channels(const String& color) {
  String c = color;
  c.trim();
  c.toLowerCase();
  auto hex = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
  };
  if (c.length() == 4 && c.charAt(0) == '#') {
    Rgb out;
    const int r = hex(c.charAt(1)), g = hex(c.charAt(2)), b = hex(c.charAt(3));
    if (r < 0 || g < 0 || b < 0) return out;
    out.r = r * 17;
    out.g = g * 17;
    out.b = b * 17;
    out.valid = true;
    return out;
  }
  if ((c.length() == 7 || c.length() == 9) && c.charAt(0) == '#') {
    Rgb out;
    int v[6];
    for (int i = 0; i < 6; i++) {
      v[i] = hex(c.charAt(1 + i));
      if (v[i] < 0) return out;
    }
    out.r = v[0] * 16 + v[1];
    out.g = v[2] * 16 + v[3];
    out.b = v[4] * 16 + v[5];
    out.valid = true;
    return out;
  }
  return Rgb();
}

inline String toHex(int r, int g, int b) {
  auto clamp = [](int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); };
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", clamp(r), clamp(g), clamp(b));
  return String(buf);
}

// Snaps a colour to what the panel can hold, exactly as lib/color-depth.ts
// does: the red channel's high nibble decides on 1 bit (the firmware's own
// parseColor quirk, mirrored there on purpose), and a 16-grey ramp by
// luminance on 4.
inline String quantizeFor1Bit(const String& color) {
  String c = color;
  c.trim();
  c.toLowerCase();
  if (c == "#000000" || c == "black" || c == "#000") return "#000000";
  if (c == "#ffffff" || c == "white" || c == "#fff") return "#ffffff";
  if (c.length() >= 7 && c.charAt(0) == '#') {
    const char nibble = c.charAt(1);
    if (nibble >= '0' && nibble <= '7') return "#000000";
  }
  return "#ffffff";
}

inline String quantizeFor4Bit(const String& color) {
  String c = color;
  c.trim();
  c.toLowerCase();
  if (c == "black") return "#000000";
  if (c == "white") return "#ffffff";
  const Rgb rgb = channels(c);
  if (!rgb.valid) return color;
  const int luma = (77 * rgb.r + 150 * rgb.g + 29 * rgb.b + 128) >> 8;
  const int level = (luma * 15 + 127) / 255;
  const int grey = level * 17;
  return toHex(grey, grey, grey);
}

/** The colour this panel would really show, given what the author picked. */
inline String applyDepth(const String& color) {
  String c = color;
  c.trim();
  if (c.isEmpty() || c.equalsIgnoreCase("transparent")) return color;
#if DEVICE_COLOR_DEPTH_1BIT
  return quantizeFor1Bit(c);
#elif DEVICE_COLOR_DEPTH_4BIT
  return quantizeFor4Bit(c);
#else
  return color;
#endif
}

/** `over` laid on `under` at `percent`. An unreadable colour leaves `under` as it is. */
inline String blend(const String& under, const String& over, int percent) {
  const Rgb u = channels(under);
  const Rgb o = channels(over);
  if (!u.valid || !o.valid) return under;
  auto mix = [&](int a, int b) {
    const float delta = (float)(b - a) * (float)percent / 100.0f;
    // Math.round's half-up, not lroundf's half-away-from-zero: the designer
    // rounds with Math.round and a negative half has to land on the same side.
    return a + (int)std::floor(delta + 0.5f);
  };
  return toHex(mix(u.r, o.r), mix(u.g, o.g), mix(u.b, o.b));
}

inline float relativeLuminance(const String& color) {
  const Rgb c = channels(color);
  if (!c.valid) return 1.0f;
  auto lin = [](int v) {
    const float s = (float)v / 255.0f;
    return s <= 0.03928f ? s / 12.92f : powf((s + 0.055f) / 1.055f, 2.4f);
  };
  return 0.2126f * lin(c.r) + 0.7152f * lin(c.g) + 0.0722f * lin(c.b);
}

/** A colour's tone: its CIELAB lightness L*, which is what Material picks by. */
inline float tone(const String& color) {
  const float y = relativeLuminance(color);
  return y <= 216.0f / 24389.0f ? (y * 24389.0f) / 27.0f : 116.0f * cbrtf(y) - 16.0f;
}

/**
 * White on a dark colour, black on a light one - Material's own rule
 * (DynamicColor.tonePrefersLightForeground): white below a tone of 60. Not
 * "whichever has the higher WCAG contrast", which puts black on a mid blue.
 */
inline String onColorFor(const String& color) {
  const float t = tone(color);
  return applyDepth(std::floor(t + 0.5f) < 60.0f ? String("#ffffff") : String("#000000"));
}

}  // namespace materialcolors
