// アプリ内 OTA の中核 (main/stackee_otacore.c) を Mac 上で回す。
//
// 実機の esp_ota_* と mbedtls の代わりに **偽のフラッシュ** と
// hostbuild/stub/sha256_stub.c を差し込む。見ているのは 5 つ:
//
//   ・0xC3 レポートの分解 (位置つきの枠) が、ホスト側の実装と同じ形か
//   ・環状バッファが 1 バイトも落とさず・並べ替えずに通すか
//   ・credit (accepted / written / ring free) の数え方
//   ・SHA-256 を流しながら計算した値が hashlib と一致するか
//   ・断りどころ — 二重 begin / 位置違い / magic 違い / sha 違い /
//     溢れ / 無通信の自動 abort
//
// 台本 (標準入力、1 行 1 手):
//   attach <ring> <bounce>        買い直して繋ぎ直す (状態も消える)
//   begin <size> <sha256hex>      ota.begin
//   data <offset> <hex>           0xC3 を 1 枚食わせる (hex が payload)
//   poll                          len=0 の 0xC3 (状態だけ返せ)
//   raw <hex>                     任意の 32 バイトを食わせる
//   pump <budget>                 環状バッファ → 偽フラッシュ
//   tick <ms>                     時計を進める
//   end                           ota.end
//   abort                         ota.abort
//   status                        状態を出す
//   fail <begin|write|end> <0|1>  偽フラッシュを次から失敗させる
//   flash                         偽フラッシュが受けた長さと SHA-256
//
// 出す行 (タブ区切り):
//   reply   <hex32>   / noreply / notmine
//   begin   <rc> <name>
//   pump    <n>
//   end     <rc> <name>
//   status  <state> <err> <size> <accepted> <written> <pending> <rejected> <overflow>
//   flash   <len> <sha256hex> <begins> <ends> <cancels>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_otacore.h"
#include "sha256_stub.h"

#define LINE_MAX 4096
#define FAKE_FLASH_MAX (3 * 1024 * 1024)

// --- 偽のフラッシュ ---------------------------------------------------------
static uint8_t *g_flash;
static uint32_t g_flash_len;
static int      g_begins, g_ends, g_cancels;
static int      g_fail_begin, g_fail_write, g_fail_end;

static int fake_begin(void *ctx, uint32_t size) {
    (void)ctx;
    g_begins++;
    if (g_fail_begin) {
        return -1;
    }
    g_flash_len = 0;
    return 0;
}

static int fake_write(void *ctx, const void *data, uint32_t len) {
    (void)ctx;
    if (g_fail_write) {
        return -2;
    }
    if (g_flash_len + len > FAKE_FLASH_MAX) {
        return -3;
    }
    // ★ 先頭バイトの検査は実機の esp_ota_write と同じ形にしておく。
    if (g_flash_len == 0 && len > 0 && ((const uint8_t *)data)[0] != 0xE9) {
        return -4;
    }
    memcpy(g_flash + g_flash_len, data, len);
    g_flash_len += len;
    return 0;
}

static int fake_end(void *ctx) {
    (void)ctx;
    g_ends++;
    return g_fail_end ? -5 : 0;
}

static void fake_cancel(void *ctx) {
    (void)ctx;
    g_cancels++;
}

static const stackee_ota_flash_ops_t FLASH_OPS = {
    .begin = fake_begin, .write = fake_write,
    .end = fake_end, .cancel = fake_cancel,
};

// --- SHA-256 ----------------------------------------------------------------
static sha256_stub_t g_sha;

static void hash_init(void *ctx)  { (void)ctx; sha256_stub_init(&g_sha); }
static void hash_update(void *ctx, const void *d, uint32_t n) {
    (void)ctx; sha256_stub_update(&g_sha, d, n);
}
static void hash_finish(void *ctx, uint8_t out[32]) {
    (void)ctx; sha256_stub_final(&g_sha, out);
}

static const stackee_ota_hash_ops_t HASH_OPS = {
    .init = hash_init, .update = hash_update, .finish = hash_finish,
};

// --- 小道具 -----------------------------------------------------------------
static uint8_t *g_ring;
static uint8_t *g_bounce;
static uint32_t g_ring_size, g_bounce_size;
static uint32_t g_now_ms;

static int hexval(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static size_t parse_hex(const char *text, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (*text == ' ' || *text == '\t') { text++; }
    while (n < cap) {
        int hi = hexval(text[0]);
        if (hi < 0) { break; }
        int lo = hexval(text[1]);
        if (lo < 0) { break; }
        out[n++] = (uint8_t)((hi << 4) | lo);
        text += 2;
    }
    return n;
}

static void print_hex(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
}

static void feed_report(const uint8_t *rep, uint32_t len) {
    uint8_t reply[STACKEE_OTA_REPORT];
    bool want = false;
    if (!stackee_otacore_feed(rep, len, reply, &want)) {
        printf("notmine\n");
        return;
    }
    if (!want) {
        printf("noreply\n");
        return;
    }
    printf("reply\t");
    print_hex(reply, STACKEE_OTA_REPORT);
    printf("\n");
}

static void print_status(void) {
    stackee_ota_status_t st;
    stackee_otacore_status(&st);
    printf("status\t%s\t%s\t%u\t%u\t%u\t%u\t%u\t%u\n",
           stackee_otacore_state_name(st.state),
           stackee_otacore_err_name(st.err),
           (unsigned)st.size, (unsigned)st.accepted, (unsigned)st.written,
           (unsigned)st.pending, (unsigned)st.rejected, (unsigned)st.overflow);
}

int main(void) {
    static char line[LINE_MAX];
    static uint8_t scratch[LINE_MAX / 2];

    g_flash = malloc(FAKE_FLASH_MAX);
    if (g_flash == NULL) {
        fprintf(stderr, "偽フラッシュが取れない\n");
        return 1;
    }
    stackee_otacore_attach(&FLASH_OPS, NULL, &HASH_OPS, NULL);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; }
        if (line[0] == '\0' || line[0] == '#') { continue; }

        if (strncmp(line, "attach ", 7) == 0) {
            unsigned ring = 0, bounce = 0;
            sscanf(line + 7, "%u %u", &ring, &bounce);
            free(g_ring);
            free(g_bounce);
            g_ring = ring ? malloc(ring) : NULL;
            g_bounce = bounce ? malloc(bounce) : NULL;
            g_ring_size = ring;
            g_bounce_size = bounce;
            g_flash_len = 0;
            g_begins = g_ends = g_cancels = 0;
            g_fail_begin = g_fail_write = g_fail_end = 0;
            g_now_ms = 0;
            stackee_otacore_reset();
            stackee_otacore_attach(&FLASH_OPS, NULL, &HASH_OPS, NULL);
            printf("attach\t%u\t%u\n", ring, bounce);
        } else if (strncmp(line, "begin ", 6) == 0) {
            unsigned size = 0;
            char shahex[80] = {0};
            sscanf(line + 6, "%u %79s", &size, shahex);
            uint8_t sha[32];
            memset(sha, 0, sizeof(sha));
            parse_hex(shahex, sha, sizeof(sha));
            int rc = stackee_otacore_begin(size, sha, g_ring, g_ring_size,
                                           g_bounce, g_bounce_size, g_now_ms);
            printf("begin\t%d\t%s\n", rc, stackee_otacore_err_name(rc));
        } else if (strncmp(line, "data ", 5) == 0) {
            unsigned offset = 0;
            char *sp = strchr(line + 5, ' ');
            offset = (unsigned)strtoul(line + 5, NULL, 10);
            uint8_t rep[STACKEE_OTA_REPORT];
            memset(rep, 0, sizeof(rep));
            size_t n = sp ? parse_hex(sp, scratch, STACKEE_OTA_PAYLOAD) : 0;
            rep[0] = STACKEE_OTA_CMD_DATA;
            rep[1] = (uint8_t)n;
            rep[2] = (uint8_t)(offset & 0xFF);
            rep[3] = (uint8_t)((offset >> 8) & 0xFF);
            rep[4] = (uint8_t)((offset >> 16) & 0xFF);
            memcpy(&rep[STACKEE_OTA_HEADER], scratch, n);
            feed_report(rep, STACKEE_OTA_REPORT);
        } else if (strcmp(line, "poll") == 0) {
            uint8_t rep[STACKEE_OTA_REPORT];
            memset(rep, 0, sizeof(rep));
            rep[0] = STACKEE_OTA_CMD_DATA;
            feed_report(rep, STACKEE_OTA_REPORT);
        } else if (strncmp(line, "raw ", 4) == 0) {
            uint8_t rep[STACKEE_OTA_REPORT];
            memset(rep, 0, sizeof(rep));
            size_t n = parse_hex(line + 4, rep, sizeof(rep));
            feed_report(rep, (uint32_t)(n > 0 ? STACKEE_OTA_REPORT : 0));
        } else if (strncmp(line, "pump", 4) == 0) {
            unsigned budget = 0;
            if (sscanf(line + 4, "%u", &budget) != 1) { budget = 0xFFFFFF; }
            printf("pump\t%u\n", (unsigned)stackee_otacore_pump(budget));
        } else if (strncmp(line, "tick ", 5) == 0) {
            g_now_ms += (unsigned)strtoul(line + 5, NULL, 10);
            stackee_otacore_tick(g_now_ms);
            printf("tick\t%u\n", g_now_ms);
        } else if (strcmp(line, "end") == 0) {
            int rc = stackee_otacore_end();
            printf("end\t%d\t%s\n", rc, stackee_otacore_err_name(rc));
        } else if (strcmp(line, "abort") == 0) {
            stackee_otacore_abort();
            printf("abort\t0\n");
        } else if (strcmp(line, "status") == 0) {
            print_status();
        } else if (strncmp(line, "fail ", 5) == 0) {
            char what[16] = {0};
            int on = 0;
            sscanf(line + 5, "%15s %d", what, &on);
            if (strcmp(what, "begin") == 0) { g_fail_begin = on; }
            else if (strcmp(what, "write") == 0) { g_fail_write = on; }
            else if (strcmp(what, "end") == 0) { g_fail_end = on; }
            printf("fail\t%s\t%d\n", what, on);
        } else if (strcmp(line, "flash") == 0) {
            sha256_stub_t s;
            uint8_t out[32];
            sha256_stub_init(&s);
            sha256_stub_update(&s, g_flash, g_flash_len);
            sha256_stub_final(&s, out);
            printf("flash\t%u\t", (unsigned)g_flash_len);
            print_hex(out, 32);
            printf("\t%d\t%d\t%d\n", g_begins, g_ends, g_cancels);
        } else if (strcmp(line, "sha") == 0) {
            stackee_ota_status_t st;
            stackee_otacore_status(&st);
            printf("sha\t%d\t", st.have_sha ? 1 : 0);
            print_hex(st.got_sha, 32);
            printf("\n");
        } else {
            printf("unknown\t%s\n", line);
        }
        fflush(stdout);
    }
    return 0;
}
