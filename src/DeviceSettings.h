#pragma once
#include <Arduino.h>

// Settings that belong to the *device*, not to the project it happens to be
// showing - the same class as its WiFi and MQTT credentials, and stored the
// same way, in NVS.
//
// The alternative, carrying them in the project, was considered and
// rejected on 2026-08-23: deploying a project would then silently
// reconfigure the device it landed on, including a project built for
// somewhere else entirely. A screen timeout is a property of where the
// device hangs, not of what it displays.
//
// They are also deliberately not declared in the DDF. A DDF-declared,
// typed property system was designed far enough to see what it would cost -
// a small type vocabulary the designer must render and validate, tolerance
// rules for types it does not know, a registry for portable naming - and
// all of it for one setting. The device already has a page of its own for
// exactly this kind of thing.
namespace DeviceSettings {

// Seconds of no *human* input after which the display turns off, backlight
// included. 0 means never.
//
// MQTT traffic deliberately does not count as activity. A screen showing
// values that change every few seconds would otherwise never sleep, which
// is exactly the case this exists for; and nobody is looking at a display
// because a number arrived.
uint32_t displayOffAfterSeconds();
bool setDisplayOffAfterSeconds(uint32_t seconds);

// 15s out of the box. Chosen for a device that hangs on a wall and is
// looked at deliberately, not one that is watched - a display that stays
// lit all night is the behaviour this feature exists to end, so defaulting
// it to "never" would ship the feature switched off.
static const uint32_t DEFAULT_DISPLAY_OFF_SECONDS = 15;
// An upper bound so a typo cannot mean "effectively never" while claiming
// to be a timeout; 0 is how you actually say never.
static const uint32_t MAX_DISPLAY_OFF_SECONDS = 86400;

}  // namespace DeviceSettings
