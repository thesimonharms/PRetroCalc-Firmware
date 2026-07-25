#ifndef PICO_STDLIB_H
#define PICO_STDLIB_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>

typedef int64_t absolute_time_t; /* microseconds since boot */
static inline absolute_time_t get_absolute_time(void) { return esp_timer_get_time(); }
static inline absolute_time_t make_timeout_time_ms(uint32_t ms) { return esp_timer_get_time() + (int64_t)ms * 1000; }
static inline bool time_reached(absolute_time_t t) { return esp_timer_get_time() >= t; }
static inline uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t / 1000); }
static inline int64_t absolute_time_diff_us(absolute_time_t from, absolute_time_t to) { return to - from; }
static inline void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static inline void sleep_us(uint64_t us) { esp_rom_delay_us((uint32_t)us); }
static inline void stdio_init_all(void) {}
static inline void tight_loop_contents(void) { taskYIELD(); }

#endif
