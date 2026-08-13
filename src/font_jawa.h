#ifndef FONT_JAWA_H
#define FONT_JAWA_H

#include <stdint.h>
#include <stdbool.h>

/* Javanese Unicode block U+A980..U+A9DF — dual 48×48 / 64×64 em-aligned
 * bitmaps (bit0 = leftmost). Combining marks share the em box so they stack
 * over base aksara. Pasangan are real OpenType .pas forms (not scaled bases).
 * Call font_jawa_set_px(48|64) to switch at runtime. */
#define JAWA_CP_MIN 0xA980u
#define JAWA_CP_MAX 0xA9DFu
#define JAWA_COUNT  (JAWA_CP_MAX - JAWA_CP_MIN + 1)

/* Active metrics — always use these (not compile-time constants). */
int  font_jawa_px(void);          /* 48 or 64 */
int  font_jawa_w(void);
int  font_jawa_h(void);
int  font_jawa_row_bytes(void);
void font_jawa_set_px(int px);    /* clamps to 48 or 64 */

/* Pasangan placement: blit pasangan bitmap at base_y + dy; reserve `extra`
 * pixels of line descent so the next line clears the stack. */
int  font_jawa_pasangan_dy(void);
int  font_jawa_pasangan_extra(void);

const uint8_t *font_jawa_glyph(uint32_t cp);
const uint8_t *font_jawa_pasangan(uint32_t cp); /* NULL if no pasangan form */
/* Below vowel form for pasangan clusters (u.ns.pas etc.); NULL if none. */
const uint8_t *font_jawa_pas_below(uint32_t cp);
bool font_jawa_is_mark(uint32_t cp);
bool font_jawa_is_below_vowel(uint32_t cp); /* suku / suku mendut / keret */
bool font_jawa_is_medial(uint32_t cp);      /* pengkal / cakra */
bool font_jawa_is_below_mark(uint32_t cp);  /* vowel | medial */
bool font_jawa_is_left_mark(uint32_t cp);   /* taling / dirga mure — visual left */
bool font_jawa_is_right_mark(uint32_t cp);  /* tarung / tolong — visual right */
bool font_jawa_is_base(uint32_t cp);
bool font_jawa_can_pasangan(uint32_t cp);
int  font_jawa_advance(uint32_t cp);
/* Blit dx for left marks (full taling/dirga mure, shifted left of aksara). */
int  font_jawa_left_mark_dx(void);
/* Extra cluster advance: left (taling) / right (tarung) so neighbors don't clip. */
int  font_jawa_left_mark_extra(void);
int  font_jawa_right_mark_extra(void);

#endif
