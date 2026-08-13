#include <Arduino.h>
#include <string.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_rom_sys.h>
#include <SdFat.h>
#include "board.h"

extern "C" {
#include "sdfs.h"
}

#ifndef SD_PIN_DET
#define SD_PIN_DET 6
#endif

#if !USE_BLOCK_DEVICE_INTERFACE
#error "need -DUSE_BLOCK_DEVICE_INTERFACE=1"
#endif

static char diag[48] = "SD not tried";
static bool mounted;
static FsVolume vol;

static void pin_out(int pin, int level) {
    if (rtc_gpio_is_valid_gpio((gpio_num_t)pin)) rtc_gpio_deinit((gpio_num_t)pin);
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)pin, GPIO_DRIVE_CAP_3);
    gpio_set_level((gpio_num_t)pin, level);
}

static void pin_in_pu(int pin) {
    if (rtc_gpio_is_valid_gpio((gpio_num_t)pin)) rtc_gpio_deinit((gpio_num_t)pin);
    gpio_reset_pin((gpio_num_t)pin);
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_pullup_en((gpio_num_t)pin);
    gpio_pulldown_dis((gpio_num_t)pin);
}

/* PicoCalc SD: bit-bang SPI0 pins, Blair-style CS dummy clocks. */
class PicoSd : public FsBlockDeviceInterface {
 public:
    int miso = SD_PIN_MISO, mosi = SD_PIN_MOSI, sck = SD_PIN_SCLK, cs = SD_PIN_CS;
    uint32_t half_us = 3;
    bool sdhc = false;
    uint32_t nsec = 0;
    bool ok = false;

    uint8_t xfer(uint8_t data) {
        uint8_t in = 0;
        for (int i = 0; i < 8; i++) {
            gpio_set_level((gpio_num_t)mosi, (data & 0x80) ? 1 : 0);
            data = (uint8_t)(data << 1);
            esp_rom_delay_us(half_us);
            gpio_set_level((gpio_num_t)sck, 1);
            esp_rom_delay_us(half_us);
            in = (uint8_t)((in << 1) | (gpio_get_level((gpio_num_t)miso) ? 1 : 0));
            gpio_set_level((gpio_num_t)sck, 0);
        }
        return in;
    }

    void dummy(int n) { while (n--) xfer(0xFF); }

    void cs_sel(void) {
        gpio_set_level((gpio_num_t)cs, 0);
        dummy(8);
    }
    void cs_desel(void) {
        gpio_set_level((gpio_num_t)cs, 1);
        dummy(8);
    }

    /* Leaves CS selected. Caller deselects. */
    uint8_t cmd(uint8_t c, uint32_t arg) {
        uint8_t crc = 0x01;
        if (c == 0) crc = 0x95;
        else if (c == 8) crc = 0x87;
        gpio_set_level((gpio_num_t)cs, 0);
        dummy(2);
        if (c != 0) wait_not_busy(200);
        xfer((uint8_t)(0x40 | c));
        xfer((uint8_t)(arg >> 24));
        xfer((uint8_t)(arg >> 16));
        xfer((uint8_t)(arg >> 8));
        xfer((uint8_t)arg);
        xfer(crc);
        xfer(0xFF); /* extra 8 clocks some cards need after the CRC */
        uint8_t r = 0xFF;
        for (int i = 0; i < 64; i++) {
            r = xfer(0xFF);
            if (!(r & 0x80)) break;
        }
        return r;
    }

    bool wait_token(uint8_t token, uint32_t ms) {
        uint32_t t0 = millis();
        do {
            if (xfer(0xFF) == token) return true;
        } while (millis() - t0 < ms);
        return false;
    }

    bool wait_not_busy(uint32_t ms) {
        uint32_t t0 = millis();
        do {
            if (xfer(0xFF) == 0xFF) return true;
        } while (millis() - t0 < ms);
        return false;
    }

    uint32_t addr(uint32_t sector) { return sdhc ? sector : sector * 512UL; }

    bool read_csd(uint8_t csd[16]) {
        uint8_t r = cmd(9, 0);
        if (r != 0) { cs_desel(); return false; }
        if (!wait_token(0xFE, 100)) { cs_desel(); return false; }
        for (int i = 0; i < 16; i++) csd[i] = xfer(0xFF);
        xfer(0xFF); xfer(0xFF);
        cs_desel();
        return true;
    }

    bool init_card(void) {
        ok = false;
        sdhc = false;
        nsec = 0;
        half_us = 3;
        pin_in_pu(miso);
        pin_out(mosi, 1);
        pin_out(sck, 0);
        pin_out(cs, 1);
        delay(20);
        dummy(80);
        delay(10);

        uint8_t r = 0xFF;
        for (int a = 0; a < 12; a++) {
            r = cmd(0, 0);
            cs_desel();
            if (r == 0x01) break;
            delay(10);
        }
        if (r != 0x01) {
            snprintf(diag, sizeof diag, "SD init CMD0=%02X", r);
            return false;
        }

        bool v2 = false;
        r = cmd(8, 0x1AA);
        if (r == 0x01) {
            uint8_t r7[4];
            for (int i = 0; i < 4; i++) r7[i] = xfer(0xFF);
            cs_desel();
            if (r7[3] != 0xAA) {
                snprintf(diag, sizeof diag, "SD init CMD8 echo");
                return false;
            }
            v2 = true;
        } else {
            cs_desel(); /* SD v1 / MMC */
        }

        /* Arduino-ESP32 uses HCS + 3.2–3.3V (bit20). HCS-only 0x40000000
         * leaves some PicoCalc cards stuck in idle (R1=0x01). */
        static const uint32_t v2_args[] = { 0x40100000UL, 0x40000000UL, 0x40FF8000UL };
        static const uint32_t v1_args[] = { 0x00100000UL, 0x00FF8000UL, 0 };
        const uint32_t *args = v2 ? v2_args : v1_args;
        unsigned nargs = v2 ? 3 : 3;
        r = 0x01;
        for (unsigned a = 0; a < nargs && r != 0; a++) {
            uint32_t t0 = millis();
            do {
                uint8_t r55 = cmd(55, 0);
                cs_desel();
                if (r55 > 1) {
                    snprintf(diag, sizeof diag, "SD init CMD55=%02X", r55);
                    return false;
                }
                r = cmd(41, args[a]);
                cs_desel();
                if (r == 0) break;
                delay(10);
            } while (millis() - t0 < 3000);
        }
        if (r != 0 && !v2) {
            uint32_t t0 = millis();
            do {
                r = cmd(1, 0x00100000UL); /* MMC CMD1 */
                cs_desel();
                if (r == 0) break;
                delay(10);
            } while (millis() - t0 < 3000);
        }
        if (r != 0) {
            snprintf(diag, sizeof diag, "SD init ACMD41=%02X", r);
            return false;
        }

        r = cmd(58, 0);
        if (r != 0) { cs_desel(); snprintf(diag, sizeof diag, "SD init CMD58=%02X", r); return false; }
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = xfer(0xFF);
        cs_desel();
        sdhc = (ocr[0] & 0x40) != 0;

        if (!sdhc) {
            r = cmd(16, 512);
            cs_desel();
            if (r != 0) {
                snprintf(diag, sizeof diag, "SD init CMD16=%02X", r);
                return false;
            }
        }

        uint8_t csd[16];
        if (!read_csd(csd)) {
            snprintf(diag, sizeof diag, "SD init CSD");
            return false;
        }
        if ((csd[0] >> 6) == 1) {
            uint32_t csize = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
            nsec = (csize + 1) << 10;
        } else {
            uint32_t csize = ((uint32_t)(csd[6] & 3) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
            uint32_t mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
            uint32_t blen = csd[5] & 0x0F;
            uint64_t bytes = ((uint64_t)(csize + 1)) << (mult + 2 + blen);
            nsec = (uint32_t)(bytes / 512ULL);
        }
        if (nsec == 0) nsec = 1;

        half_us = 1; /* ~500 kHz after init */
        ok = true;
        return true;
    }

    bool isBusy() override { return false; }
    uint32_t sectorCount() override { return nsec; }
    bool syncDevice() override {
        if (!ok) return false;
        cs_sel();
        bool r = wait_not_busy(500);
        cs_desel();
        return r;
    }
    bool readSector(uint32_t sector, uint8_t *dst) override {
        if (!ok) return false;
        uint8_t r = cmd(17, addr(sector));
        if (r != 0) { cs_desel(); return false; }
        if (!wait_token(0xFE, 200)) { cs_desel(); return false; }
        for (int i = 0; i < 512; i++) dst[i] = xfer(0xFF);
        xfer(0xFF); xfer(0xFF);
        cs_desel();
        return true;
    }
    bool readSectors(uint32_t sector, uint8_t *dst, size_t ns) override {
        while (ns--) {
            if (!readSector(sector++, dst)) return false;
            dst += 512;
        }
        return true;
    }
    bool writeSector(uint32_t sector, const uint8_t *src) override {
        if (!ok) return false;
        uint8_t r = cmd(24, addr(sector));
        if (r != 0) { cs_desel(); return false; }
        xfer(0xFE);
        for (int i = 0; i < 512; i++) xfer(src[i]);
        xfer(0xFF); xfer(0xFF);
        r = xfer(0xFF) & 0x1F;
        if (r != 0x05) { cs_desel(); return false; }
        if (!wait_not_busy(500)) { cs_desel(); return false; }
        cs_desel();
        return true;
    }
    bool writeSectors(uint32_t sector, const uint8_t *src, size_t ns) override {
        while (ns--) {
            if (!writeSector(sector++, src)) return false;
            src += 512;
        }
        return true;
    }
};

static PicoSd card;

static const char *norm_path(const char *path) {
    if (!path || !*path) return "/";
    if (path[0] == '/' && path[1] == '\0') return "/";
    return path;
}

static bool try_volume(void) {
    /* MBR partitions 1–4, then superfloppy (no partition table). */
    for (uint8_t p = 1; p <= 4; p++) {
        if (vol.begin(&card, true, p)) return true;
        vol.end();
    }
    if (vol.begin(&card, true, 0, 0)) return true;
    vol.end();
    if (vol.begin(&card, true, 0, 2048)) return true;
    vol.end();
    return false;
}

extern "C" {

const char *sdfs_diag(void) { return diag; }

bool sdfs_init(void) {
    if (mounted) return true;
    pin_in_pu(SD_PIN_DET);
    delay(5);
    int det = gpio_get_level((gpio_num_t)SD_PIN_DET);

    if (!card.init_card()) {
        /* diag already set; append DET */
        char tmp[48];
        snprintf(tmp, sizeof tmp, "%s DET=%d", diag, det);
        strncpy(diag, tmp, sizeof diag - 1);
        diag[sizeof diag - 1] = 0;
        return false;
    }
    if (!try_volume()) {
        snprintf(diag, sizeof diag, "SD card ok fat? DET=%d sec=%lu", det, (unsigned long)card.nsec);
        return false;
    }
    vol.chdir();
    mounted = true;
    uint8_t ft = vol.fatType();
    const char *kind = ft == 64 ? "exfat" : ft == 32 ? "fat32" : ft == 16 ? "fat16" : "fat";
    snprintf(diag, sizeof diag, "SD ok %s DET=%d", kind, det);
    return true;
}

bool sdfs_mkdir(const char *path) {
    if (!mounted || !path || !*path) return false;
    if (vol.exists(path)) return true;
    return vol.mkdir(path, true);
}

bool sdfs_remove(const char *path) {
    if (!mounted || !path || !*path) return false;
    return vol.remove(path) || vol.rmdir(path);
}

bool sdfs_read_file(const char *path, char *buf, uint32_t max, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!mounted || !buf || max == 0) return false;
    FsFile f;
    if (!f.open(norm_path(path), O_RDONLY)) return false;
    int n = f.read(buf, max - 1);
    f.close();
    if (n < 0) return false;
    buf[n] = 0;
    if (out_len) *out_len = (uint32_t)n;
    return true;
}

bool sdfs_write_file(const char *path, const char *buf, uint32_t len) {
    if (!mounted || !path || (!buf && len)) return false;
    FsFile f;
    if (!f.open(norm_path(path), O_WRONLY | O_CREAT | O_TRUNC)) return false;
    int n = f.write(buf ? buf : "", len);
    f.close();
    return n >= 0 && (uint32_t)n == len;
}

bool sdfs_append_file(const char *path, const char *buf, uint32_t len) {
    if (!mounted || !path || (!buf && len)) return false;
    FsFile f;
    if (!f.open(norm_path(path), O_WRONLY | O_CREAT | O_APPEND)) return false;
    int n = f.write(buf ? buf : "", len);
    f.close();
    return n >= 0 && (uint32_t)n == len;
}

int sdfs_list_dir(const char *path, char names[][48], int max, bool dirs_only) {
    if (!mounted || !names || max <= 0) return 0;
    FsFile dir;
    if (!dir.open(norm_path(path)) || !dir.isDir()) {
        if (dir.isOpen()) dir.close();
        return 0;
    }
    int n = 0;
    FsFile entry;
    while (n < max && entry.openNext(&dir, O_RDONLY)) {
        char name[48];
        entry.getName(name, sizeof name);
        bool is_dir = entry.isDir();
        entry.close();
        if (!name[0] || name[0] == '.') continue;
        if (dirs_only && !is_dir) continue;
        strncpy(names[n], name, 47);
        names[n][47] = 0;
        if (is_dir) strncat(names[n], "/", 47 - strlen(names[n]));
        n++;
    }
    dir.close();
    return n;
}

}
