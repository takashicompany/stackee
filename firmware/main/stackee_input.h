// 入力タスク。DESIGN.md §3 の「input」。
//
//   CPU1 / 最高優先度 / 1 周 1 ms
//   TCA8418 の FIFO → QMK keyboard_task → 送信キュー
//
// このタスクは I2C (PORT.A) と送信キューしか触らない。USB も画面も音も
// 触らないので、描画や会話で待たされることが構造的に無い。
#pragma once

#include <stdbool.h>
#include <stdint.h>

void stackee_input_start(void);

typedef struct {
    bool     tca_connected;
    uint32_t key_events;        // TCA8418 から取り出したイベント数
    uint8_t  keys_down;         // いま押されているキー数
    uint32_t overflows;
    uint32_t io_fails;
    uint32_t stray;
    uint32_t custom_keys;       // 独自キー (STK_*) が押された回数
} stackee_input_stats_t;

void stackee_input_stats(stackee_input_stats_t *out);

// STK_MIC_KEY (PC 側のプッシュトゥトークに使っている F13) を押しているか。
// ★ 画面 (ui タスク) が毎周読む。読むだけ / 書くのは入力タスクだけの
//   atomic な印 1 つなので、打鍵の道には何も足さない。
bool stackee_input_mic_held(void);

// ---------------------------------------------------------------------------
// 打鍵の注入 (人手ゼロの検証用)
// ---------------------------------------------------------------------------
// ★ **入力タスクの経路をそのまま通す。** TCA8418 のイベントとして
//   配線の無いスロットへ流し込み、デバウンス → QMK → 送信キュー →
//   hid_out と、普段の打鍵とまったく同じ道を歩かせる。
//   ここを迂回すると「テストは通るのに実機で打てない」が起きる。
//
// 使うスロットは ROW4 の COL0 (配線が無いので実キーと衝突しない)。
// レイヤー 0 のそこへ一時的にキーコードを置き、終わったら KC_NO に戻す。
typedef struct {
    bool     ok;
    uint16_t keycode;
    uint32_t press_us;       // 注入 → 押下レポートがキューに載るまで
    uint32_t release_us;     // 注入 → 解放レポートがキューに載るまで
    uint32_t pushed;         // 送信キューに積まれたレポート数の増分
    uint32_t sent_usb;       // USB へ出た数の増分
    uint32_t sent_ble;       // BLE へ出た数の増分
    char     dest[8];        // "USB" / "BLE"
} stackee_inject_result_t;

// 押して hold_ms 後に離す。終わるまで待って結果を返す (失敗なら ok = false)。
// ★ 呼べるのは入力タスク**以外**から (コンソールのタスク)。
bool stackee_input_inject(uint16_t keycode, uint32_t hold_ms,
                          stackee_inject_result_t *out);

// 同じものを **待たずに** 始める。始められたら true。
// ★ 押している最中の本体を外から覗くための口。上のものはコンソールの
//   タスクを hold_ms のあいだ止めてしまうので、その間 `ui.status` や
//   `lcd.crc` を読めない (STK_MIC_KEY の表情を確かめるのに要る)。
//   結果 (遅延など) は返らない。遅延を測るときは上のものを使うこと。
bool stackee_input_inject_begin(uint16_t keycode, uint32_t hold_ms);
