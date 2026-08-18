# screenbee-waveshare-1v8

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

`src/main.cpp` is a deep-sleep + knob-wake power experiment, verified on
real hardware:

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

Reverse-engineered (not in Waveshare's own docs) via
`https://github.com/KrX3D/WaveShare-Knob-Esp32S3`:

- Rotary encoder: pin_a = GPIO7, pin_b = GPIO8
- LCD (ST77916, QSPI): CS=14, SCK=13, D0-D3=15/16/17/18, RST=21
- Touch (CST816, I2C): SDA=11, SCL=12, INT=9, RST=10 (haptic driver
  DRV2605 shares this I2C bus)
- Backlight: GPIO47 (through a MOSFET, active high)

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
