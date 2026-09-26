#include "stackee_perf.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
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

// ★ 7 KB。内蔵 RAM は HTTPS の握手に残すので PSRAM に取る (2026-09-27、
//   README §24-5)。標本を積むのはタスクだけで、割り込みやキャッシュを
//   止めた区間からは触らない。取れる前 (init の前) の標本は捨てる。
static channel_t *s_channels;
static uint32_t *s_scratch;         // 中央値を出すときの写し (console だけ)
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
    if (s_channels == NULL) {
        size_t bytes = sizeof(channel_t) * STACKEE_PERF_CHANNELS;
        channel_t *c = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (c == NULL) {
            c = heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
        }
        s_scratch = heap_caps_calloc(STACKEE_PERF_WINDOW, sizeof(uint32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_scratch == NULL) {
            s_scratch = heap_caps_calloc(STACKEE_PERF_WINDOW, sizeof(uint32_t),
                                         MALLOC_CAP_8BIT);
        }
        s_channels = c;             // ★ 最後に公開する (中身は 0 で埋まっている)
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    memset(s_channels, 0, sizeof(channel_t) * STACKEE_PERF_CHANNELS);
    taskEXIT_CRITICAL(&s_lock);
}

void stackee_perf_reset(stackee_perf_channel_t ch) {
    if (ch < 0 || ch >= STACKEE_PERF_CHANNELS || s_channels == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    memset(&s_channels[ch], 0, sizeof(s_channels[ch]));
    taskEXIT_CRITICAL(&s_lock);
}

void stackee_perf_sample(stackee_perf_channel_t ch, uint32_t us) {
    if (ch < 0 || ch >= STACKEE_PERF_CHANNELS || s_channels == NULL) {
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
    if (ch < 0 || ch >= STACKEE_PERF_CHANNELS || out == NULL ||
        s_channels == NULL || s_scratch == NULL) {
        return false;
    }
    uint32_t *scratch = s_scratch;   // 呼ぶのは console だけ
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
