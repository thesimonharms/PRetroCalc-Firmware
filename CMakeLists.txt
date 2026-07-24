cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
# picotool segfaults on this host; disable it (we generate UF2 ourselves)
set(PICO_NO_PICOTOOL 1 CACHE BOOL "Skip picotool")
include(pico_sdk_import.cmake)

project(pretrocalc C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_subdirectory(lib/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico/src FatFs)

add_executable(pretrocalc
    src/main.c
    src/gfx.c
    src/font.c
    src/font_jawa.c
    src/keyboard.c
    src/sound.c
    src/os.c
    src/sdfs.c
    src/hw_config.c
    src/pscript.c
    src/app_terminal.c
    src/app_util.c
    src/app_games.c
    src/app_notes.c
    src/net.c
    src/app_chat.c
)

target_include_directories(pretrocalc PRIVATE src lib)

target_compile_options(pretrocalc PRIVATE -O2)

pico_enable_stdio_usb(pretrocalc 1)
pico_enable_stdio_uart(pretrocalc 0)

target_link_libraries(pretrocalc
    pico_stdlib
    hardware_spi
    hardware_i2c
    hardware_pwm
    hardware_dma
    hardware_watchdog
    pico_cyw43_arch_lwip_poll
    no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

# NOTE: skip pico_add_extra_outputs() — it invokes picotool, which
# segfaults on this host. Generate .bin/.uf2 manually instead.
add_custom_command(TARGET pretrocalc POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -Obinary pretrocalc.elf pretrocalc.bin
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/tools/elf2uf2.py ${CMAKE_BINARY_DIR}/elf2uf2.py
    COMMAND python elf2uf2.py pretrocalc.elf pretrocalc.uf2 0xe48bff59
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Generating pretrocalc.bin / pretrocalc.uf2"
)
