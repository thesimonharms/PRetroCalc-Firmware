# PRetroCalc OS

A retro-computer operating system firmware for the **ClockworkPi PicoCalc**
(**Raspberry Pi Pico 2 W / RP2350**), written in C on the Pico SDK.

Boots into a CRT-green terminal, plays a startup chime, and presents an
icon-grid launcher with built-in apps and games. Extensible with
**PicoScript** — a tiny compiled scripting language whose programs live on
the SD card (an `AUTORUN.PS` on the card runs at boot, like `AUTOEXEC.BAT`).

![hardware](https://www.clockworkpi.com/picocalc)

## Features

- **Retro launcher** with icon grid, battery meter, keyboard nav
- **Terminal shell** with history, `ls`/`cat`/`run`/`edit`/`mem`/`bat`/`beep`…
- **Apps:** file manager, text editor (F2 = save), calculator (expression
  parser with `+ - * / % ^` and parentheses), system monitor, settings
  (LCD/keyboard backlight, key click, power off), about screen, **LLM chat**
  over WiFi
- **Games:** Snake, Breakout, Space Invaders, Conway's Game of Life
- **WiFi/Networking:** CYW43 + lwIP (poll mode) with a minimal plain-HTTP
  client. The CHAT app talks to LAN LLM servers that don't need TLS —
  Ollama (`/api/generate`), llama.cpp (`/completion`), or any
  OpenAI-compatible `/v1/chat/completions` endpoint. Configure via
  `CHAT.CFG` on the SD card (ssid, password, host, port, path, model), or
  answer the prompts. HTTPS to public APIs is not supported (no TLS stack).
- **PicoScript VM:** `let/print/if/while/for/sub/input` plus graphics
  (`pset/line/rect/fillrect/circle/text/color/flush`), `beep`, `sleep`,
  `rnd`, `key` — compiled to bytecode, not interpreted line-by-line
- **Drivers:** ILI9488/ST7365P 320×320 (RGB666 SPI @ 62.5MHz, **double-buffered**
  96 KB RGB332 framebuffers with tear-free dirty-rect flip, DMA push), STM32
  keyboard BIOS over I2C with shift/symbol layers and auto-repeat, dual-PWM
  beeper, SD card (FatFS @ 25MHz SPI)

## Hardware targets

| Peripheral | Interface | Pins |
|---|---|---|
| LCD ILI9488 320×320 | SPI1 @ 62.5 MHz | SCK 10, TX 11, CS 13, DC 14, RST 15 |
| Keyboard STM32 BIOS | I2C1 @ 0x1F, 10 kHz | SDA 6, SCL 7 |
| Speakers (PWM) | PWM | L 26, R 27 |
| SD card | SPI0 @ 25 MHz | SCLK 18, MOSI 19, MISO 16, CS 17 |

**Clocks:** RP2350 @ 200 MHz (overclocked from 150 MHz stock), 520 KB SRAM
(double-buffered display fits comfortably), 4 MB flash, RP2350_ARM_S UF2.

## Multiboot (uf2loader)

This UF2 is built for the PicoCalc **uf2loader** multiboot bootloader. To use:
copy `pretrocalc.uf2` to `/pico2-apps/` on the SD card (Pico 2 W uses
`pico2-apps`, not `pico1-apps`). Hold **Up/F1/F5** on power-on for the menu.
The app name "PRetroCalc OS" is shown from the binary-info block.

## Build

Prerequisites: ARM GCC toolchain (`arm-none-eabi-gcc`), CMake, Ninja, Python,
and the Pico SDK (`PICO_SDK_PATH` set, or clone next to the project).

```sh
git submodule update --init --recursive   # pulls lib/no-OS-FatFS-...
cmake -G Ninja -B build
cmake --build build
# -> build/pretrocalc.uf2
```

Note: `picotool.exe` segfaults on some Windows machines, so the build uses
`tools/elf2uf2.py` (a small pure-Python ELF→UF2 converter) instead.

## Flash

Hold **BOOTSEL** on the Pico, plug in USB, copy `build/pretrocalc.uf2` to the
`RPI-RP2` drive. Done — the OS boots in under a second.

## Usage

- **Launcher:** arrows to move, ENTER to run, `T` jumps straight to the terminal.
- **Everywhere:** ESC exits back to the launcher.
- **Terminal:** type `help`. Run scripts with `run STARS.PS`.
- **SD card:** copy `sd-card/*.PS` to the card root. Write your own `.PS`
  files — see `sd-card/STARS.PS` for the language by example.

## PicoScript quick reference

```
let x = 5
print "value", x           # comments start with #
if x > 3 then ... else ... end
while x > 0 ... end
for i = 1 to 10 ... next
sub greet(n) ... end       # call with greet(42)
input("prompt"), var
color(c) pset(x,y,c) line(x0,y0,x1,y1) rect(x,y,w,h) fillrect(x,y,w,h)
circle(x,y,r) text(x,y,"str") flush beep(freq,ms) sleep(ms) rnd(n) key
```

Colors are RGB332 bytes (0-255). Screen is 320×320, text is 8×8.

## Project layout

```
src/main.c          boot + launcher + app registry
src/gfx.c           ILI9488 driver, framebuffer, primitives
src/keyboard.c      STM32 BIOS I2C keyboard
src/sound.c         PWM beeper
src/os.c            virtual terminal, status bar, line input
src/sdfs.c,hw_config.c  SD/FatFS
src/pscript.c       PicoScript compiler + VM
src/app_*.c         shell, utility apps, games
tools/elf2uf2.py    UF2 converter
sd-card/            sample PicoScript programs
```

## Credits

- PicoCalc hardware + reference drivers: [ClockworkPi](https://github.com/clockworkpi/PicoCalc)
- FatFS on SD via [carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico](https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)
- 8×8 font: public-domain font8x8 (Daniel Hepper)
