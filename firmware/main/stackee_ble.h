// BLE HID。DESIGN.md §2 の BLE 行。
//
//   NimBLE + esp_hid (esp_hid_device 例と同じ構成)
//   レポート記述子は **USB と同一内容** (Report ID 1/2/3、
//   キーボードの Usage Maximum は 0xFF で JIS の LANG1/LANG2 を含む)
//   デバイス名は現行と同じ "stackee"、外観はキーボード
//   ボンディング有効 (暗号化必須)
//
// ★ 現行 CircuitPython 版で「BLE だと英数/かなが効かない」の原因になっていた
//   adafruit_ble の Usage Maximum 0x89 制限は、こちらでは最初から存在しない
//   (記述子を自前で持っているため)。code.py が差し替えで直していた 1 か所を、
//   はじめから USB と同じ形にしてある。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// BLE でホストに見える名前。現行 code.py の BLE_NAME と同じ。
#define STACKEE_BLE_NAME "stackee"

// コントローラと NimBLE を起こし、HID サービスを立てる。
// 失敗しても致命ではない (USB キーボードとしては動く)。
esp_err_t stackee_ble_start(void);

// 1 秒ごとに呼ぶ。未接続ならアドバタイズし直す
// (現行 KMK の BLEHID.ble_monitor と同じ周期・同じ考え方)。
void stackee_ble_tick(void);

bool stackee_ble_ready(void);       // スタックが立ち上がっているか
bool stackee_ble_connected(void);

// 接続間隔 [ms]。打鍵がホストへ届くまでの遅れの下限になる
// (1 レポートは次の接続イベントまで待つ)。未接続なら -1。
int stackee_ble_interval_ms(void);

// レポートを 1 つ送る。report_id は USB と同じ 1/2/3。
// 送れなければ false (hid_out タスクが数える)。
bool stackee_ble_send(uint8_t report_id, const uint8_t *data, size_t len);

// STK_BLE_REFRESH。今の接続を切ってアドバタイズを撒き直す
// (現行 KMK の ble_refresh と同じ。★ ボンドは消さない)。
void stackee_ble_refresh(void);

// GATT Service Changed の indication を手で送る (console の ble.svc_changed)。
// 繋がっていなければ false。
bool stackee_ble_send_service_changed(void);

// ボンドを全部消す。コンソールの ble.clear_bonds から呼ぶ。
// ★ 消すと Mac 側でもペアリングを削除しないと繋がらなくなるので、
//   キーには割り当てていない (誤爆すると復旧に人手が要る)。
void stackee_ble_clear_bonds(void);

typedef struct {
    bool     started;       // stackee_ble_start() が最後まで通ったか
    const char *err;        // 失敗した場所 ("" = ここまでは順調)
    int      err_code;      // その esp_err_t
    bool     ready;
    bool     connected;
    bool     advertising;
    int      interval_ms;
    // アドバタイズの実際 (NimBLE に聞いた値)。
    uint32_t adv_starts;        // 始めた回数
    uint32_t adv_fails;         // 始められなかった回数
    uint32_t adv_revived;       // 「出ているつもり」が止まっていて撒き直した回数
    // アドバタイズの撒き方 ("directed_high" / "directed_low" / "undirected" / "off")。
    const char *adv_kind;
    uint32_t adv_directed;      // 名指しで撒いた回数
    bool     have_bond_peer;    // 名指しの相手 (ボンド済み) が居るか
    // GATT Service Changed (macOS の GATT キャッシュ対策)。
    uint16_t svc_changed_handle;
    uint32_t svc_changed_sent;
    uint32_t svc_changed_acked;
    int      svc_changed_rc;
    uint32_t connects;
    uint32_t disconnects;
    uint32_t sent;
    uint32_t failed;
} stackee_ble_stats_t;

void stackee_ble_stats(stackee_ble_stats_t *out);
