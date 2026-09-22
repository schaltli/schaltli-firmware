// Just enough Arduino for src/project/SwitchShape.h and the three headers it
// leans on (ProjectTypes.h, MaterialColors.h, LevelShape.h) to compile on a
// desktop compiler.
//
// Those headers are deliberately free of hardware - they are arithmetic and
// String, nothing else - so this is the whole of what they borrow from the
// core. Only the String methods they actually call are here; anything else
// would be a promise this shim does not keep.
//
// No min/max macros, unlike tools/arc-raster/shim/Arduino.h: this side pulls in
// <string> and <algorithm>, and a macro called `max` breaks both. The headers
// under test write their clamps out as ternaries, so nothing asks for them.
#pragma once

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class String {
 public:
  String() {}
  String(const char* s) : s_(s == nullptr ? "" : s) {}
  String(const std::string& s) : s_(s) {}
  String(int v) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", v);
    s_ = buf;
  }

  unsigned int length() const { return (unsigned int)s_.size(); }
  bool isEmpty() const { return s_.empty(); }
  char charAt(unsigned int i) const { return i < s_.size() ? s_[i] : '\0'; }
  const char* c_str() const { return s_.c_str(); }

  // Arduino's trim() strips whitespace at both ends IN PLACE and returns
  // nothing - every caller in these headers relies on that.
  void trim() {
    size_t from = 0;
    while (from < s_.size() && isspace((unsigned char)s_[from])) from++;
    size_t to = s_.size();
    while (to > from && isspace((unsigned char)s_[to - 1])) to--;
    s_ = s_.substr(from, to - from);
  }
  void toLowerCase() {
    for (size_t i = 0; i < s_.size(); i++) s_[i] = (char)tolower((unsigned char)s_[i]);
  }
  bool equalsIgnoreCase(const String& other) const {
    if (s_.size() != other.s_.size()) return false;
    for (size_t i = 0; i < s_.size(); i++) {
      if (tolower((unsigned char)s_[i]) != tolower((unsigned char)other.s_[i])) return false;
    }
    return true;
  }

  bool operator==(const String& other) const { return s_ == other.s_; }
  bool operator!=(const String& other) const { return s_ != other.s_; }
  bool operator==(const char* other) const { return s_ == (other == nullptr ? "" : other); }
  bool operator!=(const char* other) const { return !(*this == other); }

  String operator+(const String& other) const { return String(s_ + other.s_); }
  String& operator+=(const String& other) {
    s_ += other.s_;
    return *this;
  }

  const std::string& std_str() const { return s_; }

 private:
  std::string s_;
};

inline String operator+(const char* a, const String& b) { return String(a) + b; }
