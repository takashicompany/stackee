#include "stackee_wifistore.h"

#include <stdio.h>
#include <string.h>

#include "stackee_jsonlite.h"

// CircuitPython の ScannedNetworks.c と同じ走査順 (+ 14ch)。
static const int SCAN_PATTERN[] = {6, 1, 11, 3, 9, 13, 2, 4, 8, 12, 5, 7, 10, 14};
#define SCAN_PATTERN_N ((int)(sizeof(SCAN_PATTERN) / sizeof(SCAN_PATTERN[0])))

#define SCAN_SETTLE_MS          300     // 1..11ch (アクティブ走査)
#define SCAN_SETTLE_PASSIVE_MS  800     // 12ch 以上 (パッシブは滞在が 3 倍)
#define PASSIVE_FROM_CH         12

// 制御文字 (0x00..0x1F と DEL) は SSID にもパスワードにも入らない。
// ★ 入れると JSON へ書くときに 1 文字が \uXXXX の 6 バイトに膨らみ、
//   登録簿の全文が入れ物からあふれる原因になる。
static bool has_control(const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return true;
        }
    }
    return false;
}

const char *stackee_wifi_validate(const char *ssid, const char *password,
                                  int channel) {
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32 || has_control(ssid)) {
        return "bad_ssid";
    }
    if (password == NULL || has_control(password)) {
        return "bad_password";
    }
    size_t plen = strlen(password);
    // 空文字 = オープンな AP。それ以外は 8..63 文字。
    if (plen != 0 && (plen < STACKEE_WIFI_PASS_MIN || plen > 63)) {
        return "bad_password";
    }
    if (channel != 0 && (channel < STACKEE_WIFI_CH_MIN || channel > STACKEE_WIFI_CH_MAX)) {
        return "bad_channel";
    }
    return NULL;
}

int stackee_wifi_find(const stackee_wifi_list_t *list, const char *ssid) {
    if (list == NULL || ssid == NULL) {
        return -1;
    }
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->nets[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

// 配列の中の次の 1 要素 ({...}) の先頭を返す。*end_out は閉じ括弧の次。
// 配列の終わり (']') か、閉じていなければ NULL。
static const char *next_object(const char *at, const char **end_out) {
    bool in_string = false;
    // 開き括弧を探す。
    for (; *at; at++) {
        if (in_string) {
            if (*at == '\\' && at[1]) { at++; }
            else if (*at == '"')      { in_string = false; }
            continue;
        }
        if (*at == '"') { in_string = true; continue; }
        if (*at == '{') { break; }
        if (*at == ']') { return NULL; }
    }
    if (*at != '{') {
        return NULL;
    }
    const char *start = at;
    int depth = 0;
    in_string = false;
    for (; *at; at++) {
        if (in_string) {
            if (*at == '\\' && at[1]) { at++; }
            else if (*at == '"')      { in_string = false; }
            continue;
        }
        if (*at == '"')      { in_string = true; }
        else if (*at == '{') { depth++; }
        else if (*at == '}') {
            if (--depth == 0) {
                *end_out = at + 1;
                return start;
            }
        }
    }
    return NULL;
}

bool stackee_wifi_parse(const char *json, stackee_wifi_list_t *out,
                        const char **note) {
    memset(out, 0, sizeof(*out));
    if (note) { *note = NULL; }
    if (json == NULL || json[0] == '\0') {
        if (note) { *note = "corrupt"; }
        return false;
    }
    const char *array = NULL;
    size_t array_len = 0;
    if (!stackee_json_raw(json, "networks", &array, &array_len) || array[0] != '[') {
        if (note) { *note = "corrupt"; }
        return false;
    }
    const char *at = array + 1;
    const char *limit = array + array_len;
    while (at < limit && out->count < STACKEE_WIFI_MAX_NETWORKS) {
        const char *end = NULL;
        const char *obj = next_object(at, &end);
        if (obj == NULL || obj >= limit) {
            break;
        }
        at = end;
        // ★ いったん**大きめの入れ物**へ読む。net.ssid (33 B) へ直に読むと
        //   33 バイトの SSID が 32 バイトに切り詰められて「正しい」と
        //   通ってしまう (長すぎる登録を黙って受け入れることになる)。
        char ssid_raw[96] = {0};
        char pass_raw[128] = {0};
        if (!stackee_json_str(obj, "ssid", ssid_raw, sizeof(ssid_raw))) {
            continue;                       // 読めない要素は黙って捨てる
        }
        if (!stackee_json_str(obj, "password", pass_raw, sizeof(pass_raw))) {
            pass_raw[0] = '\0';
        }
        long ch = 0;
        bool dummy = false;
        if (stackee_json_bool(obj, "channel", &dummy)) {
            ch = 0;                          // bool は int の仲間ではない
        } else if (!stackee_json_int(obj, "channel", &ch)) {
            ch = 0;
        }
        if (stackee_wifi_validate(ssid_raw, pass_raw, (int)ch) != NULL) {
            continue;
        }
        if (stackee_wifi_find(out, ssid_raw) >= 0) {
            continue;                        // 同じ SSID は最初の 1 件だけ
        }
        stackee_wifi_net_t net;
        memset(&net, 0, sizeof(net));
        // 長さは validate が保証している (SSID 32 B / パスワード 63 B 以下)。
        // それでも切り詰めが起きない書き方にしておく。
        size_t slen = strlen(ssid_raw);
        size_t plen = strlen(pass_raw);
        if (slen >= sizeof(net.ssid) || plen >= sizeof(net.password)) {
            continue;
        }
        memcpy(net.ssid, ssid_raw, slen + 1);
        memcpy(net.password, pass_raw, plen + 1);
        net.channel = (int)ch;
        out->nets[out->count++] = net;
    }
    return true;
}

// ★ snprintf の戻り値 (「入れたかった長さ」) を足し込まないこと。
//   at が cap を追い越すと out + at が入れ物の外を指し、cap - at が
//   巨大な size_t に化けて、そこから先へ書き放題になる。
//   ここでは「入り切らなかったら足さずに印を立てる」形にしてある。
typedef struct {
    char  *out;
    size_t cap;
    size_t at;
    bool   full;
} dump_buf_t;

static void dump_raw(dump_buf_t *b, const char *text, size_t len) {
    if (b->full) {
        return;
    }
    if (b->at + len + 1 > b->cap) {       // NUL のぶんを残す
        b->full = true;
        return;
    }
    memcpy(b->out + b->at, text, len);
    b->at += len;
    b->out[b->at] = '\0';
}

static void dump_text(dump_buf_t *b, const char *text) {
    dump_raw(b, text, strlen(text));
}

static void dump_escaped(dump_buf_t *b, const char *text) {
    for (const char *p = text; *p && !b->full; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            char pair[2] = {'\\', (char)c};
            dump_raw(b, pair, 2);
        } else if (c < 0x20 || c == 0x7F) {
            char esc[7];
            int n = snprintf(esc, sizeof(esc), "\\u%04X", c);
            dump_raw(b, esc, (size_t)n);
        } else {
            dump_raw(b, (const char *)&c, 1);
        }
    }
}

size_t stackee_wifi_public_json(const stackee_wifi_list_t *list,
                                char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    dump_buf_t b = {out, cap, 0, false};
    dump_text(&b, "[");
    for (int i = 0; i < list->count; i++) {
        if (i > 0) {
            dump_text(&b, ",");
        }
        dump_text(&b, "{\"ssid\":\"");
        dump_escaped(&b, list->nets[i].ssid);
        dump_text(&b, "\",\"channel\":");
        if (list->nets[i].channel > 0) {
            char num[16];
            int n = snprintf(num, sizeof(num), "%d", list->nets[i].channel);
            dump_raw(&b, num, (size_t)n);
        } else {
            dump_text(&b, "null");
        }
        // ★ ここに password そのものは絶対に書かない。有無だけ。
        dump_text(&b, list->nets[i].password[0] ? ",\"has_password\":true}"
                                                : ",\"has_password\":false}");
    }
    dump_text(&b, "]");
    if (b.full) {
        out[0] = '\0';
        return 0;
    }
    return b.at;
}

size_t stackee_wifi_dumps(const stackee_wifi_list_t *list, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    dump_buf_t b = {out, cap, 0, false};
    dump_text(&b, "{\"v\":1,\"networks\":[");
    for (int i = 0; i < list->count; i++) {
        if (i > 0) {
            dump_text(&b, ",");
        }
        dump_text(&b, "{\"ssid\":\"");
        dump_escaped(&b, list->nets[i].ssid);
        dump_text(&b, "\",\"password\":\"");
        dump_escaped(&b, list->nets[i].password);
        dump_text(&b, "\",\"channel\":");
        if (list->nets[i].channel > 0) {
            char num[16];
            int n = snprintf(num, sizeof(num), "%d}", list->nets[i].channel);
            dump_raw(&b, num, (size_t)n);
        } else {
            dump_text(&b, "null}");
        }
    }
    dump_text(&b, "]}");
    // ★ 途中で入り切らなかったものを返さない。半端な JSON を保存すると
    //   登録簿を丸ごと失う。呼び手は 0 を「書けなかった」と扱うこと。
    if (b.full) {
        out[0] = '\0';
        return 0;
    }
    return b.at;
}

const char *stackee_wifi_upsert(stackee_wifi_list_t *list, const char *ssid,
                                const char *password, int channel) {
    const char *bad = stackee_wifi_validate(ssid, password, channel);
    if (bad != NULL) {
        return bad;
    }
    int at = stackee_wifi_find(list, ssid);
    if (at < 0) {
        if (list->count >= STACKEE_WIFI_MAX_NETWORKS) {
            return "full";
        }
        at = list->count++;
    }
    stackee_wifi_net_t *net = &list->nets[at];
    memset(net, 0, sizeof(*net));
    snprintf(net->ssid, sizeof(net->ssid), "%s", ssid);
    snprintf(net->password, sizeof(net->password), "%s", password);
    net->channel = channel;
    return NULL;
}

const char *stackee_wifi_remove_ssid(stackee_wifi_list_t *list, const char *ssid) {
    int at = stackee_wifi_find(list, ssid);
    if (at < 0) {
        return "not_found";
    }
    for (int i = at; i + 1 < list->count; i++) {
        list->nets[i] = list->nets[i + 1];
    }
    list->count--;
    memset(&list->nets[list->count], 0, sizeof(list->nets[list->count]));
    return NULL;
}

bool stackee_wifi_pick(const stackee_wifi_seen_t *seen, int seen_count,
                       const stackee_wifi_list_t *list,
                       stackee_wifi_pick_t *out) {
    int best_rssi = 0;
    int best_order = 0;
    int best_seen = -1;
    bool have = false;
    for (int i = 0; i < seen_count; i++) {
        int at = stackee_wifi_find(list, seen[i].ssid);
        if (at < 0) {
            continue;
        }
        int rssi = seen[i].rssi;
        if (!have || rssi > best_rssi || (rssi == best_rssi && at < best_order)) {
            have = true;
            best_rssi = rssi;
            best_order = at;
            best_seen = i;
        }
    }
    if (!have) {
        return false;
    }
    const stackee_wifi_net_t *net = &list->nets[best_order];
    memset(out, 0, sizeof(*out));
    snprintf(out->ssid, sizeof(out->ssid), "%s", net->ssid);
    snprintf(out->password, sizeof(out->password), "%s", net->password);
    out->channel = net->channel > 0 ? net->channel : seen[best_seen].channel;
    out->rssi = best_rssi;
    out->index = best_order;
    return true;
}

int stackee_wifi_seen_channel(const stackee_wifi_seen_t *seen, int seen_count,
                              const char *ssid) {
    int best_rssi = 0;
    int best_ch = 0;
    bool have = false;
    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen[i].ssid, ssid) != 0 || seen[i].channel <= 0) {
            continue;
        }
        if (!have || seen[i].rssi > best_rssi) {
            have = true;
            best_rssi = seen[i].rssi;
            best_ch = seen[i].channel;
        }
    }
    return best_ch;
}

static bool contains(const int *list, int n, int value) {
    for (int i = 0; i < n; i++) {
        if (list[i] == value) {
            return true;
        }
    }
    return false;
}

int stackee_wifi_channel_order(const stackee_wifi_list_t *list, int last_channel,
                               int *out, int cap) {
    int n = 0;
    if (list != NULL) {
        for (int i = 0; i < list->count && n < cap; i++) {
            int ch = list->nets[i].channel;
            if (ch > 0 && !contains(out, n, ch)) {
                out[n++] = ch;
            }
        }
    }
    if (last_channel > 0 && n < cap && !contains(out, n, last_channel)) {
        out[n++] = last_channel;
    }
    for (int i = 0; i < SCAN_PATTERN_N && n < cap; i++) {
        if (!contains(out, n, SCAN_PATTERN[i])) {
            out[n++] = SCAN_PATTERN[i];
        }
    }
    return n;
}

int stackee_wifi_settle_ms(int channel) {
    if (channel <= 0) {
        return SCAN_SETTLE_MS;
    }
    return (channel >= PASSIVE_FROM_CH) ? SCAN_SETTLE_PASSIVE_MS : SCAN_SETTLE_MS;
}
