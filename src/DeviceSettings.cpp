#include "DeviceSettings.h"

#include <Preferences.h>

namespace {
const char* NS_DEVICE = "device";
const char* KEY_DISPLAY_OFF = "dispOffS";  // NVS keys are capped at 15 chars
}  // namespace

namespace DeviceSettings {

uint32_t displayOffAfterSeconds() {
  Preferences prefs;
  // Read-only: opening a namespace read-write creates it, and a getter
  // should not leave a mark on storage just by being called.
  if (!prefs.begin(NS_DEVICE, true)) return DEFAULT_DISPLAY_OFF_SECONDS;
  const uint32_t value = prefs.getUInt(KEY_DISPLAY_OFF, DEFAULT_DISPLAY_OFF_SECONDS);
  prefs.end();
  return value > MAX_DISPLAY_OFF_SECONDS ? MAX_DISPLAY_OFF_SECONDS : value;
}

bool setDisplayOffAfterSeconds(uint32_t seconds) {
  if (seconds > MAX_DISPLAY_OFF_SECONDS) return false;
  Preferences prefs;
  if (!prefs.begin(NS_DEVICE, false)) return false;
  const bool ok = prefs.putUInt(KEY_DISPLAY_OFF, seconds) > 0;
  prefs.end();
  return ok;
}

}  // namespace DeviceSettings
