// TCA8418 キースキャナ (PORT.A の I2C 0x34、5x10 キーパッド)。
//
// 現行 CircuitPython 版 (firmware/kmk/code.py の TCA8418Scanner) と
// **同じレジスタ設定・同じ読み方**にしてある。レジスタ番号は TI SCPS215G 8.6。
//
// ★ 割り込み線は無い。Grove は 4 線しか無く INT が来ないため
//   (hardware/design-spec.md「INT 線: 引かない (I2C ポーリング)」)。
//   1 ms 周期で INT_STAT と件数レジスタを読む。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t slot;       // 0..49 = row * 10 + col
    bool    pressed;
} stackee_tca_event_t;

// I2C バスを用意する。TCA8418 が居なくても成功を返す (後から挿しても
// つながるように、接続は poll の中で何度でも試す)。
esp_err_t stackee_tca8418_init(void);

// FIFO を空になるまで吸い出し、events[] に詰めて件数を返す。
// 溢れを検出したら、押下中のスロットを全部「離した」として返す。
// つながっていなければ 0 を返し、一定間隔で接続を試す。
int stackee_tca8418_poll(stackee_tca_event_t *events, int max_events);

typedef struct {
    bool     connected;
    uint32_t events;        // 取り出したイベントの総数
    uint32_t overflows;     // FIFO 溢れの回数
    uint32_t io_fails;      // I2C の失敗回数
    uint32_t reconnects;    // つなぎ直した回数
    uint32_t stray;         // 配線の無いスロットから来たイベント
} stackee_tca_stats_t;

void stackee_tca8418_stats(stackee_tca_stats_t *out);
bool stackee_tca8418_connected(void);
