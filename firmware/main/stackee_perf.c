#include "stackee_perf.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 直近 256 標本。1 ms 周期なら 0.25 秒ぶん。中央値を出すときだけ複製して
// 並べ替えるので、標本を積む側 (メインタスク) は常に O(1) で終わる。
#define STACKEE_PERF_WINDOW 256

typedef struct {
    uint32_t ring[STACKEE_PERF_WINDOW];
    uint16_t filled;
    uint16_t next;
    uint32_t count;
    uint32_t max_us;
    uint32_t last_us;
} channel_t;

static channel_t s_channels[STACKEE_PERF_CHANNELS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *const s_names[STACKEE_PERF_CHANNELS] = {
    "main", "input", "input_loop", "ui", "ui_face", "ui_bar", "ui_sub",
};

const char *stackee_perf_name(stackee_perf_channel_t ch) {
    if (ch < 0 || ch >= STACKEE_PERF_CHANNELS) {
        return "?";
    }
    return s_names[ch];
}

void stackee_perf_init(void) {
    taskENTER_CRITICAL(&s_lock);
    memset(s_channels, 0, sizeof(s_channels));
    taskEXIT_CRITICAL(&s_lock);
}

void stackee_perf_reset(stackee_perf_channel_t ch) {
    if (ch < 0 || ch >= STACKEE_PERF_CHANNELS) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    memset(&s_channels[ch], 0, sizeof(s_channels[ch]));
    taskEXIT_CRITICAL(&s_lock);
}

void stackee_perf_sample(stackee_perf_channel_t ch, uint32_t us) {
    if (ch < 0 || ch >= STACKEE_PERF_CHANNELS) {
        return;
    }
    channel_t *c = &s_channels[ch];
    taskENTER_CRITICAL(&s_lock);
    c->ring[c->next] = us;
    c->next = (uint16_t)((c->next + 1) % STACKEE_PERF_WINDOW);
    if (c->filled < STACKEE_PERF_WINDOW) {
        c->filled++;
    }
    c->count++;
    c->last_us = us;
    if (us > c->max_us) {
        c->max_us = us;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static int compare_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a;
    uint32_t y = *(const uint32_t *)b;
    return (x < y) ? -1 : (x > y);
}

bool stackee_perf_stats(stackee_perf_channel_t ch, stackee_perf_stats_t *out) {
    if (ch < 0 || ch >= STACKEE_PERF_CHANNELS || out == NULL) {
        return false;
    }
    static uint32_t scratch[STACKEE_PERF_WINDOW];   // 呼ぶのは console だけ
    channel_t *c = &s_channels[ch];
    uint16_t n;
    taskENTER_CRITICAL(&s_lock);
    n = c->filled;
    memcpy(scratch, c->ring, (size_t)n * sizeof(uint32_t));
    out->count = c->count;
    out->max_us = c->max_us;
    out->last_us = c->last_us;
    taskEXIT_CRITICAL(&s_lock);

    if (n == 0) {
        out->median_us = 0;
        return false;
    }
    qsort(scratch, n, sizeof(uint32_t), compare_u32);
    out->median_us = scratch[n / 2];
    return true;
}
