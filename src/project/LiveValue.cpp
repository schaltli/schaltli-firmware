#include "project/LiveValue.h"
#include "project/LiveValueJson.h"

#include <cmath>
#include <cstdlib>

// The designer's lib/live-value.ts, line by line where it can be: no regular
// expression, no locale. Comments say only where C++ has to differ.

namespace livevalue {

static bool isWhitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool isDigit(char c) { return c >= '0' && c <= '9'; }

static std::string trimmed(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && isWhitespace(value[start])) start++;
    while (end > start && isWhitespace(value[end - 1])) end--;
    return value.substr(start, end - start);
}

static std::string lowerAscii(const std::string& value) {
    std::string out = value;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

std::optional<double> asNumber(const std::string& value) {
    const std::string text = trimmed(value);
    size_t i = 0;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) i++;
    int digits = 0;
    while (i < text.size() && isDigit(text[i])) {
        i++;
        digits++;
    }
    if (i < text.size() && text[i] == '.') {
        i++;
        while (i < text.size() && isDigit(text[i])) {
            i++;
            digits++;
        }
    }
    if (i != text.size() || digits == 0) return std::nullopt;
    // Checked to be a plain decimal above; strtod reads it as JS Number() does.
    return std::strtod(text.c_str(), nullptr);
}

bool isYes(const std::string& value) {
    const std::string word = lowerAscii(trimmed(value));
    if (word == "true" || word == "on" || word == "yes") return true;
    const auto number = asNumber(word);
    return number && *number != 0;
}

bool isNo(const std::string& value) {
    const std::string word = lowerAscii(trimmed(value));
    if (word.empty() || word == "false" || word == "off" || word == "no") return true;
    const auto number = asNumber(word);
    return number && *number == 0;
}

bool matches(const std::string& value, const std::string& op, const std::string& operand) {
    if (op == "yes") return isYes(value);
    if (op == "no") return isNo(value);
    if (op == "==") return trimmed(value) == trimmed(operand);
    if (op == "!=") return trimmed(value) != trimmed(operand);
    const auto a = asNumber(value);
    const auto b = asNumber(operand);
    if (!a || !b) return false;
    if (op == "<") return *a < *b;
    if (op == "<=") return *a <= *b;
    if (op == ">") return *a > *b;
    if (op == ">=") return *a >= *b;
    return false;
}

Applies evaluate(const LiveValue& liveValue, const std::optional<std::string>& value) {
    if (!value) return {Applies::NoValueYet, -1, liveValue.noValueYet ? &*liveValue.noValueYet : nullptr};
    for (size_t i = 0; i < liveValue.rules.size(); i++) {
        const Rule& rule = liveValue.rules[i];
        if (matches(*value, rule.op, rule.operand)) return {Applies::RuleBranch, static_cast<int>(i), &rule.result};
    }
    return {Applies::Otherwise, -1, liveValue.otherwise ? &*liveValue.otherwise : nullptr};
}

static std::string twoDigits(long long n) { return n < 10 ? "0" + std::to_string(n) : std::to_string(n); }

static std::optional<std::string> formatDuration(const std::string& value, const std::string& pattern) {
    const std::string text = trimmed(value);
    if (!asNumber(text)) return std::nullopt;
    size_t i = (!text.empty() && text[0] == '+') ? 1 : 0;
    if (i < text.size() && text[i] == '-') return std::nullopt;
    long long seconds = 0;
    while (i < text.size() && isDigit(text[i])) seconds = seconds * 10 + (text[i++] - '0');
    const long long h = seconds / 3600;
    const long long m = (seconds % 3600) / 60;
    const long long s = seconds % 60;
    if (pattern == "h:mm:ss") return std::to_string(h) + ":" + twoDigits(m) + ":" + twoDigits(s);
    if (pattern == "h:mm") return std::to_string(h) + ":" + twoDigits(m);
    return std::to_string(seconds / 60) + ":" + twoDigits(s);
}

std::string durationText(const std::string& value) {
    const auto formatted = formatDuration(value, "h:mm:ss");
    return formatted ? *formatted : value;
}

std::string formatValue(const std::string& value, const Format& format, const placeholders::Separators& separators) {
    if (format.kind == FormatKind::AsIs) return value;
    if (format.kind == FormatKind::Number) {
        const auto formatted = placeholders::formatNumber(value, {format.grouped ? 'N' : 'F', format.decimals}, separators);
        return formatted ? *formatted : value;
    }
    const auto formatted = formatDuration(value, format.pattern);
    return formatted ? *formatted : value;
}

std::string textOf(const LiveValue& liveValue, const std::optional<std::string>& value, const placeholders::Separators& separators) {
    const Applies applies = evaluate(liveValue, value);
    const std::string formatted = value ? formatValue(*value, liveValue.format, separators) : std::string();
    if (!applies.result) return applies.branch == Applies::Otherwise ? formatted : std::string();
    if (applies.result->kind != ResultKind::Text) return std::string();
    std::string out;
    for (const Part& part : applies.result->parts) out += part.isValue ? formatted : part.text;
    return out;
}

// The `<id>` of a `{live:<id>}` that starts at `at`, and where it ends.
static bool liveReferenceAt(const std::string& text, size_t at, std::string& id, size_t& end) {
    static const std::string head = "{live:";
    if (text.compare(at, head.size(), head) != 0) return false;
    const size_t close = text.find('}', at + head.size());
    if (close == std::string::npos) return false;
    id = text.substr(at + head.size(), close - at - head.size());
    for (char c : id)
        if (c == '{' || isWhitespace(c)) return false;
    end = close + 1;
    return !id.empty();
}

std::string resolveLiveText(const std::string& text, const std::vector<LiveValue>& liveValues, const Lookup& lookup,
                            const placeholders::Separators& separators) {
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if ((c == '{' || c == '}') && i + 1 < text.size() && text[i + 1] == c) {
            out += c;
            i += 2;
            continue;
        }
        std::string id;
        size_t end = 0;
        if (c == '{' && liveReferenceAt(text, i, id, end)) {
            const LiveValue* found = nullptr;
            for (const LiveValue& lv : liveValues)
                if (lv.id == id) found = &lv;
            if (found) {
                out += textOf(*found, lookup(found->source), separators);
                i = end;
                continue;
            }
        }
        out += c;
        i++;
    }
    return out;
}

Source parseSource(JsonObjectConst json) { return {json["namespace"] | "", json["path"] | ""}; }

static Result parseResult(JsonObjectConst json) {
    Result result;
    const std::string kind = json["kind"] | "text";
    if (kind == "icon") {
        result.kind = ResultKind::Icon;
        result.icon = json["icon"] | "";
        result.path = json["path"] | "";
        result.pathDark = json["pathDark"] | "";
        return result;
    }
    for (JsonVariantConst part : json["parts"].as<JsonArrayConst>()) {
        if (part.is<const char*>()) result.parts.push_back({false, part.as<std::string>()});
        else result.parts.push_back({true, ""});
    }
    return result;
}

LiveValue parseLiveValue(JsonObjectConst json) {
    LiveValue lv;
    lv.id = json["id"] | "";
    lv.source = parseSource(json["source"].as<JsonObjectConst>());
    JsonObjectConst format = json["format"].as<JsonObjectConst>();
    if (!format.isNull()) {
        const std::string kind = format["kind"] | "asIs";
        if (kind == "number") {
            lv.format.kind = FormatKind::Number;
            lv.format.decimals = format["decimals"] | 0;
            lv.format.grouped = format["grouped"] | false;
        } else if (kind == "duration") {
            lv.format.kind = FormatKind::Duration;
            lv.format.pattern = format["pattern"] | "h:mm:ss";
        }
    }
    for (JsonObjectConst rule : json["rules"].as<JsonArrayConst>()) {
        lv.rules.push_back({rule["op"] | "==", rule["operand"] | "", parseResult(rule["result"].as<JsonObjectConst>())});
    }
    if (json["otherwise"].is<JsonObjectConst>()) lv.otherwise = parseResult(json["otherwise"].as<JsonObjectConst>());
    if (json["noValueYet"].is<JsonObjectConst>()) lv.noValueYet = parseResult(json["noValueYet"].as<JsonObjectConst>());
    return lv;
}

std::vector<LiveValue> parseLiveValues(JsonArrayConst json) {
    std::vector<LiveValue> out;
    for (JsonObjectConst lv : json) out.push_back(parseLiveValue(lv));
    return out;
}

}  // namespace livevalue
