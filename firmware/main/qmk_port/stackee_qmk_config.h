// QMK の config.h に当たるもの。DESIGN.md §5。
//
// QMK は「キーボード定義の config.h をコンパイラの -include で全翻訳単位に
// 差し込む」という作り (qmk_firmware の build_keyboard.mk)。ここでも同じ
// やり方にしてある (main/CMakeLists.txt の -include)。third_party/qmk の
// ファイルには一切触らずに設定を渡せるのはこの口だけ。
#pragma once

// ---------------------------------------------------------------------------
// マトリクス
// ---------------------------------------------------------------------------
// TCA8418 の 5 行 x 10 列 = 50 スロットをそのまま QMK のマトリクスにする。
// 配線があるのは 43 個で、残り 7 個は常に 0 のままになる (DESIGN.md §5)。
#define MATRIX_ROWS 5
#define MATRIX_COLS 10

// ---------------------------------------------------------------------------
// デバウンス: **QMK 側では行わない** (2026-09-16 決定)
// ---------------------------------------------------------------------------
// ★ なぜ 0 か
//
//   実機で key.inject を 20 回流したところ、押下 → HID 送出の遅延が
//   中央値 5.99 ms / 最大 6.52 ms。設計目標 (中央値 2 ms、DESIGN.md §3) に
//   届かない。内訳のほとんどが QMK のデバウンス待ちだった。
//   取り込んである debounce/sym_defer_pk.c は「状態が DEBOUNCE ms 変化
//   しなくなってから確定する」方式なので、**押下も解放も必ず DEBOUNCE ms
//   遅れる**。5 ms がそのまま遅延として乗っていた。
//
//   TCA8418 は**チップの中でデバウンスしてから** FIFO にイベントを積む。
//   TI SCPS215G の Features は "Integrated Debounce Time of 50 us"、
//   6.9 の表は "Debounce ... MAX 60 ms" と桁が食い違っている
//   (hardware/design-spec.md にも記録済み) が、**デバウンスが入っていること
//   自体は両方が言っている**。無効化したいときのための DEBOUNCE_DIS
//   レジスタ (0x29-0x2B) が存在することも、既定で有効である裏づけになる。
//   Stackee のファームはこのレジスタを触らないので、既定のまま有効。
//
//   つまりチャタリングは I2C に乗ってくる前に取れている。そこへ QMK の
//   デバウンスを重ねても、遅延が増えるだけで得るものが無い。
//
// ★ 0 にすると何が起きるか
//   sym_defer_pk.c は `#if DEBOUNCE > 0` の外で `#include "none.c"` に
//   落ちる (ファイル末尾)。= 素通し。ビルドするファイルは変わらないので、
//   CMakeLists.txt には手を入れていない。
//
// ★ 戻し方
//   チャタリングが出たら、まず TCA8418 側のデバウンスを疑う (DEBOUNCE_DIS
//   を誰かが触っていないか)。それでも出るならここを 1 に戻す。5 は不要。
#define DEBOUNCE 0

// ---------------------------------------------------------------------------
// HoldTap (KMK との対応は tools/keycodes.md)
// ---------------------------------------------------------------------------
#define TAPPING_TERM 200

// ★ QMK の「クイックタップ」(タップした直後に同じ HoldTap キーを押し直すと、
//   長押ししてもタップ扱いのまま = オートリピート) を切る。既定では
//   QUICK_TAP_TERM = TAPPING_TERM で **有効**。
//   KMK の HoldTap には repeat= という別の仕組みがあり、keymap.py は
//   どのキーにも指定していない (HoldTapRepeat.NONE)。切らないと
//   「A を打った直後に A/Shift の長押しが効かない」という現行に無い癖が付く。
#define QUICK_TAP_TERM 0

// KMK の prefer_hold / tap_interrupted はキーごとの設定なので、QMK 側も
// キーごとに答える形にする。実体は main/keymaps/default_keymap.c の
// get_hold_on_other_key_press() / get_permissive_hold()。
#define HOLD_ON_OTHER_KEY_PRESS_PER_KEY
#define PERMISSIVE_HOLD_PER_KEY
#define TAPPING_TERM_PER_KEY

// ---------------------------------------------------------------------------
// VIA / dynamic keymap
// ---------------------------------------------------------------------------
#define VIA_ENABLE
#define RAW_ENABLE
#define DYNAMIC_KEYMAP_ENABLE
#define EXTRAKEY_ENABLE

// ---------------------------------------------------------------------------
// マウスキー
// ---------------------------------------------------------------------------
// 現行 CircuitPython 版でも KMK の MouseKeys モジュールを入れている
// (code.py: keyboard.modules.append(MouseKeys()))。QMK 側も同じように
// 有効にする。動作モードは QMK 既定 (加速つき) のまま。
// 送り先は既存の Report ID 2 のコレクション = 将来のタッチパッドと同じ
// 送信キュー (DESIGN.md §5)。
#define MOUSEKEY_ENABLE
// ★ QMK は「マウスのレポートを作る機能があるか」を MOUSE_ENABLE で表す
//   (tmk_core/protocol/report.h の has_mouse_report_changed など)。
//   MOUSEKEY_ENABLE / POINTING_DEVICE_ENABLE のどちらかがあれば立てる、
//   という関係。QMK の本家ビルドは Makefile 側で立てているので、
//   こちらでも明示する。段階 4 のタッチパッドもここに乗る。
#define MOUSE_ENABLE

// 現行 KMK の KEYMAP と同じ 6 層。
#define DYNAMIC_KEYMAP_LAYER_COUNT 6

// マクロの再生間隔 (VIA のマクロ機能。段階 1 では使わないが via.c が要求する)。
#define DYNAMIC_KEYMAP_MACRO_DELAY 10

// ---------------------------------------------------------------------------
// EEPROM (実体は NVS のブロブ。qmk_port/qmk_port_eeprom.c)
// ---------------------------------------------------------------------------
// third_party/qmk/platforms/eeprom.h は EEPROM_CUSTOM が立っていれば
// TOTAL_EEPROM_BYTE_COUNT を EEPROM_SIZE から作る。つまり「自前のドライバ」
// として名乗り出れば QMK 側は 1 文字も直さずに済む。
//
// 内訳 (nvm_eeprom_* の計算どおり):
//   eeconfig       34 B
//   VIA magic       3 B  + layout options 1 B
//   dynamic keymap  6 層 x 5 行 x 10 列 x 2 B = 600 B
//   残り                   マクロ用 (>= 100 B 必要)
#define EEPROM_CUSTOM
#define EEPROM_SIZE 1024

// ---------------------------------------------------------------------------
// 使わない機能を明示的に閉じる
// ---------------------------------------------------------------------------
#define NO_ACTION_ONESHOT       // KMK 側に相当機能が無い
#define NO_MUSIC_MODE
#define NO_DEBUG_disabled

// ESP32-S3 は 32bit。fast_timer_t を 32bit にする (platforms/timer.h)。
#define FAST_TIMER_T_SIZE 32

// ATOMIC_BLOCK は使わない。TCA8418 には割り込み線が無く (hardware/design-spec.md
// 「INT 線: Grove は 4 線しか無く物理的に来ない」)、QMK の状態を触るのは
// input タスク 1 本だけなので、割り込み禁止区間そのものが要らない。
#define IGNORE_ATOMIC_BLOCK

// ---------------------------------------------------------------------------
// 名乗り (VIA 定義 JSON の vendorId/productId と一致させること)
// ---------------------------------------------------------------------------
#define VENDOR_ID    0x303A
#define PRODUCT_ID   0x811A
#define DEVICE_VER   0x0100
#define MANUFACTURER "M5Stack"
#define PRODUCT      "Stackee"
