#include "stackee_logbuf.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ★ 本体は **PSRAM** に置く (段階 4)。内蔵 RAM に 16 KB 置いていたが、
//   カメラの DMA が内蔵の連続領域を要求するので、そこを空けたい
//   (実機の実測: 内蔵の空きが 12.6 KB、いちばん大きい塊が 7.7 KB しか
//    なかった)。
//
// ★ PSRAM に置いてよい理由: ここは esp_log の出口で、書式文字列は
//   フラッシュにある。つまり**キャッシュを止めた状態では元々呼べない**
//   (呼べば書式文字列の読み出しで落ちる)。だから PSRAM を触っても
//   新しい危険は増えない。
//
// ★ PSRAM が取れなかったときのために、内蔵に小さい控えを持つ。
//   起動のいちばん最初から溜める、という目的だけは何があっても守る。
#define LOGBUF_FALLBACK_SIZE 2048
static char     s_early[LOGBUF_FALLBACK_SIZE];
static char    *s_ring = s_early;
static uint32_t s_size = LOGBUF_FALLBACK_SIZE;
static uint32_t s_head;         // 次に書く位置
static bool     s_wrapped;
static uint32_t s_written;
static uint32_t s_dropped;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static vprintf_like_t         s_prev;
static stackee_logbuf_sink_t  s_sink;

static void push(const char *text, int len) {
    if (len <= 0) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < len; i++) {
        s_ring[s_head] = text[i];
        s_head = (s_head + 1) % s_size;
        if (s_head == 0) {
            s_wrapped = true;
        }
    }
    s_written += (uint32_t)len;
    if (s_wrapped && s_written > s_size) {
        s_dropped = s_written - s_size;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static int logbuf_vprintf(const char *fmt, va_list args) {
    // ★ ここは割り込みからも呼ばれうる。確保も待ちもしない。
    char    buf[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;   // 切れたぶんは諦める (溜めるのが目的)
    }
    push(buf, n);

    stackee_logbuf_sink_t sink = s_sink;
    if (sink != NULL) {
        sink(buf, n);
    }
    if (s_prev != NULL) {
        return s_prev(fmt, args);
    }
    return n;
}

void stackee_logbuf_install(void) {
    // ★ PSRAM は app_main より前に立ち上がっている
    //   (CONFIG_SPIRAM_BOOT_INIT=y)。取れなければ内蔵の控えのまま続ける。
    char *big = heap_caps_malloc(STACKEE_LOGBUF_SIZE,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (big != NULL) {
        s_ring = big;
        s_size = STACKEE_LOGBUF_SIZE;
    }
    s_head = 0;
    s_wrapped = false;
    s_written = 0;
    s_dropped = 0;
    s_prev = esp_log_set_vprintf(logbuf_vprintf);
    if (big == NULL) {
        ESP_LOGW("logbuf", "PSRAM が取れない。起動ログは %u B しか残せない",
                 (unsigned)s_size);
    }
}

void stackee_logbuf_set_sink(stackee_logbuf_sink_t sink) {
    s_sink = sink;
}

size_t stackee_logbuf_tail(char *out, size_t cap, size_t bytes) {
    return stackee_logbuf_read_back(out, cap, bytes, 0);
}

// 末尾から back バイト手前で終わる bytes バイトを写す (back=0 が tail)。
size_t stackee_logbuf_read_back(char *out, size_t cap, size_t bytes, size_t back) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    taskENTER_CRITICAL(&s_lock);
    uint32_t held = s_wrapped ? s_size : s_head;
    uint32_t head = s_head;
    taskEXIT_CRITICAL(&s_lock);

    if (back >= held) {
        return 0;
    }
    held -= (uint32_t)back;
    if (bytes == 0 || bytes > held) {
        bytes = held;
    }
    if (bytes > cap) {
        bytes = cap;
    }
    // (head - back) の手前 bytes バイトを順に写す。
    uint32_t start = (head + 2 * s_size - (uint32_t)back - (uint32_t)bytes) % s_size;
    for (size_t i = 0; i < bytes; i++) {
        out[i] = s_ring[(start + i) % s_size];
    }
    return bytes;
}

void stackee_logbuf_stats(stackee_logbuf_stats_t *out) {
    if (out == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    out->written = s_written;
    out->dropped = s_dropped;
    out->held = (uint16_t)(s_wrapped ? s_size : s_head);
    taskEXIT_CRITICAL(&s_lock);
}
