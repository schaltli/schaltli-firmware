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

`src/main.cpp` is a minimal PlatformIO smoke test: it toggles the display
backlight (GPIO47) off for 10s / on for 10s in a loop, to measure power
draw with a USB power meter. It does not initialize the LCD panel or touch
controller yet.

## Build

```
pio run -t upload
pio device monitor
```
