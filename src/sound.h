#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>
#include <stdbool.h>

/* Note frequencies (Hz), octave 4-6 */
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_D5  587
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_G5  784
#define NOTE_A5  880
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_D6  1175
#define NOTE_E6  1319
#define NOTE_G6  1568
#define NOTE_REST 0

typedef struct { uint16_t freq; uint16_t ms; } note_t;

void sound_init(void);
void sound_beep(uint32_t freq_hz, uint32_t ms);  /* async square-wave beep */
void sound_off(void);
void sound_update(void);                          /* call in main loop */
void sound_play(const note_t *seq, int count);    /* blocking melody */

/* classic key-click */
static inline void sound_click(void) { sound_beep(2400, 8); }

#endif
