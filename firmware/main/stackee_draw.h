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

// ---- 顔の切り詰め ----------------------------------------------------------
// 顔の素材 (faces.bin) は 240x240 のまま。**描くときだけ** 上 29 行・下 11 行を
// 捨てて 240x200 にし、空いた 40 px を字幕の帯へ回す。素材も元絵も変えない。
//   ・シートは起動時に 240x240 で展開したあと、その場で 240x200 へ詰め直す
//     (921,600 B -> 768,000 B)。以後、本体は 200 行のシートしか持たない。
//   ・画面に置く位置 (y=50) は変えない。顔は y=50..249。
//
// ★★ 29 / 11 は **32 コマ全部の余白の最小値そのもの**。上を 30 にすると
//   awake/a_00 (最初の非背景が y=29) が、下を 12 にすると camera/a_00
//   (下の余白が 11) が欠ける。この値でだけ **1 画素も落ちない**。
//   落ちる数は tools/render_expected.py の face_lost に出て、ホストテスト
//   (test_render_host.py の test_the_trim_loses_nothing) が 0 を固定している。
//   ここを動かすときは必ず数え直すこと。
//
// ★ 2026-09-21 に一度 **上下 33 px 対称** で入れたが、awake の上 29 px と
//   camera の下 11 px を数え落としていて 5 コマ 856 px 欠けた。欠けない範囲に
//   切り直したのがこの 29 / 11 (経緯は README §23-8)。
#define STACKEE_FACE_SIZE         240
#define STACKEE_FACE_TRIM_TOP     29
#define STACKEE_FACE_TRIM_BOTTOM  11
#define STACKEE_FACE_ROWS \
    (STACKEE_FACE_SIZE - STACKEE_FACE_TRIM_TOP - STACKEE_FACE_TRIM_BOTTOM)

// ---- 字幕の帯 (scratchpad/subtitle_design.md) -------------------------------
// 顔は y=50..249 (200 px)。その下の 70 px が空いているので、そこに
// **3 行**出す。**顔と領域が重ならない**ので、顔の差分描画と字幕は
// お互いを描き直さない。
//   1 行 = 22 px (上 3 px の余白 + 16 px の字形 + 下 3 px の余白)。
//   3 行で 66 px。余りの 4 px は帯の上下へ 2 px ずつ。
#define STACKEE_SUB_LINES   3
#define STACKEE_SUB_LINE_H  22
#define STACKEE_SUB_PAD     ((STACKEE_SUB_LINE_H - STACKEE_FONT16_HEIGHT) / 2)
#define STACKEE_SUB_HEIGHT  70
#define STACKEE_SUB_Y       (320 - STACKEE_SUB_HEIGHT)
// 帯の上下に残る余り (70 - 3*22 = 4 を 2 px ずつ)。
#define STACKEE_SUB_MARGIN \
    ((STACKEE_SUB_HEIGHT - STACKEE_SUB_LINES * STACKEE_SUB_LINE_H) / 2)
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

// 顔シート (4bpp、幅 size、1 コマ rows 行、縦に count 枚) の frame の矩形
// [sx, sx+w) x [sy, sy+h) を、画面の (dx+sx, dy+sy) へ置く。
// ★ 幅と高さを別々に取る。切り詰めたシート (240x174) を扱うため。
void stackee_draw_face(const stackee_canvas_t *c, const uint8_t *sheet, int size,
                       int rows, int frame, int dx, int dy, int sx, int sy,
                       int w, int h);

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

// 字幕の帯 (y=250..319) を丸ごと描き直す。
// ★ 帯は **いつでも黒**。字幕が無いときも黒いまま文字だけ消える
//   (2026-09-21 にユーザーの決定で「消すときは白」をやめた)。起動直後から黒。
//   utf8 が NULL か空      … 文字を消す (黒で塗るだけ)
//   それ以外               … 黒地に白文字。**改行 (\n) で区切って最大 3 行**。
//                            1 行で 240 px を超える字は描かない (途中で切らない)
//                            4 行目以降は捨てる (頁めくりは呼び手の仕事)
// font が NULL / 未読込みなら文字は出さない (帯だけ)。字形が無い字は 〓。
// ★ 割り当てをしない。呼ぶのは ui タスクとコンソールだけ (どちらも ui の錠の中)。
void stackee_draw_subtitle(const stackee_canvas_t *c,
                           const stackee_font16_t *font, const char *utf8);

// 帯に要る幅 [px] = **いちばん長い行**の画素数 (改行は数えない)。
// 1 行だけなら stackee_font16_text_px と同じ値になる。
int stackee_draw_subtitle_px(const stackee_font16_t *font, const char *utf8);
