// 段階 4: Raw HID の上のコンソール (main/stackee_conhid.c) を Mac 上で回す。
//
// via_command_kb() は QMK から呼ばれるが、中身は ESP-IDF にも QMK にも
// 依存していない (環状バッファと 32 バイトの詰め替えだけ)。だから
// raw_hid_send() の代わりを置くだけでホストで動かせる。
//
// tools/test_conhid_host.py が、tools/console_hid.py (Mac 側の実装) と
// docs/js/hid.js (ブラウザ側の実装) の 3 つが同じ形を作るかを見る。
//
// 台本 (標準入力、1 行 1 手):
//   tx <hex>     0xC0 のレポートを 1 枚食わせる (32 バイトぶんの 16 進)
//   raw <hex>    任意のレポートを 1 枚食わせる (VIA の本来のコマンドも試せる)
//   rx           0xC1 を 1 枚食わせる
//   info         0xC2 を 1 枚食わせる
//   write <hex>  応答を積む (必ず入る。場所が無ければ古いものを押し出す)
//   log <hex>    ログを積む (ホストが最近読んでいなければ捨てられる)
//   tick <ms>    時計を進める (ホストが読んでいるかの判定に使う)
//   read         溜まっている受信バイトを全部吐く
//
// 出す行 (タブ区切り):
//   reply   <hex>      via_command_kb が raw_hid_send した 32 バイト
//   noreply            via_command_kb が false を返した (VIA に渡す)
//   read    <hex>      stackee_conhid_read で取り出せたバイト列
//   stats   <tx_pending> <tx_dropped> <tx_overrun> <rx_dropped> <in> <out> <polls>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_conhid.h"

#define RAW_SIZE 32

static uint8_t g_last_reply[RAW_SIZE];
static int     g_have_reply;

// QMK の代わり。via.c は応答をこれで返す。
void raw_hid_send(uint8_t *data, uint8_t length) {
    memset(g_last_reply, 0, sizeof(g_last_reply));
    memcpy(g_last_reply, data, (length > RAW_SIZE) ? RAW_SIZE : length);
    g_have_reply = 1;
}

bool via_command_kb(uint8_t *data, uint8_t length);

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

static void print_hex(const char *tag, const uint8_t *data, size_t len) {
    printf("%s\t", tag);
    for (size_t i = 0; i < len; i++) {
        printf("%02x", data[i]);
    }
    printf("\n");
}

static void feed(uint8_t *report, size_t len) {
    g_have_reply = 0;
    bool handled = via_command_kb(report, (uint8_t)len);
    if (handled && g_have_reply) {
        print_hex("reply", g_last_reply, RAW_SIZE);
    } else {
        printf("noreply\n");
    }
}

int main(void) {
    stackee_conhid_init();
    // ★ 1 行に 8 KB ぶんの 16 進 (= 16 KB 文字) が来る。切り詰めると
    //   「溢れの検査」が溢れないまま通ってしまう。
    static char line[65536];
    uint8_t report[RAW_SIZE];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strncmp(line, "tx ", 3) == 0 || strncmp(line, "raw ", 4) == 0) {
            memset(report, 0, sizeof(report));
            parse_hex(line + ((line[0] == 't') ? 3 : 4), report, sizeof(report));
            feed(report, sizeof(report));
        } else if (strncmp(line, "rx", 2) == 0) {
            memset(report, 0, sizeof(report));
            report[0] = STACKEE_CONHID_CMD_RX;
            feed(report, sizeof(report));
        } else if (strncmp(line, "info", 4) == 0) {
            memset(report, 0, sizeof(report));
            report[0] = STACKEE_CONHID_CMD_INFO;
            feed(report, sizeof(report));
        } else if (strncmp(line, "write ", 6) == 0) {
            static uint8_t body[8192];
            size_t n = parse_hex(line + 6, body, sizeof(body));
            stackee_conhid_write(body, n);
        } else if (strncmp(line, "log ", 4) == 0) {
            static uint8_t body[8192];
            size_t n = parse_hex(line + 4, body, sizeof(body));
            stackee_conhid_write_log(body, n);
        } else if (strncmp(line, "tick ", 5) == 0) {
            stackee_conhid_tick((uint32_t)strtoul(line + 5, NULL, 10));
        } else if (strncmp(line, "read", 4) == 0) {
            static uint8_t body[8192];
            size_t n = 0;
            uint8_t b = 0;
            while (n < sizeof(body) && stackee_conhid_read(&b)) {
                body[n++] = b;
            }
            print_hex("read", body, n);
        }
    }
    stackee_conhid_stats_t st;
    stackee_conhid_stats(&st);
    printf("stats\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", st.tx_pending, st.tx_dropped,
           st.tx_overrun, st.rx_dropped, st.reports_in, st.reports_out,
           st.polls);
    return 0;
}
