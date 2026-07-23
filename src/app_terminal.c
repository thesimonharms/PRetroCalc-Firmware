/* PRetroCalc OS - terminal shell (BASIC-flavored command line) */
#include "apps.h"
#include "os.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "sdfs.h"
#include "pscript.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static char script_buf[8192];

static void cmd_help(void) {
    os_print(
        "Commands:\n"
        "  help            this text\n"
        "  ls [dir]        list SD card files\n"
        "  cat <file>      print file\n"
        "  run <file.ps>   run PicoScript program\n"
        "  edit <file>     text editor\n"
        "  rm <file>       delete file\n"
        "  mem             free memory info\n"
        "  bat             battery status\n"
        "  beep [f] [ms]   make a sound\n"
        "  ver             firmware version\n"
        "  apps            list applications\n"
        "  exit            back to launcher\n"
        "  reboot          reboot device\n"
        "  off             power off\n");
}

static void cmd_ls(const char *dir) {
    if (!os_sd_present) { os_print("No SD card.\n"); return; }
    char names[32][48];
    int n = sdfs_list_dir(dir && *dir ? dir : "/", names, 32, false);
    if (n == 0) { os_print("(empty)\n"); return; }
    for (int i = 0; i < n; i++) { os_print(names[i]); os_print("\n"); }
}

static void cmd_cat(const char *path) {
    uint32_t len = 0;
    if (!sdfs_read_file(path, script_buf, sizeof(script_buf), &len)) { os_print("Cannot open file.\n"); return; }
    os_print(script_buf);
    os_print("\n");
}

static void trim(char *s) {
    while (*s == ' ') memmove(s, s + 1, strlen(s));
    int n = strlen(s);
    while (n > 0 && s[n-1] == ' ') s[--n] = 0;
}

static void exec_line(char *line) {
    trim(line);
    if (line[0] == 0) return;

    char *sp = strchr(line, ' ');
    char *args = sp ? sp + 1 : (char *)"";
    if (sp) *sp = 0;
    trim(args);
    for (char *c = line; *c; c++) *c = tolower((uint8_t)*c);

    if (!strcmp(line, "help") || !strcmp(line, "?")) cmd_help();
    else if (!strcmp(line, "ls") || !strcmp(line, "dir")) cmd_ls(args);
    else if (!strcmp(line, "cat") || !strcmp(line, "type")) cmd_cat(args);
    else if (!strcmp(line, "run")) {
        uint32_t len = 0;
        if (!sdfs_read_file(args, script_buf, sizeof(script_buf), &len)) { os_print("Cannot open script.\n"); return; }
        pscript_run(script_buf);
    }
    else if (!strcmp(line, "edit")) { app_editor(); os_gem_desktop_bg(); os_term_attach_window(); os_clear_screen(); os_print("(back in terminal)\n"); }
    else if (!strcmp(line, "rm") || !strcmp(line, "del")) {
        os_print(f_unlink(args) == FR_OK ? "Deleted.\n" : "Delete failed.\n");
    }
    else if (!strcmp(line, "mem")) {
        extern char __StackLimit, __bss_end__;
        os_printf("Stack free: ~%d bytes\n", (int)(&__StackLimit - &__bss_end__));
        os_printf("Script buffer: %d bytes\n", (int)sizeof(script_buf));
    }
    else if (!strcmp(line, "bat")) {
        int b = kbd_battery_percent();
        os_printf("Battery: %d%%\n", b);
    }
    else if (!strcmp(line, "beep")) {
        int f = 880, ms = 100;
        if (*args) { f = atoi(args); char *c = strchr(args, ' '); if (c) ms = atoi(c + 1); }
        sound_beep(f, ms);
    }
    else if (!strcmp(line, "ver")) os_print("PRetroCalc OS v1.0 (RP2350 / Pico 2 W)\n");
    else if (!strcmp(line, "apps")) {
        for (int i = 0; i < app_count; i++) os_printf("%-10s %s\n", apps[i].name, apps[i].desc);
    }
    else if (!strcmp(line, "reboot")) watchdog_reboot(0, 0, 0);
    else if (!strcmp(line, "off")) kbd_power_off();
    else if (!strcmp(line, "cls") || !strcmp(line, "clear")) os_clear_screen();
    else if (!strcmp(line, "exit") || !strcmp(line, "quit")) {}
    else {
        /* try to run as app name */
        for (int i = 0; i < app_count; i++)
            if (!strcmp(line, apps[i].name)) {
                apps[i].run();
                os_gem_desktop_bg(); os_term_attach_window(); os_clear_screen();
                os_print("(back in terminal)\n");
                return;
            }
        os_print("Unknown command. Type 'help'.\n");
    }
}

void app_terminal(void) {
    os_gem_desktop_bg();
    os_term_attach_window();
    os_term_init();
    os_clear_screen();
    os_set_color(GEM_BLACK, GEM_WHITE);
    os_print("PRetroCalc OS v1.0 - 520K RAM - RP2350 @ 200MHz\n");
    os_print("Type 'help' for commands. ESC to exit.\n\n");
    static char line[120];
    for (;;) {
        os_set_color(GEM_GREEN, GEM_WHITE);
        os_print("> ");
        os_set_color(GEM_BLACK, GEM_WHITE);
        int r = os_read_line(line, sizeof(line));
        if (r < 0) { os_term_fullscreen(); return; }  /* ESC */
        if (!strcmp(line, "exit") || !strcmp(line, "quit")) { os_term_fullscreen(); return; }
        exec_line(line);
    }
}
