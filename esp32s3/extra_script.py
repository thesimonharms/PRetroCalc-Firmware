# Stage shared app sources + their headers from ../src (NOT lwipopts.h).
Import("env")
import shutil
from pathlib import Path

proj = Path(env["PROJECT_DIR"]).resolve()
root = proj.parent / "src"
stage = proj / ".shared_src"
stage.mkdir(exist_ok=True)

shared_c = [
    "font.c",
    "font_jawa.c",
    "os.c",
    "pscript.c",
    "app_terminal.c",
    "app_util.c",
    "app_games.c",
    "app_notes.c",
    "app_chat.c",
    "app_emu.c",
    "emu_gb.c",
]
shared_h = [
    "font.h",
    "font_jawa.h",
    "os.h",
    "pscript.h",
    "apps.h",
    "keyboard.h",
    "sound.h",
    "gfx.h",
    "sdfs.h",
    "net.h",
    "emu.h",
]

for name in shared_c + shared_h:
    src = root / name
    if not src.is_file():
        raise SystemExit(f"missing shared file: {src}")
    shutil.copy2(src, stage / name)

# ESP32 uses a heap-allocated framebuffer pointer (saves .bss).
gfx_h = stage / "gfx.h"
gfx_h.write_text(
    gfx_h.read_text(encoding="utf-8").replace(
        "extern uint8_t gfx_fb[LCD_WIDTH * LCD_HEIGHT];",
        "extern uint8_t *gfx_fb;",
    ),
    encoding="utf-8",
)

# Shared sources need board.h / pico shims from include/, and staged headers.
env.Append(CPPPATH=[str(proj / "include"), str(stage)])

# Patch staged shared sources for ESP32 (Pico-only symbols).
for path in stage.glob("*.c"):
    text = path.read_text(encoding="utf-8")
    orig = text
    if path.name == "app_terminal.c":
        text = text.replace(
            '        extern char __StackLimit, __bss_end__;\n'
            '        os_printf("Stack free: ~%d bytes\\n", (int)(&__StackLimit - &__bss_end__));\n',
            '        os_printf("Heap free: %u bytes\\n", (unsigned)esp_get_free_heap_size());\n',
        )
        if "esp_get_free_heap_size" in text and "#include <esp_heap_caps.h>" not in text:
            text = text.replace(
                '#include "hardware/watchdog.h"\n',
                '#include "hardware/watchdog.h"\n#include <esp_heap_caps.h>\n',
            )
    if text != orig:
        path.write_text(text, encoding="utf-8")
        print(f"patched {path.name}")

build_dir = Path(env.subst("$BUILD_DIR")) / "shared_src"
env.BuildSources(str(build_dir), str(stage))
