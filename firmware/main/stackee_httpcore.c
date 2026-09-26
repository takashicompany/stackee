#include "stackee_httpcore.h"

#include <string.h>

// 1 回の read に渡す大きさの上限。★ 受け皿が 3.84 MB でも一度に頼まない
// (進み具合を数え直せるように、打ち切りを早く見られるように)。
#define READ_STEP 4096

void stackee_hc_init(stackee_hc_t *hc, const stackee_hc_ops_t *ops) {
    memset(hc, 0, sizeof(*hc));
    hc->ops = ops;
}

static void drop(stackee_hc_t *hc) {
    hc->ops->close(hc->ops->ctx);
    hc->used = false;
}

void stackee_hc_drop(stackee_hc_t *hc) {
    if (hc->ops->connected(hc->ops->ctx)) {
        drop(hc);
    }
}

const char *stackee_hc_err_name(int err) {
    switch (err) {
        case STACKEE_HC_OK:           return "ok";
        case STACKEE_HC_ERR_CONNECT:  return "connect";
        case STACKEE_HC_ERR_SEND:     return "send";
        case STACKEE_HC_ERR_FETCH:    return "fetch";
        case STACKEE_HC_ERR_READ:     return "read";
        case STACKEE_HC_ERR_TIMEOUT:  return "timeout";
        case STACKEE_HC_ERR_OVERFLOW: return "overflow";
        case STACKEE_HC_ERR_ABORTED:  return "aborted";
        default:                      return "?";
    }
}

// 1 回ぶん (張る / 使い回す → 送る → 頭 → 本文)。成功なら 0。
// ★ 失敗しても閉じない (閉じるのは呼び手。どちらの道でも必ず閉じる)。
static int attempt(stackee_hc_t *hc, const stackee_hc_req_t *req,
                   stackee_hc_result_t *res, bool *fresh_out) {
    const stackee_hc_ops_t *ops = hc->ops;
    void *ctx = ops->ctx;
    bool get = strcmp(req->method, "GET") == 0;

    // 1. 送る前の点検。
    if (ops->connected(ctx)) {
        bool dead = ops->stale(ctx);
        bool old = !get && (!hc->used ||
                            (uint32_t)(ops->now_ms(ctx) - hc->last_used) >
                                STACKEE_HC_POST_REUSE_MAX_MS);
        if (dead || old) {
            drop(hc);
            hc->stale_closed++;
            res->stale++;
        }
    }

    bool fresh = false;
    uint32_t cms = 0;
    int r = ops->open(ctx, req, &fresh, &cms);
    *fresh_out = fresh;
    res->reused = !fresh && r != STACKEE_HC_ERR_CONNECT;
    res->connect_ms = fresh ? cms : 0;
    if (r == STACKEE_HC_OK) {
        if (fresh) {
            hc->connects++;
        } else {
            hc->reused++;
        }
    }
    if (r != STACKEE_HC_OK) {
        return r;
    }
    // ★ 張っている最中に打ち切られたロングポーリングは、ここで降りる
    //   (頭は送ってしまったので、この接続はもう使えない)。
    if (req->long_poll && ops->aborted(ctx)) {
        return STACKEE_HC_ERR_ABORTED;
    }

    // 2. 本文 (POST)。
    const uint8_t *body = req->body;
    size_t left = req->body_len;
    while (left > 0) {
        int n = ops->write(ctx, body, left);
        if (n <= 0) {
            return (n < 0) ? n : STACKEE_HC_ERR_SEND;
        }
        body += n;
        left -= (size_t)n;
    }

    // 3. 応答の頭。
    int status = 0;
    r = ops->fetch(ctx, &status);
    if (r != STACKEE_HC_OK) {
        return r;
    }
    res->status = status;

    // 4. 本文。★ 「最後まで来たか」より先に「読むものが残っていないか」を
    //   見る。応答の頭と一緒に届いた本文は通信側が抱えていて、complete が
    //   真でもまだ読み出していないことがある (esp_http_client の fetch の作り)。
    size_t got = 0;
    for (;;) {
        if (got >= req->cap) {
            // 受け皿がちょうど埋まった。まだ続きがあれば上限超え。
            // ★ complete だけでは決めない (chunked はちょうど埋まっても
            //   終わりの印をまだ読んでいないことがある)。1 バイト読んで確かめる。
            uint8_t probe;
            int n = ops->read(ctx, &probe, 1);
            res->received = got;
            if (n != 0) {
                return (n < 0) ? n : STACKEE_HC_ERR_OVERFLOW;
            }
            break;
        }
        size_t want = req->cap - got;
        if (want > READ_STEP) {
            want = READ_STEP;
        }
        int n = ops->read(ctx, req->buf + got, want);
        if (n < 0) {
            res->received = got;
            return n;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    res->received = got;
    if (!ops->complete(ctx)) {
        return STACKEE_HC_ERR_READ;     // 相手が途中で閉じた
    }
    return STACKEE_HC_OK;
}

int stackee_hc_run(stackee_hc_t *hc, const stackee_hc_req_t *req,
                   stackee_hc_result_t *res) {
    const stackee_hc_ops_t *ops = hc->ops;
    void *ctx = ops->ctx;
    memset(res, 0, sizeof(*res));
    hc->requests++;
    bool get = strcmp(req->method, "GET") == 0;

    for (int n = 0;; n++) {
        if (ops->aborted(ctx)) {
            // 送る前の打ち切り。何も送っていないので接続は残してよい。
            res->err = STACKEE_HC_ERR_ABORTED;
            hc->aborted++;
            return res->err;
        }
        bool fresh = false;
        int r = attempt(hc, req, res, &fresh);
        if (r == STACKEE_HC_OK) {
            // ★ 最後まで読めて、相手が持続を許したときだけ残す。
            if (ops->keep_alive(ctx) && ops->complete(ctx)) {
                res->kept = true;
                hc->kept++;
                hc->last_used = ops->now_ms(ctx);
                hc->used = true;
            } else {
                drop(hc);
            }
            res->err = STACKEE_HC_OK;
            return STACKEE_HC_OK;
        }

        // 失敗。★ 読み残し・書きかけの接続は必ず捨てる。
        if (ops->connected(ctx)) {
            if (r != STACKEE_HC_ERR_CONNECT) {
                hc->leftover_closed++;
            }
            drop(hc);
        }
        hc->used = false;
        if (r != STACKEE_HC_ERR_ABORTED && ops->aborted(ctx) && req->long_poll) {
            r = STACKEE_HC_ERR_ABORTED;     // shutdown で起こされた
        }
        if (r == STACKEE_HC_ERR_ABORTED) {
            res->err = r;
            hc->aborted++;
            return r;
        }
        if (r == STACKEE_HC_ERR_SEND || r == STACKEE_HC_ERR_FETCH ||
            r == STACKEE_HC_ERR_READ) {
            // 張ったばかりの接続での失敗と、使い回した接続の本文の途中の
            // 失敗は暗号の自己診断に回す (README §20)。
            if (fresh || r == STACKEE_HC_ERR_READ) {
                res->io_failed = true;
            }
        }
        bool retry = get && n == 0 &&
                     (r == STACKEE_HC_ERR_SEND || r == STACKEE_HC_ERR_FETCH ||
                      r == STACKEE_HC_ERR_READ);
        if (!retry) {
            res->err = r;
            hc->failures++;
            return r;
        }
        res->first_err = r;
        res->retried = true;
        res->received = 0;
        res->status = 0;
        hc->retries++;
    }
}
