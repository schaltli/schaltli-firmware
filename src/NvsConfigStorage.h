#pragma once
#include "interfaces/IConfigStorage.h"
#include <Preferences.h>

// IConfigStorage backed by NVS (Preferences) rather than the M5 Dial's
// LittleFS /config.json.
//
// Deliberately NVS: this board's WiFi credential persistence and boot-time
// auto-reconnect were already built and hardware-verified against the
// "wifi" namespace before the project stack was ported, and
// TestInterfaceServer's POST /api/mqtt writes the "mqtt" namespace. Porting
// ConfigManager instead would have meant rewriting all of that to gain
// nothing - the interface is what WiFiSetupServer depends on, not the
// medium.
//
// Namespaces and keys are exactly the ones the previous on-screen setup UI
// used, so credentials saved by that UI keep working after it was replaced
// by the AP captive portal.
class NvsConfigStorage : public IConfigStorage {
public:
  bool begin() override;
  bool saveWiFiCredentials(const WiFiCredentials& creds) override;
  bool loadWiFiCredentials(WiFiCredentials& creds) override;
  bool hasWiFiCredentials() override;
  void clearWiFiCredentials() override;
};
