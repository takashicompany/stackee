#include "stackee_http.h"
#include "stackee_cryptocheck.h"
#include "stackee_heapdiag.h"
#include "stackee_httpcore.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "http";

#define URL_MAX     256
#define TOKEN_MAX   160
#define TIMEOUT_MS  30000
// ロングポーリング ("/jobs/<id>?wait=<秒>" / "/inbox?after=..&wait=<秒>") の
// ときだけ、待つ秒数 + これだけ待つ。中継 (dev/server/proxy.py) は wait 秒まで
// 応答を握るので、30 秒では自分から切ってしまう。
#define WAIT_MARGIN_MS 15000
// esp_http_client の送受信の作業領域。★ 4096 を**超える**大きさにする。
// malloc は 4096 B 以下を内蔵 RAM に取る (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
// ので、2048 / 1024 のままだと接続を持ち続けるあいだ内蔵 RAM に 3 KB 居座る。
#define CLIENT_RX_BUF  4100
#define CLIENT_TX_BUF  4100
// 受け皿がこれより大きい要求 (返答の PCM) は、送ったあとでも打ち切りに従う
// (残りを読み捨てずに接続ごと捨てる)。
#define BIG_BODY       65536
// 先に張るときの受け皿。★ 未読の発話 (字幕の本文つき) が即座に返っても
// 溢れないよう、受け箱の応答と同じ 16 KB (溢れると接続を捨ててしまう)。
#define WARMUP_LIMIT   16384
// 暗号の自己診断 (README §20) を回す間隔の下限 [us]。常時ポーリングで中継が
// 落ちているあいだ、失敗のたびに回さない。
#define CRYPTO_CHECK_GAP_US (10LL * 60 * 1000000)

typedef struct {
    char        url[URL_MAX + STACKEE_HTTP_PATH_ROOM];
    const char *method;
    const void *body;
    size_t      body_len;
    size_t      limit;
    const char *content_type;   // 呼び手の定数文字列を指すだけ (写さない)
    bool        cuttable;       // 送ったあとも打ち切りに従う
    bool        warmup;         // 接続を張っておくだけ (返事は捨てる)
    bool        quiet;          // 常時ポーリングの待ち (ログを間引く)
    int         timeout_ms;
} job_t;

static struct {
    char     base[URL_MAX];
    char     token[TOKEN_MAX];
    bool     secure;

    TaskHandle_t task;
    SemaphoreHandle_t go;
    // ★ 要求の受け付け・打ち切り・次への繰り上げと、ソケット番号の出し入れを
    //   守る錠。audio タスク (状態機械) と camera タスク (撮影の押さえ) と
    //   ワーカーが触る。持つのは数 µs だけ (通信中は持たない)。
    SemaphoreHandle_t lock;

    job_t    cur;
    uint8_t *buf;
    size_t   buf_cap;
    // 打ち切り中に来た次の要求 (1 本だけ)。ワーカーが今のを畳んだら繰り上げる。
    job_t    pending;
    uint8_t *pending_buf;
    bool     has_pending;

    _Atomic size_t received;
    _Atomic int state;
    _Atomic int status;
    _Atomic int err;
    _Atomic bool abort;
    _Atomic uint32_t elapsed_ms;

    // ---- 持ち続ける 1 本の接続 (ワーカーだけが触る。sock / sent は錠の中) ----
    esp_http_client_handle_t client;
    bool     conn;              // 張ってある
    int      sock;              // 打ち切り (shutdown) 用。張っていなければ -1
    bool     sent;              // いまの要求の頭を送り終えた
    bool     connected_evt;     // この open で張った (HTTP_EVENT_ON_CONNECTED)
    int64_t  connected_us;
    stackee_hc_t hc;
    int64_t  crypto_last_us;

    uint32_t requests, failures, last_ms;
    size_t   last_bytes;
    // 直近に終わった要求 (打ち切りは数えない)。設定メニューの「直近の通信」。
    bool     any_done;
    bool     last_ok;           // 通信が通り、HTTP 2xx だった
    int      last_done_status;
    int64_t  last_done_us;
    uint32_t discarded;         // 打ち切って結果を捨てた要求
    uint32_t shutdowns;         // 待っている要求をソケットごと起こした回数
    uint32_t last_connect_ms;
    bool     last_reused;
    uint32_t warmups;           // 録音中に先に張った回数
    uint32_t quiet_n;
} h = { .sock = -1 };

static bool lock(void) {
    return h.lock != NULL && xSemaphoreTake(h.lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void unlock(void) {
    xSemaphoreGive(h.lock);
}

// ---------------------------------------------------------------------------
esp_err_t stackee_http_configure(const char *base, const char *token) {
    if (base == NULL || base[0] == '\0') {
        h.base[0] = '\0';
        h.token[0] = '\0';
        return ESP_ERR_INVALID_ARG;
    }
    bool secure = strncmp(base, "https://", 8) == 0;
    if (!secure && strncmp(base, "http://", 7) != 0) {
        ESP_LOGE(TAG, "会話 URL は http:// か https:// で始まること");
        return ESP_ERR_INVALID_ARG;
    }
    if (token != NULL && token[0] != '\0' && !secure) {
        // ★ 現行 stackee_talk.py と同じ拒否。平文にトークンを載せない。
        ESP_LOGE(TAG, "認証トークンには HTTPS が必要です");
        return ESP_ERR_INVALID_ARG;
    }
    h.secure = secure;
    snprintf(h.base, sizeof(h.base), "%s", base);
    snprintf(h.token, sizeof(h.token), "%s", token ? token : "");
    // ★ ログに出すのは「トークンがあるか」だけ。値は出さない。
    ESP_LOGI(TAG, "会話の相手 %s (token %s)", h.base, h.token[0] ? "あり" : "なし");
    return ESP_OK;
}

bool stackee_http_configured(void) {
    return h.base[0] != '\0';
}

const char *stackee_http_base(void) {
    return h.base;
}

bool stackee_http_has_token(void) {
    return h.token[0] != '\0';
}

bool stackee_http_warm(void) {
    return h.conn && h.hc.used;
}

// この 1 本に許す時間 [ms]。URL に "?wait=<秒>" (受け箱では "&wait=<秒>")
// が付いていれば、その秒数を足したぶんだけ待つ (ロングポーリング)。
// ほかの要求は従来どおり 30 秒。
// ★ ここで見るのは会話の状態機械が組み立てた自分の URL だけ。
static int wait_seconds(const char *url) {
    const char *at = strstr(url, "?wait=");
    if (at == NULL) {
        at = strstr(url, "&wait=");     // GET /inbox?after=<seq>&wait=25&job=
    }
    if (at == NULL) {
        return 0;
    }
    int seconds = 0;
    for (const char *p = at + 6; *p >= '0' && *p <= '9'; p++) {
        seconds = seconds * 10 + (*p - '0');
        if (seconds > 300) {
            return 0;
        }
    }
    return seconds;
}

static int timeout_for(const char *url) {
    int seconds = wait_seconds(url);
    return seconds > 0 ? seconds * 1000 + WAIT_MARGIN_MS : TIMEOUT_MS;
}

// ---------------------------------------------------------------------------
// esp_http_client をつなぐ (stackee_httpcore の ops)
// ---------------------------------------------------------------------------
static esp_err_t on_event(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        h.connected_evt = true;
        h.connected_us = esp_timer_get_time();
    }
    return ESP_OK;
}

static bool ensure_client(void) {
    if (h.client != NULL) {
        return true;
    }
    char url[URL_MAX + 2];
    snprintf(url, sizeof(url), "%s/", h.base);
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TIMEOUT_MS,
        .event_handler = on_event,
        .disable_auto_redirect = true,
        .buffer_size = CLIENT_RX_BUF,
        .buffer_size_tx = CLIENT_TX_BUF,
        // ★ これは TCP の SO_KEEPALIVE。HTTP の持続接続とは別物 (持続は
        //   HTTP/1.1 の既定で、相手が Connection: close を返さない限り続く)。
        .keep_alive_enable = false,
    };
    if (h.secure) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    h.client = esp_http_client_init(&config);
    return h.client != NULL;
}

static uint32_t ad_now(void *ctx) {
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool ad_connected(void *ctx) {
    (void)ctx;
    return h.client != NULL && h.conn;
}

// 送る前の点検。★ 待たない (select の待ち時間 0)。読めるものがある =
// 相手が閉じた (FIN / RST) か、頼んでいないバイト (TLS の close_notify など)
// が来ている。どちらもこの接続には次の要求を載せられない。
static bool ad_stale(void *ctx) {
    (void)ctx;
    int fd = esp_http_client_get_socket(h.client);
    if (fd < 0) {
        return true;
    }
    fd_set rfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&efds);
    FD_SET(fd, &rfds);
    FD_SET(fd, &efds);
    struct timeval tv = { 0, 0 };
    int n = select(fd + 1, &rfds, NULL, &efds, &tv);
    if (n != 0) {
        ESP_LOGI(TAG, "持っていた接続は使えない (%s)。張り直す",
                 n < 0 ? "select 失敗" : "相手が閉じた / 余分なバイト");
        return true;
    }
    return false;
}

static void ad_close(void *ctx) {
    (void)ctx;
    if (lock()) {
        h.sock = -1;
        h.sent = false;
        unlock();
    }
    if (h.client != NULL) {
        // ★ 応答の頭と一緒に届いて、まだ読み出していない本文 (esp_http_client
        //   が抱えている写し) も捨てる。残すと次の応答の頭に混ざる。
        esp_http_client_clear_response_buffer(h.client);
        esp_http_client_close(h.client);
    }
    h.conn = false;
}

static int ad_open(void *ctx, const stackee_hc_req_t *req, bool *fresh,
                   uint32_t *connect_ms) {
    (void)ctx;
    *fresh = false;
    *connect_ms = 0;
    if (!ensure_client()) {
        return STACKEE_HC_ERR_CONNECT;
    }
    esp_http_client_handle_t c = h.client;
    // ★ 同じホストなら set_url は接続を保つ (ホストが変わったときだけ閉じる)。
    if (esp_http_client_set_url(c, req->url) != ESP_OK) {
        return STACKEE_HC_ERR_CONNECT;
    }
    bool post = strcmp(req->method, "POST") == 0;
    esp_http_client_set_method(c, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_timeout_ms(c, req->timeout_ms);
    if (h.token[0] != '\0') {
        char header[TOKEN_MAX + 16];
        snprintf(header, sizeof(header), "Bearer %s", h.token);
        esp_http_client_set_header(c, "Authorization", header);
    }
    if (post) {
        esp_http_client_set_header(c, "Content-Type",
                                   req->content_type ? req->content_type : "audio/wav");
    } else {
        esp_http_client_delete_header(c, "Content-Type");
    }
    h.connected_evt = false;
    bool had = h.conn;
    int64_t t0 = esp_timer_get_time();
    // 無ければ張ってから、要求の頭 (と Content-Length) を送る。
    esp_err_t err = esp_http_client_open(c, (int)req->body_len);
    *fresh = h.connected_evt;
    if (h.connected_evt) {
        *connect_ms = (uint32_t)((h.connected_us - t0) / 1000);
    }
    if (err != ESP_OK) {
        if (had || h.connected_evt) {
            // 接続はあった (使い回し / 張れた) が頭を書けなかった。
            // 閉じるのは呼び手 (stackee_httpcore) の決まりに任せる。
            h.conn = true;
            return STACKEE_HC_ERR_SEND;
        }
        // 張れなかった。途中まで作ったものを片付ける。
        ad_close(NULL);
        return STACKEE_HC_ERR_CONNECT;
    }
    h.conn = true;
    if (lock()) {
        h.sock = esp_http_client_get_socket(c);
        h.sent = true;
        // ★ 送り終えた瞬間に打ち切られていたなら、ここで起こす
        //   (close 側は sent を見てから shutdown する)。
        if (atomic_load(&h.abort) && h.cur.cuttable && h.sock >= 0) {
            shutdown(h.sock, SHUT_RDWR);
            h.shutdowns++;
        }
        unlock();
    }
    return STACKEE_HC_OK;
}

static int ad_write(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    int n = esp_http_client_write(h.client, (const char *)data, (int)len);
    return (n > 0) ? n : STACKEE_HC_ERR_SEND;
}

static int ad_fetch(void *ctx, int *status) {
    (void)ctx;
    int64_t n = esp_http_client_fetch_headers(h.client);
    if (n < 0) {
        return (n == -ESP_ERR_HTTP_EAGAIN) ? STACKEE_HC_ERR_TIMEOUT
                                           : STACKEE_HC_ERR_FETCH;
    }
    *status = esp_http_client_get_status_code(h.client);
    return STACKEE_HC_OK;
}

static int ad_read(void *ctx, uint8_t *buf, size_t cap) {
    (void)ctx;
    int n = esp_http_client_read(h.client, (char *)buf, (int)cap);
    if (n == -ESP_ERR_HTTP_EAGAIN) {
        return STACKEE_HC_ERR_TIMEOUT;
    }
    if (n < 0) {
        return STACKEE_HC_ERR_READ;
    }
    if (n > 0) {
        atomic_fetch_add(&h.received, (size_t)n);
    }
    return n;
}

static bool ad_complete(void *ctx) {
    (void)ctx;
    return esp_http_client_is_complete_data_received(h.client);
}

static bool ad_keep_alive(void *ctx) {
    (void)ctx;
    return esp_http_client_is_persistent_connection(h.client);
}

static bool ad_aborted(void *ctx) {
    (void)ctx;
    return atomic_load(&h.abort);
}

static const stackee_hc_ops_t HC_OPS = {
    .ctx = NULL,
    .now_ms = ad_now,
    .connected = ad_connected,
    .stale = ad_stale,
    .open = ad_open,
    .write = ad_write,
    .fetch = ad_fetch,
    .read = ad_read,
    .complete = ad_complete,
    .keep_alive = ad_keep_alive,
    .close = ad_close,
    .aborted = ad_aborted,
};

// ---------------------------------------------------------------------------
// ワーカー
// ---------------------------------------------------------------------------
static void maybe_cryptocheck(const stackee_hc_result_t *res) {
    // ★ 張れなかった (TLS の握手を含む) か、張れた接続で読み書きに失敗した
    //   ときに、その場で暗号の自己診断を回す (README §20)。壊れていれば印が
    //   立ち、main が会話の合間に再起動する。
    if (!h.secure || !(res->err == STACKEE_HC_ERR_CONNECT || res->io_failed)) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (h.crypto_last_us != 0 && now - h.crypto_last_us < CRYPTO_CHECK_GAP_US) {
        return;
    }
    h.crypto_last_us = now;
    stackee_cryptocheck_result_t r;
    bool ok = stackee_cryptocheck_run(&r);
    ESP_LOGW(TAG, "  暗号の自己診断: %s (sha int=%d psram=%d / x509 sig_ok=%d)",
             ok ? "OK (壊れていない = 原因は暗号の外)" : "NG (壊れている → 再起動で戻す)",
             r.sha_internal_4k, r.sha_psram_4k, r.x509_sig_ok);
}

static void worker(void *unused) {
    (void)unused;
    for (;;) {
        xSemaphoreTake(h.go, portMAX_DELAY);
        for (;;) {
            // 今の要求 (request() か繰り上げで h.cur に入っている)。
            int64_t t0 = esp_timer_get_time();
            size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            if (!h.cur.quiet) {
                ESP_LOGI(TAG, "%s 開始%s (内蔵RAM 空き %u B / 最大の塊 %u B / PSRAM %u B / 接続 %s)",
                     h.cur.method, h.cur.warmup ? " (先に張る)" : "",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                     h.conn ? "あり" : "なし");
            }
            stackee_hc_req_t req = {
                .method = h.cur.method,
                .url = h.cur.url,
                .body = h.cur.body,
                .body_len = h.cur.body_len,
                .content_type = h.cur.content_type,
                .timeout_ms = h.cur.timeout_ms,
                .long_poll = h.cur.cuttable,
                .buf = h.buf,
                .cap = h.buf_cap,
            };
            stackee_hc_result_t res;
            stackee_hc_run(&h.hc, &req, &res);
            uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
            h.last_connect_ms = res.connect_ms;
            {
                // ★ 1 往復で内蔵 RAM が 1 KB 以上戻ってこなかったら目印を置く
                //   (heap.info の marks。README §24 の「消えた内蔵 RAM」を追う)。
                size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                if (free_after + 1024 < free_before) {
                    char what[24];
                    const char *path = strstr(h.cur.url + (h.secure ? 8 : 7), "/");
                    snprintf(what, sizeof(what), "-%u %s",
                             (unsigned)(free_before - free_after),
                             path ? path : "?");
                    stackee_heapdiag_mark(what);
                }
            }
            h.last_reused = res.reused;

            bool gave_up = atomic_load(&h.abort) || h.cur.warmup;
            // ★ 常時ポーリングの待ち (25 秒ごと) は、張り直した・送り直した・
            //   200 以外・20 本に 1 本のときだけ行を残す (ログのリングを
            //   押し流さないため)。
            bool loud = !h.cur.quiet || !res.reused || res.retried ||
                        res.stale > 0 || res.status != 200 ||
                        (h.quiet_n++ % 20) == 0;
            if (res.err != STACKEE_HC_OK && !gave_up) {
                ESP_LOGE(TAG, "%s %s 失敗 (%s, status=%d, %lums, reused=%d, retried=%d)",
                         h.cur.method, h.cur.url + (h.secure ? 8 : 7),
                         stackee_hc_err_name(res.err), res.status,
                         (unsigned long)ms, res.reused ? 1 : 0, res.retried ? 1 : 0);
                ESP_LOGE(TAG, "  失敗時の内蔵RAM 空き %u B / 最大の塊 %u B",
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
                maybe_cryptocheck(&res);
            } else if (!gave_up && loud) {
                // ★ ログは「終わった」と知らせる**前**に出す (あとにすると次の
                //   要求が h.cur を書き換えたものを出しかねない)。path も
                //   token も本文も出さない。
                // ★ connect_ms は TCP + TLS を張った時間 (使い回したら 0)、
                //   reused は既存の接続に載せたか。worker_ms との差が HTTP の往復。
                // ★ 終わったあとの内蔵 RAM も残す (README §24)。
                ESP_LOGI(TAG, "[talk-http-timing] {\"method\":\"%s\",\"status\":%d,"
                              "\"bytes\":%u,\"worker_ms\":%lu,\"connect_ms\":%lu,"
                              "\"reused\":%d,\"retried\":%d,\"stale\":%d,\"kept\":%d,"
                              "\"internal_free\":%u,\"internal_largest\":%u}",
                         h.cur.method, res.status, (unsigned)res.received,
                         (unsigned long)ms, (unsigned long)res.connect_ms,
                         res.reused ? 1 : 0, res.retried ? 1 : 0, res.stale,
                         res.kept ? 1 : 0,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            }

            // ★ 「打ち切られたか」の判定と結果の公開は錠の中で一度に行う。
            //   分けると、公開の直前に打ち切り + 次の要求が来たとき、次の要求の
            //   呼び手が前の要求の本文を自分の応答として読んでしまう。
            while (!lock()) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            // ★ 「頭を送り終えた」印はこの要求かぎり。残すと、次の要求を
            //   送る前の打ち切りが使い回し中の接続を shutdown してしまう。
            h.sent = false;
            if (atomic_load(&h.abort) || h.cur.warmup) {
                // ★ 呼び手はもうこの要求を見ていない (か、先に張っただけ)。
                //   結果は捨てる。
                if (h.cur.warmup) {
                    ESP_LOGI(TAG, "先に張った (%s, connect_ms=%lu, 接続 %s)",
                             stackee_hc_err_name(res.err),
                             (unsigned long)res.connect_ms, h.conn ? "あり" : "なし");
                } else {
                    h.discarded++;
                }
                free(h.buf);
                h.buf = NULL;
                h.buf_cap = 0;
                if (!h.cur.warmup) {
                    ESP_LOGI(TAG, "%s 打ち切り (%s, %lums, 接続 %s)", h.cur.method,
                             stackee_hc_err_name(res.err), (unsigned long)ms,
                             h.conn ? "残した" : "捨てた");
                }
                if (h.has_pending) {
                    // 待っていた次の要求を繰り上げる。state は RUNNING のまま。
                    h.cur = h.pending;
                    h.buf = h.pending_buf;
                    h.buf_cap = h.pending.limit;
                    h.pending_buf = NULL;
                    h.has_pending = false;
                    atomic_store(&h.received, 0);
                    atomic_store(&h.status, 0);
                    atomic_store(&h.err, 0);
                    atomic_store(&h.elapsed_ms, 0);
                    atomic_store(&h.abort, false);
                    unlock();
                    continue;
                }
                atomic_store(&h.received, 0);
                atomic_store(&h.state, STACKEE_HTTP_IDLE);
                unlock();
                break;
            }
            atomic_store(&h.elapsed_ms, ms);
            atomic_store(&h.status, res.status);
            atomic_store(&h.received, res.received);
            h.any_done = true;
            h.last_done_us = esp_timer_get_time();
            h.last_done_status = res.status;
            h.last_ok = (res.err == STACKEE_HC_OK) && res.status >= 200 && res.status < 300;
            if (res.err != STACKEE_HC_OK) {
                atomic_store(&h.err, res.err == STACKEE_HC_ERR_OVERFLOW ? -2 : res.err);
                h.failures++;
                atomic_store(&h.state, STACKEE_HTTP_ERROR);
                unlock();
                break;
            }
            // ★ `<=`。入れ物は limit + 1 バイト取ってあるので、ちょうど limit
            //   バイト受けたときも終端を置ける。
            if (h.buf != NULL && res.received <= h.buf_cap) {
                h.buf[res.received] = '\0';     // JSON をそのまま文字列として読めるように
            }
            h.last_ms = ms;
            h.last_bytes = res.received;
            atomic_store(&h.state, STACKEE_HTTP_DONE);
            unlock();
            break;
        }
    }
}

esp_err_t stackee_http_start(void) {
    if (h.task != NULL) {
        return ESP_OK;
    }
    h.go = xSemaphoreCreateBinary();
    h.lock = xSemaphoreCreateMutex();
    if (h.go == NULL || h.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    stackee_hc_init(&h.hc, &HC_OPS);
    // CPU0 / 低優先度。入力 (CPU1 / 最高) とは別の CPU。
    // ★ スタックは 10 KB。TLS のハンドシェイクが 6 KB では足りない
    //   (ESP-IDF の HTTPS の例も 8 KB 以上を使っている)。
    // ★★ 優先度は 1 (main / lcd と同じ、touch・ui の 3 より下)。2026-09-26 まで
    //   4 で、TLS の握手 (約 4.5〜5 秒の計算) のあいだ touch・ui・console が
    //   ほぼ回れず、タッチと顔が固まっていた。握手は計算だけなので、
    //   下の優先度で回しても周期の短い仕事の合間に進むだけ。
    //   (2026-09-27 からは接続を使い回すので、握手そのものが滅多に起きない)
    if (xTaskCreatePinnedToCore(worker, "stackee_http", 10240, NULL, 1, &h.task, 0)
            != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static uint8_t *alloc_buf(size_t limit) {
    uint8_t *buf = heap_caps_malloc(limit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        buf = heap_caps_malloc(limit + 1, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        // ★ 返答音声の受け皿は 120 秒ぶん = 3.84 MB ある。ここで落ちたときに
        //   「合計が足りない」のか「連続した塊が無い」のかが分かるように、
        //   両方を残す (会話は呼び手が失敗として畳む)。
        ESP_LOGE(TAG, "受信用の %u B を確保できない "
                      "(PSRAM 空き %u B / 最大の塊 %u B / 内蔵 %u B)",
                 (unsigned)(limit + 1),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return NULL;
    }
    buf[0] = '\0';
    return buf;
}

static bool fill_job(job_t *job, const char *method, const char *path,
                     const void *body, size_t body_len, size_t limit,
                     const char *content_type) {
    int n = snprintf(job->url, sizeof(job->url), "%s%s", h.base, path);
    if (n < 0 || (size_t)n >= sizeof(job->url)) {
        return false;
    }
    job->method = (strcmp(method, "POST") == 0) ? "POST" : "GET";
    job->body = body;
    job->body_len = body_len;
    job->content_type = (content_type != NULL) ? content_type : "audio/wav";
    job->limit = limit;
    job->timeout_ms = timeout_for(job->url);
    job->cuttable = wait_seconds(job->url) > 0 || limit > BIG_BODY;
    job->warmup = false;
    job->quiet = strcmp(job->method, "GET") == 0 && wait_seconds(job->url) > 0 &&
                 strstr(path, "/inbox") != NULL;
    return true;
}

static void release_buffer_locked(void) {
    if (h.buf != NULL) {
        free(h.buf);
        h.buf = NULL;
        h.buf_cap = 0;
    }
    atomic_store(&h.received, 0);
    atomic_store(&h.state, STACKEE_HTTP_IDLE);
}

bool stackee_http_request(const char *method, const char *path,
                          const void *body, size_t body_len, size_t limit,
                          const char *content_type) {
    if (h.task == NULL || h.base[0] == '\0' || path == NULL || path[0] != '/') {
        return false;
    }
    for (const char *p = path; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == ' ') {
            return false;
        }
    }
    if (limit == 0) {
        limit = 8192;
    }
    if (!lock()) {
        return false;
    }
    int state = atomic_load(&h.state);
    if (state == STACKEE_HTTP_RUNNING) {
        if (!atomic_load(&h.abort) && !h.cur.warmup) {
            unlock();
            return false;               // 同時に 1 本だけ
        }
        // ★ 前の要求を打ち切ったところ (常時ポーリングの待ちをキーで
        //   打ち切った直後など) か、録音中に先に張っているところ。
        //   ワーカーが畳み終えたら (張り終えたら) すぐ始める。
        if (h.has_pending) {
            free(h.pending_buf);
            h.pending_buf = NULL;
            h.has_pending = false;
        }
        uint8_t *buf = alloc_buf(limit);
        if (buf == NULL || !fill_job(&h.pending, method, path, body, body_len,
                                     limit, content_type)) {
            free(buf);
            unlock();
            return false;
        }
        h.pending_buf = buf;
        h.has_pending = true;
        h.requests++;
        unlock();
        return true;
    }
    if (state != STACKEE_HTTP_IDLE) {
        // 前の往復 (成功・失敗) の後始末をここで済ませる。
        release_buffer_locked();
    }
    uint8_t *buf = alloc_buf(limit);
    if (buf == NULL || !fill_job(&h.cur, method, path, body, body_len, limit,
                                 content_type)) {
        free(buf);
        unlock();
        return false;
    }
    h.buf = buf;
    h.buf_cap = limit;
    atomic_store(&h.received, 0);
    atomic_store(&h.status, 0);
    atomic_store(&h.err, 0);
    atomic_store(&h.abort, false);
    atomic_store(&h.elapsed_ms, 0);
    atomic_store(&h.state, STACKEE_HTTP_RUNNING);
    h.requests++;
    unlock();
    xSemaphoreGive(h.go);
    return true;
}

bool stackee_http_prewarm(const char *path) {
    if (h.task == NULL || h.base[0] == '\0' || path == NULL || path[0] != '/') {
        return false;
    }
    if (!lock()) {
        return false;
    }
    int state = atomic_load(&h.state);
    if (state == STACKEE_HTTP_RUNNING) {
        // ★ 常時ポーリングの待ちを打ち切った直後 (ワーカーが畳んでいる最中)。
        //   畳み終えたら張る (打ち切った接続は捨てられるので、ここで張らないと
        //   離したあとの POST が握手を待つことになる)。
        if (atomic_load(&h.abort) && !h.has_pending) {
            uint8_t *pbuf = alloc_buf(WARMUP_LIMIT);
            if (pbuf != NULL && fill_job(&h.pending, "GET", path, NULL, 0, WARMUP_LIMIT, NULL)) {
                h.pending.warmup = true;
                h.pending.cuttable = false;
                h.pending.quiet = false;
                h.pending_buf = pbuf;
                h.has_pending = true;
                h.warmups++;
                unlock();
                return true;
            }
            free(pbuf);
        }
        unlock();
        return false;
    }
    // もう使い回せる接続があるなら何もしない。
    if (h.conn && h.hc.used) {
        unlock();
        return false;
    }
    if (state != STACKEE_HTTP_IDLE) {
        release_buffer_locked();
    }
    uint8_t *buf = alloc_buf(WARMUP_LIMIT);
    if (buf == NULL || !fill_job(&h.cur, "GET", path, NULL, 0, WARMUP_LIMIT, NULL)) {
        free(buf);
        unlock();
        return false;
    }
    h.cur.warmup = true;
    h.cur.cuttable = false;
    h.cur.quiet = false;
    h.buf = buf;
    h.buf_cap = WARMUP_LIMIT;
    atomic_store(&h.received, 0);
    atomic_store(&h.status, 0);
    atomic_store(&h.err, 0);
    atomic_store(&h.abort, false);
    atomic_store(&h.elapsed_ms, 0);
    atomic_store(&h.state, STACKEE_HTTP_RUNNING);
    h.warmups++;
    unlock();
    xSemaphoreGive(h.go);
    return true;
}

int stackee_http_state(int *status, size_t *received, int *err, uint32_t *elapsed_ms) {
    if (status)     { *status = atomic_load(&h.status); }
    if (received)   { *received = atomic_load(&h.received); }
    if (err)        { *err = atomic_load(&h.err); }
    if (elapsed_ms) { *elapsed_ms = atomic_load(&h.elapsed_ms); }
    return atomic_load(&h.state);
}

const uint8_t *stackee_http_body(size_t *len) {
    if (atomic_load(&h.state) != STACKEE_HTTP_DONE) {
        return NULL;
    }
    if (len) { *len = atomic_load(&h.received); }
    return h.buf;
}

uint8_t *stackee_http_take(size_t *len) {
    if (!lock()) {
        return NULL;
    }
    uint8_t *buf = NULL;
    if (atomic_load(&h.state) == STACKEE_HTTP_DONE && h.buf != NULL) {
        buf = h.buf;
        if (len) { *len = atomic_load(&h.received); }
        // ★ 受信バッファの持ち主を呼び手に移す。通信側は空に戻る
        //   (release_buffer_locked は h.buf が NULL なので何も返さない)。
        h.buf = NULL;
        h.buf_cap = 0;
        release_buffer_locked();
    }
    unlock();
    return buf;
}

void stackee_http_close(void) {
    if (!lock()) {
        return;
    }
    int state = atomic_load(&h.state);
    if (state == STACKEE_HTTP_RUNNING) {
        // ★ 待たない。呼び手はこの要求 (繰り上げ待ちのものを含む) を手放す。
        //   ロングポーリングと大きな本文は、送り終えていればソケットを
        //   shutdown して待ちを起こす (データが来るまで最大 40 秒止まって
        //   いる recv を今すぐ返らせる)。短い要求は最後まで読ませて、
        //   接続を次の要求に残す。受信用の領域はワーカーが片付ける。
        if (h.has_pending) {
            free(h.pending_buf);
            h.pending_buf = NULL;
            h.has_pending = false;
        }
        atomic_store(&h.abort, true);
        if (h.cur.cuttable && h.sent && h.sock >= 0) {
            shutdown(h.sock, SHUT_RDWR);
            h.shutdowns++;
        }
        unlock();
        return;
    }
    release_buffer_locked();
    unlock();
}

void stackee_http_stats(stackee_http_stats_t *out) {
    out->state = atomic_load(&h.state);
    out->status = atomic_load(&h.status);
    out->err = atomic_load(&h.err);
    out->requests = h.requests;
    out->failures = h.failures;
    out->last_ms = h.last_ms;
    out->last_bytes = h.last_bytes;
    out->conn = h.conn;
    out->reused = h.hc.reused;
    out->connects = h.hc.connects;
    out->stale_closed = h.hc.stale_closed;
    out->leftover_closed = h.hc.leftover_closed;
    out->retries = h.hc.retries;
    out->discarded = h.discarded;
    out->shutdowns = h.shutdowns;
    out->last_connect_ms = h.last_connect_ms;
    out->last_reused = h.last_reused;
    out->warmups = h.warmups;
    out->any_done = h.any_done;
    out->last_ok = h.last_ok;
    out->last_done_status = h.last_done_status;
    out->last_done_ago_ms = h.any_done
        ? (uint32_t)((esp_timer_get_time() - h.last_done_us) / 1000) : 0;
}
