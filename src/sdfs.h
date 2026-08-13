#ifndef SDFS_H
#define SDFS_H

#include <stdbool.h>
#include <stdint.h>

bool sdfs_init(void);          /* mount /, returns false if no card */
const char *sdfs_diag(void);   /* short mount diagnostic for UI */
bool sdfs_mkdir(const char *path);
bool sdfs_remove(const char *path);
bool sdfs_read_file(const char *path, char *buf, uint32_t max, uint32_t *out_len);
bool sdfs_write_file(const char *path, const char *buf, uint32_t len);
bool sdfs_append_file(const char *path, const char *buf, uint32_t len);
bool sdfs_exists(const char *path);
/* Sequential read of one file (ROM load). Only one handle at a time. */
bool sdfs_open_ro(const char *path);
void sdfs_close_ro(void);
uint32_t sdfs_ro_size(void);
bool sdfs_ro_seek(uint32_t off);
int  sdfs_ro_read(void *buf, uint32_t len); /* bytes read, or -1 */
/* list up to max names into out (each name max 48 chars). returns count. */
int  sdfs_list_dir(const char *path, char names[][48], int max, bool dirs_only);

#endif
