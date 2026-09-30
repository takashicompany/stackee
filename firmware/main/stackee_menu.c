#include "stackee_menu.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"

#include "qmk_port.h"
#include "stackee_audio.h"
#include "stackee_ble.h"
#include "stackee_board.h"
#include "stackee_camera.h"
#include "stackee_console.h"
#include "stackee_hid_dest.h"
#include "stackee_http.h"
#include "stackee_menu_core.h"
#include "stackee_ota.h"
#include "stackee_uac.h"
#include "stackee_usb.h"
#include "stackee_volume.h"
#include "stackee_wifi.h"

static const char *TAG = "menu";

#define KEY_QUEUE_LEN   32
#define INFO_EVERY_MS   250

typedef enum {
    JOB_NONE = 0,
    JOB_ADD,
    JOB_REMOVE,
    JOB_SWITCH,
} menu_job_t;

// ★ 実体は PSRAM (画面 2 枚ぶんの行と外の様子の写しで約 8 KB)。
typedef struct {
    stackee_menu_t      core;
    stackee_menu_info_t info;
    stackee_menu_view_t view;       // 作業用 (キーを捌くとき・組み立てるとき)
    stackee_menu_view_t shown;      // いま画面に出ているもの
    bool                shown_valid;
    QueueHandle_t       keys;       // (キー << 8) | 文字
    uint32_t            info_at;
    int                 battery;
    bool                charging;

    // 登録済みの SSID (★ パスワードは持たない)。メインループが読み直す。
    char     saved[STACKEE_MENU_SAVED_MAX][STACKEE_MENU_SSID_MAX];
    int      saved_count;
    _Atomic bool list_req;

    // 登録・削除・切り替え (ui タスクが置き、メインループが片づける)
    _Atomic int job;                // menu_job_t
    char     job_ssid[STACKEE_MENU_SSID_MAX];
    char     job_pass[STACKEE_MENU_PASS_MAX];   // ★ 片づけたらすぐ消す
    int      job_channel;
    _Atomic int job_state;          // stackee_menu_job_t
    char     job_error[24];

    // 走査
    _Atomic bool scan_req;
    _Atomic int  scan_state;
    char     scan_error[24];
    int      scan_count;
    stackee_wifi_scan_net_t nets[STACKEE_MENU_NETS_MAX];

    _Atomic bool hid_req;

    // 数字 (menu.status)
    uint32_t refused;
    const char *refused_why;
    uint32_t closes;
    uint32_t paints;
    uint32_t paint_us, paint_max_us;
    uint32_t queue_drops;
    uint32_t jobs_done, jobs_failed, scans;
} menu_ctx_t;

static menu_ctx_t *s_m;
// ★ 開いているか。入力タスクが 1 ms ごとに読むので内蔵 RAM (1 バイト)。
static _Atomic bool s_open;

bool stackee_menu_is_open(void) {
    return atomic_load(&s_open);
}

// ---------------------------------------------------------------------------
// 開けるか
// ---------------------------------------------------------------------------
const char *stackee_menu_blocker(void) {
    if (s_m == NULL) {
        return "not_ready";
    }
    // ★ 会話・写真・Custom・クリップの処理中は開かない (画面と帯を使っている)。
    const char *why = stackee_audio_menu_blocker();
    if (why != NULL) {
        return why;
    }
    if (stackee_camera_busy()) {
        return "camera";
    }
    if (stackee_ota_busy()) {
        return "ota";
    }
    return NULL;
}

static bool try_open(void) {
    const char *why = stackee_menu_blocker();
    if (why != NULL) {
        s_m->refused++;
        s_m->refused_why = why;
        return false;
    }
    atomic_store(&s_open, true);
    return true;
}

void stackee_menu_settings_key(void) {
    if (s_m == NULL) {
        return;
    }
    // ★ 入力タスク。ログも出さない (印を置くだけ)。
    if (atomic_load(&s_open)) {
        atomic_store(&s_open, false);
        return;
    }
    try_open();
}

static void push_key(stackee_menu_key_t key, char ch) {
    uint16_t item = (uint16_t)(((unsigned)key << 8) | (uint8_t)ch);
    if (xQueueSend(s_m->keys, &item, 0) != pdTRUE) {
        s_m->queue_drops++;
    }
}

void stackee_menu_hid_key(uint8_t usage, uint8_t mods) {
    if (s_m == NULL || !atomic_load(&s_open)) {
        return;
    }
    stackee_menu_key_t key;
    char ch = 0;
    if (stackee_menu_key_from_hid(usage, mods, &key, &ch)) {
        push_key(key, ch);
    }
}

// ---------------------------------------------------------------------------
// 頼みごと (ui タスクの中、stackee_menu_key から呼ばれる。待たない)
// ---------------------------------------------------------------------------
static void wipe(volatile char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        p[i] = 0;
    }
}

static bool put_job(int kind, const char *ssid, const char *password, int channel) {
    if (atomic_load(&s_m->job) != JOB_NONE) {
        return false;               // 前のがまだ片づいていない
    }
    snprintf(s_m->job_ssid, sizeof(s_m->job_ssid), "%s", ssid);
    wipe(s_m->job_pass, sizeof(s_m->job_pass));
    if (password != NULL) {
        snprintf(s_m->job_pass, sizeof(s_m->job_pass), "%s", password);
    }
    s_m->job_channel = channel;
    s_m->job_error[0] = '\0';
    atomic_store(&s_m->job_state, STACKEE_MENU_JOB_RUNNING);
    atomic_store(&s_m->job, kind);
    return true;
}

static bool op_switch(const char *ssid) {
    return put_job(JOB_SWITCH, ssid, NULL, 0);
}

static bool op_add(const char *ssid, const char *password, int channel) {
    return put_job(JOB_ADD, ssid, password, channel);
}

static bool op_remove(const char *ssid) {
    return put_job(JOB_REMOVE, ssid, NULL, 0);
}

static bool op_scan(void) {
    atomic_store(&s_m->scan_state, STACKEE_MENU_JOB_RUNNING);
    atomic_store(&s_m->scan_req, true);
    return true;
}

static void op_hid(void) {
    atomic_store(&s_m->hid_req, true);      // NVS に書くのでメインループで
}

static void op_volume(int delta) {
    // Vol キーと同じ道 (値を変えるだけ。NVS へは audio タスクが静まってから)。
    stackee_volume_bump(delta);
}

static void op_clip_auto(void) {
    stackee_audio_menu_clip_auto();
}

static void op_clip_sync(void) {
    stackee_audio_menu_clip_sync();
}

static void op_health(void) {
    stackee_audio_menu_health();
}

static void op_close(void) {
    atomic_store(&s_open, false);
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

// ---------------------------------------------------------------------------
// メインループ
// ---------------------------------------------------------------------------
static void refresh_saved(void) {
    char tmp[STACKEE_MENU_SAVED_MAX][STACKEE_WIFI_SSID_MAX];
    int n = stackee_wifi_saved_ssids(tmp, STACKEE_MENU_SAVED_MAX);
    for (int i = 0; i < n; i++) {
        snprintf(s_m->saved[i], sizeof(s_m->saved[i]), "%s", tmp[i]);
    }
    s_m->saved_count = n;
}

static void job_done(const char *err) {
    if (err != NULL) {
        snprintf(s_m->job_error, sizeof(s_m->job_error), "%s", err);
        atomic_store(&s_m->job_state, STACKEE_MENU_JOB_FAILED);
        s_m->jobs_failed++;
    } else {
        atomic_store(&s_m->job_state, STACKEE_MENU_JOB_OK);
        s_m->jobs_done++;
    }
}

void stackee_menu_poll(void) {
    if (s_m == NULL) {
        return;
    }
    if (atomic_exchange(&s_m->list_req, false)) {
        refresh_saved();
    }
    if (atomic_exchange(&s_m->hid_req, false)) {
        stackee_hid_dest_t dest = stackee_hid_dest_toggle();
        ESP_LOGI(TAG, "送信先を %s にした (設定メニュー)", stackee_hid_dest_name(dest));
    }
    int job = atomic_load(&s_m->job);
    if (job != JOB_NONE) {
        const char *err = NULL;
        switch (job) {
            case JOB_ADD:
                // ★ パスワードはここで登録簿へ渡して、すぐ消す。ログに出さない。
                err = stackee_wifi_store_add(s_m->job_ssid, s_m->job_pass,
                                             s_m->job_channel);
                wipe(s_m->job_pass, sizeof(s_m->job_pass));
                if (err == NULL && !stackee_wifi_prefer(s_m->job_ssid)) {
                    err = "busy";
                }
                refresh_saved();
                break;
            case JOB_REMOVE:
                err = stackee_wifi_store_remove(s_m->job_ssid);
                refresh_saved();
                break;
            case JOB_SWITCH:
                if (!stackee_wifi_prefer(s_m->job_ssid)) {
                    err = "busy";
                } else {
                    ESP_LOGI(TAG, "%s へ切り替える (設定メニュー)", s_m->job_ssid);
                }
                break;
            default:
                break;
        }
        job_done(err);
        atomic_store(&s_m->job, JOB_NONE);
    }
    if (atomic_exchange(&s_m->scan_req, false)) {
        stackee_wifi_scan_net_t nets[STACKEE_MENU_NETS_MAX];
        const char *err = NULL;
        int n = stackee_wifi_scan_nets(nets, STACKEE_MENU_NETS_MAX, NULL, &err);
        s_m->scans++;
        if (n < 0) {
            snprintf(s_m->scan_error, sizeof(s_m->scan_error), "%s", err ? err : "scan");
            s_m->scan_count = 0;
            atomic_store(&s_m->scan_state, STACKEE_MENU_JOB_FAILED);
        } else {
            memcpy(s_m->nets, nets, sizeof(nets[0]) * (size_t)n);
            s_m->scan_count = n;
            atomic_store(&s_m->scan_state, STACKEE_MENU_JOB_OK);
        }
    }
}

// ---------------------------------------------------------------------------
// 外の様子の写し (ui タスク。★ 錠は取らない。数百 ms ごと)
// ---------------------------------------------------------------------------
static void host_of(const char *base, char *out, size_t cap) {
    const char *p = strstr(base, "://");
    p = (p != NULL) ? p + 3 : base;
    snprintf(out, cap, "%s", p);
}

static void collect_info(void) {
    stackee_menu_info_t *i = &s_m->info;
    memset(i, 0, sizeof(*i));
    // ---- Wi-Fi ----
    snprintf(i->wifi_state, sizeof(i->wifi_state), "%s", stackee_wifi_state_name());
    snprintf(i->ssid, sizeof(i->ssid), "%s", stackee_wifi_ssid());
    snprintf(i->ip, sizeof(i->ip), "%s", stackee_wifi_ip());
    i->rssi = stackee_wifi_rssi();
    i->saved_count = s_m->saved_count;
    for (int k = 0; k < s_m->saved_count && k < STACKEE_MENU_SAVED_MAX; k++) {
        snprintf(i->saved[k], sizeof(i->saved[k]), "%s", s_m->saved[k]);
    }
    // ★ 仕事の状態を先に読む。メインループは「名指しを始める → 仕事を OK にする」
    //   の順に書くので、OK が見えていれば target はもう今回のもの。
    i->job_state = atomic_load(&s_m->job_state);
    stackee_wifi_target(i->target, sizeof(i->target), &i->target_result,
                        &i->target_reason);
    i->scan_state = atomic_load(&s_m->scan_state);
    if (i->scan_state == STACKEE_MENU_JOB_OK) {
        i->scan_count = s_m->scan_count;
        for (int k = 0; k < s_m->scan_count && k < STACKEE_MENU_NETS_MAX; k++) {
            snprintf(i->nets[k].ssid, sizeof(i->nets[k].ssid), "%s", s_m->nets[k].ssid);
            i->nets[k].rssi = s_m->nets[k].rssi;
            i->nets[k].channel = s_m->nets[k].channel;
            i->nets[k].secure = s_m->nets[k].secure;
        }
    }
    snprintf(i->scan_error, sizeof(i->scan_error), "%s", s_m->scan_error);
    snprintf(i->job_error, sizeof(i->job_error), "%s", s_m->job_error);
    // ---- サーバー (トークンは触らない。見せるのはホスト部だけ) ----
    i->server_configured = stackee_http_configured();
    if (i->server_configured) {
        host_of(stackee_http_base(), i->server_host, sizeof(i->server_host));
    }
    // 受け箱・接続テスト・クリップ・直近の通信
    stackee_audio_menu_info(i);
    // ---- 本体 ----
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(i->version, sizeof(i->version), "%s", app ? app->version : "?");
    snprintf(i->profile, sizeof(i->profile), "%s", stackee_usb_profile());
    i->dest_selected = (stackee_hid_dest_selected() == STACKEE_HID_USB) ? 1 : 0;
    i->dest_effective = (stackee_hid_dest_effective() == STACKEE_HID_USB) ? 1 : 0;
    i->usb_mounted = stackee_usb_mounted();
    stackee_ble_stats_t ble;
    stackee_ble_stats(&ble);
    i->ble_connected = ble.connected;
    i->ble_advertising = ble.advertising;
    stackee_ble_peer_addr(i->ble_peer, sizeof(i->ble_peer));
    i->battery = s_m->battery;
    i->battery_present = stackee_board_battery_present();
    i->charging = s_m->charging;
    i->mic_ready = stackee_uac_usb_ready();
    i->volume = stackee_volume_percent();
}

// ---------------------------------------------------------------------------
// ui タスク
// ---------------------------------------------------------------------------
void stackee_menu_invalidate(void) {
    if (s_m != NULL) {
        s_m->shown_valid = false;
    }
}

static void drain_keys(void) {
    uint16_t item;
    while (xQueueReceive(s_m->keys, &item, 0) == pdTRUE) {
    }
}

stackee_menu_ui_t stackee_menu_ui_tick(const stackee_canvas_t *canvas,
                                       const stackee_font16_t *font,
                                       int battery, bool charging) {
    if (s_m == NULL) {
        return STACKEE_MENU_UI_NONE;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_m->battery = battery;
    s_m->charging = charging;
    bool changed = false;
    if (atomic_load(&s_open) && !s_m->core.open) {
        // 開いた。いつもルートの先頭から。前の走査・仕事の結果は見せない。
        // ★ キューは捨てない (Settings の直後にもう打たれたキーが入っている)。
        //   前の回の残りは閉じるときに捨ててある。
        stackee_menu_open(&s_m->core);
        atomic_store(&s_m->scan_state, STACKEE_MENU_JOB_IDLE);
        if (atomic_load(&s_m->job) == JOB_NONE) {
            atomic_store(&s_m->job_state, STACKEE_MENU_JOB_IDLE);
        }
        atomic_store(&s_m->list_req, true);
        s_m->shown_valid = false;
        s_m->info_at = now - INFO_EVERY_MS;
        ESP_LOGI(TAG, "開いた");
    }
    if (s_m->core.open) {
        if ((uint32_t)(now - s_m->info_at) >= INFO_EVERY_MS) {
            collect_info();
            s_m->info_at = now;
            changed = true;
        }
        uint16_t item;
        while (atomic_load(&s_open) && xQueueReceive(s_m->keys, &item, 0) == pdTRUE) {
            if (!changed) {
                collect_info();         // キーを捌く前にいまの様子を写す
                s_m->info_at = now;
            }
            stackee_menu_key(&s_m->core, &s_m->info, &s_m->view,
                             (stackee_menu_key_t)(item >> 8), (char)(item & 0xFF));
            changed = true;
        }
    }
    if (!atomic_load(&s_open)) {
        if (!s_m->core.open) {
            return STACKEE_MENU_UI_NONE;
        }
        // 閉じた。★ 入力中のパスワードも消える (stackee_menu_close)。
        stackee_menu_close(&s_m->core);
        drain_keys();
        s_m->shown_valid = false;
        s_m->closes++;
        ESP_LOGI(TAG, "閉じた");
        return STACKEE_MENU_UI_CLOSED;
    }
    if (!changed && s_m->shown_valid) {
        return STACKEE_MENU_UI_SHOWN;
    }
    stackee_menu_build(&s_m->core, &s_m->info, &s_m->view);
    if (s_m->shown_valid && memcmp(&s_m->view, &s_m->shown, sizeof(s_m->view)) == 0) {
        return STACKEE_MENU_UI_SHOWN;
    }
    int64_t t0 = esp_timer_get_time();
    stackee_draw_menu(canvas, font, &s_m->view);
    s_m->paint_us = (uint32_t)(esp_timer_get_time() - t0);
    if (s_m->paint_us > s_m->paint_max_us) {
        s_m->paint_max_us = s_m->paint_us;
    }
    s_m->shown = s_m->view;
    s_m->shown_valid = true;
    s_m->paints++;
    return STACKEE_MENU_UI_PAINTED;
}

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

static size_t put_str(char *buf, size_t cap, size_t at, const char *text) {
    at = put(buf, cap, at, "\"");
    for (const unsigned char *p = (const unsigned char *)text; *p && at + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            at = put(buf, cap, at, "\\%c", *p);
        } else if (*p < 0x20) {
            at = put(buf, cap, at, "\\u%04X", *p);
        } else {
            at = put(buf, cap, at, "%c", *p);
        }
    }
    return put(buf, cap, at, "\"");
}

static size_t reply_status(long id, const char *line, char *buf, size_t cap) {
    stackee_qmk_gate_stats_t gate;
    stackee_qmk_gate_stats(&gate);
    const stackee_menu_t *m = &s_m->core;
    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"open\":%d,\"shown\":%d,\"gate\":\"%s\","
                    "\"screen\":\"%s\",\"depth\":%d,\"sel\":%d,\"pass_len\":%d,"
                    "\"keys\":%lu,\"opens\":%lu,\"closes\":%lu,\"refused\":%lu,"
                    "\"refused_why\":\"%s\",\"blocker\":\"%s\",\"paints\":%lu,"
                    "\"paint_us\":%lu,\"paint_max_us\":%lu,\"queue_drops\":%lu,"
                    "\"gate_stats\":{\"opens\":%lu,\"closes\":%lu,\"timeouts\":%lu,"
                    "\"captured\":%lu,\"keys\":%lu,\"dropped\":%lu,\"swallowed\":%lu,"
                    "\"to_pc\":%lu},"
                    "\"job\":{\"busy\":%d,\"state\":%d,\"done\":%lu,\"failed\":%lu,"
                    "\"error\":\"%s\"},\"scan\":{\"state\":%d,\"n\":%d,\"runs\":%lu},"
                    "\"saved\":%d",
                    id, atomic_load(&s_open) ? 1 : 0, m->open ? 1 : 0,
                    stackee_qmk_gate_name(stackee_qmk_gate_state()),
                    stackee_menu_screen_names[m->screen], m->depth, m->sel,
                    m->pass_len, (unsigned long)m->keys, (unsigned long)m->opens,
                    (unsigned long)s_m->closes, (unsigned long)s_m->refused,
                    s_m->refused_why ? s_m->refused_why : "",
                    stackee_menu_blocker() ? stackee_menu_blocker() : "",
                    (unsigned long)s_m->paints, (unsigned long)s_m->paint_us,
                    (unsigned long)s_m->paint_max_us, (unsigned long)s_m->queue_drops,
                    (unsigned long)gate.opens, (unsigned long)gate.closes,
                    (unsigned long)gate.drain_timeouts, (unsigned long)gate.captured,
                    (unsigned long)gate.keys, (unsigned long)gate.dropped,
                    (unsigned long)gate.swallowed, (unsigned long)gate.to_pc,
                    atomic_load(&s_m->job) != JOB_NONE ? 1 : 0,
                    atomic_load(&s_m->job_state), (unsigned long)s_m->jobs_done,
                    (unsigned long)s_m->jobs_failed, s_m->job_error,
                    atomic_load(&s_m->scan_state), s_m->scan_count,
                    (unsigned long)s_m->scans, s_m->saved_count);
    at = stackee_audio_menu_json(buf, cap, at);
    // ★ いま画面に出ている行 (tools/menu_expected.py がこれを描いて
    //   lcd.crc y=28 h=292 と突き合わせる)。入り切らない行は落として
    //   "truncated":1 を付ける (rows=0 で行を省ける)。
    bool rows = stackee_console_bool(line, "rows", true);
    if (!s_m->shown_valid || !m->open) {
        return put(buf, cap, at, ",\"view\":null}");
    }
    const stackee_menu_view_t *v = &s_m->shown;
    at = put(buf, cap, at, ",\"view\":{\"title\":");
    at = put_str(buf, cap, at, v->title);
    at = put(buf, cap, at, ",\"footer\":");
    at = put_str(buf, cap, at, v->footer);
    at = put(buf, cap, at, ",\"selected\":%d,\"top\":%d,\"count\":%d,\"rows\":[",
             v->selected, v->top, v->count);
    bool truncated = false;
    for (int i = 0; rows && i < v->count; i++) {
        const stackee_menu_row_t *r = &v->rows[i];
        size_t need = strlen(r->label) * 2 + strlen(r->value) * 2 + 40;
        if (at + need + 40 >= cap) {
            truncated = true;
            break;
        }
        at = put(buf, cap, at, "%s[%d,%d,%d,", i ? "," : "", r->kind, r->action, r->arg);
        at = put_str(buf, cap, at, r->label);
        at = put(buf, cap, at, ",");
        at = put_str(buf, cap, at, r->value);
        at = put(buf, cap, at, "]");
    }
    return put(buf, cap, at, "],\"truncated\":%d}}", truncated ? 1 : 0);
}

static size_t reply_key(long id, const char *line, char *buf, size_t cap) {
    if (!atomic_load(&s_open)) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"closed\"}", id);
    }
    char name[12] = {0};
    char text[8] = {0};
    stackee_menu_key_t key;
    if (!stackee_console_str(line, "k", name, sizeof(name)) ||
        !stackee_menu_key_parse(name, &key)) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"badkey\"}", id);
    }
    char ch = 0;
    if (key == STACKEE_MENU_K_CHAR) {
        if (!stackee_console_str(line, "c", text, sizeof(text)) || text[0] == '\0') {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"nochar\"}", id);
        }
        ch = text[0];
    }
    push_key(key, ch);
    return put(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"k\":\"%s\"}", id,
               stackee_menu_key_name(key));
}

size_t stackee_menu_console(const char *cmd, const char *line, long id,
                            char *buf, size_t cap) {
    if (strncmp(cmd, "menu.", 5) != 0) {
        return 0;
    }
    if (s_m == NULL) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"not_ready\"}", id);
    }
    if (strcmp(cmd, "menu.status") == 0) {
        return reply_status(id, line, buf, cap);
    }
    if (strcmp(cmd, "menu.key") == 0) {
        return reply_key(id, line, buf, cap);
    }
    if (strcmp(cmd, "menu.open") == 0) {
        // Settings キーと同じ (処理中なら開かない)。
        if (atomic_load(&s_open) || try_open()) {
            return put(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"open\":1}", id);
        }
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\",\"why\":\"%s\"}", id,
                   s_m->refused_why ? s_m->refused_why : "");
    }
    if (strcmp(cmd, "menu.close") == 0) {
        atomic_store(&s_open, false);
        return put(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"open\":0}", id);
    }
    return 0;
}

// ---------------------------------------------------------------------------
void stackee_menu_start(void) {
    if (s_m != NULL) {
        return;
    }
    s_m = heap_caps_calloc(1, sizeof(*s_m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_m == NULL) {
        ESP_LOGE(TAG, "メニューの領域を取れない。Settings キーは効かない");
        return;
    }
    s_m->keys = xQueueCreateWithCaps(KEY_QUEUE_LEN, sizeof(uint16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_m->keys == NULL) {
        heap_caps_free(s_m);
        s_m = NULL;
        ESP_LOGE(TAG, "キーの列を作れない。Settings キーは効かない");
        return;
    }
    stackee_menu_init(&s_m->core, &OPS);
    ESP_LOGI(TAG, "設定メニューの用意ができた (%u B, PSRAM)", (unsigned)sizeof(*s_m));
}
