#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "BdfFont.h"
#include "DdfFontStore.h"
#include "../interfaces/IProjectLoader.h"
#include "ProjectTypes.h"
#include "ScreenGraph.h"
#include "../ClippedCanvas16.h"
#include "ColorAssetLoader.h"
#include "LevelShape.h"
#include "PillRaster.h"
#include "ArcRaster.h"
#include "NavigatorLayout.h"
#include "EnergyDiagram.h"
#include "PolygonRaster.h"
#include "Keyboard.h"
#include "TimePicker.h"
#include <array>
#include <functional>

// Color (RGB565) counterpart of schaltli-eink's ScreenRenderer -
// renders into a ClippedCanvas16 instead of a 1-bit ClippedCanvas1, real
// hex colors instead of black/white thresholding, otherwise mirrors that
// class's carefully HIL-tuned pixel logic (drawTextBox's background/
// border/text order and +1/center-align math, measureTrueTextWidth's
// dry-run measurement technique, u8g2 setFont-before-setFontMode
// ordering) as closely as possible - those fixes are color-depth-
// independent, so there's no reason to rediscover them here.
//
// Every object type in the M5 Dial DDF's supportedObjectTypes is now
// ported: box, line (now full parity with the e-paper reference - fillet,
// thick strokes, fixed arrowheads), label, MqttDataField, level-indicator,
// MQTTIconField, icon, SoftwareButton, tab-control, panel, MqttDataLine.
// icon and SoftwareButton have no e-paper equivalent to port from -
// schaltli-eink never implemented either (no touchscreen, and its own
// "icon" object type was never wired up despite existing in the schema) -
// so their rendering (draw the already-exported bitmap via ColorAssetLoader,
// always the "normal" state for SoftwareButton until real touch tracking
// exists) was designed fresh from the designer's own export shape
// (lib/project-zip.ts's path/pathNormal/pathActive fields), not ported from
// existing firmware logic.
//
// tab-control/panel/MqttDataLine (2026-08-14 port from schaltli-eink's
// ScreenRenderer) reuse that firmware's logic near-verbatim, swapping only
// parseColor()'s black/white result for parseHexColor()'s real RGB565 one -
// the geometry (fillet math, thick-line rasterization, arrowhead placement,
// panel-condition evaluation) is color-depth-independent. NOTE: touch
// hit-testing (main.cpp's findSoftwareButtonAt/findSwitchSegmentAt) is
// still top-level-only and does NOT recurse into a tab-control's active
// panel - a SoftwareButton or Switch nested inside a panel renders
// correctly but isn't tappable yet. Out of scope here (this port covers
// rendering only, per the request that introduced it); see those
// functions' own comments.
// One pill-shaped run and the colour it is painted in - the counterpart of
// PaintedPill in the designer's components/canvas/renderers/paint-pills.ts.
//
// It lives here rather than in PillRaster.h because the designer keeps colour
// out of lib/pill-raster.ts too: that file is geometry, and where the geometry
// is painted is the renderer's business.
//
// `paints == false` is a hole: the run claims its pixels - nothing behind it
// shows through - but paints nothing, which is how an outline is described
// (the outer pill in the outline's colour, the inside knocked out of it).
struct PaintedPill {
  PillRaster::Band band;
  uint16_t colour = 0;
  bool paints = true;
  // Painted in the gradient of the PillShading handed to paintPills rather
  // than in `colour` - a fill that runs from one colour to another along its
  // bar (LevelGlow.h).
  bool graded = false;
};

// A bar's fill as a gradient, and the glow around its filled part - what
// PaintedPill.colourAt and PillGlow are in the designer's paint-pills.ts, as
// numbers rather than callbacks. Positions are whole pixels along the track's
// axis, the glow's capsule in 1/8 pixel.
struct PillShading {
  ArcRaster::Rgb565 from;
  ArcRaster::Rgb565 to;
  bool vertical = false;
  bool fromEnd = false;
  int32_t trackStart = 0;
  int32_t trackLength = 1;
  // The glow: none where glowPx is 0.
  int32_t glowPx = 0;
  int32_t edge = 0;
  int32_t half = 0;
  int32_t cross = 0;
  int32_t a = 0;
  int32_t b = 0;
  // The object's own box, which the glow never leaves.
  int32_t boxX = 0;
  int32_t boxY = 0;
  int32_t boxW = 0;
  int32_t boxH = 0;
};

class ChartFeed;

class ColorScreenRenderer {
public:
  // Where a chart's history comes from (ChartFeed.h); none, and a chart says so.
  void setChartFeed(ChartFeed* feed) { chartFeed_ = feed; }
  // The NowPoint's pulse (designer lib/chart-shapes.ts ShapeLook.pulse): the
  // phase 0..1 every chart is drawn at, below 0 none. Set by a board that
  // animates, before each render, so a region drawn again matches the rest.
  void setChartPulse(double phase) { chartPulse_ = phase; }
  // Where the NowPoints of the charts drawn last stand, on the screen, with
  // room for the widest ring: what a pulse step draws again.
  struct PulseBox {
    int16_t x, y, w, h;
  };
  const std::vector<PulseBox>& chartPulseBoxes() const { return chartPulseBoxes_; }
  void clearChartPulseBoxes() { chartPulseBoxes_.clear(); }
  ColorScreenRenderer(IProjectLoader& projectLoader, ClippedCanvas16* canvas);

  // Redirects rendering to another canvas of the same size. Used to draw the
  // incoming screen of a swipe transition into CanvasPresenter's second
  // buffer while the outgoing one is still on display. A setter rather than
  // a second renderer instance because this one caches decoded assets - two
  // instances would decode and hold every image twice.
  // Text goes through canvas_ like everything else now - BdfFont writes
  // pixels directly - so there is no second drawing target to keep in
  // step. The u8g2 wrapper held its own pointer, bound once in the
  // constructor, and repointing canvas_ alone sent every glyph to the old
  // canvas: a screen rendered into the transition buffer arrived complete
  // except for its text. That whole class of bug goes with the second
  // target.
  void setCanvas(ClippedCanvas16* canvas) { canvas_ = canvas; }

  // A popup (index as screenOrPopupAt(), ProjectTypes.h) drawn over what the
  // canvas holds: its background inside the fence, then its objects. The
  // screen underneath stays; dimOutsidePopupFence() sets it back first on an
  // RGB board (designer docs/device-contract.md 2.5).
  // borderPx: how wide the fence's edge is drawn in the popup's border colour
  // (0 for none) - the board's choice.
  bool renderPopup(int popupIndex, int borderPx = 2, const String& pressedButtonId = "",
                   const String& pendingSwitchId = "", int pendingSwitchStateIndex = -1,
                   const String& pressedSwitchId = "", int pressedSwitchStateIndex = -1);
  void drawPopupBorder(const Screen& popup, int borderPx);
  // The round close button on the fence's edge (PopupFence.h), drawn over a
  // popup's objects by renderPopup() and by a region render that reaches it.
  // Radius 0, the default, draws none: a board opts in.
  void setPopupCloseBadgeRadius(int radius) { popupCloseBadgeRadius_ = radius; }
  PopupCloseBadge popupCloseBadge() const;
  void drawPopupCloseBadge(const Screen& popup);
  // A project colour as the canvas takes it - for a board drawing a popup's
  // frame itself.
  uint16_t rgb565Of(const String& hex, bool* isTransparent = nullptr) const { return parseHexColor(hex, isTransparent); }
  void dimOutsidePopupFence();
  // Every pixel of the canvas at half: the scrim over the whole screen, for a
  // board whose popup grows from its button - the scrim reaches up to the
  // window at every size on the way, not only at the last.
  void dimCanvas();

  // What `{device:id}` shows in a text: the MQTT client id, which the
  // renderer has no way to know by itself. Until it is set, empty.
  void setDeviceId(const String& id) { deviceId_ = id; }

  // A text with its placeholders resolved - `{topic:…}` from the topic's
  // last value, `{device:model}` and `{device:id}`, the project's
  // separators (designer docs/2026-10-05-placeholder-devices.md). A text
  // without a `{` comes back as it is.
  String resolvePlaceholders(const String& text) const;

  // A live value's source as it reads now: a topic's last value, the
  // device's model or id, the project's name, a combined topic's yes/no.
  // nullopt where nothing has arrived yet.
  std::optional<std::string> liveSourceValue(const livevalue::Source& source) const;

  // Whether an object without a binding of its own reads `topic` (bare, no
  // `#json.path`): a text naming it in a placeholder or through one of its
  // live values, a live icon through its live value - directly, or through a
  // combined topic and the ones that one reads (designer
  // docs/device-contract.md 2.6). What a partial redraw asks when that
  // topic changes; `combinedTopics` is the project's.
  static bool readsTopic(const ScreenObject& obj, const String& topic,
                         const std::vector<combined::CombinedTopic>& combinedTopics);

  // How far a text object's ink can reach above its box, whatever it reads:
  // drawTextBox lets accents overhang (inkOverhang), so a redraw of the box
  // alone left the dots of a new «ä» undrawn, or an old one's standing. The
  // font-wide most rather than the text's - a region a few rows too tall
  // costs nothing, one too short leaves stale pixels.
  int16_t textOverhangLimit(const ScreenObject& obj);

  // The navigator a screen shows (contract 2.7), laid out on this display;
  // false when it shows none. `nav` gets the navigator, `active` the entry
  // of the screen itself (-1 if it has none).
  bool navigatorLayout(int screenIndex, navigatorlayout::Layout& layout, const Navigator** nav = nullptr, int* active = nullptr) const;
  // How far the navigator is scrolled; -1: as far as shows the open entry.
  void setNavigatorScroll(int scroll) { navigatorScroll_ = scroll; }
  int navigatorScroll() const { return navigatorScroll_; }
  // The scroll the navigator is drawn at on that screen.
  int effectiveNavigatorScroll(int screenIndex) const;
  // Whether the project is drawn in its dark variant - what a picture of the
  // navigator kept for later has to be keyed by.
  bool themeDark() const { return projectLoader_.isLoaded() && projectLoader_.getProject().themeDark; }
  // The panel a switcher shows now (getActivePanel), for a caller bounding
  // what a value changes inside it.
  const ScreenObject* visiblePanel(const ScreenObject& switcher) const { return getActivePanel(switcher); }
  // The navigator's marker gliding with a swipe (designer #59, contract
  // 2.7): its bar drawn into the canvas alone, as the screen shows it, with
  // every entry normal, or with every entry active on the marker's colour -
  // a board composes a frame of the two. False when the screen shows none.
  enum class StripLook { AsShown, AllNormal, AllActive };
  bool renderNavigatorStrip(int screenIndex, StripLook look);
  // The marker: the box an entry's active look begins with, within an
  // entry, and its corners. False when there is none.
  bool navigatorMarkerBox(int screenIndex, navigatorlayout::Rect& box, int& radius) const;
  // The screen a tap on entry `entry` of that screen's navigator opens: the
  // entry's, or on a screen off the main row the way back (contract 2.10).
  // Empty for none.
  String navigatorEntryScreenId(int screenIndex, int entry) const;

  // The pull tabs (contract 2.10): how far they stand nudged towards the
  // middle, and for each of a screen's tabs the rectangle it covers at any
  // nudge - what a nudge has to redraw. None on a screen without tabs.
  void setPullTabNudge(int nudge) { pullTabNudge_ = nudge; }
  int pullTabNudge() const { return pullTabNudge_; }
  std::vector<screengraph::Rect> pullTabRegions(int screenIndex) const;

  // Every topic (bare) an object's live values read, combined topics
  // followed to the topics under them - what a device subscribes to.
  // An energy diagram (contract 2.8) at a moment of its motion: the wave's
  // phase and each line's dots' (by node id); none - everything at phase 0.
  using EnergyDotPhases = std::vector<std::pair<std::string, double>>;
  bool renderEnergyDiagram(const ScreenObject& obj, float wavePhase = 0, const EnergyDotPhases* dots = nullptr);
  // A chart (contract 2.13, ColorScreenRendererChart.cpp), from the history
  // the board's ChartFeed holds.
  bool renderChart(const ScreenObject& obj);
  // How often a diagram has been drawn: a board moving one takes its
  // measure again when this changes (a value, a screen, a region redraw).
  uint32_t energyDrawCount() const { return energyDrawCount_; }
  // How often a switcher was drawn: a panel shown anew may hold a diagram,
  // or no longer hold one, and the animations are worked out again.
  uint32_t switcherDrawCount() const { return switcherDrawCount_; }
  // false on a board that animates nothing (the PaperS3, its DDF's
  // screen.motion false): every diagram drawn still, dots at phase 0, as
  // with Motion off - as the designer draws it for that device.
  void setAnimates(bool animates) { animates_ = animates; }
  // Where the last diagram drawn spent its time, for /api/debug.
  const String& energyProfile() const { return energyProfile_; }

  // What moves in a diagram with Motion on (spec decision 10), worked out
  // from the values now: a Sankey's bands as the runs of whole pixels a
  // column's colour is painted into, or a Flow's lines that carry watts.
  // Places are on the canvas. Drawn for motion, a diagram's still picture
  // has no dots: the board draws them, and never into the canvas.
  struct EnergyAnimation {
    String id;
    bool sankey = false;
    struct Run {
      int16_t y, x0, x1;
      uint8_t band;
    };
    std::vector<Run> runs;
    std::vector<std::array<int, 3>> bandRgb;
    std::array<int, 3> ground{{255, 255, 255}};
    int16_t originX = 0;  // the diagram's x: a column's colour is by its x in the diagram
    struct Line {
      std::string id;
      energydiagram::FlowLine line;  // on the canvas
      bool gives = false;
      double watts = 0;
      uint16_t colour = 0;
      // A storage's: its dots run on into its track (energydiagram::storageDots).
      bool storage = false;
      energydiagram::FlowNode node;  // on the canvas
    };
    std::vector<Line> lines;
  };
  std::vector<EnergyAnimation> energyAnimations(int screenIndex);
  // The action of the node a tap at (x, y) in the diagram lands on, if it
  // has one (decision 13); never the bus.
  const ButtonAction* energyActionAt(const ScreenObject& obj, double x, double y) const;
  // A dot's pixels at (cx, cy) on the canvas, each with its coverage 1..16.
  static void energyDotPixels(double cx, double cy, const std::function<void(int16_t, int16_t, uint8_t)>& pixel);
  static uint16_t energyBlend(uint16_t under, uint16_t over, uint8_t coverage);
  // A band's colour at canvas column x at a wave phase.
  static uint16_t energyWaveColour(const EnergyAnimation& a, uint8_t band, int16_t x, double wave);
  static std::vector<std::string> liveTopics(const ScreenObject& obj,
                                             const std::vector<combined::CombinedTopic>& combinedTopics);

  // Parses every font the loaded project references, up front.
  //
  // Fonts load lazily on first use otherwise, and that first use is inside
  // a render - measured at 275ms for one .bdf, on top of the 177ms the
  // render itself takes. Paid at boot that is invisible; paid when a drag
  // starts pre-rendering a neighbouring screen it is a stall in the middle
  // of a gesture, which is the exact fault the neighbour pre-render exists
  // to remove.
  void preloadFonts();
  // The font ids one screen's objects name (masters come merged in).
  std::vector<String> fontIdsOf(int screenIndex) const;
  // For /api/debug: fonts parsed, let go since boot, PSRAM free, the reserve.
  String fontReport() const;
  ClippedCanvas16* canvas() const { return canvas_; }

  // Renders every object on the given screen index into the canvas
  // (z-index order, matching sortChildrenByZIndex on the designer side).
  // Does NOT blit to the display - caller does that once after this
  // returns, same as the color-canvas spike (checkpoint 2a).
  // pressedButtonId: if non-empty and it matches a SoftwareButton's own id
  // on this screen, that one button draws pathActive instead of pathNormal
  // - see main.cpp's touch handling for why a full re-render (not a
  // partial single-object update) is used for this: cheap enough on a
  // color LCD with no e-paper ghosting concern to optimize around, same
  // reasoning as the existing MQTT-triggered redraw path.
  // pendingSwitchId/pendingSwitchStateIndex: while a Switch's tapped
  // segment is awaiting MQTT confirmation (main.cpp's 3s pending/timeout
  // state machine), that one segment draws the pending look instead of its
  // normal active/inactive one - same "pass the one runtime highlight down
  // through render" pattern as pressedButtonId, just keyed by (object id,
  // segment index) instead of just an id, since a Switch has more than one
  // sub-region that can independently need this.
  // pressedSwitchId/pressedSwitchStateIndex: while a Switch segment is
  // actively being touched (finger still down, before release) - draws a
  // blended background/text color (halfway between the segment's normal
  // and active colors) so a press is felt immediately, distinct from both
  // the plain idle look and the thicker-bordered pending look that
  // replaces it the moment the finger lifts (2026-08-14 addition, agreed
  // in this session's follow-up design discussion).
  // Zeichnet nur ein Rechteck des Bildschirms neu - Hintergrund und alle
  // Objekte, aber auf den Ausschnitt begrenzt. Innerhalb des Rechtecks ist
  // das Ergebnis bitgleich zu renderScreen(), weil dieselbe Reihenfolge auf
  // denselben Hintergrund gezeichnet wird; ausserhalb bleibt das vorige Bild
  // stehen. Die Pixel-Gleichheit mit dem Designer bleibt damit erhalten.
  // Die Rueckmeldeparameter sind dieselben wie bei renderScreen() und
  // werden genauso durchgereicht. Sie haben Vorgabewerte, weil dieser
  // Ausschnitt-Render bis 2026-09-10 gar keine kannte: er zeichnete jedes
  // Objekt ungedrueckt, was fuer den Bogen-Ausschnitt des Knobs richtig war
  // und einen gedrueckten Knopf unmoeglich machte. Ein Tastendruck ist genau
  // der Fall, fuer den sich ein Vollbild nicht lohnt.
  bool renderScreenRegion(int screenIndex, int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                          const String& pressedButtonId = "", const String& pendingSwitchId = "",
                          int pendingSwitchStateIndex = -1, const String& pressedSwitchId = "",
                          int pressedSwitchStateIndex = -1);

  // Welches Rechteck sich aendert, wenn dieser Ring von oldValue auf newValue
  // geht - Bogenenden plus die Zahl in der Mitte. Liefert false, wenn sich
  // nichts aendert oder der Fall nicht sicher eingegrenzt werden kann; dann
  // muss der Aufrufer ein Vollbild zeichnen.
  // Das Rechteck der Zahl in der Mitte, getrennt vom Bogenabschnitt.
  bool arcLevelTextRect(const ScreenObject& obj, const String& oldValue, const String& newValue,
                        int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);

  bool arcLevelDirtyRect(const ScreenObject& obj, const String& oldValue, const String& newValue,
                         int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);
  // The middle of a ring where its numbers stand - both of them, the asked one
  // and the measured one under it - across the whole hole.
  bool arcLevelCentreRect(const ScreenObject& obj, int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);
  // A bar or slider going from oldValue to newValue: the stretch of it between
  // the two positions - fill edge, handle or pointer, and the glow's end -
  // across the whole object, and the header or number column beside it.
  // Writes up to two rectangles, returns how many, or -1 where it cannot say.
  int barLevelDragRects(const ScreenObject& obj, const String& oldValue, const String& newValue, int16_t* rx,
                        int16_t* ry, int16_t* rw, int16_t* rh);

  bool renderScreen(int screenIndex, const String& pressedButtonId = "", const String& pendingSwitchId = "",
                     int pendingSwitchStateIndex = -1, const String& pressedSwitchId = "",
                     int pressedSwitchStateIndex = -1);

  // Was an dieser Stelle obenauf liegt und einen Tipp annehmen kann, oder
  // nullptr. `objX`/`objY` bekommen die ABSOLUTE linke obere Ecke des
  // Treffers - der Aufrufer braucht sie fuer die Segmentrechnung, und bei
  // einem Objekt in einem Panel ist sie nicht obj.x/obj.y.
  //
  // Hier statt in main.cpp, weil die Antwort an zwei Dingen haengt, die
  // bereits hier leben: welches Panel eines tab-control gerade sichtbar ist
  // (getActivePanel), und um wieviel dessen Kinder verschoben gezeichnet
  // werden. Eine zweite Kopie hiesse, dass Finger und Bild ueber
  // verschiedene Geometrien reden - dieselbe Begruendung, aus der
  // getActiveSwitchStateIndex() am 2026-08-25 oeffentlich wurde.
  const ScreenObject* hitTestTappable(int screenIndex, int16_t x, int16_t y,
                                      int16_t& objX, int16_t& objY) const;

  // Was der Renderer auf diesem Screen SIEHT, eine Zeile je Objekt.
  //
  // Entstanden 2026-09-10, weil ein MqttDataField auf dem Knob gar nichts
  // zeichnete - kein Text, kein Hintergrund - und von aussen nicht zu
  // unterscheiden war, ob es fehlt, ob seine Schrift fehlt oder ob sein Wert
  // leer ist. Genau diese drei Faelle sehen auf dem Glas identisch aus, und
  // eine Stunde Codelesen hat sie nicht getrennt.
  String describeScreen(int screenIndex);

private:
  // Laeuft in Zeichenreihenfolge; der LETZTE Treffer gewinnt, weil zuletzt
  // gezeichnet heisst obenauf. Ueber Ebenen hinweg ist das richtig, ein
  // Vergleich roher zIndex-Werte waere es nicht: der zIndex eines
  // Panel-Kindes gilt unter seinen Geschwistern, nicht gegen die oberste
  // Ebene.
  void hitTestWalk(const std::vector<ScreenObject>& objects, int16_t dx, int16_t dy,
                   int16_t x, int16_t y, const ScreenObject*& treffer,
                   int16_t& objX, int16_t& objY) const;

  IProjectLoader& projectLoader_;
  ClippedCanvas16* canvas_;
  String deviceId_;
  int popupCloseBadgeRadius_ = 0;
  // Mutable: a const metric lookup may parse the font that stands in for a
  // replaced one (standIn).
  mutable DdfFontStore fontStore_;
  // The screen whose fonts trim() last kept, so a redraw of the same screen
  // does not look again.
  int fontsKeptFor_ = -1;

  bool renderObject(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                     int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);
  // renderObject without the clip of a free area around it (`clip`).
  bool renderObjectUnclipped(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                             int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);
  // The last clip shape's row spans [start, end), read once from the pixels
  // Adafruit_GFX::fillRoundRect sets for it (docs/2026-11-03-card-clip.md).
  std::vector<std::pair<int16_t, int16_t>> clipSpans_;
  int clipSpansW_ = -1, clipSpansH_ = -1, clipSpansR_ = -1;
  const std::vector<std::pair<int16_t, int16_t>>* clipSpansFor(int w, int h, int r);
  bool renderBox(const ScreenObject& obj);
  void energyInputs(const EnergyDiagramProps& e, std::vector<energydiagram::Node>& nodes,
                    std::vector<energydiagram::Value>& values) const;
  uint32_t energyDrawCount_ = 0;
  uint32_t switcherDrawCount_ = 0;
  bool animates_ = true;
  String energyProfile_;
  bool renderLine(const ScreenObject& obj);
  bool renderLabel(const ScreenObject& obj);
  bool renderMQTTDataField(const ScreenObject& obj);
  bool renderLevelIndicator(const ScreenObject& obj);
  // The object's font, as the number column's measure - ascent, descent and
  // the height of a capital.
  LevelFontMetrics levelMetrics(const ScreenObject& obj);

  // One run of text inside the level indicator's number column, clipped to it.
  // Mirrors drawLevelTextIn() in the designer's render-level-indicator.ts.
  void drawLevelText(const String& text, const LevelRect& clip, int x, int baseline, const String& fontId,
                     uint16_t color);
  // The smaller font a ring's measured value is written in, out of the
  // project's own list - a BDF font cannot be scaled, so "smaller" means
  // another font. Mirrors levelSubFont() in the designer's
  // render-level-indicator.ts.
  String levelSubFontId(const ScreenObject& obj);
  // The round counterpart of the bar. `mixInto` is what its anti-aliased
  // edges blend against where the object's own background is transparent -
  // a declared colour rather than whatever happens to be on the canvas,
  // because the designer cannot read its canvas back at an arbitrary zoom
  // and an edge that mixes into different things on the two sides is an
  // edge that fails the pixel comparison.
  bool renderArcLevel(const ScreenObject& obj, uint16_t mixInto);

  // Paints a set of pill-shaped runs, anti-aliased, in one pass - this repo's
  // copy of the designer's components/canvas/renderers/paint-pills.ts.
  //
  // The runs come in priority order - a handle before the track it lies on, a
  // hole before the ring it cuts - because every sub-sample is counted into
  // exactly one of them (PillRaster.h). That is the whole reason this exists
  // rather than a sequence of fillRoundRect calls: where two runs meet,
  // painting one over the other leaves a seam of the lower one's colour along
  // the join.
  //
  // Straight onto the canvas, not through a buffer of its own: there is no
  // zoom here, so there is no second softening to avoid - the designer draws
  // into an offscreen buffer only because its canvas is scaled.
  //
  // `mixInto` is what a soft edge blends against where nothing of the control
  // covers a pixel: a declared colour rather than whatever happens to be on
  // the canvas, exactly as renderArcLevel's own mixInto is, and for the same
  // reason - the designer cannot read its canvas back at an arbitrary zoom.
  //
  // Only ever called where PILL_ANTIALIASED is 1. Below 24 bit the callers
  // keep their whole-pixel path; see PillRaster.h for why that is a decision.
  void paintPills(const std::vector<PaintedPill>& painted, uint16_t mixInto,
                  const PillShading* shading = nullptr);

  // The background renderScreen() filled before drawing any object.
  uint16_t currentScreenBg_ = 0xFFFF;
  int navigatorScroll_ = -1;
  StripLook stripLook_ = StripLook::AsShown;
  // The screen's navigator over what is drawn, clipped to the region in
  // force if there is one (renderScreen, renderScreenRegion call it last).
  void renderNavigatorOver(int screenIndex);
  // The pull tabs over the screen, at pullTabNudge_ (renderScreen,
  // renderScreenRegion; clipped to the region in force).
  void renderPullTabs(int screenIndex);
  int pullTabNudge_ = 0;
  // The arrow (screengraph::Edge) of the way back a navigator draws on this
  // screen, or -1 on the main row or without one.
  static int backArrow(const Navigator& nav, const Screen& screen);
  // The same colour as the author wrote it, which is what a derived colour has
  // to be mixed from: going through 565 first and back would land a step away
  // from the designer's own mix (src/project/MaterialColors.h).
  String currentScreenBgHex_ = "#ffffff";
  bool renderMQTTIconField(const ScreenObject& obj);
  bool renderIcon(const ScreenObject& obj);
  bool renderSoftwareButton(const ScreenObject& obj, bool isPressed);
  bool renderSwitch(const ScreenObject& obj, int pendingStateIndex, int pressedStateIndex);
  // A text field (TextField.h): container, line, the text or its dots.
  bool renderTextField(const ScreenObject& obj);
  // A time field (TimeField.h): the text field's drawing, its text the time.
  bool renderTimeField(const ScreenObject& obj);
  // The field and `value` in it, a text field's and a time field's alike.
  void drawField(const ScreenObject& obj, const String& value, bool hidden);
  // What a field shows: the asked value until its topic answers, then the topic's.
  String fieldValue(const ScreenObject& obj) const;

 public:
  // The keyboard a tapped text field opens (Keyboard.h), drawn over the
  // screen into `frame`: the field's colours - its container behind the
  // keys, the screen's background on them, its text on their faces, its
  // line on OK - the text so far ending at the line's right edge, a dot per
  // character where the field hides its text, and the count. The board
  // presents the frame's rectangle afterwards.
  void renderKeyboard(const keyboard::State& state, const ScreenObject& field, const keyboard::Frame& frame);

  // The picker a tapped time field opens (TimePicker.h), drawn over the
  // screen into `frame`, in the field's colours as the keyboard is: wheels
  // with their values where the finger has them, or a button above and
  // below each value; Cancel and OK. The board presents frame.panel after.
  void renderTimePicker(const timepicker::State& state, const ScreenObject& field, const timepicker::Frame& frame);

 private:
  // The project's largest font whose line fits `height` with room around it;
  // the smallest where none does.
  // `maxWidth` > 0: `needs` must also fit that wide.
  const BdfFont* keyboardFont(int height, int& ascent, int& descent, const String& needs = String(), int maxWidth = 0);
  // Switch marker bar - solid for a confirmed marked state, hollow while a
  // write is unconfirmed. Geometry constants live next to the definition in
  // ColorScreenRenderer.cpp, mirroring render-switch.ts.
  bool renderMqttDataLine(const ScreenObject& obj);

  // Recurses into the active panel's children (offset-copies with
  // obj.x/obj.y added, since child coordinates are relative to the
  // tab-control's own origin) via renderObject() - draws nothing itself if
  // no panel matches. Mirrors schaltli-eink's ScreenRenderer::
  // renderTabControl() and components/canvas/renderers'
  // renderScreenObjects()'s "tab-control" case on the designer side. Takes
  // the same touch-state params renderObject() does, so a SoftwareButton/
  // Switch nested inside the active panel still renders its pressed/pending
  // look correctly even though it can't be hit-tested yet (see this file's
  // header comment).
  bool renderTabControl(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                         int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);

  // Walks tabControl.children (each expected type "panel") in zIndex order,
  // returns a pointer to the first whose condition matches the
  // tab-control's own topic value, nullptr if none match. Pointer is valid
  // as long as tabControl's own children vector isn't mutated/destroyed -
  // callers only ever use it within the same renderObject() call. Mirrors
  // schaltli-eink's ScreenRenderer::getActivePanel() exactly.
  const ScreenObject* getActivePanel(const ScreenObject& tabControl) const;

  bool evaluateCondition(float value, const String& op, float threshold) const;

  // Generalization of evaluateCondition() for panel matching: "==" / "!="
  // compare as trimmed Strings (an enum mode like "TEMP" isn't numeric, so
  // the float-only evaluateCondition() can't distinguish "TEMP" from
  // "POWER" - both toFloat() to 0.0), the four numeric operators delegate
  // to the existing evaluateCondition() via .toFloat() on both sides.
  // Mirrors schaltli-eink's ScreenRenderer::evaluateVisibilityCondition()
  // and lib/render-screen.ts's evaluateCondition() on the designer side
  // exactly, the 0.0-not-NaN fallback included.
  bool evaluateVisibilityCondition(const String& actualValue, const String& op, const String& comparisonValue) const;

  String getIconPathForValue(const ScreenObject& obj, const String& valueStr) const;
  // Index into obj.properties.states whose readValue (trimmed) equals the
  // Switch's own topic's current value (also trimmed) - mirrors the
  // designer's getActiveSwitchStateIndex() (render-switch.ts) exactly:
  // plain trimmed String equality, not evaluateCondition()'s numeric
  // operators (a state's readValue is arbitrary text, e.g. "high", not
  // necessarily a number). -1 if no topic is set, no state matches, or no
  // retained value has arrived yet.
  //
  // Public since 2026-08-25: main.cpp's tap handler needs it to work out
  // which state a single-area Switch should advance to. Recomputing "which
  // state is active" over there would be a second copy of the rule, and the
  // segment-width comment in dispatchTapAt already records what happens
  // when the drawing and the hit-testing disagree about the same geometry.
 public:
  int getActiveSwitchStateIndex(const ScreenObject& obj) const;

 private:

  void drawTextBox(const ScreenObject& obj, const String& displayText, bool drawBorder = true);

  // Line-geometry helpers, ported near-verbatim from schaltli-eink's
  // ScreenRenderer (see that class's own comments for the full derivation
  // of each - fillet tangent/arc math, thick-line rasterization via a
  // rotated-rectangle inside-test, arrowhead placement/shortening). Shared
  // by renderLine() (fixed arrowStart/arrowEnd) and renderMqttDataLine()
  // (data-driven arrow conditions, data-driven stroke width) - identical to
  // how the e-paper reference shares them between its own two callers.

  // Fills a strokeWidth>1 line as a rotated rectangle with butt caps.
  void fillThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, float width, uint16_t color);

  // Rounds every interior vertex of a multi-point line via a true circular
  // arc between tangent points (not a Bezier approximation - see the e-paper
  // reference's 2026-07-30 finding for why a Bezier looked visibly wrong at
  // sharp angles). `drawSegment` is whatever renderLine()/renderMqttDataLine()
  // would otherwise call directly per straight segment.
  void renderFilletedLine(const std::vector<Point>& points, int filletRadius,
                           const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment);

  // Straight segments when filletRadius<=0 or too few points, else
  // delegates to renderFilletedLine().
  void renderLineBody(const std::vector<Point>& points, int filletRadius,
                       const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment);

  // A filled-triangle arrowhead pointing from `from` toward `tip`.
  void drawArrowhead(const Point& tip, const Point& from, float strokeWidth, uint16_t color);

  // The point a strokeWidth-wide line body should actually stop at when
  // drawing toward an end that shows an arrowhead - short of the true tip,
  // so the body's straight edges don't poke out past the arrowhead's
  // tapering sides.
  Point shortenForArrow(const Point& tip, const Point& from, float strokeWidth);

  // The font a project object asks for, taken from the .bdf this device
  // publishes in its own DDF. Falls back to the DDF's first font when a
  // project names one this device does not declare; nullptr only if the
  // DDF itself is unreadable, which is a broken build.
  const BdfFont* fontFor(const String& fontId);
  // The font drawing a replaced id the DDF dropped, or nullptr when the id
  // draws as itself (FontIndex.h). Its metrics then count, not those the
  // project's own entry carries for the old font.
  const BdfFont* standIn(const String& fontId) const;

  // How far the clip box has to grow upward so this text's ink survives.
  // The baseline sits at y + ascent, but ink can reach above the nominal
  // ascent - capital umlauts do, by one pixel in helvR18 - and clipping to
  // the nominal box shaved the dots off Ä Ö Ü.
  //
  // Measured from the string, not the font: helvR18's font-wide maximum is
  // 29 against a nominal 22, from some stacked diacritic in Latin
  // Extended, so using that would grow every label's box by seven pixels
  // including pure ASCII ones that overhang by nothing.
  int16_t inkOverhang(const BdfFont* font, const String& text, int16_t nominalAscent) const;
  int getFontAscentById(const String& fontId) const;
  int getFontDescentById(const String& fontId) const;
  int getFontSizeById(const String& fontId) const;
  int16_t measureTrueTextWidth(const String& text, const BdfFont* font);
  String formatNumber(const String& valueStr, int decimals, const String& thousandsSeparator) const;
  float interpolateCalibration(float value, const std::vector<CalibrationPoint>& points) const;
  // Keyed by the topic the request is about, not by the object that took it:
  // two bars on one dimmer are one value, and both show what was asked of it.
  String askedTopic_;
  String askedValue_;
  float calibrationValueForPercent(float percent, const std::vector<CalibrationPoint>& points) const;

 public:
  // A level a finger can set: one with somewhere to write to
  // (docs/2026-09-17-settable-level.md in the designer repo, decision 1).
  // Only the bar so far - the ring's own sector arithmetic follows.
  static bool isSettableLevel(const ScreenObject& obj);

  // What a finger at this point means for that bar: the position as a
  // percentage of the bar, through the calibration read backwards, snapped
  // to the object's step. x/y are absolute screen coordinates, objX/objY the
  // object's own corner as hitTestTappable reports it.
  float settableLevelValueAt(const ScreenObject& obj, int16_t x, int16_t y, int16_t objX, int16_t objY) const;

  // The object with this id on that screen, with its absolute corner - the
  // same walk and the same coordinates hitTestTappable reports, for a finger
  // that has left the object it started on: a drag past the end of a bar
  // still means the end of that bar, not "nothing here".
  const ScreenObject* findObjectById(int screenIndex, const String& id, int16_t& objX, int16_t& objY) const;

  // «Adjust a slider or dial» (docs/device-contract.md §5 in the designer
  // repo, 2026-10-04): the slider or dial a press moves - the target itself,
  // or for a switcher the first settable level, in object order, of the panel
  // it shows now. nullptr when the target is not on screen or its panel holds
  // none: the press does nothing.
  const ScreenObject* adjustableLevel(int screenIndex, const String& targetId) const;

  // The value one press writes, from the value shown (`current`): one step up
  // or down onto the grid a finger snaps to (roundf(value / step) * step),
  // clamped to the outer calibration points (default 0-100). False at the end
  // of the range, and while no value is known yet (after a restart, before
  // the installation said anything) - nothing to write: stepping a heater
  // from a guessed minimum would surprise more than a ring that waits
  // (decided 2026-10-04).
  // The designer's lib/adjust-level.ts adjustedValue() does the same.
  static bool adjustedLevelValue(const ScreenObject& level, const String& current, bool up, float& next);

  // What a finger asked one object for, drawn as its setpoint marker until
  // the installation answers (docs/2026-09-17-settable-level.md in the
  // designer repo, decision 6c: the finger moves the marker, the fill keeps
  // showing what is measured, and the two coincide once the command landed).
  //
  // Not a topic: a dimmer's level is one value on the broker with no second
  // topic to hold "asked for", so the device remembers it - and forgets it
  // the moment a value arrives on that object's read topic, which is where
  // each board's main.cpp clears it. A setpoint topic, where there is one,
  // simply wins: the installation's own word about the target beats this.
  void setAskedValue(const String& markerTopic, const String& value);

  // The rectangle every object showing this marker topic lives in, as one box.
  // A board that redraws regions rather than screens needs it: a request is
  // about a value, so two bars on one dimmer both change, and redrawing only
  // the one under the finger leaves the other showing the old marker (found on
  // the 4.3B, 166 differing pixels in the conformance photograph).
  bool markerTopicBounds(int screenIndex, const String& markerTopic, int16_t& x, int16_t& y, int16_t& w,
                         int16_t& h) const;

  // What a finger moving this marker topic from oldValue to newValue changes,
  // as separate rectangles rather than one box round everything bound to it:
  // a slider across the top and a dial at the bottom on one dimmer made that
  // box the whole screen, 713 ms a drag frame on the 4.3B (2026-09-28). A ring
  // gives the stretch of its band between the two values and the numbers in
  // its middle; anything else its own rectangle. Returns how many were
  // written, or -1 where it cannot say (an object inside a switcher, more than
  // `max`) - the caller then falls back to markerTopicBounds.
  int levelDragRects(int screenIndex, const String& markerTopic, const String& oldValue, const String& newValue,
                     int16_t* rx, int16_t* ry, int16_t* rw, int16_t* rh, int max);
  // The value this marker topic is showing as asked for, or "" when none is.
  String askedValueFor(const String& markerTopic) const {
    return askedTopic_ == markerTopic ? askedValue_ : String();
  }
  void clearAskedValue();
  // What a level shows: the report, or on the dimmer pattern the finger (see the .cpp).
  String shownLevelValue(const ScreenObject& obj) const;

 private:

  // Parses "#rrggbb" (or "#rrggbbaa", alpha ignored except to detect
  // full transparency) into a real RGB565 value - unlike the e-paper
  // firmware's parseColor(), which only ever needed to decide black vs.
  // white. "transparent"/"#ffffff00"/similar all resolve to isTransparent
  // = true via the out-param, matching drawTextBox's existing
  // transparent-detection conventions (see its own comment for the exact
  // string set this mirrors).
  uint16_t parseHexColor(const String& colorStr, bool* isTransparent = nullptr) const;
  ChartFeed* chartFeed_ = nullptr;
  double chartPulse_ = -1;
  std::vector<PulseBox> chartPulseBoxes_;
};
