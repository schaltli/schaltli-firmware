#pragma once
#include <Arduino.h>
#include <vector>

// Where a screen render's time actually goes, per phase and per object type.
//
// Why (2026-09-02): a full render was recorded as costing 782ms and that
// number then shaped a decision - no finger-tracking swipe on the 4.3B,
// because nothing can be redrawn per frame at that speed. The number is real
// but it had never been broken down, so "the renderer is slow" was as far as
// anyone could get. Two very different causes are both plausible from the
// code: icons are blitted pixel by pixel through a virtual drawPixel into
// PSRAM, and icons are also read from LittleFS and decoded on a cache miss.
// One is fixed by writing rows instead of pixels, the other by not reading
// the file again - and guessing which would have been the third guess in two
// days that turned out wrong.
//
// The measured floor for touching every pixel of an 800x480 frame once is
// 25ms (PanelBenchmark.h). That is the number this is being compared against.
//
// Cost when disabled: one branch per object. Enabled: one micros() pair per
// object plus a string compare. Both are noise against anything measured
// here, and it is off unless a diagnosis endpoint turns it on.
namespace RenderProfile {

struct Bucket {
  String name;
  uint32_t us = 0;
  uint32_t count = 0;
};

// Definition lives here rather than in a .cpp so no board's build_src_filter
// has to learn about a new file - src/project/ is compiled whole by both
// envs, but a header-only unit cannot be left out by accident.
inline bool &enabledFlag()
{
  static bool enabled = false;
  return enabled;
}

inline std::vector<Bucket> &buckets()
{
  static std::vector<Bucket> b;
  return b;
}

inline bool enabled() { return enabledFlag(); }
inline void setEnabled(bool on) { enabledFlag() = on; }

inline void reset() { buckets().clear(); }

inline void add(const String &name, uint32_t us)
{
  if (!enabledFlag()) return;
  for (auto &b : buckets())
  {
    if (b.name == name)
    {
      b.us += us;
      b.count++;
      return;
    }
  }
  Bucket fresh;
  fresh.name = name;
  fresh.us = us;
  fresh.count = 1;
  buckets().push_back(fresh);
}

// Scoped timer. Deliberately reads micros() even when disabled is false only
// at construction - the branch in add() does the rest - so enabling it mid
// render cannot produce a half-measured bucket.
class Scope {
public:
  explicit Scope(const String &name) : name_(name), start_(micros()) {}
  ~Scope() { add(name_, micros() - start_); }

private:
  String name_;
  uint32_t start_;
};

// Sorted slowest first, because that is the only order anyone reads this in.
inline String report()
{
  std::vector<Bucket> sorted = buckets();
  for (size_t i = 0; i < sorted.size(); i++)
    for (size_t j = i + 1; j < sorted.size(); j++)
      if (sorted[j].us > sorted[i].us) std::swap(sorted[i], sorted[j]);

  uint32_t total = 0;
  for (const auto &b : sorted) total += b.us;

  String out;
  for (const auto &b : sorted)
  {
    // Nested buckets (asset work inside an object's own time) are indented
    // by their caller and must not be added into the percentage twice - the
    // total above therefore over-counts, so percentages are against the
    // caller-supplied total instead. Left to the caller to print.
    String label = b.name;
    while (label.length() < 26) label += " ";
    out += "    " + label + String(b.us / 1000.0f, 1) + " ms";
    if (b.count > 1) out += "  x" + String(b.count);
    out += "\n";
  }
  return out;
}

} // namespace RenderProfile
