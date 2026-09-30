// 本体の設定メニュー — 実機側の口 (README §17-2g / DESIGN.md §6e)。
//
//   Settings キー (入力タスク) ──> 開く / 閉じる (atomic の印 1 つ)
//        │                          │ 入力タスクが毎周見て、キーの横取りを入り切り
//        │                          ▼ (qmk_port の stackee_qmk_gate_set)
//   横取りしたキー (入力タスク) ──> キューに積むだけ (待たない)
//        ▼
//   ui タスク: キューを読む → stackee_menu_key() → 画面を組み立て → 変わったら描く
//        │ 頼みごと (Wi-Fi の登録・切り替え・走査、送信先の切り替え)
//        ▼
//   メインループ: stackee_menu_poll() が 1 つずつ片づける (NVS・走査は数秒かかる)
//
// 決め事そのもの (階層・選択・パスワードの編集・画面の中身) は
// stackee_menu_core.c (ESP-IDF に依存しない)。ここは糊だけ。
// ★ 大きいもの (画面 2 枚ぶんの行・外の様子の写し・走査結果) は PSRAM。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stackee_draw.h"
#include "stackee_font16.h"

// ui の起動時に 1 回 (PSRAM を取る)。
void stackee_menu_start(void);

// 開いているか (開きたいか)。入力タスク・audio タスク・ui タスクが読む。
bool stackee_menu_is_open(void);

// Settings キーの押下 (入力タスク)。開いていれば閉じる。閉じていれば、
// 会話・写真・Custom・クリップの処理中でなければ開く (処理中なら何もしない)。
void stackee_menu_settings_key(void);

// 横取りしたキー (入力タスク)。JIS として読んでキューに積むだけ。
void stackee_menu_hid_key(uint8_t usage, uint8_t mods);

// メニューを開けない理由 (NULL = 開ける)。
const char *stackee_menu_blocker(void);

// ui タスクから毎周。戻り値:
typedef enum {
    STACKEE_MENU_UI_NONE = 0,   // 閉じている (いつもの顔と帯を描く)
    STACKEE_MENU_UI_SHOWN,      // 開いている。描き直すものは無かった
    STACKEE_MENU_UI_PAINTED,    // 開いている。y=28..319 を描き直した
    STACKEE_MENU_UI_CLOSED,     // たったいま閉じた (呼び手が顔と帯を描き直す)
} stackee_menu_ui_t;

stackee_menu_ui_t stackee_menu_ui_tick(const stackee_canvas_t *canvas,
                                       const stackee_font16_t *font,
                                       int battery, bool charging);

// 画面を誰かが上書きした (ui.selftest など)。次の周で描き直させる。
void stackee_menu_invalidate(void);

// メインループから。Wi-Fi の登録・切り替え・走査、送信先の切り替えを片づける。
void stackee_menu_poll(void);

// console の menu.status / menu.key / menu.open / menu.close。
// ★ 呼び手 (stackee_ui.c) が ui の錠を持って呼ぶ。知らないコマンドなら 0。
size_t stackee_menu_console(const char *cmd, const char *line, long id,
                            char *buf, size_t cap);
