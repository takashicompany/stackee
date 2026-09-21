// CoreS3 の音のハード 2 つ。どちらも内部 I2C (G11/G12) にぶら下がっている。
//
//   ES7210  0x40  マイク用 4ch ADC。firmware/kmk/es7210.py の移植
//                 (さらにその元は M5Stack uiflow-micropython / esp_codec_dev)
//   AW88298 0x36  スピーカー用アンプ。firmware/kmk/stackee_speaker.py の移植
//                 (さらにその元は M5Unified.cpp:461-478)
//
// ★ レジスタの値は 1 つも変えていない。現行 CircuitPython 版で実際に音が
//   出ている設定そのもの。ES7210 は**スレーブ** (ESP32 が BCLK/WS を出す)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define STACKEE_ES7210_ADDR   0x40
#define STACKEE_AW88298_ADDR  0x36
// AW9523B の P0_2 = スピーカーアンプの電源 (M5Unified.cpp:461)。
#define STACKEE_AW9523_SPK_EN 0x04

esp_err_t stackee_codec_init(void);
bool      stackee_codec_ready(void);

// ---- ES7210 (マイク) -------------------------------------------------------
// 16 kHz / 16bit / MIC1+MIC2 / アナログ PGA 37.5 dB (code.py と同じ)。
esp_err_t stackee_es7210_setup(void);
esp_err_t stackee_es7210_enable(bool on);

// マイクを開ける。**全設定は 1 回だけ**で、2 回目からは電源を上げ直すぶんだけ
// (実測 44 ms → 十数 ms)。did_full に「全設定を通したか」が返る (NULL 可)。
// ★ 印の考え方は stackee_micopen.h。
esp_err_t stackee_es7210_open(bool *did_full);

// 「ほかの経路が IC を触った (かもしれない)」。次にマイクを開けるとき
// 全設定からやり直す。★ 触ったほうが自分で申告する。
void stackee_es7210_invalidate(void);

// 全設定した回数 / 電源だけで済んだ回数 / いま印が立っているか。
void stackee_es7210_open_stats(uint32_t *full, uint32_t *light, bool *dirty);

// ---- 立ち上がりの計測 (research/stackee/record_onset_2026-09-21.md) --------
// 直前の setup / enable が I2C に費やした時間 [us]。押下 → 音が録れ始めるまでの
// うち「そもそも音を取っていない」ぶんの支配項なので、数字で見えるようにする。
uint32_t stackee_es7210_last_setup_us(void);
uint32_t stackee_es7210_last_enable_us(void);

// ES7210 の状態機械 (レジスタ 0x0B の CSM_STATE)。
//   0 = power down / 1 = chip initial / 2 = normal / 3 = power up
// 読めなければ -1。★ LRCK を数えて進むので、I2S を止めている間は進まない。
int stackee_es7210_csm_state(void);
#define STACKEE_ES7210_CSM_NORMAL 2

// ---- AW88298 (スピーカー) --------------------------------------------------
// sample_rate から reg 0x06 の値を作る (M5Unified.cpp:463-470 の写し)。
uint16_t  stackee_aw88298_reg06(int sample_rate);

// アンプを起こす。戻り値は AW9523 P0_2 が「元から ON だったか」。
// amp_off にそのまま渡すと元の状態を壊さずに戻せる。
esp_err_t stackee_aw88298_on(int sample_rate, int volume_percent, int *was_on);
esp_err_t stackee_aw88298_off(int was_on);

// 音量 [%] をレジスタへ当てる。アンプが起きていないときは何もしない。
esp_err_t stackee_aw88298_volume(int percent);
bool      stackee_aw88298_powered(void);
