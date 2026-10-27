#pragma once
// The on-screen keyboard every board opens for a text field (designer
// tasks/ha-text-plan.md, decisions 3 and 4). One piece of code for the
// 4.3B, the Knob and the PaperS3: the layouts, what a key does, where the
// keys lie in the rectangle a board gives it. Drawing and touch are the
// boards' (ColorScreenRenderer::renderKeyboard, each main.cpp).
//
// The layout follows the device's «Keyboard» setting: de-CH, de-DE, en-US.
// Its default is the browser's language, the first of navigator.languages
// that maps (fromBrowserLanguages).
//
// Typing goes at the end only; there is no cursor to move. Pure C++ with
// UTF-8 strings, so it is tested on the PC (test/test_keyboard).

#include <string>
#include <vector>

#include "TextField.h"

namespace keyboard {

enum class Layout { DeCH, DeDE, EnUS };

// "de-CH", "de-DE", "en-US"; anything else is false.
bool parseLayout(const std::string& name, Layout& out);
const char* layoutName(Layout layout);

// The layout of the first language that maps, from a comma-separated list
// as navigator.languages gives it: de-CH and de-LI the Swiss one, any other
// German the German one, any English the US one. A language that maps to
// none is passed over; when none maps, en-US.
Layout fromBrowserLanguages(const std::string& languages);

enum class Layer { Lower, Upper, Symbols };

enum class KeyKind { Char, Shift, Symbols, Letters, Backspace, Space, Cancel, Ok };

struct Key {
  KeyKind kind;
  std::string text;  // what a Char key types, UTF-8; its label too
  float units;       // its width, a letter key being 1
};

// The rows of one layer, top to bottom; the last row is always
// [123|ABC] [Cancel] [space] [OK].
const std::vector<std::vector<Key>>& rows(Layout layout, Layer layer);

// What a key says on its face.
std::string label(const Key& key);

struct State {
  Layout layout = Layout::DeCH;
  Layer layer = Layer::Lower;
  std::string text;
  textfield::Bounds bounds;
};

enum class Outcome { None, Cancel, Send };

// A press of `key`. Shift holds for one character. A character past max
// is not taken; OK below min does nothing.
Outcome press(State& state, const Key& key);

// Whether OK is live (drawn as such).
inline bool canSend(const State& state) { return textfield::canSend(state.text, state.bounds); }

struct KeyRect {
  int x, y, w, h;
  const Key* key;
};

// Every key of the state's layer laid into the rectangle: four rows of
// equal height, each row centred, a key's width its units of the widest
// row's, `gap` pixels between keys.
std::vector<KeyRect> layoutKeys(const State& state, int x, int y, int w, int h, int gap);

// The key under (px, py), or nullptr.
const Key* keyAt(const std::vector<KeyRect>& keys, int px, int py);

// Where the keyboard goes on a screen: what it covers, the line showing the
// text so far, the keys, and the gap between them.
struct Frame {
  int x, y, w, h;
  int lineX, lineY, lineW, lineH;
  int keysX, keysY, keysW, keysH;
  int gap;
};

// A rectangular screen: the keys over the lower part, the line above them,
// all of it covering the screen from just above the line down. A round one
// (the Knob): the whole screen, the keys 260 x 150 (at 360) set 45 below
// the centre so their far corners stay inside the bezel - what was built
// and checked on the Knob on 2026-08-18 - and the line above them.
Frame frameFor(int width, int height, bool round);

// The open keyboard as JSON, for a test to tap by labels: its text, layout,
// layer and keys ({"label","kind","x","y","w","h"}).
std::string describe(const State& state, const Frame& frame);

}  // namespace keyboard
