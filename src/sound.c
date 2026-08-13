/* PRetroCalc OS - dual PWM speaker beeper + PCM DAC */
#include "sound.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

static uint slice_l, slice_r;
static absolute_time_t stop_at;
static repeating_timer_t pcm_timer;
static volatile int pcm_on;
static volatile uint16_t pcm_rd, pcm_wr;
static volatile uint8_t last_l, last_r;
#define PCM_N 2048
#define PCM_MASK (PCM_N - 1)
static uint8_t pcm_l[PCM_N], pcm_r[PCM_N];

static void pwm_tone_off(void) {
    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
    pwm_set_enabled(slice_l, false);
    pwm_set_enabled(slice_r, false);
}

void sound_init(void) {
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
    slice_l = pwm_gpio_to_slice_num(AUDIO_PIN_L);
    slice_r = pwm_gpio_to_slice_num(AUDIO_PIN_R);
    pwm_tone_off();
    stop_at = get_absolute_time();
    pcm_on = 0;
}

static bool pcm_cb(repeating_timer_t *t) {
    (void)t;
    uint16_t rd = pcm_rd;
    uint8_t l = last_l, r = last_r;
    if (rd != pcm_wr) {
        l = pcm_l[rd];
        r = pcm_r[rd];
        pcm_rd = (uint16_t)((rd + 1) & PCM_MASK);
        last_l = l;
        last_r = r;
    }
    pwm_set_gpio_level(AUDIO_PIN_L, l);
    pwm_set_gpio_level(AUDIO_PIN_R, r);
    return true;
}

void sound_pcm_start(int rate_hz) {
    if (rate_hz < 4000) rate_hz = SOUND_PCM_RATE;
    if (pcm_on) sound_pcm_stop();
    pcm_rd = pcm_wr = 0;
    last_l = last_r = 0;
    gpio_set_function(AUDIO_PIN_L, GPIO_FUNC_PWM);
    gpio_set_function(AUDIO_PIN_R, GPIO_FUNC_PWM);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_int(&cfg, 1);
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(slice_l, &cfg, true);
    pwm_init(slice_r, &cfg, true);
    pwm_set_gpio_level(AUDIO_PIN_L, 0);
    pwm_set_gpio_level(AUDIO_PIN_R, 0);
    int64_t us = -1000000 / rate_hz;
    add_repeating_timer_us(us, pcm_cb, NULL, &pcm_timer);
    pcm_on = 1;
}

void sound_pcm_stop(void) {
    if (pcm_on) {
        cancel_repeating_timer(&pcm_timer);
        pcm_on = 0;
    }
    last_l = last_r = 0;
    pcm_rd = pcm_wr = 0;
    pwm_tone_off();
}

void sound_pcm_silence(void) {
    pcm_rd = pcm_wr;
    last_l = last_r = 0;
    if (pcm_on) {
        pwm_set_gpio_level(AUDIO_PIN_L, 0);
        pwm_set_gpio_level(AUDIO_PIN_R, 0);
    }
}

int sound_pcm_write(uint8_t left, uint8_t right) {
    if (!pcm_on) return 0;
    uint16_t wr = pcm_wr;
    uint16_t nx = (uint16_t)((wr + 1) & PCM_MASK);
    if (nx == pcm_rd) return 0;
    pcm_l[wr] = left;
    pcm_r[wr] = right;
    pcm_wr = nx;
    return 1;
}

void sound_beep(uint32_t freq_hz, uint32_t ms) {
    if (pcm_on) return;
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
    pwm_set_gpio_level(AUDIO_PIN_L, top / 8);
    pwm_set_gpio_level(AUDIO_PIN_R, top / 8);
    stop_at = make_timeout_time_ms(ms);
}

void sound_off(void) {
    if (pcm_on) sound_pcm_stop();
    else pwm_tone_off();
}

void sound_update(void) {
    if (!pcm_on && time_reached(stop_at)) sound_off();
}

void sound_play(const note_t *seq, int count) {
    for (int i = 0; i < count; i++) {
        if (seq[i].freq == NOTE_REST) {
            sound_off();
            sleep_ms(seq[i].ms);
        } else {
            sound_beep(seq[i].freq, seq[i].ms + 30);
            sleep_ms(seq[i].ms);
        }
    }
    sound_off();
}
