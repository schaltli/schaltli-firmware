#include "DeviceSettings.h"

#include <Preferences.h>

namespace {
const char* NS_DEVICE = "device";
const char* KEY_DISPLAY_OFF = "dispOffS";  // NVS keys are capped at 15 chars
const char* KEY_IDLE_SCREEN = "idleScrId";
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

String idleScreenId() {
  Preferences prefs;
  if (!prefs.begin(NS_DEVICE, true)) return String();
  const String value = prefs.getString(KEY_IDLE_SCREEN, String());
  prefs.end();
  return value;
}

bool setIdleScreenId(const String& id) {
  if (id.length() > MAX_IDLE_SCREEN_ID_LENGTH) return false;
  Preferences prefs;
  if (!prefs.begin(NS_DEVICE, false)) return false;
  // An empty id is how the feature is switched off, and putString("") does
  // not reliably round-trip through NVS - remove the key instead, which
  // getString's default already reads back as empty.
  const bool ok = id.isEmpty() ? (prefs.remove(KEY_IDLE_SCREEN) || !prefs.isKey(KEY_IDLE_SCREEN))
                               : (prefs.putString(KEY_IDLE_SCREEN, id) > 0);
  prefs.end();
  return ok;
}

}  // namespace DeviceSettings
