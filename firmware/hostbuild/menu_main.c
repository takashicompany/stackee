// 本体の設定メニュー (main/stackee_menu_core.c + stackee_draw_menu) を Mac で走らせる。
//
// 実機と**同じ実体**に、偽の「外の様子」(info) と偽の ops を渡して、
// 階層の出入り・選択・パスワードの編集・Wi-Fi の切り替え / 追加の呼び出しを
// 台本で確かめる。画面は実機と同じ font16.bin で描いて CRC32 を出す
// (tools/test_menu_host.py が tools/menu_expected.py の期待値と突き合わせる)。
//
//   ./menu <font16.bin>       ← 命令は標準入力から 1 行 1 つ
//
//     open / close
//     key <up|down|left|right|enter|esc|bs|char> [文字]
//     type <文字列>             1 文字ずつ char を送る (空白は使えない)
//     hid <使用番号 16 進> <修飾 16 進>   JIS として読んでから送る
//     set <欄> <値>             info を書き換える (下の set_info)
//     saved <ssid,ssid,...>     登録簿 ("-" で空)
//     net <ssid> <rssi> <ch> <0|1>   走査結果を 1 件足す
//     nonets                    走査結果を空に
//     fail <op> <0|1>           その op を失敗させる
//     view                      いまの画面 (JSON 1 行) と CRC
//     state                     階層・選択・パスワードの長さ・中身が 0 か
//
// 出るもの:
//   OP <名前> <引数...>         ops が呼ばれた (パスワードも出す。ホストの台本だけ)
//   VIEW {...}                  画面
//   CRC <crc32>                 y=28..319 の CRC32
//   STATE ...
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_crc32.h"
#include "stackee_draw.h"
#include "stackee_font16.h"
#include "stackee_menu_core.h"

#define WIDTH   240
#define HEIGHT  320
#define STRIDE  (WIDTH * 2)

static uint8_t g_fb[STRIDE * HEIGHT];
static stackee_font16_t g_font;
static bool g_ready;
static stackee_menu_t g_menu;
static stackee_menu_info_t g_info;
static stackee_menu_view_t g_view;
static bool g_fail_add, g_fail_switch, g_fail_remove, g_fail_scan;

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "開けない: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size + 1);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        exit(1);
    }
    fclose(f);
    buf[size] = '\0';
    *len = (size_t)size;
    return buf;
}

// ---- 偽の ops ---------------------------------------------------------------
static bool op_switch(const char *ssid) {
    printf("OP switch %s\n", ssid);
    return !g_fail_switch;
}

static bool op_add(const char *ssid, const char *password, int channel) {
    printf("OP add %s %s %d\n", ssid, password[0] ? password : "(空)", channel);
    return !g_fail_add;
}

static bool op_remove(const char *ssid) {
    printf("OP remove %s\n", ssid);
    return !g_fail_remove;
}

static bool op_scan(void) {
    printf("OP scan\n");
    return !g_fail_scan;
}

static void op_hid(void) { printf("OP hid_toggle\n"); }
static void op_volume(int delta) { printf("OP volume %d\n", delta); }
static void op_clip_auto(void) { printf("OP clip_auto\n"); }
static void op_clip_sync(void) { printf("OP clip_sync\n"); }
static void op_health(void) { printf("OP health\n"); }
static void op_close(void) {
    printf("OP close\n");
    stackee_menu_close(&g_menu);
}

static const stackee_menu_ops_t OPS = {
    .wifi_switch = op_switch,
    .wifi_add = op_add,
    .wifi_remove = op_remove,
    .wifi_scan = op_scan,
    .hid_toggle = op_hid,
    .volume_step = op_volume,
    .clip_auto_toggle = op_clip_auto,
    .clip_sync = op_clip_sync,
    .health_test = op_health,
    .close = op_close,
};

// ---- 出力 -------------------------------------------------------------------
static void put_json_str(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            printf("\\%c", *p);
        } else if (*p < 0x20) {
            printf("\\u%04x", *p);
        } else {
            putchar(*p);
        }
    }
    putchar('"');
}

static void cmd_view(void) {
    stackee_menu_build(&g_menu, &g_info, &g_view);
    printf("VIEW {\"screen\":\"%s\",\"title\":",
           stackee_menu_screen_names[g_view.screen]);
    put_json_str(g_view.title);
    printf(",\"footer\":");
    put_json_str(g_view.footer);
    printf(",\"selected\":%d,\"top\":%d,\"rows\":[", g_view.selected, g_view.top);
    for (int i = 0; i < g_view.count; i++) {
        const stackee_menu_row_t *r = &g_view.rows[i];
        printf("%s[%d,%d,%d,", i ? "," : "", r->kind, r->action, r->arg);
        put_json_str(r->label);
        putchar(',');
        put_json_str(r->value);
        putchar(']');
    }
    printf("]}\n");
    stackee_canvas_t canvas = {
        .fb = g_fb, .stride = STRIDE, .width = WIDTH, .height = HEIGHT};
    memset(g_fb, 0x5A, sizeof(g_fb));      // 描かれない所が残っていれば CRC が変わる
    stackee_draw_menu(&canvas, g_ready ? &g_font : NULL, &g_view);
    printf("CRC %u\n", stackee_crc32(0, g_fb + (size_t)STACKEE_MENU_Y * STRIDE,
                                     (size_t)STACKEE_MENU_HEIGHT * STRIDE));
}

static void cmd_state(void) {
    bool zero = true;
    for (size_t i = 0; i < sizeof(g_menu.password); i++) {
        if (g_menu.password[i] != 0) {
            zero = false;
        }
    }
    printf("STATE open=%d screen=%s depth=%d sel=%d pass_len=%d pass_zero=%d "
           "keys=%lu\n",
           g_menu.open ? 1 : 0, stackee_menu_screen_names[g_menu.screen],
           g_menu.depth, g_menu.sel, g_menu.pass_len, zero ? 1 : 0,
           (unsigned long)g_menu.keys);
}

static void send_key(stackee_menu_key_t key, char ch) {
    stackee_menu_key(&g_menu, &g_info, &g_view, key, ch);
}

static void set_str(char *dst, size_t cap, const char *v) {
    snprintf(dst, cap, "%s", (v == NULL || strcmp(v, "-") == 0) ? "" : v);
}

static void set_info(const char *field, const char *v) {
    stackee_menu_info_t *i = &g_info;
    long n = v ? strtol(v, NULL, 0) : 0;
#define S(name) if (strcmp(field, #name) == 0) { set_str(i->name, sizeof(i->name), v); return; }
#define N(name) if (strcmp(field, #name) == 0) { i->name = n; return; }
    S(wifi_state) S(ssid) S(ip) N(rssi) S(target) N(target_result) N(target_reason)
    N(scan_state) S(scan_error) N(job_state) S(job_error)
    N(server_configured) S(server_host) N(http_seen) N(http_last_ok) N(http_last_status)
    N(http_last_ago_ms) S(inbox_phase) N(inbox_on) N(inbox_held) N(health_state)
    N(health_status) N(health_ms) S(health_error)
    N(clips_ready) N(clips_count) N(clips_bytes) N(clips_free) N(clips_auto)
    N(clips_forced_off) N(clips_synced) N(clips_last_ago_ms) S(clips_result)
    S(clips_phase) N(clips_sync_requested)
    S(version) S(profile) N(dest_selected) N(dest_effective) N(usb_mounted)
    N(ble_connected) N(ble_advertising) S(ble_peer) N(battery) N(battery_present)
    N(charging) N(mic_ready) N(volume)
#undef S
#undef N
    fprintf(stderr, "知らない欄: %s\n", field);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "使い方: menu <font16.bin>\n");
        return 2;
    }
    size_t len = 0;
    void *blob = read_file(argv[1], &len);
    g_ready = stackee_font16_open(&g_font, blob, len);
    stackee_menu_init(&g_menu, &OPS);
    snprintf(g_info.wifi_state, sizeof(g_info.wifi_state), "off");
    g_info.battery = -1;
    g_info.battery_present = -1;

    char line[1024];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *cmd = strtok(line, " ");
        char *a1 = strtok(NULL, " ");
        char *a2 = strtok(NULL, " ");
        if (strcmp(cmd, "open") == 0) {
            stackee_menu_open(&g_menu);
        } else if (strcmp(cmd, "close") == 0) {
            stackee_menu_close(&g_menu);
        } else if (strcmp(cmd, "key") == 0) {
            stackee_menu_key_t key;
            if (a1 == NULL || !stackee_menu_key_parse(a1, &key)) {
                fprintf(stderr, "知らないキー\n");
                return 2;
            }
            send_key(key, a2 ? a2[0] : 0);
        } else if (strcmp(cmd, "type") == 0) {
            for (const char *p = a1 ? a1 : ""; *p; p++) {
                send_key(STACKEE_MENU_K_CHAR, *p);
            }
        } else if (strcmp(cmd, "hid") == 0) {
            stackee_menu_key_t key;
            char ch = 0;
            uint8_t usage = (uint8_t)strtoul(a1, NULL, 16);
            uint8_t mods = (uint8_t)(a2 ? strtoul(a2, NULL, 16) : 0);
            if (stackee_menu_key_from_hid(usage, mods, &key, &ch)) {
                printf("HIDKEY %s %d\n", stackee_menu_key_name(key), (int)(unsigned char)ch);
                send_key(key, ch);
            } else {
                printf("HIDKEY - 0\n");
            }
        } else if (strcmp(cmd, "set") == 0) {
            set_info(a1, a2);
        } else if (strcmp(cmd, "saved") == 0) {
            g_info.saved_count = 0;
            if (a1 != NULL && strcmp(a1, "-") != 0) {
                for (char *tok = strtok(a1, ","); tok && g_info.saved_count < STACKEE_MENU_SAVED_MAX;
                     tok = strtok(NULL, ",")) {
                    set_str(g_info.saved[g_info.saved_count++], STACKEE_MENU_SSID_MAX, tok);
                }
            }
        } else if (strcmp(cmd, "net") == 0) {
            char *a3 = strtok(NULL, " ");
            char *a4 = strtok(NULL, " ");
            if (g_info.scan_count < STACKEE_MENU_NETS_MAX) {
                stackee_menu_net_t *n = &g_info.nets[g_info.scan_count++];
                set_str(n->ssid, sizeof(n->ssid), a1);
                n->rssi = atoi(a2);
                n->channel = atoi(a3);
                n->secure = atoi(a4) != 0;
            }
        } else if (strcmp(cmd, "nonets") == 0) {
            g_info.scan_count = 0;
        } else if (strcmp(cmd, "fail") == 0) {
            bool on = a2 && atoi(a2);
            if (strcmp(a1, "add") == 0) { g_fail_add = on; }
            else if (strcmp(a1, "switch") == 0) { g_fail_switch = on; }
            else if (strcmp(a1, "remove") == 0) { g_fail_remove = on; }
            else if (strcmp(a1, "scan") == 0) { g_fail_scan = on; }
        } else if (strcmp(cmd, "view") == 0) {
            cmd_view();
        } else if (strcmp(cmd, "state") == 0) {
            cmd_state();
        } else {
            fprintf(stderr, "知らない命令: %s\n", cmd);
            return 2;
        }
    }
    return 0;
}
