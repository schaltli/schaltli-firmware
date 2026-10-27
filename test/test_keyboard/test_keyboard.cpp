// The on-screen keyboard (Keyboard.h), on the PC:
//
//   pio test -e native -f test_keyboard
//
// Typing is pressing keys by their text, as a finger finds them on the
// glass; a word that needs a key a layout lacks fails there.

#include <unity.h>

#include <string>

#include "project/Keyboard.h"

using namespace keyboard;

void setUp() {}
void tearDown() {}

// The key of the current layer whose label is `text`, nullptr if none.
static const Key* find(const State& s, const std::string& text) {
  for (const auto& row : rows(s.layout, s.layer))
    for (const Key& key : row)
      if (key.kind == KeyKind::Char && key.text == text) return &key;
  return nullptr;
}

static const Key* findKind(const State& s, KeyKind kind) {
  for (const auto& row : rows(s.layout, s.layer))
    for (const Key& key : row)
      if (key.kind == kind) return &key;
  return nullptr;
}

// Types one character, reaching it through shift or the symbols as a person
// would; fails when no layer has it.
static void type(State& s, const std::string& ch) {
  if (ch == " ") {
    press(s, *findKind(s, KeyKind::Space));
    return;
  }
  for (int attempt = 0; attempt < 3; attempt++) {
    if (const Key* key = find(s, ch)) {
      press(s, *key);
      return;
    }
    // Lower -> Upper -> Symbols -> Lower.
    if (s.layer == Layer::Lower) press(s, *findKind(s, KeyKind::Shift));
    else if (s.layer == Layer::Upper) {
      press(s, *findKind(s, KeyKind::Shift));
      press(s, *findKind(s, KeyKind::Symbols));
    } else press(s, *findKind(s, KeyKind::Letters));
  }
  TEST_FAIL_MESSAGE(("no key types " + ch).c_str());
}

static void typeAll(State& s, const char* utf8) {
  std::string text(utf8);
  for (size_t i = 0; i < text.size();) {
    size_t n = 1;
    const unsigned char b = static_cast<unsigned char>(text[i]);
    if (b >= 0xF0) n = 4;
    else if (b >= 0xE0) n = 3;
    else if (b >= 0xC0) n = 2;
    type(s, text.substr(i, n));
    i += n;
  }
}

static void swissTypesGrueezi() {
  State s;
  s.layout = Layout::DeCH;
  typeAll(s, "Gr\xC3\xBC" "ezi 1");
  TEST_ASSERT_EQUAL_STRING("Gr\xC3\xBC" "ezi 1", s.text.c_str());
  // è é à with shift, where a Swiss keyboard has them.
  State t;
  t.layout = Layout::DeCH;
  typeAll(t, "\xC3\xA8\xC3\xA9\xC3\xA0");
  TEST_ASSERT_EQUAL_STRING("\xC3\xA8\xC3\xA9\xC3\xA0", t.text.c_str());
  // z next to t, y bottom left.
  TEST_ASSERT_EQUAL_STRING("z", rows(Layout::DeCH, Layer::Lower)[0][5].text.c_str());
}

static void germanTypesStrasse() {
  State s;
  s.layout = Layout::DeDE;
  typeAll(s, "Stra\xC3\x9F" "e \xC3\x9C");
  TEST_ASSERT_EQUAL_STRING("Stra\xC3\x9F" "e \xC3\x9C", s.text.c_str());
}

static void usHasNoUmlauts() {
  State s;
  s.layout = Layout::EnUS;
  TEST_ASSERT_NULL(find(s, "\xC3\xBC"));
  TEST_ASSERT_EQUAL_STRING("y", rows(Layout::EnUS, Layer::Lower)[0][5].text.c_str());
  typeAll(s, "Hello $5");
  TEST_ASSERT_EQUAL_STRING("Hello $5", s.text.c_str());
}

static void shiftHoldsForOneCharacter() {
  State s;
  press(s, *findKind(s, KeyKind::Shift));
  TEST_ASSERT_TRUE(s.layer == Layer::Upper);
  press(s, *find(s, "A"));
  TEST_ASSERT_TRUE(s.layer == Layer::Lower);
  press(s, *find(s, "a"));
  TEST_ASSERT_EQUAL_STRING("Aa", s.text.c_str());
}

static void boundsAndBackspace() {
  State s;
  s.bounds = textfield::bounds(2, 3);
  typeAll(s, "\xC3\xBC");
  TEST_ASSERT_TRUE(press(s, *findKind(s, KeyKind::Ok)) == Outcome::None);  // below min
  typeAll(s, "abcd");
  TEST_ASSERT_EQUAL_STRING("\xC3\xBC" "ab", s.text.c_str());  // stopped at max, by characters
  TEST_ASSERT_TRUE(press(s, *findKind(s, KeyKind::Ok)) == Outcome::Send);
  press(s, *findKind(s, KeyKind::Backspace));
  press(s, *findKind(s, KeyKind::Backspace));
  TEST_ASSERT_EQUAL_STRING("\xC3\xBC", s.text.c_str());  // a whole ü, not half of it
  TEST_ASSERT_TRUE(press(s, *findKind(s, KeyKind::Cancel)) == Outcome::Cancel);
}

static void browserLanguages() {
  TEST_ASSERT_TRUE(fromBrowserLanguages("de-CH,de;q=0.9,en") == Layout::DeCH);
  TEST_ASSERT_TRUE(fromBrowserLanguages("de-LI") == Layout::DeCH);
  TEST_ASSERT_TRUE(fromBrowserLanguages("de-DE,en-US") == Layout::DeDE);
  TEST_ASSERT_TRUE(fromBrowserLanguages("de") == Layout::DeDE);
  TEST_ASSERT_TRUE(fromBrowserLanguages("fr-CH, de-CH") == Layout::DeCH);  // French maps to none: passed over
  TEST_ASSERT_TRUE(fromBrowserLanguages("en-GB,de-CH") == Layout::EnUS);
  TEST_ASSERT_TRUE(fromBrowserLanguages("fr-FR,it") == Layout::EnUS);
  TEST_ASSERT_TRUE(fromBrowserLanguages("") == Layout::EnUS);
  Layout l;
  TEST_ASSERT_TRUE(parseLayout("de-DE", l) && l == Layout::DeDE);
  TEST_ASSERT_FALSE(parseLayout("fr-CH", l));
  TEST_ASSERT_EQUAL_STRING("de-CH", layoutName(Layout::DeCH));
}

static void geometryFillsTheRectangle() {
  State s;
  s.layout = Layout::DeCH;
  // The 4.3B's lower part: 800 wide, four rows of 60.
  const auto keys = layoutKeys(s, 0, 240, 800, 240, 4);
  TEST_ASSERT_EQUAL(11 + 11 + 9 + 4, (int)keys.size());
  // The first key of the top row starts half a gap in; the widest rows end
  // half a gap before the edge.
  TEST_ASSERT_EQUAL(2, keys[0].x);
  TEST_ASSERT_EQUAL(242, keys[0].y);
  TEST_ASSERT_EQUAL(800, keys[10].x + keys[10].w + 2);
  TEST_ASSERT_EQUAL(56, keys[0].h);
  // A tap in a gap goes to the nearest key, one far off to none.
  TEST_ASSERT_EQUAL_STRING("q", keyAt(keys, 1, 241)->text.c_str());
  TEST_ASSERT_NULL(keyAt(keys, 400, 100));
  // The bottom row: OK last.
  TEST_ASSERT_TRUE(keys.back().key->kind == KeyKind::Ok);
}

static void framesPerScreen() {
  // The 4.3B: the keys over the lower 42 %, the line above them.
  Frame f = frameFor(800, 480, false);
  TEST_ASSERT_EQUAL(268, f.keysY);
  TEST_ASSERT_EQUAL(202, f.keysH);
  TEST_ASSERT_EQUAL(480, f.keysY + f.keysH + 10);
  TEST_ASSERT_EQUAL(200, f.lineY);
  TEST_ASSERT_EQUAL(190, f.y);
  // The Knob: 260 x 150, 45 below the centre, inside the bezel.
  f = frameFor(360, 360, true);
  TEST_ASSERT_EQUAL(50, f.keysX);
  TEST_ASSERT_EQUAL(150, f.keysY);
  TEST_ASSERT_EQUAL(260, f.keysW);
  TEST_ASSERT_EQUAL(150, f.keysH);
  // Its far corners within the circle of radius 180 round (180, 180).
  const long dx = 260 / 2, dy = 150 + 150 - 180;
  TEST_ASSERT_TRUE(dx * dx + dy * dy < 180L * 180);
  TEST_ASSERT_TRUE(f.lineY > 30);
  // The PaperS3, landscape too.
  f = frameFor(960, 540, false);
  TEST_ASSERT_EQUAL(540, f.keysY + f.keysH + 11);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(swissTypesGrueezi);
  RUN_TEST(germanTypesStrasse);
  RUN_TEST(usHasNoUmlauts);
  RUN_TEST(shiftHoldsForOneCharacter);
  RUN_TEST(boundsAndBackspace);
  RUN_TEST(browserLanguages);
  RUN_TEST(geometryFillsTheRectangle);
  RUN_TEST(framesPerScreen);
  return UNITY_END();
}
