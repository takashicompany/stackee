// アプリ内 OTA の実機側 (ESP-IDF の esp_ota_* と mbedtls を stackee_otacore に
// 差し込み、コンソール命令 app.info / ota.* を足す)。
//
// 受け皿そのものは stackee_otacore.c にある (ESP-IDF に依存しない = ホストで
// 単体テストできる)。ここが持つのは
//   ・次の更新区画 (ota_1) への esp_ota_begin / write / end / abort
//   ・mbedtls の SHA-256 (ストリーミング)
//   ・環状バッファ (PSRAM) と作業バッファ (内蔵 RAM) の確保
//   ・コンソール命令の受け答え
//   ・ota.commit のあとの遅延再起動
// の 5 つだけ。
#pragma once

#include <stdbool.h>
#include <stddef.h>

// コンソールに app.info / ota.* を足し、Raw HID の 0xC3 を受ける口を開ける。
// 起動時に 1 度だけ呼ぶ (stackee_main.c)。**ここではまだ何も確保しない。**
void stackee_ota_init(void);

// メインループから毎周。環状バッファに溜まったぶんをフラッシュへ書く。
// ★ ここは数十 ms 止まることがある (フラッシュの消去)。メインループは
//   コンソールしか見ていないので構わない。キーは専用タスクが見ている
//   (ただしフラッシュ操作中は CPU1 ごと止まるので、打鍵も同じだけ待つ)。
void stackee_ota_poll(void);

// OTA が進行中か (画面や status から見たいとき用)。
bool stackee_ota_busy(void);
