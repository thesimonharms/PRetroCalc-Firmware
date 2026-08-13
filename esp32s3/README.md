# PRetroCalc OS — ESP32-S3-Pico HAL

This directory is the **main** firmware target: a **Waveshare ESP32-S3-Pico**
plugged into a **ClockworkPi PicoCalc** (same LCD + STM32 keyboard + SD +
speakers).

The Pico 2 W / RP2350 port is on the `pico2w` branch. It was the original
target, but the RP2350 is not fast or strong enough for this OS.

See the [root README](../README.md) for features, CHAT.CFG, PicoScript, and
usage. This file is the ESP32 HAL / flash notes.

## Hardware notes

| Peripheral | Interface | ESP32 GPIO | Pico GP |
|---|---|---|---|
| LCD ILI9488 320×320 | SPI2 @ 40 MHz | SCK 35, MOSI 36, MISO 37, CS 38, DC 39, RST 40 | 10–15 |
| Keyboard STM32 BIOS | I2C @ 0x1F | SDA 17, SCL 18 | 6–7 |
| Speakers | LEDC PWM | L 7, R 8 | 26–27 |
| SD card | SPI3/HSPI | SCLK 1, MOSI 2, MISO 42, CS 41 | 18,19,16,17 |

**PSRAM must stay disabled.** Pico GP10–12 map to GPIO35–37, which are the
onboard Octal PSRAM pins on the Waveshare module. The double RGB332
framebuffer of the Pico build is reduced to a **single** 96 KB buffer so
everything fits in internal SRAM with WiFi.

Flash layout (`partitions.csv`): 4 MB firmware, 8 MB `romcache` partition
used by **EMU** so Game Boy ROMs larger than SRAM can still run (copied
from the SD card once, then memory-mapped).

ESP32-S3 @ 240 MHz, 16 MB flash, Arduino-ESP32 WiFi STA (plain HTTP, no TLS).

## Build

Requires [PlatformIO](https://platformio.org/) (this tree uses the Arduino-ESP32
core under the hood):

```sh
cd esp32s3
python -m platformio run
# firmware: .pio/build/esp32s3-pico/firmware.bin
```

Flash (hold **BOOT**, plug USB-C, release after connect if needed):

```sh
python -m platformio run -t upload
python -m platformio device monitor
```

Pick the CH343 COM port if PlatformIO does not auto-detect it:
`python -m platformio run -t upload --upload-port COMx`.

## Layout

```
esp32s3/
  platformio.ini      PlatformIO env
  extra_script.py     pulls shared apps from ../src
  include/            ESP32 board.h + Pico API shims
  src/                HAL: gfx, keyboard, sound, SD, WiFi, main
../src/               shared OS / apps / Jawa font (compiled in)
```
