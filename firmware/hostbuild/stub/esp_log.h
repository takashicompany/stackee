// ESP-IDF の esp_log の代役。
//
// ★ 本物と違って「出す」のが目的ではなく、**差し込んだ出口 (vprintf) に
//   本当に流れるか**を確かめるためにある。段階 1b で足した
//   stackee_logbuf.c は esp_log_set_vprintf() で出口を横取りするので、
//   代役もその一点だけは本物と同じ振る舞いにしてある。
#pragma once
#include <stdarg.h>
#include <stdio.h>

typedef int (*vprintf_like_t)(const char *, va_list);

// ★ 実体は esp_log_stub.c に 1 つだけ置く。ヘッダの static にすると
//   翻訳単位ごとに別の変数になり、「差し込んだ出口」が片方にしか
//   見えなくなる (2026-09-16 に踏んだ)。
extern vprintf_like_t g_stub_log_vprintf;

vprintf_like_t esp_log_set_vprintf(vprintf_like_t f);
void stub_log_write(const char *fmt, ...);

#define ESP_LOGE(tag, fmt, ...) stub_log_write("E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) stub_log_write("W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) stub_log_write("I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) stub_log_write("D (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) stub_log_write("V (%s) " fmt "\n", tag, ##__VA_ARGS__)
