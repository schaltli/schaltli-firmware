#include "Placeholders.h"

// Ported from schaltli-designer lib/placeholders.ts, function by function
// and in the same order, so a change there is easy to find here. The
// comments that explain a rule live there; these say only where C++ has to
// do something the TypeScript gets for free.

namespace placeholders {

namespace {

bool isWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The fields v1 resolves. device:name and project:version are reserved and,
// like any unknown field, shown as written.
bool isField(const std::string& ns, const std::string& path) {
    if (ns == "device") return path == "model" || path == "id";
    if (ns == "project") return path == "name";
    return false;
}

bool isNamespace(const std::string& ns) { return ns == "topic" || ns == "device" || ns == "project"; }

// `F0`-`F9` or `N0`-`N9`, nothing else.
std::optional<NumberFormat> asFormat(const std::string& text) {
    if (text.size() != 2) return std::nullopt;
    if ((text[0] != 'F' && text[0] != 'N') || !isDigit(text[1])) return std::nullopt;
    return NumberFormat{text[0], text[1] - '0'};
}

Segment raw(const std::string& source) {
    Segment segment;
    segment.kind = SegmentKind::Raw;
    segment.text = source;
    return segment;
}

Segment parseBody(const std::string& body, const std::string& source) {
    if (!body.empty() && body[0] == '(') return raw(source);  // expressions are reserved for later

    size_t colon = body.find(':');
    if (colon == std::string::npos || colon == 0) return raw(source);  // unknown namespace
    std::string ns = body.substr(0, colon);
    if (!isNamespace(ns)) return raw(source);

    // The reference runs to the first whitespace; a path cannot contain any.
    size_t end = colon + 1;
    while (end < body.size() && !isWhitespace(body[end])) end++;
    std::string path = body.substr(colon + 1, end - colon - 1);
    std::string rest = body.substr(end);

    std::optional<std::string> fallback;
    std::optional<NumberFormat> format;

    if (!rest.empty()) {
        // Whitespace after the reference: only `?? fallback[:format]` is v1.
        size_t i = 0;
        while (i < rest.size() && isWhitespace(rest[i])) i++;
        if (rest.compare(i, 2, "??") != 0 || i + 2 >= rest.size() || !isWhitespace(rest[i + 2])) {
            return raw(source);  // operators are reserved for later
        }
        i += 2;
        while (i < rest.size() && isWhitespace(rest[i])) i++;
        if (i < rest.size() && rest[i] == '"') {
            size_t close = rest.find('"', i + 1);
            if (close == std::string::npos) return raw(source);  // unterminated text
            fallback = rest.substr(i + 1, close - i - 1);
            i = close + 1;
        } else {
            size_t start = i;
            if (i < rest.size() && rest[i] == '-') i++;
            while (i < rest.size() && (isDigit(rest[i]) || rest[i] == '.')) i++;
            fallback = rest.substr(start, i - start);
            if (*fallback == "" || *fallback == "-") return raw(source);  // a fallback is a number or quoted text
        }
        std::string tail = rest.substr(i);
        if (!tail.empty()) {
            if (tail[0] != ':') return raw(source);  // operators are reserved for later
            format = asFormat(tail.substr(1));
            if (!format) return raw(source);  // unknown format
        }
    } else {
        // No fallback: a format can only be the part after the path's last
        // `:`, and only if it is one; otherwise that `:` belongs to the topic.
        size_t last = path.rfind(':');
        if (last != std::string::npos && last > 0) {
            std::optional<NumberFormat> candidate = asFormat(path.substr(last + 1));
            if (candidate) {
                format = candidate;
                path = path.substr(0, last);
            }
        }
    }

    if (path.empty()) return raw(source);  // nothing after the namespace
    if (ns != "topic" && !isField(ns, path)) return raw(source);  // unknown field

    Segment segment;
    segment.kind = SegmentKind::Placeholder;
    segment.text = source;
    segment.reference = Reference{ns, path};
    segment.fallback = fallback;
    segment.format = format;
    return segment;
}

std::string trim(const std::string& value) {
    const char* space = " \t\n\r\f\v";
    size_t first = value.find_first_not_of(space);
    if (first == std::string::npos) return "";
    size_t last = value.find_last_not_of(space);
    return value.substr(first, last - first + 1);
}

}  // namespace

std::vector<Segment> parse(const std::string& text) {
    std::vector<Segment> segments;
    std::string literal;
    auto flush = [&]() {
        if (!literal.empty()) {
            Segment segment;
            segment.kind = SegmentKind::Literal;
            segment.text = literal;
            segments.push_back(segment);
        }
        literal.clear();
    };

    const size_t n = text.size();
    size_t i = 0;
    while (i < n) {
        char c = text[i];
        if (c == '{' && i + 1 < n && text[i + 1] == '{') {
            literal += '{';
            i += 2;
            continue;
        }
        if (c == '}' && i + 1 < n && text[i + 1] == '}') {
            literal += '}';
            i += 2;
            continue;
        }
        if (c != '{') {
            // A lone `}` is not a placeholder of anything; it stays as written.
            literal += c;
            i++;
            continue;
        }
        // Find the closing brace, skipping any inside a quoted fallback.
        size_t j = i + 1;
        bool quoted = false;
        while (j < n && (quoted || text[j] != '}')) {
            if (text[j] == '"') quoted = !quoted;
            j++;
        }
        if (j >= n) {
            // Unterminated: the rest of the text, as written.
            flush();
            segments.push_back(raw(text.substr(i)));
            return segments;
        }
        flush();
        segments.push_back(parseBody(text.substr(i + 1, j - i - 1), text.substr(i, j - i + 1)));
        i = j + 1;
    }
    flush();
    return segments;
}

std::optional<std::string> formatNumber(const std::string& value, NumberFormat format, const Separators& separators) {
    const std::string text = trim(value);
    const size_t n = text.size();
    const size_t places = static_cast<size_t>(format.digits);
    size_t i = 0;
    bool negative = false;
    if (i < n && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        i++;
    }
    std::string whole;
    while (i < n && isDigit(text[i])) whole += text[i++];
    std::string fraction;
    if (i < n && text[i] == '.') {
        i++;
        while (i < n && isDigit(text[i])) fraction += text[i++];
    }
    if (i != n || (whole.empty() && fraction.empty())) return std::nullopt;

    // All the kept digits as one string, then +1 on it if the first dropped
    // digit is 5 or more.
    std::string kept = fraction.substr(0, places);
    kept.append(places - kept.size(), '0');
    std::string digits = (whole.empty() ? "0" : whole) + kept;
    char dropped = fraction.size() > places ? fraction[places] : '0';
    if (dropped >= '5') {
        long k = static_cast<long>(digits.size()) - 1;
        while (k >= 0) {
            if (digits[k] == '9') {
                digits[k] = '0';
                k--;
            } else {
                digits[k]++;
                break;
            }
        }
        if (k < 0) digits.insert(digits.begin(), '1');
    }

    std::string intPart = digits.substr(0, digits.size() - places);
    // Leading zeros off, but never the last digit (the TypeScript's
    // /^0+(?=\d)/).
    size_t zeros = 0;
    while (zeros + 1 < intPart.size() && intPart[zeros] == '0') zeros++;
    intPart = intPart.substr(zeros);
    if (intPart.empty()) intPart = "0";
    const std::string fracPart = digits.substr(digits.size() - places);

    if (format.kind == 'N' && !separators.thousands.empty()) {
        std::string grouped;
        for (size_t k = 0; k < intPart.size(); k++) {
            if (k > 0 && (intPart.size() - k) % 3 == 0) grouped += separators.thousands;
            grouped += intPart[k];
        }
        intPart = grouped;
    }

    // -0.001 at F2 is 0.00, not -0.00: a sign on nothing reads like a fault.
    const bool isZero = digits.find_first_not_of('0') == std::string::npos;
    const std::string sign = negative && !isZero ? "-" : "";
    return sign + intPart + (places > 0 ? separators.decimal + fracPart : "");
}

std::string resolve(const std::string& text, const Lookup& lookup, const Separators& separators) {
    std::string out;
    for (const Segment& segment : parse(text)) {
        if (segment.kind != SegmentKind::Placeholder) {
            out += segment.text;
            continue;
        }
        std::optional<std::string> found = lookup(segment.reference);
        const std::string value = found ? *found : segment.fallback ? *segment.fallback : "";
        std::optional<std::string> formatted;
        if (segment.format) formatted = formatNumber(value, *segment.format, separators);
        out += formatted ? *formatted : value;
    }
    return out;
}

std::vector<std::string> referencedTopics(const std::string& text) {
    std::vector<std::string> topics;
    for (const Segment& segment : parse(text)) {
        if (segment.kind != SegmentKind::Placeholder || segment.reference.ns != "topic") continue;
        bool seen = false;
        for (const std::string& topic : topics) seen = seen || topic == segment.reference.path;
        if (!seen) topics.push_back(segment.reference.path);
    }
    return topics;
}

}  // namespace placeholders
