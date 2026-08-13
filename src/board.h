#ifndef PRBOARD_H
#define PRBOARD_H

#include "hardware/spi.h"
#include "hardware/i2c.h"

/* ============ PicoCalc (ClockworkPi v2.0) pin map ============ */

/* LCD: ILI9488 / ST7365P, 320x320, SPI1, 18-bit (RGB666) */
#define LCD_SPI_MOD     spi1
#define LCD_SPI_SPEED   62500000   /* RP2350: 200MHz/2; ILI9488 handles >60MHz fine */
#define LCD_PIN_SCK     10
#define LCD_PIN_TX      11
#define LCD_PIN_RX      12
#define LCD_PIN_CS      13
#define LCD_PIN_DC      14
#define LCD_PIN_RST     15
#define LCD_WIDTH       320
#define LCD_HEIGHT      320

/* Keyboard / system-management STM32 "BIOS", I2C1 @ 10 kHz, addr 0x1F */
#define KBD_I2C_MOD     i2c1
#define KBD_PIN_SDA     6
#define KBD_PIN_SCL     7
#define KBD_I2C_SPEED   10000
#define KBD_I2C_ADDR    0x1F

/* STM32 BIOS register map */
#define REG_ID_VER      0x01
#define REG_ID_BKL      0x05   /* LCD backlight */
#define REG_ID_FIF      0x09   /* key FIFO */
#define REG_ID_BK2      0x0A   /* keyboard backlight */
#define REG_ID_BAT      0x0B   /* battery: hi byte = percent (bit7=charging) */
#define REG_ID_OFF      0x0E   /* write 1 -> power off */

/* PWM audio (dual speakers) */
#define AUDIO_PIN_L     26
#define AUDIO_PIN_R     27

/* SD card, SPI0 */
#define SD_SPI_MOD      spi0
#define SD_PIN_SCLK     18
#define SD_PIN_MOSI     19
#define SD_PIN_MISO     16
#define SD_PIN_CS       17

/* User LED on Pico board */
#define LED_PIN         25

#endif
