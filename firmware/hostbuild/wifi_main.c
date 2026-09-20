// Wi-Fi 自動接続の状態機械 (main/stackee_wifism.c) を Mac 上で走らせる。
//
// 無線も時計も偽物。**本物の状態機械**に台本を流して、
// 「登録簿のどれを選ぶか」「何ミリ秒で次へ行くか」「失敗したらどう畳むか」
// を実機なしで確かめる。
//
// 台本 (標準入力):
//   nets <json>       登録簿 (load が返すもの)
//   ap <ssid> <ch> <rssi>   その場に見えている AP を足す
//   noap              見えている AP を全部消す
//   t <ms>            時刻を進める (1 ms ずつ step を回す)
//   keys <0|1>        打鍵の谷か
//   audio <0|1>       録音・再生中か
//   connect <0|1>     connect_start が成功するか
//   reason <n>        接続の結果 (0 = 進行中のまま / 1 = 接続 / 2 以上 = 失敗)
//   delay <ms>        connect_start から結果が出るまで
//   drop              リンクが切れたことにする
//   kick              console の wifi.connect
//   suspend / resume  console の wifi.scan が呼ぶもの
//   print
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_wifism.h"

#define AP_MAX 16

static uint32_t g_now;
static stackee_wifi_t g_sm;
static char g_nets_json[2048] = "{\"v\":1,\"networks\":[]}";

static struct {
    char ssid[STACKEE_WIFI_SSID_MAX];
    int  channel;
    int  rssi;
} g_ap[AP_MAX];
static int g_ap_count;

static int  g_scan_channel = -1;
static bool g_scanning;
static bool g_keys_idle = true;
static bool g_audio_busy;
static bool g_connect_ok = true;
static int  g_reason = 1;           // 既定は「繋がる」
static uint32_t g_connect_delay = 100;
static uint32_t g_connect_at;
static bool g_connecting;
static bool g_link;
static bool g_radio;

static uint32_t ops_now(void) { return g_now; }

static bool ops_load(stackee_wifi_list_t *out, const char **note) {
    return stackee_wifi_parse(g_nets_json, out, note);
}

static bool ops_radio_on(void) {
    if (!g_radio) {
        printf("RADIO on %u\n", (unsigned)g_now);
    }
    g_radio = true;
    return true;
}

static void ops_radio_off(void) {
    if (g_radio) {
        printf("RADIO off %u\n", (unsigned)g_now);
    }
    g_radio = false;
    g_link = false;
}

static bool ops_scan_start(int channel) {
    g_scan_channel = channel;
    g_scanning = true;
    printf("SCAN %d %u\n", channel, (unsigned)g_now);
    return true;
}

static bool ops_scan_ready(void) { return false; }   // settle だけで進める

static int ops_scan_read(stackee_wifi_seen_t *out, int max) {
    int n = 0;
    for (int i = 0; i < g_ap_count && n < max; i++) {
        if (g_ap[i].channel != g_scan_channel) {
            continue;
        }
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", g_ap[i].ssid);
        out[n].channel = g_ap[i].channel;
        out[n].rssi = g_ap[i].rssi;
        n++;
    }
    return n;
}

static void ops_scan_stop(void) { g_scanning = false; }

static bool ops_connect_start(const char *ssid, const char *password, int channel) {
    // ★ password はここでも出さない (実機と同じ約束をテストでも守る)。
    (void)password;
    printf("CONNECT %s %d %u\n", ssid, channel, (unsigned)g_now);
    if (!g_connect_ok) {
        return false;
    }
    g_connecting = true;
    g_connect_at = g_now + g_connect_delay;
    return true;
}

static int ops_connect_state(void) {
    if (!g_connecting || g_now < g_connect_at) {
        return 0;
    }
    if (g_reason == 1) {
        g_link = true;
    }
    return g_reason;
}

static bool ops_link_alive(void) { return g_link; }

static void ops_get_ip(char *out, int cap) {
    snprintf(out, (size_t)cap, "192.168.0.42");
}

static bool ops_keys_idle(void)  { return g_keys_idle; }
static bool ops_audio_busy(void) { return g_audio_busy; }

static void ops_log(const char *line) { printf("LOG %s\n", line); }
static void ops_ui(const char *name)  { printf("UI %s\n", name); }

static const stackee_wifi_ops_t OPS = {
    .now_ms = ops_now,
    .load = ops_load,
    .radio_on = ops_radio_on,
    .radio_off = ops_radio_off,
    .scan_start = ops_scan_start,
    .scan_ready = ops_scan_ready,
    .scan_read = ops_scan_read,
    .scan_stop = ops_scan_stop,
    .connect_start = ops_connect_start,
    .connect_state = ops_connect_state,
    .link_alive = ops_link_alive,
    .get_ip = ops_get_ip,
    .keys_idle = ops_keys_idle,
    .audio_busy = ops_audio_busy,
    .log = ops_log,
    .ui = ops_ui,
};

static int g_last_state = -1;

static void tick(void) {
    stackee_wifi_sm_step(&g_sm);
    if (g_sm.state != g_last_state) {
        g_last_state = g_sm.state;
        printf("STATE %s %u\n", stackee_wifi_sm_state_name(&g_sm), (unsigned)g_now);
    }
}

int main(void) {
    char line[2048];
    bool started = false;
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *arg = strchr(line, ' ');
        if (arg) { *arg++ = '\0'; }
        if (strcmp(line, "nets") == 0) {
            snprintf(g_nets_json, sizeof(g_nets_json), "%s", arg ? arg : "");
            continue;
        }
        if (!started && strcmp(line, "t") == 0) {
            stackee_wifi_sm_init(&g_sm, &OPS);
            g_last_state = g_sm.state;
            printf("STATE %s 0\n", stackee_wifi_sm_state_name(&g_sm));
            started = true;
        }
        if (strcmp(line, "t") == 0) {
            long ms = arg ? atol(arg) : 0;
            for (long i = 0; i < ms; i++) {
                g_now++;
                tick();
            }
        } else if (strcmp(line, "ap") == 0) {
            char ssid[STACKEE_WIFI_SSID_MAX] = {0};
            int ch = 0, rssi = 0;
            if (arg && sscanf(arg, "%32s %d %d", ssid, &ch, &rssi) == 3 &&
                g_ap_count < AP_MAX) {
                snprintf(g_ap[g_ap_count].ssid, sizeof(g_ap[g_ap_count].ssid),
                         "%s", ssid);
                g_ap[g_ap_count].channel = ch;
                g_ap[g_ap_count].rssi = rssi;
                g_ap_count++;
            }
        } else if (strcmp(line, "noap") == 0) {
            g_ap_count = 0;
        } else if (strcmp(line, "keys") == 0) {
            g_keys_idle = arg && atoi(arg) != 0;
        } else if (strcmp(line, "audio") == 0) {
            g_audio_busy = arg && atoi(arg) != 0;
        } else if (strcmp(line, "connect") == 0) {
            g_connect_ok = arg && atoi(arg) != 0;
        } else if (strcmp(line, "reason") == 0) {
            g_reason = arg ? atoi(arg) : 1;
        } else if (strcmp(line, "delay") == 0) {
            g_connect_delay = arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "drop") == 0) {
            g_link = false;
            g_connecting = false;
        } else if (strcmp(line, "kick") == 0) {
            printf("KICK %s\n", stackee_wifi_sm_kick(&g_sm));
            g_last_state = g_sm.state;
        } else if (strcmp(line, "suspend") == 0) {
            printf("SUSPEND %d\n", stackee_wifi_sm_suspend(&g_sm) ? 1 : 0);
            g_last_state = g_sm.state;
        } else if (strcmp(line, "resume") == 0) {
            stackee_wifi_sm_resume(&g_sm);
            g_last_state = g_sm.state;
        } else if (strcmp(line, "print") == 0) {
            printf("NOW %u STATE %s SSID %s IP %s NETS %d PASSES %u "
                   "CONNECTS %u FAILURES %u UP_MS %u SCANNING %d\n",
                   (unsigned)g_now, stackee_wifi_sm_state_name(&g_sm),
                   g_sm.ssid, g_sm.ip, g_sm.nets.count, (unsigned)g_sm.passes,
                   (unsigned)g_sm.connects, (unsigned)g_sm.failures,
                   (unsigned)g_sm.up_at_ms, g_scanning ? 1 : 0);
        } else {
            fprintf(stderr, "unknown command: %s\n", line);
            return 2;
        }
    }
    return 0;
}
