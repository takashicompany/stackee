// USB マイク (UAC2、16 kHz / モノラル / 16 bit)。DESIGN.md §4 の full プロファイル。
//
// 移植元は firmware/cp-uac (CircuitPython 10.3.0 の usb_audio モジュール) と、
// そこで確かめた boot.py の `usb_audio.enable(sample_rate=16000, channel_count=1)`。
// 記述子は TinyUSB が持っている TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR をそのまま使う
// (CircuitPython 版が手書きしていたものと同じ構成: IAD + AC + クロック +
//  入力端子 + 機能ユニット + 出力端子 + AS alt0/alt1 + 等時 IN エンドポイント)。
//
// ★ dev プロファイルでは 1 バイトも増えない (CFG_TUD_AUDIO = 0 で中身が消える)。
//
// ★ 会話が最優先。STK_TALK で録音中・返答の再生中は、UAC には**無音**を送る。
//   マイクとスピーカーで BCK/WS を共有している (半二重) ので、
//   両方を同時には持てないため。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define STACKEE_UAC_RATE      16000
#define STACKEE_UAC_CHANNELS  1
#define STACKEE_UAC_BYTES     2     // 16 bit LE

void stackee_uac_init(void);

// audio タスクから毎周呼ぶ。ホストが開いていなければ何もしない。
void stackee_uac_step(void);

typedef struct {
    bool     enabled;       // この像に UAC が入っているか (full プロファイル)
    bool     streaming;     // ホストが AudioStreaming を開いているか
    uint32_t opens;         // 開かれた回数
    uint32_t frames;        // TinyUSB へ書いたサンプル数
    uint32_t silence;       // 会話に譲って無音を送ったサンプル数
    uint32_t underruns;     // FIFO を埋められなかった回数
    uint16_t volume_db;     // ホストが設定した音量 (1/256 dB)
    bool     muted;
} stackee_uac_stats_t;

void stackee_uac_stats(stackee_uac_stats_t *out);
