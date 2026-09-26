// 接続の使い回しの決まり (main/stackee_httpcore.c) を Mac 上で走らせる。
//
// 偽の「接続」を 1 本だけ持つ。台本 (標準入力、1 行 1 命令):
//   t <ms>                 時計を進める
//   resp <status> <len> [close] [chunked] [cut=<n>]
//                          次の応答を積む。close = 相手が持続を許さない
//                          (Connection: close)。cut=n = n バイト送ったところで
//                          相手が閉じる (本文の途中で切れる)
//   fail <open|send|fetch|read|timeout|connect>
//                          次の試行のその段で失敗させる (1 回だけ)
//   peerclose              いま持っている接続を相手が閉じたことにする
//                          (送る前の点検で見つかる)
//   silentdead             いま持っている接続が黙って死んだことにする
//                          (点検では見つからない。送っても返事が来ない)
//   abort <0|1>            打ち切りの印
//   abortafteropen         次の open の直後に打ち切りの印を立てる
//   connectms <ms>         張るのにかかる時間
//   run <GET|POST> <url> [long] [cap=<n>] [body=<n>]
//   drop                   接続を捨てる (stackee_hc_drop)
//
// 出てくる行:
//   OPEN fresh|reused <url>   CLOSE   WRITE <n>
//   RESULT err=<名前> status=<n> got=<n> reused=<0|1> retried=<0|1>
//          kept=<0|1> stale=<n> first=<名前> io=<0|1> connect_ms=<n>
//   COUNTS requests=.. reused=.. connects=.. stale=.. leftover=.. retries=..
//          aborted=.. failures=.. kept=.. conn=<0|1>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_httpcore.h"

#define RESP_MAX 16

static uint32_t g_now;
static uint32_t g_connect_ms = 1500;

static struct {
    int  status;
    int  len;
    bool close;         // 持続を許さない
    bool chunked;       // 終わりの印を読むまで complete にならない
    int  cut;           // -1 = 切らない
} g_resp[RESP_MAX];
static int g_head, g_tail;

static bool g_conn;             // 接続を持っている
static bool g_peer_closed;      // 相手が閉じた (点検で分かる)
static bool g_silent_dead;      // 黙って死んだ (書くと分かる)
static char g_fail[16];         // 次に失敗させる段
static bool g_abort;
static bool g_abort_after_open;

// いま読んでいる応答
static int  g_cur = -1;
static int  g_sent;             // 読ませたバイト数
static bool g_complete;

static uint32_t ops_now(void *ctx) { (void)ctx; return g_now; }
static bool ops_connected(void *ctx) { (void)ctx; return g_conn; }
static bool ops_stale(void *ctx) { (void)ctx; return g_peer_closed; }

static bool take_fail(const char *stage) {
    if (strcmp(g_fail, stage) == 0) {
        g_fail[0] = '\0';
        return true;
    }
    return false;
}

static int ops_open(void *ctx, const stackee_hc_req_t *req, bool *fresh,
                    uint32_t *connect_ms) {
    (void)ctx;
    *fresh = false;
    *connect_ms = 0;
    if (!g_conn) {
        if (take_fail("connect")) {
            printf("OPEN connect-fail %s\n", req->url);
            return STACKEE_HC_ERR_CONNECT;
        }
        g_conn = true;
        g_peer_closed = false;
        g_silent_dead = false;
        *fresh = true;
        *connect_ms = g_connect_ms;
        g_now += g_connect_ms;
    }
    printf("OPEN %s %s %s\n", *fresh ? "fresh" : "reused", req->method, req->url);
    g_cur = -1;
    g_sent = 0;
    g_complete = false;
    if (g_abort_after_open) {
        g_abort_after_open = false;
        g_abort = true;
    }
    if (take_fail("open")) {
        return STACKEE_HC_ERR_SEND;
    }
    return STACKEE_HC_OK;
}

static int ops_write(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    (void)data;
    if (take_fail("send")) {
        return STACKEE_HC_ERR_SEND;
    }
    // 2 回に分けて書く (呼び手が書き足す道を通す)。
    size_t n = len > 1 ? len / 2 : len;
    printf("WRITE %u\n", (unsigned)n);
    return (int)n;
}

static int ops_fetch(void *ctx, int *status) {
    (void)ctx;
    if (take_fail("fetch")) {
        return STACKEE_HC_ERR_FETCH;
    }
    if (take_fail("timeout") || g_silent_dead) {
        return STACKEE_HC_ERR_TIMEOUT;  // 黙って死んだ接続は待っても返らない
    }
    if (g_head == g_tail) {
        return STACKEE_HC_ERR_FETCH;    // 相手が何も返さない
    }
    g_cur = g_head++;
    *status = g_resp[g_cur].status;
    g_complete = (g_resp[g_cur].len == 0);
    return STACKEE_HC_OK;
}

static int ops_read(void *ctx, uint8_t *buf, size_t cap) {
    (void)ctx;
    if (take_fail("read")) {
        return STACKEE_HC_ERR_READ;
    }
    if (g_cur < 0 || g_complete) {
        return 0;
    }
    int limit = g_resp[g_cur].len;
    if (g_resp[g_cur].cut >= 0 && g_resp[g_cur].cut < limit) {
        limit = g_resp[g_cur].cut;
    }
    int left = limit - g_sent;
    if (left <= 0) {
        if (g_resp[g_cur].chunked && g_sent >= g_resp[g_cur].len) {
            g_complete = true;          // 終わりの印 (0\r\n\r\n) を読んだ
        }
        return 0;                       // 終わり / 相手が途中で閉じた
    }
    int n = (int)cap < left ? (int)cap : left;
    if (n > 1000) {
        n = 1000;
    }
    memset(buf, 'x', (size_t)n);        // 受け皿の外へ書いていれば ASan が言う
    g_sent += n;
    if (g_sent >= g_resp[g_cur].len && !g_resp[g_cur].chunked) {
        g_complete = true;
    }
    return n;
}

static bool ops_complete(void *ctx) { (void)ctx; return g_complete; }

static bool ops_keep_alive(void *ctx) {
    (void)ctx;
    return g_cur >= 0 && !g_resp[g_cur].close;
}

static void ops_close(void *ctx) {
    (void)ctx;
    if (g_conn) {
        printf("CLOSE\n");
    }
    g_conn = false;
    g_peer_closed = false;
    g_silent_dead = false;
}

static bool ops_aborted(void *ctx) { (void)ctx; return g_abort; }

static const stackee_hc_ops_t OPS = {
    .ctx = NULL,
    .now_ms = ops_now,
    .connected = ops_connected,
    .stale = ops_stale,
    .open = ops_open,
    .write = ops_write,
    .fetch = ops_fetch,
    .read = ops_read,
    .complete = ops_complete,
    .keep_alive = ops_keep_alive,
    .close = ops_close,
    .aborted = ops_aborted,
};

int main(void) {
    stackee_hc_t hc;
    stackee_hc_init(&hc, &OPS);
    char line[512];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *arg = strchr(line, ' ');
        if (arg) { *arg++ = '\0'; }
        if (strcmp(line, "t") == 0) {
            g_now += arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "connectms") == 0) {
            g_connect_ms = arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "resp") == 0 && g_tail < RESP_MAX) {
            int status = 200, len = 0;
            char rest[256] = "";
            sscanf(arg ? arg : "", "%d %d %255[^\n]", &status, &len, rest);
            g_resp[g_tail].status = status;
            g_resp[g_tail].len = len;
            g_resp[g_tail].close = strstr(rest, "close") != NULL;
            g_resp[g_tail].chunked = strstr(rest, "chunked") != NULL;
            const char *cut = strstr(rest, "cut=");
            g_resp[g_tail].cut = cut ? atoi(cut + 4) : -1;
            g_tail++;
        } else if (strcmp(line, "fail") == 0) {
            snprintf(g_fail, sizeof(g_fail), "%s", arg ? arg : "");
        } else if (strcmp(line, "peerclose") == 0) {
            g_peer_closed = g_conn;
        } else if (strcmp(line, "silentdead") == 0) {
            g_silent_dead = g_conn;
        } else if (strcmp(line, "abort") == 0) {
            g_abort = arg && atoi(arg) != 0;
        } else if (strcmp(line, "abortafteropen") == 0) {
            g_abort_after_open = true;
        } else if (strcmp(line, "drop") == 0) {
            stackee_hc_drop(&hc);
        } else if (strcmp(line, "run") == 0) {
            char method[8] = "GET", url[256] = "/";
            char rest[256] = "";
            sscanf(arg ? arg : "", "%7s %255s %255[^\n]", method, url, rest);
            size_t cap = 8192;
            const char *c = strstr(rest, "cap=");
            if (c) { cap = (size_t)atol(c + 4); }
            size_t body_len = 0;
            const char *b = strstr(rest, "body=");
            if (b) { body_len = (size_t)atol(b + 5); }
            uint8_t *buf = malloc(cap + 1);
            uint8_t *body = body_len ? calloc(body_len, 1) : NULL;
            stackee_hc_req_t req = {
                .method = method, .url = url, .body = body, .body_len = body_len,
                .content_type = body_len ? "application/json" : NULL,
                .timeout_ms = 30000, .long_poll = strstr(rest, "long") != NULL,
                .buf = buf, .cap = cap,
            };
            stackee_hc_result_t res;
            stackee_hc_run(&hc, &req, &res);
            printf("RESULT err=%s status=%d got=%u reused=%d retried=%d kept=%d "
                   "stale=%d first=%s io=%d connect_ms=%u\n",
                   stackee_hc_err_name(res.err), res.status, (unsigned)res.received,
                   res.reused ? 1 : 0, res.retried ? 1 : 0, res.kept ? 1 : 0,
                   res.stale, stackee_hc_err_name(res.first_err),
                   res.io_failed ? 1 : 0, (unsigned)res.connect_ms);
            free(buf);
            free(body);
        } else if (strcmp(line, "counts") == 0) {
            printf("COUNTS requests=%u reused=%u connects=%u stale=%u leftover=%u "
                   "retries=%u aborted=%u failures=%u kept=%u conn=%d\n",
                   (unsigned)hc.requests, (unsigned)hc.reused,
                   (unsigned)hc.connects, (unsigned)hc.stale_closed,
                   (unsigned)hc.leftover_closed, (unsigned)hc.retries,
                   (unsigned)hc.aborted, (unsigned)hc.failures,
                   (unsigned)hc.kept, g_conn ? 1 : 0);
        } else {
            fprintf(stderr, "unknown command: %s\n", line);
            return 2;
        }
    }
    return 0;
}
