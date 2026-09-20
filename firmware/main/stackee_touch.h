// タッチパッド (FT6336 @0x38、内部 I2C)。DESIGN.md §3 の「touch」タスク。
//
//   FT6336 の読み出し → stackee_touch_core の判定 → 送信キュー (Report ID 2)
//
// ★ 判定そのものは stackee_touch_core.c にある (ESP-IDF に依存しない)。
//   こちらは「いつ読むか」「どこへ出すか」だけを持つ。
//
// ★ 人手ゼロで確かめる:
//   console の `touch.status` (読み出し回数・最後の座標・送った移動量) と
//   `touch.inject` (擬似の座標列を流してレポート数を見る)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t stackee_touch_start(void);

// STK_TOUCH_SCROLL の押し離し。押している間だけスクロールになる。
void stackee_touch_set_scroll(bool on);
bool stackee_touch_scroll(void);

typedef struct {
    bool     present;           // FT6336 が居たか
    uint8_t  vendor;            // レジスタ 0xA8 (ベンダ ID)
    uint32_t reads;             // I2C を読んだ回数
    uint32_t read_fails;
    uint32_t points;            // 接触ありで進めた回数
    uint32_t releases;
    uint32_t taps_left;
    uint32_t taps_right;
    uint32_t taps_rejected;
    uint32_t moves;             // 送ったポインタ移動レポート数
    uint32_t scrolls;           // 送ったスクロールレポート数
    uint32_t clicks;            // 送ったボタンレポート数 (押し + 離し)
    int32_t  moved_x, moved_y;  // 送った移動量の累計
    int      last_x, last_y;    // 最後に読めた生座標 (無ければ -1)
    bool     scroll_mode;
    uint32_t loop_max_us;       // 1 周の最大処理時間 (loop_debug 相当)
    uint32_t loop_n;
} stackee_touch_stats_t;

void stackee_touch_stats(stackee_touch_stats_t *out);

// 擬似の座標列を流す (自己テスト)。x/y は 2 個 1 組で n 組。
// step_ms は 1 組ごとに進める時間。最後に「離した」を 1 回入れる。
// 返すのは送ったレポート数。I2C には一切触らない。
int stackee_touch_inject(const int16_t *xy, int pairs, uint32_t step_ms,
                         bool release);
