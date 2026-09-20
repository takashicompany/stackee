// 起動ログのリングバッファ。
//
// ★ なぜ要るか: 起動直後のログは、ホストが CDC を開くより前に流れてしまう。
//   段階 1b で「BLE が起動しない」を調べようとしたとき、原因を書いている
//   はずの ESP_LOGE が 1 行も読めなかった (2026-09-16)。
//   以後どの段階でも同じことが起きるので、**起動のいちばん最初から**
//   ログを溜めて、あとから console の `log.tail` で読めるようにする。
//
// 置き場は内部 RAM。PSRAM は stackee_board_init() より前には使えないし、
// 16 KB なら内部に置いても困らない (ヒープは 300 KB 以上ある)。
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACKEE_LOGBUF_SIZE 16384

// esp_log の出口を横取りして溜め始める。**app_main のいちばん最初**に呼ぶ。
// 既存の出口 (UART) はそのまま呼び続ける。
void stackee_logbuf_install(void);

// 溜まっているうちの最後の `bytes` バイトを out に写す。
// 実際に写した長さを返す。out は NUL 終端しない。
size_t stackee_logbuf_tail(char *out, size_t cap, size_t bytes);
// 末尾から back バイト手前で終わる bytes バイト (古い部分を読むため)。
size_t stackee_logbuf_read_back(char *out, size_t cap, size_t bytes, size_t back);

typedef struct {
    uint32_t written;       // 起動から書き込んだ総バイト数
    uint32_t dropped;       // 溢れて捨てた総バイト数
    uint16_t held;          // いま持っているバイト数
} stackee_logbuf_stats_t;

void stackee_logbuf_stats(stackee_logbuf_stats_t *out);

// 別の出口 (CDC への素通し) を後ろに繋ぐ。console が使う。
typedef int (*stackee_logbuf_sink_t)(const char *text, int len);
void stackee_logbuf_set_sink(stackee_logbuf_sink_t sink);
