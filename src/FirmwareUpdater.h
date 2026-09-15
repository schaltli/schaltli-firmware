#pragma once
#include <Arduino.h>
#include <functional>

// Firmware updates over MQTT: screenbee/<clientId>/firmware in, progress out
// on the same deploy-status topic a project deploy uses (designer repo,
// docs/2026-09-15-firmware-ota.md, decisions 2, 5 and 8, and
// device-contract.md §4).
//
// The same shape as DeployManager on purpose - deploy is one of the building
// blocks that must be identical on every device. The designer publishes a
// retained {updateId, url, sha256, size, deviceId, build, force?}; the device
// pulls the image over HTTP straight into the app slot that is not running,
// reports downloading -> download_complete -> verifying -> applying ->
// rebooting, and restarts into it. A pull needs no route from the designer to
// the device, so it works wherever a project deploy does.
//
// Nothing is committed until everything has been checked: the image's size,
// its SHA-256 and this board's marker (FirmwareImage.h). Until Update.end()
// the running slot stays the boot slot, so every failure leaves the board on
// the firmware it had - it reports "error", puts its screen back and does not
// restart.
//
// Unlike the 4.3B's and PaperS3's deploy wiring, the message is never handled
// inside the MQTT callback. onMessage() only stores it and service() runs it
// from loop() once mqttClient.loop() has returned: the status publishes would
// otherwise happen inside PubSubClient's own packet parsing, which shares its
// buffer with them (the knob's pendingDeploy comment has the history).
class FirmwareUpdater
{
public:
  using PublishStatusFn =
      std::function<void(const String &updateId, const String &state, const String &error, int percent)>;
  using ActionFn = std::function<void()>;

  void setPublishStatusHandler(PublishStatusFn handler) { publishStatus_ = handler; }
  // Publishes an empty retained message to the firmware topic. The trigger is
  // retained so a device that was offline still gets it, which means the
  // broker would hand it back after the restart that ends a successful
  // update - and the update would run again, forever. Cleared before anything
  // else, as DeployManager does for the same reason.
  void setClearTriggerHandler(ActionFn handler) { clearTrigger_ = handler; }
  // The board's "Firmware-Update läuft" screen, drawn once before the
  // download starts. Nothing is drawn during it: on e-ink every progress step
  // would be a refresh of its own, and the designer shows the percentage.
  void setStartHandler(ActionFn handler) { onStart_ = handler; }
  // Puts the board's screen back after an update that did not happen.
  void setFailedHandler(ActionFn handler) { onFailed_ = handler; }

  // From the MQTT callback: stores the payload and nothing else.
  void onMessage(const String &payload)
  {
    pendingPayload_ = payload;
    pending_ = true;
  }

  // From loop(), after mqttClient.loop(). Blocks for the whole download when
  // there is an update to run - a firmware update is a deliberate, rare act,
  // and nothing else on the board should be running while its image is
  // written.
  void service();

private:
  void run(const String &payload);
  bool fail(const String &updateId, const String &error, bool updateBegun);

  PublishStatusFn publishStatus_;
  ActionFn clearTrigger_;
  ActionFn onStart_;
  ActionFn onFailed_;
  String pendingPayload_;
  bool pending_ = false;
  bool busy_ = false;
};
