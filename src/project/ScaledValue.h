#pragma once

// Scaled values: a number times a fraction plus an offset, rounded to a
// number of decimals - what a scaled topic reads and a write format sends
// (schaltli-designer tasks/ha-templates-stage2-plan.md, decisions 1 and 3;
// device contract §2.11). A port of the designer's lib/scaled-value.ts, line
// by line; the cases are test/test_scaled_value/scaled-value-vectors.json.
//
// No float: every intermediate fits in an int64_t within the bounds below.

#include <cstdint>
#include <optional>
#include <string>

namespace scaled {

// times numerator / denominator, plus offset, rounded to decimals.
struct Scale {
    int64_t numerator = 1;
    int64_t denominator = 1;
    std::string offset = "0";  // a plain decimal, at most 6 places
    int decimals = 0;          // 0-6, at least the offset's places
};

// What a level sends: `text` with `{value}` in it, the value scaled first
// where `scaled` (any of numerator, denominator, offset was given).
struct WriteFormat {
    std::string text;
    bool scaled = false;
    Scale scale;
};

constexpr int64_t MAX_FACTOR = 1000000;
constexpr int MAX_SIGNIFICANT_DIGITS = 12;
constexpr int MAX_FRACTION_DIGITS = 9;
constexpr int MAX_OFFSET_DIGITS = 6;
constexpr int MAX_DECIMALS = 6;
// A result and an offset stay below this.
constexpr int64_t LIMIT = 1000000000000LL;

struct Decimal {
    int64_t units;
    int places;
};

// A plain decimal - sign, digits, a point, digits, nothing else but spaces
// around - as an integer and the places after its point; nullopt where it is
// not one, or has more digits than allowed.
std::optional<Decimal> parseDecimal(const std::string& text, int maxSignificant, int maxFraction);

// `value` × numerator / denominator + offset, rounded half away from zero to
// `decimals`, as a plain decimal; nullopt where the value is not a plain
// decimal, the scale is not one this allows, or the result is out of bounds.
std::optional<std::string> scaleValue(const std::string& value, const Scale& scale);

// What a level publishes for the value set: the write format's text with the
// value - scaled first where the format scales - in place of `{value}`.
// nullopt where the value cannot be scaled: nothing is sent then.
std::optional<std::string> formatWrite(const std::string& value, const WriteFormat* format);

}  // namespace scaled
