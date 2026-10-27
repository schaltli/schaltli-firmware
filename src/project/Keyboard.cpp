#include "Keyboard.h"

#include <algorithm>
#include <cmath>

namespace keyboard {

namespace {

Key c(const char* text) { return Key{KeyKind::Char, text, 1.0f}; }
Key special(KeyKind kind, float units) { return Key{kind, "", units}; }

std::vector<Key> charRow(std::initializer_list<const char*> chars) {
  std::vector<Key> row;
  for (const char* ch : chars) row.push_back(c(ch));
  return row;
}

// Shift, the letters between, backspace.
std::vector<Key> shiftRow(std::initializer_list<const char*> chars) {
  std::vector<Key> row{special(KeyKind::Shift, 1.5f)};
  for (const char* ch : chars) row.push_back(c(ch));
  row.push_back(special(KeyKind::Backspace, 1.5f));
  return row;
}

// [123|ABC] [Cancel] [space] [OK], as wide as the widest letter row.
std::vector<Key> bottomRow(KeyKind layerKey, float widest) {
  return {special(layerKey, 1.5f), special(KeyKind::Cancel, 2.0f), special(KeyKind::Space, widest - 5.5f),
          special(KeyKind::Ok, 2.0f)};
}

using Rows = std::vector<std::vector<Key>>;

Rows symbols(const char* currency, float widest) {
  return {
      charRow({"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"}),
      charRow({"-", "/", ":", ";", "(", ")", currency, "&", "@", "\""}),
      [] {
        std::vector<Key> row = charRow({".", ",", "?", "!", "'", "#", "%", "+", "="});
        row.push_back(special(KeyKind::Backspace, 1.5f));
        return row;
      }(),
      bottomRow(KeyKind::Letters, widest),
  };
}

// A Swiss keyboard: ü ö ä right of P and L, è é à with shift - which is
// where a Swiss typist finds them.
const Rows DE_CH_LOWER = {
    charRow({"q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "\xC3\xBC"}),
    charRow({"a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xB6", "\xC3\xA4"}),
    shiftRow({"y", "x", "c", "v", "b", "n", "m"}),
    bottomRow(KeyKind::Symbols, 11),
};
const Rows DE_CH_UPPER = {
    charRow({"Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "\xC3\xA8"}),
    charRow({"A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\xA9", "\xC3\xA0"}),
    shiftRow({"Y", "X", "C", "V", "B", "N", "M"}),
    bottomRow(KeyKind::Symbols, 11),
};
const Rows DE_DE_LOWER = {
    charRow({"q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "\xC3\xBC"}),
    charRow({"a", "s", "d", "f", "g", "h", "j", "k", "l", "\xC3\xB6", "\xC3\xA4"}),
    shiftRow({"y", "x", "c", "v", "b", "n", "m", "\xC3\x9F"}),
    bottomRow(KeyKind::Symbols, 11),
};
// Shift on ß gives ?, as on a German keyboard.
const Rows DE_DE_UPPER = {
    charRow({"Q", "W", "E", "R", "T", "Z", "U", "I", "O", "P", "\xC3\x9C"}),
    charRow({"A", "S", "D", "F", "G", "H", "J", "K", "L", "\xC3\x96", "\xC3\x84"}),
    shiftRow({"Y", "X", "C", "V", "B", "N", "M", "?"}),
    bottomRow(KeyKind::Symbols, 11),
};
const Rows EN_US_LOWER = {
    charRow({"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"}),
    charRow({"a", "s", "d", "f", "g", "h", "j", "k", "l"}),
    shiftRow({"z", "x", "c", "v", "b", "n", "m"}),
    bottomRow(KeyKind::Symbols, 10),
};
const Rows EN_US_UPPER = {
    charRow({"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"}),
    charRow({"A", "S", "D", "F", "G", "H", "J", "K", "L"}),
    shiftRow({"Z", "X", "C", "V", "B", "N", "M"}),
    bottomRow(KeyKind::Symbols, 10),
};
const Rows DE_SYMBOLS = symbols("\xE2\x82\xAC", 11);  // €
const Rows EN_SYMBOLS = symbols("$", 10);

float rowUnits(const std::vector<Key>& row) {
  float units = 0;
  for (const Key& key : row) units += key.units;
  return units;
}

// The widest letter row: what one unit is measured against, in every layer
// of a layout, so a key keeps its size when the layer changes.
float widest(Layout layout) {
  float most = 0;
  for (const auto& row : rows(layout, Layer::Lower)) most = std::max(most, rowUnits(row));
  return most;
}

std::string lower(const std::string& s) {
  std::string out;
  for (char ch : s) out += (ch >= 'A' && ch <= 'Z') ? char(ch - 'A' + 'a') : ch;
  return out;
}

// Removes the last code point.
void dropLast(std::string& text) {
  if (text.empty()) return;
  size_t i = text.size() - 1;
  while (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) i--;
  text.erase(i);
}

}  // namespace

bool parseLayout(const std::string& name, Layout& out) {
  if (name == "de-CH") out = Layout::DeCH;
  else if (name == "de-DE") out = Layout::DeDE;
  else if (name == "en-US") out = Layout::EnUS;
  else return false;
  return true;
}

const char* layoutName(Layout layout) {
  switch (layout) {
    case Layout::DeCH: return "de-CH";
    case Layout::DeDE: return "de-DE";
    default: return "en-US";
  }
}

Layout fromBrowserLanguages(const std::string& languages) {
  size_t start = 0;
  while (start <= languages.size()) {
    size_t end = languages.find(',', start);
    if (end == std::string::npos) end = languages.size();
    std::string tag = lower(languages.substr(start, end - start));
    // Trimmed, and without a quality ("de;q=0.9", as an Accept-Language has).
    const size_t semi = tag.find(';');
    if (semi != std::string::npos) tag.erase(semi);
    while (!tag.empty() && tag.front() == ' ') tag.erase(0, 1);
    while (!tag.empty() && tag.back() == ' ') tag.pop_back();
    const std::string primary = tag.substr(0, tag.find('-'));
    if (primary == "de") return (tag == "de-ch" || tag == "de-li") ? Layout::DeCH : Layout::DeDE;
    if (primary == "en") return Layout::EnUS;
    start = end + 1;
  }
  return Layout::EnUS;
}

const std::vector<std::vector<Key>>& rows(Layout layout, Layer layer) {
  switch (layout) {
    case Layout::DeCH: return layer == Layer::Symbols ? DE_SYMBOLS : layer == Layer::Upper ? DE_CH_UPPER : DE_CH_LOWER;
    case Layout::DeDE: return layer == Layer::Symbols ? DE_SYMBOLS : layer == Layer::Upper ? DE_DE_UPPER : DE_DE_LOWER;
    default: return layer == Layer::Symbols ? EN_SYMBOLS : layer == Layer::Upper ? EN_US_UPPER : EN_US_LOWER;
  }
}

std::string label(const Key& key) {
  switch (key.kind) {
    case KeyKind::Char: return key.text;
    case KeyKind::Symbols: return "123";
    case KeyKind::Letters: return "ABC";
    case KeyKind::Cancel: return "Cancel";
    case KeyKind::Ok: return "OK";
    // Shift and backspace are drawn as shapes: no font here has arrows.
    default: return "";
  }
}

Outcome press(State& state, const Key& key) {
  switch (key.kind) {
    case KeyKind::Char:
    case KeyKind::Space: {
      if (textfield::textLength(state.text) >= state.bounds.maxLength) return Outcome::None;
      state.text += key.kind == KeyKind::Space ? std::string(" ") : key.text;
      if (state.layer == Layer::Upper) state.layer = Layer::Lower;
      return Outcome::None;
    }
    case KeyKind::Shift:
      state.layer = state.layer == Layer::Upper ? Layer::Lower : Layer::Upper;
      return Outcome::None;
    case KeyKind::Symbols:
      state.layer = Layer::Symbols;
      return Outcome::None;
    case KeyKind::Letters:
      state.layer = Layer::Lower;
      return Outcome::None;
    case KeyKind::Backspace:
      dropLast(state.text);
      return Outcome::None;
    case KeyKind::Cancel:
      return Outcome::Cancel;
    case KeyKind::Ok:
      return canSend(state) ? Outcome::Send : Outcome::None;
  }
  return Outcome::None;
}

std::vector<KeyRect> layoutKeys(const State& state, int x, int y, int w, int h, int gap) {
  std::vector<KeyRect> out;
  const auto& layerRows = rows(state.layout, state.layer);
  const float units = widest(state.layout);
  const float unit = static_cast<float>(w) / units;
  const int n = static_cast<int>(layerRows.size());
  for (int r = 0; r < n; r++) {
    const int top = y + (h * r) / n;
    const int bottom = y + (h * (r + 1)) / n;
    float at = (units - rowUnits(layerRows[r])) / 2.0f;
    for (const Key& key : layerRows[r]) {
      const int left = x + static_cast<int>(std::lround(at * unit));
      const int right = x + static_cast<int>(std::lround((at + key.units) * unit));
      out.push_back(KeyRect{left + gap / 2, top + gap / 2, right - left - gap, bottom - top - gap, &key});
      at += key.units;
    }
  }
  return out;
}

namespace {
std::string jsonString(const std::string& s) {
  std::string out = "\"";
  for (char ch : s) {
    if (ch == '"' || ch == '\\') out += '\\';
    out += ch;
  }
  return out + "\"";
}

const char* kindName(KeyKind kind) {
  switch (kind) {
    case KeyKind::Char: return "char";
    case KeyKind::Shift: return "shift";
    case KeyKind::Symbols: return "symbols";
    case KeyKind::Letters: return "letters";
    case KeyKind::Backspace: return "backspace";
    case KeyKind::Space: return "space";
    case KeyKind::Cancel: return "cancel";
    default: return "ok";
  }
}
}  // namespace

std::string describe(const State& state, const Frame& frame) {
  std::string out = "{\"open\":true,\"text\":" + jsonString(state.text) + ",\"layout\":\"" + layoutName(state.layout) +
                    "\",\"layer\":\"" +
                    (state.layer == Layer::Upper ? "upper" : state.layer == Layer::Symbols ? "symbols" : "lower") +
                    "\",\"canSend\":" + (canSend(state) ? "true" : "false") + ",\"keys\":[";
  bool first = true;
  for (const KeyRect& k : layoutKeys(state, frame.keysX, frame.keysY, frame.keysW, frame.keysH, frame.gap)) {
    if (!first) out += ",";
    first = false;
    out += "{\"label\":" + jsonString(k.key->kind == KeyKind::Char ? k.key->text : label(*k.key)) + ",\"kind\":\"" +
           kindName(k.key->kind) + "\",\"x\":" + std::to_string(k.x) + ",\"y\":" + std::to_string(k.y) +
           ",\"w\":" + std::to_string(k.w) + ",\"h\":" + std::to_string(k.h) + "}";
  }
  return out + "]}";
}

Frame frameFor(int width, int height, bool round) {
  Frame f{};
  if (round) {
    auto s = [width](int v) { return static_cast<int>(std::lround(v * width / 360.0)); };
    f.x = 0;
    f.y = 0;
    f.w = width;
    f.h = height;
    f.keysW = s(260);
    f.keysH = s(150);
    f.keysX = (width - f.keysW) / 2;
    f.keysY = height / 2 + s(45) - f.keysH / 2;
    f.lineW = s(240);
    f.lineH = s(44);
    f.lineX = (width - f.lineW) / 2;
    f.lineY = f.keysY - s(8) - f.lineH;
    f.gap = std::max(2, s(3));
    return f;
  }
  const bool landscape = width > height;
  const int shorter = std::min(width, height);
  const int margin = static_cast<int>(std::lround(shorter * 0.02));
  f.keysH = static_cast<int>(std::lround(height * (landscape ? 0.42 : 0.35)));
  f.lineH = static_cast<int>(std::lround(height * (landscape ? 0.12 : 0.07)));
  f.keysX = margin;
  f.keysW = width - 2 * margin;
  f.keysY = height - margin - f.keysH;
  f.lineX = 2 * margin;
  f.lineW = width - 4 * margin;
  f.lineY = f.keysY - margin - f.lineH;
  f.x = 0;
  f.y = f.lineY - margin;
  f.w = width;
  f.h = height - f.y;
  f.gap = std::max(3, static_cast<int>(std::lround(shorter * 0.012)));
  return f;
}

const Key* keyAt(const std::vector<KeyRect>& keys, int px, int py) {
  // The gaps count to the nearest key: a finger between two keys meant one.
  const KeyRect* best = nullptr;
  long bestDistance = 0;
  for (const KeyRect& k : keys) {
    const int dx = px < k.x ? k.x - px : (px >= k.x + k.w ? px - (k.x + k.w - 1) : 0);
    const int dy = py < k.y ? k.y - py : (py >= k.y + k.h ? py - (k.y + k.h - 1) : 0);
    const long d = static_cast<long>(dx) * dx + static_cast<long>(dy) * dy;
    if (best == nullptr || d < bestDistance) {
      best = &k;
      bestDistance = d;
    }
  }
  // Only within half a key of one, not anywhere on the screen.
  if (best == nullptr) return nullptr;
  const int reach = best->h / 2 + 1;
  return bestDistance <= static_cast<long>(reach) * reach ? best->key : nullptr;
}

}  // namespace keyboard
