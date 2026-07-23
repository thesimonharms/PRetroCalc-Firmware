#ifndef SDFS_H
#define SDFS_H

#include <stdbool.h>
#include <stdint.h>

bool sdfs_init(void);          /* mount /, returns false if no card */
bool sdfs_read_file(const char *path, char *buf, uint32_t max, uint32_t *out_len);
bool sdfs_write_file(const char *path, const char *buf, uint32_t len);
bool sdfs_append_file(const char *path, const char *buf, uint32_t len);
/* list up to max names into out (each name max 48 chars). returns count. */
int  sdfs_list_dir(const char *path, char names[][48], int max, bool dirs_only);

#endif
