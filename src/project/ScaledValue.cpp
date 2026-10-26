#include "project/ScaledValue.h"
#include "project/LiveValueJson.h"

// The designer's lib/scaled-value.ts, line by line.

namespace scaled {

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static int64_t pow10(int n) {
    int64_t p = 1;
    for (int i = 0; i < n; i++) p *= 10;
    return p;
}

std::optional<Decimal> parseDecimal(const std::string& text, int maxSignificant, int maxFraction) {
    size_t start = 0;
    size_t end = text.size();
    while (start < end && text[start] == ' ') start++;
    while (end > start && text[end - 1] == ' ') end--;
    size_t i = start;
    bool negative = false;
    if (i < end && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        i++;
    }
    std::string digits;
    int places = 0;
    bool seenDigit = false;
    while (i < end && isDigit(text[i])) {
        digits += text[i++];
        seenDigit = true;
    }
    if (i < end && text[i] == '.') {
        i++;
        while (i < end && isDigit(text[i])) {
            digits += text[i++];
            places++;
            seenDigit = true;
        }
    }
    if (i != end || !seenDigit || places > maxFraction) return std::nullopt;
    size_t first = 0;
    while (first + 1 < digits.size() && digits[first] == '0') first++;
    if (static_cast<int>(digits.size() - first) > maxSignificant) return std::nullopt;
    int64_t units = 0;
    for (size_t k = first; k < digits.size(); k++) units = units * 10 + (digits[k] - '0');
    return Decimal{negative ? -units : units, places};
}

// An integer count of 10^-decimals as a plain decimal: 2150 at 2 is "21.50"; never "-0".
static std::string decimalText(int64_t scaled, int decimals) {
    const bool negative = scaled < 0;
    std::string digits = std::to_string(negative ? -scaled : scaled);
    while (static_cast<int>(digits.size()) <= decimals) digits = "0" + digits;
    const std::string whole = digits.substr(0, digits.size() - decimals);
    const std::string fraction = digits.substr(digits.size() - decimals);
    return (negative ? "-" : "") + whole + (decimals > 0 ? "." + fraction : "");
}

static std::optional<Decimal> validScale(const Scale& scale) {
    if (scale.denominator < 1 || scale.denominator > MAX_FACTOR || scale.numerator < -MAX_FACTOR || scale.numerator > MAX_FACTOR) return std::nullopt;
    if (scale.decimals < 0 || scale.decimals > MAX_DECIMALS) return std::nullopt;
    const auto offset = parseDecimal(scale.offset, 18, MAX_OFFSET_DIGITS);
    if (!offset || offset->places > scale.decimals) return std::nullopt;
    const int64_t whole = offset->units / pow10(offset->places);
    if (whole >= LIMIT || -whole >= LIMIT) return std::nullopt;
    return offset;
}

std::optional<std::string> scaleValue(const std::string& value, const Scale& scale) {
    const auto valid = validScale(scale);
    if (!valid) return std::nullopt;
    const auto parsed = parseDecimal(value, MAX_SIGNIFICANT_DIGITS, MAX_FRACTION_DIGITS);
    if (!parsed) return std::nullopt;
    const int k = scale.decimals;

    // value × n / d = X / D: X below 10^18, D below 10^15.
    const int64_t X = parsed->units * scale.numerator;
    const int64_t D = pow10(parsed->places) * scale.denominator;
    const bool negative = X < 0;
    const int64_t magnitude = negative ? -X : X;
    const int64_t whole = magnitude / D;
    if (whole >= LIMIT) return std::nullopt;
    // |X| / D × 10^k as Q + r / D, digit by digit: r stays below D, 10r below 10^16.
    int64_t Q = whole;
    int64_t r = magnitude % D;
    for (int i = 0; i < k; i++) {
        r *= 10;
        Q = Q * 10 + r / D;
        r %= D;
    }
    // The offset in the same units, then the whole as floor F plus rr / D, 0 ≤ rr < D.
    const int64_t offset = valid->units * pow10(k - valid->places);
    int64_t F;
    int64_t rr;
    if (!negative) {
        F = Q + offset;
        rr = r;
    } else if (r == 0) {
        F = offset - Q;
        rr = 0;
    } else {
        F = offset - Q - 1;
        rr = D - r;
    }
    // Half away from zero.
    int64_t rounded;
    if (F >= 0) rounded = F + (2 * rr >= D ? 1 : 0);
    else if (rr == 0) rounded = F;
    else rounded = -(-F - 1 + (2 * (D - rr) >= D ? 1 : 0));
    const int64_t limit = LIMIT * pow10(k);
    if (rounded >= limit || -rounded >= limit) return std::nullopt;
    return decimalText(rounded, k);
}

std::optional<std::string> formatWrite(const std::string& value, const WriteFormat* format) {
    if (!format) return value;
    std::string put = value;
    if (format->scaled) {
        const auto scaledValue = scaleValue(value, format->scale);
        if (!scaledValue) return std::nullopt;
        put = *scaledValue;
    }
    static const std::string marker = "{value}";
    std::string out;
    size_t i = 0;
    while (i < format->text.size()) {
        if (format->text.compare(i, marker.size(), marker) == 0) {
            out += put;
            i += marker.size();
        } else {
            out += format->text[i++];
        }
    }
    return out;
}

}  // namespace scaled

// From the export (LiveValueJson.h): a factor that is not a whole number
// leaves a scale no value can pass - the designer refuses it too.
namespace scaled {

static int64_t factor(JsonVariantConst json, int64_t absent, bool& broken) {
    if (json.isNull()) return absent;
    if (!json.is<int64_t>()) broken = true;
    return json.as<int64_t>();
}

Scale parseScale(JsonObjectConst json) {
    Scale scale;
    bool broken = false;
    scale.numerator = factor(json["numerator"], 1, broken);
    scale.denominator = factor(json["denominator"], 1, broken);
    // A denominator of 0 is one no scale passes.
    if (broken) scale.denominator = 0;
    scale.offset = json["offset"].is<const char*>() ? json["offset"].as<std::string>() : "0";
    scale.decimals = json["decimals"].is<int>() ? json["decimals"].as<int>() : (json["decimals"].isNull() ? 0 : -1);
    return scale;
}

WriteFormat parseWriteFormat(JsonObjectConst json) {
    WriteFormat format;
    format.text = json["text"] | "";
    format.scaled = !json["numerator"].isNull() || !json["denominator"].isNull() || !json["offset"].isNull();
    format.scale = parseScale(json);
    return format;
}

}  // namespace scaled
