#ifndef EMU_H
#define EMU_H

/* Multi-system emulator front-end. app_emu() is the launcher; each core
 * implements emu_*_run(path). Add new systems by extension in app_emu.c. */

void emu_gb_run(const char *path);

#endif
