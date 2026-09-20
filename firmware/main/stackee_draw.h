// 画素を置くところ。顔 (4bpp) とステータスバー (2bpp アイコン + BDF) を
// RGB565 のフレームバッファへ描く。
//
// ★ ESP-IDF に依存しない。**実機とホストテストで同じ実体**を使う。
//   Mac 側の期待値 (tools/render_expected.py) と食い違ったとき、
//   「本体の描画が違う」のか「期待値生成が違う」のかを切り分けられるように、
//   ホストビルド (hostbuild/render_main.c) が同じ素材をこの実体へ通して
//   CRC を出す (tools/test_render_host.py)。
//
// フレームバッファの並びはパネルがそのまま受け取る形:
//   RGB565 / 1 画素 2 バイト / **上位バイトが先** / 行ストライドは stride。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stackee_bdf.h"
#include "stackee_font16.h"
#include "stackee_icons.h"

// ---- 字幕の帯 (scratchpad/subtitle_design.md) -------------------------------
// 顔は y=50..289 (240 px)。その下の 30 px が空いているので、そこに
// 1 行だけ出す。**顔と領域が重ならない**ので、顔の差分描画と字幕は
// お互いを描き直さない。
#define STACKEE_SUB_Y       290
#define STACKEE_SUB_HEIGHT  30
// テレビ字幕と同じ 1 行 15 桁 (全角 1 桁 = 16 px、半角 0.5 桁 = 8 px)。
// 15 桁 x 16 px = 240 px = 画面の幅ちょうど。
#define STACKEE_SUB_COLS    15
#define STACKEE_SUB_WIDTH   (STACKEE_SUB_COLS * STACKEE_FONT16_HEIGHT)
#define STACKEE_SUB_BG      0x000000u
#define STACKEE_SUB_FG      0xFFFFFFu

typedef struct {
    uint8_t *fb;
    int      stride;        // 1 行のバイト数
    int      width;
    int      height;
} stackee_canvas_t;

// 上段バーに出す状態。これだけで描画が決まる (時刻にも乱数にも依存しない)。
typedef struct {
    int             battery;        // 0..100、読めなければ -1
    bool            charging;
    int             volume;         // 0..100
    char            wifi[16];       // stackee_wifi の状態名 ("off" / "up" ...)
    stackee_link_t  link;
    bool            ble_connected;
} stackee_bar_state_t;

uint16_t stackee_draw_rgb565(uint32_t rgb);

void stackee_draw_fill(const stackee_canvas_t *c, int x, int y, int w, int h,
                       uint16_t color);

// 顔シート (4bpp、幅 size、縦に count 枚) の frame の矩形 [sx, sx+w) x
// [sy, sy+h) を、画面の (dx+sx, dy+sy) へ置く。
void stackee_draw_face(const stackee_canvas_t *c, const uint8_t *sheet, int size,
                       int frame, int dx, int dy, int sx, int sy, int w, int h);

// アイコンシート (2bpp、24x24 を縦に 18 枚)。濃さ 0 は透明。
void stackee_draw_icon(const stackee_canvas_t *c, const uint8_t *sheet, int tile,
                       uint32_t color, uint32_t bg, int x, int y);

// 文字を 1 行。x は左端、y_mid は高さ box_h の枠の縦中央。
// font が NULL なら内蔵 8x8 を 12x24 へ引き伸ばして出す。
void stackee_draw_text(const stackee_canvas_t *c, const stackee_bdf_font_t *font,
                       const char *text, uint32_t color, int x, int y_mid,
                       int box_h);

// 上段バーを丸ごと描き直す (黒帯 → アイコン 4 つ → 数字 2 つ)。
void stackee_draw_bar(const stackee_canvas_t *c, const stackee_bar_state_t *st,
                      const uint8_t *icons, const stackee_bdf_font_t *font);

// 字幕の帯 (y=290..319) を丸ごと描き直す。
//   utf8 が NULL か空      … 帯を消す (画面の地の色で塗る)
//   それ以外               … 黒地に白文字で 1 行。240 px を超える字は描かない
// font が NULL / 未読込みなら文字は出さない (帯だけ)。字形が無い字は 〓。
// ★ 割り当てをしない。呼ぶのは ui タスクとコンソールだけ (どちらも ui の錠の中)。
void stackee_draw_subtitle(const stackee_canvas_t *c,
                           const stackee_font16_t *font, const char *utf8);
