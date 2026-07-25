#include "sound.h"
#include "board.h"
#include "pico/stdlib.h"
#include <driver/ledc.h>

#define BEEP_MODE LEDC_LOW_SPEED_MODE
#define BEEP_TIMER LEDC_TIMER_0
#define BEEP_L_CH LEDC_CHANNEL_0
#define BEEP_R_CH LEDC_CHANNEL_1
#define BEEP_RES LEDC_TIMER_10_BIT
static absolute_time_t stop_at;
static bool active;

void sound_init(void) {
    ledc_timer_config_t timer = { .speed_mode = BEEP_MODE, .duty_resolution = BEEP_RES,
        .timer_num = BEEP_TIMER, .freq_hz = 1000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&timer);
    ledc_channel_config_t left = { .gpio_num = AUDIO_PIN_L, .speed_mode = BEEP_MODE,
        .channel = BEEP_L_CH, .timer_sel = BEEP_TIMER, .duty = 0, .hpoint = 0 };
    ledc_channel_config_t right = { .gpio_num = AUDIO_PIN_R, .speed_mode = BEEP_MODE,
        .channel = BEEP_R_CH, .timer_sel = BEEP_TIMER, .duty = 0, .hpoint = 0 };
    ledc_channel_config(&left); ledc_channel_config(&right);
    stop_at = get_absolute_time(); active = false;
}
void sound_beep(uint32_t freq_hz, uint32_t ms) {
    if (!freq_hz) { sound_off(); return; }
    if (ledc_set_freq(BEEP_MODE, BEEP_TIMER, freq_hz) != ESP_OK) { sound_off(); return; }
    ledc_set_duty(BEEP_MODE, BEEP_L_CH, 128); ledc_update_duty(BEEP_MODE, BEEP_L_CH);
    ledc_set_duty(BEEP_MODE, BEEP_R_CH, 128); ledc_update_duty(BEEP_MODE, BEEP_R_CH);
    stop_at = make_timeout_time_ms(ms); active = true;
}
void sound_off(void) {
    ledc_set_duty(BEEP_MODE, BEEP_L_CH, 0); ledc_update_duty(BEEP_MODE, BEEP_L_CH);
    ledc_set_duty(BEEP_MODE, BEEP_R_CH, 0); ledc_update_duty(BEEP_MODE, BEEP_R_CH);
    active = false;
}
void sound_update(void) { if (active && time_reached(stop_at)) sound_off(); }
void sound_play(const note_t *seq, int count) {
    if (!seq) return;
    for (int i = 0; i < count; i++) {
        if (seq[i].freq == NOTE_REST) { sound_off(); sleep_ms(seq[i].ms); }
        else { sound_beep(seq[i].freq, seq[i].ms + 30); sleep_ms(seq[i].ms); }
    }
    sound_off();
}
