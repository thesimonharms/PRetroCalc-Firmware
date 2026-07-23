#ifndef PSCRIPT_H
#define PSCRIPT_H

/* PicoScript - tiny scripting language for user programs on PRetroCalc OS.
 * Scripts live in SCRIPTS/ on the SD card and can be run from the terminal
 * or FILES app. Language is compiled on the fly to a bytecode VM (fast). */
void pscript_run(const char *source);

#endif
