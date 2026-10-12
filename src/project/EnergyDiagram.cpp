#include "project/EnergyDiagram.h"

#include <algorithm>
#include <cmath>

#include "project/ArcSinTable.h"

namespace energydiagram {

namespace {
constexpr double PI = 3.14159265358979323846;
}

Role roleFrom(const std::string& name) {
    if (name == "source") return Role::Source;
    if (name == "sink") return Role::Sink;
    if (name == "storage") return Role::Storage;
    if (name == "bus") return Role::Bus;
    return Role::Rest;
}

double jsRound(double v) { return std::floor(v + 0.5); }

std::vector<Resolved> resolve(const std::vector<Node>& nodes, const std::vector<Value>& values) {
    std::vector<Resolved> out;
    for (size_t i = 0; i < nodes.size(); i++) {
        const Node& n = nodes[i];
        if (n.role == Role::Bus) continue;
        const Value v = i < values.size() ? values[i] : Value{};
        const double raw = v.hasWatts && std::isfinite(v.watts) ? v.watts : 0;
        const double w = v.hasWatts && std::isfinite(v.watts) ? (n.reverseSign ? -raw : raw) : 0;
        Resolved r;
        r.id = n.id;
        r.role = n.role;
        r.x = n.x;
        r.y = n.y;
        if (n.role == Role::Storage) {
            r.watts = std::fabs(w);
            r.gives = w < 0;
            r.hasCharge = v.hasCharge;
            r.charge = v.charge;
        } else if (n.role == Role::Source) {
            r.watts = std::max(0.0, w);
            r.gives = true;
        } else if (n.role == Role::Sink) {
            r.watts = std::max(0.0, w);
            r.gives = false;
        } else {
            r.watts = 0;
            r.gives = false;
        }
        out.push_back(r);
    }
    double given = 0, taken = 0;
    for (const Resolved& r : out) {
        if (r.gives) given += r.watts;
        else if (r.role != Role::Rest) taken += r.watts;
    }
    for (Resolved& r : out) {
        if (r.role == Role::Rest) {
            r.watts = std::max(0.0, given - taken);
            break;
        }
    }
    return out;
}

std::string withWatts(const std::string& text, bool known, double watts) {
    static const std::string KEY = "{watts}";
    if (text.find(KEY) == std::string::npos) return text;
    std::string value = "-";
    if (known) {
        const long long whole = static_cast<long long>(jsRound(watts));
        value = std::to_string(whole);
    }
    std::string out;
    size_t at = 0;
    for (size_t hit = text.find(KEY); hit != std::string::npos; hit = text.find(KEY, at)) {
        out += text.substr(at, hit - at) + value;
        at = hit + KEY.size();
    }
    return out + text.substr(at);
}

// ---- Sankey

namespace {

std::vector<Point> bandEdge(double x0, double y0, double x1, double y1) {
    const double mx = (x0 + x1) / 2;
    std::vector<Point> pts;
    for (int i = 0; i <= SANKEY_SEGMENTS; i++) {
        const double t = static_cast<double>(i) / SANKEY_SEGMENTS;
        const double u = 1 - t;
        pts.push_back({u * u * u * x0 + 3 * u * u * t * mx + 3 * u * t * t * mx + t * t * t * x1,
                       u * u * u * y0 + 3 * u * u * t * y0 + 3 * u * t * t * y1 + t * t * t * y1});
    }
    return pts;
}

bool before(const Resolved& a, const Resolved& b) { return a.y < b.y || (a.y == b.y && a.x < b.x); }

}  // namespace

SankeyLayout sankeyLayout(const std::vector<Resolved>& nodes, Size size, double fullScale) {
    double givenSum = 0, takenSum = 0;
    for (const Resolved& n : nodes) {
        if (!(n.watts > 0)) continue;
        if (n.gives) givenSum += n.watts;
        else takenSum += n.watts;
    }
    const double total = std::max(givenSum, takenSum);
    const bool still = total <= 0;
    std::vector<Resolved> left, right;
    for (const Resolved& n : nodes) {
        if (!still && !(n.watts > 0)) continue;
        (n.gives ? left : right).push_back(n);
    }
    std::stable_sort(left.begin(), left.end(), before);
    std::stable_sort(right.begin(), right.end(), before);
    const double room = size.height - SANKEY_GAP * 4;
    const double leftBarX = SANKEY_LABEL_W;
    const double rightBarX = size.width - SANKEY_LABEL_W - SANKEY_BAR_W;
    const double busX = jsRound(size.width / 2 - SANKEY_BUS_W / 2);
    double scale = still ? 0 : (room * std::min(1.0, total / std::max(1.0, fullScale))) / total;
    auto band = [&](double w) { return std::max(SANKEY_MIN_BAND, w * scale); };
    auto fits = [&](const std::vector<Resolved>& list) {
        double sum = 0;
        for (const Resolved& n : list) sum += std::max(SANKEY_SLOT_MIN, band(n.watts));
        return sum + SANKEY_GAP * std::max<double>(0, static_cast<double>(list.size()) - 1);
    };
    for (int i = 0; i < 40 && std::max(fits(left), fits(right)) > size.height && scale * total > SANKEY_MIN_BAND; i++) scale *= 0.92;

    SankeyLayout out;
    auto place = [&](const std::vector<Resolved>& list, Side side) {
        double y = (size.height - fits(list)) / 2;
        for (const Resolved& n : list) {
            const double h = band(n.watts);
            const double slot = std::max(SANKEY_SLOT_MIN, h);
            out.slots.push_back({n.id, side, y, slot});
            out.bars.push_back({n.id, side, {side == Side::Left ? leftBarX : rightBarX, y + (slot - h) / 2, SANKEY_BAR_W, h}});
            y += slot + SANKEY_GAP;
        }
    };
    place(left, Side::Left);
    place(right, Side::Right);

    const double busH = std::max(SANKEY_MIN_BAND, total * scale);
    out.bus = {busX, (size.height - busH) / 2, SANKEY_BUS_W, busH};
    auto stack = [&](const std::vector<Resolved>& list, Side side) {
        double by = out.bus.y;
        for (const Resolved& n : list) {
            const Bar* bar = nullptr;
            for (const Bar& b : out.bars)
                if (b.id == n.id) bar = &b;
            const double h = n.watts * scale;
            const double bh = std::max(1.5, h);
            std::vector<Point> top, bottom;
            if (side == Side::Left) {
                const double x0 = leftBarX + SANKEY_BAR_W - SANKEY_UNDER_BAR, x1 = busX + SANKEY_UNDER_BUS;
                top = bandEdge(x0, bar->box.y, x1, by);
                bottom = bandEdge(x0, bar->box.y + bar->box.height, x1, by + bh);
            } else {
                const double x0 = busX + SANKEY_BUS_W - SANKEY_UNDER_BUS, x1 = rightBarX + SANKEY_UNDER_BAR;
                top = bandEdge(x0, by, x1, bar->box.y);
                bottom = bandEdge(x0, by + bh, x1, bar->box.y + bar->box.height);
            }
            Band b;
            b.id = n.id;
            b.polygon = top;
            b.polygon.insert(b.polygon.end(), bottom.rbegin(), bottom.rend());
            out.bands.push_back(b);
            by += h;
        }
    };
    if (!still) {
        stack(left, Side::Left);
        stack(right, Side::Right);
    }
    return out;
}

// ---- Flow

double lineWidth(double watts) {
    if (!(watts > 0)) return 3;
    return std::max(3.0, std::min(12.0, 2 + std::log10(watts + 1) * 3.4));
}

FlowLayout flowLayout(const std::vector<Node>& nodes, const std::vector<Resolved>& resolved) {
    FlowLayout out;
    for (const Node& n : nodes) {
        if (n.role == Role::Bus) {
            out.busX = n.x;
            out.busY = n.y;
            break;
        }
    }
    out.busR = FLOW_BUS_R;
    const double besideBelow = std::cos(PI / 6);
    for (const Resolved& n : resolved) {
        const bool live = n.watts > 0;
        const double dx = out.busX - n.x, dy = out.busY - n.y;
        const double len = std::sqrt(dx * dx + dy * dy);
        const double ux = len > 0 ? dx / len : 0, uy = len > 0 ? dy / len : 0;
        const bool ringed = n.role == Role::Storage;
        const double outer = FLOW_NODE_R + (ringed ? FLOW_RING_GAP + FLOW_RING_W / 2 : 0);
        FlowNode node;
        node.id = n.id;
        node.x = n.x;
        node.y = n.y;
        node.r = FLOW_NODE_R;
        node.live = live;
        if (uy > besideBelow) {
            node.labelX = n.x + outer + FLOW_LABEL_GAP;
            node.labelY = n.y - FLOW_NODE_R + 8;
            node.labelBeside = true;
        } else {
            node.labelX = n.x;
            node.labelY = n.y + FLOW_NODE_R + (ringed ? 30 : 20);
        }
        if (ringed) {
            node.hasRing = true;
            node.ringR = FLOW_NODE_R + FLOW_RING_GAP;
            node.ringFraction = n.hasCharge ? std::max(0.0, std::min(1.0, n.charge / 100)) : 0;
        }
        out.nodes.push_back(node);
        // A storage's line ends at its track's outer edge.
        const double start = ringed ? FLOW_NODE_R + FLOW_RING_GAP + FLOW_RING_W / 2 : FLOW_NODE_R + FLOW_LINE_GAP;
        if (len <= start + FLOW_BUS_R) continue;
        FlowLine line;
        line.id = n.id;
        line.x0 = n.x + ux * start;
        line.y0 = n.y + uy * start;
        line.x1 = out.busX - ux * FLOW_BUS_R;
        line.y1 = out.busY - uy * FLOW_BUS_R;
        line.width = lineWidth(n.watts);
        line.live = live;
        out.lines.push_back(line);
    }
    return out;
}

// ---- Names, lines and icons

double rowStep(double size) { return jsRound(size * ROW_LEADING); }

double labelHeight(int lines, const LabelMetrics& m) {
    const int n = std::min(MAX_NODE_LINES, std::max(0, lines));
    return rowStep(m.fontSize) * (1 + std::max(0, n - 1)) + (n ? rowStep(m.valueFontSize) : 0);
}

DiagramBoxes sankeyBoxes(const SankeyLayout& layout, const std::vector<Node>& nodes, Size size, const LabelMetrics& m) {
    DiagramBoxes out;
    const double iconSize = rowStep(m.fontSize);
    const double width = SANKEY_LABEL_W - 12 - iconSize - 6;
    for (const Slot& slot : layout.slots) {
        int lines = 0;
        for (const Node& n : nodes)
            if (n.id == slot.id) lines = std::min(MAX_NODE_LINES, n.lines);
        const bool left = slot.side == Side::Left;
        const double top = jsRound(slot.y + (slot.height - labelHeight(lines, m)) / 2);
        const double x = left ? 0 : size.width - SANKEY_LABEL_W + 12;
        const double cx = left ? SANKEY_LABEL_W - 12 - iconSize / 2 : x + iconSize / 2;
        NodeBoxes b;
        b.id = slot.id;
        b.icon = {jsRound(cx - iconSize / 2), top, iconSize, iconSize};
        b.label = {left ? 0 : x + iconSize + 6, top, width, left ? Align::Right : Align::Left};
        out.nodes.push_back(b);
    }
    if (!layout.bars.empty()) {
        out.hasBus = true;
        out.bus = {jsRound(layout.bus.x - 60), std::max(0.0, jsRound(layout.bus.y - rowStep(m.fontSize) - 4)), 120 + SANKEY_BAR_W, Align::Center};
    }
    return out;
}

double flowIcon() { return jsRound(FLOW_NODE_R * 0.95); }

DiagramBoxes flowBoxes(const FlowLayout& layout, const LabelMetrics& m) {
    DiagramBoxes out;
    out.hasBus = true;
    out.bus = {layout.busX + layout.busR + 8, layout.busY - jsRound(m.fontSize * 0.6), 120, Align::Left};
    const double icon = flowIcon();
    for (const FlowNode& fn : layout.nodes) {
        const double top = fn.labelBeside ? fn.y - FLOW_NODE_R
                                          : std::max(fn.labelY - jsRound(m.fontSize * 0.8), fn.y + (fn.hasRing ? fn.ringR + FLOW_RING_W / 2 + 4 : fn.r + 4));
        NodeBoxes b;
        b.id = fn.id;
        b.icon = {jsRound(fn.x - icon / 2), jsRound(fn.y - icon / 2), icon, icon};
        b.label = {fn.labelBeside ? fn.labelX : fn.labelX - FLOW_LABEL_W / 2, top, FLOW_LABEL_W, fn.labelBeside ? Align::Left : Align::Center};
        out.nodes.push_back(b);
    }
    return out;
}

std::string nodeAtTap(bool sankey, const std::vector<Node>& nodes, const std::vector<Resolved>& resolved, Size size, double fullScale,
                      double x, double y) {
    if (sankey) {
        const SankeyLayout layout = sankeyLayout(resolved, size, fullScale);
        for (const Slot& s : layout.slots) {
            const bool onSide = s.side == Side::Left ? x < SANKEY_LABEL_W + SANKEY_BAR_W : x >= size.width - SANKEY_LABEL_W - SANKEY_BAR_W;
            if (y >= s.y && y < s.y + s.height && onSide) return s.id;
        }
        return "";
    }
    for (const Node& n : nodes)
        if (n.role != Role::Bus && std::hypot(x - n.x, y - n.y) <= FLOW_NODE_R) return n.id;
    return "";
}

// ---- Shapes

namespace {

double sinDeg(int deg) { return ARC_SIN_TABLE[((deg % 360) + 360) % 360] / (double)ARC_SIN_SCALE; }
double cosDeg(int deg) { return sinDeg(deg + 90); }
Point8 at8(double x, double y) { return {toEighths(x), toEighths(y)}; }

}  // namespace

int32_t toEighths(double px) {
    const double e = px * 8;
    return (int32_t)(e < 0 ? -std::floor(-e + 0.5) : std::floor(e + 0.5));
}

int circleStep(double r) { return r <= 4 ? 15 : r <= 16 ? 6 : r <= 40 ? 3 : 2; }

std::vector<Point8> discOutline(double cx, double cy, double r) {
    const int step = circleStep(r);
    std::vector<Point8> out;
    for (int d = 0; d < 360; d += step) out.push_back(at8(cx + r * cosDeg(d), cy + r * sinDeg(d)));
    return out;
}

std::vector<Point8> ringOutline(double cx, double cy, double outer, double inner) {
    std::vector<Point8> out = discOutline(cx, cy, outer);
    out.push_back(out[0]);
    const int step = circleStep(inner);
    for (int d = 360; d >= 0; d -= step) out.push_back(at8(cx + inner * cosDeg(d), cy + inner * sinDeg(d)));
    return out;
}

std::vector<Point8> capsuleOutline(double x0, double y0, double x1, double y1, double width) {
    const double dx = x1 - x0, dy = y1 - y0;
    const double len = std::sqrt(dx * dx + dy * dy);
    const double ux = len > 0 ? dx / len : 1, uy = len > 0 ? dy / len : 0;
    const double r = width / 2;
    const int step = circleStep(r);
    std::vector<Point8> out;
    auto cap = [&](double cx, double cy, int from) {
        for (int d = from; d <= from + 180; d += step) {
            const double c = cosDeg(d), s = sinDeg(d);
            out.push_back(at8(cx + r * (c * ux - s * uy), cy + r * (c * uy + s * ux)));
        }
    };
    cap(x1, y1, -90);
    cap(x0, y0, 90);
    return out;
}

std::vector<Point8> arcOutline(double cx, double cy, double r, double width, int sweep) {
    if (sweep >= 360) return ringOutline(cx, cy, r + width / 2, r - width / 2);
    const double h = width / 2;
    const int from = -90, to = -90 + sweep;
    std::vector<Point8> out;
    auto arc = [&](double radius, int a, int b) {
        const int step = circleStep(radius);
        if (a <= b) {
            for (int d = a; d < b; d += step) out.push_back(at8(cx + radius * cosDeg(d), cy + radius * sinDeg(d)));
        } else {
            for (int d = a; d > b; d -= step) out.push_back(at8(cx + radius * cosDeg(d), cy + radius * sinDeg(d)));
        }
        out.push_back(at8(cx + radius * cosDeg(b), cy + radius * sinDeg(b)));
    };
    auto cap = [&](double ex, double ey, int a) {
        const int step = circleStep(h);
        for (int d = a; d <= a + 180; d += step) out.push_back(at8(ex + h * cosDeg(d), ey + h * sinDeg(d)));
    };
    arc(r + h, from, to);
    cap(cx + r * cosDeg(to), cy + r * sinDeg(to), to);
    arc(r - h, to, from);
    cap(cx + r * cosDeg(from), cy + r * sinDeg(from), from + 180);
    return out;
}

std::vector<Point8> rectOutline(double x, double y, double width, double height) {
    return {at8(x, y), at8(x + width, y), at8(x + width, y + height), at8(x, y + height)};
}

std::vector<Point8> barOutline(double x, double y, double width, double height, double radius, bool left) {
    std::vector<Point8> out;
    auto corner = [&](double cx, double cy, int a) {
        for (int d = a; d <= a + 90; d += 15) out.push_back(at8(cx + radius * cosDeg(d), cy + radius * sinDeg(d)));
    };
    if (left) {
        out.push_back(at8(x + width, y));
        out.push_back(at8(x + width, y + height));
        if (radius > 0) {
            corner(x + radius, y + height - radius, 90);
            corner(x + radius, y + radius, 180);
        } else {
            out.push_back(at8(x, y + height));
            out.push_back(at8(x, y));
        }
    } else {
        out.push_back(at8(x, y));
        if (radius > 0) {
            corner(x + width - radius, y + radius, 270);
            corner(x + width - radius, y + height - radius, 0);
        } else {
            out.push_back(at8(x + width, y));
            out.push_back(at8(x + width, y + height));
        }
        out.push_back(at8(x, y + height));
    }
    return out;
}

std::vector<Point8> bandOutline(const Band& band) {
    std::vector<Point8> out;
    for (const Point& p : band.polygon) out.push_back(at8(p.x, p.y));
    return out;
}

uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)); }

std::array<int, 3> expand565(uint16_t c) {
    const int r5 = (c >> 11) & 0x1f, g6 = (c >> 5) & 0x3f, b5 = c & 0x1f;
    return {{(r5 * 527 + 23) >> 6, (g6 * 259 + 33) >> 6, (b5 * 527 + 23) >> 6}};
}

uint16_t mix565(uint16_t under, uint16_t over, int w) {
    if (w >= 256) return over;
    if (w <= 0) return under;
    const std::array<int, 3> u = expand565(under), o = expand565(over);
    return rgb565((u[0] * (256 - w) + o[0] * w + 128) >> 8, (u[1] * (256 - w) + o[1] * w + 128) >> 8, (u[2] * (256 - w) + o[2] * w + 128) >> 8);
}

// ---- Motion

double dotGap(double watts) {
    const double w = std::min(1000.0, std::max(1.0, std::fabs(watts)));
    return DOT_GAP_1W * std::pow(DOT_GAP_FULL / DOT_GAP_1W, std::log10(w) / 3);
}

static double wrap(double v) {
    const double m = std::fmod(v, 1.0);
    return std::fmod(m + 1, 1.0);
}

double advanceDots(double phase, double seconds, double watts) { return wrap(phase + (seconds * DOT_PACE) / dotGap(watts)); }

std::vector<Point> dotsOnLine(const FlowLine& line, double phase, bool gives, double watts) {
    const double len = std::sqrt((line.x1 - line.x0) * (line.x1 - line.x0) + (line.y1 - line.y0) * (line.y1 - line.y0));
    const double gap = dotGap(watts);
    std::vector<Point> out;
    for (double d = phase * gap; d < len; d += gap) {
        const double f = gives ? d / len : 1 - d / len;
        out.push_back({line.x0 + (line.x1 - line.x0) * f, line.y0 + (line.y1 - line.y0) * f});
    }
    return out;
}

std::array<int, 3> trackColour(const std::array<int, 3>& bar, const std::array<int, 3>& ground) {
    std::array<int, 3> out{};
    for (int i = 0; i < 3; i++) out[i] = (int)jsRound(ground[i] + (bar[i] - ground[i]) * TRACK_ALPHA);
    return out;
}

std::vector<PathDot> storageDots(const FlowLine& line, const FlowNode& node, double phase, bool gives, double watts) {
    const double len = std::sqrt((line.x1 - line.x0) * (line.x1 - line.x0) + (line.y1 - line.y0) * (line.y1 - line.y0));
    const double gap = dotGap(watts);
    std::vector<PathDot> out;
    const int sweep = node.hasRing ? std::max(0, std::min(360, (int)jsRound(node.ringFraction * 360))) : 0;
    const double toBus = std::atan2(line.y1 - node.y, line.x1 - node.x);
    const double entry = std::fmod(std::fmod(toBus * 180 / PI + 90, 360.0) + 360, 360.0);
    double arc = 0;
    int way = 1;
    bool inBar = false;
    if (node.hasRing && sweep > 0) {
        if (entry <= sweep) {
            inBar = true;
            const double back = entry, on = sweep - entry;
            way = on >= back ? 1 : -1;
            arc = std::max(back, on);
        } else {
            const double back = entry - sweep, on = 360 - entry;
            way = on <= back ? 1 : -1;
            arc = std::min(back, on);
        }
    }
    const double r = node.hasRing ? node.ringR : 0;
    const double lineEnd = std::sqrt((line.x0 - node.x) * (line.x0 - node.x) + (line.y0 - node.y) * (line.y0 - node.y));
    const double inward = node.hasRing && sweep > 0 ? std::max(0.0, lineEnd - r) : 0;
    const double onLine = len + inward;
    const double arcLen = arc / 360 * 2 * PI * r;
    const double total = onLine + arcLen;
    for (double d = phase * gap; d < total; d += gap) {
        const double t = gives ? total - d : d;
        PathDot p;
        if (t <= len) {
            const double f = 1 - t / len;
            p.x = line.x0 + (line.x1 - line.x0) * f;
            p.y = line.y0 + (line.y1 - line.y0) * f;
            p.onLine = true;
        } else if (t <= onLine) {
            const double q = lineEnd - (t - len);
            p.x = node.x + q * std::cos(toBus);
            p.y = node.y + q * std::sin(toBus);
            p.alpha = inBar ? 1 : TRACK_DOT_ALPHA;
            p.onBar = inBar;
        } else {
            const double s = t - onLine;
            const double deg = entry + way * s * 360 / (2 * PI * r);
            const double rad = (deg - 90) * PI / 180;
            p.x = node.x + r * std::cos(rad);
            p.y = node.y + r * std::sin(rad);
            p.alpha = inBar ? std::max(0.0, 1 - s / arcLen) : TRACK_DOT_ALPHA;
            p.onBar = inBar;
        }
        if (p.alpha > 0) out.push_back(p);
    }
    return out;
}

double advanceWave(double phase, double seconds) { return wrap(phase + seconds / WAVE_SECONDS); }

double waveMix(double x, double phase) { return 0.42 - 0.16 * std::cos((2 * PI * (x - phase * WAVE_PX)) / WAVE_PX); }

std::array<int, 3> waveColour(const std::array<int, 3>& band, const std::array<int, 3>& ground, double x, double phase) {
    const double t = waveMix(x, phase);
    std::array<int, 3> out{};
    for (int i = 0; i < 3; i++) out[i] = static_cast<int>(jsRound(band[i] + (ground[i] - band[i]) * t));
    return out;
}

}  // namespace energydiagram
