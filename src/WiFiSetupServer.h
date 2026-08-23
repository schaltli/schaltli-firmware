#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <PubSubClient.h>
#include "interfaces/IDisplay.h"
#include "interfaces/IConfigStorage.h"

// Lean counterpart of MqttEPaperDisplay2's UnifiedConfigurator - WiFi +
// MQTT setup only, no project upload (that's checkpoint 4's job, tied to
// a ProjectInstaller that doesn't exist for this device yet - see the
// project history for why this was deliberately split rather than
// porting the full dual-purpose class now). Same AP/STA auto-detect
// behavior, same /api/scan, /api/wifi, /api/mqtt, /api/mqtt-test routes.
class WiFiSetupServer {
public:
  WiFiSetupServer(IDisplay& display, IConfigStorage& config);
  ~WiFiSetupServer();

  // Auto-detects: STA mode (web server on the existing connection) if
  // WiFi is already connected, AP mode otherwise.
  bool start();
  // Force AP mode - for a long button-press while already connected.
  bool startAP();
  void stop();

  // True once someone pressed Cancel in the portal. Latching, not a
  // callback: the caller decides what leaving setup mode means.
  bool exitRequested() const { return exitRequested_; }
  bool isRunning() const { return running_; }
  void handleClient();
  bool hasWiFiCredentials() const { return !configuredSSID_.isEmpty(); }

  // Call every loop() iteration while in AP setup mode - updates the
  // on-screen "time left before auto-reset" / "client connected" status
  // (throttled to redraw only when the displayed value actually changes,
  // not every call) and returns true once 2 minutes have elapsed with no
  // client ever having connected, meaning the caller should ESP.restart()
  // itself (this class only reports the condition, matching main.cpp's
  // existing convention of deciding restarts itself - see its
  // hasWiFiCredentials()-triggered restart for the identical split).
  // No-op (always returns false) outside AP mode - nothing to count down
  // against once already on the home network.
  bool tick();

private:
  IDisplay& display_;
  // Answers every DNS query with the AP's own address while the AP is up.
  // Without it a phone joins, fails its connectivity check (Android asks
  // for connectivitycheck.gstatic.com, iOS for captive.apple.com), decides
  // this network leads nowhere and routes traffic to mobile data instead -
  // so 192.168.4.1 becomes unreachable from the very device that just
  // joined. Observed exactly that way on 2026-08-23: the server was
  // running and answering fine over the station interface the whole time.
  DNSServer* dnsServer_ = nullptr;
  // Set by the portal's Cancel button. Read and acted on by the main loop
  // rather than restarting inside the request handler, so the browser gets
  // its response before the device goes away - a restart mid-request looks
  // to the user like the page crashed.
  bool exitRequested_ = false;
  IConfigStorage& config_;
  WebServer* webServer_;
  bool running_;
  bool apMode_;

  String configuredSSID_;

  // AP-mode "auto-reset if nobody connects" countdown state - see tick().
  unsigned long apEnteredAtMs_ = 0;
  int lastRenderedCountdownState_ = -1;  // -1 = nothing rendered yet, -2 = "connected" already rendered, else seconds left last shown

  static const char* AP_SSID;
  static const char* AP_PASSWORD;
  static const IPAddress AP_IP;
  static const IPAddress AP_GATEWAY;
  static const IPAddress AP_SUBNET;
  static const unsigned long AP_AUTO_RESET_MS;

  void handleRoot();
  void handleWiFiScan();
  void handleWiFiConfigure();
  void handleMqttConfigure();
  void handleMqttTest();
  void handleNotFound();

  String scanNetworks();
  bool saveWiFiCredentials(const String& ssid, const String& password);
  bool saveMqttConfig(const String& protocol, const String& host, int port, const String& username, const String& password);
  String testMqttConnection(const String& protocol, const String& host, int port, const String& username, const String& password);

  // secondsLeft/clientConnected only affect the AP-mode screen's extra
  // status line - see tick()'s comment for the states they represent.
  void showSetupScreenAP(int secondsLeft = -1, bool clientConnected = false);
  void showSetupScreenSTA();
  void sendJSONResponse(bool success, const String& message);
};
