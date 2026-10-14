#pragma once
#include <Arduino.h>
#include "MqttClient.h"

// What a designer asks a device for over the broker, and the device's
// answers (docs/device-contract.md §4, 2026-10-14, schaltli-designer#70):
//
//   ddf/get       -> ddf       the device's description, retained
//   recovery/get  -> recovery  the project kept for recovery, not retained;
//                              empty when there is none
//
// Until then the designer fetched both over HTTP from the device's own
// server, which a designer in a VM behind NAT cannot reach and which left a
// door open on every device that no broker rule covered.
//
// A request only sets a flag: publishing from inside the MQTT callback
// corrupts the packet PubSubClient is still parsing (contract §4,
// "Implementation gotchas" 2). service() answers from the loop.
class DeviceRequests {
public:
  explicit DeviceRequests(MqttClient &client) : client_(client) {}

  // Once, before the first connect: the client replays its subscriptions
  // on every reconnect.
  void subscribe();

  // From the message callback; true when the topic was one of these.
  bool onMessage(const String &topic);

  // From loop().
  void service();

private:
  String topic(const char *leaf) const;
  bool publishDdf();
  bool publishRecovery();

  MqttClient &client_;
  bool ddfAsked_ = false;
  bool recoveryAsked_ = false;
};
