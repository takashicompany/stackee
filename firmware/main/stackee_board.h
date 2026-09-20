// CoreS3 の電源まわり。内部 I2C (SDA=G12 / SCL=G11) に AXP2101 と AW9523B が
// いる。CircuitPython の board.c (boards/m5stack_cores3/board.c) が起動時に
// 書いている値をそのまま書く。ここを飛ばすと LCD のバックライトも
// リセット解除も来ないので画面は真っ暗のままになる。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t stackee_board_init(void);

// ---------------------------------------------------------------------------
// 内部 I2C の相乗り (段階 3)
// ---------------------------------------------------------------------------
// ★ このバスには AXP2101 (電池) / AW9523B (電源とリセット) / ES7210 (マイク) /
//   AW88298 (スピーカー) / FT6336 (タッチ) がぶら下がっている。触るタスクが
//   ui・audio・console と複数あるので、**入出力は必ずここの錠を通す**。
//   ESP-IDF の i2c_master にもバス錠はあるが、こちらは
//   「読んで書き換えて書き戻す」を割り込ませないためのもの。
esp_err_t stackee_board_i2c_add(uint8_t address, i2c_master_dev_handle_t *out);
esp_err_t stackee_board_i2c_write(i2c_master_dev_handle_t dev,
                                  const uint8_t *buf, size_t len);
esp_err_t stackee_board_i2c_write_read(i2c_master_dev_handle_t dev,
                                       const uint8_t *w, size_t wlen,
                                       uint8_t *r, size_t rlen);

// AW9523B の P0 の 1 ビットを立てる / 落とす。戻り値は「元から立っていたか」
// (1 = 立っていた / 0 = 落ちていた / -1 = 読めない)。
// スピーカーのアンプ有効ビット (P0_2) をここから触る。
int stackee_board_aw9523_p0(uint8_t mask, bool on);

// AXP2101 のレジスタの 1 ビットを立てる / 落とす / 読むだけ。
// on = 1 で立て、0 で落とし、-1 なら読むだけ。
// 返すのは**操作後のレジスタの値** (読めなければ -1)。
// ★ read-modify-write なので、必ず board の錠を通す。カメラの電源
//   (REG 0x90 bit2 = ALDO3) をここから触る。
int stackee_board_axp_bit(uint8_t reg, uint8_t mask, int on);

// I2C の失敗回数と、バスリセット後のやり直しで通った回数。
void stackee_board_i2c_stats(uint32_t *fail, uint32_t *recovered);

// 電池残量 [%]。読めなければ -1 (AXP2101 REG 0xA4、値域外は捨てる)。
int stackee_board_battery_percent(void);
// 電池電圧 [mV] (VBAT ADC)。読めなければ -1。
int stackee_board_battery_mv(void);
// AXP2101 の任意のレジスタを読む (診断用。console の axp.read)。
int stackee_board_axp_read(uint8_t reg);
int stackee_board_axp_write(uint8_t reg, uint8_t value);   // 0 / -1
// 電源を切る (Soft PWROFF)。戻すには電源ボタン。長押し検出からだけ呼ぶ。
void stackee_board_power_off(void);

// 充電中か。読めなければ -1 (AXP2101 REG 0x01 bit6:5 == 01)。
int stackee_board_charging(void);
