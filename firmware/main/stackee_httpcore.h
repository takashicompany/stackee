// HTTPS の 1 往復を「持ち続けている 1 本の接続」の上で回す決まり (2026-09-27)。
//
// ★ ESP-IDF に依存しない。接続の張り方・読み書きは ops で外から差し込む。
//   実機では stackee_http.c が esp_http_client をつなぎ、hostbuild
//   (tools/test_httpcore_host.py) が偽の接続をつないで、
//     - 使い回し (2 本目は張らずに送る)
//     - 相手が閉じていた (送る前の点検で見つけて張り直す)
//     - 読み残し (打ち切り・上限超え・エラー) があれば必ず閉じる
//     - GET は 1 回だけ送り直す / POST は送り直さない
//   を時刻つきで確かめる。
//
// ★ 決まり (変えるときは README §17-2d と test_httpcore_host.py を一緒に):
//   1. 送る前に、持っている接続を点検する。相手が閉じた・頼んでいない
//      バイトが来ている (TLS の close_notify を含む) なら捨てて張り直す。
//   2. POST は「直前 STACKEE_HC_POST_REUSE_MAX_MS 以内に使えた接続」しか
//      使い回さない。黙って死んだ接続に POST を書いて 30 秒待たされるのを
//      避けるため (POST は送り直さないので、外れたら会話が失敗する)。
//   3. 応答を最後まで読めて、相手が持続を許したときだけ接続を残す。
//      それ以外 (失敗・上限超え・打ち切り・本文の読み残し・Connection: close)
//      は必ず閉じる。**読み残した接続を次の要求に使わない。**
//   4. GET は、送信・応答の頭・本文の途中で失敗したら 1 回だけ張り直して
//      送り直す (張れなかった・待ち時間切れ・上限超え・打ち切りは送り直さない)。
//      POST (/talk /look /key) は自動では送り直さない (二重に受理させない)。
//   5. 打ち切り (aborted) は「送る前」ならそのまま返す。送ったあとは
//      ロングポーリング (long_poll) の要求だけが従う (呼び手がソケットを
//      shutdown して起こす)。短い要求は最後まで読んで接続を残す
//      (その接続を次の要求がそのまま使える)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// POST が使い回してよい接続の古さ [ms]。
#define STACKEE_HC_POST_REUSE_MAX_MS 30000

enum {
    STACKEE_HC_OK = 0,
    STACKEE_HC_ERR_CONNECT = -1,    // 張れなかった (DNS / TCP / TLS)
    STACKEE_HC_ERR_SEND = -2,       // 要求 (頭か本文) を書けなかった
    STACKEE_HC_ERR_FETCH = -3,      // 応答の頭を読めなかった
    STACKEE_HC_ERR_READ = -4,       // 本文の途中で切れた
    STACKEE_HC_ERR_TIMEOUT = -5,    // 待ち時間切れ
    STACKEE_HC_ERR_OVERFLOW = -6,   // 受け皿を超えた
    STACKEE_HC_ERR_ABORTED = -7,    // 打ち切った
};

typedef struct {
    const char *method;         // "GET" / "POST"
    const char *url;
    const void *body;
    size_t      body_len;
    const char *content_type;   // POST のときだけ
    int         timeout_ms;
    bool        long_poll;      // 送ったあとも打ち切りに従う (wait= 付き)
    uint8_t    *buf;            // 受け皿 (cap バイトまで)
    size_t      cap;
} stackee_hc_req_t;

typedef struct {
    int      err;               // STACKEE_HC_*
    int      status;
    size_t   received;
    bool     reused;            // 最後の試行は既存の接続で送った
    bool     retried;           // GET を 1 回送り直した
    bool     kept;              // 終わったあと接続を残した
    int      stale;             // 送る前の点検 / POST の古さで捨てた回数
    int      first_err;         // 送り直す前の失敗 (送り直していなければ 0)
    uint32_t connect_ms;        // 最後に張った接続にかかった時間 (使い回しなら 0)
    // 張れた接続の上で読み書きに失敗した (★ 使い回した接続の送信・頭の失敗は
    // 「相手が閉じていた」がほとんどなので含めない)。暗号の自己診断の対象。
    bool     io_failed;
} stackee_hc_result_t;

typedef struct {
    void *ctx;
    uint32_t (*now_ms)(void *ctx);
    bool (*connected)(void *ctx);   // 接続を持っているか
    bool (*stale)(void *ctx);       // 持っている接続が使えない (相手が閉じた等)
    // 要求の頭を送る。接続が無ければ張ってから。*fresh = 今回張ったか。
    // 戻り値 0 / STACKEE_HC_ERR_CONNECT / STACKEE_HC_ERR_SEND。
    int  (*open)(void *ctx, const stackee_hc_req_t *req, bool *fresh,
                 uint32_t *connect_ms);
    // 本文を書く。書けたバイト数 (>0) か STACKEE_HC_ERR_*。
    int  (*write)(void *ctx, const uint8_t *data, size_t len);
    int  (*fetch)(void *ctx, int *status);          // 0 / STACKEE_HC_ERR_*
    // 本文を読む。>0 読めた / 0 もう無い / STACKEE_HC_ERR_*。
    int  (*read)(void *ctx, uint8_t *buf, size_t cap);
    bool (*complete)(void *ctx);    // 本文を最後まで読んだ
    bool (*keep_alive)(void *ctx);  // 相手が持続を許した
    void (*close)(void *ctx);
    bool (*aborted)(void *ctx);
} stackee_hc_ops_t;

typedef struct {
    const stackee_hc_ops_t *ops;
    uint32_t last_used;         // 最後に往復が成功した時刻
    bool     used;              // last_used が有効 (いまの接続で成功したことがある)
    // 数 (inbox.status / talk.status の http に出す)
    uint32_t requests, reused, connects, stale_closed, leftover_closed;
    uint32_t retries, aborted, failures, kept;
} stackee_hc_t;

void stackee_hc_init(stackee_hc_t *hc, const stackee_hc_ops_t *ops);

// 1 往復。戻り値は res->err と同じ。
int stackee_hc_run(stackee_hc_t *hc, const stackee_hc_req_t *req,
                   stackee_hc_result_t *res);

// 持っている接続を捨てる (Wi-Fi が落ちた・設定が変わったとき)。
void stackee_hc_drop(stackee_hc_t *hc);

const char *stackee_hc_err_name(int err);
