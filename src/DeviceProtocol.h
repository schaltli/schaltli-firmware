#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "MqttClient.h"
#include "DeviceInfo.h"

// The MQTT half of the device contract: the topics under
// screenbee/<clientId>/ that every ScreenBee device speaks, and the exact
// payload shapes docs/device-contract.md §4 specifies.
//
// Extracted rather than written a second time. These four functions are pure
// protocol - no board, no display, no pins - and the 4.3B port needed all of
// them verbatim. Copying them would have been the first step of the thing
// that already hurts here once: the LittleFS orphan leak, fixed in one
// firmware and still open in two others because the code existed three
// times.
//
// The knob's main.cpp still carries its own copies. Deliberately left alone:
// removing them means editing a verified firmware whose HIL cannot be run
// right now, and a silent divergence is worse than a visible duplication
// with a note on it. It should adopt this file as soon as that device is
// reachable again.
// Takes MqttClient rather than IMessagingClient because the client id is
// part of every topic here and only the concrete class exposes it. Widening
// the interface for that would have reached into three firmwares to serve
// one caller, and this class is explicitly the MQTT half of the contract
// rather than a transport-neutral thing.
class DeviceProtocol {
public:
  explicit DeviceProtocol(MqttClient &client) : client_(client) {}

  String deviceTopic(const char *leaf) const
  {
    return String(TOPIC_PREFIX) + "/" + client_.getClientId() + "/" + leaf;
  }

  // ddfHash and url are what make a device appear under "Announced Devices"
  // at all - without them the designer treats it as firmware that publishes
  // no DDF and skips it. The hash is the identity of the bytes served at
  // url, and the designer verifies one against the other: on a mismatch the
  // fetch is refused rather than cached, so an announcement that lies is
  // visible instead of quietly poisoning the designer's copy.
  //
  // ddfUrl is passed in rather than built here so this file needs no WiFi
  // and no web server - a device that cannot serve its own DDF passes an
  // empty string and is simply skipped by auto-discovery, which is the
  // documented behaviour rather than a failure.
  void publishHello(const String &ddfUrl)
  {
    JsonDocument doc;
    doc["deviceId"] = DEVICE_ID;
    doc["firmwareVersion"] = FIRMWARE_VERSION;
    // The build's identity (DeviceInfo.h), which the designer compares with
    // the firmware its release carries to say whether a newer one exists.
    doc["firmwareBuild"] = FIRMWARE_BUILD;
    // What this firmware can read, as one major.minor. The designer's deploy
    // dialog checks it *before* uploading, so a device on an older major
    // says so in the dialog rather than refusing the download afterwards,
    // where the only evidence is a serial log nobody is watching.
    doc["systemGeneration"] = String(SYSTEM_GENERATION_MAJOR) + "." + String(SYSTEM_GENERATION_MINOR);
    doc["ddfHash"] = DDF_HASH;
    if (!ddfUrl.isEmpty()) doc["url"] = ddfUrl;
    String payload;
    serializeJson(doc, payload);
    client_.publish(deviceTopic("hello"), payload, true);
  }

  void publishDeployStatus(const String &deployId, const String &state, const String &error, int percent)
  {
    JsonDocument doc;
    doc["deployId"] = deployId;
    doc["state"] = state;
    if (!error.isEmpty()) doc["error"] = error;
    // Optional per the contract, but the deploy dialog reads
    // `status.percent ?? 0` - never publishing one leaves its progress bar
    // visibly stuck at 0 for the whole deploy even when it works.
    if (percent >= 0) doc["percent"] = percent;
    String payload;
    serializeJson(doc, payload);
    client_.publish(deviceTopic("deploy-status"), payload, false);
  }

  // The deploy topic is retained, so the broker redelivers the last trigger
  // on every fresh subscribe - including the one after the reboot that ends
  // a successful deploy. Without clearing it, a deploy loops forever. This
  // has to be the first thing a deploy handler does, before the busy check
  // and before any download.
  void clearDeployTrigger() { client_.publish(deviceTopic("deploy"), "", true); }

  // The firmware trigger is retained, and loops the same way if it is not
  // cleared (FirmwareUpdater.h).
  void clearFirmwareTrigger() { client_.publish(deviceTopic("firmware"), "", true); }

private:
  MqttClient &client_;
};
