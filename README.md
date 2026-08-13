# PRetroCalc OS

A retro-computer operating system for the **ClockworkPi PicoCalc**, written in C.
**`main` targets a Waveshare ESP32-S3-Pico** (Xtensa LX7) dropped into the
PicoCalc in place of the stock Raspberry Pi Pico.

Boots into a CRT-green GEM/TOS desktop, plays a startup chime, and presents an
icon-grid launcher with built-in apps and games. Extensible with **PicoScript**
— a tiny compiled scripting language whose programs live on the SD card (an
`AUTORUN.PS` on the card runs at boot, like `AUTOEXEC.BAT`).

![hardware](https://www.clockworkpi.com/picocalc)

The original **Pico 2 W / RP2350** port lives on the [`pico2w`](https://github.com/thesimonharms/PRetroCalc-Firmware/tree/pico2w)
branch. It was the first target, but the RP2350 simply isn't fast or strong
enough for this OS — LLM chat, Aksara Jawa notes, and a full desktop need more
CPU, RAM, and a real WiFi radio than the Pico 2 W can give. Development
continues on Xtensa.

## Features

- **Retro launcher** with icon grid, battery meter, keyboard nav
- **Terminal shell** with history, `ls`/`cat`/`run`/`edit`/`mem`/`bat`/`beep`…
- **Apps:** file manager, text editor (F2 = save), **NOTES** (UTF-8 markdown +
  Aksara Jawa / Carakan keyboard), calculator (expression parser with
  `+ - * / % ^` and parentheses), system monitor, settings (LCD/keyboard
  backlight, key click, power off), about screen, **LLM chat** over WiFi
- **Games:** Snake, Breakout, Space Invaders, Conway's Game of Life
- **WiFi/Networking:** ESP32 STA WiFi with a minimal plain-HTTP client. The
  CHAT app talks to LAN LLM servers that don't need TLS — **malaikat**,
  **Ollama**, **LM Studio** (OpenAI-compat on port 1234), llama.cpp, or any
  OpenAI-compatible `/v1/chat/completions` endpoint. Configure via `CHAT.CFG`
  on the SD card, or use the on-device setup wizard (Ollama / LM Studio /
  malaikat / Other). HTTPS to public APIs is not supported (no TLS stack).
- **PicoScript VM:** `let/print/if/while/for/sub/input` plus graphics
  (`pset/line/rect/fillrect/circle/text/color/flush`), `beep`, `sleep`,
  `rnd`, `key` — compiled to bytecode, not interpreted line-by-line
- **Drivers:** ILI9488/ST7365P 320×320 (RGB666 SPI @ 40 MHz, single 96 KB
  RGB332 framebuffer with dirty-rect flush), STM32 keyboard BIOS over I2C with
  shift/symbol layers and auto-repeat, dual-PWM beeper, SD card (SPI)

## Hardware

**Board:** [Waveshare ESP32-S3-Pico](https://www.waveshare.com/wiki/ESP32-S3-Pico)
in a ClockworkPi PicoCalc (same LCD, STM32 keyboard, SD slot, speakers).

| Peripheral | Interface | ESP32 GPIO | PicoCalc GP |
|---|---|---|---|
| LCD ILI9488 320×320 | SPI2 @ 40 MHz | SCK 35, MOSI 36, MISO 37, CS 38, DC 39, RST 40 | 10–15 |
| Keyboard STM32 BIOS | I2C @ 0x1F, 10 kHz | SDA 17, SCL 18 | 6–7 |
| Speakers (PWM) | LEDC | L 7, R 8 | 26–27 |
| SD card | SPI3/HSPI | SCLK 1, MOSI 2, MISO 42, CS 41 | 18, 19, 16, 17 |

**Clocks:** ESP32-S3 @ 240 MHz, ~512 KB internal SRAM, 16 MB flash.

**PSRAM must stay disabled.** Pico GP10–12 map to GPIO35–37, which are the
onboard Octal PSRAM pins on the Waveshare module. The Pico build's double
RGB332 framebuffer is reduced to a **single** 96 KB buffer so everything fits
in internal SRAM with WiFi.

## Build

Requires [PlatformIO](https://platformio.org/) (Arduino-ESP32 core):

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

## Usage

- **Launcher:** arrows to move, ENTER to run, `T` jumps straight to the terminal.
- **Everywhere:** ESC exits back to the launcher.
- **Terminal:** type `help`. Run scripts with `run STARS.PS`.
- **SD card:** copy `sd-card/*.PS` to the card root. Write your own `.PS`
  files — see `sd-card/STARS.PS` for the language by example.
- **NOTES:** markdown files under `NOTES/` on the SD card. Toggle the Aksara
  Jawa keyboard layer from the app.
- **LLM chat:** open CHAT. Needs WiFi + a LAN LLM server (no TLS). Keys inside
  chat: ↑/↓/PgUp/PgDn scroll, Tab = show/hide thinking, **Ctrl+M** = pick model.

### CHAT.CFG (SD card root)

Plain text, one field per line:

```
YourWiFiSSID
YourWiFiPassword
192.168.1.50
11434
/api/generate
llama3
ollama
```

| Line | Meaning | Examples |
|------|---------|----------|
| 1 | WiFi SSID | `NETGEAR82` |
| 2 | WiFi password | |
| 3 | Server IP or hostname | `192.168.1.25` |
| 4 | Port | `8080` (malaikat), `11434` (Ollama), `1234` (LM Studio) |
| 5 | HTTP path | `/v1/chat/completions`, `/api/generate` |
| 6 | Model name/id | malaikat `--alias`, `llama3`, or LM Studio's model id |
| 7 | API type *(optional)* | `malaikat`, `ollama`, `lmstudio`, `openai`, `llamacpp` |

- **`malaikat`, `lmstudio`, and `openai`** share the OpenAI chat request shape.
- If line 7 is omitted, the API is **auto-detected** from path/port
  (`/v1/chat/completions` on port `8080` → malaikat;
  `/v1/chat/completions` or port `1234` → OpenAI/LM Studio;
  `/api/generate` or port `11434` → Ollama;
  `/completion` or port `8080` → legacy llama.cpp).
- If `CHAT.CFG` is missing, the on-device wizard asks **1=Ollama / 2=LM Studio /
  3=malaikat / 4=Other** and writes the file for next time.

#### malaikat checklist

[malaikat](https://github.com/thesimonharms/malaikat) is a ROCm llama.cpp wrapper
with an OpenAI-compat API at `http://<pc>:8080/v1`. PicoCalc cannot reach
`127.0.0.1` on the PC — bind to the LAN.

1. Serve on all interfaces (not localhost-only):

```powershell
.\malaikat.exe serve -config .\coding.yaml -host 0.0.0.0
```

Or set `host: 0.0.0.0` in `coding.yaml` / `%AppData%\malaikat\last.yaml`.
2. Allow Windows firewall inbound on port **8080**.
3. Put the PC's LAN IP in `CHAT.CFG` with:

```
YourSSID
YourPassword
192.168.1.50
8080
/v1/chat/completions
qwen3.6-35b-a3b-mtp
malaikat
```

Line 6 is malaikat's `--alias` (or leave blank and use **Ctrl+M** after connect).
See also `sd-card/CHAT.CFG.malaikat.example`.

#### LM Studio checklist

1. In LM Studio → **Developer** → start the server.
2. Enable **Serve on Local Network** (not localhost-only).
3. Load a model (or enable JIT loading).
4. Allow Windows firewall inbound on port **1234** if prompted.
5. Put the PC's LAN IP in `CHAT.CFG` with:

```
YourSSID
YourPassword
192.168.1.50
1234
/v1/chat/completions
my-model-id
lmstudio
```

6. On the PicoCalc: open CHAT → connect → **Ctrl+M** to pick a model if needed.

#### Ollama checklist

1. Expose Ollama on the LAN (`OLLAMA_HOST=0.0.0.0` or the desktop app's expose toggle).
2. `CHAT.CFG` example:

```
YourSSID
YourPassword
192.168.1.50
11434
/api/generate
llama3
ollama
```

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
esp32s3/            PlatformIO project — ESP32-S3 HAL (this is what you flash)
  platformio.ini
  extra_script.py   stages shared apps from ../src
  include/          board.h + Pico API shims
  src/              gfx, keyboard, sound, SD, WiFi, main
src/                shared OS / apps / PicoScript / Jawa font
sd-card/            sample PicoScript programs + CHAT.CFG*.example
tools/              Jawa font rasterizers
```

HAL-specific notes: [`esp32s3/README.md`](esp32s3/README.md).

## Pico 2 W / RP2350

Checkout the [`pico2w`](https://github.com/thesimonharms/PRetroCalc-Firmware/tree/pico2w)
branch for the original Pico SDK build (UF2, uf2loader, CYW43 WiFi, double-buffered
display). That branch is the place to keep the RP2350 port alive; `main` is
ESP32-S3.

## Credits

- PicoCalc hardware + reference drivers: [ClockworkPi](https://github.com/clockworkpi/PicoCalc)
- ESP32-S3-Pico module: [Waveshare](https://www.waveshare.com/wiki/ESP32-S3-Pico)
- 8×8 font: public-domain font8x8 (Daniel Hepper)
