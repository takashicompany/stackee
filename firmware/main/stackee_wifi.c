#include "stackee_wifi.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "stackee_assets.h"
#include "stackee_audio.h"
#include "stackee_console.h"
#include "stackee_input.h"
#include "stackee_ui.h"
#include "stackee_wifism.h"
#include "stackee_wifistore.h"

static const char *TAG = "wifi";

#define NET_TICK_MS      20
#define NVS_NAMESPACE    "stackee"
#define NVS_KEY_NETS     "wifi_nets"
#define FAT_PATH         STACKEE_ASSETS_MOUNT "/wifi_networks.json"
// 登録簿の全文が入る大きさ。8 件 x (SSID 32B + パスワード 63B) で、
// 中身が全部 `"` か `\` でも逃がしたあとに収まること
// (8 x (64 + 126 + 40) + 20 = 1,860)。★ 足りないと保存が丸ごと失敗する。
#define STORE_MAX        2560
#define SCAN_RECORDS     24

static struct {
    bool started;
    bool driver_up;
    bool radio_on;
    stackee_wifi_t sm;
    esp_netif_t *netif;

    _Atomic bool scan_done;
    _Atomic bool got_ip;
    _Atomic int  disconnect_reason;
    _Atomic bool connecting;

    wifi_ap_record_t records[SCAN_RECORDS];

    uint32_t last_key_events;
    uint32_t last_key_ms;

    TaskHandle_t task;
    // ★ 状態機械は net タスクが進め、console タスクが kick / suspend で
    //   割り込む。同じ構造体を 2 つのタスクが書き換えるので錠をかける。
    SemaphoreHandle_t lock;
} w;

static bool sm_lock(void) {
    return w.lock != NULL && xSemaphoreTake(w.lock, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void sm_unlock(void) {
    if (w.lock != NULL) {
        xSemaphoreGive(w.lock);
    }
}

// ---------------------------------------------------------------------------
// 登録簿の置き場 (NVS。初回だけ FAT から移す)
// ---------------------------------------------------------------------------
static bool store_read(char *out, size_t cap) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = cap;
    esp_err_t err = nvs_get_blob(handle, NVS_KEY_NETS, out, &len);
    nvs_close(handle);
    if (err != ESP_OK || len == 0 || len >= cap) {
        return false;
    }
    out[len] = '\0';
    return true;
}

static bool store_write(const char *text) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(handle, NVS_KEY_NETS, text, strlen(text));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

// CircuitPython 版が FAT に書いた登録簿。読み取り専用マウントなので読むだけ。
static bool fat_read(char *out, size_t cap) {
    FILE *f = fopen(FAT_PATH, "rb");
    if (f == NULL) {
        return false;
    }
    size_t got = fread(out, 1, cap - 1, f);
    fclose(f);
    out[got] = '\0';
    return got > 0;
}

// ★ password は絶対にログへ出さない。件数だけ。
// ★ 作業用の領域はヒープに取る。net タスクと console タスクの両方から
//   呼ばれるので、static にすると互いの中身を壊し合う。
static bool load_list(stackee_wifi_list_t *out, const char **note) {
    char *text = malloc(STORE_MAX);
    if (text == NULL) {
        memset(out, 0, sizeof(*out));
        if (note) { *note = "nomem"; }
        return false;
    }
    bool ok;
    if (store_read(text, STORE_MAX)) {
        ok = stackee_wifi_parse(text, out, note);
    } else if (fat_read(text, STORE_MAX)) {
        ok = stackee_wifi_parse(text, out, note);
        if (ok && out->count > 0) {
            // ★ 初回だけ FAT から NVS へ移す。以後は NVS が正。
            if (stackee_wifi_dumps(out, text, STORE_MAX) == 0) {
                ESP_LOGW(TAG, "登録簿が入れ物に収まらない。移行しない");
            } else if (store_write(text)) {
                ESP_LOGI(TAG, "FAT の登録簿 %d 件を NVS へ移した", out->count);
            } else {
                ESP_LOGW(TAG, "FAT の登録簿を NVS へ移せなかった");
            }
        }
    } else {
        memset(out, 0, sizeof(*out));
        if (note) { *note = "missing"; }
        ok = true;
    }
    free(text);
    return ok;
}

static bool save_list(const stackee_wifi_list_t *list) {
    char *text = malloc(STORE_MAX);
    if (text == NULL) {
        return false;
    }
    // ★ 0 = 入れ物に収まらなかった。半端な JSON を保存すると登録簿を
    //   丸ごと失うので、**何も書かずに失敗を返す**。
    bool ok = stackee_wifi_dumps(list, text, STORE_MAX) > 0 && store_write(text);
    free(text);
    return ok;
}

// ---------------------------------------------------------------------------
// イベント
// ---------------------------------------------------------------------------
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_SCAN_DONE) {
        atomic_store(&w.scan_done, true);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = data;
        atomic_store(&w.got_ip, false);
        atomic_store(&w.disconnect_reason, e ? e->reason : 1);
        atomic_store(&w.connecting, false);
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        atomic_store(&w.disconnect_reason, 0);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        atomic_store(&w.got_ip, true);
        atomic_store(&w.connecting, false);
        atomic_store(&w.disconnect_reason, 0);
    }
}

// ---------------------------------------------------------------------------
// 状態機械の口
// ---------------------------------------------------------------------------
static uint32_t ops_now(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool ops_load(stackee_wifi_list_t *out, const char **note) {
    return load_list(out, note);
}

static bool ops_radio_on(void) {
    if (!w.driver_up) {
        return false;
    }
    if (w.radio_on) {
        return true;
    }
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return false;
    }
    w.radio_on = true;
    return true;
}

static void ops_radio_off(void) {
    if (!w.radio_on) {
        return;
    }
    // ★ BLE と RF を取り合わない (現行と同じ考え)。
    esp_wifi_stop();
    w.radio_on = false;
    atomic_store(&w.got_ip, false);
}

static bool ops_scan_start(int channel) {
    atomic_store(&w.scan_done, false);
    wifi_scan_config_t config = {0};
    config.channel = (uint8_t)channel;
    config.show_hidden = false;
    if (channel >= 12) {
        // パッシブ走査。滞在時間は ESP-IDF の既定の 3 倍に合わせる。
        config.scan_type = WIFI_SCAN_TYPE_PASSIVE;
        config.scan_time.passive = 360;
    } else {
        config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        config.scan_time.active.min = 100;
        config.scan_time.active.max = 120;
    }
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan ch=%d を始められない: %s", channel, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool ops_scan_ready(void) {
    return atomic_load(&w.scan_done);
}

static int ops_scan_read(stackee_wifi_seen_t *out, int max) {
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK || num == 0) {
        return 0;
    }
    if (num > SCAN_RECORDS) {
        num = SCAN_RECORDS;
    }
    if (esp_wifi_scan_get_ap_records(&num, w.records) != ESP_OK) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < (int)num && n < max; i++) {
        snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", (const char *)w.records[i].ssid);
        out[n].channel = w.records[i].primary;
        out[n].rssi = w.records[i].rssi;
        n++;
    }
    return n;
}

static void ops_scan_stop(void) {
    esp_wifi_scan_stop();
    atomic_store(&w.scan_done, false);
}

static bool ops_connect_start(const char *ssid, const char *password, int channel) {
    wifi_config_t config;
    memset(&config, 0, sizeof(config));
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password);
    if (channel > 0) {
        // ★ channel を渡すと FAST_SCAN がそこだけを見る。渡さないと全チャネル
        //   走査になり、実測で数秒増える (stackee_wifi.py のコメント)。
        config.sta.channel = (uint8_t)channel;
    }
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK) {
        return false;
    }
    atomic_store(&w.got_ip, false);
    atomic_store(&w.disconnect_reason, 0);
    atomic_store(&w.connecting, true);
    // ★ ここは即戻る。DHCP まで待たない (待つのは linkup 状態)。
    return esp_wifi_connect() == ESP_OK;
}

static int ops_connect_state(void) {
    if (atomic_load(&w.got_ip)) {
        return 1;
    }
    int reason = atomic_load(&w.disconnect_reason);
    if (reason != 0) {
        // 状態機械は「2 以上 = 切断理由」と見る。esp_wifi の理由コードを
        // そのまま渡す (1 = UNSPECIFIED も失敗扱いにしたいので 2 に寄せる)。
        return (reason < 2) ? 2 : reason;
    }
    return 0;
}

static bool ops_link_alive(void) {
    return atomic_load(&w.got_ip);
}

static void ops_get_ip(char *out, int cap) {
    esp_netif_ip_info_t info;
    if (w.netif != NULL && esp_netif_get_ip_info(w.netif, &info) == ESP_OK) {
        snprintf(out, (size_t)cap, IPSTR, IP2STR(&info.ip));
    } else {
        snprintf(out, (size_t)cap, "0.0.0.0");
    }
}

// 打鍵の谷か。★ 入力タスクには触らない (数えた数を読むだけ)。
static bool ops_keys_idle(void) {
    stackee_input_stats_t input;
    stackee_input_stats(&input);
    uint32_t now = ops_now();
    if (input.key_events != w.last_key_events || input.keys_down > 0) {
        w.last_key_events = input.key_events;
        w.last_key_ms = now;
    }
    return (uint32_t)(now - w.last_key_ms) >= STACKEE_WIFI_IDLE_BEFORE_CONNECT_MS;
}

static bool ops_audio_busy(void) {
    return stackee_audio_busy();
}

static void ops_log(const char *line) {
    ESP_LOGI(TAG, "%s", line);
}

static void ops_ui(const char *state_name) {
    stackee_ui_set_wifi(state_name);
}

static const stackee_wifi_ops_t WIFI_OPS = {
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

// ---------------------------------------------------------------------------
// console
// ---------------------------------------------------------------------------
static size_t put(char *buf, size_t cap, size_t at, const char *fmt, ...) {
    if (at >= cap) {
        return at;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + at, cap - at, fmt, args);
    va_end(args);
    return (n < 0) ? at : at + (size_t)n;
}

static size_t put_json_str(char *buf, size_t cap, size_t at, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p && at + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            at = put(buf, cap, at, "\\%c", *p);
        } else if (*p < 0x20) {
            at = put(buf, cap, at, "\\u%04X", *p);
        } else {
            at = put(buf, cap, at, "%c", *p);
        }
    }
    return at;
}

// ★ 状態機械を触るのは必ず錠の中で。
static const char *kick_locked(void) {
    if (!sm_lock()) {
        return "busy";
    }
    const char *state = stackee_wifi_sm_kick(&w.sm);
    sm_unlock();
    return state;
}

static size_t reply_list(long id, char *buf, size_t cap) {
    stackee_wifi_list_t list;
    const char *note = NULL;
    load_list(&list, &note);
    size_t at = put(buf, cap, 0, "{\"id\":%ld,\"n\":%d,\"networks\":", id, list.count);
    // ★ 外へ出してよい形は stackee_wifi_public_json が 1 か所で決めている
    //   (password は入らない)。ここで組み立て直さない。
    //   tools/test_cfg_host.py がその 1 か所を直接確かめている。
    if (at < cap) {
        size_t n = stackee_wifi_public_json(&list, buf + at, cap - at);
        if (n == 0) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"toolong\"}", id);
        }
        at += n;
    }
    if (note != NULL) {
        at = put(buf, cap, at, ",\"note\":\"%s\"", note);
    }
    return put(buf, cap, at, "}");
}

static size_t reply_add(long id, const char *line, char *buf, size_t cap) {
    // ★ **大きめの入れ物**へ読む。登録簿の大きさ (33/64 B) に直接読むと、
    //   33 バイトの SSID が 32 バイトに切り詰められて「正しい」と通り、
    //   ユーザーが打っていないものが登録される。
    char ssid[96] = {0};
    char password[128] = {0};
    if (!stackee_console_str(line, "ssid", ssid, sizeof(ssid))) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"bad_ssid\"}", id);
    }
    // ★ password のキー自体が無い / null のときは弾く。現行
    //   stackee_wifi_store.validate(None) が 'bad_password' を返すのに合わせる
    //   (オープンな AP は `"password":""` と明示して登録する)。
    if (!stackee_console_str(line, "password", password, sizeof(password))) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"bad_password\"}", id);
    }
    // channel は 1..14 の整数か省略 (null)。★ 真偽値は整数の仲間ではない。
    const char *ch_text = stackee_console_value(line, "channel");
    if (ch_text != NULL && (strncmp(ch_text, "true", 4) == 0 ||
                            strncmp(ch_text, "false", 5) == 0)) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"bad_channel\"}", id);
    }
    int channel = (int)stackee_console_int(line, "channel", 0);
    const char *bad = stackee_wifi_validate(ssid, password, channel);
    if (bad != NULL) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"%s\"}", id, bad);
    }
    stackee_wifi_list_t list;
    const char *note = NULL;
    load_list(&list, &note);
    bad = stackee_wifi_upsert(&list, ssid, password, channel);
    if (bad != NULL) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"%s\"}", id, bad);
    }
    if (!save_list(&list)) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"write_failed\"}", id);
    }
    const char *state = kick_locked();
    return put(buf, cap, 0,
               "{\"id\":%ld,\"ok\":1,\"n\":%d,\"wifi_state\":\"%s\"}",
               id, list.count, state);
}

static size_t reply_remove(long id, const char *line, char *buf, size_t cap) {
    char ssid[STACKEE_WIFI_SSID_MAX] = {0};
    if (!stackee_console_str(line, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"bad_ssid\"}", id);
    }
    stackee_wifi_list_t list;
    const char *note = NULL;
    load_list(&list, &note);
    const char *bad = stackee_wifi_remove_ssid(&list, ssid);
    if (bad != NULL) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"%s\"}", id, bad);
    }
    if (!save_list(&list)) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"write_failed\"}", id);
    }
    const char *state = kick_locked();
    return put(buf, cap, 0,
               "{\"id\":%ld,\"ok\":1,\"n\":%d,\"wifi_state\":\"%s\"}",
               id, list.count, state);
}

// 全チャネルを 1 回走査する。★ 録音・再生中は断る (現行と同じ制約)。
static size_t reply_scan(long id, char *buf, size_t cap) {
    if (stackee_audio_busy()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"audio_busy\"}", id);
    }
    int64_t t0 = esp_timer_get_time();
    if (!sm_lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    bool held = stackee_wifi_sm_suspend(&w.sm);
    sm_unlock();
    if (!ops_radio_on()) {
        if (sm_lock()) {
            stackee_wifi_sm_resume(&w.sm);
            sm_unlock();
        }
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"radio\"}", id);
    }
    atomic_store(&w.scan_done, false);
    wifi_scan_config_t config = {0};
    config.show_hidden = false;
    config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 100;
    config.scan_time.active.max = 150;
    if (esp_wifi_scan_start(&config, false) != ESP_OK) {
        if (sm_lock()) {
            stackee_wifi_sm_resume(&w.sm);
            sm_unlock();
        }
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"scan\"}", id);
    }
    // 全チャネルで 3〜6 秒。ここで待つのは console タスクだけで、キー入力は
    // 別タスク (CPU1) なので打鍵は止まらない。
    for (int i = 0; i < 900 && !atomic_load(&w.scan_done); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    stackee_wifi_seen_t seen[STACKEE_WIFI_SEEN_MAX];
    int n = ops_scan_read(seen, STACKEE_WIFI_SEEN_MAX);
    esp_wifi_scan_stop();
    if (!held) {
        ops_radio_off();
    }
    if (sm_lock()) {
        stackee_wifi_sm_resume(&w.sm);
        sm_unlock();
    }

    // SSID が重複するので、いちばん強い 1 つに畳む。
    int order[STACKEE_WIFI_SEEN_MAX];
    int count = 0;
    for (int i = 0; i < n; i++) {
        int found = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(seen[order[j]].ssid, seen[i].ssid) == 0) {
                found = j;
                break;
            }
        }
        if (found < 0) {
            order[count++] = i;
        } else if (seen[i].rssi > seen[order[found]].rssi) {
            order[found] = i;
        }
    }
    // 強い順に並べる。
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            if (seen[order[j]].rssi > seen[order[i]].rssi) {
                int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }
    if (count > 10) {
        count = 10;             // 1 枠に収まる数 (CDC の送信 FIFO は 2048 B)
    }
    size_t at = put(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"radio_kept\":%d,\"nets\":[",
                    id, held ? 1 : 0);
    for (int i = 0; i < count; i++) {
        const stackee_wifi_seen_t *net = &seen[order[i]];
        at = put(buf, cap, at, "%s{\"ssid\":\"", i ? "," : "");
        at = put_json_str(buf, cap, at, net->ssid);
        at = put(buf, cap, at, "\",\"ch\":%d,\"rssi\":%d}", net->channel, net->rssi);
    }
    return put(buf, cap, at, "],\"n\":%d,\"total_ms\":%lu}", count,
               (unsigned long)((esp_timer_get_time() - t0) / 1000));
}

static size_t wifi_console(const char *cmd, const char *line, long id,
                           char *buf, size_t cap) {
    if (!w.started) {
        return 0;
    }
    if (strcmp(cmd, "wifi.list") == 0)    { return reply_list(id, buf, cap); }
    if (strcmp(cmd, "wifi.add") == 0)     { return reply_add(id, line, buf, cap); }
    if (strcmp(cmd, "wifi.remove") == 0)  { return reply_remove(id, line, buf, cap); }
    if (strcmp(cmd, "wifi.scan") == 0)    { return reply_scan(id, buf, cap); }
    if (strcmp(cmd, "wifi.connect") == 0) {
        // ★ ここでは絶対にブロックしない。次の周期から出直させるだけ。
        const char *state = kick_locked();
        return put(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"wifi_state\":\"%s\"}",
                   id, state);
    }
    if (strcmp(cmd, "wifi.off") == 0) {
        // ★ 切り分け用。無線を止めて自動接続も黙らせる。
        //   「Wi-Fi を切ると BLE が繋がるか」を人手なしで確かめられる。
        if (!sm_lock()) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
        }
        stackee_wifi_sm_suspend(&w.sm);
        sm_unlock();
        ops_radio_off();
        ESP_LOGW(TAG, "wifi.off: 無線を止めた (自動接続も止まる)");
        return put(buf, cap, 0,
                   "{\"id\":%ld,\"ok\":1,\"radio\":false,\"wifi_state\":\"%s\"}",
                   id, stackee_wifi_sm_state_name(&w.sm));
    }
    if (strcmp(cmd, "wifi.on") == 0) {
        if (!sm_lock()) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
        }
        stackee_wifi_sm_resume(&w.sm);
        const char *state = stackee_wifi_sm_kick(&w.sm);
        sm_unlock();
        ESP_LOGI(TAG, "wifi.on: 自動接続を再開する");
        return put(buf, cap, 0,
                   "{\"id\":%ld,\"ok\":1,\"radio\":true,\"wifi_state\":\"%s\"}",
                   id, state);
    }
    if (strcmp(cmd, "wifi.status") == 0) {
        if (!sm_lock()) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
        }
        size_t at = put(buf, cap, 0,
                   "{\"id\":%ld,\"ok\":1,\"wifi_state\":\"%s\",\"ssid\":\"%s\","
                   "\"ip\":\"%s\",\"nets\":%d,\"passes\":%lu,\"connects\":%lu,"
                   "\"failures\":%lu,\"connect_ms\":%lu,\"up_ms\":%lu}",
                   id, stackee_wifi_sm_state_name(&w.sm), w.sm.ssid, w.sm.ip,
                   w.sm.nets.count, (unsigned long)w.sm.passes,
                   (unsigned long)w.sm.connects, (unsigned long)w.sm.failures,
                   (unsigned long)w.sm.connect_ms, (unsigned long)w.sm.up_at_ms);
        sm_unlock();
        return at;
    }
    return 0;
}

// ---------------------------------------------------------------------------
static void net_task(void *unused) {
    (void)unused;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(NET_TICK_MS));
        if (!sm_lock()) {
            continue;
        }
        stackee_wifi_sm_step(&w.sm);
        sm_unlock();
    }
}

esp_err_t stackee_wifi_start(void) {
    if (w.started) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err));
        return err;
    }
    w.netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        on_wifi_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        on_ip_event, NULL, NULL);
    // ★ 認証情報を NVS に置かない。登録簿はこちらで持っている
    //   (ドライバに二重に持たせると、消したはずの AP へ勝手に繋ぎに行く)。
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    // ★ 省電力を**明示する**。BLE と RF を分け合うには、STA が DTIM の
    //   合間に電波を離す modem sleep である必要がある
    //   (esp_wifi.h: "Default power save type is WIFI_PS_MIN_MODEM")。
    //   既定と同じ値だが、既定に頼らず書いておく。
    //   WIFI_PS_NONE にすると BLE のアドバタイズが電波の時間を取れない。
    esp_err_t ps = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    // ★ 電波の取り合いは **BLE を優先**する。この機械の本業はキーボードで、
    //   Wi-Fi は会話のときだけ使う。繋がらない BLE より遅い会話のほうがよい。
    //   (esp_coexist.h: ESP_COEX_PREFER_BT = "bluetooth will have more
    //    opportunity to use RF")
    esp_err_t coex = esp_coex_preference_set(ESP_COEX_PREFER_BT);
    ESP_LOGI(TAG, "省電力 MIN_MODEM (%s) / 共存は BLE 優先 (%s)",
             esp_err_to_name(ps), esp_err_to_name(coex));
    w.driver_up = true;
    w.last_key_ms = ops_now();

    w.lock = xSemaphoreCreateMutex();
    if (w.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    stackee_wifi_sm_init(&w.sm, &WIFI_OPS);
    w.started = true;
    stackee_console_register(wifi_console);
    // CPU0 / 低優先度。入力 (CPU1 / 最高) とは別の CPU。
    if (xTaskCreatePinnedToCore(net_task, "stackee_net", 6144, NULL, 2, &w.task, 0)
            != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool stackee_wifi_connected(void) {
    return w.started && stackee_wifi_sm_connected(&w.sm);
}

const char *stackee_wifi_state_name(void) {
    return w.started ? stackee_wifi_sm_state_name(&w.sm) : "off";
}

const char *stackee_wifi_ssid(void) {
    return w.sm.ssid;
}

const char *stackee_wifi_ip(void) {
    return w.sm.ip;
}

int stackee_wifi_net_count(void) {
    return w.sm.nets.count;
}

uint32_t stackee_wifi_connect_ms(void) {
    return w.sm.connect_ms;
}

uint32_t stackee_wifi_up_ms(void) {
    return w.sm.up_at_ms;
}
