// 音。DESIGN.md §3 の「audio」タスク (CPU0 / 高優先度)。
//
//   録音 (ES7210 → I2S RX) と 再生 (I2S TX → AW88298) を **半二重** で入れ替える。
//   BCK (G34) と WS (G33) がマイクとスピーカーで同じ線なので、同時には持てない。
//   現行 CircuitPython 版 (stackee_halfduplex.py) と同じ決まり。
//
//   このタスクの上で会話の状態機械 (stackee_talksm) も 1 周 1 段ずつ進む。
//   通信そのものは別タスク (stackee_http) なので、ここが待つことは無い。
//
// ★★ **音を鳴らさない検証のための「ヌル出力」がある。**
//   `audio.null 1` にすると I2S も AW88298 も一切触らず、DMA 相当の
//   カウンタだけを実時間で進める。会社で使っている本体で検証するときは
//   必ずこれを先に立てる (tools/check_phase3.py が最初にやる)。
//   **既定はヌルではない** — 提出後の普段使いでは本当に鳴らすため。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define STACKEE_AUDIO_RATE     16000
// 立ち上がりの計測: 先頭 500 ms を 50 ms ずつ 10 枠で見る。
#define STACKEE_AUDIO_HEAD_SLOTS 10

#define STACKEE_AUDIO_ACK_MAX  5

// I2S のピン (firmware/kmk/code.py と CoreS3 のボード定義と同じ)。
#define STACKEE_I2S_MCLK_GPIO  0    // ★ 基板の緑 LED と共有。出すと薄く光る
#define STACKEE_I2S_BCLK_GPIO  34
#define STACKEE_I2S_WS_GPIO    33
#define STACKEE_I2S_DIN_GPIO   14   // ES7210 → ESP32
#define STACKEE_I2S_DOUT_GPIO  13   // ESP32 → AW88298

// 素材 (一次回答) を読み、コーデックを登録し、audio タスクを起こす。
// 音は鳴らさない (マイクも開かない)。
//   post_path … 会話の送信先のパス ("/talk")。STACKEE_TALK_URL から
//               stackee_main.c が切り出して渡す。
esp_err_t stackee_audio_start(const char *post_path);

bool stackee_audio_ready(void);

// 録音中か再生中か (音量の保存を先送りする判断、Wi-Fi 走査の抑止に使う)。
bool stackee_audio_busy(void);
bool stackee_audio_recording(void);
bool stackee_audio_speaking(void);

// STK_TALK の押し離し。入力タスクから呼ばれる (ここではブロックしない)。
void stackee_audio_talk_key(bool pressed);

// ヌル出力の切り替え。
void stackee_audio_set_null(bool on);
bool stackee_audio_null(void);

// status に混ぜる文字列 (会話の状態名と直近の返答文)。
const char *stackee_audio_talk_state(void);
const char *stackee_audio_screen(void);

// ---------------------------------------------------------------------------
// 段階 4: USB マイク (UAC) が使う口
// ---------------------------------------------------------------------------
// ★ 呼んでよいのは audio タスクだけ。会話 (STK_TALK・返答の再生) が
//   優先で、そちらが動いている間は -1 を返す (UAC 側は無音を送る)。
// 返すのは読めたサンプル数 (0 なら「いまは無い」、-1 なら「使えない」)。
int  stackee_audio_uac_pull(int16_t *dst, int max_samples);
void stackee_audio_uac_release(void);
