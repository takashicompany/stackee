// 画面。DESIGN.md §3 の「ui」タスク (CPU0 / 中優先度)。
//
//   顔 (既存素材・差分描画・状態機械) + 上段ステータスバー
//
// ★ フレームバッファを触るのはこのタスクとコンソールの検査コマンドだけ。
//   入力タスク (CPU1) は画面のことを何も知らない。ui が状態を「取りに行く」
//   作りにしてあるので、打鍵の道に描画の都合が 1 行も混ざらない。
//
// ★ 人手ゼロで確かめる (DESIGN.md §6 の「提出の方式」):
//   lcd.crc / lcd.dump / face.set / face.auto / bar.set / bar.auto /
//   ui.selftest / ui.status / ui.assets を console から呼び、
//   tools/render_expected.py が出す期待値と tools/check_phase2.py で照合する。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 素材を読んで ui タスクを起こす。素材が無くても致命ではない
// (バーだけ・内蔵フォントだけ、で続ける)。
esp_err_t stackee_ui_start(void);

bool stackee_ui_ready(void);

// 段階 3 で音声・Wi-Fi 側から呼ぶ口。
void stackee_ui_set_volume(int percent);
// stackee_wifism の状態名をそのまま渡す ("up" / "scan_wait" / "off" ...)。
void stackee_ui_set_wifi(const char *state_name);

// 会話の様子。顔の状態機械 (listening / thinking / speaking) に渡る。
//   recording … 録音中             -> listening
//   busy      … 送信〜返答待ち      -> thinking
//   speaking  … 再生中             -> speaking
void stackee_ui_set_talk(bool recording, bool busy, bool speaking);

// 段階 4: 撮影中だけ描画を止める (DESIGN.md §3)。画面はそのまま残る。
void stackee_ui_pause(bool on);
bool stackee_ui_paused(void);

// いま画面に出ている 1 行 (= 返答文)。status の "screen" に出る。
// ★ 現行 CircuitPython 版と同じ意味: Mac から「返した文が本当に本体へ
//   届いたか」を確かめる唯一の手段。
void stackee_ui_set_screen(const char *text);
const char *stackee_ui_screen(void);

// 返答音声の字幕。画面の下の帯 (y=290..319) に 1 行だけ出す。
// NULL か空で帯を消す。同じ文字列なら描き直さない。
// ★ 呼ぶのは会話の状態機械 (audio タスク)。実際に描くのは ui タスクだけで、
//   ここでは文字列を置くだけ (フレームバッファに触らない)。
void stackee_ui_set_subtitle(const char *utf8);

// 電源ボタン (AXP2101 IRQ 0x49) の検出回数。boot_irq は起動直後に残っていた印。
void stackee_ui_pwrkey_stats(int *long_presses, int *short_presses, int *boot_irq);
