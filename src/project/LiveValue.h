#pragma once

// Live values: whatever changes with one value, set as rules
// (schaltli-designer docs/2026-10-07-live-values.md, device contract §2.6).
//
// A line-by-line port of the designer's lib/live-value.ts, as Placeholders.h
// is of lib/placeholders.ts; the Android app has a third. All three are held
// to the same cases, test/test_live_value/live-value-vectors.json - a
// byte-for-byte copy of the designer's lib/live-value/vectors.json.
//
// Plain C++ on std::string, no Arduino and no ArduinoJson: `pio test -e
// native` runs the vectors on the PC, and ProjectTypes.h includes this into
// the host tools (tools/theme-variant, …), which have neither. Reading one
// from JSON is LiveValueJson.h.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Placeholders.h"

namespace livevalue {

// Where a live value's one value comes from: "topic" (a `#json.path`
// included), "device", "project" or "combined".
struct Source {
    std::string ns;
    std::string path;
};

enum class ResultKind { Text, Icon };

// A text result's piece: literal text, or the value in its format.
struct Part {
    bool isValue = false;
    std::string text;
};

struct Result {
    ResultKind kind = ResultKind::Text;
    std::vector<Part> parts;  // Text
    std::string icon;         // Icon: the asset id
    std::string path;         // Icon: the baked bitmap the export wrote for it
    std::string pathDark;
};

struct Rule {
    std::string op;  // == != < <= > >= yes no
    std::string operand;
    Result result;
};

enum class FormatKind { AsIs, Number, Duration };

struct Format {
    FormatKind kind = FormatKind::AsIs;
    int decimals = 0;      // Number
    bool grouped = false;  // Number
    std::string pattern;   // Duration: h:mm:ss, h:mm or m:ss
};

struct LiveValue {
    std::string id;
    Source source;
    Format format;
    std::vector<Rule> rules;
    std::optional<Result> otherwise;   // absent: the value in its format
    std::optional<Result> noValueYet;  // absent: nothing
};

// Which branch applies: a rule's index, Otherwise or No value yet.
struct Applies {
    enum Branch { RuleBranch, Otherwise, NoValueYet } branch;
    int rule = -1;
    const Result* result = nullptr;  // null where the live value leaves it to the default
};

// A value that never arrived is std::nullopt; "" is an empty message that did.
using Lookup = std::function<std::optional<std::string>(const Source&)>;

// A plain decimal number once trimmed (sign, digits, point, digits), or nullopt.
std::optional<double> asNumber(const std::string& value);
bool isYes(const std::string& value);
bool isNo(const std::string& value);
bool matches(const std::string& value, const std::string& op, const std::string& operand);
Applies evaluate(const LiveValue& liveValue, const std::optional<std::string>& value);
std::string formatValue(const std::string& value, const Format& format, const placeholders::Separators& separators);

// Whole seconds as h:mm:ss, or the value as it came where it is no number of
// seconds: a dial's or gauge's number shown as a duration (designer device
// contract §2.12, the lib/live-value.ts formatDuration the designer draws with).
std::string durationText(const std::string& value);
// What a live value reads as text; an icon result reads as nothing.
std::string textOf(const LiveValue& liveValue, const std::optional<std::string>& value, const placeholders::Separators& separators);
// A text with `{live:<id>}` references as it reads.
std::string resolveLiveText(const std::string& text, const std::vector<LiveValue>& liveValues, const Lookup& lookup,
                            const placeholders::Separators& separators);

}  // namespace livevalue
