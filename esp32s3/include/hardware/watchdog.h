#ifndef HARDWARE_WATCHDOG_H
#define HARDWARE_WATCHDOG_H

#include <stdint.h>
#include <esp_system.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    (void)pc; (void)sp; (void)delay_ms;
    esp_restart();
}

#ifdef __cplusplus
}
#endif

#endif
