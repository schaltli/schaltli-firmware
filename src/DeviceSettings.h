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

// The screen the device shows at boot, and the one it returns to when the
// display goes dark. Empty means neither happens: boot renders screen 0 and
// blanking leaves whatever was on screen, which is what every device did
// before this existed.
//
// The point is not convenience, it is which screen a person finds when they
// walk up. A panel that switches heating and air conditioning should not sit
// waiting on those; it should sit on the light, where a mistaken touch costs
// nothing, and the critical screens should have to be navigated to
// deliberately. Leaving the device wherever the last visitor left it makes
// that a matter of luck.
//
// Stored as the project's own screen *id*, not an index. An index survives
// nothing: reorder the screens in the designer, redeploy, and the device
// would return to a different screen while still reporting the same setting -
// silently landing on the critical page this feature exists to keep it off.
// An id that a redeploy removes resolves to nothing, and nothing is the safe
// answer: the feature switches itself off rather than guessing.
String idleScreenId();
bool setIdleScreenId(const String& id);

// NVS values are not unbounded and neither are designer screen ids; a value
// longer than this is a caller error rather than something to truncate.
static const size_t MAX_IDLE_SCREEN_ID_LENGTH = 64;

}  // namespace DeviceSettings
