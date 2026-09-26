// 非同期の HTTPS。firmware/native-http (CircuitPython 用の espidf.http_*) と
// 同じ考え方で、DNS・TCP・TLS・HTTP をぜんぶ **専用タスク** に閉じ込める。
//
//   呼ぶ側 (audio タスクの会話の状態機械) は start して、あとは 1 周期 1 回
//   state を読むだけ。1 ミリ秒も待たない。
//
// ★ 同時に 1 本だけ。POST 失敗時の自動再送はしない (二重の会話生成を避ける)。
// ★ 接続は 1 本を持ち続けて使い回す (2026-09-27、README §17-2d)。決まりは
//   stackee_httpcore.h。受信バッファ (stackee_http_close で返す) と接続
//   (ワーカーが持ち続ける) は別のもの。
// ★ 通信中に stackee_http_close を呼ぶと「打ち切り」。ロングポーリングは
//   ソケットを shutdown してすぐ起こす。打ち切りの直後に来た次の要求は
//   受け付けて (true)、ワーカーが畳み終えたらすぐ始める。
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

// 使い回せる接続を持っているか (1 度は往復が通った接続)。常時ポーリングが
// 「張り直しの最初の 1 本は待たない要求にする」ために見る。
bool        stackee_http_warm(void);

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

// 接続が無ければ、返事を捨てる短い GET (path) で先に張っておく
// (会話キーの録音中に TLS の握手を済ませ、離したあとの POST を待たせない)。
// ★ 呼び手は結果を見ない (state も変えずに見えるのは RUNNING だけ)。
//   先に張っている最中に stackee_http_request が来たら、張り終えたところで
//   その要求を始める (true を返す)。
bool stackee_http_prewarm(const char *path);

// 完了していれば受信した本体 (NUL 終端つき)。close するまで有効。
const uint8_t *stackee_http_body(size_t *len);

// 通信中なら打ち切りを頼んで即戻る。終わっていれば受信バッファを返す
// (★ 接続は閉じない。次の要求が使い回す)。
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
    // 接続の使い回し (2026-09-27)
    bool     conn;              // いま接続を持っている
    uint32_t reused;            // 既存の接続に載せた要求の数
    uint32_t connects;          // 張った数
    uint32_t stale_closed;      // 送る前の点検で捨てた数
    uint32_t leftover_closed;   // 読み残し・失敗で閉じた数
    uint32_t retries;           // GET を送り直した数
    uint32_t discarded;         // 打ち切って結果を捨てた数
    uint32_t shutdowns;         // 待ちをソケットごと起こした数
    uint32_t last_connect_ms;   // 直近の要求で張った時間 (使い回しなら 0)
    bool     last_reused;
    uint32_t warmups;           // 録音中に先に張った回数
} stackee_http_stats_t;

void stackee_http_stats(stackee_http_stats_t *out);
