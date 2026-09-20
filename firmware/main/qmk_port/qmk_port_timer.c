// QMK の時計。third_party/qmk/platforms/timer.h の実装。
//
// QMK は「起動からのミリ秒」しか要求しない (TIMER_DIFF_* で折り返しを扱うので
// 32bit が一周しても壊れない)。ESP-IDF の esp_timer は 64bit マイクロ秒なので、
// 1000 で割って渡すだけ。
//
// ★ ホストビルド (hostbuild/) では時計を外から進められる必要がある
//   (打鍵列テストは「200 ms の境界の両側」を確かめる)。そのため実際の時刻源は
//   stackee_qmk_now_ms() という 1 つの関数に閉じ込め、テスト側はそれを
//   差し替える。
#include "timer.h"

#include "qmk_port.h"

// QMK の platforms/timer.h が extern 宣言している。AVR 版では割り込みで
// 増えるカウンタだが、ここでは使わない。
volatile uint32_t timer_count = 0;

void timer_init(void) {
    timer_count = 0;
}

void timer_clear(void) {
    timer_count = 0;
}

void timer_save(void) {}

void timer_restore(void) {}

uint16_t timer_read(void) {
    return (uint16_t)stackee_qmk_now_ms();
}

uint32_t timer_read32(void) {
    return stackee_qmk_now_ms();
}

uint16_t timer_elapsed(uint16_t last) {
    return TIMER_DIFF_16((uint16_t)stackee_qmk_now_ms(), last);
}

uint32_t timer_elapsed32(uint32_t last) {
    return TIMER_DIFF_32(stackee_qmk_now_ms(), last);
}
