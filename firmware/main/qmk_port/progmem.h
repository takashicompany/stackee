// AVR の PROGMEM を無効化する。ESP32 のフラッシュは普通に読めるので、
// QMK が pgm_read_* で読んでいるところはそのまま参照で足りる。
#pragma once

#include <stdint.h>
#include <string.h>

#define PROGMEM
#define PSTR(x) x
#define PGM_P const char *
#define memcpy_P(dest, src, n) memcpy(dest, src, n)
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#define pgm_read_ptr(addr) (*(void *const *)(addr))
#define strcmp_P(s1, s2) strcmp(s1, s2)
#define strcpy_P(dest, src) strcpy(dest, src)
#define strlen_P(src) strlen(src)
