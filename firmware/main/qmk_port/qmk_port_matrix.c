// QMK のマトリクス。TCA8418 の 5x10 スロットを MATRIX_ROWS x MATRIX_COLS に写す。
//
// QMK 本体 (third_party/qmk/quantum/matrix_common.c) が用意している
// 「CUSTOM MATRIX LITE」の口だけを埋める:
//
//   matrix_init_custom()                   起動時に 1 回
//   matrix_scan_custom(current_matrix[])   1 周ごと。行ビットを埋めて返す
//
// matrix_get_row・matrix_print は QMK 側がそのまま面倒を見る。こちらが持つのは
// 「TCA8418 のイベントを行ビットに直す」ところだけ。
//
// ★ **デバウンスは QMK 側では行っていない** (stackee_qmk_config.h の
//   `DEBOUNCE 0`)。sym_defer_pk.c は押下も解放も DEBOUNCE ms ぶん遅らせる
//   ので、5 ms でもキー → 送出の中央値が 1.5 ms から 6.0 ms へ悪化した
//   (2026-09-16 の実測、RESULTS.md 段階 1)。チャタリングは TCA8418 の
//   ハード側デバウンス (レジスタ DEBOUNCE_DIS = 0 = 有効) が吸っている。
//
// 現行 CircuitPython 版 (firmware/kmk/code.py の TCA8418Scanner) は
// 「スロット番号 = row * 10 + col」でイベントを返す。その番号をそのまま
// 行/列に割り戻すので、配線表は keymap.py の 1 か所にしか無い。
//
// ★ TCA8418 は「押した」「離した」のイベントしかくれない (今どのキーが
//   押されているかを読み直すレジスタが無い)。だから行ビットは
//   イベントで積み上げた状態をこちらが覚えておく必要がある。
#include "matrix.h"

#include <string.h>

#include "qmk_port.h"
#include "quantum.h"

static matrix_row_t s_state[MATRIX_ROWS];   // イベントで積み上げた現在の状態
static volatile bool s_changed;             // 前回の scan 以降に変化したか

void matrix_init_custom(void) {
    memset((void *)s_state, 0, sizeof(s_state));
    s_changed = false;
}

bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    bool changed = s_changed;
    s_changed = false;
    if (changed) {
        memcpy(current_matrix, (const void *)s_state, sizeof(s_state));
    }
    return changed;
}

void stackee_qmk_matrix_event(uint8_t slot, bool pressed) {
    if (slot >= STACKEE_SLOT_COUNT) {
        return;
    }
    uint8_t      row = (uint8_t)(slot / STACKEE_MATRIX_COLS);
    uint8_t      col = (uint8_t)(slot % STACKEE_MATRIX_COLS);
    matrix_row_t bit = ((matrix_row_t)1) << col;
    matrix_row_t before = s_state[row];
    if (pressed) {
        s_state[row] |= bit;
    } else {
        s_state[row] &= (matrix_row_t)~bit;
    }
    if (s_state[row] != before) {
        s_changed = true;
    }
}

void stackee_qmk_matrix_release_all(void) {
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        if (s_state[row] != 0) {
            s_state[row] = 0;
            s_changed = true;
        }
    }
}

uint8_t stackee_qmk_matrix_pressed_count(void) {
    uint8_t count = 0;
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        matrix_row_t bits = s_state[row];
        while (bits) {
            count = (uint8_t)(count + (bits & 1));
            bits >>= 1;
        }
    }
    return count;
}
