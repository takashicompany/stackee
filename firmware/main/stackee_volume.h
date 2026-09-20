// 音量。firmware/kmk/stackee_volume.py + stackee_volume_store.py の移植。
//
//   0..100 [%]、STK_VOLUP / STK_VOLDN で 5 きざみ、0 でミュート。
//   値が変わってから 2 秒静かなら保存する (打鍵中・会話中は先送り)。
//
// ★ 保存先は **NVS** ("stackee" / "volume")。現行 CircuitPython 版は FAT の
//   /stackee_volume.json に書いていたので、**初回起動時にその値を NVS へ移す**
//   (DESIGN.md §8b の決定)。実機の現行値 (15 か 20) を失わない。
//
// ★ 値の決め方と保存のタイミングは ESP-IDF に依存しない (ここ)。
//   AW88298 のレジスタへ当てるのは stackee_audio.c。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STACKEE_VOLUME_STEP         5
#define STACKEE_VOLUME_SAVE_IDLE_MS 2000
#define STACKEE_VOLUME_DEFAULT      20

typedef struct {
    int      percent;
    int      saved;             // 最後に保存できた値
    uint32_t changed_at;        // 値が変わった時刻 [ms]
    uint32_t last_input_at;     // 最後に打鍵を見た時刻 [ms]
    uint32_t changes;           // 変わった回数 (status 用)
    uint32_t saves;             // 保存した回数
} stackee_volume_state_t;

void stackee_volume_state_init(stackee_volume_state_t *v, int loaded, uint32_t now);

// 0..100 に丸める。変わったときだけ changed_at を更新する。
void stackee_volume_state_set(stackee_volume_state_t *v, int percent, uint32_t now);

// STK_VOLUP / STK_VOLDN。delta は +STEP / -STEP。
void stackee_volume_state_step(stackee_volume_state_t *v, int delta, uint32_t now);

void stackee_volume_state_note_input(stackee_volume_state_t *v, uint32_t now);

bool stackee_volume_state_pending(const stackee_volume_state_t *v);

// 保存してよいか。値が変わってから 2 秒、最後の打鍵から 2 秒、
// かつ音が鳴っていない (audio_busy == false) こと。
bool stackee_volume_state_should_save(const stackee_volume_state_t *v,
                                      uint32_t now, bool audio_busy);

void stackee_volume_state_mark_saved(stackee_volume_state_t *v);

// ---------------------------------------------------------------------------
// AW88298 のレジスタ値 (stackee_speaker.py の volume_bits と同じ)
// ---------------------------------------------------------------------------
// データシート v1.6 p35: 上位バイトが VOL。粗調 6 dB / 微調 0.5 dB きざみ。
// percent 0 は -96 dB 相当 (実際は HMUTE も立てる)。
uint16_t stackee_volume_bits(int percent);

// ---------------------------------------------------------------------------
// 実機側 (ESP-IDF) の口
// ---------------------------------------------------------------------------
// NVS から読む。無ければ FAT の /stackee_volume.json を見て、あれば
// その値を NVS へ移す。どちらも無ければ STACKEE_VOLUME_DEFAULT。
void stackee_volume_init(void);

int  stackee_volume_percent(void);
bool stackee_volume_save_pending(void);
void stackee_volume_set(int percent);
void stackee_volume_bump(int delta);        // +5 / -5
void stackee_volume_note_input(void);

// 保存の面倒を見る。ui / audio より優先度の低いところから定期的に呼ぶ。
void stackee_volume_task_step(bool audio_busy);

// 移行の記録 (status 用)。"nvs" / "fat" / "default"。
const char *stackee_volume_source(void);

// 保存待ちがあれば今すぐ NVS に書く (電源断の直前用)。
bool stackee_volume_flush(void);
