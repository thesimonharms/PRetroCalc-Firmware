#ifndef PICO_BINARY_INFO_H
#define PICO_BINARY_INFO_H
/* uf2loader binary-info is RP-only; no-ops on ESP32. */
#define bi_decl(x)            ((void)0)
#define bi_program_name(s)    0
#define bi_program_description(s) 0
#define bi_program_version_string(s) 0
#endif
