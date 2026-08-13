/* PRetroCalc OS - filesystem layer over FatFS (carlk3 no-OS-FatFS lib) */
#include "sdfs.h"
#include "board.h"
#include "ff.h"
#include "hw_config.h"
#include "pico/stdlib.h"
#include <string.h>

static bool mounted = false;

bool sdfs_init(void) {
    sd_card_t *pSD = sd_get_by_num(0);
    FRESULT fr = f_mount(&pSD->state.fatfs, pSD->state.drive_prefix, 1);
    if (fr != FR_OK) { mounted = false; return false; }
    mounted = true;
    return true;
}

const char *sdfs_diag(void) {
    return mounted ? "ok" : "no card";
}

bool sdfs_mkdir(const char *path) {
    if (!mounted || !path || !*path) return false;
    FRESULT fr = f_mkdir(path);
    return fr == FR_OK || fr == FR_EXIST;
}

bool sdfs_remove(const char *path) {
    if (!mounted || !path || !*path) return false;
    return f_unlink(path) == FR_OK;
}

bool sdfs_read_file(const char *path, char *buf, uint32_t max, uint32_t *out_len) {
    if (!mounted) return false;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK) return false;
    UINT br = 0;
    FRESULT r = f_read(&f, buf, max - 1, &br);
    f_close(&f);
    buf[br] = 0;
    if (out_len) *out_len = br;
    return r == FR_OK;
}

bool sdfs_write_file(const char *path, const char *buf, uint32_t len) {
    if (!mounted) return false;
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT bw;
    FRESULT r = f_write(&f, buf, len, &bw);
    f_close(&f);
    return r == FR_OK && bw == len;
}

bool sdfs_append_file(const char *path, const char *buf, uint32_t len) {
    if (!mounted) return false;
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_OPEN_APPEND) != FR_OK) return false;
    UINT bw;
    FRESULT r = f_write(&f, buf, len, &bw);
    f_close(&f);
    return r == FR_OK && bw == len;
}

bool sdfs_exists(const char *path) {
    if (!mounted || !path || !*path) return false;
    FILINFO fi;
    return f_stat(path, &fi) == FR_OK;
}

static FIL ro;
static bool ro_open;

bool sdfs_open_ro(const char *path) {
    if (!mounted || !path || !*path) return false;
    if (ro_open) { f_close(&ro); ro_open = false; }
    if (f_open(&ro, path, FA_READ) != FR_OK) return false;
    ro_open = true;
    return true;
}

void sdfs_close_ro(void) {
    if (ro_open) { f_close(&ro); ro_open = false; }
}

uint32_t sdfs_ro_size(void) {
    return ro_open ? (uint32_t)f_size(&ro) : 0;
}

bool sdfs_ro_seek(uint32_t off) {
    return ro_open && f_lseek(&ro, off) == FR_OK;
}

int sdfs_ro_read(void *buf, uint32_t len) {
    if (!ro_open || !buf) return -1;
    UINT br = 0;
    if (f_read(&ro, buf, len, &br) != FR_OK) return -1;
    return (int)br;
}

int sdfs_list_dir(const char *path, char names[][48], int max, bool dirs_only) {
    if (!mounted) return 0;
    DIR d;
    if (f_opendir(&d, path) != FR_OK) return 0;
    FILINFO fi;
    int n = 0;
    while (n < max && f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
        if (dirs_only && !(fi.fattrib & AM_DIR)) continue;
        if (fi.fattrib & AM_HID) continue;
        strncpy(names[n], fi.fname, 47);
        names[n][47] = 0;
        if (fi.fattrib & AM_DIR) strncat(names[n], "/", 47 - strlen(names[n]));
        n++;
    }
    f_closedir(&d);
    return n;
}
