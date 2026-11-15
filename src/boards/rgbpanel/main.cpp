// Firmware entry point for the Waveshare RGB boards: the
// ESP32-S3-Touch-LCD-4.3B, where it was written and every measurement below
// was taken, and since 2026-10-30 the ESP32-S3-Touch-LCD-4. What differs
// between them - geometry, timings, pins, the IO expander - is the board's
// BoardProfile.h, found on the env's include path; this file names no pin
// and no expander chip.
//
// The bring-up scaffolding this file grew during the port has been removed
// now that the board runs real projects: the colour-bar test pattern, the
// backlight blink, the fill-rate benchmark, the font sample sheet, the touch
// dots and the NVS tap counters. Each earned its keep once and then started
// costing something - they flickered through every boot and one of them was
// still reachable by tapping. They are in the history if a measurement needs
// repeating.
//
// NO FINGER-TRACKING SWIPE ON THIS BOARD. That is a decision, and the first
// version of this note got the reason wrong, so here is the measured one.
//
// The claim used to be that the panel's memory bus was full: 72ms per frame
// against "a measured 43ms floor". Then the factory LVGL demo was flashed
// back and slid roughly 80% of the screen smoothly - so the bus plainly was
// not full, and a measurement of one implementation had been generalised into
// a property of the hardware. PanelBenchmark.h now lives next to this file so
// the question is settled with numbers instead of a story; run it with
// GET /api/debug?set=bench=1.
//
// What it says (2026-09-01, 800x480, 750KB a frame):
//
//   fb_memset      25.3 ms   write the framebuffer, nothing else
//   psram_read     27.1 ms   read a rendered canvas, nothing else
//   fb_from_sram   25.3 ms   internal SRAM -> framebuffer = 39 fps
//   fb_from_psram  66.2 ms   canvas -> framebuffer
//   compose        65.7 ms   one real swipe frame = 15.2 fps
//
// So the hardware does 39 fps full-screen - if the source sits in internal
// SRAM. That is exactly what LVGL does: it re-renders cheap widgets into a
// small SRAM buffer every frame and pushes only the result, 750KB in one
// direction. We cannot, because a real project takes 782ms to draw, so our
// screens must be pre-rendered and cached in PSRAM - and then every frame
// both reads 750KB and writes 750KB. Twice the traffic, 2.6x the time. The
// gap is structural, not a missing optimisation.
//
// Three ways out were measured and none worked. Staging the copy through a
// 16KB SRAM block, on the theory that interleaved read and write bursts were
// costing bus turnarounds, came out at 66.3ms - identical, so that theory is
// dead. Handing the copy to GDMA came out at 133.9ms, twice as slow as the
// CPU, because it arbitrates for the same memory controller the LCD is
// already streaming through. The third is real but insufficient: our data
// cache has 32-byte lines where Waveshare's own ESP-IDF demo sets 64
// (CONFIG_ESP32S3_DATA_CACHE_LINE_64B), which the Arduino framework cannot
// change without rebuilding it as an ESP-IDF component. It would only help
// the read half, so the ceiling would still be around 22 fps.
//
// Half-row compositing during motion did reach ~28 fps by halving the
// traffic, and was judged on the glass: "sieht nicht so toll aus" - losing
// vertical resolution during a horizontal movement is the most visible way to
// spend that. So screen changes stay on a tab-control or a button here, where
// nothing is expected to track a finger. A short non-tracking transition at
// 15 fps has NOT been tried and is the one option still open.
//
// Pin map and RGB timings are Waveshare's own, taken from
// waveshareteam/ESP32-S3-Touch-LCD-4.3B (Apache-2.0),
// examples/Arduino/examples/08_DrawColorBar/waveshare_lcd_port.h, and
// cross-checked line by line against the pin table in the board's wiki. Both
// sources agree; nothing here is inferred from the 1.8" knob port, which is
// the mistake that produced four hardware bugs on the last one.
#include "../../ChartFeed.h"
#include "project/NavigatorLayout.h"
#include "project/PinchDetector.h"
#include "project/CameraFeed.h"
#include "project/ChartShapes.h"
#include "project/ScreenGraphProject.h"
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include "Gt911.h"
#include "TestProjectJson.h"
#include "PanelDisplayAdapter.h"
#include "../../NvsConfigStorage.h"
#include "../../WiFiSetupServer.h"
#include "../../MqttClient.h"
#include "../../DeployManager.h"
#include "../../TestInterfaceServer.h"
#include "../../DeviceRequests.h"
#include "../../TestInterfaceSwitch.h"
#include "../../WiFiSupervisor.h"
#include "../../BootReport.h"
#include "../../FirmwareImage.h"
#include "../../FirmwareUpdater.h"
#include "../../TaskLoad.h"
#include "RgbPanel.h"
#include "PanelBenchmark.h"
#include "../../project/RenderProfile.h"
#include "ArcSelfTest.h"
#include "FollowSwipe.h"
#include "../../DeviceSettings.h"
#include "../../DeviceProtocol.h"
#include "../../ClippedCanvas16.h"
#include "../../project/ProjectLoader.h"
#include "../../project/ThemeVariant.h"
#include "../../project/ColorScreenRenderer.h"
#include "../../project/GuardSet.h"
#include "../../project/SwitchShape.h"
#include "../../project/Keyboard.h"
#include "../../project/TimePicker.h"
#include "../../DeviceSettings.h"
#include "project/ObjectTypes.h"
#include "BoardProfile.h"

static RgbPanel panel;
static BoardExpander expander;
static Gt911 touch;
static bool startExpander();

// --- rendering ------------------------------------------------------------
// The same ColorScreenRenderer the knob and the M5 Dial run, unchanged. Its
// output goes into an RGB565 canvas; the only board-specific part is where
// that canvas then goes - here a straight blit to the RGB panel, with no
// LVGL in the path. The knob composites through LVGL because it wants
// transitions; that costs a second and third 768KB buffer here and is a
// decision to take with measurements, not before them.
static ProjectLoader projectLoader;
static ClippedCanvas16 *canvas = nullptr;
static ColorScreenRenderer *renderer = nullptr;
// Guarded controls' covers on the screen showing (designer docs/2026-11-15-guard.md).
static GuardSet guards;
static void renderRegion(int16_t rx, int16_t ry, int16_t rw, int16_t rh, const char *reason, bool present);
static int currentScreen = 0;
// The popup open over currentScreen, as an index after the screens
// (screenOrPopupAt, ProjectTypes.h), or -1 (designer docs/device-contract.md
// 2.5). Only ever this board's own state: never on MQTT. While one is open,
// taps, drags, hit tests and button actions are the popup's.
static int openPopup = -1;
// Where the button that opened it was, for the frame to grow from.
static bool popupOriginKnown = false;
static int16_t popupOriginX = 0, popupOriginY = 0, popupOriginW = 0, popupOriginH = 0;
// How wide the fence's edge is drawn in the popup's border colour.
static const int POPUP_BORDER_PX = 2;
// The close button's radius on the fence's corner (PopupFence.h); the
// designer's popupCloseBadge has the same number for this board.
static const int POPUP_CLOSE_BADGE_RADIUS = 24;
// What the last popup did, for /api/debug.
static String lastPopup = "none since boot";
// The screen or popup a finger or a button acts on now.
static int activeScreen() { return openPopup >= 0 ? openPopup : currentScreen; }
// The transition's second surface and the transition itself. See
// FollowSwipe.h for why a full-screen follow exists on a board that
// measures 15fps for one.
static ClippedCanvas16 *spareCanvas = nullptr;
// The navigator's strip with every entry active on the marker's colour, for
// the marker gliding with a swipe (FollowSwipe.h, designer #59). Without it
// the marker jumps at the end, as before.
static ClippedCanvas16 *markerCanvas = nullptr;
static FollowSwipe swipe;
// A value that arrived while a transition was running. The value itself is
// stored the moment it lands; only the drawing waits.
// Which broker this device is pointed at. Read once at boot with the rest
// of the credentials and kept here, because "mqtt NOT connected" says
// nothing about who it failed to reach - and the only other way to find out
// is a serial monitor, whose attach resets the board. It also makes the
// setting restorable: POST /api/mqtt can overwrite it and nothing could
// read it back before changing it.
static String brokerHost;
static uint16_t brokerPort = 0;

// Which topics changed since the last draw, and what they held before.
//
// The old value is the part that is easy to lose: an arc-level's dirty
// rectangle is the ground the pointer SWEPT, so it needs where the value
// came from, not just where it went. Coalescing keeps the oldest one seen
// per topic for exactly that reason - three values arriving inside one
// window have swept the whole way from the first to the last.
struct PendingTopic {
  String topic;
  String oldValue;
};
static const int MAX_PENDING_TOPICS = 6;
static PendingTopic pendingTopics[MAX_PENDING_TOPICS];
static int pendingTopicCount = 0;
static bool pendingTopicOverflow = false;
// What the last value change cost, so "the partial path is running" stays
// checkable from outside rather than being assumed.
static String lastValueDraw = "none since boot";
static int pagingTargetFor(int d, bool vertical = false);
// ?set=partialcheck=on: after every value change drawn in part (or found to
// need nothing), the same screen drawn whole into the spare canvas, at the
// same values, and the two compared pixel for pixel - so a partial draw can
// be held to a full one on whatever project the board runs, without a race
// against the next value (2026-10-30, the van's energy screen).
static bool partialCheck = false;
static uint32_t partialChecks = 0, partialCheckFailures = 0;
static String lastPartialCheck = "off";

// ------------------------------------------------------ display blanking
//
// Ported from the knob, whose rules were argued out there and are not
// board-specific. The screen turns off, backlight included, after a
// configured stretch with no HUMAN input - MQTT traffic deliberately does
// not count, or a panel showing values that change every few seconds would
// never sleep, which is exactly the case this exists for.
//
// The first touch afterwards only wakes. It does not press the button it
// happens to land on: this panel switches lights and a heater, and reaching
// for a dark screen to see what it says must not turn something on. Same
// rule the reserved setup hold follows from the other side - a five-second
// hold is unambiguous, a stab in the dark is not.
//
// The setting has been offered in this board's own settings page since the
// port and did nothing here until 2026-09-10; the backlight sits on the
// CH422G rather than a GPIO, which is the only real difference from the
// knob.
static bool displayIsOff = false;
static uint32_t lastHumanInputMs = 0;
static uint32_t displayOffAfterMs = 0;  // 0 = never, cached from NVS
static String idleScreenId;             // empty = feature off, cached from NVS
// A contact that arrived on a dark screen. Held for the whole contact so a
// wake cannot also become a tap or a swipe when the finger moves.
static bool contactOnlyWoke = false;

static bool renderPending = false;

// Light or dark, as schaltli/state/theme last said (the designer's
// docs/2026-09-26-device-switch.md). Kept apart from the project: the
// retained value can arrive before the project is loaded, and must hold for
// it once it is.
static bool wantedThemeDark = false;

// Shows the loaded project in the wanted variant. Swapped here, drawn by the
// loop: a change asks for a whole screen (pendingTopicOverflow), because
// every colour, the background and every baked bitmap change at once - no
// partial redraw has a rectangle for that.
static void applyWantedTheme()
{
  if (!projectLoader.isLoaded()) return;
  if (projectLoader.getProject().themeDark == wantedThemeDark) return;
  projectLoader.applyThemeVariant(wantedThemeDark);
  pendingTopicOverflow = true;
  renderPending = true;
}
static unsigned long lastTopicRenderMs = 0;
// Drawing every value as it lands left the loop no time for anything else,
// so values are coalesced to at most one render in this window. The first
// value after a quiet spell is still drawn at once - the window has already
// passed by then. 250 ms when a render plus its blit cost up to 490 ms; since
// pill-shaped controls sample only their edges (PillRaster::pixelBandsQuick,
// 2026-09-27) it is about 170 ms, and a quarter of a second of waiting on top
// was most of what a dimmer's value took to show.
static const unsigned long TOPIC_RENDER_MIN_MS = 150;

// Where the swipes that never happened went. Half of twenty were missing
// twice in a row while every recorded one was clean, and the two possible
// explanations need different fixes: either the contact is never seen at
// all, because the loop was busy elsewhere for the whole 400ms of it, or it
// is seen and never recognised. Counting both settles it in one run.
// The finger, as opposed to the controller. A GT911 report says what the
// glass looks like at that instant; the silence between reports says
// nothing at all, so the state has to be held here rather than re-derived
// from every read. See Gt911::poll().
// How far a contact wandered, so a release can tell a tap from a gesture
// that was never wide enough to become one.
static int16_t contactDrift = 0;
static bool fingerDown = false;
static int16_t touchX = 0;
static int16_t touchY = 0;
static unsigned long lastTouchReportMs = 0;
// Longer than any gap this controller leaves between reports, short enough
// that a report lost for good cannot hold a gesture open.
static const unsigned long TOUCH_LOST_MS = 150;

// How much of the time the loop is inside a value render and therefore
// blind to the glass. A render plus its blit is 180ms and values arrive
// faster than that on this vehicle, so this is the number that decides
// whether a 300ms gesture can be seen at all.
static uint32_t topicRenders = 0;
static uint32_t topicRenderUs = 0;
static unsigned long dutyWindowStartMs = 0;
// Diagnostic only: lets a gesture be judged with nothing else touching the
// panel. Not a setting - it survives nothing, and the board renders again
// after a restart.
static bool topicRenderEnabled = true;

// The measuring square owns the panel while it is up. Without this it lasted
// until the next touch: a press repaints its object from the project, and a
// tap on a button repaints everything - and a ruler in one hand and a finger
// on the glass is exactly the situation it exists for.
static bool measuringPattern = false;

// Puts the panel under a measuring pattern, lit. The loop reads no touch
// while one is up, so a panel that had gone dark before could not be woken
// - a black screen to hold a ruler against (the 4", 2026-10-31).
static void holdMeasuringPattern()
{
  measuringPattern = true;
  lastHumanInputMs = millis();
  if (displayIsOff)
  {
    displayIsOff = false;
    expander.setBacklight(true);
  }
}

// Where a turn of the loop goes. Twelve of twenty gestures never produced a
// contact at all while the board was rendering nothing, so something else
// is holding the loop for longer than a finger stays on the glass. Guessing
// which has now been wrong twice, so each section is timed instead.
static uint32_t loopTurns = 0;
static uint32_t webUs = 0, mqttUs = 0, touchUs = 0;
static uint32_t worstWebUs = 0, worstMqttUs = 0, worstTouchUs = 0;
static uint32_t worstTurnUs = 0;

static uint32_t contactCount = 0;
static uint32_t contactSamples = 0;
static uint32_t missedContacts = 0;
static uint32_t missedMaxSamples = 0;
static int missedMaxDx = 0;

// The pinch proof of concept (designer docs/2026-10-16-charts.md, decision
// 15): does a GT911 tell a pinch apart reliably? Every two-finger contact is
// decided at the lift and written to /api/debug?set=pinches=1; after
// ?set=pinches=show it is also shown on the panel for a moment, so it can be
// judged in the hand. No action is dispatched yet. touchPointCount and touchX2/Y2 are the last report's
// point count and second point; touchX/Y stay the first.
static PinchDetector pinch;
static uint8_t touchPointCount = 0;
static int16_t touchX2 = 0, touchY2 = 0;
static uint32_t pinchIns = 0, pinchOuts = 0, pinchNones = 0;
static String pinchLog;
static unsigned long pinchShownUntilMs = 0;
static bool pinchShow = false;

// --- networking -----------------------------------------------------------
// All six of these compiled for this board without a single change. What was
// missing was only the wiring, and that lived in the knob's main.cpp rather
// than anywhere shareable - which is why DeviceProtocol.h exists.
static NvsConfigStorage configStorage;
static PanelDisplayAdapter *displayAdapter = nullptr;

// What a finger held for the setup gesture sees once it has held long enough
// to be asking: the seconds left, and how to back out.
static void showHoldCountdown(int secondsLeft)
{
  if (displayAdapter) displayAdapter->showLines({"Setup in " + String(secondsLeft) + "s", "loslassen zum Abbrechen"});
}
static WiFiSetupServer *wifiSetupServer = nullptr;
static MqttClient mqttClient;
static DeployManager deployManager;
static FirmwareUpdater firmwareUpdater;
// A deploy trigger, held from the MQTT callback until loop() runs it.
// DeployManager::handleDeployMessage() must not run inside the callback:
// its status publishes share PubSubClient's buffer with the packet being
// parsed (DeployManager.h). This board did exactly that until 2026-09-15.
// hil/deploy-check.js passed against it all the same, with ~16 publishes
// per deploy, but the M5 Dial lost status that way at ~100, and the rule
// the class states is not one to keep only while it happens to hold.
static bool pendingDeploy = false;
static String pendingDeployPayload;
static TestInterfaceServer testInterface;
static DeviceProtocol protocol(mqttClient);
// ddf/get and recovery/get, answered over the broker (DeviceRequests.h).
static DeviceRequests deviceRequests(mqttClient);
// hil-on <password> / hil-off over USB (TestInterfaceSwitch.h).
static TestInterfaceSwitch testInterfaceSwitch;
static bool topicsSubscribed = false;
// The charts' history for the shown screen (ChartFeed.h), and the screen and
// popup it was last fitted to.
static ChartFeed chartFeed;
static int chartFeedScreen = -1;
static int chartFeedPopup = -1;
static bool setupModeActive = false;

// The reserved setup gesture, same numbers as the knob: hold a finger still
// for five seconds, with a countdown from one second in so it is
// discoverable rather than secret. It has to exist - once a board has
// credentials it never starts its AP again, so without this there is no way
// back into setup short of erasing NVS. Identical to the knob's on purpose:
// entering setup is one of the cross-device building blocks that were agreed
// to be the same everywhere rather than per board.
static const unsigned long SETUP_HOLD_MS = 5000;
// After three seconds at rest, as on the knob: a finger resting a moment on a
// control is not asking for setup (2026-09-27).
static const unsigned long SETUP_HOLD_FEEDBACK_MS = 3000;
static const int SETUP_HOLD_MAX_DRIFT = 40;
// A finger on an 800x480 panel is not a mouse pointer, and a button here is
// finger sized. Well below the 12px that starts a horizontal drag, so a tap
// and a swipe can never both fire from one contact.
static const int TAP_MAX_DRIFT = 10;

// What the last tap hit, in words. Serial is not a neutral observer on this
// board - opening the port resets it - and the effect of a tap is usually a
// published command, which is invisible when the broker is away. Without
// this, "did the tap land" is unanswerable from outside.
static String lastTap = "none since boot";
// What the last feedback repaint cost. Claimed at roughly 80ms before it
// was built; kept on the device so the claim stays checkable.
static String lastFeedback = "none since boot";

// How long a tap waits for its pressed look (designer
// docs/2026-10-11-energy-diagram.md, success criterion 3): from the moment
// the sample arrived - posted to /api/touch, or read from the panel - to the
// press feedback being on the glass. The feedback's own cost above says
// what the repaint takes; this adds whatever the loop was busy with before
// it got to it, which is what anything moving on the screen would make
// worse. The last 32 kept, for a median and a 90th percentile.
static uint32_t contactSampleUs = 0;
static bool tapLatencyOpen = false;
static uint32_t tapLatencyUs[32];
// Of it, the part before the press feedback began: waiting, not drawing.
static uint32_t tapWaitUs[32];
static uint32_t pressStartUs = 0;
static uint8_t tapLatencyCount = 0, tapLatencyNext = 0;
static void recordTapLatency(uint32_t us)
{
  tapWaitUs[tapLatencyNext] = pressStartUs - contactSampleUs;
  tapLatencyUs[tapLatencyNext] = us;
  tapLatencyNext = (tapLatencyNext + 1) % 32;
  if (tapLatencyCount < 32) tapLatencyCount++;
}
// The energy diagram's motion, defined before loop() (see there).
static void energyStep();
static CameraFeed cameraFeed;
static void cameraStep();
static String cameraReport();
static void cameraResetStats();
static uint32_t loopLastUs = 0, loopMaxGapUs = 0;
// What the turn behind loopMaxGapUs spent where: values drawn, the energy
// diagram's step, the Camera's picture (criterion 4 on the 4.3B, 2026-11-10).
static uint32_t turnValuesUs = 0, turnEnergyUs = 0, turnCameraUs = 0;
static uint32_t worstValuesUs = 0, worstEnergyUs = 0, worstCameraUs = 0;
static String energyReport();

static String tapLatencyReport()
{
  if (tapLatencyCount == 0) return "none since boot (?set=taplatency=clear resets)";
  uint32_t sorted[32];
  for (uint8_t i = 0; i < tapLatencyCount; i++) sorted[i] = tapLatencyUs[i];
  for (uint8_t i = 1; i < tapLatencyCount; i++)
  {
    uint32_t v = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = v;
  }
  const uint32_t median = sorted[tapLatencyCount / 2];
  const uint32_t p90 = sorted[(tapLatencyCount * 9) / 10 >= tapLatencyCount ? tapLatencyCount - 1 : (tapLatencyCount * 9) / 10];
  uint32_t waits[32];
  for (uint8_t i = 0; i < tapLatencyCount; i++) waits[i] = tapWaitUs[i];
  for (uint8_t i = 1; i < tapLatencyCount; i++)
  {
    uint32_t v = waits[i];
    int j = i - 1;
    while (j >= 0 && waits[j] > v) { waits[j + 1] = waits[j]; j--; }
    waits[j + 1] = v;
  }
  return String(tapLatencyCount) + " taps, median " + String(median / 1000.0f, 1) + " ms, p90 " +
         String(p90 / 1000.0f, 1) + " ms, worst " + String(sorted[tapLatencyCount - 1] / 1000.0f, 1) +
         " ms (waiting before the feedback: median " + String(waits[tapLatencyCount / 2] / 1000.0f, 1) + " ms)";
}

// Touch feedback. The shared renderer has drawn all three of these states
// from the start and this board had never asked for any of them: a finger
// went down on a switch, a command went out, and the panel looked exactly
// as it had a moment earlier. On a vehicle, where the answer comes back
// from a broker over a link that may be down, "nothing happened" and "I
// have your tap and I am waiting" have to look different.
static String levelDragObjId;
static String levelDragTopic;
static String levelDragWriteTopic;
static std::optional<scaled::WriteFormat> levelDragWriteFormat;
static String levelDragPayload;
// After a release: the value whose answer ends the request, and until when
// to wait for it (endLevelDrag, the MQTT callback, loop()).
static String levelAwaitValue;
static unsigned long levelAwaitUntilMs = 0;
static const unsigned long LEVEL_AWAIT_MS = 2500;
// The topic whose next message ends the request - the setpoint topic where
// there is one, else the read topic. Separate from levelDragTopic because the
// request outlives the finger.
static String levelAskedTopic;

// The keyboard a tapped text field opened (Keyboard.h, designer
// tasks/ha-text-plan.md). While it is open every contact is its own - no
// swipe, no setup hold, nothing under it - and nothing else draws: the
// values that arrive meanwhile are drawn when it closes, with the screen.
static bool keyboardOpen = false;
static bool keyboardContact = false;
static keyboard::State keyboardState;
static keyboard::Frame keyboardFrame;
static ScreenObject keyboardField;

// The picker a tapped time field opened (TimePicker.h, designer
// tasks/ha-time-plan.md): two wheels that follow the finger and snap on
// release. While it is open every contact is its own, as the keyboard's.
static bool pickerOpen = false;
static bool pickerContact = false;
static timepicker::State pickerState;
static timepicker::Frame pickerFrame;
static ScreenObject pickerField;
// The wheel a finger is on, where it began and its last few samples.
static timepicker::Wheel *pickerWheel = nullptr;
static int16_t pickerStartY = 0, pickerLastY = 0;
static bool pickerMoved = false;
static int16_t pickerSampleY[6];
static uint32_t pickerSampleMs[6];
static int pickerSamples = 0;
static int16_t levelDragX = 0, levelDragY = 0, levelDragW = 0, levelDragH = 0;
static unsigned long levelDragLastPublishMs = 0;
// A drag must not put thirty messages on the broker - the same coalescing the
// van's fan step uses. 100 ms, ten a second: the value on the screen can follow
// the finger no closer than this, and 250 ms read as a lag behind it. Pekaway
// takes a stream like that in its stride - it files a dimmer level 200 ms
// after the last command anyway.
static const unsigned long LEVEL_PUBLISH_MIN_MS = 100;

static String pressedButtonId;
static String pressedSwitchId;
static int pressedSwitchIndex = -1;
static String pendingSwitchId;
static int pendingSwitchIndex = -1;
// The topic the marked switch reads, and whether it has answered since.
static String pendingReadTopic;
static bool pendingAnswered = false;
static unsigned long pendingSinceMs = 0;
// Long enough for a broker on the same network to answer several times
// over, short enough that a dead link does not leave a switch waiting for
// ever on a screen nobody is watching.
static const unsigned long PENDING_TIMEOUT_MS = 4000;
// The rectangle the feedback lives in. Only this is redrawn, because a
// button lighting up does not justify the whole screen.
static int16_t feedbackX = 0, feedbackY = 0, feedbackW = 0, feedbackH = 0;
static bool feedbackActive = false;
// The pending mark keeps its OWN rectangle, and that is the whole fix for a
// bug reported from the glass: tap three tiles in a row and only the last
// one's mark ever cleared. Both states used to share one rectangle, so the
// press on tile three moved it there, and when tile three's mark expired it
// repainted tile three - leaving marks on one and two standing for ever,
// because nothing pointed at them any more.
static int16_t pendingX = 0, pendingY = 0, pendingW = 0, pendingH = 0;
// Declared here because the test interface is wired up before the feedback
// helpers are defined, and a screen forced from outside invalidates them
// exactly as one chosen by a finger does.
static void forgetTouchFeedback();
// Counts full renders, so a caller can tell whether something it invoked
// already repainted the screen and a region render would be waste.
static uint32_t renderGeneration = 0;

// A touch sample posted to /api/touch, standing in for the panel.
//
// Exists because a gesture is not otherwise reachable from a test. Posting
// an action says whether the action works; posting a series of these drives
// the real detector, the real dispatch and the real transition - which is
// where every fault of the last two days actually lived.
static int16_t injectX = 0, injectY = 0;
static bool injectDown = false;
// A second finger, posted with x2/y2 (TestInterfaceServer's
// TwoFingerTouchHandler), for a pinch.
static bool injectTwo = false;
static int16_t injectX2 = 0, injectY2 = 0;
static bool injectLiftOnce = false;
static unsigned long injectUntilMs = 0;
// The knob holds an injected sample for 400ms. This board needs longer, and
// the reason is its own snapshot: 1.15MB against the knob's 259KB, roughly
// a second on the wire. A press that expires while the photograph of it is
// still being sent cannot be photographed - which is how the press feedback
// first appeared to do nothing at all. Still short enough that a test which
// stops posting cannot wedge the panel into a phantom contact.
static const unsigned long TOUCH_INJECT_HOLD_MS = 2000;
static bool holdActive = false;
static int16_t holdStartX = 0, holdStartY = 0;
// Where the finger went down, for the whole contact. holdStartX/Y is the setup
// hold's reference and restarts whenever the finger wanders, so a swipe
// measured from it was only the last stretch and never long enough to count -
// found 2026-10-06 when a swipe on a popup did not close it.
static int16_t contactStartX = 0, contactStartY = 0;
static unsigned long holdStartMs = 0;
static int holdLastSecondShown = -1;
static bool holdConsumed = false;

static void setupRenderStack();
static void setupNetworking();
static void enterSetupMode();

// Which screens are hidden (designer device contract 2.7): out of paging,
// never the start, still a goto target.
static std::vector<bool> hiddenFlags()
{
  std::vector<bool> hidden;
  if (!projectLoader.isLoaded()) return hidden;
  for (const Screen &screen : projectLoader.getProject().screens) hidden.push_back(screen.hidden);
  return hidden;
}

// -1 when nothing is configured, no project is loaded, or the saved id is
// not in this project. Every caller treats that as "leave the screen alone".
static int resolveIdleScreenIndex()
{
  if (idleScreenId.isEmpty() || !projectLoader.isLoaded()) return -1;
  const ProjectConfig &project = projectLoader.getProject();
  for (size_t i = 0; i < project.screens.size(); i++)
  {
    if (project.screens[i].id == idleScreenId) return (int)i;
  }
  return -1;
}

// The navigator owns a contact that began on its strip (designer device
// contract 2.7): a drag along it scrolls it, a still release is a tap, and
// nothing it does pages.
static bool navigatorContact = false;
static bool navigatorScrolled = false;
static int navigatorStartScroll = 0;

static bool onNavigatorStrip(int16_t x, int16_t y, navigatorlayout::Layout &layout)
{
  if (!renderer || openPopup >= 0 || !renderer->navigatorLayout(currentScreen, layout)) return false;
  const navigatorlayout::Rect &s = layout.bar;
  return x >= s.x && y >= s.y && x < s.x + s.width && y < s.y + s.height;
}

// The screen the navigator last followed to: it follows a screen change
// only, never a redraw of the same screen - which would undo a scroll the
// finger just made (found on the PaperS3, 2026-10-08).
static int navigatorFollowedScreen = -1;

// Before `target` is drawn: the navigator scrolled as far as shows its
// entry, from where it is now. Untouched (-1) it does that by itself.
static void navigatorFollow(int target)
{
  if (target == navigatorFollowedScreen) return;
  navigatorFollowedScreen = target;
  if (!renderer || renderer->navigatorScroll() < 0) return;
  navigatorlayout::Layout layout;
  int active = -1;
  if (!renderer->navigatorLayout(target, layout, nullptr, &active) || active < 0) return;
  renderer->setNavigatorScroll(navigatorlayout::scrollToShow(layout, active, renderer->navigatorScroll()));
}

static void renderAndPresent(const char *reason)
{
  if (!renderer || !canvas) return;
  renderer->clearChartPulseBoxes();
  navigatorFollow(currentScreen);
  uint32_t t0 = micros();
  bool ok;
  if (openPopup >= 0)
  {
    // The screen underneath as it is, set back to half, and the popup over
    // it; the touch feedback belongs to the popup.
    ok = renderer->renderScreen(currentScreen);
    renderer->dimOutsidePopupFence();
    ok = renderer->renderPopup(openPopup, POPUP_BORDER_PX, pressedButtonId, pendingSwitchId, pendingSwitchIndex,
                               pressedSwitchId, pressedSwitchIndex) && ok;
  }
  else
  {
    ok = renderer->renderScreen(currentScreen, pressedButtonId, pendingSwitchId, pendingSwitchIndex,
                                pressedSwitchId, pressedSwitchIndex);
  }
  if (keyboardOpen) renderer->renderKeyboard(keyboardState, keyboardField, keyboardFrame);
  if (pickerOpen) renderer->renderTimePicker(pickerState, pickerField, pickerFrame);
  renderGeneration++;
  pendingTopicCount = 0;
  pendingTopicOverflow = false;
  uint32_t t1 = micros();
  panel.blit(canvas->getBuffer());
  uint32_t t2 = micros();
  Serial.printf(BOARD_LOG "render(screen %d, %s): %s | draw %lums, blit %lums\n",
                currentScreen, reason, ok ? "ok" : "FAILED",
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000));
}

static void wakeDisplay()
{
  lastHumanInputMs = millis();
  if (!displayIsOff) return;
  displayIsOff = false;
  expander.setBacklight(true);
  // Repainted before the backlight can reveal a stale frame: the panel
  // keeps whatever it last received, and that is the screen from before it
  // went dark, which may be minutes old.
  renderAndPresent("woke");
  Serial.println("[display] woke");
}

static void blankDisplay(const char *reason)
{
  if (displayIsOff) return;
  displayIsOff = true;
  expander.setBacklight(false);
  Serial.printf("[display] off (%s)\n", reason);
  // Dark, every guarded control locks (E2); waking repaints.
  guards.clear();

  // Back to the idle screen AFTER the backlight is out: the panel would
  // otherwise be seen flipping pages on its way off, which reads as the
  // device doing something rather than settling. Nothing is visible now,
  // and waking repaints, so the next person finds the idle screen rather
  // than wherever the last one left it.
  const int idle = resolveIdleScreenIndex();
  if (idle < 0 || (idle == currentScreen && openPopup < 0)) return;
  currentScreen = idle;
  openPopup = -1;
  forgetTouchFeedback();
  renderer->renderScreen(currentScreen);
}

// Re-read from NVS so a saved setting takes effect without a restart.
static void applyDeviceSettings()
{
  displayOffAfterMs = DeviceSettings::displayOffAfterSeconds() * 1000UL;
  idleScreenId = DeviceSettings::idleScreenId();
  lastHumanInputMs = millis();
  wakeDisplay();
  Serial.printf("[display] blanking after %us, idle screen \"%s\"\n",
                (unsigned)(displayOffAfterMs / 1000), idleScreenId.c_str());
}

static void setupRenderStack()
{
  if (!LittleFS.begin(true))
  {
    Serial.println(BOARD_LOG "LittleFS mount FAILED");
    return;
  }

  // A deployed project wins, always. The built-in fixture exists only so
  // this board had something to render before it could receive anything -
  // and for a while it also silently outranked a real deploy, because this
  // ran unconditionally and never looked at /PROJECT. A deploy would
  // download, verify, install and reboot correctly, and the panel would come
  // back showing the test pattern as if nothing had happened.
  bool loaded = projectLoader.loadProject();
  if (loaded)
  {
    Serial.println(BOARD_LOG "loaded deployed project from /PROJECT");
    // The first screen of the main row (contract 2.10; before it, the first
    // screen that is not hidden, which is what an older export's row is).
    currentScreen = screengraphproject::startIndex(projectLoader.getProject());
  }
  else
  {
    Serial.println(BOARD_LOG "no deployed project - falling back to the built-in fixture");
    File f = LittleFS.open("/render-smoke.json", "w");
    if (!f)
    {
      Serial.println(BOARD_LOG "could not write the fallback project");
      return;
    }
    f.print(TEST_PROJECT_JSON);
    f.close();
    loaded = projectLoader.loadProject("/render-smoke.json");
  }
  if (!loaded)
  {
    Serial.println(BOARD_LOG "loadProject FAILED");
    return;
  }
  // The theme may have been reported before there was a project to show it.
  projectLoader.applyThemeVariant(wantedThemeDark);
  Serial.printf(BOARD_LOG "project \"%s\", %ux%u, %u screen(s)\n",
                projectLoader.getProject().name.c_str(),
                projectLoader.getProject().screenWidth,
                projectLoader.getProject().screenHeight,
                (unsigned)projectLoader.getProject().screens.size());

  // 800*480*2 = 768000 bytes. Internal SRAM has nothing like that free, so a
  // non-null buffer is itself the proof it landed in PSRAM - but report
  // where, because "it worked" and "it worked for the reason I assumed" are
  // different claims.
  canvas = new ClippedCanvas16(PANEL_W, PANEL_H);
  if (!canvas || !canvas->getBuffer())
  {
    Serial.printf(BOARD_LOG "FATAL: %u-byte canvas allocation failed\n",
                  (unsigned)(PANEL_W * PANEL_H * 2));
    return;
  }
  Serial.printf(BOARD_LOG "canvas at %p (%s), psram free %u\n", canvas->getBuffer(),
                esp_ptr_external_ram(canvas->getBuffer()) ? "PSRAM" : "internal SRAM",
                (unsigned)ESP.getFreePsram());

  renderer = new ColorScreenRenderer(projectLoader, canvas);
  renderer->setChartFeed(&chartFeed);
  renderer->setCameraFeed(&cameraFeed);
  cameraFeed.setMinPeriodMs(CAMERA_MIN_PERIOD_MS);
  // {device:id} in a text (designer docs/2026-10-05-placeholder-devices.md).
  // The id is fixed at construction, so it is there before the first draw.
  renderer->setDeviceId(mqttClient.getClientId());
  // An open popup shows a close button on its window's top right corner
  // (designer docs/device-contract.md 2.5) - this board first (the user,
  // 2026-10-07). 48 px across, about 5.5 mm here.
  renderer->setPopupCloseBadgeRadius(POPUP_CLOSE_BADGE_RADIUS);
  // A cover moved: only where it was and is, drawn and blitted.
  guards.begin(renderer, [](int16_t x, int16_t y, int16_t w, int16_t h, const char *reason) { renderRegion(x, y, w, h, reason, true); });

  // A second canvas of the same size, so the incoming screen can be drawn
  // while the outgoing one is still on the panel. Taken at boot rather than
  // at the first gesture: there are 6.5MB of PSRAM free and an allocation
  // that failed mid-swipe would have nowhere to go.
  spareCanvas = new ClippedCanvas16(PANEL_W, PANEL_H);
  markerCanvas = new ClippedCanvas16(PANEL_W, PANEL_H);
  if (spareCanvas && spareCanvas->getBuffer())
  {
    swipe.attach(&panel, &canvas, spareCanvas, renderer, &currentScreen,
                 (int)projectLoader.getProject().screens.size(), PANEL_W, PANEL_H,
                 markerCanvas && markerCanvas->getBuffer() ? markerCanvas : nullptr);
    Serial.printf(BOARD_LOG "swipe %s: %u screens, spare canvas at %p, psram free %u\n",
                  swipe.ready() ? "ready" : "idle (needs two screens)",
                  (unsigned)projectLoader.getProject().screens.size(), spareCanvas->getBuffer(),
                  (unsigned)ESP.getFreePsram());
  }
  else
  {
    Serial.println(BOARD_LOG "spare canvas allocation failed - no transitions");
  }

  // Every font the project uses, before the first render - as the knob has
  // done since it had fonts to parse (src/main.cpp). Parsed on first use
  // instead, a 4-bit font (150-250 KB of hex) cost the first draw of each
  // new screen half a second: popups opened in 1.1 s, taps and swipes
  // during it went by (2026-11-01, designer docs/2026-11-01-4bit-fonts.md).
  if (projectLoader.isLoaded())
    for (const Screen &screen : projectLoader.getProject().screens)
      for (const ScreenObject &o : screen.objects)
        if (o.properties.camera && !cameraFeed.allocate()) Serial.println("[camera] no PSRAM for the pictures");
  renderer->preloadFonts();
  renderAndPresent("boot");
}

static void subscribeToProjectTopics()
{
  if (topicsSubscribed || !projectLoader.isLoaded() || !mqttClient.isConnected()) return;
  std::vector<String> topics;
  // A local topic is the display's own and never subscribed (contract 2.9).
  for (const auto &t : projectLoader.getProject().topics)
    if (!t.local) topics.push_back(t.topic);
  if (!topics.empty()) mqttClient.subscribeToKeys(topics);
  topicsSubscribed = true;
  Serial.printf(BOARD_LOG "subscribed to %d project topic(s)\n", (int)topics.size());
}

static void enterSetupMode()
{
  if (setupModeActive || !wifiSetupServer) return;
  Serial.println(BOARD_LOG "entering setup mode - starting AP");
  setupModeActive = true;
  // The test interface owns port 80 in normal operation and the setup server
  // wants the same port, so it has to be stopped rather than left to fail a
  // bind nothing checks. In setup mode the device serves no snapshots and
  // accepts no deploys anyway.
  testInterface.stop();
  wifiSetupServer->startAP();
}

// A late WiFi, on demand, for hil/late-wifi.js (2026-09-14).
//
// GET /api/debug?set=latewifi=<seconds> restarts the board, and the boot
// that follows holds WiFi.begin() back for that long - a stand-in for an
// access point that takes its time to associate, which is what a board
// restarted mid-conformance-run sometimes met. RTC_NOINIT survives
// ESP.restart() and nothing else, so a power cycle can never inherit it.
static const uint32_t LATE_WIFI_MAGIC = 0x1A7E3F1F;
RTC_NOINIT_ATTR uint32_t lateWifiMagic;
RTC_NOINIT_ATTR uint32_t lateWifiSeconds;
static uint32_t lateWifiBootSeconds = 0;  // what this boot was asked for, for the report
static uint32_t restartAtMs = 0;          // nonzero: restart once the response is out
static WiFiSupervisor wifi;
static TaskLoad taskLoad;  // ?set=tasks=1
// Set while "WLAN nicht erreichbar" is on the panel, so the project comes
// back the moment the link does rather than on the next value.
static bool noWifiScreenShown = false;

// Local topics (designer contract 2.9): a write to one waits here for loop(),
// which applies it as an arriving message - after the switch that wrote it
// has marked what it asked for, so the value answers that mark.
static std::vector<std::pair<String, String>> localWrites;

// Every write to a project topic: a local one is this display's value, any
// other goes to the broker - not retained, a command, not a state. True when
// it was taken.
static bool writeProjectTopic(const String &topic, const String &payload)
{
  if (projectLoader.isLoaded() && projectLoader.isLocalTopic(topic))
  {
    localWrites.push_back({topic, payload});
    return true;
  }
  return mqttClient.publish(topic, payload, false);
}

// A project topic's new value, from the broker or - for a local topic - from
// a write on this display (writeProjectTopic, applied from loop()): kept, and
// drawn as an arriving message is.
static void applyTopicValue(const String &topic, const String &payload)
{
  // The answer a switch's mark is waiting for - its own read topic, and no
  // other. The theme's answer is one too, so this comes before it.
  if (!pendingSwitchId.isEmpty() && topic == pendingReadTopic) pendingAnswered = true;

  // Light or dark. Before the project's own topics: it is not one of them
  // unless a Theme switch reads it too, which then gets the value as well.
  if (topic == String(TOPIC_PREFIX) + "/state/theme")
  {
    wantedThemeDark = themevariant::isDarkPayload(payload);
    if (projectLoader.isLoaded()) projectLoader.setTopicValue(topic, payload);
    applyWantedTheme();
    // The navigator's light/dark switch shows the value even where the
    // variant stays (a first "light"): drawn again, as applyWantedTheme
    // only does on a change (designer contract 2.7, 2026-10-11).
    if (projectLoader.isLoaded()) renderPending = true;
    return;
  }
  // The installation's word ends the request: the marker goes, and the fill
  // shows what was reported - even when that is not what was asked for
  // (decision 6c).
  if (renderer != nullptr && !levelAskedTopic.isEmpty() && topic == levelAskedTopic)
  {
    // After the release, only the answer to the value it was released on
    // ends the request. The answers to the values the drag passed through
    // are still on their way then, and each used to end it and be drawn -
    // the fill went to one of them and on to the end value, two jumps
    // after the finger lifted (2026-09-27, in the van). Kept, not drawn,
    // until the end value's answer comes or the wait runs out (loop()).
    //
    // And while the finger is still down, no answer ends it: the finger is
    // still asking. An answer used to, and when the finger then rested
    // before lifting, nothing set the marker again - the handle fell back
    // to a value the drag had passed through, then went on to the end value.
    const bool fingerDown = !levelDragObjId.isEmpty();
    const bool awaiting = !levelAwaitValue.isEmpty() && (long)(levelAwaitUntilMs - millis()) > 0 &&
                          fabsf(payload.toFloat() - levelAwaitValue.toFloat()) > 0.001f;
    if (fingerDown || awaiting)
    {
      projectLoader.setTopicValue(topic, payload);
      return;
    }
    renderer->clearAskedValue();
    levelAskedTopic = "";
    levelAwaitValue = "";
  }

  // Read before it is overwritten: the partial redraw needs the value the
  // screen is still showing.
  const String previous = projectLoader.getTopicValue(topic);
  projectLoader.setTopicValue(topic, payload);

  bool known = false;
  for (int i = 0; i < pendingTopicCount; i++)
  {
    if (pendingTopics[i].topic == topic) { known = true; break; }
  }
  if (!known)
  {
    if (pendingTopicCount < MAX_PENDING_TOPICS)
    {
      pendingTopics[pendingTopicCount].topic = topic;
      pendingTopics[pendingTopicCount].oldValue = previous;
      pendingTopicCount++;
    }
    else
    {
      // More topics than rectangles worth drawing. A full screen is the
      // honest answer rather than a partial one that misses something.
      pendingTopicOverflow = true;
    }
  }

  // Never drawn from here. This runs inside mqttClient.loop(), so a
  // render costing 180ms happened between two touch samples and, during a
  // transition, drew the outgoing screen straight over the frame being
  // composed. Measured on a real gesture: 7 composed frames in 3.4
  // seconds of dragging, the rest of the time spent here.
  //
  // The value is stored the moment it lands. Only the drawing waits, and
  // the loop decides when.
  renderPending = true;
}

static void setupNetworking()
{
  if (!canvas) return;

  displayAdapter = new PanelDisplayAdapter(*canvas, panel, PANEL_W, PANEL_H);
  configStorage.begin();
  wifiSetupServer = new WiFiSetupServer(*displayAdapter, configStorage);

  WiFiCredentials creds;
  if (!configStorage.loadWiFiCredentials(creds) || creds.ssid.isEmpty())
  {
    Serial.println(BOARD_LOG "no WiFi credentials - starting setup AP");
    setupModeActive = true;
    wifiSetupServer->startAP();
    return;
  }

  Serial.printf(BOARD_LOG "connecting to \"%s\"\n", creds.ssid.c_str());
  // WiFiSupervisor turns the radio's power save off, as the knob has had it
  // since 2026-08-24 - a cross-device building block this port never copied.
  // Measured on this board on 2026-09-14, interleaved on the same minutes:
  // ping averaged 67-96ms with the factory WIFI_PS_MIN_MODEM and 7-35ms
  // without, and HTTP answers dropped to about 20ms. ?set=wifips=min puts
  // the factory mode back for a comparison.
  uint32_t holdBackMs = 0;
  if (lateWifiMagic == LATE_WIFI_MAGIC)
  {
    lateWifiMagic = 0;
    lateWifiBootSeconds = lateWifiSeconds;
    holdBackMs = lateWifiSeconds * 1000UL;
    Serial.printf(BOARD_LOG "WiFi held back %us by ?set=latewifi\n", (unsigned)lateWifiSeconds);
  }
  wifi.begin(creds.ssid, creds.password, holdBackMs);
  for (int i = 0; i < 40 && !wifi.up(); i++)
  {
    wifi.tick();
    delay(250);
  }

  // A wait, not a verdict. Everything below is set up either way - the
  // server binds without an address and MQTT retries on its own - because
  // returning here is what left a board on the network with nothing
  // listening until its next restart (2026-09-14, hil/late-wifi.js).
  if (!wifi.up())
  {
    Serial.println(BOARD_LOG "no WiFi yet - carrying on, the supervisor keeps trying");
    displayAdapter->showLines({"WLAN nicht erreichbar", creds.ssid});
    noWifiScreenShown = true;
  }

  // The snapshot endpoint reads this canvas directly, which is what gives
  // this board a HIL comparison at all.
  if (testInterface.begin(*canvas)) Serial.println(BOARD_LOG "test interface on :80");

  // The three the HIL orchestrator drives. Only the debug handler was wired
  // before, which is why no pixel-parity run has ever been possible on this
  // board even though the endpoints serving it were already there.
  // What is on the glass rather than in the canvas: during a swipe they
  // differ, which is what hil/navigator.js reads to see the strip stand still.
  testInterface.setPanelShadowHandler([]() -> const uint16_t * { return panel.front(); });

  testInterface.setScreenSwitchHandler([](int index) -> bool {
    if (!projectLoader.isLoaded()) return false;
    if (index < 0 || index >= (int)projectLoader.getProject().screens.size()) return false;
    currentScreen = index;
    openPopup = -1;
    forgetTouchFeedback();
    renderAndPresent("test interface: screen switch");
    return true;
  });

  // Lets a test ask "has the device applied this value yet" instead of
  // sleeping and hoping - the fix for a whole class of flaky HIL runs.
  testInterface.setTopicValueHandler(
      [](const String &topic) { return projectLoader.getTopicValue(topic); });

  testInterface.setSettingsChangedHandler(applyDeviceSettings);
  // The Device tab's idle-screen dropdown is built from this.
  testInterface.setScreenListHandler([]() -> String {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    if (projectLoader.isLoaded())
    {
      for (const auto &screen : projectLoader.getProject().screens)
      {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = screen.id;
        o["name"] = screen.name;
      }
    }
    String out;
    serializeJson(doc, out);
    return out;
  });

  testInterface.setTouchHandler([](int16_t x, int16_t y, bool down) {
    injectX = x;
    injectY = y;
    injectTwo = false;
    if (down && !injectDown) contactSampleUs = micros();
    injectDown = down;
    // A posted lift ends the window rather than extending it, so the
    // release runs against the real panel - which is also reading idle -
    // instead of against 400ms of frozen injected state.
    injectUntilMs = down ? millis() + TOUCH_INJECT_HOLD_MS : 0;
    injectLiftOnce = !down;
  });
  testInterface.setTwoFingerTouchHandler([](int16_t x, int16_t y, int16_t x2, int16_t y2, bool down) {
    injectX = x;
    injectY = y;
    injectX2 = x2;
    injectY2 = y2;
    injectTwo = down;
    if (down && !injectDown) contactSampleUs = micros();
    injectDown = down;
    injectUntilMs = down ? millis() + TOUCH_INJECT_HOLD_MS : 0;
    injectLiftOnce = !down;
  });

  // GET /api/debug?set=bench=1 measures where a swipe frame goes. Behind a
  // parameter rather than run at boot because it writes rubbish into the
  // framebuffer for a few hundred milliseconds before re-rendering, and
  // nobody wants that on a panel that is showing a project.
  testInterface.setPickerHandler([]() -> String {
    if (!pickerOpen) return String("{\"open\":false}");
    return String(timepicker::describe(pickerState, pickerFrame).c_str());
  });
  testInterface.setKeyboardHandler([]() -> String {
    if (!keyboardOpen) return String("{\"open\":false}");
    return String(keyboard::describe(keyboardState, keyboardFrame).c_str());
  });
  testInterface.setDebugHandler([](const String &setArg) -> String {
    // ?set=charts: the charts' history it holds (ChartFeed::describe).
    if (setArg == "charts") return chartFeed.describe();
    // ?set=coredump: where the last panic happened (BootReport::coreDump).
    if (setArg == "coredump") return BootReport::coreDump();
    // ?set=extmem=<bytes>: from what size a malloc goes to PSRAM, from now on
    // (setup() sets it at boot) - to weigh a render's speed against it.
    if (setArg.startsWith("extmem="))
    {
      heap_caps_malloc_extmem_enable((size_t)setArg.substring(7).toInt());
      return String("extmem ") + setArg.substring(7) + "\n";
    }
    // ?set=chartpulse=off stills the NowPoint for a test comparing pixels; =on again.
    if (setArg == "chartpulse=off" || setArg == "chartpulse=on")
    {
      chartFeed.setPulsing(setArg == "chartpulse=on");
      return chartFeed.describe();
    }
    // ?set=theme=dark|light: what schaltli/state/theme would do, without a
    // broker - for the HIL dark pass and for looking at it by hand.
    if (setArg.startsWith("theme="))
    {
      wantedThemeDark = themevariant::isDarkPayload(setArg.substring(6));
      applyWantedTheme();
    }
    // ?set=marker=<screen index>,<px>: the frame a swipe towards that screen
    // shows with the navigator's marker that far on, on the glass alone -
    // for hil/navigator.js to hold against the designer's (#59).
    String markerShown;
    if (setArg.startsWith("marker="))
    {
      const int comma = setArg.indexOf(',');
      const bool shown = comma > 0 && swipe.showMarker(setArg.substring(7, comma).toInt(), setArg.substring(comma + 1).toInt());
      markerShown = String("marker ") + (shown ? "shown" : "not shown: it would not glide there") + "\n";
    }
    String out = markerShown + "ip " + WiFi.localIP().toString() + "  rssi " + String(WiFi.RSSI()) + " dBm\n";
    out += String("theme ") + (wantedThemeDark ? "dark" : "light") + ", shown " +
           (projectLoader.isLoaded() && projectLoader.getProject().themeDark ? "dark" : "light") +
           " (?set=theme=dark|light)\n";
    // The lowest since boot and the largest block: an fopen with none left
    // aborted the board in the van (2026-10-30, ?set=coredump).
    out += "heap " + String(ESP.getFreeHeap() / 1024) + " KB (lowest " + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024) +
           " KB, largest block " + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024) + " KB), psram free " + String(ESP.getFreePsram() / 1024) + " KB\n";
    out += "uptime " + String(millis() / 1000) + " s\n";
    {
      // The radio's power save, switchable live so it can be measured on and
      // off against the same board, room and minute (2026-09-14). Straight to
      // esp_wifi_set_ps() rather than WiFi.setSleep(true), which in this core
      // selects the deeper mode rather than the factory WIFI_PS_MIN_MODEM -
      // see the knob's setWiFiPowerSave() in src/main.cpp. Set before
      // reporting, so the line below already shows what was asked for.
      if (setArg == "wifips=none") esp_wifi_set_ps(WIFI_PS_NONE);
      if (setArg == "wifips=min") esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
      wifi_ps_type_t ps = WIFI_PS_NONE;
      esp_wifi_get_ps(&ps);
      out += String("wifi power save ") +
             (ps == WIFI_PS_NONE ? "none" : ps == WIFI_PS_MIN_MODEM ? "min modem" : "max modem") +
             " (?set=wifips=none|min)\n";
      out += wifi.report();
      if (setArg == "camerastats=reset") cameraResetStats();
      if (setArg.startsWith("cameraperiod=")) cameraFeed.setMinPeriodMs((uint32_t)setArg.substring(13).toInt());
      out += cameraReport() + "\n";
      if (setArg == "wifiscan=1") out += wifi.scanReport();
      if (setArg.startsWith("pclk="))
      {
        const uint32_t hz = (uint32_t)setArg.substring(5).toInt();
        out += "pixel clock -> " + String(hz) + " Hz: " + (panel.setPixelClock(hz) ? "ok" : "refused") +
               " (" + String(PCLK_HZ) + " is this build's default; 8000000 showed no picture)\n";
      }
      if (setArg == "tasks=1")
      {
        out += taskLoad.report();
        // Internal memory alongside the task load: with
        // CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP the core falls back to PSRAM
        // for WiFi and lwIP buffers when internal heap runs short, and PSRAM
        // is the bus this board's panel refresh never stops using.
        out += "  internal heap free " + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024) +
               " KB, largest block " +
               String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024) +
               " KB, lowest ever " + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024) +
               " KB\n";
      }
      if (lateWifiBootSeconds > 0)
        out += "wifi held back " + String(lateWifiBootSeconds) + " s at boot (?set=latewifi=<s>)\n";
      if (setArg.startsWith("latewifi="))
      {
        lateWifiSeconds = (uint32_t)setArg.substring(9).toInt();
        lateWifiMagic = LATE_WIFI_MAGIC;
        restartAtMs = millis() + 500;
        out += "restarting with WiFi held back " + String(lateWifiSeconds) + " s\n";
      }
    }
    // Which image is running, so an update that arrived over the network
    // can be told from a device that merely rebooted. The MD5 is of the
    // running sketch and changes with every build; the timestamp is this
    // file's last compile, which is close enough to say "that one".
    out += "firmware " + ESP.getSketchMD5() + ", " + String(ESP.getSketchSize() / 1024) +
           " KB, built " + String(__DATE__) + " " + String(__TIME__) + "\n";
    out += BootReport::line();
    out += FirmwareImage::debugLine();
    out += panel.report();
    out += "  last tap: " + lastTap + "\n";
    out += "  popup: " + (openPopup >= 0 ? String("open, index ") + String(openPopup) : String("none open")) +
           " - last: " + lastPopup + "\n";
    out += "  last feedback: " + lastFeedback + "\n";
    out += "  tap latency: " + tapLatencyReport() + "\n";
    out += "  energy motion: " + energyReport() + "\n";
    if (renderer) out += "  energy draw: " + renderer->energyProfile() + "\n";
    // The font cache (DdfFontStore): what is parsed, what was let go to keep
    // PSRAM free (designer docs/2026-11-01-4bit-fonts.md).
    if (renderer) out += "  fonts: " + renderer->fontReport() + " (?set=fontreserve=<KB>)\n";
    // Where a swipe leads from here, each way (pagingTargetFor): -1 is
    // nowhere - what a swipe that never follows the finger comes down to.
    out += "  swipes lead: finger left " + String(pagingTargetFor(-1)) + ", right " + String(pagingTargetFor(1)) +
           ", up " + String(pagingTargetFor(-1, true)) + ", down " + String(pagingTargetFor(1, true)) + " (screen " +
           String(currentScreen) + ")\n";
    out += "  last value draw: " + lastValueDraw + "\n";
    out += "  partial check: " + lastPartialCheck + " (" + String(partialChecks) + " checked, " + String(partialCheckFailures) +
           " differed; ?set=partialcheck=on|off)\n";
    out += "  display " + String(displayIsOff ? "off" : "on") + ", blanking after " +
           String(displayOffAfterMs / 1000) + " s, idle screen \"" + idleScreenId + "\"\n";
    out += swipe.report();
    out += "  contacts " + String(contactCount) + ", of them missed " + String(missedContacts) +
           " (most samples in a missed one " + String(missedMaxSamples) + ", widest dx " +
           String(missedMaxDx) + " px)\n";
    {
      const unsigned long windowMs = millis() - dutyWindowStartMs;
      out += "  value renders " + String(topicRenders) + " in " + String(windowMs / 1000.0f, 1) +
             " s = " + String(windowMs ? topicRenderUs / 10.0f / windowMs : 0.0f, 1) +
             "% of the time blind" + String(topicRenderEnabled ? "" : "  (rendering is OFF)") + "\n";
      out += "  broker " + (brokerHost.isEmpty() ? String("(none configured)")
                                               : brokerHost + ":" + String(brokerPort)) + "\n";
    out += "  loop " + String(loopTurns) + " turns, " +
             String(loopTurns ? windowMs * 1000.0f / loopTurns / 1000.0f : 0.0f, 1) +
             " ms each on average, worst turn " + String(worstTurnUs / 1000.0f, 1) + " ms\n";
      out += "  worst web " + String(worstWebUs / 1000.0f, 1) + " ms, worst mqtt " +
             String(worstMqttUs / 1000.0f, 1) + " ms, worst touch read " +
             String(worstTouchUs / 1000.0f, 1) + " ms\n";
      out += "  totals over the window: web " + String(webUs / 1000) + " ms, mqtt " +
             String(mqttUs / 1000) + " ms, touch " + String(touchUs / 1000) + " ms, mqtt " +
             String(mqttClient.isConnected() ? "connected" : "NOT connected") + "\n";
    }
    if (setArg == "bench=1" && canvas)
    {
      out += "\n" + PanelBenchmark::run(panel.back(), canvas->getBuffer(), PANEL_W, PANEL_H);
      renderAndPresent("after benchmark");
    }
    else if (setArg.startsWith("regionbench=") && canvas && renderer)
    {
      // ?set=regionbench=x,y,w,h: what redrawing one rectangle of the current
      // screen costs - the path a value change takes - first call and the
      // best of five after it. Put in for a screen-sized icon behind a live
      // value (designer #23): the first call may fill the image cache, every
      // later one is what a dimmer being dragged over it pays each time.
      int v[4] = {0, 0, 0, 0};
      String rest = setArg.substring(String("regionbench=").length());
      for (int i = 0; i < 4; i++)
      {
        const int comma = rest.indexOf(',');
        v[i] = (comma < 0 ? rest : rest.substring(0, comma)).toInt();
        rest = comma < 0 ? String("") : rest.substring(comma + 1);
      }
      uint32_t t0 = micros();
      renderer->renderScreenRegion(currentScreen, v[0], v[1], v[2], v[3]);
      const uint32_t first = micros() - t0;
      uint32_t best = UINT32_MAX, worst = 0;
      for (int i = 0; i < 5; i++)
      {
        t0 = micros();
        renderer->renderScreenRegion(currentScreen, v[0], v[1], v[2], v[3]);
        const uint32_t d = micros() - t0;
        if (d < best) best = d;
        if (d > worst) worst = d;
      }
      renderAndPresent("after regionbench");
      out += "\nregionbench " + String(v[0]) + "," + String(v[1]) + " " + String(v[2]) + "x" + String(v[3]) +
             ": first " + String(first / 1000.0f, 1) + " ms, best " + String(best / 1000.0f, 1) + " ms, worst " +
             String(worst / 1000.0f, 1) + " ms\n";
    }
    else if (setArg == "render=1" && canvas && renderer)
    {
      // What a value change actually costs, measured before anything is
      // instrumented. The breakdown further down is inflated by its own
      // probes - two cycle-counter reads on every one of 129,600 arc
      // pixels - so quoting a profiled total as the render time would
      // overstate it, and this board has been burnt by a number that
      // nobody re-checked once already.
      //
      // Both halves are here because a value arriving over MQTT pays for
      // both: renderScreen() draws into the PSRAM canvas, and the whole
      // canvas is then pushed to the panel.
      out += "\nrender (screen " + String(currentScreen) + ")\n";

      uint32_t plainBest = UINT32_MAX, plainWorst = 0;
      for (int i = 0; i < 5; i++)
      {
        uint32_t p0 = micros();
        renderer->renderScreen(currentScreen);
        uint32_t d = micros() - p0;
        if (d < plainBest) plainBest = d;
        if (d > plainWorst) plainWorst = d;
      }

      // The same call the render path makes, so the number is the real
      // one and not a benchmark standing in for it.
      uint32_t blitBest = UINT32_MAX;
      for (int i = 0; i < 5; i++)
      {
        uint32_t b0 = micros();
        panel.blit(canvas->getBuffer());
        uint32_t d = micros() - b0;
        if (d < blitBest) blitBest = d;
      }

      out += "  render          " + String(plainBest / 1000.0f, 1) + " ms  (best of 5, worst " + String(plainWorst / 1000.0f, 1) + ")\n";
      out += "  blit to panel   " + String(blitBest / 1000.0f, 1) + " ms  (best of 5)\n";
      out += "  a value change costs both: " + String((plainBest + blitBest) / 1000.0f, 1) + " ms\n";

      out += "\nbreakdown (profiled, so inflated - see the overhead line)\n";

      RenderProfile::setEnabled(true);

      // No cold-cache run is reported, because there is no way to get one
      // here: the board renders this screen at boot, so every asset it
      // uses is already decoded and cached by the time anyone can ask.
      // An earlier version labelled the first run "first render" as though
      // it were cold, which was simply wrong.
      uint32_t bestUs = UINT32_MAX;
      String bestReport;
      for (int i = 0; i < 3; i++)
      {
        RenderProfile::reset();
        uint32_t s0 = micros();
        renderer->renderScreen(currentScreen);
        uint32_t d = micros() - s0;
        if (d < bestUs)
        {
          bestUs = d;
          bestReport = RenderProfile::report();
        }
      }

      RenderProfile::setEnabled(false);

      out += "  profiled render " + String(bestUs / 1000.0f, 1) + " ms  (best of 3)\n";
      out += bestReport;
      out += "  profiler overhead " + String((bestUs > plainBest ? bestUs - plainBest : 0) / 1000.0f, 1) + " ms - subtract it from every line above\n";
      out += "  floor for touching every pixel once: 25 ms (see PanelBenchmark.h)\n";

      renderAndPresent("after render profile");
    }
    else if (setArg == "taplatency=clear")
    {
      tapLatencyCount = 0;
      tapLatencyNext = 0;
      out += "tap latency cleared\n";
    }
    else if (setArg == "swipes=1" || setArg == "swipes=clear")
    {
      if (setArg == "swipes=clear")
      {
        swipe.clearLog();
        contactCount = 0;
        contactSamples = 0;
        missedContacts = 0;
        missedMaxSamples = 0;
        missedMaxDx = 0;
        topicRenders = 0;
        topicRenderUs = 0;
        dutyWindowStartMs = millis();
        loopTurns = 0;
        webUs = mqttUs = touchUs = 0;
        worstWebUs = worstMqttUs = worstTouchUs = worstTurnUs = 0;
        out += "\ngesture log cleared\n";
      }
      else
      {
        out += "\ngesture log\n" + swipe.log();
      }
    }
    else if (setArg == "pinches=1" || setArg == "pinches=clear" || setArg == "pinches=show" || setArg == "pinches=hide")
    {
      if (setArg == "pinches=show" || setArg == "pinches=hide")
      {
        pinchShow = setArg == "pinches=show";
        out += String("\npinches are ") + (pinchShow ? "now shown on the panel" : "no longer shown") + "\n";
      }
      else if (setArg == "pinches=clear")
      {
        pinchLog = "";
        pinchIns = pinchOuts = pinchNones = 0;
        out += "\npinch log cleared\n";
      }
      else
      {
        out += "\npinches: " + String(pinchIns) + " in, " + String(pinchOuts) + " out, " + String(pinchNones) + " neither\n";
        out += "pinch log\n" + pinchLog;
      }
    }
    else if (setArg.startsWith("fontreserve="))
    {
      DdfFontStore::setReserveBytes((size_t)setArg.substring(12).toInt() * 1024);
      out += "\nfont reserve now " + String(DdfFontStore::reserveBytes() / 1024) + " KB\n";
    }
    else if (setArg == "partialcheck=on" || setArg == "partialcheck=off")
    {
      partialCheck = setArg == "partialcheck=on";
      partialChecks = partialCheckFailures = 0;
      lastPartialCheck = partialCheck ? "on, nothing drawn in part yet" : "off";
    }
    else if (setArg == "topicrender=off" || setArg == "topicrender=on")
    {
      topicRenderEnabled = setArg == "topicrender=on";
      out += String("\nvalue rendering is now ") + (topicRenderEnabled ? "on" : "off") + "\n";
      if (topicRenderEnabled) renderAndPresent("rendering back on");
    }
    else if (setArg == "aspect=1" && canvas)
    {
      // A square of equal pixel counts, for measuring with a ruler.
      //
      // Why the panel and not the datasheet: a circle was reported as wider
      // than tall, and the render turned out to be exactly round - 360px
      // across and 360 down. So the distortion is the panel's pixel pitch,
      // and the way to learn it is to measure something drawn rather than
      // to work backwards from the case, where a bezel eats a few
      // millimetres of height and turns a 6% error into an apparent 16%.
      //
      // 400x400 with a cross through the middle: the block is easy to lay a
      // ruler along, and the arms say where its edges are if the corners
      // fall under a rounded bezel.
      holdMeasuringPattern();
      const int16_t side = 400;
      const int16_t x0 = (int16_t)((PANEL_W - side) / 2);
      const int16_t y0 = (int16_t)((PANEL_H - side) / 2);
      canvas->fillScreen(0x0000);
      canvas->fillRect(x0, y0, side, side, 0xFFFF);
      canvas->fillRect(x0 + side / 2 - 1, 0, 2, PANEL_H, 0xF800);
      canvas->fillRect(0, y0 + side / 2 - 1, PANEL_W, 2, 0xF800);
      panel.blit(canvas->getBuffer());
      out += "\nmeasuring square: " + String(side) + "x" + String(side) +
             " px, centred, red cross through its middle\n";
      out += "  measure the WHITE block, width and height, in mm\n";
      out += "  the panel ignores touches until you put the project back\n";
      out += "  equal numbers mean square pixels; wider means a circle cannot look round\n";
      out += "  ?set=aspect=0 puts the project back\n";
    }
    else if (setArg == "circle=1" && canvas)
    {
      // A closed ring of exactly 400px outer diameter, drawn through the
      // project's own rasterizer.
      //
      // Why not simply trust the measurement already taken from a snapshot:
      // the project's dial is open at the bottom, so its vertical diameter
      // had to be inferred from the centre and the top rather than read off.
      // A closed ring can be laid a ruler against, and it exercises
      // ArcRaster on the way, so a bug in the rasterizer could not hide
      // behind a hand-drawn circle either.
      holdMeasuringPattern();
      const int32_t side = 400;
      const int16_t x0 = (int16_t)((PANEL_W - side) / 2);
      const int16_t y0 = (int16_t)((PANEL_H - side) / 2);

      ArcRaster::RingGeometry geom;
      geom.size = side;
      geom.thickness = 20;
      geom.track = ArcRaster::makeSector(0, 360 * ArcRaster::ANGLE_SCALE);
      geom.fill = ArcRaster::makeSector(0, 0);
      // A ring that goes all the way round has no ends to round and no handle
      // on it, so the outer diameter a ruler reads is the one asked for here.

      const ArcRaster::Rgb565 ink = ArcRaster::toRgb565(0xFFFF);
      const ArcRaster::Rgb565 back = ArcRaster::toRgb565(0x0000);
      const ArcRaster::Rgb565 none = ArcRaster::toRgb565(0x0000);

      canvas->fillScreen(0x0000);
      const uint32_t t0 = micros();
      for (int32_t py = 0; py < side; py++)
      {
        const ArcRaster::RowSpan span = ArcRaster::rowSpan(geom, py);
        for (int32_t px = span.x0; px <= span.x1; px++)
        {
          if (span.holeX1 >= span.holeX0 && px >= span.holeX0 && px <= span.holeX1) continue;
          const ArcRaster::PixelBands bands = ArcRaster::pixelBands(geom, px, py);
          const int32_t covered = bands.fill + bands.track + bands.handle;
          if (covered == 0) continue;
          const uint16_t mixed = ArcRaster::blendBands(none, bands.fill, ink, bands.track, none,
                                                       bands.handle, back,
                                                       (uint8_t)(ArcRaster::COVERAGE_MAX - covered));
          canvas->drawPixel(x0 + px, y0 + py, mixed);
        }
      }
      const uint32_t drawUs = micros() - t0;

      // Marks at the four extremes, so a ruler has something to aim at.
      canvas->fillRect(x0 - 30, y0 + side / 2 - 1, 24, 2, 0xF800);
      canvas->fillRect(x0 + side + 6, y0 + side / 2 - 1, 24, 2, 0xF800);
      canvas->fillRect(x0 + side / 2 - 1, y0 - 30, 2, 24, 0xF800);
      canvas->fillRect(x0 + side / 2 - 1, y0 + side + 6, 2, 24, 0xF800);
      panel.blit(canvas->getBuffer());

      out += "\nclosed ring: outer diameter " + String(side) + " px both ways, drawn in " +
             String(drawUs / 1000.0f, 1) + " ms\n";
      out += "  red marks sit level with its outer edge on all four sides\n";
      out += "  measure across the outside of the ring, horizontally and vertically\n";
      out += "  the panel ignores touches until ?set=aspect=0 puts the project back\n";
    }
    else if (setArg == "aspect=0")
    {
      measuringPattern = false;
      renderAndPresent("after the measuring square");
      out += "\nproject restored\n";
    }
    else if (setArg.startsWith("setupscreen=") && wifiSetupServer)
    {
      // Draws the real AP setup screen, the one WiFiSetupServer draws in
      // setup mode, into the canvas without bringing the AP up - setup mode
      // stops this server, so /snapshot.bmp could otherwise never show it.
      // hil/waveshare4v3b/setup-screen.js reads the QR code back out of it.
      // Held like the measuring square, so an arriving value cannot paint
      // the project back over it before the snapshot is taken.
      holdMeasuringPattern();
      wifiSetupServer->showSetupScreenAP(setArg.substring(12).toInt(), false);
      out += "\nsetup screen drawn, ?set=aspect=0 puts the project back\n";
    }
    else if (setArg.startsWith("holdcountdown=") && displayAdapter)
    {
      // The setup hold's countdown as a held finger shows it, without the
      // finger - a snapshot taken during a hold would lift it. The PaperS3's
      // has the same name; setup-screen.js reads this one for its edges.
      holdMeasuringPattern();
      showHoldCountdown(setArg.substring(14).toInt());
      out += "\nhold countdown drawn, ?set=aspect=0 puts the project back\n";
    }
    else if (setArg == "arcselftest=1")
    {
      // Needs no project, no broker and no fixture - it compares the
      // rasterizer against itself, which is why it can live on the device.
      bool ok = true;
      out += "\n" + ArcSelfTest::run(ok);
    }
    else
    {
      out += "\nadd ?set=bench=1 for panel bandwidth, ?set=render=1 for the render breakdown, ";
      out += "?set=arcselftest=1 to prove the arc short cut, ";
      out += "?set=swipes=1 for the gesture log (?set=swipes=clear to reset it), ";
      out += "?set=pinches=1 for the pinch log (?set=pinches=clear to reset it, ?set=pinches=show|hide to see each on the panel), ";
      out += "?set=topicrender=off to judge a gesture with nothing else drawing, ";
      out += "?set=aspect=1 for a 400x400 measuring square, ?set=circle=1 for a closed 400px ring, ";
      out += "?set=setupscreen=<seconds> for the AP setup screen, ?set=holdcountdown=<seconds> for the setup hold's countdown ";
      out += "(?set=aspect=0 puts the project back)\n";
    }
    return out;
  });

  if (creds.mqttHost.isEmpty())
  {
    Serial.println(BOARD_LOG "no broker configured - skipping MQTT");
    return;
  }

  brokerHost = creds.mqttHost;
  brokerPort = (uint16_t)creds.mqttPort;
  mqttClient.configure(creds.mqttHost, (uint16_t)creds.mqttPort, creds.mqttUsername, creds.mqttPassword);
  deployManager.setPublishStatusHandler(
      [](const String &id, const String &state, const String &error, int percent) {
        protocol.publishDeployStatus(id, state, error, percent);
      });
  deployManager.setClearTriggerHandler([]() { protocol.clearDeployTrigger(); });

  // Subscribed up front rather than from the connected callback: connect()
  // replays its subscription list on every reconnect, so queueing it once
  // covers every later connection too.
  mqttClient.subscribe(protocol.deviceTopic("deploy"));
  deviceRequests.subscribe();

  // Firmware over the air (FirmwareUpdater.h). The screen says what is
  // happening, lit even if it had gone dark, and the project comes back if
  // the update does not happen.
  firmwareUpdater.setPublishStatusHandler(
      [](const String &id, const String &state, const String &error, int percent) {
        protocol.publishDeployStatus(id, state, error, percent);
      });
  firmwareUpdater.setClearTriggerHandler([]() { protocol.clearFirmwareTrigger(); });
  firmwareUpdater.setStartHandler([]() {
    wakeDisplay();
    if (displayAdapter) displayAdapter->showLines({"Firmware-Update", "bitte nicht ausschalten"});
  });
  firmwareUpdater.setFailedHandler([]() { renderAndPresent("firmware update failed"); });
  mqttClient.subscribe(protocol.deviceTopic("firmware"));
  // Light or dark for the whole installation - retained, so the broker
  // answers with the current theme on every connect.
  mqttClient.subscribe(String(TOPIC_PREFIX) + "/state/theme");
  mqttClient.setConnectedCallback([]() {
    protocol.publishHello();
    protocol.publishTestInterface(testInterface.isRunning() ? "http://" + WiFi.localIP().toString() : "");
    Serial.println(BOARD_LOG "published hello");
  });
  mqttClient.setCallback([](const String &topic, const String &payload) {
    if (deviceRequests.onMessage(topic)) return;
    // The charts' history (contract 2.13): held for the shown screen, drawn from loop().
    if (chartFeed.onMessage(topic, payload)) return;
    if (topic == protocol.deviceTopic("firmware"))
    {
      firmwareUpdater.onMessage(payload);
      return;
    }
    if (topic == protocol.deviceTopic("deploy"))
    {
      pendingDeployPayload = payload;
      pendingDeploy = true;
      return;
    }
    // A real topic under a local topic's name does not reach the display
    // (designer contract 2.9) - a subscription from an earlier project, say.
    if (projectLoader.isLoaded() && projectLoader.isLocalTopic(topic)) return;
    applyTopicValue(topic, payload);
  });
  mqttClient.connect();
  Serial.printf(BOARD_LOG "mqtt %s:%d as %s\n", creds.mqttHost.c_str(), creds.mqttPort,
                mqttClient.getClientId().c_str());
}

void setup()
{
  // Every malloc of 256 bytes and more into PSRAM, not only those of 4 KB and
  // more (the framework's CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). A large project
  // - its strings, vectors and topic values - left 25-31 KB of internal RAM
  // with 1.6 MB of PSRAM free, and in the van a chart drawn over an energy
  // diagram took the rest: the icon's fopen found no room for its lock and
  // aborted the board (2026-10-30, ?set=coredump). Drivers that need internal
  // or DMA memory ask for it by its caps and are not affected.
  heap_caps_malloc_extmem_enable(256);
  Serial.begin(115200);
  // Writes wait for a reader for 1 ms at most. The USB serial counts as
  // connected once a host has read from it or written to it - hil-on, hil?,
  // a monitor - and from then on every print with nobody reading waited up
  // to 100 ms for room. The knob logs a lot: a few minutes of HIL and its
  // loop spent so long in prints that WiFi and MQTT starved, until a hard
  // reset (2026-10-14). Not 0: HWCDC::write counts its tries down from the
  // timeout, and from 0 that wraps round - a print then waited until
  // someone read, and the PaperS3's HTTP went silent for minutes. After one
  // 1 ms try without progress it marks the host gone, and later prints pass
  // straight through until a host reads again.
  Serial.setTxTimeoutMs(1);
  delay(200);
  Serial.println();
  Serial.println(BOARD_LOG "boot");

  // The board's own order (its BoardProfile.h): the 4.3B's panel runs
  // without the expander, the 4"'s needs power, reset and an SPI
  // configuration from it first.
  if (EXPANDER_BEFORE_PANEL && !startExpander()) return;

  if (!panel.begin(PANEL_W, PANEL_H, PANEL_PINS, PANEL_TIMING, BOUNCE_BUFFER_PX))
  {
    // Nothing to draw on if this fails, so the serial line is the only
    // channel left - which is why this build keeps CDC-on-boot enabled.
    Serial.println(BOARD_LOG "panel.begin() FAILED");
    return;
  }
  panel.fill(0x0000);

  if (!EXPANDER_BEFORE_PANEL) startExpander();
  if (!touch.begin(expander, TP_INT))
    Serial.println(BOARD_LOG "GT911 not found on 0x5D or 0x14");
  else
    Serial.printf(BOARD_LOG "GT911 at 0x%02X\n", touch.address());

  setupRenderStack();
  // After the project is loaded: the idle screen is held as an id and has
  // to be resolved against a project that exists.
  applyDeviceSettings();
  setupNetworking();
}

// The shared I2C bus and the IO expander, then whatever the board's panel
// needs from them (BoardProfile.h's boardPanelInit). False when the board
// cannot go on.
static bool startExpander()
{
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);
  const bool up = expander.begin(Wire);
  if (!up) Serial.printf(BOARD_LOG "%s init FAILED\n", expander.name());
  return boardPanelInit(up);
}

// Which segment of a switch a tap at x selects.
//
// One function rather than two, because the pressed segment and the
// published one have to be the same or the panel lights up one thing and
// commands another.
static int switchSegmentFor(const ScreenObject *hit, int16_t x, int16_t objX)
{
  // One rule, in switchshape::stateIndexForTap - the designer's own
  // switchStateIndexForTap, mirrored. In the switch form two states toggle
  // wherever they are hit, more take the slot under the finger (or advance,
  // beside the track); a group takes the segment. A second copy here would
  // drift, and a finger would hit something other than what it points at.
  const int states = (int)hit->properties.states.size();
  if (states <= 0) return -1;
  const int active = renderer == nullptr ? -1 : renderer->getActiveSwitchStateIndex(*hit);
  // In the object's own coordinates: a Switch inside a panel carries
  // coordinates relative to its tab-control, while the finger is in screen
  // ones. objX is where the hit test found the object, so this is the offset
  // between the two.
  const int localX = x - objX + hit->x;
  return switchshape::stateIndexForTap(*hit, localX, active);
}

// Redraws only the rectangle the feedback lives in, then shows it.
//
// The region render is what makes this worth doing at all: a full screen is
// 113ms before the panel even sees it, and a press that arrives a tenth of a
// second late is not feedback. The copy to the panel is only the rectangle
// too (RgbPanel::blitRegion), whose per-buffer bookkeeping is what lets a
// partial copy into a framebuffer two flips old come out whole. It was the
// whole screen here until 2026-09-27 - 61 ms of every frame of a dragged
// dimmer, which held the handle to about nine frames a second while the lamp
// itself followed the finger in 45 ms.
static void renderRegion(int16_t rx, int16_t ry, int16_t rw, int16_t rh, const char *reason,
                         bool present = true)
{
  if (!renderer || !canvas || rw <= 0 || rh <= 0) return;
  const uint32_t t0 = micros();
  renderer->renderScreenRegion(activeScreen(), rx, ry, rw, rh,
                               pressedButtonId, pendingSwitchId, pendingSwitchIndex,
                               pressedSwitchId, pressedSwitchIndex);
  const uint32_t t1 = micros();
  // A transition composes from the canvas, not from the panel, so the one
  // caller that is about to start one needs the canvas corrected and
  // nothing else - the first composed frame replaces the glass anyway.
  if (present) panel.blitRegion(canvas->getBuffer(), rx, ry, rw, rh);
  const uint32_t t2 = micros();
  Serial.printf("[feedback] %s: region %lums, blit %lums\n", reason,
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000));
  lastFeedback = String(reason) + ": " + String(rw) + "x" + String(rh) +
                 " region " + String((t1 - t0) / 1000.0f, 1) + " ms" +
                 (present ? ", blit " + String((t2 - t1) / 1000.0f, 1) + " ms" : ", no blit");
}

// A drag through time moves every chart on the screen - they share its
// range - so each is drawn again where it stands, and nothing else: the loop's
// own redraw is never reached while the finger is down.
static void renderCharts(const char *reason)
{
  if (!projectLoader.isLoaded()) return;
  const ProjectConfig &project = projectLoader.getProject();
  if (currentScreen < 0 || currentScreen >= (int)project.screens.size()) return;
  for (const ScreenObject &o : project.screens[currentScreen].objects)
    if (o.properties.chart) renderRegion(o.x, o.y, o.width, o.height, reason);
}

// The rectangle whatever is currently showing lives in.
static void renderFeedbackRegion(const char *reason, bool present = true)
{
  if (!feedbackActive) return;
  renderRegion(feedbackX, feedbackY, feedbackW, feedbackH, reason, present);
}

// A finger has landed on something tappable: show it pressed at once,
// before anything is decided about what the gesture will turn out to be.
static void beginPressFeedback(int16_t x, int16_t y)
{
  if (!projectLoader.isLoaded() || renderer == nullptr) return;

  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(activeScreen(), x, y, objX, objY);
  if (hit == nullptr) return;

  if (hit->type == "button") pressedButtonId = hit->id;
  else if (objecttypes::isSwitch(hit->type))
  {
    pressedSwitchId = hit->id;
    pressedSwitchIndex = switchSegmentFor(hit, x, objX);
  }
  else return;

  feedbackX = objX;
  feedbackY = objY;
  feedbackW = hit->width;
  feedbackH = hit->height;
  feedbackActive = true;
  pressStartUs = micros();
  renderFeedbackRegion("press");
  if (tapLatencyOpen)
  {
    tapLatencyOpen = false;
    recordTapLatency(micros() - contactSampleUs);
  }
}

// The finger is gone, whatever it turned out to mean. Anything still shown
// pressed has to stop being pressed, and a switch waiting for its broker
// keeps its pending mark.
static void clearPressFeedback()
{
  if (pressedButtonId.isEmpty() && pressedSwitchId.isEmpty()) return;
  pressedButtonId = "";
  pressedSwitchId = "";
  pressedSwitchIndex = -1;
  renderFeedbackRegion("release");
  if (pendingSwitchId.isEmpty()) feedbackActive = false;
}

// A tap has gone out and nothing has confirmed it yet.
static void markPending(const String &id, int index, int16_t rx, int16_t ry, int16_t rw, int16_t rh)
{
  // A mark on some other tile is now stale, and it is the only thing that
  // knows where it is. Wiped before the new one takes the state over.
  if (!pendingSwitchId.isEmpty() && pendingSwitchId != id)
  {
    const int16_t oldX = pendingX, oldY = pendingY, oldW = pendingW, oldH = pendingH;
    pendingSwitchId = "";
    pendingSwitchIndex = -1;
    renderRegion(oldX, oldY, oldW, oldH, "superseded");
  }

  pendingSwitchId = id;
  pendingSwitchIndex = index;
  pendingSinceMs = millis();
  pendingX = rx;
  pendingY = ry;
  pendingW = rw;
  pendingH = rh;
}

// Whatever came back, or ran out of patience.
static void clearPending(const char *reason)
{
  if (pendingSwitchId.isEmpty()) return;
  const int16_t x = pendingX, y = pendingY, w = pendingW, h = pendingH;
  pendingSwitchId = "";
  pendingSwitchIndex = -1;
  renderRegion(x, y, w, h, reason);
  feedbackActive = false;
}

// Everything the touch feedback believes, dropped without repainting.
// For a screen change, where the rectangles it remembers belong to a screen
// that is no longer showing and repainting them would put a mark on the
// wrong picture.
static void forgetTouchFeedback()
{
  pressedButtonId = "";
  pressedSwitchId = "";
  pressedSwitchIndex = -1;
  pendingSwitchId = "";
  pendingSwitchIndex = -1;
  feedbackActive = false;
  // Every guarded control locks with the screen it was on (E2).
  guards.clear();
}

static String stripJsonPath(const String &topicOrPath)
{
  const int hashIndex = topicOrPath.indexOf('#');
  return hashIndex == -1 ? topicOrPath : topicOrPath.substring(0, hashIndex);
}

// Redraws only what a new value actually changes.
//
// Ported from the knob, which learned the shape of this the hard way: its
// first version demanded exactly one object per topic and fell back to a
// full screen otherwise, and the shortcut vanished silently the day someone
// bound a second object to the same brightness. So: any number of objects,
// each with its own rectangle.
//
// What this board adds is the second half. The knob hands its regions to
// LVGL, which flushes only what changed; here the canvas still has to reach
// the panel, and a full blit is 69ms against a 113ms render - so redrawing
// less would have saved a third of nothing without RgbPanel::blitRegion().
//
// Returns false when the change cannot be bounded safely; the caller then
// draws a full screen, which is always correct and merely slow.
static void checkPartialAgainstFull();
static bool renderValueChangesPartial()
{
  if (renderer == nullptr || canvas == nullptr || !projectLoader.isLoaded()) return false;
  if (pendingTopicOverflow || pendingTopicCount == 0) return false;

  // While a popup is open only its own objects are redrawn; what changes on
  // the screen underneath is drawn when the popup closes, with the rest.
  const Screen *active = screenOrPopupAt(projectLoader.getProject(), activeScreen());
  if (active == nullptr) return false;
  const Screen &screen = *active;

  const int MAX_RECTS = 8;
  int16_t rx[MAX_RECTS], ry[MAX_RECTS], rw[MAX_RECTS], rh[MAX_RECTS];
  int rects = 0;

  const auto &combinedTopics = projectLoader.getProject().combinedTopics;
  // Whether an object reads one of the changed topics in any way this board
  // knows of - what it is bound to, its setpoint marker, what it writes back
  // and hears answered, a placeholder, a live value, an energy node. One that
  // does and gets no rectangle below leaves the whole screen to be drawn.
  auto mentions = [&](const ScreenObject &obj) {
    for (int i = 0; i < pendingTopicCount; i++)
    {
      const String &t = pendingTopics[i].topic;
      if (stripJsonPath(obj.properties.topic) == t || stripJsonPath(obj.properties.setpointTopic) == t ||
          stripJsonPath(obj.properties.writeTopic) == t)
        return true;
      if (ColorScreenRenderer::readsTopic(obj, t, combinedTopics)) return true;
      for (const std::string &read : ColorScreenRenderer::liveTopics(obj, combinedTopics))
        if (t == read.c_str()) return true;
    }
    return false;
  };

  // A switcher's objects lie in its visible panel, their coordinates
  // relative to the switcher (as hitTestWalk reads them); the other panels
  // are not on the glass. Until 2026-10-30 any object with children sent
  // every value to a full screen: on the van's energy screen, a switcher of
  // two diagrams, that was 200-250 ms with the touch unread about every two
  // seconds, and a swipe begun then started late or not at all.
  std::function<bool(const std::vector<ScreenObject> &, int16_t, int16_t)> visit;
  visit = [&](const std::vector<ScreenObject> &objects, int16_t dx, int16_t dy) -> bool {
  for (const auto &source : objects)
  {
    ScreenObject obj = source;
    obj.x += dx;
    obj.y += dy;
    const int before = rects;
    const bool reads = mentions(obj);
    if (obj.type == "switcher")
    {
      // Its own topic picks the panel: the whole switcher anew.
      if (reads)
      {
        if (rects + 1 > MAX_RECTS) return false;
        rx[rects] = obj.x;
        ry[rects] = obj.y;
        rw[rects] = obj.width;
        rh[rects] = obj.height;
        rects++;
        continue;
      }
      const ScreenObject *panelShown = renderer->visiblePanel(source);
      if (panelShown != nullptr && !visit(panelShown->children, obj.x, obj.y)) return false;
      continue;
    }
    if (!obj.children.empty()) return false;

    // What this object needs drawn: 1 bounded (a rectangle or none), 0 not.
    auto bound = [&]() -> int {
      // A text whose placeholder names a changed topic (designer
      // docs/2026-10-05-placeholder-devices.md). It has no binding of its own,
      // so the loop below would pass it by - and when another object on the
      // screen is bound to the same topic, only that one was redrawn and the
      // text kept the old value. Its whole box, as for any other text; no old
      // value is needed for that.
      // A live icon the same way, and a text through its live values - also
      // through a combined topic the changed topic feeds (contract 2.6).
      // An energy diagram the same way, as one box: a value moves its bands,
      // lines and labels (contract 2.8).
      if (obj.type == "text" || obj.type == "energy-diagram" || !obj.properties.liveIconId.isEmpty())
      {
        bool named = false;
        for (int i = 0; i < pendingTopicCount && !named; i++)
        {
          named = ColorScreenRenderer::readsTopic(obj, pendingTopics[i].topic, projectLoader.getProject().combinedTopics);
        }
        if (!named) return 1;
        if (rects + 1 > MAX_RECTS) return 0;
        // Accents above a text's box belong to it (textOverhangLimit).
        const int16_t over = obj.type == "text" ? std::min<int16_t>(renderer->textOverhangLimit(obj), obj.y) : 0;
        rx[rects] = obj.x;
        ry[rects] = obj.y - over;
        rw[rects] = obj.width;
        rh[rects] = obj.height + over;
        rects++;
        return 1;
      }

      if (obj.properties.topic.isEmpty()) return 1;

      const String objTopic = stripJsonPath(obj.properties.topic);
      const String *oldValue = nullptr;
      for (int i = 0; i < pendingTopicCount; i++)
      {
        if (pendingTopics[i].topic == objTopic) { oldValue = &pendingTopics[i].oldValue; break; }
      }
      if (oldValue == nullptr) return 1;
      if (oldValue->isEmpty()) return 0;  // nothing to compare a sweep against

      if (objecttypes::isArc(obj.type))
      {
        const String newValue = projectLoader.getTopicValue(obj.properties.topic);
        if (newValue.isEmpty() || newValue == *oldValue) return 0;
        if (rects + 2 > MAX_RECTS) return 0;
        // The swept arc and the number in the middle, separately: their
        // common hull would be nearly the whole object, because one is at
        // the rim and the other at the centre.
        if (!renderer->arcLevelDirtyRect(obj, *oldValue, newValue, &rx[rects], &ry[rects], &rw[rects],
                                         &rh[rects]))
        {
          return 0;
        }
        rects++;
        if (renderer->arcLevelTextRect(obj, *oldValue, newValue, &rx[rects], &ry[rects], &rw[rects],
                                       &rh[rects]))
        {
          rects++;
        }
      }
      else
      {
        if (rects + 1 > MAX_RECTS) return 0;
        rx[rects] = obj.x;
        ry[rects] = obj.y;
        rw[rects] = obj.width;
        rh[rects] = obj.height;
        rects++;
      }
      return 1;
    };
    if (!bound()) return false;
    if (reads && rects == before) return false;
  }
  return true;
  };
  if (!visit(screen.objects, 0, 0)) return false;
  // A navigator entry that reads a changed topic - a live screen icon -
  // redrawn as its own rectangle, cut to the strip (contract 2.7).
  {
    navigatorlayout::Layout layout;
    const Navigator *nav = nullptr;
    int active = -1;
    if (openPopup < 0 && renderer->navigatorLayout(currentScreen, layout, &nav, &active))
    {
      const int scroll = renderer->effectiveNavigatorScroll(currentScreen);
      const auto &combinedTopics = projectLoader.getProject().combinedTopics;
      for (int e = 0; e < (int)nav->entries.size(); e++)
      {
        const SpiramObjects &objects = e == active ? nav->entries[e].active : nav->entries[e].normal;
        bool reads = false;
        for (const ScreenObject &obj : objects)
          for (int i = 0; i < pendingTopicCount && !reads; i++)
            reads = ColorScreenRenderer::readsTopic(obj, pendingTopics[i].topic, combinedTopics);
        if (!reads) continue;
        const navigatorlayout::Rect r = navigatorlayout::entryRect(layout, e, scroll);
        const int x0 = max(r.x, layout.strip.x), y0 = max(r.y, layout.strip.y);
        const int x1 = min(r.x + r.width, layout.strip.x + layout.strip.width);
        const int y1 = min(r.y + r.height, layout.strip.y + layout.strip.height);
        if (x1 <= x0 || y1 <= y0) continue;
        if (rects + 1 > MAX_RECTS) return false;
        rx[rects] = x0;
        ry[rects] = y0;
        rw[rects] = x1 - x0;
        rh[rects] = y1 - y0;
        rects++;
      }
    }
  }

  // Nothing shown reads what changed - a value for another screen: nothing to
  // draw. It used to be a full screen, every time.
  if (rects == 0)
  {
    lastValueDraw = "nothing on this screen reads it";
    checkPartialAgainstFull();
    return true;
  }

  const uint32_t t0 = micros();
  for (int i = 0; i < rects; i++)
  {
    if (!renderer->renderScreenRegion(activeScreen(), rx[i], ry[i], rw[i], rh[i], pressedButtonId,
                                      pendingSwitchId, pendingSwitchIndex, pressedSwitchId,
                                      pressedSwitchIndex))
    {
      // Half the regions drawn and the rest not: the canvas is now a mix
      // and only a full screen can be trusted to sort it out.
      return false;
    }
  }
  const uint32_t t1 = micros();
  for (int i = 0; i < rects; i++) panel.blitRegion(canvas->getBuffer(), rx[i], ry[i], rw[i], rh[i]);
  const uint32_t t2 = micros();

  lastValueDraw = String(rects) + " region(s), render " + String((t1 - t0) / 1000.0f, 1) + " ms, blit " +
                  String((t2 - t1) / 1000.0f, 1) + " ms, " + String(panel.lastPresentPixels()) +
                  " px copied";
  checkPartialAgainstFull();
  return true;
}

static void checkPartialAgainstFull()
{
  if (!partialCheck || openPopup >= 0 || renderer == nullptr || canvas == nullptr || spareCanvas == nullptr ||
      spareCanvas->getBuffer() == nullptr)
    return;
  renderer->setCanvas(spareCanvas);
  renderer->renderScreen(activeScreen(), pressedButtonId, pendingSwitchId, pendingSwitchIndex, pressedSwitchId,
                         pressedSwitchIndex);
  renderer->setCanvas(canvas);
  const uint16_t *a = canvas->getBuffer(), *b = spareCanvas->getBuffer();
  uint32_t n = 0;
  int x0 = PANEL_W, y0 = PANEL_H, x1 = -1, y1 = -1;
  for (int y = 0; y < PANEL_H; y++)
    for (int x = 0; x < PANEL_W; x++)
      if (a[y * PANEL_W + x] != b[y * PANEL_W + x])
      {
        n++;
        x0 = min(x0, x);
        y0 = min(y0, y);
        x1 = max(x1, x);
        y1 = max(y1, y);
      }
  partialChecks++;
  if (n) partialCheckFailures++;
  lastPartialCheck = n == 0 ? String("0 px differ from a full draw")
                            : String(n) + " px differ from a full draw, in (" + String(x0) + "," + String(y0) + ")-(" +
                                  String(x1) + "," + String(y1) + ")";
}

// ------------------------------------------------------------------ popups
//
// A popup over the current screen (designer docs/device-contract.md 2.5). On
// this board: the whole screen set back to half, then a window growing as a
// rectangle from the button that opened it to the fence - rectangles only, no
// pixels scaled, a few frames - and the popup's content at the end. The scrim
// reaches up to the window at every size: dimming only outside the final
// fence first left a bright band between the two while it grew (the user,
// 2026-10-06). Closing draws the screen underneath again, whole.
// Four sizes between the button and the window: six were tried on the board
// and looked worse (the user, 2026-10-06).
static const int POPUP_GROW_FRAMES = 4;

static void openPopupAt(int index)
{
  if (!renderer || !canvas) return;
  const ProjectConfig &project = projectLoader.getProject();
  const Screen *popup = screenOrPopupAt(project, index);
  if (popup == nullptr) return;
  const bool replacing = openPopup >= 0;
  openPopup = index;
  forgetTouchFeedback();
  const uint32_t t0 = micros();

  // A popup replacing another, or one opened by something with no place on
  // the glass (a hardware button, the test interface), appears at once.
  if (!replacing && popupOriginKnown)
  {
    renderer->renderScreen(currentScreen);
    renderer->dimCanvas();
    // The dimmed screen kept, to put back under the finished popup: the
    // growing rectangles cover what lies outside a round fence's circle.
    const size_t bytes = (size_t)PANEL_W * PANEL_H * sizeof(uint16_t);
    const bool kept = spareCanvas != nullptr && spareCanvas->getBuffer() != nullptr;
    if (kept) memcpy(spareCanvas->getBuffer(), canvas->getBuffer(), bytes);
    panel.blit(canvas->getBuffer());
    const PopupFence &f = project.popupFence;
    bool transparent = false;
    const uint16_t ground =
        renderer->rgb565Of(popup->backgroundColor.isEmpty() ? "#ffffff" : popup->backgroundColor, &transparent);
    const uint16_t edge =
        popup->borderColor.isEmpty() ? ground : renderer->rgb565Of(popup->borderColor, &transparent);
    for (int k = 1; k <= POPUP_GROW_FRAMES; k++)
    {
      const int16_t x = popupOriginX + (f.x - popupOriginX) * k / (POPUP_GROW_FRAMES + 1);
      const int16_t y = popupOriginY + (f.y - popupOriginY) * k / (POPUP_GROW_FRAMES + 1);
      const int16_t r = popupOriginX + popupOriginW +
                        (f.x + f.width - popupOriginX - popupOriginW) * k / (POPUP_GROW_FRAMES + 1);
      const int16_t b = popupOriginY + popupOriginH +
                        (f.y + f.height - popupOriginY - popupOriginH) * k / (POPUP_GROW_FRAMES + 1);
      canvas->fillRect(x, y, r - x, b - y, ground);
      for (int i = 0; i < POPUP_BORDER_PX; i++) canvas->drawRect(x + i, y + i, r - x - 2 * i, b - y - 2 * i, edge);
      panel.blitRegion(canvas->getBuffer(), x, y, r - x, b - y);
    }
    if (kept)
    {
      memcpy(canvas->getBuffer(), spareCanvas->getBuffer(), bytes);
    }
    else
    {
      renderer->renderScreen(currentScreen);
      renderer->dimOutsidePopupFence();
    }
    renderer->renderPopup(openPopup, POPUP_BORDER_PX);
    renderGeneration++;
    pendingTopicCount = 0;
    pendingTopicOverflow = false;
    panel.blit(canvas->getBuffer());
  }
  else
  {
    renderAndPresent("popup opened");
  }
  lastPopup = "opened \"" + popup->name + "\" in " + String((micros() - t0) / 1000) + " ms";
  Serial.printf("[popup] %s\n", lastPopup.c_str());
}

static void closePopup(const char *reason)
{
  if (openPopup < 0) return;
  openPopup = -1;
  forgetTouchFeedback();
  renderAndPresent("popup closed");
  lastPopup = String("closed by ") + reason;
  Serial.printf("[popup] %s\n", lastPopup.c_str());
}

// What a project binds to a button, and what this board does about it.
// Ported from the knob rather than reinvented: same action names, same
// order, same treatment of an unknown one.
static void dispatchButtonAction(const ButtonAction &action)
{
  if (action.type.isEmpty() || !projectLoader.isLoaded()) return;
  const ProjectConfig &project = projectLoader.getProject();
  const int count = (int)project.screens.size();
  // The screen's time range (contract 2.13): drawn again from loop().
  if (chartFeed.applyAction(action, project, currentScreen)) return;

  if (action.type == "next-screen" || action.type == "previous-screen")
  {
    if (count == 0) return;
    const int delta = (action.type == "next-screen") ? 1 : -1;
    // Over the main row (contract 2.10): from a screen off it, from its page.
    currentScreen = screengraphproject::rowStepIndex(project, currentScreen, delta);
    openPopup = -1;
    forgetTouchFeedback();
    renderAndPresent("button: screen change");
  }
  else if (action.type == "goto-neighbour")
  {
    // The screen lying on that side (2.10); none there: nothing.
    screengraph::Edge side;
    if (!screengraph::edgeFrom(action.side.c_str(), side)) return;
    const int target = screengraphproject::neighbourIndex(project, currentScreen, side);
    if (target < 0) return;
    currentScreen = target;
    openPopup = -1;
    forgetTouchFeedback();
    renderAndPresent("button: neighbour");
  }
  else if (action.type == "goto-screen")
  {
    for (int i = 0; i < count; i++)
    {
      if (project.screens[i].id == action.targetScreenId)
      {
        currentScreen = i;
        openPopup = -1;
        forgetTouchFeedback();
        renderAndPresent("button: goto screen");
        break;
      }
    }
  }
  else if (action.type == "send-mqtt")
  {
    // Not retained: this is a command, not a state. What comes back on the
    // read topic is what the panel then shows.
    // «Only when» not yes: nothing goes out. «Then clear»: a local topic only.
    const auto message = projectLoader.pressedMessage(action);
    if (message)
    {
      writeProjectTopic(action.mqttTopic, *message);
      if (!action.clears.isEmpty() && projectLoader.isLocalTopic(action.clears)) writeProjectTopic(action.clears, "");
    }
    Serial.printf("[tap] send-mqtt %s = %s%s\n", action.mqttTopic.c_str(), message ? message->c_str() : "", message ? "" : " (held: Only when is not yes)");
  }
  else if (action.type == "device-action")
  {
    // Actions the DEVICE offers and the designer only names - declared in
    // this board's DDF as deviceActions. An id this firmware does not know
    // is logged and skipped, never an error: a project may legitimately
    // have been built against a device offering more of them, and additive
    // changes degrade rather than break.
    if (action.deviceActionId == "displayOff")
    {
      blankDisplay("button");
    }
    else
    {
      Serial.printf("[tap] unknown device action \"%s\" - skipping\n",
                    action.deviceActionId.c_str());
    }
  }
  else if (action.type == "goto-setup-mode")
  {
    enterSetupMode();
  }
  else if (action.type == "open-popup")
  {
    const int index = popupIndexById(project, action.targetScreenId);
    if (index < 0 || !project.popupFence.present)
    {
      Serial.printf("[tap] no popup \"%s\" - skipping\n", action.targetScreenId.c_str());
      return;
    }
    openPopupAt(index);
  }
  else if (action.type == "close-popup")
  {
    closePopup("button");
  }
}

// What a tap at this point hits, and what that does.
//
// Until now this board could render a SoftwareButton and a Switch, and show
// the Switch's state correctly because that arrives over MQTT - but a finger
// on either did nothing at all. It rendered a control panel without being
// one.
//
// The search runs through ColorScreenRenderer::hitTestTappable(), not a loop
// over screen.objects here, because the answer depends on two things that
// already live in the renderer: which panel of a tab-control is showing, and
// how far its children are drawn shifted. A second copy would mean the finger
// and the picture disagreeing about geometry. The knob learned that the hard
// way - its top-level switch worked and the four inside panels did not.
// Which screen a horizontal swipe should arrive at, or -1 when this one is
// not a page turn on this screen.
//
// Until 2026-09-10 this board paged on every horizontal drag and never
// asked the project. That happened to match what the vehicle project binds,
// which is exactly why it could look right while being wrong: a project
// that binds a leftward swipe to something else - a command, a jump to a
// named screen - would have had its screens turned instead.
//
// Since the screen graph (designer device contract 2.10) the slide follows
// exactly where a swipe leads to the neighbour it pulls in - where the pull
// tab stands - up and down as well as sideways; `d` is the finger's travel
// along the axis. Everything else, a wrap from the row's last page among
// it, is the release's.
static int pagingTargetFor(int d, bool vertical)
{
  if (!projectLoader.isLoaded()) return -1;
  const ProjectConfig &project = projectLoader.getProject();
  if ((int)project.screens.size() < 2 || openPopup >= 0) return -1;  // a swipe on a popup closes it, at release

  const char *swipeId = vertical ? (d < 0 ? "swipe-up" : "swipe-down") : (d < 0 ? "swipe-left" : "swipe-right");
  if (!screengraphproject::swipeLeadsToNeighbour(project, currentScreen, swipeId)) return -1;
  screengraph::Edge side;
  screengraph::sideOfSwipe(swipeId, side);
  const int target = screengraphproject::neighbourIndex(project, currentScreen, side);
  return target == currentScreen ? -1 : target;
}

// A gesture that was not a page turn, resolved at release the way the knob
// resolves all of its swipes: the dominant axis names a button, and the
// project says what that button does. Without this, a swipe bound to
// send-mqtt on this board did nothing at all.
static const int SWIPE_MIN_DISTANCE = 60;  // px, well past a jittery tap
static const int SWIPE_AXIS_RATIO = 2;

static void dispatchSwipeAtRelease(int dx, int dy)
{
  if (!projectLoader.isLoaded()) return;

  const char *buttonId = nullptr;
  if (abs(dx) > abs(dy))
  {
    if (abs(dx) >= SWIPE_MIN_DISTANCE && abs(dx) >= abs(dy) * SWIPE_AXIS_RATIO)
      buttonId = dx > 0 ? "swipe-right" : "swipe-left";
  }
  else
  {
    if (abs(dy) >= SWIPE_MIN_DISTANCE && abs(dy) >= abs(dx) * SWIPE_AXIS_RATIO)
      buttonId = dy > 0 ? "swipe-down" : "swipe-up";
  }
  if (buttonId == nullptr) return;

  // Any swipe closes an open popup and does nothing else (designer
  // docs/device-contract.md 2.5); one a level took never reaches here.
  if (openPopup >= 0)
  {
    lastTap = String(buttonId) + " -> close popup";
    closePopup("swipe");
    return;
  }

  const ButtonAction action = projectLoader.getButtonAction(currentScreen, buttonId);
  if (action.type.isEmpty()) return;
  Serial.printf("[swipe] %s -> %s\n", buttonId, action.type.c_str());
  lastTap = String(buttonId) + " -> " + action.type;
  dispatchButtonAction(action);
}

// ------------------------------------------- a level a finger can set
//
// A level indicator with a write topic is operable (designer
// docs/2026-09-17-settable-level.md): the press sets the value under the
// finger, the drag follows it, the release publishes what it settled on.
//
// The finger moves the MARKER, not the fill (decision 6c): the fill keeps
// showing what the installation reports, the marker what was asked for, and
// the two coincide once the command has landed. The asked-for value lives in
// the renderer (setAskedValue) and is dropped as soon as the installation
// speaks about that same setpoint.
//
// Until 2026-09-17 the finger set the reading itself, which looked quicker
// and was a claim: it showed a wish as a measurement.
//
// The rectangle is redrawn rather than the screen: this panel is 800x480, and
// a full render per touch sample would make the bar crawl behind the finger.
static String formatLevelPayload(float value)
{
  if (fabsf(value - roundf(value)) < 0.001f) return String((long)roundf(value));
  String out = String(value, 2);
  while (out.endsWith("0")) out.remove(out.length() - 1);
  if (out.endsWith(".")) out.remove(out.length() - 1);
  return out;
}

// What changes when the asked value goes from `before` to `after`, drawn and
// shown: every object bound to it, each on its own - a ring only along the
// stretch of band between the two values and in its middle. One box round
// all of them was the whole screen for a slider and a dial on one dimmer.
static void renderLevelDragChange(const String &before, const String &after)
{
  const int MAX_RECTS = 8;
  int16_t rx[MAX_RECTS], ry[MAX_RECTS], rw[MAX_RECTS], rh[MAX_RECTS];
  const int rects =
      renderer->levelDragRects(activeScreen(), levelAskedTopic, before, after, rx, ry, rw, rh, MAX_RECTS);
  if (rects <= 0)
  {
    renderRegion(levelDragX, levelDragY, levelDragW, levelDragH, "level drag");
    return;
  }
  const uint32_t t0 = micros();
  for (int i = 0; i < rects; i++)
  {
    renderer->renderScreenRegion(activeScreen(), rx[i], ry[i], rw[i], rh[i], pressedButtonId, pendingSwitchId,
                                 pendingSwitchIndex, pressedSwitchId, pressedSwitchIndex);
  }
  const uint32_t t1 = micros();
  long pixels = 0;
  for (int i = 0; i < rects; i++)
  {
    panel.blitRegion(canvas->getBuffer(), rx[i], ry[i], rw[i], rh[i]);
    pixels += (long)rw[i] * rh[i];
  }
  const uint32_t t2 = micros();
  lastFeedback = "level drag: " + String(rects) + " region(s), " + String(pixels) + " px, render " +
                 String((t1 - t0) / 1000.0f, 1) + " ms, blit " + String((t2 - t1) / 1000.0f, 1) + " ms";
  Serial.printf("[feedback] level drag: %d region(s), render %lums, blit %lums\n", rects,
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000));
}

static void applyLevelDragValue(const String &payload, bool force)
{
  if (payload.isEmpty()) return;
  const bool changed = payload != levelDragPayload;
  if (changed)
  {
    // What is on the glass now: the last value this drag asked for, or on
    // the first touch whatever was showing before it.
    String before = levelDragPayload;
    if (before.isEmpty() && renderer != nullptr)
    {
      before = renderer->askedValueFor(levelAskedTopic);
      if (before.isEmpty()) before = projectLoader.getTopicValue(levelAskedTopic);
    }
    levelDragPayload = payload;
    if (renderer != nullptr)
    {
      // Keyed by the topic the request is about (levelAskedTopic), not by the
      // object that took it.
      renderer->setAskedValue(levelAskedTopic, payload);
      renderLevelDragChange(before, payload);
    }
  }
  const unsigned long now = millis();
  if (!force && (now - levelDragLastPublishMs < LEVEL_PUBLISH_MIN_MS || !changed)) return;
  levelDragLastPublishMs = now;
  // Not retained: a command, not a state - exactly as a Switch publishes.
  String sent;
  if (levelWritePayload(levelDragWriteFormat, payload, sent)) writeProjectTopic(levelDragWriteTopic, sent);
}

static bool beginLevelDrag(int16_t x, int16_t y)
{
  levelDragObjId = "";
  levelDragTopic = "";
  levelDragWriteTopic = "";
  levelDragPayload = "";
  // A new finger: whatever the last release waited for is not the question now.
  levelAwaitValue = "";
  if (!projectLoader.isLoaded() || renderer == nullptr) return false;

  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(activeScreen(), x, y, objX, objY);
  if (hit == nullptr || !ColorScreenRenderer::isSettableLevel(*hit)) return false;

  levelDragObjId = hit->id;
  levelDragTopic = hit->properties.topic;
  levelDragWriteTopic = hit->properties.writeTopic;
  levelDragWriteFormat = hit->properties.writeFormat;
  levelAskedTopic = hit->properties.setpointTopic.isEmpty() ? hit->properties.topic
                                                            : hit->properties.setpointTopic;
  levelDragX = objX;
  levelDragY = objY;
  levelDragW = hit->width;
  levelDragH = hit->height;
  // Every object showing this same value, as one rectangle: the request is
  // about the value, so they all change, and redrawing only the one under the
  // finger left the others showing the old marker.
  int16_t boundsX = 0, boundsY = 0, boundsW = 0, boundsH = 0;
  if (renderer->markerTopicBounds(activeScreen(), levelAskedTopic, boundsX, boundsY, boundsW, boundsH))
  {
    levelDragX = boundsX;
    levelDragY = boundsY;
    levelDragW = boundsW;
    levelDragH = boundsH;
  }
  levelDragLastPublishMs = 0;
  Serial.printf("[level] \"%s\" under the finger -> %s\n", hit->id.c_str(), levelDragWriteTopic.c_str());
  applyLevelDragValue(formatLevelPayload(renderer->settableLevelValueAt(*hit, x, y, objX, objY)), true);
  return true;
}

static void updateLevelDrag(int16_t x, int16_t y)
{
  if (levelDragObjId.isEmpty() || renderer == nullptr) return;
  int16_t objX = 0, objY = 0;
  // The finger may leave the object - it then sets the end value rather than
  // handing the gesture back, so the value is computed on the object it went
  // down on.
  const ScreenObject *held = renderer->findObjectById(activeScreen(), levelDragObjId, objX, objY);
  if (held == nullptr) return;
  applyLevelDragValue(formatLevelPayload(renderer->settableLevelValueAt(*held, x, y, objX, objY)), false);
}

static void endLevelDrag()
{
  if (levelDragObjId.isEmpty()) return;
  if (!levelDragPayload.isEmpty() && !levelDragWriteTopic.isEmpty())
  {
    String sent;
    if (levelWritePayload(levelDragWriteFormat, levelDragPayload, sent)) writeProjectTopic(levelDragWriteTopic, sent);
    // What the answer has to say before the fill moves (see the MQTT
    // callback). Long enough for a van's bridge and its Pekaway to answer,
    // short enough that a value it would not take is shown soon.
    levelAwaitValue = levelDragPayload;
    levelAwaitUntilMs = millis() + LEVEL_AWAIT_MS;
    // Already answered while the finger rested: a bridge publishes only
    // changes, so no second answer comes - end the request now rather than
    // after the wait.
    const String reported = projectLoader.getTopicValue(levelAskedTopic);
    if (!levelAskedTopic.isEmpty() && !reported.isEmpty() && renderer != nullptr &&
        fabsf(reported.toFloat() - levelAwaitValue.toFloat()) <= 0.001f)
    {
      levelAwaitValue = "";
      renderer->clearAskedValue();
      levelAskedTopic = "";
      pendingTopicOverflow = true;
      renderPending = true;
    }
    Serial.printf("[level] released on %s = %s\n", levelDragWriteTopic.c_str(), levelDragPayload.c_str());
    lastTap = "level " + levelDragObjId + " -> " + levelDragPayload;
  }
  levelDragObjId = "";
}

// The keyboard alone, drawn and shown: a key's press redraws only it.
static void presentKeyboard()
{
  const uint32_t t0 = micros();
  renderer->renderKeyboard(keyboardState, keyboardField, keyboardFrame);
  panel.blitRegion(canvas->getBuffer(), keyboardFrame.x, keyboardFrame.y, keyboardFrame.w, keyboardFrame.h);
  lastFeedback = "keyboard: " + String((micros() - t0) / 1000.0f, 1) + " ms";
}

static void openKeyboard(const ScreenObject &field)
{
  keyboard::Layout layout = keyboard::Layout::EnUS;
  keyboard::parseLayout(DeviceSettings::keyboardLayoutOrDefault().c_str(), layout);
  keyboardField = field;
  keyboardState = keyboard::State();
  keyboardState.layout = layout;
  if (field.properties.keyboard == "digits") keyboardState.layer = keyboard::Layer::Symbols;
  keyboardState.bounds = textfield::bounds(field.properties.minLength, field.properties.maxLength);
  // Typing goes on from what the field shows.
  String shown = renderer->askedValueFor(field.properties.topic);
  if (shown.isEmpty() && projectLoader.hasTopicValue(field.properties.topic)) shown = projectLoader.getTopicValue(field.properties.topic);
  keyboardState.text = textfield::clampText(shown.c_str(), keyboardState.bounds.maxLength);
  keyboardFrame = keyboard::frameFor(canvas->width(), canvas->height(), false);
  keyboardOpen = true;
  keyboardContact = false;
  presentKeyboard();
}

// OK sends the text as it is, not retained - a command, as a switch's - and
// the field shows it until its topic answers, as a slider shows its
// handle (the same asked value, levelAskedTopic).
static void closeKeyboard(bool send)
{
  keyboardOpen = false;
  keyboardContact = false;
  if (send)
  {
    const String text(keyboardState.text.c_str());
    const bool sent = writeProjectTopic(keyboardField.properties.writeTopic, text);
    levelAskedTopic = keyboardField.properties.topic;
    renderer->setAskedValue(levelAskedTopic, text);
    lastTap = "text field \"" + keyboardField.id + "\" -> " + keyboardField.properties.writeTopic + " = " + text +
              (sent ? "" : "  (NOT published - broker away)");
    Serial.printf("[tap] text field \"%s\" -> %s = %s\n", keyboardField.id.c_str(),
                  keyboardField.properties.writeTopic.c_str(), text.c_str());
  }
  else
  {
    lastTap = "text field \"" + keyboardField.id + "\" cancelled";
  }
  renderAndPresent(send ? "keyboard sent" : "keyboard cancelled");
}

// A finger landing while the keyboard is open: the key under it, at once.
static void keyboardTouch(int16_t x, int16_t y)
{
  const std::vector<keyboard::KeyRect> keys = keyboard::layoutKeys(
      keyboardState, keyboardFrame.keysX, keyboardFrame.keysY, keyboardFrame.keysW, keyboardFrame.keysH, keyboardFrame.gap);
  const keyboard::Key *key = keyboard::keyAt(keys, x, y);
  if (key == nullptr)
  {
    lastTap = "(" + String(x) + "," + String(y) + ") keyboard, no key";
    return;
  }
  const keyboard::Outcome outcome = keyboard::press(keyboardState, *key);
  lastTap = "(" + String(x) + "," + String(y) + ") keyboard key \"" + String(keyboard::label(*key).c_str()) + "\"";
  if (outcome == keyboard::Outcome::Send) closeKeyboard(true);
  else if (outcome == keyboard::Outcome::Cancel) closeKeyboard(false);
  else presentKeyboard();
}

static void presentPicker()
{
  renderer->renderTimePicker(pickerState, pickerField, pickerFrame);
  const timepicker::Rect &p = pickerFrame.panel;
  panel.blitRegion(canvas->getBuffer(), p.x, p.y, p.w, p.h);
}

static void openPicker(const ScreenObject &field)
{
  pickerField = field;
  String shown = renderer->askedValueFor(field.properties.topic);
  if (shown.isEmpty() && projectLoader.hasTopicValue(field.properties.topic)) shown = projectLoader.getTopicValue(field.properties.topic);
  timefield::TimeOfDay t;
  const bool known = timefield::parse(shown.c_str(), t);
  pickerState = timepicker::open(known ? &t : nullptr, field.properties.minuteStep);
  pickerFrame = timepicker::frameFor(canvas->width(), canvas->height(), false, timepicker::Style::Wheels);
  pickerOpen = true;
  pickerContact = false;
  pickerWheel = nullptr;
  presentPicker();
}

// OK sends the time on the step as HH:MM:00, not retained, and the field
// shows it until its topic answers (levelAskedTopic, as the keyboard).
static void closePicker(bool send)
{
  pickerOpen = false;
  pickerContact = false;
  pickerWheel = nullptr;
  if (send)
  {
    const String payload(timefield::payload(timepicker::value(pickerState)).c_str());
    const bool sent = writeProjectTopic(pickerField.properties.writeTopic, payload);
    levelAskedTopic = pickerField.properties.topic;
    renderer->setAskedValue(levelAskedTopic, payload);
    lastTap = "time field \"" + pickerField.id + "\" -> " + pickerField.properties.writeTopic + " = " + payload +
              (sent ? "" : "  (NOT published - broker away)");
    Serial.printf("[tap] time field \"%s\" -> %s = %s\n", pickerField.id.c_str(),
                  pickerField.properties.writeTopic.c_str(), payload.c_str());
  }
  else
  {
    lastTap = "time field \"" + pickerField.id + "\" cancelled";
  }
  renderAndPresent(send ? "picker sent" : "picker cancelled");
}

// A finger on the open picker: a wheel follows it; OK and Cancel act on
// the press, as the keyboard's keys do.
static void pickerTouch(int16_t x, int16_t y, bool down, bool fresh)
{
  const uint32_t now = millis();
  if (down && fresh)
  {
    const timepicker::Part part = timepicker::partAt(pickerFrame, x, y);
    if (part == timepicker::Part::Ok) return closePicker(true);
    if (part == timepicker::Part::Cancel) return closePicker(false);
    pickerWheel = part == timepicker::Part::Hour ? &pickerState.hour : part == timepicker::Part::Minute ? &pickerState.minute : nullptr;
    pickerStartY = pickerLastY = y;
    pickerMoved = false;
    pickerSamples = 0;
  }
  if (pickerWheel == nullptr) return;
  if (down)
  {
    if (abs(y - pickerStartY) >= 4) pickerMoved = true;
    const int16_t dy = y - pickerLastY;
    pickerLastY = y;
    const int slot = pickerSamples % 6;
    pickerSampleY[slot] = y;
    pickerSampleMs[slot] = now;
    pickerSamples++;
    if (dy != 0 && pickerMoved)
    {
      timepicker::drag(*pickerWheel, dy, pickerFrame.itemH);
      presentPicker();
    }
    return;
  }
  // The lift: a still tap brings the neighbour under it, a drag settles -
  // after a flick, further.
  if (!pickerMoved)
  {
    const timepicker::Rect &r = pickerWheel == &pickerState.hour ? pickerFrame.hour : pickerFrame.minute;
    const int k = timepicker::neighbourAt(r, pickerFrame.itemH, pickerStartY);
    if (k != 0) timepicker::nudge(*pickerWheel, k, pickerFrame.itemH, now);
  }
  else
  {
    float velocity = 0;
    if (pickerSamples >= 2)
    {
      const int newest = (pickerSamples - 1) % 6;
      const int oldest = pickerSamples > 6 ? pickerSamples % 6 : 0;
      const uint32_t dt = pickerSampleMs[newest] - pickerSampleMs[oldest];
      if (dt > 0) velocity = (float)(pickerSampleY[newest] - pickerSampleY[oldest]) / (float)dt;
    }
    timepicker::release(*pickerWheel, velocity, pickerFrame.itemH, now);
  }
  pickerWheel = nullptr;
}

static bool dispatchTapAt(int16_t x, int16_t y)
{
  if (!projectLoader.isLoaded() || renderer == nullptr) return false;

  // A tap on the navigator opens its entry's screen, as goto-screen does
  // (contract 2.7). Only on a screen: with a popup open the active index is
  // the popup's, and a tap out there closed it before reaching here.
  {
    navigatorlayout::Layout layout;
    const Navigator *nav = nullptr;
    if (renderer->navigatorLayout(activeScreen(), layout, &nav))
    {
      // Its light/dark switch asks for the other variant (2.7, generation
      // 1.6); the bridge answers on schaltli/state/theme, which redraws.
      const navigatorlayout::Rect &sw = layout.themeSwitch;
      if (layout.hasThemeSwitch && x >= sw.x && y >= sw.y && x < sw.x + sw.width && y < sw.y + sw.height)
      {
        const char *wanted = wantedThemeDark ? "light" : "dark";
        lastTap = "(" + String(x) + "," + String(y) + ") navigator theme switch -> " + wanted;
        mqttClient.publish("schaltli/cmnd/theme", wanted, false);
        return true;
      }
      const int entry = navigatorlayout::entryAt(layout, x, y, renderer->effectiveNavigatorScroll(activeScreen()));
      const bool onStrip = x >= layout.bar.x && y >= layout.bar.y && x < layout.bar.x + layout.bar.width &&
                           y < layout.bar.y + layout.bar.height;
      if (entry >= 0)
      {
        lastTap = "(" + String(x) + "," + String(y) + ") navigator entry " + String(entry);
        // The entry's screen, or off the main row the way back (2.10).
        ButtonAction go;
        go.type = "goto-screen";
        go.targetScreenId = renderer->navigatorEntryScreenId(activeScreen(), entry);
        if (!go.targetScreenId.isEmpty() && go.targetScreenId != projectLoader.getProject().screens[currentScreen].id) dispatchButtonAction(go);
        return true;
      }
      // Its empty end is still the navigator's: nothing under it is reached.
      if (onStrip)
      {
        lastTap = "(" + String(x) + "," + String(y) + ") navigator, no entry";
        return true;
      }
    }
  }

  // Absolute corner of whatever was hit. For an object inside a panel that
  // is NOT obj.x/obj.y, and the segment arithmetic below depends on it.
  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(activeScreen(), x, y, objX, objY);
  if (hit == nullptr)
  {
    lastTap = "(" + String(x) + "," + String(y) + ") hit nothing tappable";
    return false;
  }
  // A guarded control switches only while its cover waits above it; each
  // change tops the wait up (designer docs/2026-11-15-guard.md, decision 13).
  if (hit->guard.on)
  {
    if (!guards.unlocked(hit->id))
    {
      lastTap = "(" + String(x) + "," + String(y) + ") " + hit->type + " \"" + hit->id + "\" is guarded";
      return false;
    }
    guards.changed(hit->id, millis());
  }

  if (hit->type == "button")
  {
    Serial.printf("[tap] button \"%s\"\n", hit->id.c_str());
    lastTap = "(" + String(x) + "," + String(y) + ") SoftwareButton \"" + hit->id + "\" action " +
              (hit->properties.action.type.isEmpty() ? String("(none)") : hit->properties.action.type);
    // Where a popup it opens grows from.
    popupOriginKnown = true;
    popupOriginX = objX;
    popupOriginY = objY;
    popupOriginW = hit->width;
    popupOriginH = hit->height;
    dispatchButtonAction(hit->properties.action);
    popupOriginKnown = false;
    return true;
  }

  // A node of an energy diagram with an action (contract 2.8): the hit test
  // only lets a diagram catch a tap on one.
  if (hit->type == "energy-diagram")
  {
    // Its own switch first (designer docs/2026-11-11-energy-mode-switch.md):
    // the mode shown on this display, until the project is loaded again.
    const int asked = renderer->energySwitchAt(*hit, x - objX, y - objY);
    if (asked != 0)
    {
      Serial.printf("[tap] energy diagram \"%s\" shows %s\n", hit->id.c_str(), asked == 2 ? "sankey" : "flow");
      lastTap = "(" + String(x) + "," + String(y) + ") energy diagram \"" + hit->id + "\" switch " + (asked == 2 ? "sankey" : "flow");
      if (renderer->energyShowMode(*hit, asked == 2)) renderRegion(objX, objY, hit->width, hit->height, "energy mode");
      return true;
    }
    const ButtonAction *action = renderer->energyActionAt(*hit, x - objX, y - objY);
    if (action == nullptr) return false;
    Serial.printf("[tap] energy diagram \"%s\" action %s\n", hit->id.c_str(), action->type.c_str());
    lastTap = "(" + String(x) + "," + String(y) + ") energy diagram \"" + hit->id + "\" action " + action->type;
    popupOriginKnown = true;
    popupOriginX = x - 20;
    popupOriginY = y - 20;
    popupOriginW = 40;
    popupOriginH = 40;
    const ButtonAction copy = *action;
    dispatchButtonAction(copy);
    popupOriginKnown = false;
    return true;
  }

  if (hit->type == "time-field")
  {
    Serial.printf("[tap] time field \"%s\" - picker\n", hit->id.c_str());
    lastTap = "(" + String(x) + "," + String(y) + ") time field \"" + hit->id + "\" - picker open";
    openPicker(*hit);
    return true;
  }

  if (hit->type == "text-field")
  {
    Serial.printf("[tap] text field \"%s\" - keyboard\n", hit->id.c_str());
    lastTap = "(" + String(x) + "," + String(y) + ") text field \"" + hit->id + "\" - keyboard open";
    openKeyboard(*hit);
    return true;
  }

  const int states = (int)hit->properties.states.size();
  if (states <= 0 || hit->properties.writeTopic.isEmpty())
  {
    lastTap = "(" + String(x) + "," + String(y) + ") " + hit->type + " \"" + hit->id +
              "\" has nothing to write";
    return false;
  }

  const int index = switchSegmentFor(hit, x, objX);
  if (index < 0) return false;

  const String &value = hit->properties.states[index].writeValue;
  if (value.isEmpty()) return false;

  Serial.printf("[tap] Switch \"%s\" segment %d -> %s = %s\n", hit->id.c_str(), index,
                hit->properties.writeTopic.c_str(), value.c_str());
  // Not retained: a command, not a state. The state comes back on the read
  // topic and colours the segment, so the switch shows what actually
  // happened rather than what it believed it was doing.
  const bool sent = writeProjectTopic(hit->properties.writeTopic, value);
  // Marked as asked-for either way. If the command did not even leave the
  // device, the mark expiring is the honest outcome - the alternative is a
  // segment that looks switched because we said so.
  markPending(hit->id, index, objX, objY, hit->width, hit->height);
  // What ends the mark: this switch's own read topic answering (JSON field
  // paths stripped - the answer arrives on the bare topic).
  pendingReadTopic = stripJsonPath(hit->properties.topic);
  pendingAnswered = false;
  lastTap = "(" + String(x) + "," + String(y) + ") Switch \"" + hit->id + "\" segment " +
            String(index) + " -> " + hit->properties.writeTopic + " = " + value +
            (sent ? "" : "  (NOT published - broker away)");
  return true;
}

// One touch sample, from the glass or from a test, and the caller cannot
// tell which. Injection wins while its window lasts; a posted lift ends it
// at once rather than leaving the panel to time out into it.
static Gt911::Status readTouch(int16_t &x, int16_t &y)
{
  if (injectLiftOnce)
  {
    injectLiftOnce = false;
    touchPointCount = 0;
    return Gt911::Lifted;
  }
  if (injectUntilMs != 0 && (long)(millis() - injectUntilMs) < 0)
  {
    x = injectX;
    y = injectY;
    touchX2 = injectX2;
    touchY2 = injectY2;
    touchPointCount = !injectDown ? 0 : injectTwo ? 2 : 1;
    return injectDown ? Gt911::Touched : Gt911::Lifted;
  }

  Gt911::Point points[Gt911::MAX_POINTS];
  uint8_t n = 0;
  const Gt911::Status status = touch.poll(points, Gt911::MAX_POINTS, n);
  if (status == Gt911::Touched && n > 0)
  {
    x = (int16_t)points[0].x;
    y = (int16_t)points[0].y;
    touchPointCount = n;
    if (n >= 2)
    {
      touchX2 = (int16_t)points[1].x;
      touchY2 = (int16_t)points[1].y;
    }
  }
  else if (status == Gt911::Lifted)
  {
    touchPointCount = 0;
  }
  return status;
}

// A two-finger contact is over: what it was, logged and shown.
static void finishPinch()
{
  const PinchDetector::Result r = pinch.finish();
  if (r == PinchDetector::PinchIn) pinchIns++;
  else if (r == PinchDetector::PinchOut) pinchOuts++;
  else pinchNones++;

  char line[140];
  snprintf(line, sizeof line, "%lu  %-9s  %d -> %d px, %d two-finger samples, second finger after %lu ms, up to %u points\n",
           millis(), PinchDetector::name(r), pinch.startDistance(), pinch.endDistance(), pinch.twoFingerSamples(),
           (unsigned long)pinch.secondFingerAfterMs(), (unsigned)pinch.maxPoints());
  Serial.printf("[pinch] %s", line);
  pinchLog += line;
  // The last few dozen, not all of them since boot.
  if (pinchLog.length() > 4000)
  {
    const int cut = pinchLog.indexOf('\n', pinchLog.length() - 4000);
    pinchLog = pinchLog.substring(cut + 1);
  }

  // Pinch in and pinch out are gestures as the swipes are (contract 2.13):
  // the screen's action for them, where it has one.
  if ((r == PinchDetector::PinchIn || r == PinchDetector::PinchOut) && projectLoader.isLoaded() && openPopup < 0)
  {
    const ButtonAction action = projectLoader.getButtonAction(currentScreen, r == PinchDetector::PinchIn ? "pinch-in" : "pinch-out");
    if (!action.type.isEmpty()) dispatchButtonAction(action);
  }

  if (pinchShow && displayAdapter)
  {
    displayAdapter->showLines({r == PinchDetector::PinchIn ? "Pinch in" : r == PinchDetector::PinchOut ? "Pinch out" : "kein Pinch",
                               String(pinch.startDistance()) + " -> " + String(pinch.endDistance()) + " px"});
    pinchShownUntilMs = millis() + 1200;
  }
}

// ---------------------------------------------------------------------------
// The energy diagram's motion (designer docs/2026-10-11-energy-diagram.md,
// decision 10; device contract 2.8): a Flow's dots running along its lines,
// a Sankey's wave through its bands, for every diagram with Motion on that
// the screen shows. Measured first in the spike (decision 14), whose way
// this keeps: ten steps a second on a fixed grid at the end of loop(), none
// while a finger is down; and everything painted straight into every
// framebuffer, never owed by one, so no present - a tap's feedback
// included - pays for it.
//
// The canvas keeps the still picture: a diagram drawn for motion has no
// dots in it (ColorScreenRenderer), and a dot's old box is put back from
// the canvas. The wave is painted into the canvas too, so that what is put
// back is the band as it is now. Whatever redraws the diagram - a value, a
// screen - changes the renderer's count of diagrams drawn, and the moving
// parts are worked out again from the values then.
// ---------------------------------------------------------------------------
static const uint32_t ENERGY_STEP_MS = 100;
static std::vector<ColorScreenRenderer::EnergyAnimation> energyAnims;
static uint32_t energySeenDraws = UINT32_MAX, energySeenGeneration = UINT32_MAX, energySeenSwitchers = UINT32_MAX;
static int energySeenScreen = -1;
static double energyWave = 0;
static std::vector<std::pair<std::string, double>> energyDotPhases;  // "<diagram>/<node>" -> phase
struct EnergyDotBox { int16_t x, y; };
static std::vector<EnergyDotBox> energyDotBoxes;
static const int16_t ENERGY_DOT_BOX = 10;  // DOT_R 3.5, its soft edge and a pixel either side
static unsigned long energyLastStepMs = 0, energyLastMotionMs = 0, energyWindowMs = 0;
static uint32_t energySteps = 0, energyWindowSteps = 0, energyDotsUs = 0, energyWaveUs = 0, energyRestoreUs = 0, energyRasterUs = 0, energyFlushUs = 0;
static float energyStepsPerSecond = 0;
static size_t energyDotCount = 0;

static double &energyDotPhase(const std::string &key)
{
  for (auto &p : energyDotPhases)
    if (p.first == key) return p.second;
  energyDotPhases.push_back({key, 0.0});
  return energyDotPhases.back().second;
}

// Rows y0..y1 out of the cache, in every framebuffer.
static void energyWriteBack(int16_t y0, int16_t y1)
{
  const int16_t H = canvas->height();
  y0 = max<int16_t>(0, y0);
  y1 = min<int16_t>(H - 1, y1);
  for (size_t f = 0; f < panel.bufferCount() && y1 >= y0; f++) panel.writeBack(f, y0, y1);
}



// A dot's old box back from the canvas into every framebuffer.
static void energyRestoreDots(int16_t &rowLo, int16_t &rowHi)
{
  const uint16_t *src = canvas->getBuffer();
  const int16_t W = canvas->width(), H = canvas->height();
  for (const EnergyDotBox &b : energyDotBoxes)
  {
    const int16_t x0 = max<int16_t>(0, b.x), x1 = min<int16_t>(W, b.x + ENERGY_DOT_BOX);
    if (x1 <= x0) continue;
    for (int16_t y = max<int16_t>(0, b.y); y < min<int16_t>(H, b.y + ENERGY_DOT_BOX); y++)
    {
      for (size_t f = 0; f < panel.bufferCount(); f++)
        memcpy(panel.buffer(f) + (size_t)y * W + x0, src + (size_t)y * W + x0, (size_t)(x1 - x0) * sizeof(uint16_t));
    }
    rowLo = min(rowLo, b.y);
    rowHi = max<int16_t>(rowHi, b.y + ENERGY_DOT_BOX - 1);
  }
  energyDotBoxes.clear();
}

// The NowPoint's pulse (designer docs/2026-10-16-charts.md decisions 9 and
// 13): a ring widening from it and fading, once every 1.8 s, about 20 frames
// a second - only the boxes around the points drawn again, the rest of the
// chart as it is. Not while a finger, a slide or a popup is up or the display
// is off; stilled by ?set=chartpulse=off, and then drawn once without a ring.
static unsigned long chartPulseLastMs = 0;
static bool chartPulseShown = false;

static void chartPulseStep()
{
  if (!canvas || !renderer || !projectLoader.isLoaded() || displayIsOff || holdActive || swipe.active() || setupModeActive || openPopup >= 0) return;
  const unsigned long now = millis();
  if (now - chartPulseLastMs < 50) return;
  chartPulseLastMs = now;
  const bool on = chartFeed.pulsing();
  if (!on && !chartPulseShown) return;
  chartPulseShown = on;
  renderer->setChartPulse(on ? (double)(now % chartshapes::PULSE_MS) / chartshapes::PULSE_MS : -1);
  const std::vector<ColorScreenRenderer::PulseBox> boxes = renderer->chartPulseBoxes();
  for (const ColorScreenRenderer::PulseBox &b : boxes)
  {
    const int16_t x0 = max<int16_t>(0, b.x), y0 = max<int16_t>(0, b.y);
    const int16_t x1 = min<int16_t>(canvas->width(), b.x + b.w), y1 = min<int16_t>(canvas->height(), b.y + b.h);
    if (x1 <= x0 || y1 <= y0) continue;
    renderer->renderScreenRegion(currentScreen, x0, y0, x1 - x0, y1 - y0);
    panel.blitRegion(canvas->getBuffer(), x0, y0, x1 - x0, y1 - y0);
  }
}

// The pull tabs' nudge (designer device contract 2.10): towards the middle
// and back once every 5 s, the tabs' rectangles redrawn while it moves. Not
// while a finger, a slide or a popup is up, the display is off, or the test
// interface is on - there the tabs stand still at rest, so a snapshot never
// catches one half way.
static int pullTabShownNudge = 0;
static int pullTabShownScreen = -1;

static void pullTabStep()
{
  if (!canvas || !renderer || displayIsOff || holdActive || swipe.active() || setupModeActive || openPopup >= 0) return;
  const int nudge = testInterface.isRunning() ? 0 : screengraph::nudgeAt((long)millis());
  if (currentScreen != pullTabShownScreen)
  {
    // A screen drawn anew drew its tabs at the nudge in force.
    pullTabShownScreen = currentScreen;
    pullTabShownNudge = renderer->pullTabNudge();
  }
  if (nudge == pullTabShownNudge) return;
  renderer->setPullTabNudge(nudge);
  pullTabShownNudge = nudge;
  for (const screengraph::Rect &r : renderer->pullTabRegions(currentScreen))
  {
    renderer->renderScreenRegion(currentScreen, r.x, r.y, r.width, r.height, pressedButtonId, pendingSwitchId, pendingSwitchIndex,
                                 pressedSwitchId, pressedSwitchIndex);
    panel.blitRegion(canvas->getBuffer(), r.x, r.y, r.width, r.height);
  }
}

// A Camera on what is shown (designer docs/2026-11-09-camera.md): the feed runs
// while one is visible, and each new picture goes to the canvas and to the glass
// as one rectangle, with whatever lies above it drawn again first.
static const uint32_t CAMERA_STALE_MS = 1000;
static bool cameraMissingShown = false;

static const ScreenObject *cameraObjectShown()
{
  if (!canvas || !renderer || !projectLoader.isLoaded() || displayIsOff || setupModeActive) return nullptr;
  const ProjectConfig &project = projectLoader.getProject();
  const int index = activeScreen();
  if (index < 0 || index >= (int)project.screens.size()) return nullptr;
  for (const ScreenObject &o : project.screens[index].objects)
    if (o.properties.camera) return &o;
  return nullptr;
}

// A label on the picture stays on it: the picture went in underneath only now.
static void cameraRedrawAbove(const ScreenObject &camera, int16_t width, int16_t height)
{
  const int index = activeScreen();
  for (const screengraph::Rect &r : renderer->regionsAbove(index, camera, width, height))
    renderer->renderScreenRegion(index, r.x, r.y, r.width, r.height, pressedButtonId, pendingSwitchId, pendingSwitchIndex,
                                 pressedSwitchId, pressedSwitchIndex);
}

static void cameraStep()
{
  const ScreenObject *camera = cameraObjectShown();
  if (camera == nullptr)
  {
    if (cameraFeed.active()) cameraFeed.stop();
    return;
  }
  if (!cameraFeed.active() || cameraFeed.objectId() != camera->id)
  {
    cameraFeed.start(camera->id, camera->properties.camera->url, camera->properties.camera->username,
                     camera->properties.camera->password, camera->width, camera->height);
    cameraMissingShown = false;
  }
  // While a finger moves the page the frames wait; the feed goes on.
  if (holdActive || swipe.active()) return;
  const uint16_t *picture = cameraFeed.takeNewest();
  if (picture == nullptr)
  {
    if (!cameraMissingShown && cameraFeed.pictureAgeMs() > CAMERA_STALE_MS)
    {
      cameraFeed.forgetShown();
      renderer->drawCameraMissing(*camera);
      cameraRedrawAbove(*camera, camera->width, camera->height);
      panel.blitRegion(canvas->getBuffer(), camera->x, camera->y, camera->width, camera->height);
      cameraMissingShown = true;
    }
    return;
  }
  cameraMissingShown = false;
  const int16_t W = canvas->width(), width = cameraFeed.width(), height = cameraFeed.height();
  uint16_t *dst = canvas->getBuffer();
  for (int16_t row = 0; row < height; row++)
    memcpy(dst + (size_t)(camera->y + row) * W + camera->x, picture + (size_t)row * width, (size_t)width * sizeof(uint16_t));
  cameraRedrawAbove(*camera, width, height);
  panel.blitRegion(canvas->getBuffer(), camera->x, camera->y, width, height);
}

static String cameraReport()
{
  return cameraFeed.report() + ", min period ms " + String(cameraFeed.minPeriodMs()) + ", loop gap max ms " +
         String(loopMaxGapUs / 1000.0f, 1) + " (values " + String(worstValuesUs / 1000.0f, 1) + ", energy " +
         String(worstEnergyUs / 1000.0f, 1) + ", camera " + String(worstCameraUs / 1000.0f, 1) + ")";
}

static void cameraResetStats()
{
  cameraFeed.resetStats();
  // The next pass starts counting: what is left of the request that asked for
  // this is not a pause the camera caused.
  loopMaxGapUs = 0;
  loopLastUs = 0;
  worstValuesUs = worstEnergyUs = worstCameraUs = 0;
}

static void energyStep()
{
  if (!canvas || !renderer || displayIsOff || holdActive || swipe.active() || setupModeActive) return;
  const unsigned long now = millis();
  if (now - energyLastStepMs < ENERGY_STEP_MS) return;
  const double dt = (energyLastMotionMs ? now - energyLastMotionMs : ENERGY_STEP_MS) / 1000.0;
  energyLastMotionMs = now;
  energyLastStepMs += ENERGY_STEP_MS;
  if (now - energyLastStepMs >= 2 * ENERGY_STEP_MS) energyLastStepMs = now;

  const int screen = activeScreen();
  int16_t rowLo = INT16_MAX, rowHi = -1;
  const uint32_t t0 = micros();
  const uint32_t tr = micros();
  energyRestoreDots(rowLo, rowHi);
  energyRestoreUs += micros() - tr;
  // A popup's diagrams move as a screen's (its index is activeScreen()'s), and
  // a switcher drawn anew may show a diagram, or no longer show one.
  if (renderer->energyDrawCount() != energySeenDraws || renderer->switcherDrawCount() != energySeenSwitchers ||
      renderGeneration != energySeenGeneration || screen != energySeenScreen)
  {
    energyAnims = renderer->energyAnimations(screen);
    energySeenDraws = renderer->energyDrawCount();
    energySeenSwitchers = renderer->switcherDrawCount();
    energySeenGeneration = renderGeneration;
    energySeenScreen = screen;
  }
  if (energyAnims.empty()) return;

  // The dots: on by their pace, drawn over the still picture into every framebuffer.
  const uint16_t *src = canvas->getBuffer();
  const int16_t W = canvas->width(), H = canvas->height();
  energyDotCount = 0;
  for (const auto &a : energyAnims)
  {
    for (const auto &l : a.lines)
    {
      double &phase = energyDotPhase(std::string(a.id.c_str()) + "/" + l.id);
      phase = energydiagram::advanceDots(phase, dt, l.watts);
      // A storage's dots run on into its track, lighter on the bar, fading.
      std::vector<energydiagram::PathDot> path;
      if (l.storage) path = energydiagram::storageDots(l.line, l.node, phase, l.gives, l.watts);
      else
        for (const energydiagram::Point &p : energydiagram::dotsOnLine(l.line, phase, l.gives, l.watts)) path.push_back({p.x, p.y, 1, false, true});
      for (const energydiagram::PathDot &d : path)
      {
        const uint32_t td = micros();
        const uint16_t ink = d.onBar ? energydiagram::mix565(l.colour, 0xFFFF, (int)energydiagram::jsRound(energydiagram::BAR_DOT_LIGHT * 256)) : l.colour;
        const int strength = (int)energydiagram::jsRound(d.alpha * 256);
        ColorScreenRenderer::energyDotPixels(d.x, d.y, [&](int16_t x, int16_t y, uint8_t c) {
          if (x < 0 || y < 0 || x >= W || y >= H) return;
          const uint16_t px = energydiagram::mix565(src[(size_t)y * W + x], ink, c * strength / 16);
          for (size_t f = 0; f < panel.bufferCount(); f++) panel.buffer(f)[(size_t)y * W + x] = px;
        });
        energyRasterUs += micros() - td;
        const EnergyDotBox box{(int16_t)(lroundf(d.x) - ENERGY_DOT_BOX / 2), (int16_t)(lroundf(d.y) - ENERGY_DOT_BOX / 2)};
        rowLo = min(rowLo, box.y);
        rowHi = max<int16_t>(rowHi, box.y + ENERGY_DOT_BOX - 1);
        energyDotBoxes.push_back(box);
        energyDotCount++;
      }
    }
  }
  const uint32_t t1 = micros();

  // The wave: a row of colours per band - a column's colour is by its x
  // alone -, then each run of the band's whole pixels copied from it into
  // the canvas and every framebuffer.
  energyWave = energydiagram::advanceWave(energyWave, dt);
  static std::vector<uint16_t> row;
  row.resize(W);
  uint16_t *canvasBuf = canvas->getBuffer();
  for (const auto &a : energyAnims)
  {
    if (!a.sankey) continue;
    for (uint8_t band = 0; band < a.bandRgb.size(); band++)
    {
      int16_t lo = INT16_MAX, hi = -1;
      for (const auto &r : a.runs)
        if (r.band == band) { lo = min(lo, r.x0); hi = max(hi, r.x1); }
      if (hi < lo) continue;
      for (int16_t x = lo; x <= hi; x++) row[x] = ColorScreenRenderer::energyWaveColour(a, band, x, energyWave);
      for (const auto &r : a.runs)
      {
        if (r.band != band) continue;
        const size_t at = (size_t)r.y * W + r.x0, n = (size_t)(r.x1 - r.x0 + 1) * sizeof(uint16_t);
        memcpy(canvasBuf + at, row.data() + r.x0, n);
        for (size_t f = 0; f < panel.bufferCount(); f++) memcpy(panel.buffer(f) + at, row.data() + r.x0, n);
        rowLo = min(rowLo, r.y);
        rowHi = max(rowHi, r.y);
      }
    }
  }
  // One range out of the cache: a box at a time cost more (2026-10-12).
  const uint32_t tf = micros();
  if (rowHi >= rowLo) energyWriteBack(rowLo, rowHi);
  energyFlushUs += micros() - tf;
  const uint32_t t2 = micros();
  energyDotsUs += t1 - t0;
  energyWaveUs += t2 - t1;
  energySteps++;
  energyWindowSteps++;
  if (now - energyWindowMs >= 2000)
  {
    energyStepsPerSecond = energyWindowSteps * 1000.0f / (now - energyWindowMs);
    energyWindowSteps = 0;
    energyWindowMs = now;
  }
}

static String energyReport()
{
  if (energyAnims.empty()) return "still (no diagram with Motion on this screen)";
  const float n = energySteps ? (float)energySteps : 1.0f;
  return String((unsigned)energyAnims.size()) + " diagram(s), " + String((unsigned)energyDotCount) + " dots, " + String(energyStepsPerSecond, 1) +
         " steps/s, per step: dots " + String(energyDotsUs / n / 1000.0f, 2) + " ms (restore " + String(energyRestoreUs / n / 1000.0f, 2) + ", raster " +
         String(energyRasterUs / n / 1000.0f, 2) + ", flush " + String(energyFlushUs / n / 1000.0f, 2) + "), wave " + String(energyWaveUs / n / 1000.0f, 2) + " ms";
}

// The longest time between two passes of loop() since the last reset: how long
// a finger or a value can wait (spec success criterion 4).

void loop()
{
  const uint32_t loopNowUs = micros();
  if (loopLastUs != 0 && loopNowUs - loopLastUs > loopMaxGapUs)
  {
    loopMaxGapUs = loopNowUs - loopLastUs;
    worstValuesUs = turnValuesUs;
    worstEnergyUs = turnEnergyUs;
    worstCameraUs = turnCameraUs;
  }
  loopLastUs = loopNowUs;
  turnValuesUs = turnEnergyUs = turnCameraUs = 0;
  if (restartAtMs && millis() >= restartAtMs) ESP.restart();
  if (!setupModeActive && wifi.tick() && noWifiScreenShown)
  {
    noWifiScreenShown = false;
    renderAndPresent("wifi up");
  }

  // Setup mode owns the device completely: it draws its own screen, it is
  // the only thing on port 80, and nothing else may run. Both faults in the
  // first version came from not doing that - the normal loop kept rendering
  // underneath and painted the project back over the setup screen, and
  // tick(), which draws that screen and reports the idle timeout, was never
  // called at all.
  if (setupModeActive)
  {
    wifiSetupServer->handleClient();

    // "tap to cancel", which the screen has said since this board first drew
    // it and which did nothing until 2026-09-15: this branch never read the
    // touch panel. A tap anywhere counts, the PaperS3's rule, but only one
    // that begins after setup mode did - the finger that just held the glass
    // for five seconds is still on it, and its lift is not a tap.
    //
    // Initialised on the first pass through here, which is the moment setup
    // mode began: from the hold, fingerDown is still true; from a boot with
    // no credentials, nothing has touched the glass.
    //
    // A tap survives this loop being held elsewhere. The setup page's network
    // scan holds it for about 3s, and a tap inside that window still arrived
    // as a touch and then a lift (measured over serial, 2026-09-15): the
    // GT911 keeps its first unread report until it is cleared, not its last.
    static bool setupLiftSeen = !fingerDown;
    static bool setupTapDown = false;
    static unsigned long setupLastReportMs = millis();
    bool tapToCancel = false;
    {
      int16_t tx = 0, ty = 0;
      const Gt911::Status status = readTouch(tx, ty);
      if (status == Gt911::Touched)
      {
        setupLastReportMs = millis();
        if (setupLiftSeen) setupTapDown = true;
      }
      else if (status == Gt911::Lifted)
      {
        tapToCancel = setupTapDown;
        setupLiftSeen = true;
      }
      else if (millis() - setupLastReportMs >= TOUCH_LOST_MS)
      {
        // Quiet for longer than any gap between reports, and the lift was
        // never reported - the same fallback the normal loop uses. The
        // entry finger is given a whole quiet second before it counts as
        // gone, because ending that contact too early would turn its lift
        // into a tap and cancel setup mode the moment it opened.
        if (setupTapDown) tapToCancel = true;
        else if (millis() - setupLastReportMs >= 1000) setupLiftSeen = true;
      }
    }

    // Leaving restarts rather than tearing the AP down and resuming in
    // place, the same choice the knob makes: a restart has one well-tested
    // path back into normal operation, unwinding AP mode by hand has none.
    // It also means setupModeActive never has to be cleared.
    if (wifiSetupServer->tick() || wifiSetupServer->exitRequested() || tapToCancel)
    {
      Serial.printf(BOARD_LOG "leaving setup mode%s - restarting\n", tapToCancel ? " (tap to cancel)" : "");
      delay(200);
      ESP.restart();
    }
    delay(5);
    return;
  }

  // A transition owns the whole loop while it runs, the same way setup mode
  // does above. The web client and the MQTT client can each block for
  // longer than a frame, and a value arriving through the second one used
  // to render the outgoing screen straight over the gesture. Nothing else
  // gets a turn until the finger is up.
  if (swipe.active())
  {
    int16_t dragX = touchX, dragY = touchY;
    const Gt911::Status dragStatus = readTouch(dragX, dragY);

    if (dragStatus == Gt911::Touched)
    {
      lastTouchReportMs = millis();
      swipe.follow(dragX, dragY);
      // The web client gets its turn here too, as while holding: a test
      // driving a swipe through /api/touch moves the finger with requests,
      // and without this none of them was served until the swipe gave up -
      // it followed the first step only and slid back (hil/navigator.js,
      // 2026-10-08). With nobody asking it costs nothing.
      testInterface.handleClient();
      return;
    }

    if (dragStatus == Gt911::NoNewSample)
    {
      // Nothing new to say is not a lifted finger. Wait for the controller
      // instead of composing the same frame again, and give up only once it
      // has been quiet for longer than any gap it leaves between reports.
      swipe.noteRecovery();
      if (millis() - lastTouchReportMs < TOUCH_LOST_MS)
      {
        delay(2);
        return;
      }
    }

    const int screenBeforeRelease = currentScreen;
    swipe.release();
    // The transition may have swapped which canvas is current. Anything
    // holding the old pointer would now be looking at the screen that just
    // left the glass.
    testInterface.setCanvas(*canvas);
    if (displayAdapter) displayAdapter->setCanvas(*canvas);
    // The navigator stood still through the swipe, so the glass still holds
    // the strip of the screen that left - its highlight one behind. The new
    // canvas has it right already, and goes up whole: a copy, not a render
    // (about 60 ms). Whole, not the strip alone - the swipe wrote its frames
    // straight into the panel's buffers, so the buffer a region copy lands
    // in still holds an earlier frame of the slide everywhere else. Found by
    // hand 2026-10-08: after a few swipes the number on the screen and the
    // highlighted entry no longer agreed.
    if (currentScreen != screenBeforeRelease && renderer)
    {
      navigatorlayout::Layout layout;
      if (renderer->navigatorLayout(currentScreen, layout)) panel.blit(canvas->getBuffer());
    }
    // Whatever was marked belonged to the screen that just slid away, or to
    // the one that stayed - either way its rectangle has been repainted by
    // the transition and the mark is gone from the glass already.
    forgetTouchFeedback();
    // Deliberately not rendering here, even though a value is usually
    // waiting. That render is 180ms during which the next contact cannot
    // be seen, and these gestures come in quick succession - a fifth of a
    // second of blindness immediately after every swipe is its own way of
    // losing every other one. The loop below draws it on its next turn,
    // after it has looked for a finger.
    return;
  }

  // A finger on the glass owns the loop, for the same reason a transition
  // does. Recognising a swipe needs two touch samples inside one contact,
  // and a contact is over in 300ms - while one arriving value costs 180ms
  // to draw and the web client can block for longer. Eleven of twenty real
  // swipes were never seen at all: both samples landed either side of the
  // gesture, so the finger was only ever observed standing still.
  //
  // Whatever arrives meanwhile waits in the socket for a second or two,
  // which nobody can see, and is drawn when the finger lifts.
  const uint32_t turnStart = micros();
  loopTurns++;

  // Nothing may draw over the measuring square, including a finger.
  if (measuringPattern)
  {
    testInterface.handleClient();
    delay(10);
    return;
  }

  int16_t sampleX = touchX, sampleY = touchY;
  const uint32_t touchT0 = micros();
  const Gt911::Status touchStatus = readTouch(sampleX, sampleY);
  {
    const uint32_t d = micros() - touchT0;
    touchUs += d;
    if (d > worstTouchUs) worstTouchUs = d;
  }
  if (touchStatus == Gt911::Touched)
  {
    // Where each contact starts and ends, as the controller reports it: a
    // tap that hits nothing leaves no other trace, and whether the touch
    // panel's coordinates are the display's is a question only a finger
    // can ask (hil/touch-corners.js, the 4" on 2026-10-30).
    if (!fingerDown) Serial.printf("[touch] down %d,%d\n", sampleX, sampleY);
    fingerDown = true;
    touchX = sampleX;
    touchY = sampleY;
    lastTouchReportMs = millis();
  }
  else if (touchStatus == Gt911::Lifted)
  {
    if (fingerDown) Serial.printf("[touch] up %d,%d\n", touchX, touchY);
    fingerDown = false;
  }
  else if (fingerDown && (millis() - lastTouchReportMs) >= TOUCH_LOST_MS)
  {
    // Quiet for longer than a gap between reports, and it never said the
    // finger was gone. Treated as gone, so one lost report cannot leave a
    // contact open for ever.
    fingerDown = false;
  }

  // The picker takes every contact while it is open.
  if (pickerOpen)
  {
    if (fingerDown)
    {
      const bool fresh = !pickerContact;
      pickerContact = true;
      const bool woke = fresh && displayIsOff;
      if (fresh) wakeDisplay();
      if (!woke) pickerTouch(touchX, touchY, true, fresh);
      lastHumanInputMs = millis();
    }
    else if (pickerContact)
    {
      pickerContact = false;
      pickerTouch(touchX, touchY, false, false);
    }
    holdActive = false;
    testInterface.handleClient();
    delay(5);
    return;
  }

  // The keyboard takes every contact while it is open, a key on its press.
  if (keyboardOpen)
  {
    if (fingerDown && !keyboardContact)
    {
      keyboardContact = true;
      const bool woke = displayIsOff;
      wakeDisplay();
      if (!woke) keyboardTouch(touchX, touchY);
    }
    else if (!fingerDown)
    {
      keyboardContact = false;
    }
    if (fingerDown) lastHumanInputMs = millis();
    holdActive = false;
    testInterface.handleClient();
    delay(5);
    return;
  }

  // The reserved setup hold, checked before anything else can claim this
  // contact.
  if (fingerDown)
  {
    if (!holdActive)
    {
      holdActive = true;
      // A finger from the panel arrives now; a posted one arrived when the
      // request was handled (setTouchHandler).
      if (!(injectUntilMs != 0 && injectDown)) contactSampleUs = micros();
      tapLatencyOpen = true;
      holdStartX = touchX;
      holdStartY = touchY;
      contactStartX = touchX;
      contactStartY = touchY;
      holdStartMs = millis();
      holdLastSecondShown = -1;
      holdConsumed = false;
      contactCount++;
      contactSamples = 0;
      pinch.begin(millis());
      contactDrift = 0;
      contactOnlyWoke = displayIsOff;
      wakeDisplay();
      // Before anything is decided about what this gesture will be. A press
      // that waits to find out is not a press - but a finger that arrived on
      // a dark panel is not pressing anything at all.
      //
      // A settable level is offered the contact first and keeps it: it has no
      // pressed look to show, because the value moving under the finger is
      // the feedback (designer docs/2026-09-17-settable-level.md).
      // An open popup's close button takes the contact before anything under it.
      const bool onCloseBadge = openPopup >= 0 && renderer && renderer->popupCloseBadge().contains(touchX, touchY);
      navigatorlayout::Layout navLayout;
      navigatorContact = !contactOnlyWoke && onNavigatorStrip(touchX, touchY, navLayout);
      navigatorScrolled = false;
      if (navigatorContact) navigatorStartScroll = renderer->effectiveNavigatorScroll(currentScreen);
      // A guarded control not unlocked: its cover takes the contact - no
      // pressed look, no tap, no swipe (designer docs/2026-11-15-guard.md).
      bool guardContact = false;
      if (!contactOnlyWoke && !onCloseBadge && !navigatorContact && renderer)
      {
        int16_t gx = 0, gy = 0;
        const ScreenObject *under = renderer->hitTestTappable(activeScreen(), touchX, touchY, gx, gy);
        if (under != nullptr) guardContact = guards.down(*under, activeScreen(), gx, gy, touchX, touchY, millis());
      }
      if (!contactOnlyWoke && !onCloseBadge && !navigatorContact && !guardContact && !beginLevelDrag(touchX, touchY)) beginPressFeedback(touchX, touchY);
    }
    lastHumanInputMs = millis();
    if (contactOnlyWoke)
    {
      // Nothing else may claim this contact: not a tap, not a drag, not the
      // setup hold. The next one is a real one.
      testInterface.handleClient();
      delay(5);
      return;
    }
    contactSamples++;
    const int dx = (int)touchX - holdStartX;
    const int dy = (int)touchY - holdStartY;

    // Travel that is clearly horizontal is a page turn, not a hold that
    // wandered. Tested before the drift branch below, which would otherwise
    // swallow every drag by treating it as a fresh hold reference. A finger
    // that has already been shown a countdown is left alone.
    // Measured from where the finger went down, not from where the drag was
    // recognised: the screen then sits exactly under the finger, and the
    // commit threshold means the distance the user actually swiped.
    const int16_t drift = (int16_t)max(abs(dx), abs(dy));
    if (drift > contactDrift) contactDrift = drift;

    if (abs(dx) > abs(missedMaxDx)) missedMaxDx = dx;

    // The navigator owns this gesture: along its strip it scrolls, and it
    // never pages, even across (contract 2.7). Only the strip is redrawn.
    if (navigatorContact)
    {
      navigatorlayout::Layout layout;
      if (renderer && renderer->navigatorLayout(currentScreen, layout))
      {
        const int along = layout.horizontal ? dx : dy;
        if (navigatorScrolled || abs(along) > TAP_MAX_DRIFT)
        {
          const int scroll = navigatorlayout::clampScroll(layout, navigatorStartScroll - along);
          if (scroll != renderer->effectiveNavigatorScroll(currentScreen))
          {
            renderer->setNavigatorScroll(scroll);
            renderRegion(layout.strip.x, layout.strip.y, layout.strip.width, layout.strip.height, "navigator scroll");
          }
          navigatorScrolled = true;
        }
      }
      testInterface.handleClient();
      delay(5);
      return;
    }

    // A cover owns this gesture: it follows the finger up and nothing else
    // may have it - not a page turn, a pinch or the setup hold (E6).
    if (guards.fingerActive())
    {
      guards.move(touchX, touchY, millis());
      testInterface.handleClient();
      delay(5);
      return;
    }

    // The level owns this gesture: follow the finger and nothing else. Before
    // the paging branch below, which a horizontal drag would otherwise turn
    // into a page turn (decision 8).
    if (!levelDragObjId.isEmpty())
    {
      updateLevelDrag(touchX, touchY);
      testInterface.handleClient();
      delay(5);
      return;
    }

    // Two fingers make the contact a pinch, and nothing else may have it:
    // not a tap, a swipe or the setup hold, even once one finger has lifted
    // and the other wanders on. Only fresh reports are fed in, so the count
    // of two-finger samples means what the panel actually said.
    if (touchStatus == Gt911::Touched) pinch.sample(touchPointCount, touchX, touchY, touchX2, touchY2, millis());
    if (pinch.multi())
    {
      if (!pressedButtonId.isEmpty() || !pressedSwitchId.isEmpty())
      {
        pressedButtonId = "";
        pressedSwitchId = "";
        pressedSwitchIndex = -1;
        renderFeedbackRegion("pinch took over", false);
        if (pendingSwitchId.isEmpty()) feedbackActive = false;
      }
      testInterface.handleClient();
      delay(5);
      return;
    }

    // A sideways drag that began on a scrolling chart moves it through time
    // (contract 2.13; designer charts decision 12): no page change, the
    // chart follows the finger and settles on whole days when let go.
    if (!chartFeed.dragging() && !holdConsumed && openPopup < 0 && abs(dx) >= FollowSwipe::startThresholdPx() && abs(dx) > 2 * abs(dy))
    {
      const ScreenObject *chart = ChartFeed::scrollingChartAt(projectLoader.getProject(), currentScreen, contactStartX, contactStartY);
      if (chart != nullptr)
      {
        chartFeed.beginDrag(projectLoader.getProject(), currentScreen, *chart, contactStartX);
        if (!pressedButtonId.isEmpty() || !pressedSwitchId.isEmpty())
        {
          pressedButtonId = "";
          pressedSwitchId = "";
          pressedSwitchIndex = -1;
          renderFeedbackRegion("chart drag took over", false);
          if (pendingSwitchId.isEmpty()) feedbackActive = false;
        }
      }
    }
    if (chartFeed.dragging())
    {
      chartFeed.dragTo(projectLoader.getProject(), touchX);
      if (chartFeed.takeRedraw()) renderCharts("chart drag");
      testInterface.handleClient();
      delay(5);
      return;
    }

    const bool sideways = abs(dx) >= FollowSwipe::startThresholdPx() && abs(dx) > 2 * abs(dy) && pagingTargetFor(dx) >= 0;
    const bool upDown = abs(dy) >= FollowSwipe::startThresholdPx() && abs(dy) > 2 * abs(dx) && pagingTargetFor(dy, true) >= 0;
    if (!holdConsumed && (sideways || upDown))
    {
      // The finger is leaving, so nothing it touched on the way is pressed.
      // Corrected in the canvas only: the transition reads from there, and
      // the panel is about to be replaced wholesale.
      if (!pressedButtonId.isEmpty() || !pressedSwitchId.isEmpty())
      {
        pressedButtonId = "";
        pressedSwitchId = "";
        pressedSwitchIndex = -1;
        renderFeedbackRegion("drag took over", false);
        if (pendingSwitchId.isEmpty()) feedbackActive = false;
      }
      // The incoming screen enters from the edge the finger is heading
      // toward, whichever screen the project says that is.
      const int target = sideways ? pagingTargetFor(dx) : pagingTargetFor(dy, true);
      const int travel = sideways ? dx : dy;
      navigatorFollow(target);
      if (swipe.start(target, travel < 0 ? 1 : -1, sideways ? holdStartX : holdStartY, !sideways))
      {
        holdActive = false;
        return;
      }
    }

    if (abs(dx) <= SETUP_HOLD_MAX_DRIFT && abs(dy) <= SETUP_HOLD_MAX_DRIFT)
    {
      const unsigned long held = millis() - holdStartMs;
      if (held >= SETUP_HOLD_MS)
      {
        enterSetupMode();
        return;
      }
      if (held >= SETUP_HOLD_FEEDBACK_MS)
      {
        holdConsumed = true;
        const int secondsLeft = (int)((SETUP_HOLD_MS - held + 999) / 1000);
        if (secondsLeft != holdLastSecondShown)
        {
          holdLastSecondShown = secondsLeft;
          showHoldCountdown(secondsLeft);
        }
      }
    }
    else
    {
      // Moved too far for a hold. Nothing else claims a horizontal drag
      // on this board, so the reference simply restarts from here - a
      // finger that wandered and then settled can still reach five seconds.
      holdStartX = touchX;
      holdStartY = touchY;
      holdStartMs = millis();
      holdLastSecondShown = -1;
    }

    // The web client still gets its turn, MQTT still does not. That split is
    // the whole point: the 3s that blinded this board was a broker connect,
    // measured, while the web server's worst turn was 13ms. Leaving it out
    // as well cost more than it saved - a press cannot be photographed if
    // the request for the photograph is only served after the finger lifts,
    // which is exactly how the press feedback came to look like it was not
    // working at all. A test fetching a full snapshot does block a gesture
    // for about a second; that is a trade taken deliberately, because
    // nobody fetches snapshots while swiping except a test that wants one.
    testInterface.handleClient();

    // Otherwise nothing else gets a turn. 5ms is short against a gesture
    // and long against an I2C read, so a 300ms swipe is seen dozens of
    // times instead of once or twice.
    delay(5);
    return;
  }

  if (holdActive)
  {
    holdActive = false;

    // The navigator's contact: a still one is its tap, anything else was a
    // scroll and is over. Never a swipe.
    if (navigatorContact)
    {
      navigatorContact = false;
      if (!navigatorScrolled && !contactOnlyWoke && contactDrift <= TAP_MAX_DRIFT) dispatchTapAt(holdStartX, holdStartY);
      contactOnlyWoke = false;
      testInterface.handleClient();
      delay(5);
      return;
    }

    // A cover's contact: lifted before it locked in, the cover goes (and a
    // plain tap shows the hint); after, nothing. Never a tap or a swipe.
    if (guards.fingerActive())
    {
      guards.up(millis());
      contactOnlyWoke = false;
      testInterface.handleClient();
      delay(5);
      return;
    }

    // A drag through a chart's time is neither a tap nor a swipe.
    if (chartFeed.dragging())
    {
      chartFeed.endDrag(projectLoader.getProject());
      contactOnlyWoke = false;
      testInterface.handleClient();
      delay(5);
      return;
    }

    // A drag on a level is neither a tap nor a swipe: it ends with what the
    // finger settled on (decision 3).
    if (!levelDragObjId.isEmpty())
    {
      endLevelDrag();
      contactOnlyWoke = false;
      testInterface.handleClient();
      delay(5);
      return;
    }

    if (pinch.multi())
    {
      finishPinch();
      contactOnlyWoke = false;
      testInterface.handleClient();
      delay(5);
      return;
    }

    // A contact that came and went without ever being read as a swipe.
    // How many samples it got, and how far it was seen to travel, say
    // which of the two explanations is the right one.
    missedContacts++;
    if (contactSamples > missedMaxSamples) missedMaxSamples = contactSamples;

    // A contact that stayed put and was not claimed by the setup hold is a
    // tap. Dispatched from where the finger went down rather than where it
    // left, because that is the point the user aimed at.
    // Unpressed first, then dispatched: an action that changes screens
    // repaints everything itself, and a region render afterwards would be
    // drawing yesterday's rectangle onto today's screen.
    pressedButtonId = "";
    pressedSwitchId = "";
    pressedSwitchIndex = -1;

    const uint32_t generationBefore = renderGeneration;
    // A contact that arrived on a dark panel wakes it and does nothing
    // else, all the way to the lift. Checked here as well as at touch-down
    // because the down path returns early and never reaches the dispatch -
    // so without this the wake tap fired on release anyway, which is the
    // exact behaviour the rule exists to prevent.
    if (!contactOnlyWoke && !holdConsumed && contactDrift <= TAP_MAX_DRIFT)
    {
      // A tap on an open popup's close button, or beside the popup, closes
      // it and does nothing else.
      if (openPopup >= 0 && renderer && renderer->popupCloseBadge().contains(holdStartX, holdStartY))
      {
        lastTap = "(" + String(holdStartX) + "," + String(holdStartY) + ") on the close button";
        closePopup("close button");
      }
      else if (openPopup >= 0 && !projectLoader.getProject().popupFence.contains(holdStartX, holdStartY))
      {
        lastTap = "(" + String(holdStartX) + "," + String(holdStartY) + ") outside the popup";
        closePopup("tap outside");
      }
      else
      {
        dispatchTapAt(holdStartX, holdStartY);
      }
    }
    else if (!contactOnlyWoke && !holdConsumed)
    {
      // Moved too far to be a tap and never became a page turn: whatever the
      // project binds to this direction, if anything.
      dispatchSwipeAtRelease((int)touchX - contactStartX, (int)touchY - contactStartY);
    }
    contactOnlyWoke = false;
    if (renderGeneration == generationBefore)
    {
      renderFeedbackRegion("release");
      if (pendingSwitchId.isEmpty()) feedbackActive = false;
    }
    // A hold that showed a countdown and was then released must not count as
    // anything else, and the panel has to be put back because the countdown
    // drew over it.
    if (holdConsumed)
    {
      holdConsumed = false;
      renderAndPresent("hold cancelled");
    }
  }

  if (pinchShownUntilMs != 0 && (long)(millis() - pinchShownUntilMs) >= 0)
  {
    pinchShownUntilMs = 0;
    renderAndPresent("pinch shown");
  }

  const uint32_t webT0 = micros();
  testInterface.handleClient();
  {
    const uint32_t d = micros() - webT0;
    webUs += d;
    if (d > worstWebUs) worstWebUs = d;
  }

  const uint32_t mqttT0 = micros();
  mqttClient.loop();
  // What this display wrote to its local topics, taken in as arrivals.
  if (!localWrites.empty())
  {
    std::vector<std::pair<String, String>> writes;
    writes.swap(localWrites);
    for (const auto &w : writes) applyTopicValue(w.first, w.second);
  }
  subscribeToProjectTopics();

  // The charts' history: what the shown screen reads, subscribed when the
  // screen, its range or the day changed; drawn again when it arrived.
  if (projectLoader.isLoaded())
  {
    const ProjectConfig &project = projectLoader.getProject();
    const int popup = openPopup >= 0 ? openPopup - (int)project.screens.size() : -1;
    if (chartFeed.due() || currentScreen != chartFeedScreen || popup != chartFeedPopup)
    {
      chartFeedScreen = currentScreen;
      chartFeedPopup = popup;
      chartFeed.update(project, currentScreen, popup, mqttClient);
    }
    if (chartFeed.takeRedraw() && currentScreen < (int)project.screens.size() &&
        (ChartFeed::hasChart(project.screens[currentScreen].objects) || popup >= 0))
      renderAndPresent("chart");
  }
  firmwareUpdater.service();
  deviceRequests.service();
  testInterfaceSwitch.service();
  if (pendingDeploy)
  {
    pendingDeploy = false;
    const String payload = pendingDeployPayload;
    pendingDeployPayload = "";
    deployManager.handleDeployMessage(payload);
  }
  {
    const uint32_t d = micros() - mqttT0;
    mqttUs += d;
    if (d > worstMqttUs) worstMqttUs = d;
  }

  // The switch's own answer arrived, so the device has newer state than the
  // mark does. The mark goes before the render rather than after it, so the
  // fresh value decides the picture instead of being drawn under a stale
  // "waiting". Only its own answer: any value used to end it, and on a screen
  // with anything else live one of those came in first - the knob went back
  // to the old side until the answer moved it again, three jumps for one tap
  // (2026-09-27, the Theme switch on the 4.3B in the van).
  if (pendingAnswered && !pendingSwitchId.isEmpty())
  {
    pendingSwitchId = "";
    pendingSwitchIndex = -1;
    pendingAnswered = false;
    feedbackActive = false;
  }
  else if (!pendingSwitchId.isEmpty() && (millis() - pendingSinceMs) >= PENDING_TIMEOUT_MS)
  {
    // Nothing came back. Better to drop the mark than to leave a switch
    // looking like it is still being asked.
    clearPending("pending timed out");
  }
  // While asked, a Toggle Button's light pulses (designer docs/2026-11-11-
  // led-look.md, decision 4): its rectangle redrawn on the renderer's beat.
  {
    static unsigned long lastPulseMs = 0;
    if (!pendingSwitchId.isEmpty() && pressedSwitchId.isEmpty() && millis() - lastPulseMs >= 500)
    {
      lastPulseMs = millis();
      renderRegion(pendingX, pendingY, pendingW, pendingH, "asked pulse");
    }
  }

  // A released level whose end value was never answered: the installation's
  // last word is shown after all, as decision 6c has it - just later.
  if (!levelAwaitValue.isEmpty() && (long)(millis() - levelAwaitUntilMs) >= 0)
  {
    levelAwaitValue = "";
    if (renderer != nullptr && !levelAskedTopic.isEmpty())
    {
      renderer->clearAskedValue();
      levelAskedTopic = "";
      pendingTopicOverflow = true;
      renderPending = true;
    }
  }

  // Only ever from here, where no finger is down: a screen going dark under
  // a hand resting on it would be the one moment it must not.
  if (!displayIsOff && displayOffAfterMs > 0 && (millis() - lastHumanInputMs) >= displayOffAfterMs)
  {
    blankDisplay("inactivity");
  }

  // While the keyboard or the picker is open the values wait: closing draws the screen.
  if ((keyboardOpen || pickerOpen) && renderPending)
  {
    renderPending = false;
    pendingTopicCount = 0;
    pendingTopicOverflow = false;
  }

  if (renderPending && topicRenderEnabled && (millis() - lastTopicRenderMs) >= TOPIC_RENDER_MIN_MS)
  {
    renderPending = false;
    lastTopicRenderMs = millis();
    const uint32_t t0 = micros();
    // Drawn even while dark: waking repaints, but a partial redraw would
    // otherwise have nothing to catch up on and the canvas would be stale in
    // exactly the rectangles that changed.
    if (!renderValueChangesPartial())
    {
      lastValueDraw = "full screen";
      renderAndPresent("topic");
    }
    topicRenderUs += micros() - t0;
    turnValuesUs += micros() - t0;
    topicRenders++;
    pendingTopicCount = 0;
    pendingTopicOverflow = false;
  }

  // The energy diagram's motion, after the touch and the values (see there).
  if (!keyboardOpen && !pickerOpen)
  {
    const uint32_t te = micros();
    guards.tick(millis());
    energyStep();
    const uint32_t tc = micros();
    turnEnergyUs += tc - te;
    // Not in a turn that drew values: the two together made the longest
    // pause, 162 ms of values and 98 of a picture in one turn on the 4.3B
    // (293 ms, 286 allowed; 2026-11-10). The picture waits one turn.
    if (turnValuesUs == 0) cameraStep();
    turnCameraUs += micros() - tc;
    pullTabStep();
    chartPulseStep();
  }
  // A released wheel settling onto its value, a frame per pass.
  if (pickerOpen && !pickerContact)
  {
    const uint32_t now = millis();
    const bool moved = timepicker::animate(pickerState.hour, now) | timepicker::animate(pickerState.minute, now);
    if (moved) presentPicker();
  }

  {
    const uint32_t turn = micros() - turnStart;
    if (turn > worstTurnUs) worstTurnUs = turn;
  }

  delay(20);
}
