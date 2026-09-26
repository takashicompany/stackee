#include "stackee_http.h"
#include "stackee_cryptocheck.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "http";

#define URL_MAX     256
#define TOKEN_MAX   160
#define TIMEOUT_MS  30000
// ロングポーリング ("/jobs/<id>?wait=<秒>") のときだけ、待つ秒数 + これだけ
// 待つ。中継 (dev/server/proxy.py) は wait 秒まで応答を握るので、30 秒では
// 自分から切ってしまう。
#define WAIT_MARGIN_MS 15000

static struct {
    char     base[URL_MAX];
    char     token[TOKEN_MAX];
    bool     secure;

    TaskHandle_t task;
    SemaphoreHandle_t go;

    // ワーカーに渡すもの (start の前にだけ書く)。
    char        url[URL_MAX + STACKEE_HTTP_PATH_ROOM];
    const char *method;
    const void *body;
    size_t      body_len;
    size_t      limit;
    const char *content_type;   // 呼び手の定数文字列を指すだけ (写さない)

    // ワーカーが埋めるもの。
    uint8_t *buf;
    size_t   buf_cap;
    _Atomic size_t received;
    _Atomic int state;
    _Atomic int status;
    _Atomic int err;
    _Atomic bool cancel;
    _Atomic uint32_t elapsed_ms;

    uint32_t requests, failures, last_ms;
    size_t   last_bytes;
} h;

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

// この 1 本に許す時間 [ms]。URL に "?wait=<秒>" (受け箱では "&wait=<秒>")
// が付いていれば、その秒数を足したぶんだけ待つ (ロングポーリング)。
// ほかの要求は従来どおり 30 秒。
// ★ ここで見るのは会話の状態機械が組み立てた自分の URL だけ。
static int timeout_for(const char *url) {
    const char *at = strstr(url, "?wait=");
    if (at == NULL) {
        at = strstr(url, "&wait=");     // GET /inbox?after=<seq>&wait=25&job=
    }
    if (at == NULL) {
        return TIMEOUT_MS;
    }
    int seconds = 0;
    for (const char *p = at + 6; *p >= '0' && *p <= '9'; p++) {
        seconds = seconds * 10 + (*p - '0');
        if (seconds > 300) {
            return TIMEOUT_MS;
        }
    }
    return seconds * 1000 + WAIT_MARGIN_MS;
}

static void release_buffer(void) {
    if (h.buf != NULL) {
        free(h.buf);
        h.buf = NULL;
        h.buf_cap = 0;
    }
    atomic_store(&h.received, 0);
    atomic_store(&h.state, STACKEE_HTTP_IDLE);
}

// ---------------------------------------------------------------------------
static esp_err_t on_event(esp_http_client_event_t *evt) {
    if (atomic_load(&h.cancel)) {
        return ESP_FAIL;            // perform を途中で止める
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
        return ESP_OK;
    }
    size_t at = atomic_load(&h.received);
    if (h.buf == NULL || at + (size_t)evt->data_len > h.buf_cap) {
        atomic_store(&h.err, -2);   // 応答が限度を超えた
        return ESP_FAIL;
    }
    memcpy(h.buf + at, evt->data, (size_t)evt->data_len);
    atomic_store(&h.received, at + (size_t)evt->data_len);
    return ESP_OK;
}

static void worker(void *unused) {
    (void)unused;
    for (;;) {
        xSemaphoreTake(h.go, portMAX_DELAY);
        int64_t t0 = esp_timer_get_time();
        // ★ 握手の前に内蔵 RAM の残りを残す。mbedtls_ssl_setup が
        //   -0x008D (ALLOC_FAILED) で落ちるのはここが足りないとき。
        ESP_LOGI(TAG, "%s 開始 (内蔵RAM 空き %u B / 最大の塊 %u B / PSRAM %u B)",
                 h.method,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        esp_http_client_config_t config = {
            .url = h.url,
            .method = (strcmp(h.method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
            .timeout_ms = timeout_for(h.url),
            .event_handler = on_event,
            .disable_auto_redirect = true,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .keep_alive_enable = false,
        };
        if (h.secure) {
            config.crt_bundle_attach = esp_crt_bundle_attach;
        }
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            atomic_store(&h.err, -1);
            atomic_store(&h.state, STACKEE_HTTP_ERROR);
            continue;
        }
        if (h.token[0] != '\0') {
            char header[TOKEN_MAX + 16];
            snprintf(header, sizeof(header), "Bearer %s", h.token);
            esp_http_client_set_header(client, "Authorization", header);
        }
        if (config.method == HTTP_METHOD_POST) {
            esp_http_client_set_header(client, "Content-Type", h.content_type);
            esp_http_client_set_post_field(client, (const char *)h.body, (int)h.body_len);
        }
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        atomic_store(&h.elapsed_ms, ms);
        atomic_store(&h.status, status);
        if (err != ESP_OK || atomic_load(&h.cancel)) {
            if (atomic_load(&h.err) == 0) {
                atomic_store(&h.err, (int)err);
            }
            h.failures++;
            ESP_LOGE(TAG, "%s %s 失敗 (%s, status=%d, %lums)",
                     h.method, h.url + (h.secure ? 8 : 7), esp_err_to_name(err),
                     status, (unsigned long)ms);
            ESP_LOGE(TAG, "  失敗時の内蔵RAM 空き %u B / 最大の塊 %u B",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            // ★ 接続段階の失敗 (TLS 込み) なら、その場で暗号の自己診断を回す
            //   (README §20)。壊れていれば印が立ち、main が会話の合間に再起動する。
            if (err == ESP_ERR_HTTP_CONNECT && h.secure) {
                stackee_cryptocheck_result_t r;
                bool ok = stackee_cryptocheck_run(&r);
                ESP_LOGW(TAG, "  暗号の自己診断: %s (sha int=%d psram=%d / x509 sig_ok=%d)",
                         ok ? "OK (壊れていない = 原因は暗号の外)" : "NG (壊れている → 再起動で戻す)",
                         r.sha_internal_4k, r.sha_psram_4k, r.x509_sig_ok);
            }
            atomic_store(&h.state, STACKEE_HTTP_ERROR);
            continue;
        }
        size_t got = atomic_load(&h.received);
        // ★ `<=`。入れ物は limit + 1 バイト取ってあるので、ちょうど limit
        //   バイト受けたときも終端を置ける。`<` にすると、8192 バイト
        //   ぴったりの応答だけ終端が無いまま文字列として読まれる。
        if (h.buf != NULL && got <= h.buf_cap) {
            h.buf[got] = '\0';      // JSON をそのまま文字列として読めるように
        }
        h.last_ms = ms;
        h.last_bytes = got;
        // ★ ログは「終わった」と知らせる**前**に出す。あとにすると、
        //   次の要求が h.method / h.url を書き換えたものを出しかねない。
        //   path は出すが token も本文も出さない。
        // ★ 終わったあとの内蔵 RAM も残す。HTTPS の握手はハードウェア AES が
        //   DMA 可能な内蔵 RAM を要るので、1 往復ごとにどれだけ戻ってきて
        //   いないかが分かると、痩せていく原因を後から追える (README §24)。
        ESP_LOGI(TAG, "[talk-http-timing] {\"method\":\"%s\",\"status\":%d,"
                      "\"bytes\":%u,\"worker_ms\":%lu,"
                      "\"internal_free\":%u,\"internal_largest\":%u}",
                 h.method, status, (unsigned)got, (unsigned long)ms,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        atomic_store(&h.state, STACKEE_HTTP_DONE);
    }
}

esp_err_t stackee_http_start(void) {
    if (h.task != NULL) {
        return ESP_OK;
    }
    h.go = xSemaphoreCreateBinary();
    if (h.go == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // CPU0 / 低優先度。入力 (CPU1 / 最高) とは別の CPU。
    // ★ スタックは 10 KB。TLS のハンドシェイクが 6 KB では足りない
    //   (ESP-IDF の HTTPS の例も 8 KB 以上を使っている)。
    // ★★ 優先度は 1 (main / lcd と同じ、touch・ui の 3 より下)。2026-09-26 まで
    //   4 で、「低優先度」の意図と食い違っていた。keep-alive を切っているので
    //   要求のたびに TLS の握手が走り、それが CPU0 を約 4.5〜5 秒握り続ける
    //   (会話 1 回で POST・poll・音声 GET の 3 回)。4 だとその間 touch (3)・
    //   ui (3)・console (main, 1) がほぼ回れず (実測: 毎秒 200 周 → 6〜50)、
    //   タッチと顔が固まっていた。握手は計算だけなので、
    //   下の優先度で回しても周期の短い仕事の合間に進むだけで失うものは小さい。
    if (xTaskCreatePinnedToCore(worker, "stackee_http", 10240, NULL, 1, &h.task, 0)
            != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool stackee_http_request(const char *method, const char *path,
                          const void *body, size_t body_len, size_t limit,
                          const char *content_type) {
    if (h.task == NULL || h.base[0] == '\0' || path == NULL || path[0] != '/') {
        return false;
    }
    int state = atomic_load(&h.state);
    if (state == STACKEE_HTTP_RUNNING) {
        return false;               // 同時に 1 本だけ
    }
    if (state != STACKEE_HTTP_IDLE) {
        // 前の往復 (成功・失敗・キャンセル) の後始末をここで済ませる。
        release_buffer();
    }
    for (const char *p = path; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == ' ') {
            return false;
        }
    }
    if (limit == 0) {
        limit = 8192;
    }
    h.buf = heap_caps_malloc(limit + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (h.buf == NULL) {
        h.buf = heap_caps_malloc(limit + 1, MALLOC_CAP_8BIT);
    }
    if (h.buf == NULL) {
        // ★ 返答音声の受け皿は 120 秒ぶん = 3.84 MB ある。ここで落ちたときに
        //   「合計が足りない」のか「連続した塊が無い」のかが分かるように、
        //   両方を残す (会話は呼び手が失敗として畳む)。
        ESP_LOGE(TAG, "受信用の %u B を確保できない "
                      "(PSRAM 空き %u B / 最大の塊 %u B / 内蔵 %u B)",
                 (unsigned)(limit + 1),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return false;
    }
    h.buf_cap = limit;
    h.buf[0] = '\0';
    snprintf(h.url, sizeof(h.url), "%s%s", h.base, path);
    h.method = (strcmp(method, "POST") == 0) ? "POST" : "GET";
    h.body = body;
    h.body_len = body_len;
    h.content_type = (content_type != NULL) ? content_type : "audio/wav";
    h.limit = limit;
    atomic_store(&h.received, 0);
    atomic_store(&h.status, 0);
    atomic_store(&h.err, 0);
    atomic_store(&h.cancel, false);
    atomic_store(&h.elapsed_ms, 0);
    atomic_store(&h.state, STACKEE_HTTP_RUNNING);
    h.requests++;
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

void stackee_http_close(void) {
    int state = atomic_load(&h.state);
    if (state == STACKEE_HTTP_RUNNING) {
        // ★ 待たない。ワーカーは次のイベントで ESP_FAIL を返して畳む。
        //   受信用の領域は次の stackee_http_request() が片付ける。
        atomic_store(&h.cancel, true);
        return;
    }
    release_buffer();
}

void stackee_http_stats(stackee_http_stats_t *out) {
    out->state = atomic_load(&h.state);
    out->status = atomic_load(&h.status);
    out->err = atomic_load(&h.err);
    out->requests = h.requests;
    out->failures = h.failures;
    out->last_ms = h.last_ms;
    out->last_bytes = h.last_bytes;
}
