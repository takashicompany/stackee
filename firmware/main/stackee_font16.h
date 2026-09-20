// 16 px の日本語ビットマップフォント (字幕用)。
//
// 素材は tools/gen_font16.py が東雲 BDF から作る assets/font16.bin
// (半角 8x16 + 全角 16x16、Public Domain)。起動時に FAT から PSRAM へ
// 丸ごと読み、以後は**その 1 枚の配列を読むだけ**。顔 (faces.bin) と同じ流儀で、
// 描画中にファイルを読まない・割り当てもしない。
//
// ★ 性能の約束 (README §21):
//   ・字形を引くのは二分探索だけ = O(log n)。7,037 字で 13 回の比較。
//   ・malloc しない。返すのは font16.bin の中を指すポインタ。
//   ・ESP-IDF に依存しない。ホストビルド (hostbuild/subtitle_main.c) で
//     実機とまったく同じ実体を走らせ、Python 側の参照実装と突き合わせる。
//
// ■ font16.bin の形 (すべてリトルエンディアン。詳しくは tools/gen_font16.py)
//    0  "STKFNT16"   8 B
//    8  u16 version / u16 height / u16 narrow_count / u16 wide_count
//   16  u32 narrow_codes_off / narrow_bits_off / wide_codes_off /
//       wide_bits_off / total_bytes
//   36  u32 crc32 (40 バイト目から末尾まで)
//   40  半角の Unicode 表 (u16 昇順) → 字形 (1 字 16 B)
//       全角の Unicode 表 (u16 昇順) → 字形 (1 字 32 B、1 行 2 B、左が上位)
//
// ★ 幅ごとに区画を分けてあるので「何番目の字か」から掛け算だけで字形の場所が
//   出る。位置の表を持たずに済み、索引は Unicode の 2 バイトだけで足りる。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACKEE_FONT16_HEIGHT 16
// 字形が無いときに代わりに出す字 (〓 GETA MARK)。
#define STACKEE_FONT16_TOFU   0x3013u

typedef struct {
    const uint8_t *data;            // font16.bin の先頭 (持ち主は呼び手)
    size_t         len;
    int            narrow_count;    // 8x16
    int            wide_count;      // 16x16
    const uint8_t *narrow_codes;    // u16 昇順
    const uint8_t *narrow_bits;
    const uint8_t *wide_codes;
    const uint8_t *wide_bits;
    bool           ok;
} stackee_font16_t;

typedef struct {
    const uint8_t *rows;    // 16 行。幅 8 なら 1 行 1 B、16 なら 1 行 2 B
    int            width;   // 8 か 16
} stackee_font16_glyph_t;

// 読み込んだバイト列を検分する (目印・版・長さ・CRC32)。data は呼び手が
// 持ち続けること (中身をコピーしない)。
bool stackee_font16_open(stackee_font16_t *f, const void *data, size_t len);

static inline bool stackee_font16_ready(const stackee_font16_t *f) {
    return f != NULL && f->ok;
}

// 字形を引く。無ければ false (呼び手が 〓 に倒す)。
bool stackee_font16_glyph(const stackee_font16_t *f, uint32_t cp,
                          stackee_font16_glyph_t *out);

// 字形が無ければ 〓 で代替する。font16.bin が無ければ false。
bool stackee_font16_glyph_or_tofu(const stackee_font16_t *f, uint32_t cp,
                                  stackee_font16_glyph_t *out);

// その字が進む画素数 (8 か 16)。引けなければ 0。
int stackee_font16_advance(const stackee_font16_t *f, uint32_t cp);

// UTF-8 を 1 文字ぶん進める。壊れていたら 1 バイト進めて 0xFFFD を返す。
// 文字列の終わりでは NULL を返す。
const char *stackee_font16_utf8(const char *p, uint32_t *cp);

// 文字列を描くのに要る画素数。切らずに丸ごと測る。
int stackee_font16_text_px(const stackee_font16_t *f, const char *utf8);

// max_px に収まる範囲の**バイト数**。はみ出す字は入れない (途中で切らない)。
size_t stackee_font16_fit(const stackee_font16_t *f, const char *utf8, int max_px);
