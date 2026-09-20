// hid_out タスク (送信キュー -> USB / BLE)。DESIGN.md §3。
//
// 送信先の選び方そのものは stackee_hid_dest.h。ここは「キューから取って出す」
// タスクだけを持つ。
#pragma once

#include <stdint.h>

void stackee_hid_out_start(void);

// 送信先が繋がっていなかったために捨てたレポートの数。
uint32_t stackee_hid_out_dropped_no_dest(void);
