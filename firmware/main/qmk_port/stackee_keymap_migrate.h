// 既定キーマップを変えたときの「移行」。
//
// ★ **なぜ要るのか。** VIA / Remap で配列を 1 度でも書き換えた本体は、
//   以後 **EEPROM (NVS) に保存された配列** で動く。像を新しくしても
//   `main/keymaps/default_keymap.c` は見に行かないので、**新しい既定は
//   黙って無視される**。2026-09-21 に実機で踏んだ: 右下のキーを
//   `MIC(KC_F13)` にした像を入れたのに、保存済みの配列が `KC_F13`
//   (0x0068) のままで、顔がまったく変わらなかった。
//
// 仕組み:
//   ・EEPROM の「キーボード用の 4 バイト」(eeconfig_read_kb /
//     eeconfig_update_kb。VIA はここを触らない) に移行番号を持つ。
//   ・起動時に、未適用の移行を**古いほうから順に**当てる。
//   ・当て終わったら番号を進める。二度と当たらない。
//
// ★ 移行は「**旧い既定のままなら差し替える。ユーザーが変えていたら
//   触らない**」で書くこと。保存済みの配列はユーザーのものなので、
//   知らない値を勝手に上書きしない。
#pragma once

#include <stdint.h>

// いまの移行番号 (この像が知っている最後の段)。
#define STACKEE_KEYMAP_MIGRATION_LATEST 1

// EEPROM に入っている番号。
uint32_t stackee_keymap_migration_level(void);

// 未適用の移行を順に当てて番号を進める。**実際に書き換えたキーの数**を返す
// (0 = 当てるものが無かった / ユーザーが変えていたので触らなかった)。
// ★ 呼ぶのは keyboard_init() のあと 1 回だけ (stackee_qmk_init)。
int stackee_keymap_migrate(void);
