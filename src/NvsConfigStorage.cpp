#include "NvsConfigStorage.h"

// Two namespaces rather than one, matching what already exists on this
// board: WiFi and MQTT settings are saved independently (the setup portal
// can save one without having the other), and POST /api/mqtt writes only
// the second.
static const char* NS_WIFI = "wifi";
static const char* NS_MQTT = "mqtt";

bool NvsConfigStorage::begin() {
  // NVS is initialised by the Arduino core before setup() runs; each
  // Preferences call opens and closes its own handle, so there is nothing
  // to do here. Kept because the interface requires it.
  return true;
}

bool NvsConfigStorage::saveWiFiCredentials(const WiFiCredentials& creds) {
  Preferences prefs;
  if (!prefs.begin(NS_WIFI, false)) return false;
  prefs.putString("ssid", creds.ssid);
  prefs.putString("pass", creds.password);
  prefs.end();

  if (!prefs.begin(NS_MQTT, false)) return false;
  prefs.putString("protocol", creds.mqttProtocol);
  prefs.putString("host", creds.mqttHost);
  // Stored as a string because that is what the previous setup UI wrote and
  // what POST /api/mqtt still writes - changing the type here would
  // silently orphan settings already on devices.
  prefs.putString("port", String(creds.mqttPort));
  prefs.putString("user", creds.mqttUsername);
  prefs.putString("pass", creds.mqttPassword);
  prefs.end();
  return true;
}

bool NvsConfigStorage::loadWiFiCredentials(WiFiCredentials& creds) {
  Preferences prefs;
  if (!prefs.begin(NS_WIFI, true)) return false;
  creds.ssid = prefs.getString("ssid", "");
  creds.password = prefs.getString("pass", "");
  prefs.end();

  if (prefs.begin(NS_MQTT, true)) {
    creds.mqttProtocol = prefs.getString("protocol", "mqtt://");
    creds.mqttHost = prefs.getString("host", "");
    int port = prefs.getString("port", "1883").toInt();
    creds.mqttPort = port > 0 ? port : 1883;
    creds.mqttUsername = prefs.getString("user", "");
    creds.mqttPassword = prefs.getString("pass", "");
    prefs.end();
  }

  // "Loaded" means there is a network to try, mirroring ConfigManager's own
  // contract - MQTT settings are optional and their absence is not failure.
  return !creds.ssid.isEmpty();
}

bool NvsConfigStorage::hasWiFiCredentials() {
  Preferences prefs;
  if (!prefs.begin(NS_WIFI, true)) return false;
  bool has = prefs.getString("ssid", "").length() > 0;
  prefs.end();
  return has;
}

void NvsConfigStorage::clearWiFiCredentials() {
  Preferences prefs;
  if (prefs.begin(NS_WIFI, false)) {
    prefs.clear();
    prefs.end();
  }
}
