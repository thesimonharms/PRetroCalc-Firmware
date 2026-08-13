#ifndef S3_PIE_H
#define S3_PIE_H

#include <stddef.h>
#include <stdint.h>

/* ESP32-S3 PIE (128-bit SIMD) fill/copy. Handles any size/alignment;
 * 16-byte aligned bulk uses ee.vld/ee.vst. */
void s3_pie_memset(void *dst, uint8_t val, size_t n);
void s3_pie_memcpy(void *dst, const void *src, size_t n);

#endif
