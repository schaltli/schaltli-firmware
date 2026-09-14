// M5Stack PaperS3 - board bring-up.
//
// Third board in this repo, started 2026-09-13. The decisions it is being
// built to are in the designer repo's docs/2026-09-13-papers3-grilling.md,
// cited by number where they bite.
//
// What is here: the panel, the project stack above it, the test interface
// that lets a conformance run compare this device against the designer pixel
// for pixel, and touch - taps on Switch and SoftwareButton, plus the
// ten-second hold that opens setup mode.
//
// Almost none of the code below is this board's. ProjectLoader,
// ColorScreenRenderer, MqttClient, DeployManager and TestInterfaceServer
// already run on two boards; this file is the wiring and the parts that are
// genuinely different, which is the panel (EpdPanel.h) and what that implies
// for when to draw.

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "DeviceInfo.h"
#include "EpdPanel.h"
#include "PanelDisplayAdapter.h"
#include "../../ClippedCanvas16.h"
#include "../../DeployManager.h"
#include "../../DeviceProtocol.h"
#include "../../MqttClient.h"
#include "../../NvsConfigStorage.h"
#include "../../TestInterfaceServer.h"
#include "../../WiFiSetupServer.h"
#include "../../project/ColorScreenRenderer.h"
#include "../../project/ProjectLoader.h"

// The panel's own size, read back at boot and checked against what the DDF
// declares. A mismatch means the designer is laying out screens for a panel
// this device does not have, and every pixel comparison afterwards would be
// measuring that instead of whatever it was meant to measure.
static const int16_t DDF_SCREEN_W = 960;
static const int16_t DDF_SCREEN_H = 540;

static EpdPanel panel;
static ClippedCanvas16 *canvas = nullptr;
static PanelDisplayAdapter *displayAdapter = nullptr;

static ProjectLoader projectLoader;
static ColorScreenRenderer *renderer = nullptr;
static int currentScreen = 0;

static NvsConfigStorage configStorage;
static WiFiSetupServer *wifiSetupServer = nullptr;
static bool setupModeActive = false;

static MqttClient mqttClient;
static DeviceProtocol protocol(mqttClient);
static DeployManager deployManager;
static TestInterfaceServer testInterface;
static bool topicsSubscribed = false;

// Set when a topic value lands, cleared when the screen is drawn. The draw
// never happens inside the MQTT callback: on this panel it costs the better
// part of a second, and doing that inside the network loop blocks everything
// else for exactly as long. The value is stored the moment it arrives; only
// the drawing waits, and loop() decides when.
static bool renderPending = false;

// --- touch ------------------------------------------------------------
//
// The panel's GT911 comes with M5Unified, so unlike the 4.3B there is no
// driver to port - M5.update() fills M5.Touch and the code below only has to
// decide what a contact means.
//
// Injection exists so a test can press without a finger: POST /api/touch
// sets a point and a short window during which it wins over the glass. A
// posted lift ends the window at once rather than extending it, so the
// release runs against a panel that is also reading idle.
static const uint32_t TOUCH_INJECT_HOLD_MS = 2000;
static int16_t injectX = 0, injectY = 0;
static bool injectDown = false;
static uint32_t injectUntilMs = 0;
static bool injectLiftOnce = false;

// The setup-mode gesture (decision 7): ten seconds held in the top left
// quadrant. Long on purpose - 0.3s of feedback latency trains people to
// press longer, so the 4.3B's two seconds would fire here by accident.
static const uint32_t SETUP_HOLD_MS = 10000;
static bool holdActive = false;
static uint32_t holdStartMs = 0;
static int holdLastSecondShown = -1;
// Where the contact began and where it is now. A tap acts on the first, a
// swipe on the difference, and a hold needs to know it has not become either.
static int16_t contactStartX = 0, contactStartY = 0;
static int16_t contactLastX = 0, contactLastY = 0;
static bool contactMoved = false;

// Past a jittery finger, and clearly one axis rather than a diagonal. Same
// numbers as the 4.3B's, which were settled against real hands.
static const int SWIPE_MIN_DISTANCE = 60;
static const int SWIPE_AXIS_RATIO = 2;
// A hold stops being a hold once the finger travels. Without this a slow
// drag that began in the top left would still open setup mode.
static const int HOLD_MOVE_CANCEL = 20;

// One status line every ten seconds, because on this board the boot log is
// genuinely hard to catch: its USB device re-enumerates on every reset, so a
// serial capture started beforehand ends up holding a handle to a device
// that no longer exists and reads nothing, while one started afterwards has
// already missed setup(). Rather than choreograph that, the device just says
// where it is, forever, and any capture started at any moment finds out.
static void heartbeat()
{
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 10000) return;
  lastMs = millis();
  Serial.printf("[papers3] up %lus | wifi %s %s | mqtt %s | project %s | screen %d | partials %u/%u\n",
                (unsigned long)(millis() / 1000),
                WiFi.status() == WL_CONNECTED ? "ok" : "down",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                mqttClient.isConnected() ? "ok" : "down",
                projectLoader.isLoaded() ? "loaded" : "none", currentScreen,
                (unsigned)panel.partialsSinceFull(), (unsigned)EpdPanel::PARTIALS_BEFORE_CLEANUP);
}

static void enterSetupMode();

static void renderAndPresent(const char *reason)
{
  if (!renderer || !canvas) return;
  const uint32_t t0 = micros();
  const bool ok = renderer->renderScreen(currentScreen);
  const uint32_t t1 = micros();
  panel.blit(canvas->getBuffer());
  const uint32_t t2 = micros();
  renderPending = false;
  Serial.printf("[papers3] render(screen %d, %s): %s | draw %lums, panel %lums%s\n",
                currentScreen, reason, ok ? "ok" : "FAILED",
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000),
                panel.lastWasFull() ? " (full refresh)" : "");
}

// Which state a tap on a Switch asks for. Segment arithmetic identical to
// ColorScreenRenderer::renderSwitch()'s own division, because a finger and
// the picture disagreeing about where a segment ends is a bug nobody can
// see in a screenshot.
static int switchSegmentFor(const ScreenObject *hit, int16_t x, int16_t objX)
{
  const int states = (int)hit->properties.states.size();
  if (states <= 0) return -1;

  if (hit->properties.mode == "single")
  {
    // One area, one target: a tap moves to the next state and wraps. The
    // active index comes from the renderer rather than a second copy of the
    // rule here - what is drawn and what is tapped have to agree.
    const int active = renderer->getActiveSwitchStateIndex(*hit);
    return (active < 0) ? 0 : (active + 1) % states;
  }

  int index = ((int)(x - objX) * states) / hit->width;
  if (index < 0) index = 0;
  if (index >= states) index = states - 1;
  return index;
}

static void dispatchButtonAction(const ButtonAction &action)
{
  if (action.type.isEmpty() || !projectLoader.isLoaded()) return;
  const ProjectConfig &project = projectLoader.getProject();
  const int count = (int)project.screens.size();

  if (action.type == "next-screen" || action.type == "previous-screen")
  {
    if (count == 0) return;
    const int delta = (action.type == "next-screen") ? 1 : -1;
    currentScreen = ((currentScreen + delta) % count + count) % count;
    panel.forceFullNext();
    renderAndPresent("button: screen change");
  }
  else if (action.type == "goto-screen")
  {
    for (int i = 0; i < count; i++)
    {
      if (project.screens[i].id == action.targetScreenId)
      {
        currentScreen = i;
        panel.forceFullNext();
        renderAndPresent("button: goto screen");
        break;
      }
    }
  }
  else if (action.type == "send-mqtt")
  {
    // Not retained: a command, not a state. What comes back on the read
    // topic is what the panel then shows.
    mqttClient.publish(action.mqttTopic, action.mqttMessage, false);
  }
}

// A press at x,y. Returns true when it hit something that acted.
//
// The search runs through ColorScreenRenderer::hitTestTappable() rather than
// a loop over the screen's objects here, because the answer depends on two
// things that already live in the renderer: which panel of a tab-control is
// showing, and how far its children are drawn shifted. A second copy would
// mean the finger and the picture disagreeing about geometry, which is how
// the knob ended up with a working top-level switch and four dead ones
// inside panels.
static bool handleTap(int16_t x, int16_t y)
{
  if (!renderer || !projectLoader.isLoaded()) return false;

  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(currentScreen, x, y, objX, objY);
  if (hit == nullptr) return false;

  if (hit->type == "SoftwareButton")
  {
    Serial.printf("[papers3] tap: SoftwareButton \"%s\"\n", hit->id.c_str());
    dispatchButtonAction(hit->properties.action);
    return true;
  }

  const int states = (int)hit->properties.states.size();
  if (states <= 0 || hit->properties.writeTopic.isEmpty()) return false;

  const int index = switchSegmentFor(hit, x, objX);
  if (index < 0) return false;
  const String &value = hit->properties.states[index].writeValue;
  if (value.isEmpty()) return false;

  // Not retained: a command, not a state. The state comes back on the read
  // topic and colours the segment, so the switch shows what actually
  // happened rather than what it believed it was doing.
  const bool sent = mqttClient.publish(hit->properties.writeTopic, value, false);
  Serial.printf("[papers3] tap: Switch \"%s\" segment %d -> %s = %s%s\n", hit->id.c_str(), index,
                hit->properties.writeTopic.c_str(), value.c_str(),
                sent ? "" : "  (NOT published - broker away)");
  return true;
}

// A drag, resolved at release: the dominant axis names a button and the
// project says what that button does.
//
// No animated page turn, unlike the 4.3B's follow-swipe. On this panel every
// intermediate frame is its own refresh of a second or so, so a gesture that
// drags the outgoing screen along would take the better part of a minute and
// leave a trail of ghosts. The screen changes once, at release, with a clean
// paint.
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

  const ButtonAction action = projectLoader.getButtonAction(currentScreen, buttonId);
  if (action.type.isEmpty())
  {
    Serial.printf("[papers3] swipe: %s, nothing bound on this screen\n", buttonId);
    return;
  }
  Serial.printf("[papers3] swipe: %s -> %s\n", buttonId, action.type.c_str());
  dispatchButtonAction(action);
}

// One touch sample, from the glass or from a test, and the caller cannot
// tell which.
static bool readTouch(int16_t &x, int16_t &y)
{
  if (injectLiftOnce)
  {
    injectLiftOnce = false;
    return false;
  }
  if (millis() < injectUntilMs)
  {
    x = injectX;
    y = injectY;
    return injectDown;
  }

  const auto &d = M5.Touch.getDetail();
  if (!d.isPressed()) return false;
  x = d.x;
  y = d.y;
  return true;
}

// Press, hold and release, plus the setup gesture.
//
// Acting on release rather than on contact: a hold that becomes setup mode
// must not also have fired whatever it was resting on.
static void serviceTouch()
{
  int16_t x = 0, y = 0;
  const bool down = readTouch(x, y);
  if (down) panel.noteTouch();

  if (down && !holdActive)
  {
    holdActive = true;
    holdStartMs = millis();
    holdLastSecondShown = -1;
    contactStartX = contactLastX = x;
    contactStartY = contactLastY = y;
    contactMoved = false;
    return;
  }

  if (down && holdActive)
  {
    contactLastX = x;
    contactLastY = y;
    if (abs(x - contactStartX) > HOLD_MOVE_CANCEL || abs(y - contactStartY) > HOLD_MOVE_CANCEL)
      contactMoved = true;

    // Setup mode: ten seconds, still, in the top left quadrant (decisions 7
    // and 20). A gesture that has travelled is on its way to being a swipe
    // and must not also open setup.
    if (contactMoved) return;
    const bool inCorner = contactStartX < panel.width() / 2 && contactStartY < panel.height() / 2;
    if (!inCorner) return;

    const uint32_t held = millis() - holdStartMs;
    if (held >= SETUP_HOLD_MS)
    {
      holdActive = false;
      enterSetupMode();
      return;
    }

    const int second = (int)(held / 1000);
    if (second != holdLastSecondShown && second > 0)
    {
      holdLastSecondShown = second;
      // Log only, nothing drawn yet. Whatever draws a countdown here later
      // cannot trigger a clean-up flash mid-hold: the finger resting on the
      // glass keeps the panel from ever counting as idle (EpdPanel.h).
      Serial.printf("[papers3] setup hold %d/%d s\n", second, (int)(SETUP_HOLD_MS / 1000));
    }
    return;
  }

  if (!down && holdActive)
  {
    holdActive = false;
    const int dx = contactLastX - contactStartX;
    const int dy = contactLastY - contactStartY;

    // A travelled gesture is a swipe and never also a tap on whatever it
    // started over. A still one is a tap, provided it was not the setup
    // hold, which has already acted by now.
    if (contactMoved)
    {
      dispatchSwipeAtRelease(dx, dy);
      return;
    }
    if ((millis() - holdStartMs) < SETUP_HOLD_MS) handleTap(contactStartX, contactStartY);
  }
}

static void subscribeToProjectTopics()
{
  if (topicsSubscribed || !projectLoader.isLoaded() || !mqttClient.isConnected()) return;
  std::vector<String> topics;
  for (const auto &t : projectLoader.getProject().topics) topics.push_back(t.topic);
  if (!topics.empty()) mqttClient.subscribeToKeys(topics);
  topicsSubscribed = true;
  Serial.printf("[papers3] subscribed to %d project topic(s)\n", (int)topics.size());
}

static bool setupPanelAndProject()
{
  if (!panel.begin())
  {
    Serial.println("[papers3] panel did not come up - nothing else can work");
    return false;
  }
  Serial.printf("[papers3] panel %dx%d\n", panel.width(), panel.height());

  if (panel.width() != DDF_SCREEN_W || panel.height() != DDF_SCREEN_H)
  {
    // Loud rather than adaptive. Taking the panel's word for it would let
    // this device render happily at a size the designer never laid out for,
    // and the comparison would then fail on dimensions with no hint that the
    // DDF is the thing that is wrong.
    Serial.printf("[papers3] DDF declares %dx%d - fix ddf-source-papers3/device.json\n",
                  DDF_SCREEN_W, DDF_SCREEN_H);
  }

  // ~1MB at 960x540x2. Internal SRAM has nothing like that free, so a
  // non-null buffer is itself the proof it landed in PSRAM - but report
  // where, because "it worked" and "it worked for the reason I assumed" are
  // different claims.
  canvas = new ClippedCanvas16(panel.width(), panel.height());
  if (!canvas || !canvas->getBuffer())
  {
    Serial.printf("[papers3] FATAL: %u-byte canvas allocation failed - is PSRAM on?\n",
                  (unsigned)((int32_t)panel.width() * panel.height() * 2));
    return false;
  }
  Serial.printf("[papers3] canvas at %p (%s), psram free %u\n", canvas->getBuffer(),
                esp_ptr_external_ram(canvas->getBuffer()) ? "PSRAM" : "internal SRAM",
                (unsigned)ESP.getFreePsram());

  displayAdapter = new PanelDisplayAdapter(*canvas, panel, panel.width(), panel.height());
  renderer = new ColorScreenRenderer(projectLoader, canvas);

  if (!LittleFS.begin(true))
  {
    Serial.println("[papers3] LittleFS mount failed");
    displayAdapter->showLines({"Dateisystem defekt"});
    return false;
  }

  if (!projectLoader.loadProject())
  {
    // Not a failure of this function: a device with no project yet is the
    // normal state of a board that has just been flashed, and it still has
    // to reach WiFi so one can be deployed to it.
    Serial.println("[papers3] no project installed yet");
    displayAdapter->showLines({"ScreenBee", "PaperS3", "", "kein Projekt installiert"});
    return true;
  }

  Serial.printf("[papers3] project \"%s\" %dx%d, %u screen(s)\n",
                projectLoader.getProject().name.c_str(), projectLoader.getProject().screenWidth,
                projectLoader.getProject().screenHeight,
                (unsigned)projectLoader.getProject().screens.size());
  renderAndPresent("boot");
  return true;
}

static void enterSetupMode()
{
  if (setupModeActive || !wifiSetupServer) return;
  Serial.println("[papers3] entering setup mode - starting AP");
  setupModeActive = true;
  // The test interface owns port 80 in normal operation and the setup server
  // wants the same port, so it has to be stopped rather than left to fail a
  // bind nothing checks.
  testInterface.stop();
  wifiSetupServer->startAP();
}

static void setupNetworking()
{
  if (!canvas || !displayAdapter) return;

  configStorage.begin();
  wifiSetupServer = new WiFiSetupServer(*displayAdapter, configStorage);

  WiFiCredentials creds;
  if (!configStorage.loadWiFiCredentials(creds) || creds.ssid.isEmpty())
  {
    Serial.println("[papers3] no WiFi credentials - starting setup AP");
    enterSetupMode();
    return;
  }

  Serial.printf("[papers3] connecting to \"%s\"\n", creds.ssid.c_str());
  WiFi.mode(WIFI_STA);
  // Radio power save off, for the reason and measurement recorded at the
  // same line in the 4.3B's main.cpp. Mains-powered (decision 5), so the
  // 20-40mA it costs buys a panel that answers without waiting for its radio.
  WiFi.setSleep(false);
  WiFi.begin(creds.ssid.c_str(), creds.password.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[papers3] WiFi connect FAILED");
    displayAdapter->showLines({"WLAN nicht erreichbar", creds.ssid});
    return;
  }
  Serial.printf("[papers3] WiFi ok, ip %s\n", WiFi.localIP().toString().c_str());

  // The snapshot endpoint reads this canvas directly, which is what gives
  // this board a pixel comparison at all - and the reason ghosting is
  // invisible to it, since the glass is never what gets compared.
  if (testInterface.begin(*canvas)) Serial.println("[papers3] test interface on :80");

  testInterface.setScreenSwitchHandler([](int index) -> bool {
    if (!projectLoader.isLoaded()) return false;
    if (index < 0 || index >= (int)projectLoader.getProject().screens.size()) return false;
    currentScreen = index;
    // A screen change is a new picture, not a change to the old one, so it
    // gets a clean paint rather than spending the partial budget on a
    // rectangle that covers everything anyway.
    panel.forceFullNext();
    renderAndPresent("test interface: screen switch");
    return true;
  });

  // Lets a test ask "has the device applied this value yet" instead of
  // sleeping and hoping - the fix for a whole class of flaky runs.
  testInterface.setTopicValueHandler(
      [](const String &topic) { return projectLoader.getTopicValue(topic); });

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

  // Lets a run press a Switch or a SoftwareButton without a finger, which
  // is what makes those two types testable at all (decision 11).
  testInterface.setTouchHandler([](int16_t x, int16_t y, bool down) {
    injectX = x;
    injectY = y;
    injectDown = down;
    injectUntilMs = down ? millis() + TOUCH_INJECT_HOLD_MS : 0;
    injectLiftOnce = !down;
  });

  testInterface.setDebugHandler([](const String &setting) -> String {
    String out = "ip " + WiFi.localIP().toString() + "  rssi " + String(WiFi.RSSI()) + " dBm\n";
    out += "heap " + String(ESP.getFreeHeap() / 1024) + " KB, psram free " +
           String(ESP.getFreePsram() / 1024) + " KB\n";
    out += "uptime " + String(millis() / 1000) + " s\n";
    out += String("wifi power save ") + (WiFi.getSleep() ? "on" : "none") + "\n";
    out += "firmware " + ESP.getSketchMD5() + ", built " + String(__DATE__) + " " +
           String(__TIME__) + "\n";
    // The refresh state, because decision 11 asks for this to be checked by
    // hand and no automated test can see it: the snapshot comes from the
    // canvas, not the glass.
    // ?set=repaint=1 redraws the current screen once, partially, exactly as a
    // topic value would. It is how hil/papers3/refresh-rule.js piles up
    // partials without a project, a broker or a finger.
    if (setting == "repaint=1") renderAndPresent("test interface: repaint");
    // ?set=setupscreen=<seconds left> draws the real AP setup screen into the
    // canvas without bringing the AP up, so /snapshot.bmp can show it -
    // setup mode itself stops this server. hil/papers3/setup-screen.js reads
    // the QR code back out of it. POST /api/screen puts the project back.
    if (setting.startsWith("setupscreen="))
      wifiSetupServer->showSetupScreenAP(setting.substring(12).toInt(), false);
    out += "partials since last full refresh: " + String(panel.partialsSinceFull()) + " of " +
           String(EpdPanel::PARTIALS_BEFORE_CLEANUP) + ", last paint was " +
           String(panel.lastWasFull() ? "full" : "partial") + "\n";
    out += "last touch: " +
           (panel.touchedSinceBoot() ? String(panel.msSinceTouch()) + " ms ago" : String("never")) +
           ", clean-up after " + String(EpdPanel::IDLE_AFTER_TOUCH_MS) + " ms without one\n";
    return out;
  });

  if (creds.mqttHost.isEmpty())
  {
    Serial.println("[papers3] no broker configured - skipping MQTT");
    return;
  }

  mqttClient.configure(creds.mqttHost, (uint16_t)creds.mqttPort, creds.mqttUsername,
                       creds.mqttPassword);
  deployManager.setPublishStatusHandler(
      [](const String &id, const String &state, const String &error, int percent) {
        protocol.publishDeployStatus(id, state, error, percent);
      });
  deployManager.setClearTriggerHandler([]() { protocol.clearDeployTrigger(); });

  // Subscribed up front rather than from the connected callback: connect()
  // replays its subscription list on every reconnect, so queueing it once
  // covers every later connection too.
  mqttClient.subscribe(protocol.deviceTopic("deploy"));
  mqttClient.setConnectedCallback([]() {
    protocol.publishHello("http://" + WiFi.localIP().toString() + "/ddf.zip");
    Serial.println("[papers3] published hello");
  });
  mqttClient.setCallback([](const String &topic, const String &payload) {
    if (topic == protocol.deviceTopic("deploy"))
    {
      deployManager.handleDeployMessage(payload);
      return;
    }
    projectLoader.setTopicValue(topic, payload);
    renderPending = true;
  });
  mqttClient.connect();
  Serial.printf("[papers3] mqtt %s:%d as %s\n", creds.mqttHost.c_str(), creds.mqttPort,
                mqttClient.getClientId().c_str());
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[papers3] %s %s, DDF %s\n", DEVICE_ID, FIRMWARE_VERSION, DDF_HASH);

  if (!setupPanelAndProject()) return;
  setupNetworking();
}

void loop()
{
  M5.update();

  // Setup mode owns the device completely: it draws its own screen, it is
  // the only thing on port 80, and nothing else may run - the same shape the
  // 4.3B settled on after the normal loop kept painting the project back
  // over the setup screen.
  if (setupModeActive)
  {
    wifiSetupServer->handleClient();

    // "tap to cancel", which the screen has promised since this board first
    // drew it and which did nothing until 2026-09-14: this branch never read
    // the touch panel at all. A tap anywhere counts, but only one that
    // begins after setup mode did - the finger that just held the corner for
    // ten seconds is still on the glass, and its lift is not a tap.
    static bool setupLiftSeen = false;
    static bool setupTapDown = false;
    bool tapToCancel = false;
    if (M5.Touch.getDetail().isPressed())
    {
      if (setupLiftSeen) setupTapDown = true;
    }
    else
    {
      tapToCancel = setupTapDown;
      setupLiftSeen = true;
    }

    if (wifiSetupServer->tick() || wifiSetupServer->exitRequested() || tapToCancel)
    {
      Serial.printf("[papers3] leaving setup mode%s - restarting\n", tapToCancel ? " (tap to cancel)" : "");
      delay(200);
      ESP.restart();
    }
    return;
  }

  serviceTouch();
  testInterface.handleClient();
  mqttClient.loop();
  subscribeToProjectTopics();

  if (renderPending) renderAndPresent("topic value");

  // The quiet-moment clean-up (decision 6). The canvas still holds the screen
  // the last paint showed, so this is the same picture, painted clean.
  if (panel.cleanupDue())
  {
    panel.forceFullNext();
    renderAndPresent("idle clean-up");
  }

  heartbeat();
  delay(5);
}
