// esp_log の代役の実体 (ヘッダは stub/esp_log.h)。
#include "esp_log.h"

vprintf_like_t g_stub_log_vprintf;

vprintf_like_t esp_log_set_vprintf(vprintf_like_t f) {
    vprintf_like_t prev = g_stub_log_vprintf;
    g_stub_log_vprintf = f;
    return prev;
}

void stub_log_write(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_stub_log_vprintf) {
        g_stub_log_vprintf(fmt, args);
    }
    va_end(args);
}
