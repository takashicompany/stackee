// USB。DESIGN.md §4 / §8b。プロファイルが 2 つある。
//
//   dev  … CDC(IN 2) + HID(IN 1) + Raw HID(IN 1) + EP0(1) = 5  ← 上限ちょうど
//   full … HID(IN 1) + Raw HID(IN 1) + UAC マイク(IN 1) + EP0(1) = 4
//
// ESP32-S3 の IN エンドポイントは EP0 込みで 5 本が上限。full は CDC を
// 捨てて UAC マイクを載せ、コンソールとログは Raw HID (VIA の独自
// command id 0xC0〜、stackee_conhid.c) に相乗りさせる。
//
// 切り替えはビルド時:  ./build.sh --profile full
// (トップの CMakeLists が -DSTACKEE_USB_PROFILE_FULL=1 を全体に渡す)
//
// VID/PID/製造元/製品名/シリアルは CircuitPython 版と同じにしてある。ポートを
// UID で選ぶ既存の道具 (firmware/kmk/tools/stackee_serial.py) がそのまま動く。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// HID インターフェース番号 (tud_hid_n_* の引数)。
#define STACKEE_HID_ITF_KEYS    0   // キーボード / マウス / コンシューマ
#define STACKEE_HID_ITF_RAW     1   // VIA (Raw HID)

// キー用インターフェースの Report ID。
#define STACKEE_REPORT_ID_KEYBOARD  1
#define STACKEE_REPORT_ID_MOUSE     2
#define STACKEE_REPORT_ID_CONSUMER  3

// ★ Raw HID は **Report ID なし** (DESIGN.md §8b の決定)。
//   トップレベルコレクションが 1 つだけなので ID が要らず、VIA / Remap が
//   前提にしている「Report ID 0 で 32 バイトを送受信」がそのまま通る。
#define STACKEE_RAW_HID_SIZE 32

// ---------------------------------------------------------------------------
// UAC マイク (full プロファイルのみ)
// ---------------------------------------------------------------------------
// インターフェース番号。tud_audio_set_itf_cb が wIndex で渡してくる値。
#define STACKEE_UAC_ITF_AC   2
#define STACKEE_UAC_ITF_AS   3
// TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR が焼き込んでいるエンティティ ID。
#define STACKEE_UAC_ENTITY_INPUT_TERMINAL   0x01
#define STACKEE_UAC_ENTITY_FEATURE_UNIT     0x02
#define STACKEE_UAC_ENTITY_OUTPUT_TERMINAL  0x03
#define STACKEE_UAC_ENTITY_CLOCK_SOURCE     0x04

void stackee_usb_start(void);
bool stackee_usb_mounted(void);

// "dev" か "full"。hello / status に出す。
const char *stackee_usb_profile(void);
// この像に CDC があるか (= コンソールがシリアルにも出るか)。
bool stackee_usb_has_cdc(void);

// 記述子の静的確認 (tools/hid_desc_check.py) 用に、実体を外から読めるようにする。
const uint8_t *stackee_usb_hid_report_desc(uint8_t itf, size_t *len);

// ---------------------------------------------------------------------------
// 脱出路
// ---------------------------------------------------------------------------
// ROM の USB ダウンロードモード (303a:0009) へ落として再起動する。
// tools/flash.py の enter_rom() がつかまえる先。CDC の 1200bps タッチ、
// コンソールの bootloader コマンド、QK_BOOT のどれからでもここへ来る。
void stackee_usb_request_rom_download(void);

// 普通の再起動 (コンソールの reset コマンド)。
void stackee_usb_request_restart(void);

// ---------------------------------------------------------------------------
// Raw HID の受信
// ---------------------------------------------------------------------------
// ホストから届いた 32 バイトを 1 件取り出す。無ければ false。
//
// ★ 取り出して QMK (via.c) に渡すのは **入力タスク**。USB のコールバックから
//   直接 QMK を呼ぶと、入力タスクと同じ状態を 2 本のタスクが触ることになる。
bool stackee_usb_raw_rx_pop(uint8_t *buf32);
uint32_t stackee_usb_raw_rx_dropped(void);
