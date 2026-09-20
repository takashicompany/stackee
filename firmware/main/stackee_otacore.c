#include "stackee_otacore.h"

#include <string.h>

// ---------------------------------------------------------------------------
// 状態 (このファイルに 1 組だけ。ハンドルは 1 つ = 二重起動は構造的に起きない)
// ---------------------------------------------------------------------------
// ★ .bss に置くのは**この構造体だけ**で、環状バッファも作業バッファも持たない
//   (呼び手が PSRAM / ヒープから渡す)。内蔵 RAM の常駐を増やさないため。
static const stackee_ota_flash_ops_t *s_flash;
static void                          *s_flash_ctx;
static const stackee_ota_hash_ops_t  *s_hash;
static void                          *s_hash_ctx;

static uint8_t  *s_ring;
static uint32_t  s_ring_size;
static uint8_t  *s_bounce;
static uint32_t  s_bounce_len;

static volatile uint8_t  s_state;
static volatile uint8_t  s_err;
static volatile uint32_t s_size;
static volatile uint32_t s_accepted;    // feed だけが書く
static volatile uint32_t s_written;     // pump だけが書く
static volatile uint32_t s_rejected;
static volatile uint32_t s_overflow;
static volatile uint32_t s_last_ms;
static volatile uint32_t s_begin_ms;
static volatile uint32_t s_now_ms;
static volatile uint32_t s_acked_at;    // 最後に応答を返した accepted の値

static uint8_t s_want_sha[32];
static uint8_t s_got_sha[32];
static bool    s_have_sha;

static const char *const ERR_NAME[STACKEE_OTA_ERR_COUNT] = {
    "none", "busy", "arg", "nomem", "flash_begin", "offset", "full",
    "flash_write", "flash_end", "sha", "size", "idle", "state", "magic",
};

static const char *const STATE_NAME[] = { "idle", "receiving", "done", "failed" };

const char *stackee_otacore_err_name(int err) {
    if (err < 0 || err >= STACKEE_OTA_ERR_COUNT) {
        return "?";
    }
    return ERR_NAME[err];
}

const char *stackee_otacore_state_name(int state) {
    if (state < 0 || state > STACKEE_OTA_FAILED) {
        return "?";
    }
    return STATE_NAME[state];
}

void stackee_otacore_attach(const stackee_ota_flash_ops_t *flash, void *flash_ctx,
                            const stackee_ota_hash_ops_t *hash, void *hash_ctx) {
    s_flash = flash;
    s_flash_ctx = flash_ctx;
    s_hash = hash;
    s_hash_ctx = hash_ctx;
}

// ---------------------------------------------------------------------------
static uint32_t ring_used(void) {
    return s_accepted - s_written;      // どちらも単調増加 (像は 2 MB まで)
}

static uint32_t ring_free(void) {
    return (s_ring_size > ring_used()) ? (s_ring_size - ring_used()) : 0;
}

static void put_u32(uint8_t *at, uint32_t v) {
    at[0] = (uint8_t)(v & 0xFF);
    at[1] = (uint8_t)((v >> 8) & 0xFF);
    at[2] = (uint8_t)((v >> 16) & 0xFF);
    at[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void fill_reply(uint8_t *reply, bool rejected) {
    memset(reply, 0, STACKEE_OTA_REPORT);
    reply[0] = STACKEE_OTA_CMD_DATA;
    reply[1] = s_state;
    reply[2] = s_err;
    put_u32(&reply[3], s_accepted);
    put_u32(&reply[7], s_written);
    put_u32(&reply[11], s_size);
    reply[15] = rejected ? STACKEE_OTA_REPLY_REJECTED : 0;
    put_u32(&reply[16], ring_free());
}

static void fail(uint8_t err) {
    if (s_state == STACKEE_OTA_RECEIVING && s_flash && s_flash->cancel) {
        s_flash->cancel(s_flash_ctx);
    }
    s_state = STACKEE_OTA_FAILED;
    s_err = err;
}

// ---------------------------------------------------------------------------
int stackee_otacore_begin(uint32_t size, const uint8_t sha256[32],
                          uint8_t *ring, uint32_t ring_size,
                          uint8_t *bounce, uint32_t bounce_len,
                          uint32_t now_ms) {
    if (s_state == STACKEE_OTA_RECEIVING) {
        return STACKEE_OTA_ERR_BUSY;    // ★ 既存の受信を壊さない (二重起動の拒否)
    }
    if (size == 0 || sha256 == NULL) {
        return STACKEE_OTA_ERR_ARG;
    }
    if (size > 0x00FFFFFFu) {
        return STACKEE_OTA_ERR_ARG;     // 24 bit の位置に収まらない
    }
    if (ring == NULL || ring_size < STACKEE_OTA_RING_MIN ||
        bounce == NULL || bounce_len == 0) {
        return STACKEE_OTA_ERR_NOMEM;
    }
    if (s_flash == NULL || s_hash == NULL) {
        return STACKEE_OTA_ERR_ARG;
    }

    s_ring = ring;
    s_ring_size = ring_size;
    s_bounce = bounce;
    s_bounce_len = bounce_len;
    s_size = size;
    memcpy(s_want_sha, sha256, 32);
    memset(s_got_sha, 0, 32);
    s_have_sha = false;
    s_accepted = 0;
    s_written = 0;
    s_rejected = 0;
    s_overflow = 0;
    s_acked_at = 0;
    s_err = STACKEE_OTA_ERR_NONE;
    s_begin_ms = now_ms;
    s_last_ms = now_ms;
    s_now_ms = now_ms;

    int rc = s_flash->begin(s_flash_ctx, size);
    if (rc != 0) {
        s_state = STACKEE_OTA_FAILED;
        s_err = STACKEE_OTA_ERR_FLASH_BEGIN;
        return STACKEE_OTA_ERR_FLASH_BEGIN;
    }
    s_hash->init(s_hash_ctx);
    // ★ 最後に立てる。ここより前に立てると、入力タスクが用意の途中の
    //   環状バッファへ書き込む隙ができる。
    s_state = STACKEE_OTA_RECEIVING;
    return STACKEE_OTA_ERR_NONE;
}

// ---------------------------------------------------------------------------
// 入力タスクから。**環状バッファに積むだけ**。
// ---------------------------------------------------------------------------
bool stackee_otacore_feed(const uint8_t *report, uint32_t report_len,
                          uint8_t *reply, bool *want_reply) {
    if (report == NULL || report_len < 1 || report[0] != STACKEE_OTA_CMD_DATA) {
        return false;
    }
    if (want_reply != NULL) {
        *want_reply = false;
    }
    if (report_len < STACKEE_OTA_HEADER) {
        if (reply && want_reply) { fill_reply(reply, true); *want_reply = true; }
        return true;
    }

    uint32_t len = report[1];
    uint32_t offset = (uint32_t)report[2] | ((uint32_t)report[3] << 8) |
                      ((uint32_t)report[4] << 16);
    if (len > STACKEE_OTA_PAYLOAD) {
        len = STACKEE_OTA_PAYLOAD;
    }
    if (STACKEE_OTA_HEADER + len > report_len) {
        len = report_len - STACKEE_OTA_HEADER;
    }

    // len == 0 は「状態だけ返せ」。受信中でなくても答える。
    if (len == 0) {
        if (reply && want_reply) { fill_reply(reply, false); *want_reply = true; }
        return true;
    }

    if (s_state != STACKEE_OTA_RECEIVING) {
        if (s_state != STACKEE_OTA_FAILED) {
            s_err = STACKEE_OTA_ERR_STATE;
        }
        if (reply && want_reply) { fill_reply(reply, true); *want_reply = true; }
        return true;
    }

    s_last_ms = s_now_ms;

    // ★ 位置が合わない枠は**黙って捨てる**。取りこぼしのあとにホストが
    //   送り直すぶんが必ずここへ来る (既に受け取ったぶんの再送も含む)。
    //   応答で accepted を返すので、ホストはそこから送り直せばよい。
    if (offset != s_accepted) {
        s_rejected++;
        if (reply && want_reply) { fill_reply(reply, true); *want_reply = true; }
        return true;
    }
    // 申告より長い像は受けない。
    if (s_accepted + len > s_size) {
        len = s_size - s_accepted;
        if (len == 0) {
            s_rejected++;
            if (reply && want_reply) { fill_reply(reply, true); *want_reply = true; }
            return true;
        }
    }
    // ★ ESP の像かどうかを**最初の 1 バイトで**見る。esp_ota_write も同じ
    //   検査をするが、そこまで行くと 1 セクタ消してしまっている。
    if (s_accepted == 0 && report[STACKEE_OTA_HEADER] != 0xE9) {
        fail(STACKEE_OTA_ERR_MAGIC);
        if (reply && want_reply) { fill_reply(reply, true); *want_reply = true; }
        return true;
    }
    if (len > ring_free()) {
        // credit を守っていればここには来ない。来たら数えて捨てる
        // (ホストは accepted を見て送り直す)。
        s_overflow++;
        if (reply && want_reply) { fill_reply(reply, true); *want_reply = true; }
        return true;
    }

    uint32_t head = s_accepted % s_ring_size;
    uint32_t first = s_ring_size - head;
    if (first > len) {
        first = len;
    }
    memcpy(&s_ring[head], &report[STACKEE_OTA_HEADER], first);
    if (first < len) {
        memcpy(&s_ring[0], &report[STACKEE_OTA_HEADER + first], len - first);
    }
    s_accepted += len;

    // 応答は 1 KB ごとに 1 枚だけ。Raw HID の送信キューを OTA で埋めない。
    bool due = (s_accepted >= s_size) ||
               (s_accepted - s_acked_at >= STACKEE_OTA_ACK_EVERY);
    if (due && reply && want_reply) {
        s_acked_at = s_accepted;
        fill_reply(reply, false);
        *want_reply = true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// メインループから。環状バッファ → 内蔵の作業バッファ → フラッシュ。
// ---------------------------------------------------------------------------
uint32_t stackee_otacore_pump(uint32_t budget) {
    if (s_state != STACKEE_OTA_RECEIVING) {
        return 0;
    }
    uint32_t done = 0;
    while (done < budget) {
        uint32_t used = ring_used();
        if (used == 0) {
            break;
        }
        uint32_t want = budget - done;
        if (want > s_bounce_len) {
            want = s_bounce_len;
        }
        if (want > used) {
            want = used;
        }
        // ★ 作業バッファいっぱい (4 KB) に揃えてから渡す。こうすると
        //   esp_ota_write の先行消去がセクタ境界ちょうどで起き、1 回の
        //   書き込みで消去するのが必ず 1 セクタになる。
        //   例外は像の最後の端数だけ (もう後ろが来ない)。
        bool last = (s_accepted >= s_size);
        if (want < s_bounce_len && !last) {
            break;      // まだ溜まっていない。次の周回で
        }

        uint32_t tail = s_written % s_ring_size;
        uint32_t first = s_ring_size - tail;
        if (first > want) {
            first = want;
        }
        memcpy(s_bounce, &s_ring[tail], first);
        if (first < want) {
            memcpy(&s_bounce[first], &s_ring[0], want - first);
        }
        int rc = s_flash->write(s_flash_ctx, s_bounce, want);
        if (rc != 0) {
            fail(STACKEE_OTA_ERR_FLASH_WRITE);
            return done;
        }
        s_hash->update(s_hash_ctx, s_bounce, want);
        s_written += want;
        done += want;
    }
    return done;
}

void stackee_otacore_tick(uint32_t now_ms) {
    s_now_ms = now_ms;
    if (s_state != STACKEE_OTA_RECEIVING) {
        return;
    }
    if ((uint32_t)(now_ms - s_last_ms) >= STACKEE_OTA_IDLE_MS) {
        fail(STACKEE_OTA_ERR_IDLE);
    }
}

int stackee_otacore_end(void) {
    if (s_state == STACKEE_OTA_FAILED) {
        return s_err;
    }
    if (s_state != STACKEE_OTA_RECEIVING) {
        return STACKEE_OTA_ERR_STATE;
    }
    // 残りを全部書く。★ 呼び手 (メインループ) はここで数百 ms 止まる。
    while (ring_used() > 0) {
        uint32_t n = stackee_otacore_pump(ring_used());
        if (s_state != STACKEE_OTA_RECEIVING) {
            return s_err;
        }
        if (n == 0) {
            break;      // 作業バッファより小さい端数が残ることはない (上で揃えた)
        }
    }
    if (s_written != s_size) {
        fail(STACKEE_OTA_ERR_SIZE);
        return STACKEE_OTA_ERR_SIZE;
    }
    s_hash->finish(s_hash_ctx, s_got_sha);
    s_have_sha = true;
    if (memcmp(s_got_sha, s_want_sha, 32) != 0) {
        fail(STACKEE_OTA_ERR_SHA);
        return STACKEE_OTA_ERR_SHA;
    }
    int rc = s_flash->end(s_flash_ctx);
    if (rc != 0) {
        // ★ esp_ota_end が失敗した時点でハンドルは無効になっている。
        //   cancel を重ねて呼ばない。
        s_state = STACKEE_OTA_FAILED;
        s_err = STACKEE_OTA_ERR_FLASH_END;
        return STACKEE_OTA_ERR_FLASH_END;
    }
    s_state = STACKEE_OTA_DONE;
    s_err = STACKEE_OTA_ERR_NONE;
    return STACKEE_OTA_ERR_NONE;
}

void stackee_otacore_abort(void) {
    if (s_state == STACKEE_OTA_RECEIVING && s_flash && s_flash->cancel) {
        s_flash->cancel(s_flash_ctx);
    }
    s_state = STACKEE_OTA_IDLE;
    s_err = STACKEE_OTA_ERR_NONE;
    s_accepted = 0;
    s_written = 0;
    s_size = 0;
    s_have_sha = false;
}

void stackee_otacore_reset(void) {
    s_state = STACKEE_OTA_IDLE;
    s_err = STACKEE_OTA_ERR_NONE;
    s_ring = NULL;
    s_ring_size = 0;
    s_bounce = NULL;
    s_bounce_len = 0;
    s_accepted = s_written = s_size = 0;
    s_rejected = s_overflow = 0;
    s_have_sha = false;
}

int stackee_otacore_state(void) {
    return s_state;
}

void stackee_otacore_status(stackee_ota_status_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->err = s_err;
    out->size = s_size;
    out->accepted = s_accepted;
    out->written = s_written;
    out->pending = ring_used();
    out->ring_size = s_ring_size;
    out->rejected = s_rejected;
    out->overflow = s_overflow;
    out->elapsed_ms = s_now_ms - s_begin_ms;
    memcpy(out->want_sha, s_want_sha, 32);
    memcpy(out->got_sha, s_got_sha, 32);
    out->have_sha = s_have_sha;
}

bool stackee_otacore_take_bounce(uint8_t **out) {
    if (out == NULL || s_state == STACKEE_OTA_RECEIVING || s_bounce == NULL) {
        return false;
    }
    *out = s_bounce;
    s_bounce = NULL;
    s_bounce_len = 0;
    return true;
}
