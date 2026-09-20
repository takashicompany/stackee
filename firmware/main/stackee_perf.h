// 自己計測の土台。
//
// 合否は人手の打鍵確認ではなく、この数字で決める (DESIGN.md §8)。段階 0 では
// メインタスクの周期だけを積むが、チャンネルを足せば段階 1 の「キー押下 →
// USB 送出」の遅延もそのまま同じ形で status に出せる。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STACKEE_PERF_MAIN = 0,   // メインタスクの 1 周 [us]
    STACKEE_PERF_INPUT,      // 段階 1: キー検出 → HID レポート生成 [us]
    STACKEE_PERF_INPUT_LOOP, // 段階 1: 入力タスクの 1 周 [us]
    STACKEE_PERF_UI,         // 段階 2: LCD ワーカーの 1 回の転送 [us]
    STACKEE_PERF_UI_FACE,    // 段階 2: 顔 1 コマぶんの描画 (遷移が終わるまで) [us]
    STACKEE_PERF_UI_BAR,     // 段階 2: ステータスバー 1 回の描き直し [us]
    STACKEE_PERF_UI_SUB,     // 字幕: 帯 1 回の描き直し [us] (約束は 2 ms 以下)
    STACKEE_PERF_CHANNELS,
} stackee_perf_channel_t;

typedef struct {
    uint32_t count;          // reset からの標本数
    uint32_t max_us;
    uint32_t median_us;
    uint32_t last_us;
} stackee_perf_stats_t;

// チャンネル名 ("main" / "input" / "ui")。status の JSON にそのまま出す。
const char *stackee_perf_name(stackee_perf_channel_t ch);

void stackee_perf_init(void);
void stackee_perf_reset(stackee_perf_channel_t ch);
void stackee_perf_sample(stackee_perf_channel_t ch, uint32_t us);

// 直近の標本 (最大 STACKEE_PERF_WINDOW 個) から中央値を出す。
bool stackee_perf_stats(stackee_perf_channel_t ch, stackee_perf_stats_t *out);
