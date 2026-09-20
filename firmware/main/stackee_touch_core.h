// タッチパッド → マウスの「決め方」だけを取り出したもの。
//
// ★ ESP-IDF に一切依存しない。Mac 上でホストビルドして
//   tools/test_touch_host.py から打ち込み、現行 CircuitPython 版
//   (firmware/kmk/stackee_touch.py) と同じ判断をするかを確かめるため。
//   I2C も FreeRTOS もレポート送出も、こちら側 (stackee_touch.c) の仕事。
//
// 移植元の仕様 (firmware/kmk/stackee_touch.py の冒頭に全部書いてある):
//   ・指でなぞる          → ポインタが動く
//   ・短くタップ          → 左クリック (200ms 以内・10ms 以上・移動 30 以下)
//   ・右 30% でタップ     → 右クリック
//   ・STK_TOUCH_SCROLL を押している間だけスクロールになる
//   ・移動量は指数移動平均でならし、デッドゾーン以下は捨てる
//
// ★ 符号は 2 つ別々にかかる (移植元の実機で 180 度ずれた件)。
//     (a) 画面の回転ぶん (rotation=270) … rotate_delta
//     (b) パネル自体の向き (180 度)     … pointer_invert_* で**ポインタにだけ**
//   スクロールは (b) をかけない。移植元の実機でスクロールの向きは正しかった。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  rotation;              // 270
    int  raw_width;             // 320
    int  raw_height;            // 240
    int  sensitivity;           // 4
    float deadzone;             // 0
    float smoothing;            // 0.4
    int  tap_time_ms;           // 200
    int  tap_min_time_ms;       // 10
    int  tap_max_move;          // 30
    int  click_hold_ms;         // 120
    float right_click_zone;     // 0.3 (負なら左側が右クリック)
    float scroll_sensitivity;   // 0.05
    bool invert_x, invert_y;                // 共有の差分ごと反転
    bool pointer_invert_x, pointer_invert_y; // ポインタだけ反転
} stackee_touch_cfg_t;

// 既定値 (firmware/kmk/stackee_touch.py の TouchpadMouse.__init__ と同じ)。
void stackee_touch_cfg_default(stackee_touch_cfg_t *out);

typedef enum {
    STACKEE_TOUCH_EV_NONE = 0,
    STACKEE_TOUCH_EV_MOVE,      // ポインタ移動 (dx, dy)
    STACKEE_TOUCH_EV_SCROLL,    // スクロール (v = 縦, h = 横)
    STACKEE_TOUCH_EV_CLICK,     // タップ (button = 1 左 / 2 右)
} stackee_touch_evkind_t;

typedef struct {
    uint8_t kind;
    int8_t  dx, dy;             // MOVE
    int8_t  v, h;               // SCROLL
    uint8_t button;             // CLICK (1 = 左 / 2 = 右)
} stackee_touch_ev_t;

#define STACKEE_TOUCH_EV_MAX 4

typedef struct {
    stackee_touch_cfg_t cfg;
    bool     scroll_mode;
    // 直前の接触点。touching=false なら「離れている」
    bool     touching;
    int      last_x, last_y;
    float    smoothed_dx, smoothed_dy;
    float    scroll_acc_x, scroll_acc_y;
    // タップ判定
    uint32_t touch_start_ms;
    int      start_screen_x;
    int      start_raw_x, start_raw_y;
    float    total_move;
    // 数えるもの (console の touch.status に出す)
    uint32_t points;            // 接触ありで進めた回数
    uint32_t releases;
    uint32_t taps_left;
    uint32_t taps_right;
    uint32_t taps_rejected;     // 時間か移動量で却下したもの
} stackee_touch_state_t;

void stackee_touch_state_init(stackee_touch_state_t *st,
                              const stackee_touch_cfg_t *cfg);

// 動きの積み上げだけ捨てる (スクロールキーの離し・接触の切れ目)。
void stackee_touch_reset_motion(stackee_touch_state_t *st);

// 1 回ぶん進める。
//   have_point = false なら「離れている」
//   now_ms は単調増加のミリ秒
// 返すのは out に書いたイベント数 (0..STACKEE_TOUCH_EV_MAX)。
int stackee_touch_step(stackee_touch_state_t *st, bool have_point,
                       int x, int y, uint32_t now_ms,
                       stackee_touch_ev_t *out, int out_max);

// 生座標 → 回転後の画面 X (右クリック領域の判定に使う)。テストから直接見る。
int stackee_touch_screen_x(const stackee_touch_cfg_t *cfg, int x, int y);
void stackee_touch_rotate_delta(const stackee_touch_cfg_t *cfg,
                                int dx, int dy, int *out_dx, int *out_dy);

// FT6336 の生バイト列 (レジスタ 0x00 から 16 バイト) から 1 点目を取り出す。
// 接触が無ければ false。★ ここも純粋な関数にしてテストする
//   (0xFFFF の幽霊点を捨てる・接触数 3 以上を 2 に丸める、を確かめるため)。
bool stackee_touch_parse(const uint8_t *buf16, int *out_x, int *out_y,
                         int *out_count);

#ifdef __cplusplus
}
#endif
