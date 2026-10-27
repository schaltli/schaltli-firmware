#pragma once
// A text field: a topic's text in a field, and a new one typed and sent to
// its write topic (designer tasks/ha-text-plan.md, an MQTT `text` of Home
// Assistant).
//
// Ported line by line from the designer's lib/text-field.ts: how long a
// text is, what a hidden one shows, where the parts sit. The designer, the
// app and every board must agree on each of these, so nothing here is
// cleverer than the original. Text is UTF-8 throughout.

#include <cstdint>
#include <string>

namespace textfield {

// Home Assistant's own bounds for an MQTT text: `min` 0, `max` 255.
constexpr int MAX_LENGTH = 255;

// A text's length in characters - code points, as Python's `len()` counts
// them, which is what Home Assistant checks `min` and `max` against. Not
// bytes: "ü" is two of them and one character.
inline int textLength(const std::string& text) {
  int n = 0;
  for (unsigned char c : text)
    if ((c & 0xC0) != 0x80) n++;
  return n;
}

// The first `max` characters of `text`, never half of one.
inline std::string clampText(const std::string& text, int max) {
  int n = 0;
  for (size_t i = 0; i < text.size(); i++) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
      if (n == max) return text.substr(0, i);
      n++;
    }
  }
  return text;
}

struct Bounds {
  int minLength = 0;
  int maxLength = MAX_LENGTH;
};

// A field's own bounds, kept inside 0-255, max at least 1, min never above max.
inline Bounds bounds(int minLength, int maxLength) {
  auto whole = [](int v) { return v < 0 ? 0 : (v > MAX_LENGTH ? MAX_LENGTH : v); };
  Bounds b;
  b.maxLength = whole(maxLength);
  if (b.maxLength < 1) b.maxLength = 1;
  b.minLength = whole(minLength);
  if (b.minLength > b.maxLength) b.minLength = b.maxLength;
  return b;
}

// Whether «OK» sends: the text is within the field's bounds.
inline bool canSend(const std::string& text, const Bounds& b) {
  const int n = textLength(text);
  return n >= b.minLength && n <= b.maxLength;
}

// U+2022, what a hidden text is drawn as; '*' for a font without it.
constexpr uint16_t HIDDEN_CHAR = 0x2022;
inline const char* hiddenChar(bool fontHasBullet) { return fontHasBullet ? "\xE2\x80\xA2" : "*"; }

// What the field draws for `text`: as it is, or one dot per character.
inline std::string shownText(const std::string& text, bool hidden, bool fontHasBullet) {
  if (!hidden) return text;
  std::string out;
  const int n = textLength(text);
  for (int i = 0; i < n; i++) out += hiddenChar(fontHasBullet);
  return out;
}

// Where the field's parts go, in whole pixels: the container a rounded
// rectangle over the whole object, the corner a fifth of the height; a
// 2-px line along the bottom between the corners; the text from `pad` in,
// cut `pad` before the right edge.
struct Geometry {
  int radius;
  int pad;
  int lineY;  // the line's top edge, relative to the object's top
  int lineHeight;
};

constexpr int LINE = 2;

inline Geometry geometry(int width, int height) {
  const int w = width < 1 ? 1 : width;
  const int h = height < 1 ? 1 : height;
  Geometry g;
  g.radius = h / 5 < w / 2 ? h / 5 : w / 2;
  g.pad = h / 4 > g.radius ? h / 4 : g.radius;
  g.lineHeight = LINE < h ? LINE : h;
  g.lineY = h - g.lineHeight;
  return g;
}

// The text's baseline relative to the object's top: its line centred.
inline int baseline(int height, int ascent, int descent) {
  const int h = height < 1 ? 1 : height;
  return (h - (ascent + descent)) / 2 + ascent;
}

}  // namespace textfield
