#ifndef PRBOARD_H
#define PRBOARD_H

/* Waveshare ESP32-S3-Pico pin map for the ClockworkPi PicoCalc. */
#ifndef LCD_SPI_SPEED
#define LCD_SPI_SPEED 40000000
#endif
#ifndef LCD_PIN_SCK
#define LCD_PIN_SCK 35
#endif
#ifndef LCD_PIN_TX
#define LCD_PIN_TX 36
#endif
#ifndef LCD_PIN_RX
#define LCD_PIN_RX 37
#endif
#ifndef LCD_PIN_CS
#define LCD_PIN_CS 38
#endif
#ifndef LCD_PIN_DC
#define LCD_PIN_DC 39
#endif
#ifndef LCD_PIN_RST
#define LCD_PIN_RST 40
#endif
#define LCD_WIDTH 320
#define LCD_HEIGHT 320

#ifndef KBD_PIN_SDA
#define KBD_PIN_SDA 17
#endif
#ifndef KBD_PIN_SCL
#define KBD_PIN_SCL 18
#endif
#define KBD_I2C_SPEED 10000
#define KBD_I2C_ADDR 0x1F
#define REG_ID_VER 0x01
#define REG_ID_BKL 0x05
#define REG_ID_FIF 0x09
#define REG_ID_BK2 0x0A
#define REG_ID_BAT 0x0B
#define REG_ID_OFF 0x0E

#ifndef AUDIO_PIN_L
#define AUDIO_PIN_L 7
#endif
#ifndef AUDIO_PIN_R
#define AUDIO_PIN_R 8
#endif

#ifndef SD_PIN_SCLK
#define SD_PIN_SCLK 1
#endif
#ifndef SD_PIN_MOSI
#define SD_PIN_MOSI 2
#endif
#ifndef SD_PIN_MISO
#define SD_PIN_MISO 42
#endif
#ifndef SD_PIN_CS
#define SD_PIN_CS 41
#endif
#ifndef SD_PIN_DET
#define SD_PIN_DET 6
#endif

#endif
