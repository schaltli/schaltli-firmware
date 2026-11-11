#pragma once
#include "ChartCore.h"
#include <Arduino.h>
#include <memory>
#include <optional>
#include <vector>

#include "PopupFence.h"
#include "CombinedTopics.h"
#include "LiveValue.h"
#include "ScaledValue.h"
#include "SpiramAllocator.h"

/**
 * Data structures for project.json configuration
 * Represents screens, objects, and properties
 */

// Value-Icon pair for conditional icon display
struct ValueIconPair {
  String id;
  String comparisonOperator;  // ">", "<", "==", ">=", "<=", "!="
  float value;
  String thenShowIcon;        // Icon ID to show
  String path;                // Path to BMP file
  String pathDark;            // The dark variant's bitmap (ThemeVariant.h); empty: same as path
};

// Calibration point for level indicators
struct CalibrationPoint {
  float value;                // Input value from MQTT
  float barSizePercent;       // Bar fill percentage (0-100)
};

// One segment of a Switch object - mirrors the designer's SwitchState
// (components/canvas/renderers/render-switch.ts) field-for-field.
// `readValue` is matched (trimmed String equality) against the Switch's own
// `topic` to decide which segment is "active"; `writeValue` is published
// as-is (raw String, not retained) to the Switch's `writeTopic` when this
// segment is tapped. `iconPath`/`iconPathActive` - baked twice by the
// designer's exportSwitchStateIcon() (lib/asset-export.ts) against the
// segment's normal vs. active background color respectively, since the
// segment's own fill changes live at runtime and a single fixed-background
// bake would look wrong half the time (2026-08-14 finding).
struct SwitchState {
  String id;
  String label;
  String readValue;
  String writeValue;
  String iconPath;
  // A genuinely different picture while this state is the active one
  // (2026-08-25). It used to be the same picture baked against
  // activeBackgroundColor, back when the active segment was filled; the
  // marker bar replaced that fill, so nothing behind the icon changes any
  // more and the exporter only ships this file when the author really did
  // pick a second icon. Empty means "same picture either way" - the
  // fallback below in renderSwitch already handled that case, because
  // exports predating the second variant looked exactly like this.
  String iconPathActive;
  // The dark variant's two bitmaps (ThemeVariant.h); empty: same as light.
  String iconPathDark;
  String iconPathActiveDark;
  // A Toggle Button's state icon as it stands on the pressed face; empty:
  // as iconPath (designer contract «toggle-button»).
  String iconPathPressed;
  String iconPathPressedDark;
  // "single" mode only: does this state show the marker bar? Ignored in
  // "segmented", where the bar always marks whichever segment is active.
  bool showMarker;

  SwitchState() : showMarker(false) {}
};

// One vertex of a (possibly multi-segment) line - mirrors the designer's
// LinePoint (render-line.ts). Absolute screen coordinates, same as every
// other object's own x/y.
struct Point {
  int x;
  int y;
};

// A single button action - mirrors the designer's HardwareButtonAction
// (components/project-editor.tsx) field-for-field. `type` is one of
// "next-screen" / "previous-screen" / "goto-screen" / "send-mqtt" /
// "goto-setup-mode", or empty to mean "nothing configured" (the default-
// constructed value - callers treat an empty type as a no-op). Shared
// between the physical push button (HardwareButtonDef::defaultAction,
// Screen::buttonActions) and a SoftwareButton object's own
// ObjectProperties::action below - both dispatch through main.cpp's
// dispatchButtonAction(), one shared function rather than two parallel
// ones. "goto-setup-mode" takes no extra fields (like next-screen/
// previous-screen) - it's the only supported way into setup mode from a
// project-configured button; the rear GPIO0 button is a separate,
// hardcoded, project-independent fallback that bypasses this entirely
// (deliberately not exposed here or in the DDF - see main.cpp's own
// comment on it).
struct ButtonAction {
  String type;
  String targetScreenId;   // for "goto-screen"
  String mqttTopic;        // for "send-mqtt"
  String mqttMessage;      // for "send-mqtt"
  // for "device-action" - an id the *device* offers and the designer only
  // names, declared in this board's DDF as deviceActions (e.g.
  // "showScreenMenu"). The designer has no idea what any of them do, it just
  // lets a project bind one by name. An id this firmware doesn't recognise
  // is skipped and logged rather than erroring - same "additive changes
  // degrade, never break" convention unknown object types already follow, so
  // a project built against a newer device still runs here.
  String deviceActionId;
  // for "adjust-level" (designer docs/device-contract.md §5, 2026-10-04): the
  // slider or dial - or the switcher whose visible panel holds one - a press
  // moves one step, and which way ("up"/"down"). main.cpp's adjustLevel().
  String targetObjectId;
  String direction;
  // for "goto-neighbour" (contract 2.10): "up", "down", "left" or "right" -
  // the screen lying there, Screen::neighbours.
  String side;
  // for "move-range" (contract 2.13): "earlier" or "later"; for "set-range":
  // the span, "1h", "6h", "24h" or "7d" (ChartCore.h RangeView).
  String rangeDirection;
  String range;
  // for "send-mqtt", the DDF feature "button-values" (ButtonValues.h): a
  // combined topic that must be yes for the press to send, and a local
  // topic set empty after it did.
  String onlyWhen;
  String clears;
};

// The energy diagram (designer docs/device-contract.md 2.8, generation
// 1.7): its data as the export gives it. The places come from
// EnergyDiagram.h, worked out from the topics' values when it is drawn.
struct EnergyLine {
  String text;      // `{watts}` filled in before it is resolved
  String liveText;  // with `{live:<id>}`, read in preference to `text`
  std::vector<livevalue::LiveValue> liveValues;
};
// A node's icon, baked per look (2.8): with watts `live` - a storage
// `charge` or `discharge` - and at 0 W `muted`.
struct EnergyIcon {
  int size = 0;
  String live, liveDark;
  String charge, chargeDark;
  String discharge, dischargeDark;
  String muted, mutedDark;
};
struct EnergyNodeProps {
  String id;
  String role;  // source, sink, storage, rest, bus
  String name;
  float x = 0;
  float y = 0;
  String topic;
  String chargeTopic;
  bool reverseSign = false;
  String color, colorDark;
  bool hasIcon = false;
  EnergyIcon icon;
  std::vector<EnergyLine> lines;
  ButtonAction action;
};
struct EnergyDiagramProps {
  bool sankey = false;
  bool motion = true;
  float fullScale = 1000;
  String fontId, valueFontId;
  int fontSize = 16;
  int valueFontSize = 16;
  String textColor, textColorDark;
  String mutedColor, mutedColorDark;
  String busColor, busColorDark;
  String idleColor, idleColorDark;
  String chargeColor, chargeColorDark;
  String dischargeColor, dischargeColorDark;
  std::vector<EnergyNodeProps> nodes;
};

// An energy diagram's data behind a pointer that copies what it points at:
// a copied object (a master's on each of its screens) has its own, so the
// dark swap (ThemeVariant.h) never meets one diagram twice.
struct EnergyDiagramHandle {
  std::unique_ptr<EnergyDiagramProps> p;
  EnergyDiagramHandle() = default;
  EnergyDiagramHandle(const EnergyDiagramHandle& o) : p(o.p ? new EnergyDiagramProps(*o.p) : nullptr) {}
  EnergyDiagramHandle& operator=(const EnergyDiagramHandle& o) {
    if (this != &o) p.reset(o.p ? new EnergyDiagramProps(*o.p) : nullptr);
    return *this;
  }
  EnergyDiagramHandle(EnergyDiagramHandle&&) = default;
  EnergyDiagramHandle& operator=(EnergyDiagramHandle&&) = default;
  explicit operator bool() const { return static_cast<bool>(p); }
  EnergyDiagramProps& operator*() const { return *p; }
  EnergyDiagramProps* operator->() const { return p.get(); }
};

// A software button as the device draws it (designer device contract,
// «button»; docs/2026-11-04-native-buttons.md in the designer): both faces,
// where icon and label go, relative to the button. Every judgement is the
// designer's (lib/button-shape.ts); this only holds what it decided. Until
// 2026-11-04 a button was two pictures, pathNormal/pathActive.
struct ButtonFace {
  int r = 0;
  String fill;     // empty: no fill
  String outline;  // empty: no ring
  String ink;
};
struct ButtonLookProps {
  ButtonFace normal, pressed;
  // The dark theme's faces (lookDark): colours only; empty: as light.
  ButtonFace normalDark, pressedDark;
  int textX = 0, baseline = 0, clipX = 0, clipW = 0;
  bool hasIcon = false;
  int iconX = 0, iconY = 0, iconSize = 0;
  String iconPath, iconPathPressed, iconPathDark, iconPathPressedDark;
};
struct ButtonLookHandle {
  std::unique_ptr<ButtonLookProps> p;
  ButtonLookHandle() = default;
  ButtonLookHandle(const ButtonLookHandle& o) : p(o.p ? new ButtonLookProps(*o.p) : nullptr) {}
  ButtonLookHandle& operator=(const ButtonLookHandle& o) {
    if (this != &o) p.reset(o.p ? new ButtonLookProps(*o.p) : nullptr);
    return *this;
  }
  ButtonLookHandle(ButtonLookHandle&&) = default;
  ButtonLookHandle& operator=(ButtonLookHandle&&) = default;
  explicit operator bool() const { return static_cast<bool>(p); }
  ButtonLookProps& operator*() const { return *p; }
  ButtonLookProps* operator->() const { return p.get(); }
};

// A Toggle Button as the device draws it (designer device contract
// «toggle-button», docs/2026-11-06-toggle-button.md and, since 2026-11-11,
// docs/2026-11-11-led-look.md): a calm tile per state with its name, and a
// light - Korry's window or an LED strip - that says the state. Every
// judgement is the designer's (lib/toggle-button-shape.ts); this only holds
// it.
struct ToggleLook {
  ButtonFace normal, pressed, normalDark, pressedDark;
  String label;
  int textX = 0, baseline = 0, clipX = 0, clipW = 0;
  bool hasIcon = false;
  int iconX = 0, iconY = 0, iconSize = 0;
  // The state's label as a line of text (LED Horizontal: at the end of the
  // name's line; LED Vertical: under the name), in its own ink.
  bool hasSecond = false;
  int secondTextX = 0, secondBaseline = 0;
  String secondFontId, secondInk, secondInkDark;
  // The light: its rectangle; its fill for the state; asked and askedDim in
  // turn while a request is outstanding; a 1-px ring where it needs one.
  int lightX = 0, lightY = 0, lightW = 0, lightH = 0, lightR = 0;
  String lightFill, lightAsked, lightAskedDim, lightOutline;
  String lightFillDark, lightAskedDark, lightAskedDimDark, lightOutlineDark;
  // Korry's window word, centred, with its ink per fill.
  bool hasWord = false;
  String wordText, wordFontId, wordInk, wordAskedInk, wordInkDark, wordAskedInkDark;
  int wordX = 0, wordBaseline = 0;
};
struct ToggleButtonProps {
  std::vector<ToggleLook> looks;
  ToggleLook none;
  String ring, ringDark;
};
struct ToggleButtonHandle {
  std::unique_ptr<ToggleButtonProps> p;
  ToggleButtonHandle() = default;
  ToggleButtonHandle(const ToggleButtonHandle& o) : p(o.p ? new ToggleButtonProps(*o.p) : nullptr) {}
  ToggleButtonHandle& operator=(const ToggleButtonHandle& o) {
    if (this != &o) p.reset(o.p ? new ToggleButtonProps(*o.p) : nullptr);
    return *this;
  }
  ToggleButtonHandle(ToggleButtonHandle&&) = default;
  ToggleButtonHandle& operator=(ToggleButtonHandle&&) = default;
  explicit operator bool() const { return static_cast<bool>(p); }
  ToggleButtonProps& operator*() const { return *p; }
  ToggleButtonProps* operator->() const { return p.get(); }
};

// A Camera (designer docs/2026-11-09-camera.md): where its picture comes from.
struct CameraProps {
  String url;
  String username, password;  // HTTP Basic, optional; never logged
};
struct CameraHandle {
  std::unique_ptr<CameraProps> p;
  CameraHandle() = default;
  CameraHandle(const CameraHandle& o) : p(o.p ? new CameraProps(*o.p) : nullptr) {}
  CameraHandle& operator=(const CameraHandle& o) {
    if (this != &o) p.reset(o.p ? new CameraProps(*o.p) : nullptr);
    return *this;
  }
  CameraHandle(CameraHandle&&) = default;
  CameraHandle& operator=(CameraHandle&&) = default;
  explicit operator bool() const { return static_cast<bool>(p); }
  CameraProps& operator*() const { return *p; }
  CameraProps* operator->() const { return p.get(); }
};

// A chart (designer device contract 2.13): its model for the core
// (ChartCore.h), and its colours as the dark swap (ThemeVariant.h) takes
// them - the chart's own beyond textColor and lineColor, which every object
// has, and one per series and zone, in their order.
struct ChartProps {
  chartcore::Chart chart;
  String fontId;
  // The days' names above the plot; empty: fontId.
  String dateFontId;
  String mutedColor, mutedColorDark;
  // The line at now and a stack's NowPoint (designer lib/chart-shapes.ts ShapeLook.now).
  String nowColor, nowColorDark;
  std::vector<String> seriesColor, seriesColorDark;
  std::vector<String> zoneColor, zoneColorDark;
};

struct ChartHandle {
  std::unique_ptr<ChartProps> p;
  ChartHandle() = default;
  ChartHandle(const ChartHandle& o) : p(o.p ? new ChartProps(*o.p) : nullptr) {}
  ChartHandle& operator=(const ChartHandle& o) {
    if (this != &o) p.reset(o.p ? new ChartProps(*o.p) : nullptr);
    return *this;
  }
  ChartHandle(ChartHandle&&) = default;
  ChartHandle& operator=(ChartHandle&&) = default;
  explicit operator bool() const { return static_cast<bool>(p); }
  ChartProps& operator*() const { return *p; }
  ChartProps* operator->() const { return p.get(); }
};

// A shape an object is clipped by (device contract 2.1, `clip`;
// docs/2026-11-03-card-clip.md in the designer): a rounded rectangle in the
// object's own coordinate space, already inside the clipping area's edge.
// A pixel of the object shows only where Adafruit_GFX::fillRoundRect(x, y,
// width, height, radius) would set one, for every shape it carries.
struct ClipShape {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int radius = 0;
};

// Base properties for screen objects
struct ObjectProperties {
  // Common properties
  String topic;                           // MQTT topic (e.g., "Freshwater/Level")
  String displayAs;                       // "Display as-is", "Formatted Number"
  String backgroundColor;
  String borderColor;
  String textColor;
  String color;                           // Alternate text color property (for labels)
  // The dark variant's value, beside its light one (the designer's
  // docs/device-contract.md §2.3). Empty: the same as light. Swapped in by
  // ThemeVariant.h while schaltli/state/theme says dark.
  String backgroundColorDark;
  String borderColorDark;
  String textColorDark;
  String colorDark;
  String fillColorDark;
  String fillEndColorDark;
  String trackColorDark;
  String trackEdgeColorDark;
  String strokeColorDark;
  String switchColorDark;
  // A text field's (designer tasks/ha-text-plan.md): its container, the line
  // along its bottom, and their dark variants. Its text is textColor.
  String fieldColor;
  String fieldColorDark;
  String lineColor;
  String lineColorDark;
  // Its bounds in characters (textfield::bounds) and whether it shows dots;
  // `hidden` in the JSON, not that name here: Screen has one of its own.
  int minLength = 0;
  int maxLength = 255;
  bool hideText = false;
  // "digits": the keyboard opens on its digits (button-values).
  String keyboard;
  // A time field's minute step (timefield::step reads it leniently).
  int minuteStep = 5;
  String textAlign;                       // "left", "center", "right"
  String fontId;                          // Font identifier
  String fontWeight;                      // "normal", "bold" (for labels)
  
  // Label properties
  String text;                            // Static text for labels
  int fontSize;                           // Font size in pixels (for labels)
  
  // MqttDataField properties
  String prefix;                          // Text before value
  String postfix;                         // Text after value
  String thousandsSeparator;
  int numberOfDecimals;                   // Number of decimal places for formatted numbers
  
  // MQTTIconField properties
  std::vector<ValueIconPair> valueIconPairs;
  
  // level-indicator properties
  String barDirection;                    // "left-to-right", "right-to-left", "bottom-to-top", "top-to-bottom"
  String displayValue;                    // "none", "percentage", "value", "duration" (dial, gauge)
  String fillColor;                       // Color of filled portion of bar
  // Where the fill runs to along its scale, from fillColor (a theme's second
  // accent); empty is a flat fill. And how far a glow reaches around it, in
  // pixels; 0 is none. Both written by the export only for 24 bit
  // (designer lib/themes.ts withLevelLook, LevelGlow.h).
  String fillEndColor;
  int glow = 0;
  // A level's empty track and the edge round it, from the theme's roles
  // track and trackEdge (designer lib/themes.ts, 2026-09-30). Empty: derived
  // as before - the fill mixed halfway to the background (LevelShape.h
  // trackPaint). "transparent" edge: none.
  String trackColor;
  String trackEdgeColor;
  std::vector<CalibrationPoint> calibrationPoints;

  // arc-level properties. Shares topic, displayValue, fillColor,
  // backgroundColor, textColor, fontId and calibrationPoints with the bar
  // above - it is the same readout on a circular track, and the value
  // mapping is deliberately the identical interpolation.
  String setpointTopic;                   // Optional second value, drawn as a marker; empty = no marker
  String markerColor;                     // The setpoint marker
  String arcDirection;                    // "cw" (default) or "ccw" - which way round the scale runs
  int minAngle;                           // Whole degrees, 0 at twelve o'clock, where the scale starts
  int maxAngle;                           // ... and where it ends. Equal to minAngle means a full ring.
  int arcThickness;                       // Ring thickness in px, inwards from the object's edge
  int markerWidth;                        // Marker width in whole degrees

  // line properties
  int strokeWidth;                        // Line thickness in px (1-10); box border thickness (0-10, 0 = no border)
  String strokeStyle;                     // "solid", "dashed", "dotted" - only "solid" is rendered so far
  // A line's real vertices (segmented/multi-point line) - empty for every
  // line saved before this existed, or a plain two-point line that hasn't
  // been touched by the segmented-line tool; renderLine() then falls back
  // to the (x, y)-to-(x+width, y+height) derivation every line used before,
  // exactly like the designer's own getLinePoints() fallback.
  std::vector<Point> points;
  int filletRadius;                       // Rounds interior vertices (0 = sharp corners)
  bool arrowStart;                        // Filled-triangle arrowhead at points[0] (plain "line" only)
  bool arrowEnd;                          // Filled-triangle arrowhead at points[last] (plain "line" only)

  // MqttDataLine properties - a flow-visualization line: `calibrationPoints`
  // (reused verbatim from level-indicator, see ScreenRenderer::
  // renderMqttDataLine() for why the field stays named barSizePercent) maps
  // abs(topic value) to a stroke width in px; these two independent
  // operator+value conditions decide, each on its own, whether that same
  // topic's signed value shows an arrowhead at that end - unlike `line`'s
  // fixed arrowStart/arrowEnd booleans, or `panel`'s single shared
  // comparisonOperator/Value (one condition is enough there since a panel
  // only ever needs to be shown or not) - two ends need two independent
  // conditions evaluated simultaneously, so this can't reuse that single
  // pair.
  String arrowStartOperator;
  String arrowStartValue;
  String arrowEndOperator;
  String arrowEndValue;

  // box properties
  String strokeColor;                     // Box border color - empty means "no border", same as the
                                           // designer's `if (strokeColor && strokeWidth > 0)` check
  int cornerRadius;                       // Corner radius in px (0 = square corners)
  // Clipped by these, outermost first (`clip`); empty for almost every object.
  std::vector<ClipShape> clips;

  // panel properties - matched against the parent tab-control's `topic`
  // value (that object's own `topic` field, reused rather than duplicated).
  // Same shape as ValueIconPair's comparisonOperator (">", "<", "==", ">=",
  // "<=", "!=") but comparisonValue is a String, not a float - a mode enum
  // like "TEMP" isn't numeric. For the four numeric operators both sides
  // are parsed with toFloat() at evaluation time; for "==" / "!=" the
  // comparison is a trimmed String comparison instead.
  String comparisonOperator;
  String comparisonValue;

  // SoftwareButton properties - the button's own configured action (empty
  // type = configured with no action, still renders/presses visually but
  // dispatchButtonAction() is a no-op for it). Parsed from properties.action
  // by ProjectLoader::parseObjectProperties() via the same parseButtonAction()
  // hardware buttons already use - see ButtonAction's own comment above.
  ButtonAction action;

  // Switch properties - mirrors the designer's obj.properties shape
  // (components/property-panel/switch-properties.tsx): `topic` (already
  // declared above, shared with every other topic-bound type) is the read
  // topic (retained); `writeTopic` is the command destination a tapped
  // segment's writeValue gets published to (not retained).
  //
  // Which form it takes is the object's own type - "button-group" for the
  // n segments side by side, "switch" for the knob in a track. It was a
  // `mode` property here until 2026-09-20; see the designer repo's
  // docs/2026-09-20-control-split.md.
  //
  // `activeBackgroundColor` is the marker bar's colour, solid and hollow
  // alike. It kept its name through the 2026-08-25 change from a filled
  // active segment to a bar: the value saved in every existing project was
  // already the right colour for the new job, and renaming the key would
  // have needed a migration to say nothing new. There is no
  // activeTextColor any more - with no fill, a label never sits on a
  // different background than its neighbours.
  String writeTopic;
  String activeBackgroundColor;
  std::vector<SwitchState> states;

  // The marker's shape on a settable level. Not read since 2026-09-19: a
  // stroke always means a settable value and always looks the same (designer
  // docs/2026-09-19-slider-look.md, decision 4). Old projects still carry it.
  String markerStyle;

  // How thick a level indicator's track is, across the bar, in pixels. Set by
  // the author rather than derived from the object, since a vertical tank made
  // wide enough for its name got a track as wide as the name (designer
  // docs/2026-09-19-slider-look.md, decision 14). 0 means "unset": Material's
  // 16.
  int barThickness;

  // A Switch since 2026-09-20 (designer docs/2026-09-20-switch-look.md): one
  // colour, and how loud the chosen state is - "filled" (default) or "tonal".
  // The container, every label and the quiet track follow from them.
  String switchStyle;
  String switchColor;

  // A settable level (docs/2026-09-17-settable-level.md in the designer
  // repo): `writeTopic` above, shared with the Switch, is what makes a
  // level-indicator or arc-level operable, and `step` is what a value set by
  // a finger snaps to - so a drag reports 35 and 40 rather than 37 and then
  // 38. Default 1; anything <= 0 means no snapping.
  float step;
  // What a set value is published as (designer docs/device-contract.md
  // 2.11, feature "scaled-values"): a text around the value, scaled first.
  // Absent: the number alone. Through levelWritePayload() below, always.
  std::optional<scaled::WriteFormat> writeFormat;

  // Live values (designer docs/device-contract.md 2.6, generation 1.4): a
  // text's `liveText` with `{live:<id>}` where a value stands, read in
  // preference to `text`; an icon is live when `liveIconId` names one of
  // `liveValues`. Each icon result's path is resolved like `path`.
  String liveText;
  std::vector<livevalue::LiveValue> liveValues;
  String liveIconId;

  // An energy diagram's own data (above); empty for every other type, so
  // the other objects pay a pointer for it.
  EnergyDiagramHandle energy;
  // A chart's (above); empty for every other type.
  ChartHandle chart;
  // A software button's look (above); empty for every other type.
  ButtonLookHandle button;
  // A Toggle Button's looks (above); empty for every other type.
  ToggleButtonHandle toggle;
  CameraHandle camera;

  ObjectProperties()
    : numberOfDecimals(2), fontSize(12), strokeWidth(1), cornerRadius(0), filletRadius(0),
      step(1.0f), barThickness(0), arrowStart(false), arrowEnd(false),
      activeBackgroundColor("#2563eb") {}
};

// Screen object (can be live-icon, live-text, switcher, panel, etc.)
struct ScreenObject {
  String type;                // "live-icon", "live-text", "switcher", "panel", etc.
  String id;
  int x;
  int y;
  int width;
  int height;
  int zIndex;
  ObjectProperties properties;
  // Bitmap paths the designer's export flattens onto the object itself
  // (not nested in `properties`) - see lib/project-zip.ts: an "icon"
  // object gets `path`. A "button" gets its look (properties.button).
  // Not present in schaltli-eink's ProjectTypes.h - that firmware
  // never implemented either object type (no touchscreen, e-paper icons
  // were never wired up either), so these are new here for M5 Dial.
  String path;
  // The dark variant's bitmap (ThemeVariant.h); empty: same as light.
  String pathDark;
  // Only meaningful for "switcher" (children must all be "panel") and
  // "panel" (children are arbitrary regular objects) - every other type is
  // always a leaf (empty). Child x/y are relative to this object's own
  // (x, y) origin, not absolute screen coordinates - the renderer
  // accumulates an offset as it descends. A std::vector member of the
  // struct's own type is standard, well-defined C++ (the vector only needs
  // a complete element type at instantiation, not at struct-definition
  // time), no forward-declaration trick required.
  std::vector<ScreenObject> children;
};

// One screen's override for a single button id (e.g. "button-0") - see
// Screen::buttonActions below. A vector of (key, value) pairs rather than
// std::map, matching this codebase's existing convention of linear-scanning
// small vectors (see ProjectLoader::getTopicValue) instead of pulling in
// map's overhead for lists that are realistically a handful of entries.
struct ScreenButtonAction {
  String buttonId;
  ButtonAction action;
};

// Project-wide default action for a physical button. `id` is exactly the
// adornment SVG's own element id (e.g. "button-0") - the designer no longer
// declares a separate hardwareButtons[] list in device.json at all
// (2026-08-16); every id^="button" element in the SVG *is* a hardware
// button, and that same id is what firmware must match here. Only `id` and
// `defaultAction` matter to firmware - the designer's HardwareButton.name
// is designer-UI-only metadata (the button's display label).
struct HardwareButtonDef {
  String id;
  ButtonAction defaultAction;
};

// A navigator (designer docs/device-contract.md 2.7): a strip along an edge
// with an entry per listed screen, each entry ordinary objects relative to
// its top left - `active` for the open screen, `normal` for the others.
// In PSRAM: an entry is a few large ScreenObjects, and a navigator over a
// dozen screens held 87 KB of internal heap with them there (SpiramAllocator.h).
using SpiramObjects = std::vector<ScreenObject, SpiramAllocator<ScreenObject>>;

struct NavigatorEntry {
  String screenId;
  SpiramObjects normal;
  SpiramObjects active;
};

struct Navigator {
  String id;
  String edge;
  int thickness = 0;
  int entryLength = 0;
  String backgroundColor;
  String backgroundColorDark;
  // The line towards the screen (2.7, 2026-10-10); empty: none.
  String borderColor;
  String borderColorDark;
  // The light/dark switch (2.7, generation 1.6): "near", "far" or empty for
  // none, how long its place is, and its objects relative to that place.
  String themeSwitchAt;
  int themeSwitchLength = 0;
  SpiramObjects themeSwitchObjects;
  std::vector<NavigatorEntry> entries;
  // The way back (2.10): on a screen off the main row the one entry, by the
  // way its arrow points (screengraph::Edge order: up, down, left, right).
  // Empty where no screen of this navigator needs that arrow.
  SpiramObjects back[4];
};

// Screen definition
struct Screen {
  String id;
  String name;
  // The navigator this screen shows (2.7); empty: none.
  String navigatorId;
  // Out of next/previous paging, still a goto target; never the start (2.7).
  bool hidden = false;
  // The screen graph (2.10): the id of the screen lying on each edge, in
  // screengraph::Edge order (up, down, left, right), empty where none; the
  // edge the way back lies at ("" on the main row); the pull tab's colour.
  String neighbours[4];
  String back;
  String pullTabColor;
  String pullTabColorDark;
  // The span its charts open with (contract 2.13): "1h", "6h", "24h" or "7d";
  // empty: 24 hours.
  String timeRange;
  String path;                            // Background BMP path (optional)
  String backgroundColor;                 // Screen fill color, e.g. "#ffffff" (optional, defaults to white)
  String backgroundColorDark;             // The dark variant's (ThemeVariant.h); empty: same as light
  std::vector<ScreenObject> objects;
  // Per-screen override of a button's action, keyed by button id (e.g.
  // "button-0") - takes priority over ProjectConfig::hardwareButtons's
  // defaultAction for the same id. Absent for older projects (additive
  // JSON field), in which case every button on this screen just falls back
  // to the project-wide default, same as if this were empty.
  std::vector<ScreenButtonAction> buttonActions;
  // Present only when the designer's device.json declared
  // needsPageIconsInSize AND this screen has an icon assigned - a plain
  // square 8-bit grayscale PGM mask (see
  // ColorAssetLoader::drawGrayscaleMaskToCanvas). The knob's screen menu
  // drew it until the screen graph (contract 2.10) replaced the menu; kept
  // for the knob's hints and map (designer docs/2026-10-17-screen-graph.md,
  // decisions 6 and 15). Empty for every screen otherwise (most
  // projects, and any project built before this field existed) - callers
  // must treat empty as "no icon", not an error.
  String pageIconPath;
  // A popup's frame colours (designer docs/device-contract.md 2.5): the
  // theme's outline for a border, the scrim's colour. Empty on a screen;
  // the board decides whether it uses them.
  String borderColor;
  String borderColorDark;
  String scrimColor;
};

// One field pulled out of a "json"-type topic's payload - mirrors the
// designer's JsonSubtopic (components/project-editor.tsx) field-for-
// field. `path` uses JSONPath's shorthand member/index syntax (RFC 9535) -
// a genuine *subset* of JSONPath, not an invented look-alike: "temp",
// "$.temp", "nested.temp", "readings[0].value", "['odd key.with.dots']"
// are all valid standard JSONPath that a real JSONPath engine would
// evaluate identically (see ProjectLoader::extractJsonField / its
// tokenizeJsonPath() helper for the parser, mirrored exactly in the
// designer's lib/json-path.ts). What's intentionally not implemented is
// the rest of the query language - wildcards, recursive descent, filter
// expressions, slices, unions - which a "pull one named field out of a
// flat sensor payload" use case doesn't need, and which has no
// well-maintained ESP32/Arduino implementation to build on anyway
// (researched 2026-07-25: ArduinoJson's own JSONPath support request,
// upstream issue #821, is still open/unimplemented; the one alternative
// with path-style access, FirebaseJson, would just be a second JSON
// parser pulled in for the same member/index capability ArduinoJson,
// already a dependency, already provides). An object binds to one via the
// composite string "<topic>#<path>" stored in its own properties.topic -
// see ProjectLoader::getTopicValue, which is where that string actually
// gets split and the field extracted.
struct JsonSubtopic {
  String id;
  String path;
  String type;                            // "numeric" or "text" - the VALUE type once extracted
};

// MQTT Topic definition
struct MQTTTopic {
  String id;
  String topic;
  String type;                            // "numeric", "string", "boolean", "json"
  std::vector<String> examples;
  String currentValue;                    // Current value (updated by MQTT later) - for "json", the whole raw payload
  // Whether a message has arrived on this topic since the project loaded. A
  // placeholder's `??` fallback stands only for a value that never arrived;
  // an empty message that did arrive shows empty (designer
  // docs/2026-09-25-text-placeholders.md).
  bool arrived = false;
  std::vector<JsonSubtopic> subtopics;     // only meaningful when type == "json"
  // A topic the display keeps itself (designer contract 2.9): never
  // subscribed, a write is its value and is never published. It starts at
  // `initial` after every load where the project gives one.
  bool local = false;
  String initial;
};

// Font definition with metrics
struct Font {
  String id;                              // Font ID (e.g., "font-4")
  String name;                            // Full font name
  String displayName;                     // Display name for UI
  String internalName;                    // U8g2 font name (e.g., "u8g2_font_helvR08_tf")
  int size;                               // Total line height in pixels
  int ascent;                             // Height above baseline
  int descent;                            // Height below baseline
};

// Main project configuration
struct ProjectConfig {
  String name;
  int screenWidth;
  int screenHeight;
  String exportColorDepth;
  std::vector<MQTTTopic> topics;
  std::vector<Font> fonts;
  std::vector<Screen> screens;
  // Popups (designer docs/device-contract.md 2.5), apart from screens so that
  // nothing paging through `screens` ever meets one. Where a screen index is
  // taken - rendering, hit tests, button actions - a popup is addressed as
  // screens.size() + its position here; see screenOrPopupAt().
  std::vector<Screen> popups;
  PopupFence popupFence;
  std::vector<Navigator> navigators;
  // The main row's screen ids in order (2.10): what next and previous walk.
  // Empty in an export from before it - then the screens that are not
  // hidden, which is what it is for such a project (ScreenGraphProject.h).
  std::vector<String> mainRow;
  std::vector<HardwareButtonDef> hardwareButtons;
  // Combined topics (contract 2.6), in evaluation order - each after every
  // one it reads. The device's own: computed, never published.
  std::vector<combined::CombinedTopic> combinedTopics;
  // How a placeholder's F and N formats write a number (designer
  // docs/device-contract.md 2.4): one character each, the thousands one may
  // be empty. An export from before 2026-10-05 has neither: Switzerland's.
  String decimalSeparator = ".";
  String thousandsSeparator = "'";
  // Which variant the pairs above currently hold, and which pairs
  // ThemeVariant.h swapped to get there - kept so that exactly those are
  // swapped back.
  bool themeDark;
  std::vector<bool> themeSwapped;

  ProjectConfig() : screenWidth(0), screenHeight(0), themeDark(false) {}
};

// The screen or popup behind an index: 0..screens.size()-1 a screen, from
// screens.size() on a popup. nullptr outside both.
inline const Screen* screenOrPopupAt(const ProjectConfig& project, int index) {
  const int screens = (int)project.screens.size();
  if (index < 0) return nullptr;
  if (index < screens) return &project.screens[index];
  if (index - screens < (int)project.popups.size()) return &project.popups[index - screens];
  return nullptr;
}

inline bool isPopupIndex(const ProjectConfig& project, int index) {
  return index >= (int)project.screens.size() && screenOrPopupAt(project, index) != nullptr;
}

// The index of the popup with this id, or -1.
inline int popupIndexById(const ProjectConfig& project, const String& id) {
  for (size_t i = 0; i < project.popups.size(); i++) {
    if (project.popups[i].id == id) return (int)(project.screens.size() + i);
  }
  return -1;
}

// What a set level publishes for `value` - a finger, a knob's turn, a
// button's adjust-level alike: its write format's text around the value,
// scaled first (designer docs/device-contract.md 2.11). False where the value
// cannot be scaled: nothing is sent then. What is drawn and asked stays
// `value`.
inline bool levelWritePayload(const std::optional<scaled::WriteFormat>& format, const String& value, String& out) {
  const auto sent = scaled::formatWrite(value.c_str(), format ? &*format : nullptr);
  if (!sent) return false;
  out = String(sent->c_str());
  return true;
}
