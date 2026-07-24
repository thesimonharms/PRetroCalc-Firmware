#ifndef FONT_JAWA_H
#define FONT_JAWA_H

#include <stdint.h>
#include <stdbool.h>

/* Javanese Unicode block U+A980..U+A9DF — 64×64 em-aligned bitmaps.
 * Each glyph: 64 rows × 8 bytes (bit0 = leftmost pixel).
 * Combining marks share the same em box so they stack over base aksara. */
#define JAWA_CP_MIN 0xA980u
#define JAWA_CP_MAX 0xA9DFu
#define JAWA_COUNT  (JAWA_CP_MAX - JAWA_CP_MIN + 1)
#define JAWA_W      64
#define JAWA_H      64
#define JAWA_ROW_BYTES 8
#define JAWA_BYTES  (JAWA_H * JAWA_ROW_BYTES)

extern const uint8_t font_jawa[JAWA_COUNT][JAWA_BYTES];
/* Per-glyph advance: 0 for combining marks, ink width + bearing for bases. */
extern const uint8_t font_jawa_adv[JAWA_COUNT];

const uint8_t *font_jawa_glyph(uint32_t cp);
bool font_jawa_is_mark(uint32_t cp);
bool font_jawa_is_base(uint32_t cp);
int  font_jawa_advance(uint32_t cp);

#endif
