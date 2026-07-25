#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <string.h>
#include "board.h"

extern "C" {
#include "sdfs.h"
}

/* LCD owns SPI2/FSPI — SD must use SPI3/HSPI. */
static SPIClass sd_spi(HSPI);
static bool mounted;
static const char *absolute_path(const char *path, char out[128]) {
    if (!path || !*path) return "/";
    if (path[0] == '/') return path;
    snprintf(out, 128, "/%s", path);
    return out;
}

extern "C" {

bool sdfs_init(void) {
    if (mounted) return true;
    sd_spi.begin(SD_PIN_SCLK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    mounted = SD.begin(SD_PIN_CS, sd_spi);
    return mounted;
}
bool sdfs_read_file(const char *path, char *buf, uint32_t max, uint32_t *out_len) {
    if (out_len) *out_len = 0;
    if (!mounted || !buf || max == 0) return false;
    char full[128]; File f = SD.open(absolute_path(path, full), FILE_READ);
    if (!f || f.isDirectory()) return false;
    size_t n = f.readBytes(buf, max - 1); f.close(); buf[n] = 0;
    if (out_len) *out_len = n; return true;
}
bool sdfs_write_file(const char *path, const char *buf, uint32_t len) {
    if (!mounted || !path || (!buf && len)) return false;
    char full[128]; const char *p = absolute_path(path, full);
    SD.remove(p);
    File f = SD.open(p, FILE_WRITE);
    if (!f) return false;
    size_t n = f.write((const uint8_t *)buf, len); f.close(); return n == len;
}
bool sdfs_append_file(const char *path, const char *buf, uint32_t len) {
    if (!mounted || !path || (!buf && len)) return false;
    char full[128]; File f = SD.open(absolute_path(path, full), FILE_APPEND);
    if (!f) return false;
    size_t n = f.write((const uint8_t *)buf, len); f.close(); return n == len;
}
int sdfs_list_dir(const char *path, char names[][48], int max, bool dirs_only) {
    if (!mounted || !names || max <= 0) return 0;
    char full[128]; File dir = SD.open(absolute_path(path, full), FILE_READ);
    if (!dir || !dir.isDirectory()) return 0;
    int n = 0;
    for (File entry = dir.openNextFile(); entry && n < max; entry = dir.openNextFile()) {
        if (dirs_only && !entry.isDirectory()) { entry.close(); continue; }
        const char *name = entry.name();
        const char *base = strrchr(name, '/'); name = base ? base + 1 : name;
        strncpy(names[n], name, 47); names[n][47] = 0;
        if (entry.isDirectory()) strncat(names[n], "/", 47 - strlen(names[n]));
        entry.close(); n++;
    }
    dir.close(); return n;
}

}
