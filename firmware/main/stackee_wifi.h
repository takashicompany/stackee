// Wi-Fi 自動接続の実機側。状態機械そのものは stackee_wifism.c。
//
//   net タスク (CPU0 / 低優先度 / 20 ms) が 1 周 1 段ずつ進める。
//   登録簿は NVS ("stackee" / "wifi_nets") に JSON のまま置く。初回だけ
//   FAT の /wifi_networks.json を読んで移す (音量と同じ移し方)。
//
// console のコマンドと応答の形は firmware/kmk/stackee_console.py と同じ:
//   wifi.scan / wifi.list / wifi.add / wifi.remove / wifi.connect
//   (Web 操作盤がこの形を読む)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "stackee_wifistore.h"

esp_err_t stackee_wifi_start(void);

bool        stackee_wifi_connected(void);
const char *stackee_wifi_state_name(void);
const char *stackee_wifi_ssid(void);
const char *stackee_wifi_ip(void);
int         stackee_wifi_net_count(void);
uint32_t    stackee_wifi_connect_ms(void);
// 起動から接続までの時間 [ms]。まだ繋がっていなければ 0。
uint32_t    stackee_wifi_up_ms(void);

// ---------------------------------------------------------------------------
// 設定メニューの口 (2026-10-01、README §17-2g)
// ---------------------------------------------------------------------------
typedef struct {
    char ssid[STACKEE_WIFI_SSID_MAX];
    int  rssi;
    int  channel;
    bool secure;                // WIFI_AUTH_OPEN 以外
} stackee_wifi_scan_net_t;

// 全チャネルを 1 回走査 (wifi.scan と同じ)。強い順・SSID ごとに 1 件。
// 失敗なら -1 と err ("audio_busy" / "radio" / "scan" / "busy")。
// ★ 3〜6 秒止まる。呼ぶのはメインループだけ。
int stackee_wifi_scan_nets(stackee_wifi_scan_net_t *out, int max,
                           bool *radio_kept, const char **err);
// 登録簿に足す / 上書き (wifi.add と同じ検査・同じ保存)。成功で NULL、
// 失敗ならエラー名。★ 繋ぎにはいかない (stackee_wifi_prefer を呼ぶ)。
const char *stackee_wifi_store_add(const char *ssid, const char *password, int channel);
// 登録簿から消す (wifi.remove と同じ。消したら探索し直す)。
const char *stackee_wifi_store_remove(const char *ssid);
// その SSID へ切り替える (いまの接続を落として 1 周期だけ名指しで探す)。
bool stackee_wifi_prefer(const char *ssid);
// 登録済みの SSID (★ パスワードは返さない)。
int  stackee_wifi_saved_ssids(char out[][STACKEE_WIFI_SSID_MAX], int max);
// 繋がっている AP の電波 [dBm]。繋がっていなければ 0。
int  stackee_wifi_rssi(void);
// 名指しの結果 (0 なし / 1 試行中 / 2 接続した / 3 失敗) と理由。
void stackee_wifi_target(char *ssid, size_t cap, int *result, int *reason);
