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

esp_err_t stackee_wifi_start(void);

bool        stackee_wifi_connected(void);
const char *stackee_wifi_state_name(void);
const char *stackee_wifi_ssid(void);
const char *stackee_wifi_ip(void);
int         stackee_wifi_net_count(void);
uint32_t    stackee_wifi_connect_ms(void);
// 起動から接続までの時間 [ms]。まだ繋がっていなければ 0。
uint32_t    stackee_wifi_up_ms(void);
