// 内蔵の 8x8 フォント (段階 0 の起動画面・診断表示用)。
// 実体は tools/gen_font8x8.py が生成する stackee_font8x8.c。
#pragma once

#include <stdint.h>

#define STACKEE_FONT_FIRST  0x20
#define STACKEE_FONT_LAST   0x7E
#define STACKEE_FONT_COUNT  (STACKEE_FONT_LAST - STACKEE_FONT_FIRST + 1)

// [文字][行]。bit0 が左端の画素。
extern const uint8_t stackee_font8x8[STACKEE_FONT_COUNT][8];
