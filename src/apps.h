#ifndef APPS_H
#define APPS_H

/* App registry: each app runs until it returns, then control goes back
 * to the launcher. Apps own the whole screen while running. */

typedef struct {
    const char *name;
    const char *desc;
    void (*run)(void);
    int  icon_color;   /* RGB332 */
} app_t;

extern const app_t apps[];
extern const int app_count;

/* built-in apps */
void app_terminal(void);
void app_calc(void);
void app_editor(void);
void app_files(void);
void app_monitor(void);
void app_settings(void);
void app_about(void);
void app_snake(void);
void app_breakout(void);
void app_invaders(void);
void app_life(void);

#endif
