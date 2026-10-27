#pragma once

// A button's message sent with values, only when, then clearing (designer
// tasks/ha-alarm-plan.md, decision 4; DDF feature "button-values"): what an
// alarm panel's Disarm needs - the code typed in a text field put into the
// command, sent only where it matches, and not left standing in the field.
//
// A port of the designer's lib/button-values.ts:
// - the message may hold `{topic:…}`, replaced at the press by what that
//   topic holds (nothing yet: empty). Only that is special: every other
//   brace stands as written, a command being JSON so often;
// - `onlyWhen`: a combined topic's name; the press sends nothing unless it
//   is "true";
// - `clears`: a local topic set empty after sending - the board's to do.
//
// Plain C++, no Arduino: `pio test -e native -f test_button_values`.

#include <functional>
#include <optional>
#include <string>

namespace buttonvalues {

using Lookup = std::function<std::optional<std::string>(const std::string&)>;

// The message with each `{topic:path}` replaced by `valueOf(path)` - empty
// where it has none - and everything else as written. An opening without
// its closing brace stays as written too.
inline std::string fillMessage(const std::string& message, const Lookup& valueOf) {
  static const std::string OPEN = "{topic:";
  std::string out;
  size_t i = 0;
  for (;;) {
    const size_t at = message.find(OPEN, i);
    if (at == std::string::npos) break;
    const size_t close = message.find('}', at);
    if (close == std::string::npos) break;
    out += message.substr(i, at - i);
    out += valueOf(message.substr(at + OPEN.size(), close - at - OPEN.size())).value_or("");
    i = close + 1;
  }
  return out + message.substr(i);
}

// What a press sends: the message filled in, or nothing where its «Only
// when» is not yes.
inline std::optional<std::string> pressedMessage(const std::string& message, const std::string& onlyWhen, const Lookup& combined,
                                                 const Lookup& topic) {
  if (!onlyWhen.empty() && combined(onlyWhen).value_or("") != "true") return std::nullopt;
  return fillMessage(message, topic);
}

}  // namespace buttonvalues
