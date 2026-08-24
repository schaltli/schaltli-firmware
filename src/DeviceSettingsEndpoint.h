#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include "DeviceSettings.h"

// The /api/device-settings request handling itself, shared by the two web
// servers that expose it - WiFiSetupServer in setup mode, TestInterfaceServer
// in normal operation. They are separate classes owning separate WebServer
// instances, so the only thing they can share is a free function over one.
//
// Shared for the same reason the form markup is (DeviceSettingsHTML.h): two
// copies of "which fields exist, what the bounds are, what an absent field
// means" would drift, and the half that drifted would be the one reached in
// whichever mode nobody was testing.
namespace DeviceSettingsEndpoint {

// GET: current values plus the screen list the "return to" dropdown needs.
// `screensJson` is a ready-made JSON array of {"id","name"} - the servers
// have no business knowing what a project is, so they get handed it.
inline String currentJson(const String& screensJson) {
  JsonDocument doc;
  doc["displayOffAfterSeconds"] = DeviceSettings::displayOffAfterSeconds();
  doc["idleScreenId"] = DeviceSettings::idleScreenId();
  doc["screens"] = serialized(screensJson.isEmpty() ? String("[]") : screensJson);
  String output;
  serializeJson(doc, output);
  return output;
}

// POST. Returns false and fills `messageOut` with the reason on a rejected
// or failed save; true and a short confirmation otherwise.
//
// Every field is optional and is written only when the request actually
// carried it: a caller that knows about one setting must not silently clear
// another by not mentioning it. The HIL suites post displayOffAfterSeconds
// alone all the time, and doing that must not wipe the screen choice.
inline bool save(WebServer& server, String& messageOut) {
  bool touchedAnything = false;

  if (server.hasArg("displayOffAfterSeconds")) {
    const long value = server.arg("displayOffAfterSeconds").toInt();
    if (value < 0 || (unsigned long)value > DeviceSettings::MAX_DISPLAY_OFF_SECONDS) {
      messageOut = "Out of range (0 - 86400)";
      return false;
    }
    if (!DeviceSettings::setDisplayOffAfterSeconds((uint32_t)value)) {
      messageOut = "Could not save";
      return false;
    }
    touchedAnything = true;
    messageOut = value == 0 ? "Saved - display stays on" : "Saved";
  }

  if (server.hasArg("idleScreenId")) {
    const String id = server.arg("idleScreenId");
    if (id.length() > DeviceSettings::MAX_IDLE_SCREEN_ID_LENGTH) {
      messageOut = "Screen id too long";
      return false;
    }
    if (!DeviceSettings::setIdleScreenId(id)) {
      messageOut = "Could not save the screen to return to";
      return false;
    }
    touchedAnything = true;
    if (messageOut.isEmpty()) messageOut = "Saved";
  }

  if (!touchedAnything) {
    // Not a silent success: a request naming no known field is a caller
    // bug, and answering "Saved" to it would hide the typo it almost
    // certainly is.
    messageOut = "Nothing to save - expected displayOffAfterSeconds and/or idleScreenId";
    return false;
  }
  return true;
}

}  // namespace DeviceSettingsEndpoint
