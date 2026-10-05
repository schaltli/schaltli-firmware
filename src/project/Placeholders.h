#pragma once

// Placeholders in texts: `Küche {topic:home/kitchen/humidity:F0} %`
// (schaltli-designer docs/2026-09-25-text-placeholders.md, the devices' side
// docs/2026-10-05-placeholder-devices.md).
//
// A line-by-line port of the designer's lib/placeholders.ts, and the Android
// app has a third (data/Placeholders.kt). All three are held to the same
// cases, test/test_placeholders/placeholder-vectors.json - a byte-for-byte
// copy of the designer's lib/placeholders/vectors.json.
//
// Plain C++ on std::string, no Arduino: `pio test -e native` runs the
// vectors on the PC. The renderer converts to and from String at its edge.
// Text is UTF-8 bytes; every character the grammar looks at is ASCII, so a
// byte walk reads it the way the designer's UTF-16 walk does.

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace placeholders {

struct Separators {
    std::string decimal = ".";
    std::string thousands = "'";  // may be empty: no grouping
};

struct NumberFormat {
    char kind;   // 'F' fixed decimals, 'N' the same with thousands grouped
    int digits;  // 0-9
};

struct Reference {
    std::string ns;    // "topic", "device" or "project"
    std::string path;  // a topic (with `#json.path`), or a device/project field
};

enum class SegmentKind {
    Literal,
    Placeholder,
    Raw,  // unknown, reserved or broken: shown exactly as written
};

struct Segment {
    SegmentKind kind;
    std::string text;    // Literal: the text; Placeholder and Raw: the source, braces included
    Reference reference;  // Placeholder only
    std::optional<std::string> fallback;
    std::optional<NumberFormat> format;
};

// A value that never arrived is std::nullopt - the only case `??` applies
// to; an empty message that did arrive is "".
using Lookup = std::function<std::optional<std::string>(const Reference&)>;

// A text as literals and placeholders, in order.
std::vector<Segment> parse(const std::string& text);

// A decimal payload rounded and written with the separators, or nullopt if it
// is not a plain decimal number (then it is shown as it is). Rounded on the
// digits as written, half away from zero, never through a float.
std::optional<std::string> formatNumber(const std::string& value, NumberFormat format, const Separators& separators);

// The text as it reads.
std::string resolve(const std::string& text, const Lookup& lookup, const Separators& separators);

// Every topic a text refers to, each once, `#json.path` included.
std::vector<std::string> referencedTopics(const std::string& text);

// Whether a text can hold a placeholder at all - the cheap test before
// resolving, so a text without one is drawn as it always was.
inline bool mayContainPlaceholder(const std::string& text) { return text.find('{') != std::string::npos; }

}  // namespace placeholders
