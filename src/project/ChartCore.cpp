#include "ChartCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace chartcore {

// ---- small things the TypeScript has for free

// Math.round: half up, towards +infinity.
static double jsRound(double x) { return std::floor(x + 0.5); }
static double round1(double n) { return jsRound(n * 10) / 10; }

static std::string pad2(long v) {
  char b[24];
  std::snprintf(b, sizeof b, "%02ld", v);
  return b;
}

// Days since 1970-01-01 for a civil date, and back (H. Hinnant's algorithms).
static long daysFromCivil(long y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

static std::string civilFromDays(long z) {
  z += 719468;
  const long era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long y = static_cast<long>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned d = doy - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
  char b[16];
  std::snprintf(b, sizeof b, "%04ld-%02u-%02u", y, m, d);
  return b;
}

static long dayNumber(const std::string& date) {
  long y = std::strtol(date.c_str(), nullptr, 10);
  unsigned m = static_cast<unsigned>(std::strtol(date.c_str() + 5, nullptr, 10));
  unsigned d = static_cast<unsigned>(std::strtol(date.c_str() + 8, nullptr, 10));
  return daysFromCivil(y, m, d);
}

std::string addDays(const std::string& date, int n) { return civilFromDays(dayNumber(date) + n); }

int daysBetween(const std::string& a, const std::string& b) { return static_cast<int>(dayNumber(b) - dayNumber(a)); }

std::string dateOf(double t, const Calendar& cal) {
  const auto& days = cal.days;
  if (days.empty()) return "1970-01-01";
  size_t at = 0;
  for (size_t i = 0; i < days.size(); i++)
    if (days[i].start <= t) at = i;
  const Day& base = days[at];
  const int off = static_cast<int>(std::floor((t - base.start) / 86400));
  return off == 0 || (at > 0 && at < days.size() - 1) ? base.date : addDays(base.date, off);
}

double midnightOf(const std::string& date, const Calendar& cal) {
  const auto& days = cal.days;
  for (const Day& d : days)
    if (d.date == date) return d.start;
  if (days.empty()) return 0;
  const Day& near = daysBetween(days.front().date, date) < 0 ? days.front() : days.back();
  return near.start + daysBetween(near.date, date) * 86400.0;
}

std::string weekdayOf(const std::string& date) {
  static const char* names[] = {"Do", "Fr", "Sa", "So", "Mo", "Di", "Mi"};  // 1970-01-01 was a Thursday
  long n = dayNumber(date) % 7;
  if (n < 0) n += 7;
  return names[n];
}

bool Series::annotated(const char* a) const { return std::find(annotations.begin(), annotations.end(), a) != annotations.end(); }

// ---- the chart, from JSON

static Axis axisFrom(JsonObjectConst a) {
  Axis out;
  if (a.isNull()) return out;
  out.scale = a["scale"] | "auto";
  if (a["min"].is<double>()) {
    out.hasMin = true;
    out.min = a["min"].as<double>();
  }
  if (a["max"].is<double>()) {
    out.hasMax = true;
    out.max = a["max"].as<double>();
  }
  out.unit = a["unit"] | "";
  return out;
}

Chart chartFrom(JsonObjectConst p) {
  Chart c;
  c.mode = p["mode"] | "time";
  if (p["days"].is<int>()) {
    c.hasDays = true;
    c.days = p["days"].as<int>();
  }
  c.scroll = p["scroll"] | false;
  c.left = axisFrom(p["left"]);
  if (!p["right"].isNull()) {
    c.hasRight = true;
    c.right = axisFrom(p["right"]);
  }
  for (JsonObjectConst z : p["zones"].as<JsonArrayConst>()) {
    Zone zone;
    zone.axis = z["axis"] | "left";
    if (z["from"].is<double>()) {
      zone.hasFrom = true;
      zone.from = z["from"].as<double>();
    }
    if (z["to"].is<double>()) {
      zone.hasTo = true;
      zone.to = z["to"].as<double>();
    }
    zone.color = z["color"] | "";
    zone.colorDark = z["colorDark"] | "";
    c.zones.push_back(zone);
  }
  c.legend = p["legend"] | "top-right";
  for (JsonObjectConst s : p["series"].as<JsonArrayConst>()) {
    Series out;
    out.id = s["id"] | "";
    out.balance = std::string(s["source"]["kind"] | "topic") == "balance";
    out.topic = s["source"]["topic"] | "";
    out.name = s["name"] | "";
    out.valueKind = s["valueKind"] | "level";
    out.invert = s["invert"] | false;
    if (s["factor"].is<double>()) {
      out.hasFactor = true;
      out.factor = s["factor"].as<double>();
    }
    out.unit = s["unit"] | "";
    if (s["decimals"].is<int>()) {
      out.hasDecimals = true;
      out.decimals = s["decimals"].as<int>();
    }
    out.draw = s["draw"] | "line";
    out.stack = s["stack"] | "";
    out.axis = s["axis"] | "left";
    out.color = s["color"] | "";
    out.colorDark = s["colorDark"] | "";
    out.range = s["range"] | false;
    out.dailyValue = s["dailyValue"] | "";
    for (JsonVariantConst a : s["annotations"].as<JsonArrayConst>()) out.annotations.push_back(a.as<std::string>());
    c.series.push_back(out);
  }
  return c;
}

// ---- the window

static Window baseWindow(const Chart& chart, const std::string& range, double now, const Calendar& cal) {
  const std::string today = dateOf(now, cal);
  Window w;
  if (chart.mode == "per-day") {
    const int n = std::max(1, chart.hasDays ? chart.days : 7);
    for (int i = 0; i < n; i++) w.dates.push_back(addDays(today, i - n + 1));
    w.from = midnightOf(w.dates.front(), cal);
    w.to = midnightOf(addDays(today, 1), cal);
    w.source = "daily";
    w.step = 86400;
    return w;
  }
  if (range == "1h" || range == "6h") {
    const double step = range == "1h" ? 60 : 300;
    w.to = std::floor(now / step) * step + step;
    w.from = w.to - (range == "1h" ? 3600 : 6 * 3600);
    w.source = range == "1h" ? "recent" : "recent6";
    w.step = step;
    return w;
  }
  const int n = range == "7d" ? 7 : 1;
  for (int i = 0; i < n; i++) w.dates.push_back(addDays(today, i - n + 1));
  w.from = midnightOf(w.dates.front(), cal);
  w.to = midnightOf(addDays(today, 1), cal);
  w.source = "day";
  w.step = 900;
  return w;
}

Window chartWindow(const Chart& chart, const std::string& range, double now, const Calendar& cal, double back) {
  Window base = baseWindow(chart, range, now, cal);
  if (back <= 0) return base;
  const double from = base.from - back;
  const double to = base.to - back;
  if (base.source == "daily") {
    const int shift = static_cast<int>(jsRound(back / 86400));
    Window w = base;
    w.from = from;
    w.to = to;
    for (std::string& d : w.dates) d = addDays(d, -shift);
    return w;
  }
  const double kept = base.source == "recent" ? 2 * 3600 : base.source == "recent6" ? 6 * 3600 : 0;
  if (now - from <= kept) {
    base.from = from;
    base.to = to;
    return base;
  }
  Window w;
  w.from = from;
  w.to = to;
  w.source = "day";
  w.step = 900;
  for (std::string d = dateOf(from, cal); midnightOf(d, cal) < to; d = addDays(d, 1)) w.dates.push_back(d);
  return w;
}

std::string historyName(const std::string& topic) {
  std::string out = topic;
  const size_t at = out.find('#');
  if (at != std::string::npos) out[at] = '~';
  return out;
}

std::vector<std::string> historyTopicsFor(const Chart& chart, const Window& window) {
  std::vector<std::string> out;
  auto add = [&out](const std::string& t) {
    if (std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
  };
  for (const Series& s : chart.series) {
    if (s.balance || s.topic.empty()) continue;
    const std::string base = "schaltli/history/" + historyName(s.topic);
    if (window.source == "day")
      for (const std::string& d : window.dates) add(base + "/day/" + d);
    else add(base + "/" + window.source);
  }
  return out;
}

// ---- a screen's range

static const char* RANGES[] = {"1h", "6h", "24h", "7d"};

static int rangeIndex(const std::string& r) {
  for (int i = 0; i < 4; i++)
    if (r == RANGES[i]) return i;
  return 2;
}

double spanOf(const std::string& range) {
  if (range == "1h") return 3600;
  if (range == "6h") return 6 * 3600;
  if (range == "7d") return 7 * 86400;
  return 86400;
}

RangeView settled(const RangeView& v, double now, const Calendar& cal) {
  RangeView out = v;
  if (v.back <= 0) {
    out.back = 0;
    return out;
  }
  if (v.range != "24h" && v.range != "7d") return v;
  const double todayStart = midnightOf(dateOf(now, cal), cal) - (v.range == "7d" ? 6 * 86400 : 0);
  const double from = todayStart - v.back;
  const double before = midnightOf(dateOf(from, cal), cal);
  const double after = midnightOf(dateOf(from + 86400, cal), cal);
  const double near = from - before <= after - from ? before : after;
  if (std::fabs(from - near) > 6 * 3600) return v;
  out.back = std::max(0.0, todayStart - near);
  return out;
}

RangeView narrowed(const RangeView& v, double now, const Calendar& cal) {
  RangeView out = v;
  out.range = RANGES[std::max(0, rangeIndex(v.range) - 1)];
  return settled(out, now, cal);
}

RangeView widened(const RangeView& v, double now, const Calendar& cal) {
  RangeView out = v;
  out.range = RANGES[std::min(3, rangeIndex(v.range) + 1)];
  return settled(out, now, cal);
}

RangeView moved(const RangeView& v, bool earlier, double now, const Calendar& cal) {
  RangeView out = v;
  out.back = earlier ? v.back + spanOf(v.range) : std::max(0.0, v.back - spanOf(v.range));
  return settled(out, now, cal);
}

RangeView setRange(const std::string& range) {
  RangeView out;
  out.range = range;
  out.back = 0;
  return out;
}

RangeView dragged(const RangeView& v, double seconds) {
  RangeView out = v;
  out.back = std::max(0.0, v.back + seconds);
  return out;
}

// ---- a series' history

static std::string dailyValueOf(const Series& s) {
  if (!s.dailyValue.empty()) return s.dailyValue;
  return s.valueKind == "power" ? "energy" : s.valueKind == "state" ? "runtime" : "mean";
}

SeriesData seriesData(const Series& s, const std::vector<JsonObjectConst>& payloads, const Calendar& cal) {
  SeriesData out;
  const double factor = s.hasFactor ? s.factor : 1;
  auto scale = [&](double v) { return s.invert ? -v * factor : v * factor; };
  for (JsonObjectConst p : payloads) {
    if (p.isNull()) continue;
    if (p["days"].is<JsonArrayConst>()) {
      const std::string which = dailyValueOf(s);
      for (JsonObjectConst d : p["days"].as<JsonArrayConst>()) {
        DayBar bar;
        bar.date = d["date"] | "";
        if (which == "energy") {
          const bool hasIn = d["energyIn"].is<double>(), hasOut = d["energyOut"].is<double>();
          const double in = hasIn ? d["energyIn"].as<double>() * factor : 0;
          const double outE = hasOut ? -d["energyOut"].as<double>() * factor : 0;
          if (s.invert) {
            bar.hasUp = hasOut;
            bar.up = -outE;
            bar.hasDown = hasIn;
            bar.down = -in;
          } else {
            bar.hasUp = hasIn;
            bar.up = in;
            bar.hasDown = hasOut;
            bar.down = outE;
          }
        } else if (which == "runtime") {
          bar.hasUp = d["runtime"].is<double>();
          bar.up = bar.hasUp ? d["runtime"].as<double>() / 60 : 0;
        } else {
          bar.hasUp = d[which].is<double>();
          bar.up = bar.hasUp ? d[which].as<double>() * factor : 0;
        }
        out.days.push_back(bar);
      }
      continue;
    }
    if (s.valueKind == "state") {
      const bool minutes = p["date"].is<const char*>();
      const double base = minutes ? midnightOf(p["date"].as<std::string>(), cal) : 0;
      for (JsonArrayConst pair : p["on"].as<JsonArrayConst>()) {
        const double a = pair[0].as<double>(), b = pair[1].as<double>();
        out.on.push_back(minutes ? std::make_pair(base + a * 60, base + b * 60) : std::make_pair(a, b));
      }
      continue;
    }
    const double len = p["step"] | 900.0;
    JsonArrayConst mean = p["mean"].as<JsonArrayConst>();
    const size_t n = mean.isNull() ? 0 : mean.size();
    const double start = p["date"].is<const char*>() ? midnightOf(p["date"].as<std::string>(), cal) : (p["end"] | 0.0) - n * len;
    JsonArrayConst mins = p["min"].as<JsonArrayConst>();
    JsonArrayConst maxs = p["max"].as<JsonArrayConst>();
    for (size_t i = 0; i < n; i++) {
      if (!mean[i].is<double>()) continue;
      const double m = mean[i].as<double>();
      const double lo = scale(!mins.isNull() && i < mins.size() && mins[i].is<double>() ? mins[i].as<double>() : m);
      const double hi = scale(!maxs.isNull() && i < maxs.size() && maxs[i].is<double>() ? maxs[i].as<double>() : m);
      Step st;
      st.t = start + i * len;
      st.len = len;
      st.mean = scale(m);
      st.min = std::min(lo, hi);
      st.max = std::max(lo, hi);
      out.steps.push_back(st);
    }
  }
  std::stable_sort(out.steps.begin(), out.steps.end(), [](const Step& a, const Step& b) { return a.t < b.t; });
  std::stable_sort(out.on.begin(), out.on.end(), [](const std::pair<double, double>& a, const std::pair<double, double>& b) { return a.first < b.first; });
  return out;
}

// ---- numbers and texts

double niceStep(double span, int count) {
  if (!(span > 0)) return 1;
  const double raw = span / count;
  const double power = std::pow(10, std::floor(std::log10(raw)));
  const double f = raw / power;
  return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 2.5 ? 2.5 : f <= 5 ? 5 : 10) * power;
}

// Number.prototype.toFixed: a tie goes to the larger magnitude, the sign kept.
std::string fixed(double v, int decimals) {
  const double scaled = std::fabs(v) * std::pow(10, decimals);
  double n = std::floor(scaled);
  if (scaled - n >= 0.5) n += 1;
  const long long whole = static_cast<long long>(n);
  long long div = 1;
  for (int i = 0; i < decimals; i++) div *= 10;
  std::string out = std::to_string(whole / div);
  if (decimals > 0) {
    std::string frac = std::to_string(whole % div);
    while (static_cast<int>(frac.size()) < decimals) frac = "0" + frac;
    out += "." + frac;
  }
  return v < 0 ? "-" + out : out;
}

static std::string withDecimal(std::string text, const std::string& decimal) {
  if (decimal == ".") return text;
  const size_t at = text.find('.');
  if (at != std::string::npos) text.replace(at, 1, decimal);
  return text;
}

std::string axisLabel(double value, double step, const std::string& decimal) {
  int decimals = 0;
  while (decimals < 4 && std::fabs(jsRound(step * std::pow(10, decimals)) - step * std::pow(10, decimals)) > 1e-6) decimals++;
  return withDecimal(fixed(std::fabs(value) < step / 1e6 ? 0 : value, decimals), decimal);
}

std::string valueText(double v, const Series& s, const std::string& decimal) {
  const int decimals = s.hasDecimals ? s.decimals : (std::fabs(v) >= 100 ? 0 : 1);
  const std::string text = withDecimal(fixed(v, decimals), decimal);
  return s.unit.empty() ? text : text + " " + s.unit;
}

std::string energyText(double wh, const std::string& decimal) {
  if (std::fabs(wh) >= 1000) return withDecimal(fixed(wh / 1000, 1), decimal) + " kWh";
  char b[32];
  std::snprintf(b, sizeof b, "%.0f Wh", jsRound(wh));
  return b;
}

std::string runtimeText(double minutes) {
  const long m = static_cast<long>(jsRound(minutes));
  return std::to_string(m / 60) + ":" + pad2(m % 60) + " h";
}

static std::string clockText(double t, const Calendar& cal) {
  const double m = midnightOf(dateOf(t, cal), cal);
  const long hh = static_cast<long>(std::floor((t - m) / 3600));
  const long mm = static_cast<long>(std::floor(std::fmod(t - m, 3600) / 60));
  return pad2(hh) + ":" + pad2(mm);
}

// ---- the layout

struct Range {
  double min = 0, max = 1;
};

static Range axisRange(const Axis& axis, const std::vector<double>& values) {
  const double lo = values.empty() ? 0 : *std::min_element(values.begin(), values.end());
  const double hi = values.empty() ? 1 : *std::max_element(values.begin(), values.end());
  if (axis.scale == "fixed" && axis.hasMin && axis.hasMax && axis.max > axis.min) return {axis.min, axis.max};
  if (axis.scale == "at-least" && axis.hasMin && axis.hasMax) return {std::min(axis.min, lo), std::max(axis.max, hi)};
  double min = lo >= 0 ? 0 : lo;
  double max = std::max(hi, 0.0);
  if (max <= min) max = min + 1;
  const double step = niceStep(max - min);
  min = std::floor(min / step) * step;
  max = std::ceil(max / step) * step;
  return {min, max};
}

static AxisLayout axisLayout(const Range& r, const Box& plot, const std::string& decimal) {
  AxisLayout out;
  out.min = r.min;
  out.max = r.max;
  const double step = niceStep(r.max - r.min);
  const double first = std::ceil(r.min / step - 1e-9) * step;
  for (double v = first; v <= r.max + step * 1e-6; v += step) {
    AxisTick t;
    t.value = v;
    t.y = round1(plot.y + plot.h - ((v - r.min) / (r.max - r.min)) * plot.h);
    t.label = axisLabel(v, step, decimal);
    out.ticks.push_back(t);
  }
  return out;
}

struct Slice {
  double t = 0, len = 0, lo = 0, hi = 0;
};

static std::vector<std::vector<Slice>> stackSlices(const Chart& chart, const std::vector<SeriesData>& data) {
  std::vector<std::vector<Slice>> out(chart.series.size());
  std::vector<std::string> keys;
  for (const Series& s : chart.series)
    if (s.draw == "stacked" && std::find(keys.begin(), keys.end(), s.stack) == keys.end()) keys.push_back(s.stack);
  for (const std::string& key : keys) {
    std::vector<size_t> members;
    for (size_t i = 0; i < chart.series.size(); i++)
      if (chart.series[i].draw == "stacked" && chart.series[i].stack == key) members.push_back(i);
    std::map<double, std::pair<double, std::map<size_t, double>>> byT;
    for (size_t i : members) {
      if (chart.series[i].balance) continue;
      for (const Step& st : data[i].steps) {
        auto& e = byT[st.t];
        if (e.second.empty()) e.first = st.len;
        e.second[i] = st.mean;
      }
    }
    for (auto& entry : byT) {
      const double t = entry.first, len = entry.second.first;
      const auto& values = entry.second.second;
      double up = 0, down = 0;
      for (size_t i : members) {
        bool has = false;
        double v = 0;
        if (chart.series[i].balance) {
          bool all = true;
          double sum = 0;
          for (size_t m : members) {
            if (chart.series[m].balance) continue;
            auto it = values.find(m);
            if (it == values.end()) {
              all = false;
              break;
            }
            sum += it->second;
          }
          if (all) {
            has = true;
            v = -sum;
          }
        } else {
          auto it = values.find(i);
          if (it != values.end()) {
            has = true;
            v = it->second;
          }
        }
        if (!has) continue;
        if (v >= 0) {
          out[i].push_back({t, len, up, up + v});
          up += v;
        } else {
          out[i].push_back({t, len, down + v, down});
          down += v;
        }
      }
    }
  }
  return out;
}

static std::vector<std::string> figures(const Series& s, const SeriesData& d, const Window& w, const LayoutOptions& o) {
  std::vector<std::string> out;
  if (s.annotated("current") && s.valueKind != "state") {
    for (auto it = d.steps.rbegin(); it != d.steps.rend(); ++it) {
      if (it->t <= o.now) {
        out.push_back("jetzt " + valueText(it->mean, s, o.decimal));
        break;
      }
    }
  }
  if (s.annotated("sum") && s.valueKind == "power") {
    double inWh = 0, outWh = 0;
    for (const Step& st : d.steps) {
      const double a = std::max(st.t, w.from);
      const double b = std::min(std::min(st.t + st.len, w.to), o.now);
      if (b <= a) continue;
      const double wh = st.mean * (b - a) / 3600;
      if (wh >= 0) inWh += wh;
      else outWh -= wh;
    }
    const std::string text = outWh > 0 ? "+" + energyText(inWh, o.decimal) + " / −" + energyText(outWh, o.decimal) : energyText(inWh, o.decimal);
    const bool snapped = w.source == "day" || w.source == "daily";
    out.push_back(snapped ? text : text + " (" + clockText(w.from, o.cal) + " – " + clockText(std::min(w.to, o.now), o.cal) + ")");
  }
  if (s.valueKind == "state" && (s.annotated("runtime") || s.annotated("starts"))) {
    double minutes = 0;
    int starts = 0;
    for (const auto& p : d.on) {
      const double lo = std::max(p.first, w.from), hi = std::min(p.second, w.to);
      if (hi > lo) minutes += (hi - lo) / 60;
      if (p.first >= w.from && p.first < w.to) starts++;
    }
    if (s.annotated("runtime")) out.push_back(runtimeText(minutes));
    if (s.annotated("starts")) out.push_back(std::to_string(starts) + "×");
  }
  return out;
}

Layout chartLayout(const Chart& chart, const Window& w, const std::vector<SeriesData>& data, const LayoutOptions& o) {
  Layout L;
  const std::string& decimal = o.decimal;
  const double pad = jsRound(o.fontSize / 3);
  const double labelW = jsRound(o.fontSize * 2.6);
  bool usesRight = false;
  for (const Series& s : chart.series)
    if (s.axis == "right") usesRight = true;
  const double bottom = o.fontSize + pad * 2;
  const double rowH = o.fontSize + pad;
  const double legendBelow = chart.legend == "bottom" ? rowH * std::max<size_t>(1, chart.series.size()) : 0;
  Box& plot = L.plot;
  plot.x = labelW + pad;
  plot.y = pad + jsRound(o.fontSize / 2);
  plot.w = std::max(1.0, o.width - labelW - pad * 2 - (usesRight ? labelW : 0));
  plot.h = std::max(1.0, o.height - bottom - legendBelow - pad - jsRound(o.fontSize / 2));
  const bool perDay = chart.mode == "per-day";
  const double span = w.to - w.from;
  auto xOf = [&](double t) { return round1(plot.x + ((t - w.from) / span) * plot.w); };
  auto inWindow = [&](double t, double len) { return t + len > w.from && t < w.to; };
  const std::vector<std::vector<Slice>> stacks = perDay ? std::vector<std::vector<Slice>>(chart.series.size()) : stackSlices(chart, data);
  auto hasDate = [&](const std::string& d) { return std::find(w.dates.begin(), w.dates.end(), d) != w.dates.end(); };

  auto valuesOn = [&](const std::string& axis) {
    std::vector<double> v;
    for (size_t i = 0; i < chart.series.size(); i++) {
      const Series& s = chart.series[i];
      if (s.axis != axis || (!perDay && s.valueKind == "state")) continue;
      if (perDay) {
        for (const DayBar& d : data[i].days) {
          if (!hasDate(d.date)) continue;
          if (d.hasUp) v.push_back(d.up);
          if (d.hasDown) v.push_back(d.down);
        }
      } else if (s.draw == "stacked") {
        for (const Slice& x : stacks[i])
          if (inWindow(x.t, x.len)) {
            v.push_back(x.lo);
            v.push_back(x.hi);
          }
      } else {
        for (const Step& st : data[i].steps)
          if (inWindow(st.t, st.len)) {
            if (s.range) {
              v.push_back(st.min);
              v.push_back(st.max);
            } else v.push_back(st.mean);
          }
      }
    }
    return v;
  };
  const Range leftRange = axisRange(chart.left, valuesOn("left"));
  Axis rightAxis;
  if (chart.hasRight) rightAxis = chart.right;
  const Range rightRange = usesRight ? axisRange(rightAxis, valuesOn("right")) : Range{};
  L.left = axisLayout(leftRange, plot, decimal);
  if (usesRight) {
    L.hasRight = true;
    L.right = axisLayout(rightRange, plot, decimal);
  }
  auto yOf = [&](double v, const std::string& axis) {
    const Range& r = axis == "right" && usesRight ? rightRange : leftRange;
    return round1(plot.y + plot.h - ((v - r.min) / (r.max - r.min)) * plot.h);
  };

  // The time axis.
  const double slot = perDay ? plot.w / std::max<size_t>(1, w.dates.size()) : 0;
  if (perDay) {
    for (size_t j = 0; j < w.dates.size(); j++) L.times.push_back({round1(plot.x + slot * (j + 0.5)), weekdayOf(w.dates[j])});
  } else {
    double every = 0;
    if (span <= 3600) every = 900;
    else if (span <= 6 * 3600) every = 3600;
    else if (span <= 25 * 3600) every = 6 * 3600;
    for (std::string d = dateOf(w.from, o.cal); midnightOf(d, o.cal) <= w.to; d = addDays(d, 1)) {
      const double m = midnightOf(d, o.cal);
      if (m >= w.from && m <= w.to) L.midnights.push_back({xOf(m), weekdayOf(d)});
      if (every == 0) continue;
      const double next = midnightOf(addDays(d, 1), o.cal);
      for (double t = m; t < next; t += every) {
        if (t < w.from || t > w.to) continue;
        const long hh = static_cast<long>(std::floor((t - m) / 3600));
        const long mm = static_cast<long>(jsRound(std::fmod(t - m, 3600) / 60));
        L.times.push_back({xOf(t), pad2(hh) + ":" + pad2(mm)});
      }
    }
  }

  // Zones.
  for (const Zone& z : chart.zones) {
    const Range& r = z.axis == "right" && usesRight ? rightRange : leftRange;
    const double top = yOf(std::min(z.hasTo ? z.to : r.max, r.max), z.axis);
    const double bottomY = yOf(std::max(z.hasFrom ? z.from : r.min, r.min), z.axis);
    L.zones.push_back({{plot.x, top, plot.w, round1(bottomY - top)}, z.color, z.colorDark});
  }

  const bool nowOn = !perDay && o.now >= w.from && o.now < w.to;
  std::map<std::string, int> stateIndex;
  {
    int k = 0;
    for (const Series& s : chart.series)
      if (s.valueKind == "state") stateIndex[s.id] = k++;
  }
  const double bandH = std::max(4.0, jsRound(o.fontSize * 0.6));
  const double barW = perDay ? (slot * 0.7) / std::max<size_t>(1, chart.series.size()) : 0;

  for (size_t index = 0; index < chart.series.size(); index++) {
    const Series& s = chart.series[index];
    const SeriesData& d = data[index];
    auto yS = [&](double v) { return yOf(v, s.axis); };
    SeriesLayout out;
    out.id = s.id;
    out.draw = s.draw;
    out.axis = s.axis;
    out.color = s.color;
    out.colorDark = s.colorDark;
    out.pattern = static_cast<int>(index);
    if (perDay) {
      const double zero = yS(0);
      for (size_t j = 0; j < w.dates.size(); j++) {
        const DayBar* day = nullptr;
        for (const DayBar& b : d.days)
          if (b.date == w.dates[j]) {
            day = &b;
            break;
          }
        const double x = round1(plot.x + slot * j + slot * 0.15 + index * barW);
        if (!day) continue;
        const std::pair<bool, double> vs[2] = {{day->hasUp, day->up}, {day->hasDown, day->down}};
        for (const auto& v : vs) {
          if (!v.first || v.second == 0) continue;
          const double y = yS(v.second);
          out.bars.push_back({x, std::min(y, zero), round1(barW), round1(std::fabs(zero - y))});
        }
      }
    } else if (s.valueKind == "state") {
      const int k = stateIndex.count(s.id) ? stateIndex[s.id] : 0;
      const double y = plot.y + plot.h - (k + 1) * bandH;
      for (const auto& p : d.on) {
        if (p.second <= w.from || p.first >= w.to) continue;
        const double x = xOf(std::max(p.first, w.from));
        out.bands.push_back({x, y, round1(xOf(std::min(p.second, w.to)) - x), bandH});
        if (p.first >= w.from) out.starts.push_back(xOf(p.first));
      }
    } else if (s.draw == "stacked") {
      std::vector<std::vector<Slice>> runs;
      for (const Slice& x : stacks[index]) {
        if (!inWindow(x.t, x.len)) continue;
        if (!runs.empty() && runs.back().back().t + runs.back().back().len == x.t) runs.back().push_back(x);
        else runs.push_back({x});
      }
      for (const auto& run : runs) {
        std::vector<Point> upper, lower;
        for (const Slice& x : run) {
          upper.push_back({xOf(x.t), yS(x.hi)});
          upper.push_back({xOf(x.t + x.len), yS(x.hi)});
          lower.push_back({xOf(x.t), yS(x.lo)});
          lower.push_back({xOf(x.t + x.len), yS(x.lo)});
        }
        std::reverse(lower.begin(), lower.end());
        out.lines.push_back(upper);
        std::vector<Point> area = upper;
        area.insert(area.end(), lower.begin(), lower.end());
        out.areas.push_back(area);
      }
    } else {
      if (s.draw == "line" || s.draw == "area" || s.draw == "steps") {
        std::vector<Step> steps;
        for (const Step& st : d.steps)
          if (inWindow(st.t, st.len)) steps.push_back(st);
        std::vector<std::vector<Step>> runs;
        for (const Step& st : steps) {
          if (!runs.empty() && runs.back().back().t + runs.back().back().len == st.t) runs.back().push_back(st);
          else runs.push_back({st});
        }
        const double zero = yS(0);
        for (const auto& run : runs) {
          std::vector<Point> points;
          for (const Step& st : run) {
            if (s.draw == "steps") {
              points.push_back({xOf(st.t), yS(st.mean)});
              points.push_back({xOf(st.t + st.len), yS(st.mean)});
            } else points.push_back({xOf(st.t + st.len / 2), yS(st.mean)});
          }
          out.lines.push_back(points);
          if (s.draw == "area") {
            std::vector<Point> area;
            area.push_back({points.front().first, zero});
            area.insert(area.end(), points.begin(), points.end());
            area.push_back({points.back().first, zero});
            out.areas.push_back(area);
          }
          if (s.range) {
            std::vector<Point> band;
            for (const Step& st : run) band.push_back({xOf(st.t + st.len / 2), yS(st.max)});
            for (auto it = run.rbegin(); it != run.rend(); ++it) band.push_back({xOf(it->t + it->len / 2), yS(it->min)});
            out.ranges.push_back(band);
          }
        }
        if (nowOn) {
          for (auto it = steps.rbegin(); it != steps.rend(); ++it)
            if (it->t <= o.now) {
              out.hasNowPoint = true;
              out.nowPoint = {xOf(o.now), yS(it->mean)};
              break;
            }
        }
      }
      // Min and max on the curve.
      std::vector<Step> shown;
      for (const Step& st : d.steps)
        if (inWindow(st.t, st.len)) shown.push_back(st);
      if (!shown.empty()) {
        for (const char* which : {"min", "max"}) {
          if (!s.annotated(which)) continue;
          const bool isMin = std::string(which) == "min";
          Step pick = shown.front();
          for (const Step& b : shown)
            if (isMin ? b.mean < pick.mean : b.mean > pick.mean) pick = b;
          out.marks.push_back({xOf(pick.t + pick.len / 2), yS(pick.mean), valueText(pick.mean, s, decimal) + " " + clockText(pick.t, o.cal), isMin});
        }
      }
    }
    L.series.push_back(out);
  }

  // The legend.
  if (chart.legend != "none") {
    for (size_t i = 0; i < chart.series.size(); i++) {
      const Series& s = chart.series[i];
      LegendRow row;
      if (chart.legend == "bottom") {
        row.x = plot.x;
        row.y = plot.y + plot.h + bottom + i * rowH;
        row.align = "left";
      } else if (chart.legend == "top-left") {
        row.x = plot.x + pad;
        row.y = plot.y + pad + i * rowH;
        row.align = "left";
      } else {
        row.x = plot.x + plot.w - pad;
        row.y = plot.y + pad + i * rowH;
        row.align = "right";
      }
      row.id = s.id;
      row.swatch = {row.align == "right" ? row.x - 10 : row.x, round1(row.y + o.fontSize / 2 - 2), 10, 4};
      row.color = s.color;
      row.colorDark = s.colorDark;
      row.pattern = static_cast<int>(i);
      row.name = s.name;
      row.figures = figures(s, data[i], w, o);
      L.legend.push_back(row);
    }
  }

  // The texts, as boxes.
  const double half = jsRound(o.fontSize / 2);
  auto box = [&](const std::string& text, double x, double y, double bw, const char* align, const char* tone) {
    L.texts.push_back({text, round1(x), round1(y), round1(bw), o.fontSize, align, tone});
  };
  for (const AxisTick& t : L.left.ticks) box(t.label, 0, t.y - half, plot.x - pad, "right", "muted");
  if (L.hasRight)
    for (const AxisTick& t : L.right.ticks) box(t.label, plot.x + plot.w + pad, t.y - half, labelW, "left", "muted");
  for (const Labelled& t : L.times) box(t.label, t.x - labelW, plot.y + plot.h + pad, labelW * 2, "center", "muted");
  for (const Labelled& m : L.midnights)
    if (m.x < plot.x + plot.w - o.fontSize * 2) box(m.label, m.x + 3, plot.y, labelW * 2, "left", "muted");
  for (const SeriesLayout& s : L.series)
    for (const Mark& m : s.marks) box(m.label, m.x - labelW * 3, m.below ? m.y + pad : m.y - pad - o.fontSize, labelW * 6, "center", "text");
  for (const LegendRow& row : L.legend) {
    std::string line = row.name;
    for (const std::string& f : row.figures) line += "  " + f;
    if (row.align == "right") box(line, plot.x, row.y, row.x - 14 - plot.x, "right", "text");
    else box(line, row.x + 14, row.y, plot.w - 14 - (row.x - plot.x), "left", "text");
  }
  if (!perDay && o.now >= w.to) box("→", plot.x, plot.y + plot.h / 2 - half, plot.w - 2, "right", "muted");

  L.nowOn = nowOn;
  L.nowPast = !perDay && !nowOn && o.now >= w.to;
  if (nowOn) L.nowX = xOf(o.now);
  return L;
}

}  // namespace chartcore
