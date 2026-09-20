#include "stackee_ota.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "stackee_conhid.h"
#include "stackee_console.h"
#include "stackee_otacore.h"
#include "stackee_usb.h"

static const char *TAG = "ota";

// 環状バッファ (PSRAM)。★ 内蔵 RAM を増やさない、が最優先の制約。
// 最初の ota.begin で確保して、そのあとは**手放さない**。受信中に free すると
// 入力タスクが書いている最中の領域を返すことになるため。64 KB は credit の 2 倍。
#define OTA_RING_BYTES   (64 * 1024)
// 内蔵 RAM の作業バッファ。ESP-IDF の esp_flash_write は元バッファが外部 RAM
// だと 32 バイトずつしか書けず、その都度キャッシュを落とすので桁違いに遅い。
// 4 KB にしておくと 1 回の書き込みがフラッシュ 1 セクタの消去にちょうど揃う。
#define OTA_BOUNCE_BYTES 4096
// 1 周あたりに書く上限。4 KB の書き込み 2 回ぶん。
#define OTA_PUMP_BUDGET  (2 * OTA_BOUNCE_BYTES)

static uint8_t *s_ring;
static uint8_t *s_bounce;

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_target;
static bool s_sha_live;

// ota.commit / app.boot_factory の遅延再起動 (応答を返し切ってから落ちる)。
static int64_t s_restart_at_us;

// ★ 内蔵 RAM の常駐を増やさないための置き場。**PSRAM に 1 つだけ**取り、
//   そのあとは手放さない。ここに入っているのはどれも
//   「フラッシュ操作の**最中**には触らないもの」だけ (キャッシュが止まって
//   いる間に PSRAM を読むと落ちる)。
//
//   sha  … SHA-256 の途中経過。ESP-IDF v6 の mbedtls は 4.x 系
//          (TF-PSA-Crypto) で `mbedtls/sha256.h` は private に移っている。
//          公開されているのは psa_hash_* のほう (stackee_cryptocheck.c と
//          同じ口)。ハードウェアの SHA も IDF の PSA ドライバ経由で効く。
//          触るのは pump / end の中で、esp_ota_write から戻ったあとだけ。
//   running_sha … 走っている像の SHA-256。起動中は変わらないので 1 度だけ
//          測る (esp_partition_get_sha256 は 1.4 MB を読んで流すので、
//          app.info のたびにやると重い)。
typedef struct {
    psa_hash_operation_t sha;
    uint8_t              running_sha[32];
    bool                 running_ok;
    bool                 running_done;
} stackee_ota_slow_t;

static stackee_ota_slow_t *s_slow;

// PSRAM の置き場を用意する。取れなければ NULL (呼び手が判断する)。
static stackee_ota_slow_t *slow(void) {
    if (s_slow == NULL) {
        s_slow = heap_caps_calloc(1, sizeof(*s_slow), MALLOC_CAP_SPIRAM);
    }
    return s_slow;
}

// ---------------------------------------------------------------------------
// stackee_otacore に差し込む実体
// ---------------------------------------------------------------------------
static int flash_begin(void *ctx, uint32_t size) {
    (void)ctx;
    (void)size;
    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL) {
        ESP_LOGE(TAG, "次の更新区画が無い");
        return ESP_ERR_NOT_FOUND;
    }
    // ★ OTA_WITH_SEQUENTIAL_WRITES。OTA_SIZE_UNKNOWN だと 2 MB を最初に
    //   全部消すので、その間ずっとフラッシュが止まる (= USB も止まる)。
    esp_err_t err = esp_ota_begin(s_target, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        s_handle = 0;
        return (int)err;
    }
    ESP_LOGI(TAG, "%s (0x%06lx, %lu B) へ %lu B 書く",
             s_target->label, (unsigned long)s_target->address,
             (unsigned long)s_target->size, (unsigned long)size);
    return 0;
}

static int flash_write(void *ctx, const void *data, uint32_t len) {
    (void)ctx;
    if (s_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
    }
    return (int)err;
}

static int flash_end(void *ctx) {
    (void)ctx;
    if (s_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
    }
    return (int)err;
}

static void flash_cancel(void *ctx) {
    (void)ctx;
    if (s_handle != 0) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
}

static const stackee_ota_flash_ops_t FLASH_OPS = {
    .begin = flash_begin, .write = flash_write,
    .end = flash_end, .cancel = flash_cancel,
};

static void hash_init(void *ctx) {
    (void)ctx;
    stackee_ota_slow_t *sl = slow();
    if (sl == NULL) {
        return;
    }
    if (s_sha_live) {
        psa_hash_abort(&sl->sha);
        s_sha_live = false;
    }
    psa_crypto_init();      // 何度呼んでもよい
    sl->sha = psa_hash_operation_init();
    psa_status_t st = psa_hash_setup(&sl->sha, PSA_ALG_SHA_256);
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_hash_setup: %d", (int)st);
        return;
    }
    s_sha_live = true;
}

static void hash_update(void *ctx, const void *data, uint32_t len) {
    (void)ctx;
    if (s_sha_live) {
        psa_hash_update(&s_slow->sha, (const uint8_t *)data, len);
    }
}

static void hash_finish(void *ctx, uint8_t out[32]) {
    (void)ctx;
    memset(out, 0, 32);
    if (s_sha_live) {
        size_t got = 0;
        psa_hash_finish(&s_slow->sha, out, 32, &got);
        s_sha_live = false;
    }
}

static const stackee_ota_hash_ops_t HASH_OPS = {
    .init = hash_init, .update = hash_update, .finish = hash_finish,
};

// ---------------------------------------------------------------------------
// 小道具
// ---------------------------------------------------------------------------
static size_t append(char *buf, size_t cap, size_t at, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static size_t append(char *buf, size_t cap, size_t at, const char *fmt, ...) {
    if (at >= cap) {
        return at;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + at, cap - at, fmt, args);
    va_end(args);
    return (n < 0) ? at : at + (size_t)n;
}

static size_t append_hex32(char *buf, size_t cap, size_t at, const uint8_t *sha) {
    for (int i = 0; i < 32; i++) {
        at = append(buf, cap, at, "%02x", sha[i]);
    }
    return at;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

// 64 文字の 16 進を 32 バイトへ。形が違えば false。
static bool parse_sha256(const char *text, uint8_t out[32]) {
    if (text == NULL) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        int hi = hexval(text[i * 2]);
        int lo = hexval(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return text[64] == '\0';
}

// 区画 1 つぶんの情報。sha256 は像が無い / 壊れていれば null。
static size_t append_partition(char *buf, size_t cap, size_t at, const char *key,
                               const esp_partition_t *part, bool cache_sha) {
    at = append(buf, cap, at, "\"%s\":", key);
    if (part == NULL) {
        return append(buf, cap, at, "null");
    }
    at = append(buf, cap, at,
                "{\"label\":\"%s\",\"addr\":%lu,\"capacity\":%lu",
                part->label, (unsigned long)part->address,
                (unsigned long)part->size);

    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        at = append(buf, cap, at, ",\"version\":\"%.32s\",\"project\":\"%.32s\","
                                  "\"idf\":\"%.32s\"",
                    desc.version, desc.project_name, desc.idf_ver);
    } else {
        at = append(buf, cap, at, ",\"version\":null,\"project\":null,\"idf\":null");
    }

    uint8_t sha[32];
    bool ok;
    stackee_ota_slow_t *sl = cache_sha ? slow() : NULL;
    if (sl != NULL && sl->running_done) {
        ok = sl->running_ok;
        memcpy(sha, sl->running_sha, 32);
    } else {
        // ★ 像の末尾に付いている SHA-256 を返す (hash_appended = 1)。
        //   ホスト側の `esptool image_info` と同じ値になる = ディスク上の
        //   バイト列とフラッシュ上のバイト列を端から端まで突き合わせられる。
        ok = (esp_partition_get_sha256(part, sha) == ESP_OK);
        if (sl != NULL) {
            memcpy(sl->running_sha, sha, 32);
            sl->running_ok = ok;
            sl->running_done = true;
        }
    }
    if (ok) {
        at = append(buf, cap, at, ",\"sha256\":\"");
        at = append_hex32(buf, cap, at, sha);
        at = append(buf, cap, at, "\"");
    } else {
        at = append(buf, cap, at, ",\"sha256\":null");
    }
    return append(buf, cap, at, "}");
}

static const char *img_state_name(const esp_partition_t *part) {
    esp_ota_img_states_t state;
    if (part == NULL || esp_ota_get_state_partition(part, &state) != ESP_OK) {
        return "unknown";
    }
    switch (state) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    default:                         return "undefined";
    }
}

// ---------------------------------------------------------------------------
// バッファの用意と後始末
// ---------------------------------------------------------------------------
static bool buffers_ready(void) {
    if (slow() == NULL) {
        ESP_LOGE(TAG, "SHA-256 の置き場 (PSRAM) が取れない");
        return false;
    }
    if (s_ring == NULL) {
        // ★ PSRAM。内蔵 RAM は TLS のために空けておく (README §17-2)。
        s_ring = heap_caps_malloc(OTA_RING_BYTES, MALLOC_CAP_SPIRAM);
        if (s_ring == NULL) {
            ESP_LOGE(TAG, "環状バッファ (PSRAM %d B) が取れない", OTA_RING_BYTES);
            return false;
        }
    }
    if (s_bounce == NULL) {
        s_bounce = heap_caps_malloc(OTA_BOUNCE_BYTES,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_bounce == NULL) {
            ESP_LOGE(TAG, "作業バッファ (内蔵 %d B) が取れない", OTA_BOUNCE_BYTES);
            return false;
        }
    }
    return true;
}

// 受信が終わったら内蔵 RAM の作業バッファを返す。★ 環状バッファ (PSRAM) は
// 返さない (入力タスクが触る先を消せないため)。
static void release_bounce(void) {
    uint8_t *p = NULL;
    if (stackee_otacore_take_bounce(&p)) {
        heap_caps_free(p);
        if (p == s_bounce) {
            s_bounce = NULL;
        }
    }
}

// ---------------------------------------------------------------------------
// コンソール命令
// ---------------------------------------------------------------------------
static size_t reply_app_info(long id, char *buf, size_t cap) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    size_t at = append(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"proto\":1,", id);
    at = append_partition(buf, cap, at, "running", running, true);
    at = append(buf, cap, at, ",");
    at = append_partition(buf, cap, at, "boot", (boot == running) ? running : boot,
                          boot == running);
    at = append(buf, cap, at, ",");
    at = append_partition(buf, cap, at, "next", next, false);

    stackee_ota_status_t st;
    stackee_otacore_status(&st);
    at = append(buf, cap, at,
                ",\"ota_state\":\"%s\",\"state\":\"%s\",\"err\":\"%s\","
                "\"accepted\":%lu,\"written\":%lu,\"size\":%lu}",
                img_state_name(running),
                stackee_otacore_state_name(st.state),
                stackee_otacore_err_name(st.err),
                (unsigned long)st.accepted, (unsigned long)st.written,
                (unsigned long)st.size);
    return at;
}

static size_t reply_ota_status(long id, char *buf, size_t cap) {
    stackee_ota_status_t st;
    stackee_otacore_status(&st);
    size_t at = append(buf, cap, 0,
                       "{\"id\":%ld,\"ok\":1,\"state\":\"%s\",\"err\":\"%s\","
                       "\"size\":%lu,\"accepted\":%lu,\"written\":%lu,"
                       "\"pending\":%lu,\"ring\":%lu,\"rejected\":%lu,"
                       "\"overflow\":%lu,\"elapsed_ms\":%lu,\"credit\":%lu",
                       id, stackee_otacore_state_name(st.state),
                       stackee_otacore_err_name(st.err),
                       (unsigned long)st.size, (unsigned long)st.accepted,
                       (unsigned long)st.written, (unsigned long)st.pending,
                       (unsigned long)st.ring_size, (unsigned long)st.rejected,
                       (unsigned long)st.overflow, (unsigned long)st.elapsed_ms,
                       (unsigned long)STACKEE_OTA_CREDIT);
    if (st.have_sha) {
        at = append(buf, cap, at, ",\"sha256\":\"");
        at = append_hex32(buf, cap, at, st.got_sha);
        at = append(buf, cap, at, "\"");
    }
    return append(buf, cap, at, "}");
}

static size_t reply_ota_begin(long id, const char *line, char *buf, size_t cap) {
    long size = stackee_console_int(line, "size", 0);
    char sha_text[80];
    uint8_t want[32];

    if (!stackee_console_str(line, "sha256", sha_text, sizeof(sha_text)) ||
        !parse_sha256(sha_text, want)) {
        return append(buf, cap, 0,
                      "{\"id\":%ld,\"error\":\"badsha\","
                      "\"note\":\"sha256 は 64 文字の 16 進で渡すこと\"}", id);
    }
    if (size <= 0) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"badsize\"}", id);
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"nopartition\"}", id);
    }
    if ((size_t)size > next->size) {
        return append(buf, cap, 0,
                      "{\"id\":%ld,\"error\":\"toolarge\",\"capacity\":%lu}",
                      id, (unsigned long)next->size);
    }
    if (stackee_otacore_state() == STACKEE_OTA_RECEIVING) {
        // ★ 二重起動はここで止まる。ハンドルは 1 つしかない。
        return append(buf, cap, 0,
                      "{\"id\":%ld,\"error\":\"busy\","
                      "\"note\":\"既に別の転送が進行中。ota.abort で止めること\"}", id);
    }
    release_bounce();
    if (!buffers_ready()) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"nomem\"}", id);
    }
    int rc = stackee_otacore_begin((uint32_t)size, want, s_ring, OTA_RING_BYTES,
                                   s_bounce, OTA_BOUNCE_BYTES,
                                   (uint32_t)(esp_timer_get_time() / 1000));
    if (rc != STACKEE_OTA_ERR_NONE) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"%s\"}", id,
                      stackee_otacore_err_name(rc));
    }
    return append(buf, cap, 0,
                  "{\"id\":%ld,\"ok\":1,\"target\":\"%s\",\"size\":%ld,"
                  "\"chunk\":%d,\"credit\":%lu,\"ring\":%d,\"cmd\":%d,"
                  "\"idle_ms\":%lu}",
                  id, next->label, size, STACKEE_OTA_PAYLOAD,
                  (unsigned long)STACKEE_OTA_CREDIT, OTA_RING_BYTES,
                  STACKEE_OTA_CMD_DATA, (unsigned long)STACKEE_OTA_IDLE_MS);
}

static size_t reply_ota_end(long id, char *buf, size_t cap) {
    int rc = stackee_otacore_end();
    release_bounce();
    stackee_ota_status_t st;
    stackee_otacore_status(&st);

    size_t at = append(buf, cap, 0, "{\"id\":%ld,\"ok\":%d,\"state\":\"%s\","
                                    "\"err\":\"%s\",\"written\":%lu,\"size\":%lu",
                       id, (rc == STACKEE_OTA_ERR_NONE) ? 1 : 0,
                       stackee_otacore_state_name(st.state),
                       stackee_otacore_err_name(st.err),
                       (unsigned long)st.written, (unsigned long)st.size);
    at = append(buf, cap, at, ",\"sha256\":");
    if (st.have_sha) {
        at = append(buf, cap, at, "\"");
        at = append_hex32(buf, cap, at, st.got_sha);
        at = append(buf, cap, at, "\"");
    } else {
        at = append(buf, cap, at, "null");
    }
    at = append(buf, cap, at, ",\"want_sha256\":\"");
    at = append_hex32(buf, cap, at, st.want_sha);
    at = append(buf, cap, at, "\"");

    // ★★ sha256 は 2 種類ある。混ぜると一生合わない。
    //   "sha256" (上)        … 受け取ったバイト列**全体**の SHA-256。
    //                          ホストの `shasum -a 256 stackee.bin` と同じ。
    //                          転送で化けていないかを見る値。
    //   "partition_sha256"   … 区画の像に**埋め込まれている** SHA-256
    //                          (末尾 32 バイト = hash_appended)。
    //                          `esptool image_info` と同じで、app.info の
    //                          running/boot/next が返すのもこちら。
    //                          **どの区画に何が入っているかを名指しする値。**
    //   esp_partition_get_sha256 は返す前に中身を検証するので、ここが
    //   取れたということは「書いた像が ESP-IDF の検査を通った」でもある。
    const esp_partition_t *target = s_target;
    uint8_t part_sha[32];
    if (target != NULL) {
        at = append(buf, cap, at, ",\"partition\":\"%s\"", target->label);
    } else {
        at = append(buf, cap, at, ",\"partition\":null");
    }
    if (rc == STACKEE_OTA_ERR_NONE && target != NULL &&
        esp_partition_get_sha256(target, part_sha) == ESP_OK) {
        at = append(buf, cap, at, ",\"partition_sha256\":\"");
        at = append_hex32(buf, cap, at, part_sha);
        at = append(buf, cap, at, "\",\"valid\":1");
    } else {
        at = append(buf, cap, at, ",\"partition_sha256\":null,\"valid\":0");
    }
    // ★ ここでは切り替えない。「書けた」と「そっちで起動する」は別の決断。
    return append(buf, cap, at, ",\"committed\":0}");
}

static size_t reply_ota_commit(long id, char *buf, size_t cap) {
    if (stackee_otacore_state() != STACKEE_OTA_DONE || s_target == NULL) {
        return append(buf, cap, 0,
                      "{\"id\":%ld,\"error\":\"notready\","
                      "\"note\":\"ota.end が通っていない\"}", id);
    }
    esp_err_t err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"setboot\",\"rc\":\"%s\"}",
                      id, esp_err_to_name(err));
    }
    // 応答を返し切ってから落ちる。すぐ落とすと呼んだ側に返事が届かない。
    s_restart_at_us = esp_timer_get_time() + 500000;
    return append(buf, cap, 0,
                  "{\"id\":%ld,\"ok\":1,\"boot\":\"%s\",\"in_ms\":500}",
                  id, s_target->label);
}

static size_t reply_ota_abort(long id, char *buf, size_t cap) {
    stackee_otacore_abort();
    release_bounce();
    return append(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"state\":\"idle\"}", id);
}

// otadata を消して factory (uf2 = CircuitPython の UF2 ブートローダ) を選ぶ。
// ★ **戻れる道**。ROM を通らずに 1 命令で UF2 まで戻せる。
//   ESP-IDF の esp_ota_set_boot_partition は factory を指定されたとき
//   「ota info 区画を初期化するだけ」なので、otadata が消えて既定の
//   factory が選ばれる。README §OTA に書いてある。
static size_t reply_boot_factory(long id, char *buf, size_t cap) {
    const esp_partition_t *factory = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory == NULL) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"nofactory\"}", id);
    }
    if (stackee_otacore_state() == STACKEE_OTA_RECEIVING) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    esp_err_t err = esp_ota_set_boot_partition(factory);
    if (err != ESP_OK) {
        return append(buf, cap, 0, "{\"id\":%ld,\"error\":\"setboot\",\"rc\":\"%s\"}",
                      id, esp_err_to_name(err));
    }
    s_restart_at_us = esp_timer_get_time() + 500000;
    return append(buf, cap, 0,
                  "{\"id\":%ld,\"ok\":1,\"boot\":\"%s\",\"in_ms\":500}",
                  id, factory->label);
}

static size_t ota_console(const char *cmd, const char *line, long id,
                          char *buf, size_t cap) {
    if (strcmp(cmd, "app.info") == 0)         { return reply_app_info(id, buf, cap); }
    if (strcmp(cmd, "ota.begin") == 0)        { return reply_ota_begin(id, line, buf, cap); }
    if (strcmp(cmd, "ota.status") == 0)       { return reply_ota_status(id, buf, cap); }
    if (strcmp(cmd, "ota.end") == 0)          { return reply_ota_end(id, buf, cap); }
    if (strcmp(cmd, "ota.commit") == 0)       { return reply_ota_commit(id, buf, cap); }
    if (strcmp(cmd, "ota.abort") == 0)        { return reply_ota_abort(id, buf, cap); }
    if (strcmp(cmd, "app.boot_factory") == 0) { return reply_boot_factory(id, buf, cap); }
    return 0;       // 知らないコマンド。次の拡張へ回す
}

// ---------------------------------------------------------------------------
// Raw HID の 0xC3 (入力タスクから)
// ---------------------------------------------------------------------------
static bool ota_raw_hook(const uint8_t *report, uint32_t len,
                         uint8_t *reply, bool *want_reply) {
    return stackee_otacore_feed(report, len, reply, want_reply);
}

// ---------------------------------------------------------------------------
void stackee_ota_init(void) {
    stackee_otacore_attach(&FLASH_OPS, NULL, &HASH_OPS, NULL);
    stackee_conhid_set_raw_hook(ota_raw_hook);
    stackee_console_register(ota_console);
}

bool stackee_ota_busy(void) {
    return stackee_otacore_state() == STACKEE_OTA_RECEIVING;
}

void stackee_ota_poll(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    stackee_otacore_tick(now_ms);
    if (stackee_otacore_state() == STACKEE_OTA_RECEIVING) {
        // ★ ここが数十 ms 止まる (4 KB 書くと 1 セクタ消える)。
        stackee_otacore_pump(OTA_PUMP_BUDGET);
    } else if (s_bounce != NULL) {
        release_bounce();        // 無通信で自動 abort したときなど
    }
    if (s_restart_at_us != 0 && esp_timer_get_time() >= s_restart_at_us) {
        s_restart_at_us = 0;
        ESP_LOGW(TAG, "起動区画を切り替えたので再起動する");
        vTaskDelay(pdMS_TO_TICKS(20));
        // ★ 素の esp_restart() を呼ばない。カメラを開いたまま落とすと
        //   起動後にフラッシュが読めなくなる (README §18)。
        //   stackee_usb_request_restart() が ALDO3 を切ってから落とす。
        stackee_usb_request_restart();
    }
}
