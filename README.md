# schaltli-firmware

Firmware for the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** — one of the two
PSRAM-equipped boards evaluated as a successor to the M5 Dial
(`screenbee-m5dial`), which was retired due to its 320KB-SRAM/no-PSRAM
memory ceiling.

## Board

- ESP32-S3, dual-core, 240MHz
- 512KB SRAM, 8MB octal PSRAM, 16MB flash
- 1.8" round LCD, 360×360, ST77916 driver (QSPI)
- CST816 capacitive touch (I2C)
- Physical rotary knob
- Companion ESP32 MCU (audio/vibration) — not used yet

## Current state

`src/main.cpp` is a first LVGL bring-up: a WiFi setup screen (scanned-SSID
dropdown, a **plaintext** password field, Connect button) rendered via
Arduino_GFX + LVGL v8, with working capacitive touch input. Verified on
real hardware: dropdown selection, and password entry (cursor positioning
by tap, on-screen keyboard opened by a longer press) all work through
actual touch, with the keyboard resized/repositioned to stay fully inside
the round bezel - see below for how.

On a successful WiFi connect, credentials are saved to NVS
(`Preferences`, namespace `"wifi"`) and auto-tried on every boot
(`WiFi.begin()` with an 8s timeout in `setup()`, before LVGL/display init)
so the device reconnects without any UI interaction after the first setup.
Once connected (whether by auto-reconnect or manual setup), the dropdown/
password/Connect UI is replaced by a compact "connected" header (a green
checkmark + SSID + a small edit button) and an MQTT broker config section
appears below it - protocol/host/port on one flex row, then
username/password fields (icon-prefixed placeholders, `LV_SYMBOL_LIST` /
`LV_SYMBOL_EYE_OPEN`, both visible-text like the WiFi password field) and
a Save button, all persisted to NVS (`Preferences`, namespace `"mqtt"`).
Tapping the edit button re-enters WiFi setup mode: re-scans, repopulates
the SSID dropdown, reselects the current network if still in range, and
restores the previously-entered password into the field instead of
leaving it blank - editing is meant to change to a *different* network,
not retype one that's already connected. This mirrors the MQTT config
fields from `screenbee-m5dial`'s HTML AP-setup interface, but rendered
natively via LVGL instead of a captive-portal web page. MQTT *client*
connection itself (actually connecting/testing the broker) is not
implemented yet - only the config UI and NVS storage.

This superseded an earlier deep-sleep + knob-wake power experiment (see
"Deep sleep / knob wake" below) - that one is no longer what's flashed,
but its findings (EXT0 wake works, ~50mA floor) remain valid and are kept
as reference for whenever power-management work resumes.

### LVGL bring-up gotchas (all found and fixed the hard way)

- **Wrong ST77916 init table gave a striped/garbled image at any clock
  speed** (tried 40/20/8MHz, no difference). Arduino_GFX ships two init
  operand tables, `st77916_180_init_operations` (the constructor default)
  and `st77916_150_init_operations` - this panel needs the **150** variant.
  The `180`/`150` naming is not display-diagonal-inches, just two
  different factory gamma/voltage calibrations; there's no way to know
  which one a given panel needs except trying both.
- **A stale `rtc_gpio_hold_en()` from the earlier deep-sleep experiment
  latched LCD_RST/TOUCH_RST low permanently**, surviving normal
  resets/reflashes (RTC pad holds are a hardware latch, not cleared by
  soft reset). Call `rtc_gpio_hold_dis()` on both pins (and
  `gpio_deep_sleep_hold_dis()`) at the very top of `setup()` before
  touching the display - otherwise it's stuck in hardware reset and no
  amount of driver-level debugging will explain why.
- **CST816 touch protocol**: fetched Waveshare's own official demo zip
  (`https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip`,
  `Arduino/examples/08_LVGL_Test/cst816.cpp`) since neither datasheet
  register-map assumptions nor several standard fixes (repeated-start vs.
  stop, reset pulse ordering, disabling auto-sleep) got real data out of
  the chip. Two things the working demo does differently from every
  guess: **no reset pin is ever driven** (GPIO10/TOUCH_RST doesn't need
  toggling at all), and it writes `0x00` to register `0x00` once at init
  ("switch to normal mode") - without that write the chip ACKs I2C and
  returns a fixed all-zero 7-byte block forever, touched or not. Read 7
  bytes from register `0x00`. **Corrected 2026-08-20**: this section
  originally claimed `data[2]` ("finger count") sat at 0 during drags and
  keyed "touched" off `buf[1]` instead - that was wrong, an artifact of
  that day's reset/register experiments, and it is exactly why swipe
  gestures never worked. See "CST816 touch protocol, settled by probe"
  below for what the chip actually reports.
- **`LV_TICK_CUSTOM` (LVGL calling Arduino's `millis()` from a `.c`
  translation unit via `LV_TICK_CUSTOM_INCLUDE "Arduino.h"`) silently
  never worked** in this arduino-esp32/LVGL v8.4 combination -
  `lv_tick_get()` returned a constant `0` forever, even after a full
  `pio run -t clean` + rebuild. Since LVGL believed no time ever passed,
  **no periodic timer ever ran past the very first paint** - not
  animations, not indev polling, nothing - which is exactly why touch
  input looked "unresponsive" even once the CST816 driver itself was
  proven correct via direct polling. Fixed by setting `LV_TICK_CUSTOM 0`
  in `include/lv_conf.h` and calling `lv_tick_inc(5)` once per `loop()`
  iteration instead (the standard, more robust pattern for Arduino
  targets anyway).
- **PlatformIO doesn't track `include/lv_conf.h` as a dependency for
  library sources.** Editing it and re-running `pio run -t upload` only
  recompiles `src/main.cpp` - the LVGL `.c` files silently keep using
  whatever config they were last compiled with. **Always run
  `pio run -t clean` after any `lv_conf.h` change** before rebuilding, or
  the new setting won't actually take effect (this is exactly how the
  `LV_TICK_CUSTOM` fix above needed two rebuild rounds to actually land).

### Password entry: on-screen keyboard, resized and repositioned for the round bezel

`lv_keyboard`'s default rectangular button grid extends into the square
framebuffer's corners, which are outside the physically visible round
area - the keyboard's outer keys get clipped by the bezel. Two approaches
were tried:

1. **Custom knob-driven character picker** (rotate an `lv_roller` through
   all 94 likely password characters, tap Insert/Backspace/Done). Worked,
   but the user's verdict after trying it: tapping individual letters on
   the standard keyboard is simply faster than rotating through a long
   character list one step at a time - reverted.
2. **Resized + repositioned standard keyboard** (what's actually flashed
   now): `lv_keyboard` sized to 260x150 and centered 45px below screen
   center - its farthest corner sits ~177px from center, just inside the
   180px bezel radius, so no keys get clipped. When it opens, the password
   textarea is moved to `LV_ALIGN_CENTER, 0, -95` (and raised to the front
   via `lv_obj_move_foreground`) so it's visible in the space above the
   keys instead of being covered by the keyboard's normal-layout position;
   it moves back to its resting spot (`LV_ALIGN_TOP_MID, 0, 130`) when the
   keyboard closes.

Two more fixes were needed to make opening/closing the keyboard itself
reliable:

- **Tap-to-reopen**: `LV_EVENT_FOCUSED` only fires once per focus session,
  so after manually closing the keyboard the field stayed logically
  focused and a second tap did nothing. Fixed by opening on
  `LV_EVENT_LONG_PRESSED` instead of `FOCUSED` - deliberately *not*
  `CLICKED`/every tap, since `lv_textarea` already moves the cursor to the
  tapped character on a plain short tap by itself, so a quick tap can be
  used purely to position the cursor without the keyboard jumping in and
  covering the screen; a held tap is the explicit "start editing" gesture.
- **Long-press never fired**: measured real presses on this touch panel
  released around ~225ms - always under LVGL's default 400ms
  `long_press_time`, so `LV_EVENT_LONG_PRESSED` was never reached and nothing
  visibly happened when pressing, no matter how the field was touched.
  Fixed by setting `indev_drv.long_press_time = 200` (in `setup()`, after
  `lv_indev_drv_init()`) - comfortably under a deliberate hold, still well
  above a quick cursor-positioning tap. Diagnosed by logging every
  `lv_event_code_t` the field received (see git history) - the sequence
  was `PRESSED → PRESSING×N → RELEASED → SHORT_CLICKED`, never
  `LONG_PRESSED`, which pinpointed the timing threshold as the actual
  cause rather than a touch or hit-testing bug.

### Small-widget LVGL gotchas found while building the connected/MQTT UI

- **Flex-child fields can't just use `lv_obj_align()` to pop out for
  editing.** The generic long-press-to-edit handler used for the WiFi
  password field (`fieldKeyboardCb`, aligns the field to screen center
  while the keyboard is open) breaks for `mqttHostArea`/`mqttPortArea`,
  which are children of a `LV_FLEX_FLOW_ROW` container (`mqttRow`) -
  `lv_obj_align()` fights the flex layout engine and the field stays
  clipped to its small auto-sized slot. Fixed with a separate callback
  (`mqttRowFieldKeyboardCb`) that calls `lv_obj_set_parent()` to reparent
  the field to `lv_scr_act()` for editing and back to `mqttRow` on close,
  instead of aligning it in place.
- **Default button theme padding can eat a small button's icon glyph
  entirely.** A 30x30 `lv_btn` with an `LV_SYMBOL_EDIT` label looked
  visually clipped - not because of the round bezel, but because LVGL's
  default button theme padding left too little interior room for the
  glyph to render at full size. Fixed by bumping the button to 34x34,
  adding `lv_obj_set_style_pad_all(btn, 2, 0)` to shrink the theme's
  default padding, and explicitly `lv_obj_center()`-ing the label inside
  it rather than relying on the button's automatic content layout.

### Deep sleep / knob wake (earlier experiment, findings kept for later)

- Backlight (GPIO47) off, LCD reset (GPIO21) and touch reset (GPIO10)
  held low through deep sleep via `rtc_gpio_hold_en()`.
- Wakes via EXT0 on GPIO7 (rotary encoder pin_a, idle HIGH, pulses LOW on
  rotation) - confirmed: rotating the knob reliably wakes the chip from
  deep sleep.
- On wake, flashes the backlight for 5s and prints the wakeup cause over
  serial, then goes back to sleep.

Measured with a USB power meter: ~150mA backlight on, ~80mA backlight off
(still awake), ~50mA in deep sleep. Holding the LCD/touch reset lines low
made no measurable difference to that 50mA floor - per the schematic
(`https://github.com/KrX3D/WaveShare-Knob-Esp32S3`), the haptic driver's
EN pin is hardwired to 3V3 (not GPIO-controlled), and the companion ESP32
(audio/vibration co-processor) has no power-gate reachable from the S3's
GPIOs - its own EN pin only wires to the CH340 auto-reset circuit used to
program it. ~50mA is treated as the practical deep-sleep floor for this
board unless the companion ESP32 is given its own sleep-aware firmware
(out of scope for now).

## Encoder / pin reference

**The knob is NOT a quadrature encoder.** It is a bidirectional pulse
encoder: turning right emits debounced LOW pulses on GPIO8 only, turning
left on GPIO7 only; both idle HIGH and the opposite channel stays HIGH
(bar sub-100µs crosstalk bounce at the pulse's rising edge). One pulse =
one detent; measured pulse widths 38-324ms at hand speed. Authoritative
source: Waveshare's own demo
(`files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip`,
`Arduino/examples/04_Encoder_Test/bidi_switch_knob.c` - a *modified*
espressif iot_knob whose whole modification is decoding the channels
independently; `user_config.h` names ECA=GPIO8 -> right, ECB=GPIO7 ->
left). `RotaryEncoderReader` replicates that decode (3ms esp_timer poll,
2-tick debounce). A Gray-code quadrature decoder ported from the M5 Dial
ran here first and produced only +1/-1 jitter - fed a single toggling
channel, that is all a quadrature state machine can emit. Verified
end-to-end on hardware 2026-08-20: 5 physical right + 3 left detents
counted exactly (`GET /api/debug`, `dispatchedRight`/`dispatchedLeft`).

Pins (vendor demo + `KrX3D/WaveShare-Knob-Esp32S3`):

- Rotary encoder: GPIO8 = right pulses, GPIO7 = left pulses (idle HIGH)
- LCD (ST77916, QSPI): CS=14, SCK=13, D0-D3=15/16/17/18, RST=21
- Touch (CST816, I2C): SDA=11, SCL=12, INT=9, RST=10 (haptic driver
  DRV2605 shares this I2C bus)
- Backlight: GPIO47 (through a MOSFET, active high)

**Serial monitoring is not a neutral observer on this board.** Every
"crash while turning the knob" during input bring-up was `rst:0x15,
USB_UART_CHIP_RESET` - a reset issued by the S3's native USB peripheral,
which interprets host DTR/RTS line changes as reset requests. With no COM
port open the same firmware ran through the whole verification without a
single reset. Diagnose input hardware over `GET /api/debug` (uptime as
reboot detector, raw edge recorder, raw touch probe, dispatched-detent
counters), not over a serial monitor.

## CST816 touch protocol, settled by probe

Settled 2026-08-20 by recording every raw 7-byte read during real
gestures (the `touchProbe` array in `GET /api/debug`), after swipes
produced 2 samples per drag under the earlier `buf[1]`-based driver:

- **`data[2]` (finger count) is the "touched" criterion** - exactly what
  the official demo uses. It holds 1 through an entire drag with live
  coordinates at 7ms polling, stationary holds included.
- **`data[1]` (gesture id) is NOT a touch indicator.** During a drag it
  is mostly 0 (it pulses only when the chip's own gesture engine fires a
  code), and after the finger lifts it **latches the last gesture code
  (5 = click) for ~450ms with frozen coordinates** while `data[2]` is
  already 0. Keying "touched" off it gives phantom releases mid-drag and
  a phantom stationary touch after release.
- **The chip has scan gaps**: occasional all-zero reads (sometimes
  >100ms) in the middle of an active drag. The gesture detector therefore
  only accepts a release after 80ms of sustained idle readings
  (`RELEASE_DEBOUNCE_MS`), and measures gesture duration to the last
  down-read so the debounce tail doesn't inflate it.
- Polling rate is NOT a factor: solid tracking at 7ms, no need to match
  LVGL's 30ms cadence. (`GET /api/debug?set=touchPeriodMs=N` adjusts the
  sample period at runtime if this ever needs re-testing.)

Hardware-verified same day: repeated swipe-ups reliably open the screen
menu, taps select tablets. The swipe->menu->tap chain needs a human
finger; the dispatch half (`swipe-up` -> `showScreenMenu` -> overlay
render) is covered automatably via `POST /api/input`, which is how the
two halves were isolated in the first place.

## USB / dual-MCU quirk

The single USB-C port is shared between the main ESP32-S3 and a companion
classic ESP32 (audio/vibration co-processor) via an analog switch. Which
chip gets the USB connection depends on **which way round the USB-C cable
is plugged in** - flip it 180° if `esptool` reports the wrong chip (e.g.
"This chip is ESP32, not ESP32-S3" means you're talking to the companion
chip). The S3 enumerates natively as `VID:PID=303A:1001`; the companion
chip shows up as a CH340 (`VID:PID=1A86:7523`) COM port. There's also a
physical slide switch on the board, but that's just power on/off, not MCU
selection.

The board ships in an aluminum case with no reachable BOOT button, but
none is needed - the S3's native USB auto-resets into the bootloader via
`esptool` as long as the cable orientation and power switch are both
right. Deep sleep drops the S3's native USB, so the OS-visible COM port
disappears while asleep and reappears on wake/reset.

## Build

```
pio run -t upload
pio device monitor
```

## License

The code in this repository is licensed under the GNU Affero General Public
License, version 3 or (at your option) any later version - see
[LICENSE](LICENSE).

The name "Schaltli" and the Schaltli mark are not covered by that license. A
fork may use the code, but not present itself as Schaltli.
