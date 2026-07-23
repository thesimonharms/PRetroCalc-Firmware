/* PRetroCalc OS - dual PWM speaker beeper */
#include "sound.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

static uint slice_l, slice_r;
static absolute_time_t stop_at;

void sound_init(void) {
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
    slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
    slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);
    pwm_set_enabled(slice_l, false);
    pwm_set_enabled(slice_r, false);
    stop_at = get_absolute_time();
}

void sound_beep(uint32_t freq_hz, uint32_t ms) {
    if (freq_hz == 0) { sound_off(); return; }
    uint32_t sys = clock_get_hz(clk_sys);
    uint32_t div = sys / (freq_hz * 4096);
    if (div < 1) div = 1;
    if (div > 255) div = 255;
    uint32_t top = sys / (div * freq_hz);
    if (top > 4095) top = 4095;

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&cfg, div);
    pwm_config_set_wrap(&cfg, top);
    pwm_init(slice_l, &cfg, true);
    pwm_init(slice_r, &cfg, true);
    pwm_set_gpio_level(AUDIO_PIN_L, top / 8);   /* ~12% duty: gentle volume */
    pwm_set_gpio_level(AUDIO_PIN_R, top / 8);
    stop_at = make_timeout_time_ms(ms);
}

void sound_off(void) {
    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
    pwm_set_enabled(slice_l, false);
    pwm_set_enabled(slice_r, false);
}

void sound_update(void) {
    if (time_reached(stop_at)) sound_off();
}

void sound_play(const note_t *seq, int count) {
    for (int i = 0; i < count; i++) {
        if (seq[i].freq == NOTE_REST) {
            sound_off();
            sleep_ms(seq[i].ms);
        } else {
            sound_beep(seq[i].freq, seq[i].ms + 30); /* slight overlap */
            sleep_ms(seq[i].ms);
        }
    }
    sound_off();
}
