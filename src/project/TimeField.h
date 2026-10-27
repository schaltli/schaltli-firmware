#pragma once
// A time field: a topic's time of day in a field, and a new one set on a
// picker and sent to the write topic (designer tasks/ha-time-plan.md, an
// MQTT `time` of Home Assistant).
//
// Ported line by line from the designer's lib/time-field.ts: what is read
// as a time, how it is shown, what is sent, how a minute lands on the step.
// The field itself is drawn as a text field's (TextField.h).

#include <string>

namespace timefield {

constexpr int DEFAULT_STEP = 5;
constexpr const char* NO_TIME = "--:--";

struct TimeOfDay {
  int hour = 0;
  int minute = 0;
};

// One of 1, 5, 10, 15, 30; anything else is the default.
inline int step(int minuteStep) {
  switch (minuteStep) {
    case 1:
    case 5:
    case 10:
    case 15:
    case 30:
      return minuteStep;
    default:
      return DEFAULT_STEP;
  }
}

inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The digits at text[i], at most `max` of them: their value and where they end.
inline bool digitsAt(const std::string& text, size_t i, size_t max, int& value, size_t& end) {
  end = i;
  value = 0;
  while (end < text.size() && end - i < max && isDigit(text[end])) value = value * 10 + (text[end++] - '0');
  return end > i;
}

// The time of day in a reported value: H:MM or HH:MM, either with :SS and a
// fraction after it, or an ISO date and time whose time is kept as written.
// Blanks around it are ignored; anything else is no time (false).
inline bool parse(const std::string& value, TimeOfDay& out) {
  size_t a = 0, b = value.size();
  while (a < b && (value[a] == ' ' || value[a] == '\t' || value[a] == '\r' || value[a] == '\n')) a++;
  while (b > a && (value[b - 1] == ' ' || value[b - 1] == '\t' || value[b - 1] == '\r' || value[b - 1] == '\n')) b--;
  std::string text = value.substr(a, b - a);
  if (text.size() > 11 && text[4] == '-' && text[7] == '-' && (text[10] == 'T' || text[10] == 't' || text[10] == ' ')) {
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
      if (!isDigit(text[i])) return false;
    text = text.substr(11);
    for (size_t i = 0; i < text.size(); i++) {
      const char c = text[i];
      if (c == 'Z' || c == 'z' || c == '+' || c == '-') {
        text = text.substr(0, i);
        break;
      }
    }
  }
  int hour = 0, minute = 0, ignored = 0;
  size_t hourEnd = 0, minuteEnd = 0;
  if (!digitsAt(text, 0, 2, hour, hourEnd) || hourEnd >= text.size() || text[hourEnd] != ':') return false;
  if (!digitsAt(text, hourEnd + 1, 2, minute, minuteEnd) || minuteEnd - hourEnd - 1 != 2) return false;
  size_t end = minuteEnd;
  if (end < text.size() && text[end] == ':') {
    int second = 0;
    size_t secondEnd = 0;
    if (!digitsAt(text, end + 1, 2, second, secondEnd) || secondEnd - end - 1 != 2 || second > 59) return false;
    end = secondEnd;
    if (end < text.size() && text[end] == '.') {
      size_t fractionEnd = 0;
      if (!digitsAt(text, end + 1, 9, ignored, fractionEnd)) return false;
      end = fractionEnd;
    }
  }
  if (end != text.size() || hour > 23 || minute > 59) return false;
  out.hour = hour;
  out.minute = minute;
  return true;
}

// The field's text: 06:45, always 24 hours.
inline std::string format(const TimeOfDay& t) {
  char buf[6];
  buf[0] = char('0' + t.hour / 10);
  buf[1] = char('0' + t.hour % 10);
  buf[2] = ':';
  buf[3] = char('0' + t.minute / 10);
  buf[4] = char('0' + t.minute % 10);
  buf[5] = 0;
  return buf;
}

// A reported value as the field shows it: its time, or «--:--».
inline std::string shown(const std::string& value) {
  TimeOfDay t;
  return parse(value, t) ? format(t) : std::string(NO_TIME);
}

// A time put on the step to the nearest, half a step up: 06:58 on 5 is
// 07:00, 23:58 is 00:00.
inline TimeOfDay onStep(const TimeOfDay& t, int step) {
  const int total = t.hour * 60 + t.minute;
  const int rounded = ((2 * total + step) / (2 * step)) * step % 1440;
  TimeOfDay out;
  out.hour = rounded / 60;
  out.minute = rounded % 60;
  return out;
}

// What OK publishes: HH:MM:00, as Home Assistant's time.isoformat() writes it.
inline std::string payload(const TimeOfDay& t) { return format(t) + ":00"; }

}  // namespace timefield
