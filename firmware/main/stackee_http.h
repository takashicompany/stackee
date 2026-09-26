// 非同期の HTTPS。firmware/native-http (CircuitPython 用の espidf.http_*) と
// 同じ考え方で、DNS・TCP・TLS・HTTP をぜんぶ **専用タスク** に閉じ込める。
//
//   呼ぶ側 (audio タスクの会話の状態機械) は start して、あとは 1 周期 1 回
//   state を読むだけ。1 ミリ秒も待たない。
//
// ★ 同時に 1 本だけ。POST 失敗時の自動再送はしない (二重の会話生成を避ける)。
// ★ トークンはヘッダにだけ置き、ログには絶対に出さない。
// ★ 証明書は esp_crt_bundle (標準 CA バンドル) + ホスト名検証。
//   リダイレクトは追わない。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// path のぶんの余白 (base + path を 1 本の URL にする)。
#define STACKEE_HTTP_PATH_ROOM 192

typedef enum {
    STACKEE_HTTP_IDLE = 0,
    STACKEE_HTTP_RUNNING,
    STACKEE_HTTP_DONE,
    STACKEE_HTTP_ERROR,
} stackee_http_state_t;

// base は "https://host:port" の形 (末尾に / を付けない)。token は空でよい。
// ★ token は HTTPS のときしか受け付けない (平文に載せない)。
esp_err_t stackee_http_configure(const char *base, const char *token);

// 設定できているか (STACKEE_TALK_URL が入っているか)。
bool        stackee_http_configured(void);
const char *stackee_http_base(void);
bool        stackee_http_has_token(void);

esp_err_t stackee_http_start(void);      // ワーカーを起こす (1 回だけ)

// 送る。path は "/" で始まること。body は完了まで呼び手が持ち続ける。
// content_type は POST の Content-Type ("audio/wav" / "image/jpeg")。
// NULL なら "audio/wav" (会話の既定)。GET では使わない。
// ★ Content-Length は esp_http_client が body_len から付ける。
bool stackee_http_request(const char *method, const char *path,
                          const void *body, size_t body_len, size_t limit,
                          const char *content_type);

// 進み具合。戻り値は state。
int stackee_http_state(int *status, size_t *received, int *err, uint32_t *elapsed_ms);

// 完了していれば受信した本体 (NUL 終端つき)。close するまで有効。
const uint8_t *stackee_http_body(size_t *len);

// 通信中ならキャンセルを頼んで即戻る。終わっていれば解放する。
void stackee_http_close(void);

// status 用のまとめ。
typedef struct {
    int      state;
    int      status;
    int      err;
    uint32_t requests;
    uint32_t failures;
    uint32_t last_ms;
    size_t   last_bytes;
} stackee_http_stats_t;

void stackee_http_stats(stackee_http_stats_t *out);
