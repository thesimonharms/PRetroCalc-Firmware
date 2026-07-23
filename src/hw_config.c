/* PRetroCalc OS - SD card hardware config for carlk3 no-OS-FatFS lib.
 * PicoCalc SD slot: SPI0, SCLK=18 MOSI=19 MISO=16 CS=17 */
#include "hw_config.h"
#include "board.h"

static spi_t spis[] = {
    {
        .hw_inst = SD_SPI_MOD,
        .miso_gpio = SD_PIN_MISO,
        .mosi_gpio = SD_PIN_MOSI,
        .sck_gpio = SD_PIN_SCLK,
        .baud_rate = 25 * 1000 * 1000,   /* RP2350 SPI is fast; SD cards handle 25MHz fine */
    }
};

static sd_spi_if_t spi_if = {
    .spi = &spis[0],
    .ss_gpio = SD_PIN_CS,
};

static sd_card_t sd_cards[] = {
    {
        .type = SD_IF_SPI,
        .spi_if_p = &spi_if,
        .use_card_detect = false,
    }
};

size_t sd_get_num(void) { return sizeof(sd_cards) / sizeof(sd_cards[0]); }

sd_card_t *sd_get_by_num(size_t num) {
    return num < sd_get_num() ? &sd_cards[num] : NULL;
}

size_t spi_get_num(void) { return sizeof(spis) / sizeof(spis[0]); }

spi_t *spi_get_by_num(size_t num) {
    return num < spi_get_num() ? &spis[num] : NULL;
}
