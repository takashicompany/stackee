// NVS に置く小物。QMK の EEPROM (ブロブ) は qmk_port.h 側の宣言。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// HID の送信先 (0 = BLE / 1 = USB)。stackee_hid_dest.c から使う。
bool stackee_nvs_load_hid_dest(uint8_t *out);
bool stackee_nvs_save_hid_dest(uint8_t value);

// NimBLE のボンドのうち CCCD (通知の購読) の記録だけを消す。鍵は残すので、
// Mac 側でペアリングを削除する必要は無い。消した件数、失敗なら -1。
int stackee_nvs_drop_nimble_cccd(void);
