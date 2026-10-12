#include "project/EnergyDiagram.h"

#include <algorithm>
#include <cmath>

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
        const double len = std::hypot(dx, dy);
        const double ux = len > 0 ? dx / len : 0, uy = len > 0 ? dy / len : 0;
        const bool ringed = n.role == Role::Storage;
        const double outer = FLOW_NODE_R + (ringed ? FLOW_RING_GAP : 0);
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
            node.labelY = n.y + FLOW_NODE_R + (ringed ? 26 : 20);
        }
        if (ringed) {
            node.hasRing = true;
            node.ringR = FLOW_NODE_R + FLOW_RING_GAP;
            node.ringFraction = n.hasCharge ? std::max(0.0, std::min(1.0, n.charge / 100)) : 0;
        }
        out.nodes.push_back(node);
        if (len <= FLOW_NODE_R + FLOW_LINE_GAP + FLOW_BUS_R) continue;
        FlowLine line;
        line.id = n.id;
        line.x0 = n.x + ux * (FLOW_NODE_R + FLOW_LINE_GAP);
        line.y0 = n.y + uy * (FLOW_NODE_R + FLOW_LINE_GAP);
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
                                          : std::max(fn.labelY - jsRound(m.fontSize * 0.8), fn.y + (fn.hasRing ? fn.ringR + 4 : fn.r + 4));
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
    const double len = std::hypot(line.x1 - line.x0, line.y1 - line.y0);
    const double gap = dotGap(watts);
    std::vector<Point> out;
    for (double d = phase * gap; d < len; d += gap) {
        const double f = gives ? d / len : 1 - d / len;
        out.push_back({line.x0 + (line.x1 - line.x0) * f, line.y0 + (line.y1 - line.y0) * f});
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
