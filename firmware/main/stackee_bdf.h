// BDF (ビットマップフォント) の必要な字だけを展開する。
//
// 上段の数字は /efont/ h24 (半角 12x24) を ASCII 95 文字に絞った
// stackee_assets/status_h24.bdf。表示に使うのは "0123456789%-? " の 14 文字
// だけなので、起動時にそこだけ読んで画素を持つ (描画中にファイルを読まない
// という現行 CircuitPython 版の約束をそのまま守る)。
//
// ESP-IDF に依存しない。ホストビルドでそのまま動く。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACKEE_BDF_MAX_GLYPHS 16
#define STACKEE_BDF_MAX_ROWS   32

typedef struct {
    int32_t  code;                          // ASCII コード
    int16_t  w, h, xo, yo, adv;             // BBX と DWIDTH
    // 1 行ぶんの画素。**左端が最上位ビット (bit15)**。
    uint16_t rows[STACKEE_BDF_MAX_ROWS];
} stackee_bdf_glyph_t;

typedef struct {
    int ascent;
    int descent;
    int count;
    stackee_bdf_glyph_t glyphs[STACKEE_BDF_MAX_GLYPHS];
} stackee_bdf_font_t;

// text は NUL 終端でなくてよい (len で切る)。wanted は欲しい文字の並び。
// 欲しい文字が 1 つでも欠けていたら false (呼び出し側は内蔵フォントへ倒す)。
bool stackee_bdf_parse(const char *text, size_t len, const char *wanted,
                       stackee_bdf_font_t *out);

const stackee_bdf_glyph_t *stackee_bdf_glyph(const stackee_bdf_font_t *font, char c);
