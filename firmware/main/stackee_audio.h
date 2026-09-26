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
#include <stddef.h>
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

// ---------------------------------------------------------------------------
// 画像を見せる (POST /look、2026-09-26)。呼ぶのは camera タスク。
// ---------------------------------------------------------------------------
// 撮る前に会話の口を押さえる。戻り値は stackee_talk_look_reserve_t
// (0 = 撮ってよい / 1 = 会話中などで撮らない / 2 = 送れない。画面に出した)。
// ★ 押さえている間と画像の往復の間は、STK_TALK と talk.inject を受け付けない。
int  stackee_audio_look_reserve(void);
// 撮れなかったときに押さえを外す。
void stackee_audio_look_release(void);
// 撮れた JPEG を渡す。audio タスクが拾って写し取り、POST /look を始めるまで
// 待つ (最大 2 秒ほど)。戻ったら jpeg は自由に使ってよい。
// play=false なら返答を鳴らす直前で止める (一次回答も鳴らさない)。
bool stackee_audio_look_submit(const uint8_t *jpeg, size_t len, bool play);
// camera.look_status に混ぜる "talk":{…} (job / 返答の長さ / 各段の ms / エラー)。
size_t stackee_audio_look_json(char *buf, size_t cap, size_t at);

// ---------------------------------------------------------------------------
// stackee 独自キー CSTM_0〜CSTM_9 (POST /key、2026-09-27)
// ---------------------------------------------------------------------------
// キーの押下。**入力タスクから呼ぶので待たない** (印を 1 つ置くだけ)。
// 実際の送信は audio タスクが会話の状態機械で進める。キーは常に鳴らす。
// ★ 会話・画像・他の CSTM の途中なら audio タスクが黙って捨てる。
void stackee_audio_cstm_key(int n);
