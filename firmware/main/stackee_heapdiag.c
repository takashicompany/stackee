#include "stackee_heapdiag.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "stackee_console.h"

#define MARKS 12

typedef struct {
    uint32_t up_s;
    uint32_t free;
    uint32_t largest;
    char     what[24];
} mark_t;

static mark_t s_marks[MARKS];   // 12 x 36 B
static int    s_mark_next;
static int    s_mark_count;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void stackee_heapdiag_mark(const char *what) {
    mark_t m;
    m.up_s = (uint32_t)(esp_timer_get_time() / 1000000);
    m.free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m.largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    snprintf(m.what, sizeof(m.what), "%s", what ? what : "");
    portENTER_CRITICAL(&s_mux);
    s_marks[s_mark_next] = m;
    s_mark_next = (s_mark_next + 1) % MARKS;
    if (s_mark_count < MARKS) {
        s_mark_count++;
    }
    portEXIT_CRITICAL(&s_mux);
}

static size_t put(char *buf, size_t cap, size_t at, const char *fmt, ...) {
    if (at >= cap) {
        return at;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + at, cap - at, fmt, args);
    va_end(args);
    return (n < 0) ? at : at + (size_t)n;
}

// 使用中の塊を大きさ別に数える (上限 32 / 64 / … / 16384 / それ以上)。
#define BUCKETS 11
#define BIG_KEEP 8

typedef struct {
    uint32_t count[BUCKETS];
    uint32_t bytes[BUCKETS];
    uint32_t used_blocks, free_blocks;
    uint32_t big_size[BIG_KEEP];
    uintptr_t big_addr[BIG_KEEP];
} walk_t;

static bool walker(walker_heap_into_t heap, walker_block_info_t block, void *user) {
    (void)heap;
    walk_t *w = user;
    if (!block.used) {
        w->free_blocks++;
        return true;
    }
    w->used_blocks++;
    int b = 0;
    size_t limit = 32;
    while (b < BUCKETS - 1 && block.size > limit) {
        limit <<= 1;
        b++;
    }
    w->count[b]++;
    w->bytes[b] += (uint32_t)block.size;
    // いちばん大きい BIG_KEEP 個を残す (挿入ソート)。
    for (int i = 0; i < BIG_KEEP; i++) {
        if (block.size > w->big_size[i]) {
            for (int j = BIG_KEEP - 1; j > i; j--) {
                w->big_size[j] = w->big_size[j - 1];
                w->big_addr[j] = w->big_addr[j - 1];
            }
            w->big_size[i] = (uint32_t)block.size;
            w->big_addr[i] = (uintptr_t)block.ptr;
            break;
        }
    }
    return true;
}

// 主なタスクのスタックの「一番減ったときの残り」[B]。
static const char *const TASKS[] = {
    "stackee_http", "stackee_audio", "stackee_ui", "stackee_lcd",
    "stackee_net", "touch", "camera", "input", "hid_out", "usbd", "main",
    "tiT", "wifi", "sys_evt", "esp_timer", "nimble_host", "btController",
};

static size_t reply_heap_info(long id, char *buf, size_t cap) {
    multi_heap_info_t in, dma, ps;
    heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&dma, MALLOC_CAP_DMA);
    heap_caps_get_info(&ps, MALLOC_CAP_SPIRAM);
    // ★ 数え上げの入れ物は PSRAM に一時的に取る (console のスタックも
    //   内蔵 RAM も食わない。数えている最中に内蔵 RAM を動かさない)。
    walk_t *wp = heap_caps_calloc(1, sizeof(walk_t), MALLOC_CAP_SPIRAM);
    if (wp == NULL) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"nomem\"}", id);
    }
    heap_caps_walk(MALLOC_CAP_INTERNAL, walker, wp);

    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"internal\":{\"free\":%u,\"largest\":%u,"
                    "\"min\":%u,\"alloc\":%u,\"alloc_blocks\":%u,\"free_blocks\":%u},"
                    "\"dma\":{\"free\":%u,\"largest\":%u},"
                    "\"psram\":{\"free\":%u,\"largest\":%u},\"hist\":[",
                    id, (unsigned)in.total_free_bytes, (unsigned)in.largest_free_block,
                    (unsigned)in.minimum_free_bytes, (unsigned)in.total_allocated_bytes,
                    (unsigned)in.allocated_blocks, (unsigned)in.free_blocks,
                    (unsigned)dma.total_free_bytes, (unsigned)dma.largest_free_block,
                    (unsigned)ps.total_free_bytes, (unsigned)ps.largest_free_block);
    for (int b = 0; b < BUCKETS; b++) {
        at = put(buf, cap, at, "%s[%u,%lu,%lu]", b ? "," : "", 32u << b,
                 (unsigned long)wp->count[b], (unsigned long)wp->bytes[b]);
    }
    at = put(buf, cap, at, "],\"big\":[");
    for (int i = 0; i < BIG_KEEP && wp->big_size[i] > 0; i++) {
        at = put(buf, cap, at, "%s[%lu,\"%08lx\"]", i ? "," : "",
                 (unsigned long)wp->big_size[i], (unsigned long)wp->big_addr[i]);
    }
    free(wp);
    at = put(buf, cap, at, "],\"stack_free\":{");
    bool first = true;
    for (size_t i = 0; i < sizeof(TASKS) / sizeof(TASKS[0]); i++) {
        TaskHandle_t t = xTaskGetHandle(TASKS[i]);
        if (t == NULL) {
            continue;
        }
        at = put(buf, cap, at, "%s\"%s\":%u", first ? "" : ",", TASKS[i],
                 (unsigned)uxTaskGetStackHighWaterMark(t));
        first = false;
    }
    at = put(buf, cap, at, "},\"marks\":[");
    mark_t copy[MARKS];
    int count, next;
    portENTER_CRITICAL(&s_mux);
    memcpy(copy, s_marks, sizeof(copy));
    count = s_mark_count;
    next = s_mark_next;
    portEXIT_CRITICAL(&s_mux);
    for (int i = 0; i < count; i++) {
        const mark_t *m = &copy[(next - count + i + MARKS) % MARKS];
        at = put(buf, cap, at, "%s[%lu,%lu,%lu,\"%s\"]", i ? "," : "",
                 (unsigned long)m->up_s, (unsigned long)m->free,
                 (unsigned long)m->largest, m->what);
    }
    at = put(buf, cap, at, "]}");
    return at;
}

static size_t heapdiag_console(const char *cmd, const char *line, long id,
                               char *buf, size_t cap) {
    (void)line;
    if (strcmp(cmd, "heap.info") == 0) {
        return reply_heap_info(id, buf, cap);
    }
    return 0;
}

void stackee_heapdiag_start(void) {
    stackee_console_register(heapdiag_console);
    stackee_heapdiag_mark("boot");
}
