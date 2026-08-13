#include "sound.h"
#include "board.h"
#include "pico/stdlib.h"
#include <driver/ledc.h>
#include <driver/gpio.h>
#include <driver/timer.h>
#include <soc/ledc_struct.h>
#include <esp_attr.h>
#include <esp_intr_alloc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#define BEEP_MODE LEDC_LOW_SPEED_MODE
#define BEEP_TIMER LEDC_TIMER_0
#define BEEP_L_CH LEDC_CHANNEL_0
#define BEEP_R_CH LEDC_CHANNEL_1
#define BEEP_RES LEDC_TIMER_10_BIT
#define PCM_RES LEDC_TIMER_8_BIT
#define PCM_CARRIER 312500
#define PCM_N 2048
#define PCM_MASK (PCM_N - 1)
#define TG TIMER_GROUP_1
#define TIDX TIMER_0

static absolute_time_t stop_at;
static bool beep_on;
static volatile int pcm_on;
static volatile uint16_t pcm_rd, pcm_wr;
static volatile uint8_t last_l, last_r;
static uint8_t pcm_l[PCM_N], pcm_r[PCM_N];
static SemaphoreHandle_t pcm_gate;
static int pcm_rate;
static int pcm_timer_inited;

static void pins_idle(void) {
    ledc_stop(BEEP_MODE, BEEP_L_CH, 0);
    ledc_stop(BEEP_MODE, BEEP_R_CH, 0);
    ledc_timer_pause(BEEP_MODE, BEEP_TIMER);
    gpio_set_level(AUDIO_PIN_L, 0);
    gpio_set_level(AUDIO_PIN_R, 0);
}

static inline void IRAM_ATTR pwm_duty8(int ch, uint8_t s) {
    LEDC.channel_group[0].channel[ch].duty.duty = ((uint32_t)s << 4);
    LEDC.channel_group[0].channel[ch].conf1.duty_start = 1;
    LEDC.channel_group[0].channel[ch].conf0.low_speed_update = 1;
}

static bool IRAM_ATTR pcm_timer_cb(void *arg) {
    (void)arg;
    uint16_t rd = pcm_rd;
    uint8_t l = last_l, r = last_r;
    if (rd != pcm_wr) {
        l = pcm_l[rd];
        r = pcm_r[rd];
        pcm_rd = (uint16_t)((rd + 1) & PCM_MASK);
        last_l = l;
        last_r = r;
    }
    pwm_duty8(BEEP_L_CH, l);
    pwm_duty8(BEEP_R_CH, r);
    return false;
}

static void beep_timer_setup(void) {
    ledc_timer_config_t timer = { .speed_mode = BEEP_MODE, .duty_resolution = BEEP_RES,
        .timer_num = BEEP_TIMER, .freq_hz = 1000, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&timer);
    ledc_channel_config_t left = { .gpio_num = AUDIO_PIN_L, .speed_mode = BEEP_MODE,
        .channel = BEEP_L_CH, .timer_sel = BEEP_TIMER, .duty = 0, .hpoint = 0 };
    ledc_channel_config_t right = { .gpio_num = AUDIO_PIN_R, .speed_mode = BEEP_MODE,
        .channel = BEEP_R_CH, .timer_sel = BEEP_TIMER, .duty = 0, .hpoint = 0 };
    ledc_channel_config(&left);
    ledc_channel_config(&right);
}

static void pcm_timer_pause(void) {
    if (!pcm_timer_inited) return;
    timer_pause(TG, TIDX);
    timer_disable_intr(TG, TIDX);
}

/* Timer ISRs run on the core that called timer_init. Install once on core 0
 * so LCD SPI on the Arduino core is not preempted 22k times a second. */
static void pcm_core0_setup(void *arg) {
    (void)arg;
    if (!pcm_timer_inited) {
        timer_config_t tcfg = {
            .alarm_en = TIMER_ALARM_EN,
            .counter_en = TIMER_PAUSE,
            .intr_type = TIMER_INTR_LEVEL,
            .counter_dir = TIMER_COUNT_UP,
            .auto_reload = TIMER_AUTORELOAD_EN,
            .divider = 80,
            .clk_src = TIMER_SRC_CLK_APB,
        };
        timer_init(TG, TIDX, &tcfg);
        timer_isr_callback_add(TG, TIDX, pcm_timer_cb, NULL, ESP_INTR_FLAG_IRAM);
        pcm_timer_inited = 1;
    }
    timer_set_counter_value(TG, TIDX, 0);
    timer_set_alarm_value(TG, TIDX, 1000000u / (uint32_t)pcm_rate);
    timer_enable_intr(TG, TIDX);
    timer_start(TG, TIDX);
    if (pcm_gate) xSemaphoreGive(pcm_gate);
    vTaskDelete(NULL);
}

void sound_init(void) {
    beep_timer_setup();
    stop_at = get_absolute_time();
    beep_on = false;
    pcm_on = 0;
    if (!pcm_gate) pcm_gate = xSemaphoreCreateBinary();
    pins_idle();
}

void sound_pcm_start(int rate_hz) {
    if (rate_hz < 4000) rate_hz = SOUND_PCM_RATE;
    if (pcm_on) sound_pcm_stop();
    beep_on = false;
    pcm_rd = pcm_wr = 0;
    last_l = last_r = 0;
    pcm_rate = rate_hz;

    ledc_timer_config_t timer = { .speed_mode = BEEP_MODE, .duty_resolution = PCM_RES,
        .timer_num = BEEP_TIMER, .freq_hz = PCM_CARRIER, .clk_cfg = LEDC_AUTO_CLK };
    ledc_timer_config(&timer);
    ledc_timer_resume(BEEP_MODE, BEEP_TIMER);
    ledc_channel_config_t left = { .gpio_num = AUDIO_PIN_L, .speed_mode = BEEP_MODE,
        .channel = BEEP_L_CH, .timer_sel = BEEP_TIMER, .duty = 0, .hpoint = 0 };
    ledc_channel_config_t right = { .gpio_num = AUDIO_PIN_R, .speed_mode = BEEP_MODE,
        .channel = BEEP_R_CH, .timer_sel = BEEP_TIMER, .duty = 0, .hpoint = 0 };
    ledc_channel_config(&left);
    ledc_channel_config(&right);
    pwm_duty8(BEEP_L_CH, 0);
    pwm_duty8(BEEP_R_CH, 0);

    if (!pcm_gate) pcm_gate = xSemaphoreCreateBinary();
    while (xSemaphoreTake(pcm_gate, 0) == pdTRUE) {}
    pcm_on = 1;
    if (pcm_timer_inited) {
        timer_set_counter_value(TG, TIDX, 0);
        timer_set_alarm_value(TG, TIDX, 1000000u / (uint32_t)pcm_rate);
        timer_enable_intr(TG, TIDX);
        timer_start(TG, TIDX);
    } else {
        xTaskCreatePinnedToCore(pcm_core0_setup, "pcm0", 2048, NULL, 5, NULL, 0);
        xSemaphoreTake(pcm_gate, pdMS_TO_TICKS(500));
    }
}

void sound_pcm_stop(void) {
    if (pcm_on) {
        pcm_on = 0;
        pcm_timer_pause();
    }
    last_l = last_r = 0;
    pcm_rd = pcm_wr = 0;
    beep_timer_setup();
    pins_idle();
}

void sound_pcm_silence(void) {
    pcm_rd = pcm_wr;
    last_l = last_r = 0;
    if (pcm_on) {
        pwm_duty8(BEEP_L_CH, 0);
        pwm_duty8(BEEP_R_CH, 0);
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
    if (!freq_hz) { sound_off(); return; }
    ledc_timer_resume(BEEP_MODE, BEEP_TIMER);
    if (ledc_set_freq(BEEP_MODE, BEEP_TIMER, freq_hz) != ESP_OK) { sound_off(); return; }
    ledc_set_duty(BEEP_MODE, BEEP_L_CH, 128); ledc_update_duty(BEEP_MODE, BEEP_L_CH);
    ledc_set_duty(BEEP_MODE, BEEP_R_CH, 128); ledc_update_duty(BEEP_MODE, BEEP_R_CH);
    stop_at = make_timeout_time_ms(ms); beep_on = true;
}

void sound_off(void) {
    if (pcm_on) sound_pcm_stop();
    else pins_idle();
    beep_on = false;
}

void sound_update(void) {
    if (beep_on && time_reached(stop_at)) sound_off();
}

void sound_play(const note_t *seq, int count) {
    if (!seq) return;
    for (int i = 0; i < count; i++) {
        if (seq[i].freq == NOTE_REST) { sound_off(); sleep_ms(seq[i].ms); }
        else { sound_beep(seq[i].freq, seq[i].ms + 30); sleep_ms(seq[i].ms); }
    }
    sound_off();
}
