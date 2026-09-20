// QMK の wait_ms / wait_us。実体は qmk_port_stubs.c。
//
// ★ input タスク (最高優先度) からは呼ばれない。QMK の中で wait を使うのは
//   起動時とブートローダへ飛ぶ直前だけ。
#pragma once

#include <stdint.h>

void wait_ms(uint32_t ms);
void wait_us(uint32_t us);

// QMK の GPIO マトリクス用の待ち。Stackee は I2C なので使われないが、
// matrix_common.c の弱い実装がこの名前を参照する。
#define waitInputPinDelay() wait_us(1)
