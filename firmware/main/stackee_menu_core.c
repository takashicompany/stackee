#include "stackee_menu_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "stackee_wifistore.h"

const char *const stackee_menu_screen_names[STACKEE_MENU_S_COUNT] = {
    "root", "wifi", "wifi_saved", "wifi_delete", "wifi_confirm", "wifi_scan",
    "wifi_password", "wifi_result", "server", "clips", "device",
};

// 伏せ字は最大この数まで並べる (あとは描くときに切れる)。
#define PASS_STARS_MAX 26

// ---------------------------------------------------------------------------
// 小道具
// ---------------------------------------------------------------------------
// UTF-8 の字の途中で切らずに写す。
static void copy_utf8(char *dst, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        // 継続バイト (10xxxxxx) の手前まで戻る。
        while (n > 0 && ((unsigned char)src[n] & 0xC0u) == 0x80u) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void fmt(char *dst, size_t cap, const char *format, ...)
    __attribute__((format(printf, 3, 4)));

static void fmt(char *dst, size_t cap, const char *format, ...) {
    char tmp[128];
    va_list args;
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    copy_utf8(dst, cap, tmp);
}

void stackee_menu_fmt_ago(uint32_t ms, char *out, size_t cap) {
    uint32_t s = ms / 1000u;
    if (s < 60u) {
        fmt(out, cap, "%lu秒前", (unsigned long)s);
    } else if (s < 3600u) {
        fmt(out, cap, "%lu分前", (unsigned long)(s / 60u));
    } else if (s < 86400u) {
        fmt(out, cap, "%lu時間前", (unsigned long)(s / 3600u));
    } else {
        fmt(out, cap, "%lu日前", (unsigned long)(s / 86400u));
    }
}

void stackee_menu_fmt_bytes(uint64_t bytes, char *out, size_t cap) {
    if (bytes < 1024u) {
        fmt(out, cap, "%lu B", (unsigned long)bytes);
    } else if (bytes < 1024u * 1024u) {
        fmt(out, cap, "%lu KB", (unsigned long)(bytes / 1024u));
    } else {
        // 小数 1 桁 (切り捨て)。
        uint64_t tenth = bytes * 10u / (1024u * 1024u);
        fmt(out, cap, "%lu.%lu MB", (unsigned long)(tenth / 10u),
            (unsigned long)(tenth % 10u));
    }
}

int stackee_menu_rssi_bars(int rssi) {
    if (rssi == 0) {
        return 0;
    }
    if (rssi >= -55) { return 4; }
    if (rssi >= -65) { return 3; }
    if (rssi >= -75) { return 2; }
    return 1;
}

static void fmt_bars(int rssi, char *out, size_t cap) {
    int bars = stackee_menu_rssi_bars(rssi);
    char tmp[32] = {0};
    for (int i = 0; i < 4; i++) {
        strcat(tmp, i < bars ? "■" : "□");
    }
    copy_utf8(out, cap, tmp);
}

static const char *wifi_state_jp(const stackee_menu_info_t *info) {
    const char *s = info->wifi_state;
    if (strcmp(s, "up") == 0) { return "接続中"; }
    if (strcmp(s, "off") == 0) { return info->saved_count == 0 ? "登録なし" : "止まっています"; }
    if (strcmp(s, "boot") == 0 || strcmp(s, "wait") == 0) { return "待機中"; }
    if (strcmp(s, "connect") == 0 || strcmp(s, "linkup") == 0) { return "接続しています"; }
    if (strcmp(s, "load") == 0 || strcmp(s, "radio") == 0 ||
        strncmp(s, "scan", 4) == 0) {
        return "探しています";
    }
    return s[0] ? s : "?";
}

static bool wifi_up(const stackee_menu_info_t *info) {
    return strcmp(info->wifi_state, "up") == 0;
}

static const char *reason_jp(int reason) {
    switch (reason) {
        case -1:
        case 201: return "見つからない";
        case -2: return "時間切れ";
        case 15:
        case 202:
        case 204: return "パスワード違い?";
        default: return NULL;
    }
}

static const char *inbox_jp(const stackee_menu_info_t *info) {
    if (info->inbox_held) { return "保留中 (メニュー中)"; }
    if (!info->inbox_on) { return "止めています"; }
    const char *p = info->inbox_phase;
    if (strcmp(p, "wait") == 0 || strcmp(p, "poll") == 0) { return "待っています"; }
    if (strcmp(p, "seq") == 0) { return "確かめています"; }
    if (strcmp(p, "say") == 0) { return "発話を扱っています"; }
    if (strcmp(p, "sleep") == 0) { return "休んでいます"; }
    if (strcmp(p, "off") == 0) { return "止めています"; }
    return p[0] ? p : "?";
}

static const char *clip_result_jp(const char *r) {
    if (strcmp(r, "ok") == 0) { return "成功"; }
    if (strcmp(r, "same") == 0) { return "変化なし"; }
    if (strcmp(r, "error") == 0) { return "失敗"; }
    if (strcmp(r, "aborted") == 0) { return "中断"; }
    if (strcmp(r, "404") == 0) { return "未対応"; }
    return r;
}

static const char *clip_phase_jp(const char *p) {
    if (strcmp(p, "idle") == 0) { return "待機中"; }
    if (strcmp(p, "scan") == 0) { return "確かめています"; }
    if (strcmp(p, "sleep") == 0) { return "休んでいます"; }
    if (strcmp(p, "aborting") == 0) { return "中断しています"; }
    if (strcmp(p, "clear") == 0) { return "消しています"; }
    if (strcmp(p, "list") == 0 || strcmp(p, "remove") == 0 ||
        strcmp(p, "fetch") == 0 || strcmp(p, "write") == 0 ||
        strcmp(p, "evict") == 0) {
        return "取り込み中";
    }
    return p[0] ? p : "?";
}

// ---------------------------------------------------------------------------
// 行を積む
// ---------------------------------------------------------------------------
static stackee_menu_row_t *add(stackee_menu_view_t *v, int kind, int action,
                               int arg, const char *label, const char *value) {
    if (v->count >= STACKEE_MENU_ROWS_MAX) {
        return NULL;
    }
    stackee_menu_row_t *r = &v->rows[v->count++];
    memset(r, 0, sizeof(*r));
    r->kind = (uint8_t)kind;
    r->action = (uint8_t)action;
    r->arg = (int8_t)arg;
    copy_utf8(r->label, sizeof(r->label), label);
    copy_utf8(r->value, sizeof(r->value), value ? value : "");
    return r;
}

static void info_row(stackee_menu_view_t *v, const char *label, const char *value) {
    add(v, STACKEE_MENU_ROW_INFO, STACKEE_MENU_A_NONE, 0, label, value);
}

static void note_row(stackee_menu_view_t *v, const char *text) {
    add(v, STACKEE_MENU_ROW_NOTE, STACKEE_MENU_A_NONE, 0, text, "");
}

static void sub_row(stackee_menu_view_t *v, int screen, const char *label,
                    const char *value) {
    add(v, STACKEE_MENU_ROW_SUB, STACKEE_MENU_A_OPEN, screen, label, value);
}

static void item_row(stackee_menu_view_t *v, int action, int arg,
                     const char *label, const char *value) {
    add(v, STACKEE_MENU_ROW_ITEM, action, arg, label, value);
}

static bool selectable(const stackee_menu_row_t *r) {
    return r->kind == STACKEE_MENU_ROW_ITEM || r->kind == STACKEE_MENU_ROW_SUB;
}

static int item_count(const stackee_menu_view_t *v) {
    int n = 0;
    for (int i = 0; i < v->count; i++) {
        if (selectable(&v->rows[i])) {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// 画面ごとの中身
// ---------------------------------------------------------------------------
#define FOOT_NAV  "↑↓選ぶ Enter決定 Esc戻る"

static void build_root(const stackee_menu_info_t *info, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "設定");
    fmt(v->footer, sizeof(v->footer), "↑↓選ぶ Enter決定 Esc閉じる");
    char value[STACKEE_MENU_TEXT_MAX];
    sub_row(v, STACKEE_MENU_S_WIFI, "Wi-Fi",
            wifi_up(info) ? info->ssid : wifi_state_jp(info));
    const char *server = !info->server_configured ? "未設定"
                         : !info->http_seen       ? "まだ通信なし"
                         : info->http_last_ok     ? "OK"
                                                  : "失敗";
    sub_row(v, STACKEE_MENU_S_SERVER, "サーバー", server);
    if (info->clips_ready) {
        fmt(value, sizeof(value), "%d 件", info->clips_count);
    } else {
        fmt(value, sizeof(value), "なし");
    }
    sub_row(v, STACKEE_MENU_S_CLIPS, "クリップ", value);
    sub_row(v, STACKEE_MENU_S_DEVICE, "本体",
            info->dest_selected == 1 ? "USB" : "BLE");
}

static void build_wifi(const stackee_menu_info_t *info, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "Wi-Fi");
    fmt(v->footer, sizeof(v->footer), FOOT_NAV);
    char value[STACKEE_MENU_TEXT_MAX];
    info_row(v, "状態", wifi_state_jp(info));
    info_row(v, "SSID", (wifi_up(info) && info->ssid[0]) ? info->ssid : "-");
    if (wifi_up(info) && info->rssi != 0) {
        char bars[24];
        fmt_bars(info->rssi, bars, sizeof(bars));
        fmt(value, sizeof(value), "%d dBm %s", info->rssi, bars);
    } else {
        fmt(value, sizeof(value), "-");
    }
    info_row(v, "電波", value);
    info_row(v, "IP", (wifi_up(info) && info->ip[0]) ? info->ip : "-");
    if (info->job_state == STACKEE_MENU_JOB_FAILED) {
        fmt(value, sizeof(value), "失敗 (%s)", info->job_error);
        note_row(v, value);
    }
    fmt(value, sizeof(value), "%d 件", info->saved_count);
    sub_row(v, STACKEE_MENU_S_WIFI_SAVED, "登録済みネットワーク", value);
    sub_row(v, STACKEE_MENU_S_WIFI_SCAN, "ネットワークを追加", "");
    sub_row(v, STACKEE_MENU_S_WIFI_DELETE, "登録を削除", "");
}

static void build_saved(const stackee_menu_info_t *info, stackee_menu_view_t *v,
                        bool deleting) {
    fmt(v->title, sizeof(v->title), deleting ? "登録を削除" : "登録済みネットワーク");
    fmt(v->footer, sizeof(v->footer),
        deleting ? "↑↓選ぶ Enter削除 Esc戻る" : "↑↓選ぶ Enter接続 Esc戻る");
    if (info->saved_count <= 0) {
        note_row(v, "登録がありません");
        return;
    }
    if (info->job_state == STACKEE_MENU_JOB_FAILED) {
        char value[STACKEE_MENU_TEXT_MAX];
        fmt(value, sizeof(value), "失敗 (%s)", info->job_error);
        note_row(v, value);
    }
    for (int i = 0; i < info->saved_count && i < STACKEE_MENU_SAVED_MAX; i++) {
        bool current = wifi_up(info) && strcmp(info->saved[i], info->ssid) == 0;
        item_row(v, deleting ? STACKEE_MENU_A_DELETE_PICK : STACKEE_MENU_A_SWITCH, i,
                 info->saved[i], current ? "接続中" : "");
    }
}

static void build_confirm(const stackee_menu_t *m, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "登録を削除");
    fmt(v->footer, sizeof(v->footer), FOOT_NAV);
    info_row(v, "ネットワーク", m->pick_ssid);
    note_row(v, "登録を削除しますか");
    item_row(v, STACKEE_MENU_A_DELETE_YES, 0, "削除する", "");
    item_row(v, STACKEE_MENU_A_BACK, 0, "やめる", "");
}

static void build_scan(const stackee_menu_info_t *info, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "ネットワークを追加");
    fmt(v->footer, sizeof(v->footer), FOOT_NAV);
    char value[STACKEE_MENU_TEXT_MAX];
    if (info->scan_state == STACKEE_MENU_JOB_RUNNING ||
        info->scan_state == STACKEE_MENU_JOB_IDLE) {
        note_row(v, "探しています…");
        return;
    }
    if (info->scan_state == STACKEE_MENU_JOB_FAILED) {
        fmt(value, sizeof(value), "探せません (%s)", info->scan_error);
        note_row(v, value);
        item_row(v, STACKEE_MENU_A_SCAN_AGAIN, 0, "もう一度探す", "");
        return;
    }
    if (info->scan_count <= 0) {
        note_row(v, "見つかりません");
    }
    for (int i = 0; i < info->scan_count && i < STACKEE_MENU_NETS_MAX; i++) {
        const stackee_menu_net_t *n = &info->nets[i];
        char bars[24];
        fmt_bars(n->rssi, bars, sizeof(bars));
        fmt(value, sizeof(value), "%s %s", bars, n->secure ? "鍵" : "開");
        item_row(v, STACKEE_MENU_A_SCAN_PICK, i, n->ssid, value);
    }
    item_row(v, STACKEE_MENU_A_SCAN_AGAIN, 0, "もう一度探す", "");
}

static void build_password(const stackee_menu_t *m, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "パスワード");
    fmt(v->footer, sizeof(v->footer), "Enter確定 BS消す Esc取消");
    char value[STACKEE_MENU_TEXT_MAX];
    info_row(v, "ネットワーク", m->pick_ssid);
    fmt(value, sizeof(value), "%d 文字", m->pass_len);
    info_row(v, "パスワード", value);
    // ★ 伏せ字だけ。中身は view に入れない。
    char stars[PASS_STARS_MAX + 2];
    int n = m->pass_len < PASS_STARS_MAX ? m->pass_len : PASS_STARS_MAX;
    memset(stars, '*', (size_t)n);
    stars[n] = '_';
    stars[n + 1] = '\0';
    item_row(v, STACKEE_MENU_A_PASSWORD, 0, stars, "");
    note_row(v, "8〜63 文字。Enterで接続");
}

static void build_result(const stackee_menu_t *m, const stackee_menu_info_t *info,
                         stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "Wi-Fi の接続");
    fmt(v->footer, sizeof(v->footer), "Enter戻る Esc戻る");
    char value[STACKEE_MENU_TEXT_MAX];
    info_row(v, "ネットワーク", m->pick_ssid);
    bool mine = strcmp(info->target, m->pick_ssid) == 0;
    if (info->job_state == STACKEE_MENU_JOB_RUNNING) {
        // まだメインループが受け取っていない (target は前の回の結果のことがある)。
        note_row(v, "接続しています…");
    } else if (info->job_state == STACKEE_MENU_JOB_FAILED) {
        fmt(value, sizeof(value), "保存できません (%s)", info->job_error);
        note_row(v, value);
    } else if (mine && info->target_result == 2) {
        note_row(v, "接続しました");
        info_row(v, "IP", info->ip[0] ? info->ip : "-");
    } else if (mine && info->target_result == 3) {
        note_row(v, "接続できませんでした");
        const char *why = reason_jp(info->target_reason);
        if (why != NULL) {
            fmt(value, sizeof(value), "%s", why);
        } else {
            fmt(value, sizeof(value), "コード %d", info->target_reason);
        }
        info_row(v, "理由", value);
    } else {
        note_row(v, "接続しています…");
    }
    item_row(v, STACKEE_MENU_A_BACK, 0, "戻る", "");
}

static void build_server(const stackee_menu_info_t *info, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "サーバー");
    fmt(v->footer, sizeof(v->footer), FOOT_NAV);
    char value[STACKEE_MENU_TEXT_MAX];
    if (!info->server_configured) {
        note_row(v, "STACKEE_TALK_URL 未設定");
        return;
    }
    // ホスト名は長いので 1 行ぶん使う (見出しの行 + 名前だけの行)。
    info_row(v, "接続先", "");
    info_row(v, info->server_host, "");
    if (!info->http_seen) {
        fmt(value, sizeof(value), "まだ");
    } else {
        char ago[24];
        stackee_menu_fmt_ago(info->http_last_ago_ms, ago, sizeof(ago));
        fmt(value, sizeof(value), "%s %s", info->http_last_ok ? "成功" : "失敗", ago);
    }
    info_row(v, "直近の通信", value);
    info_row(v, "受け箱", inbox_jp(info));
    switch (info->health_state) {
        case STACKEE_MENU_JOB_RUNNING:
            fmt(value, sizeof(value), "テスト中…");
            break;
        case STACKEE_MENU_JOB_OK:
            fmt(value, sizeof(value), "OK %lu ms", (unsigned long)info->health_ms);
            break;
        case STACKEE_MENU_JOB_FAILED:
            if (info->health_status > 0) {
                fmt(value, sizeof(value), "失敗 HTTP %d", info->health_status);
            } else {
                fmt(value, sizeof(value), "失敗");
            }
            break;
        default:
            value[0] = '\0';
            break;
    }
    item_row(v, STACKEE_MENU_A_HEALTH, 0, "接続テスト", value);
    if (info->health_state == STACKEE_MENU_JOB_FAILED && info->health_error[0]) {
        note_row(v, info->health_error);
    }
}

static void build_clips(const stackee_menu_info_t *info, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "クリップ");
    fmt(v->footer, sizeof(v->footer), FOOT_NAV);
    char value[STACKEE_MENU_TEXT_MAX];
    if (!info->clips_ready) {
        note_row(v, "クリップは使えません");
        return;
    }
    fmt(value, sizeof(value), "%d 件", info->clips_count);
    info_row(v, "件数", value);
    stackee_menu_fmt_bytes(info->clips_bytes, value, sizeof(value));
    info_row(v, "合計", value);
    stackee_menu_fmt_bytes(info->clips_free, value, sizeof(value));
    info_row(v, "FAT の空き", value);
    if (info->clips_synced) {
        stackee_menu_fmt_ago(info->clips_last_ago_ms, value, sizeof(value));
        info_row(v, "最後の取り込み", value);
        info_row(v, "結果", clip_result_jp(info->clips_result));
    } else {
        info_row(v, "最後の取り込み", "まだ");
    }
    info_row(v, "状態", clip_phase_jp(info->clips_phase));
    item_row(v, STACKEE_MENU_A_CLIP_AUTO, 0, "自動取得",
             info->clips_forced_off ? "OFF (設定)" : info->clips_auto ? "ON" : "OFF");
    item_row(v, STACKEE_MENU_A_CLIP_SYNC, 0, "今すぐ取り込む",
             info->clips_sync_requested ? "頼みました" : "");
}

static void build_device(const stackee_menu_info_t *info, stackee_menu_view_t *v) {
    fmt(v->title, sizeof(v->title), "本体");
    fmt(v->footer, sizeof(v->footer), "←→音量 Enter切替 Esc戻る");
    char value[STACKEE_MENU_TEXT_MAX];
    info_row(v, "ファーム", info->version[0] ? info->version : "?");
    if (info->dest_selected == 1 && info->dest_effective != 1) {
        fmt(value, sizeof(value), "USB (未接続→BLE)");     // 実際は BLE で送る
    } else {
        fmt(value, sizeof(value), "%s", info->dest_selected == 1 ? "USB" : "BLE");
    }
    item_row(v, STACKEE_MENU_A_DEST, 0, "送信先", value);
    if (info->ble_connected) {
        fmt(value, sizeof(value), "%s", info->ble_peer[0] ? info->ble_peer : "接続中");
    } else if (info->ble_advertising) {
        fmt(value, sizeof(value), "待ち受け中");
    } else {
        fmt(value, sizeof(value), "止まっています");
    }
    info_row(v, "Bluetooth", value);
    if (info->battery_present == 0) {
        fmt(value, sizeof(value), "なし");
    } else if (info->battery < 0) {
        fmt(value, sizeof(value), "?");
    } else {
        fmt(value, sizeof(value), "%d%%%s", info->battery,
            info->charging ? " 充電中" : "");
    }
    info_row(v, "電池", value);
    info_row(v, "USB マイク", info->mic_ready ? "使える" : "使えない");
    fmt(value, sizeof(value), "← %d%% →", info->volume);
    item_row(v, STACKEE_MENU_A_VOLUME, 0, "音量", value);
}

void stackee_menu_build(const stackee_menu_t *m, const stackee_menu_info_t *info,
                        stackee_menu_view_t *v) {
    memset(v, 0, sizeof(*v));
    v->screen = m->screen;
    v->selected = -1;
    if (m->notice[0]) {
        note_row(v, m->notice);
    }
    switch (m->screen) {
        case STACKEE_MENU_S_WIFI:          build_wifi(info, v); break;
        case STACKEE_MENU_S_WIFI_SAVED:    build_saved(info, v, false); break;
        case STACKEE_MENU_S_WIFI_DELETE:   build_saved(info, v, true); break;
        case STACKEE_MENU_S_WIFI_CONFIRM:  build_confirm(m, v); break;
        case STACKEE_MENU_S_WIFI_SCAN:     build_scan(info, v); break;
        case STACKEE_MENU_S_WIFI_PASSWORD: build_password(m, v); break;
        case STACKEE_MENU_S_WIFI_RESULT:   build_result(m, info, v); break;
        case STACKEE_MENU_S_SERVER:        build_server(info, v); break;
        case STACKEE_MENU_S_CLIPS:         build_clips(info, v); break;
        case STACKEE_MENU_S_DEVICE:        build_device(info, v); break;
        default:                           build_root(info, v); break;
    }
    // 選んでいる行。項目が減っていたら最後の項目に寄せる。
    int items = item_count(v);
    if (items > 0) {
        int want = m->sel < 0 ? 0 : (m->sel >= items ? items - 1 : m->sel);
        int n = 0;
        for (int i = 0; i < v->count; i++) {
            if (!selectable(&v->rows[i])) {
                continue;
            }
            if (n++ == want) {
                v->selected = i;
                break;
            }
        }
    }
    // 見えている範囲。選んでいる行が必ず入るように (時刻に依らず決まる)。
    int last = (v->selected >= 0) ? v->selected : 0;
    v->top = (last >= STACKEE_MENU_VISIBLE) ? last - STACKEE_MENU_VISIBLE + 1 : 0;
}

// ---------------------------------------------------------------------------
// 階層の出入り
// ---------------------------------------------------------------------------
static void wipe_password(stackee_menu_t *m) {
    // ★ 消すときは中身を全部 0 で埋める (長さだけ戻さない)。
    volatile char *p = m->password;
    for (size_t i = 0; i < sizeof(m->password); i++) {
        p[i] = 0;
    }
    m->pass_len = 0;
}

static void push(stackee_menu_t *m, int screen) {
    if (m->depth < STACKEE_MENU_DEPTH_MAX) {
        m->stack[m->depth].screen = (uint8_t)m->screen;
        m->stack[m->depth].sel = (int8_t)m->sel;
        m->depth++;
    }
    m->screen = screen;
    m->sel = 0;
}

static void pop(stackee_menu_t *m) {
    if (m->screen == STACKEE_MENU_S_WIFI_PASSWORD) {
        wipe_password(m);
    }
    if (m->depth <= 0) {
        return;
    }
    m->depth--;
    m->screen = m->stack[m->depth].screen;
    m->sel = m->stack[m->depth].sel;
}

// 結果の画面へ。戻り先は Wi-Fi の画面にする (走査・入力の画面へは戻さない)。
static void goto_result(stackee_menu_t *m) {
    if (m->screen == STACKEE_MENU_S_WIFI_PASSWORD) {
        wipe_password(m);
    }
    while (m->depth > 0 && m->stack[m->depth - 1].screen != STACKEE_MENU_S_WIFI) {
        m->depth--;
    }
    if (m->depth > 0) {
        // Wi-Fi の画面を「いまの画面」に戻してから、結果を積む。
        m->depth--;
        m->screen = m->stack[m->depth].screen;
        m->sel = m->stack[m->depth].sel;
    }
    push(m, STACKEE_MENU_S_WIFI_RESULT);
}

void stackee_menu_init(stackee_menu_t *m, const stackee_menu_ops_t *ops) {
    memset(m, 0, sizeof(*m));
    m->ops = ops;
    m->screen = STACKEE_MENU_S_ROOT;
}

void stackee_menu_open(stackee_menu_t *m) {
    wipe_password(m);
    m->open = true;
    m->depth = 0;
    m->screen = STACKEE_MENU_S_ROOT;
    m->sel = 0;
    m->notice[0] = '\0';
    m->pick_ssid[0] = '\0';
    m->opens++;
}

void stackee_menu_close(stackee_menu_t *m) {
    wipe_password(m);
    m->open = false;
    m->depth = 0;
    m->screen = STACKEE_MENU_S_ROOT;
    m->sel = 0;
    m->notice[0] = '\0';
}

static void set_notice(stackee_menu_t *m, const char *text) {
    copy_utf8(m->notice, sizeof(m->notice), text);
}

// ---------------------------------------------------------------------------
// キー
// ---------------------------------------------------------------------------
static void enter(stackee_menu_t *m, const stackee_menu_info_t *info,
                  const stackee_menu_row_t *row) {
    const stackee_menu_ops_t *ops = m->ops;
    char text[STACKEE_MENU_TEXT_MAX];
    switch (row->action) {
        case STACKEE_MENU_A_OPEN:
            push(m, row->arg);
            if (row->arg == STACKEE_MENU_S_WIFI_SCAN && ops->wifi_scan != NULL) {
                ops->wifi_scan();
            }
            return;
        case STACKEE_MENU_A_SWITCH: {
            int i = row->arg;
            if (i < 0 || i >= info->saved_count) {
                return;
            }
            if (wifi_up(info) && strcmp(info->saved[i], info->ssid) == 0) {
                set_notice(m, "もう接続しています");
                return;
            }
            copy_utf8(m->pick_ssid, sizeof(m->pick_ssid), info->saved[i]);
            if (ops->wifi_switch == NULL || !ops->wifi_switch(m->pick_ssid)) {
                set_notice(m, "いまは切り替えられません");
                return;
            }
            goto_result(m);
            return;
        }
        case STACKEE_MENU_A_DELETE_PICK: {
            int i = row->arg;
            if (i < 0 || i >= info->saved_count) {
                return;
            }
            copy_utf8(m->pick_ssid, sizeof(m->pick_ssid), info->saved[i]);
            push(m, STACKEE_MENU_S_WIFI_CONFIRM);
            m->sel = 1;             // ★ 既定は「やめる」
            return;
        }
        case STACKEE_MENU_A_DELETE_YES:
            if (ops->wifi_remove == NULL || !ops->wifi_remove(m->pick_ssid)) {
                pop(m);
                set_notice(m, "いまは削除できません");
                return;
            }
            fmt(text, sizeof(text), "削除しました: %s", m->pick_ssid);
            pop(m);
            set_notice(m, text);
            return;
        case STACKEE_MENU_A_BACK:
            pop(m);
            return;
        case STACKEE_MENU_A_SCAN_AGAIN:
            if (ops->wifi_scan != NULL) {
                ops->wifi_scan();
            }
            m->sel = 0;
            return;
        case STACKEE_MENU_A_SCAN_PICK: {
            int i = row->arg;
            if (i < 0 || i >= info->scan_count) {
                return;
            }
            const stackee_menu_net_t *n = &info->nets[i];
            copy_utf8(m->pick_ssid, sizeof(m->pick_ssid), n->ssid);
            m->pick_channel = n->channel;
            m->pick_secure = n->secure;
            if (n->secure) {
                push(m, STACKEE_MENU_S_WIFI_PASSWORD);
                wipe_password(m);
                return;
            }
            // オープンな AP はパスワード無しで登録して繋ぐ。
            if (ops->wifi_add == NULL || !ops->wifi_add(m->pick_ssid, "", m->pick_channel)) {
                set_notice(m, "いまは登録できません");
                return;
            }
            goto_result(m);
            return;
        }
        case STACKEE_MENU_A_HEALTH:
            if (!info->server_configured) {
                return;
            }
            if (info->health_state == STACKEE_MENU_JOB_RUNNING) {
                return;             // まだ前のが終わっていない
            }
            if (ops->health_test != NULL) {
                ops->health_test();
            }
            return;
        case STACKEE_MENU_A_CLIP_AUTO:
            if (info->clips_forced_off) {
                set_notice(m, "settings.toml で OFF です");
                return;
            }
            if (ops->clip_auto_toggle != NULL) {
                ops->clip_auto_toggle();
            }
            return;
        case STACKEE_MENU_A_CLIP_SYNC:
            if (ops->clip_sync != NULL) {
                ops->clip_sync();
            }
            return;
        case STACKEE_MENU_A_DEST:
            if (ops->hid_toggle != NULL) {
                ops->hid_toggle();
            }
            return;
        case STACKEE_MENU_A_VOLUME:
            set_notice(m, "← → で変えます");
            return;
        default:
            return;
    }
}

static void submit_password(stackee_menu_t *m) {
    const char *bad = stackee_wifi_validate(m->pick_ssid, m->password, m->pick_channel);
    if (bad != NULL || m->pass_len < STACKEE_WIFI_PASS_MIN) {
        set_notice(m, "8〜63 文字にしてください");
        return;
    }
    if (m->ops->wifi_add == NULL ||
        !m->ops->wifi_add(m->pick_ssid, m->password, m->pick_channel)) {
        set_notice(m, "いまは登録できません");
        return;
    }
    goto_result(m);         // ★ ここでパスワードを消す
}

bool stackee_menu_key(stackee_menu_t *m, const stackee_menu_info_t *info,
                      stackee_menu_view_t *view, stackee_menu_key_t key, char ch) {
    if (!m->open || key == STACKEE_MENU_K_NONE) {
        return false;
    }
    m->keys++;
    m->notice[0] = '\0';
    stackee_menu_build(m, info, view);

    // ---- パスワードの入力中 ----
    if (m->screen == STACKEE_MENU_S_WIFI_PASSWORD) {
        switch (key) {
            case STACKEE_MENU_K_CHAR:
                if ((unsigned char)ch >= 0x20 && (unsigned char)ch <= 0x7E &&
                    m->pass_len < STACKEE_MENU_PASS_MAX - 1) {
                    m->password[m->pass_len++] = ch;
                    m->password[m->pass_len] = '\0';
                }
                return true;
            case STACKEE_MENU_K_BS:
                // ★ 空のときの Backspace は何もしない (うっかり抜けない)。
                if (m->pass_len > 0) {
                    m->password[--m->pass_len] = '\0';
                }
                return true;
            case STACKEE_MENU_K_ENTER:
                submit_password(m);
                return true;
            case STACKEE_MENU_K_ESC:
                pop(m);             // 取消 (パスワードは消える)
                return true;
            default:
                return true;        // 矢印は使わない
        }
    }

    int items = item_count(view);
    const stackee_menu_row_t *row =
        (view->selected >= 0) ? &view->rows[view->selected] : NULL;
    switch (key) {
        case STACKEE_MENU_K_UP:
            if (m->sel > 0) {
                m->sel--;
            }
            if (m->sel >= items) {
                m->sel = items > 0 ? items - 1 : 0;
            }
            return true;
        case STACKEE_MENU_K_DOWN:
            if (m->sel < items - 1) {
                m->sel++;
            }
            return true;
        case STACKEE_MENU_K_LEFT:
        case STACKEE_MENU_K_RIGHT:
            if (row != NULL && row->action == STACKEE_MENU_A_VOLUME &&
                m->ops->volume_step != NULL) {
                m->ops->volume_step(key == STACKEE_MENU_K_RIGHT ? 5 : -5);
            }
            return true;
        case STACKEE_MENU_K_ENTER:
            if (row != NULL) {
                enter(m, info, row);
            }
            return true;
        case STACKEE_MENU_K_ESC:
            if (m->depth == 0) {
                if (m->ops->close != NULL) {
                    m->ops->close();
                }
                return true;
            }
            pop(m);
            return true;
        case STACKEE_MENU_K_BS:
            if (m->depth > 0) {
                pop(m);
            }
            return true;
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// HID → メニューのキー (JIS 配列)
// ---------------------------------------------------------------------------
#define MOD_SHIFT_MASK 0x22u     // LShift | RShift
#define MOD_OTHER_MASK 0xDDu     // Ctrl / Alt / GUI (左右)

// 記号キー (JIS)。{使用番号, 素, Shift}。0 は「出ない」。
static const struct {
    uint8_t usage;
    char    plain;
    char    shifted;
} JIS_KEYS[] = {
    {0x1E, '1', '!'}, {0x1F, '2', '"'}, {0x20, '3', '#'}, {0x21, '4', '$'},
    {0x22, '5', '%'}, {0x23, '6', '&'}, {0x24, '7', '\''}, {0x25, '8', '('},
    {0x26, '9', ')'}, {0x27, '0', 0},
    {0x2C, ' ', ' '},
    {0x2D, '-', '='}, {0x2E, '^', '~'}, {0x2F, '@', '`'}, {0x30, '[', '{'},
    {0x31, ']', '}'}, {0x32, ']', '}'}, {0x33, ';', '+'}, {0x34, ':', '*'},
    {0x36, ',', '<'}, {0x37, '.', '>'}, {0x38, '/', '?'},
    {0x87, '\\', '_'},          // ろ
    {0x89, '\\', '|'},          // ¥ (ASCII では逆斜線)
};

bool stackee_menu_key_from_hid(uint8_t usage, uint8_t mods,
                               stackee_menu_key_t *key, char *ch) {
    bool shift = (mods & MOD_SHIFT_MASK) != 0;
    *ch = 0;
    switch (usage) {
        case 0x52: *key = STACKEE_MENU_K_UP; return true;
        case 0x51: *key = STACKEE_MENU_K_DOWN; return true;
        case 0x50: *key = STACKEE_MENU_K_LEFT; return true;
        case 0x4F: *key = STACKEE_MENU_K_RIGHT; return true;
        case 0x28:
        case 0x58: *key = STACKEE_MENU_K_ENTER; return true;
        case 0x29: *key = STACKEE_MENU_K_ESC; return true;
        case 0x2A:
        case 0x4C: *key = STACKEE_MENU_K_BS; return true;
        case 0x2B:  // Tab は下、Shift+Tab は上 (Tab は既定配列の親指にある)
            *key = shift ? STACKEE_MENU_K_UP : STACKEE_MENU_K_DOWN;
            return true;
        default:
            break;
    }
    if ((mods & MOD_OTHER_MASK) != 0) {
        return false;           // Ctrl / Alt / Cmd つきの文字は使わない
    }
    if (usage >= 0x04 && usage <= 0x1D) {
        *key = STACKEE_MENU_K_CHAR;
        *ch = (char)((shift ? 'A' : 'a') + (usage - 0x04));
        return true;
    }
    for (size_t i = 0; i < sizeof(JIS_KEYS) / sizeof(JIS_KEYS[0]); i++) {
        if (JIS_KEYS[i].usage != usage) {
            continue;
        }
        char c = shift ? JIS_KEYS[i].shifted : JIS_KEYS[i].plain;
        if (c == 0) {
            return false;
        }
        *key = STACKEE_MENU_K_CHAR;
        *ch = c;
        return true;
    }
    return false;
}

static const char *const KEY_NAMES[] = {
    "none", "up", "down", "left", "right", "enter", "esc", "bs", "char",
};

bool stackee_menu_key_parse(const char *name, stackee_menu_key_t *out) {
    for (int i = 1; i < (int)(sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0])); i++) {
        if (strcmp(name, KEY_NAMES[i]) == 0) {
            *out = (stackee_menu_key_t)i;
            return true;
        }
    }
    return false;
}

const char *stackee_menu_key_name(stackee_menu_key_t key) {
    if ((int)key < 0 || (int)key >= (int)(sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]))) {
        return "?";
    }
    return KEY_NAMES[key];
}
