#ifndef S3_PIE_H
#define S3_PIE_H

#include <stddef.h>
#include <stdint.h>

/* ESP32-S3 PIE (128-bit SIMD) fill/copy. Handles any size/alignment;
 * 16-byte aligned bulk uses ee.vld/ee.vst. */
void s3_pie_memset(void *dst, uint8_t val, size_t n);
void s3_pie_memcpy(void *dst, const void *src, size_t n);
/* 2× horizontal duplicate of n_src uint16 pixels. n_src should be a multiple
 * of 8; src/dst 16-byte aligned. dst must hold 2 * n_src elements. */
void s3_pie_scale2_u16(uint16_t *dst, const uint16_t *src, size_t n_src);

#endif
