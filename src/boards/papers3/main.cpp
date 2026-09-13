// M5Stack PaperS3 - board bring-up.
//
// Third board in this repo, started 2026-09-13. The decisions it is being
// built to are in the designer repo's docs/2026-09-13-papers3-grilling.md,
// cited by number where they bite.
//
// What is here: the panel, the project stack above it, and the test
// interface that lets a conformance run compare this device against the
// designer pixel for pixel. What is deliberately not here yet: touch, and
// therefore the setup-mode gesture, Switch and SoftwareButton. Those are the
// next increment - wiring them at the same time as everything else would
// give a first failure six possible causes instead of one.
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
                (unsigned)panel.partialsSinceFull(), (unsigned)EpdPanel::PARTIALS_BEFORE_FULL);
}

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

  testInterface.setDebugHandler([](const String &) -> String {
    String out = "ip " + WiFi.localIP().toString() + "  rssi " + String(WiFi.RSSI()) + " dBm\n";
    out += "heap " + String(ESP.getFreeHeap() / 1024) + " KB, psram free " +
           String(ESP.getFreePsram() / 1024) + " KB\n";
    out += "uptime " + String(millis() / 1000) + " s\n";
    out += "firmware " + ESP.getSketchMD5() + ", built " + String(__DATE__) + " " +
           String(__TIME__) + "\n";
    // The refresh state, because decision 11 asks for this to be checked by
    // hand and no automated test can see it: the snapshot comes from the
    // canvas, not the glass.
    out += "partials since last full refresh: " + String(panel.partialsSinceFull()) + " of " +
           String(EpdPanel::PARTIALS_BEFORE_FULL) + ", last paint was " +
           String(panel.lastWasFull() ? "full" : "partial") + "\n";
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
    if (wifiSetupServer->tick() || wifiSetupServer->exitRequested())
    {
      Serial.println("[papers3] leaving setup mode - restarting");
      delay(200);
      ESP.restart();
    }
    return;
  }

  testInterface.handleClient();
  mqttClient.loop();
  subscribeToProjectTopics();

  if (renderPending) renderAndPresent("topic value");

  heartbeat();
  delay(5);
}
