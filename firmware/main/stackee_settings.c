#include "stackee_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char key[STACKEE_SETTINGS_KEY_MAX];
    char value[STACKEE_SETTINGS_VALUE_MAX];
} entry_t;

static entry_t s_entries[STACKEE_SETTINGS_MAX_KEYS];
static int     s_count;

static bool key_char(char c) {
    return c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

// TOML の基本文字列から中身を取り出す (stackee_console.py の unquote と同じ範囲)。
static void unquote(const char *raw, size_t len, char *out, size_t cap) {
    size_t pos = 0;
    if (len >= 2 && raw[0] == '"' && raw[len - 1] == '"') {
        raw++;
        len -= 2;
        for (size_t i = 0; i < len && pos + 1 < cap; i++) {
            char c = raw[i];
            if (c != '\\' || i + 1 >= len) {
                out[pos++] = c;
                continue;
            }
            char next = raw[++i];
            switch (next) {
                case 'n': out[pos++] = '\n'; break;
                case 't': out[pos++] = '\t'; break;
                case 'r': out[pos++] = '\r'; break;
                case 'u': {
                    // \uXXXX。設定に日本語は入れない前提なので ASCII だけ扱う。
                    if (i + 4 < len) {
                        char hex[5] = {raw[i + 1], raw[i + 2], raw[i + 3], raw[i + 4], 0};
                        long v = strtol(hex, NULL, 16);
                        i += 4;
                        if (v > 0 && v < 0x80) {
                            out[pos++] = (char)v;
                        }
                    }
                    break;
                }
                default: out[pos++] = next; break;
            }
        }
    } else {
        for (size_t i = 0; i < len && pos + 1 < cap; i++) {
            out[pos++] = raw[i];
        }
    }
    out[pos] = '\0';
}

int stackee_settings_load_text(const char *text) {
    s_count = 0;
    if (text == NULL) {
        return 0;
    }
    const char *line = text;
    while (*line && s_count < STACKEE_SETTINGS_MAX_KEYS) {
        const char *end = strchr(line, '\n');
        size_t len = (end == NULL) ? strlen(line) : (size_t)(end - line);
        // 行末の CR を落とす。
        while (len > 0 && (line[len - 1] == '\r')) {
            len--;
        }
        const char *body = line;
        size_t blen = len;
        while (blen > 0 && (*body == ' ' || *body == '\t')) {
            body++;
            blen--;
        }
        if (blen > 0 && *body != '#') {
            const char *eq = memchr(body, '=', blen);
            if (eq != NULL) {
                size_t klen = (size_t)(eq - body);
                while (klen > 0 && (body[klen - 1] == ' ' || body[klen - 1] == '\t')) {
                    klen--;
                }
                bool ok = klen > 0 && klen < STACKEE_SETTINGS_KEY_MAX &&
                          !(body[0] >= '0' && body[0] <= '9');
                for (size_t i = 0; ok && i < klen; i++) {
                    ok = key_char(body[i]);
                }
                if (ok) {
                    const char *value = eq + 1;
                    size_t vlen = (size_t)((body + blen) - value);
                    while (vlen > 0 && (*value == ' ' || *value == '\t')) {
                        value++;
                        vlen--;
                    }
                    while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\t')) {
                        vlen--;
                    }
                    entry_t *slot = &s_entries[s_count];
                    memcpy(slot->key, body, klen);
                    slot->key[klen] = '\0';
                    unquote(value, vlen, slot->value, sizeof(slot->value));
                    // 同じキーが 2 度出たら後勝ち (CircuitPython と同じ)。
                    int found = -1;
                    for (int i = 0; i < s_count; i++) {
                        if (strcmp(s_entries[i].key, slot->key) == 0) {
                            found = i;
                            break;
                        }
                    }
                    if (found >= 0) {
                        s_entries[found] = *slot;
                    } else {
                        s_count++;
                    }
                }
            }
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    return s_count;
}

const char *stackee_settings_get(const char *key) {
    if (key == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_entries[i].key, key) == 0) {
            return s_entries[i].value;
        }
    }
    return NULL;
}

bool stackee_settings_has(const char *key) {
    const char *value = stackee_settings_get(key);
    return value != NULL && value[0] != '\0';
}

long stackee_settings_int(const char *key, long fallback) {
    const char *value = stackee_settings_get(key);
    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    char *end = NULL;
    long out = strtol(value, &end, 10);
    return (end == value) ? fallback : out;
}

int stackee_settings_count(void) {
    return s_count;
}

const char *stackee_settings_key_at(int index) {
    if (index < 0 || index >= s_count) {
        return NULL;
    }
    return s_entries[index].key;
}

// ---------------------------------------------------------------------------
// 段階 4: settings.get / settings.raw / settings.set
// ---------------------------------------------------------------------------
// ★ 並びは現行 firmware/kmk/stackee_console.py の ALLOWED_KEYS /
//   LEGACY_WIFI_KEYS / SECRET_KEYS / REPORT_KEYS と 1 対 1。
//
// ★ CIRCUITPY_WIFI_SSID は **ALLOWED に入れない**。書くと CircuitPython の
//   supervisor が起動中に connect() を 4 回呼び、AP 不在の場所で起動が
//   19 秒延びる (wifi_autoconnect_design.md §1.1 の実測)。C 版は
//   supervisor を通らないが、CircuitPython に戻したときに刺さるので
//   同じ禁止を続ける。
static const char *const ALLOWED_KEYS[] = {
    "STACKEE_HOST",
    "STACKEE_PORT",
    // ★ C 版で増えたぶん。会話の相手はこの 2 つで決まる (段階 3)。
    "STACKEE_TALK_URL",
    "STACKEE_TALK_TOKEN",
    NULL,
};

static const char *const SECRET_KEYS[] = {
    "STACKEE_WIFI_PASSWORD",
    "CIRCUITPY_WIFI_PASSWORD",
    "CIRCUITPY_WEB_API_PASSWORD",
    "STACKEE_TALK_TOKEN",
    NULL,
};

static const char *const REPORT_KEYS[] = {
    "STACKEE_HOST",
    "STACKEE_PORT",
    "STACKEE_TALK_URL",
    "STACKEE_TALK_TOKEN",
    // 廃止した Wi-Fi キー。書き換えは拒むが、古い settings.toml に残って
    // いたらページが「移行してください」と言えるように見せる。
    "STACKEE_WIFI_SSID",
    "STACKEE_WIFI_PASSWORD",
    "STACKEE_WIFI_CHANNEL",
    "STACKEE_WIFI_RETRY_S",
    "STACKEE_MAX_PCM_MS",
    "CIRCUITPY_WIFI_SSID",
    "CIRCUITPY_WIFI_PASSWORD",
    "CIRCUITPY_WEB_API_PASSWORD",
    NULL,
};

static bool in_list(const char *const *list, const char *key) {
    if (key == NULL) {
        return false;
    }
    for (int i = 0; list[i] != NULL; i++) {
        if (strcmp(list[i], key) == 0) {
            return true;
        }
    }
    return false;
}

bool stackee_settings_key_allowed(const char *key) {
    return in_list(ALLOWED_KEYS, key);
}

bool stackee_settings_key_secret(const char *key) {
    return in_list(SECRET_KEYS, key);
}

const char *const *stackee_settings_report_keys(void) { return REPORT_KEYS; }
const char *const *stackee_settings_secret_keys(void) { return SECRET_KEYS; }

// ---- 1 行から「ベアキー = 値」を切り出す (stackee_console.py の split_key) --
//
// 行頭の空白は許す。`#` で始まる行はキーではない。
static bool split_key(const char *line, size_t len, char *key, size_t key_cap,
                      size_t *lead) {
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    if (lead != NULL) {
        *lead = i;
    }
    if (i >= len || line[i] == '#') {
        return false;
    }
    const char *body = line + i;
    size_t blen = len - i;
    const char *eq = memchr(body, '=', blen);
    if (eq == NULL) {
        return false;
    }
    size_t klen = (size_t)(eq - body);
    while (klen > 0 && (body[klen - 1] == ' ' || body[klen - 1] == '\t')) {
        klen--;
    }
    if (klen == 0 || klen >= key_cap) {
        return false;
    }
    if (body[0] >= '0' && body[0] <= '9') {
        return false;
    }
    for (size_t j = 0; j < klen; j++) {
        if (!key_char(body[j])) {
            return false;
        }
    }
    memcpy(key, body, klen);
    key[klen] = '\0';
    return true;
}

// 出力用の小道具。cap を超えても「必要だった長さ」を数え続ける。
typedef struct {
    char  *buf;
    size_t cap;
    size_t at;
} sink_t;

static void put(sink_t *s, const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s->buf != NULL && s->at + 1 < s->cap) {
            s->buf[s->at] = data[i];
        }
        s->at++;
    }
}

static void put_str(sink_t *s, const char *str) {
    put(s, str, strlen(str));
}

static void sink_end(sink_t *s) {
    if (s->buf != NULL && s->cap > 0) {
        s->buf[(s->at < s->cap) ? s->at : (s->cap - 1)] = '\0';
    }
}

// TOML の基本文字列として値を書く (stackee_console.py の quote と同じ範囲)。
static void put_quoted(sink_t *s, const char *value) {
    put_str(s, "\"");
    for (const char *p = value; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[2] = {'\\', *p};
            put(s, esc, 2);
        } else if (*p == '\n') {
            put_str(s, "\\n");
        } else if (*p == '\r') {
            put_str(s, "\\r");
        } else if (*p == '\t') {
            put_str(s, "\\t");
        } else if ((unsigned char)*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04X", (unsigned char)*p);
            put_str(s, esc);
        } else {
            put(s, p, 1);
        }
    }
    put_str(s, "\"");
}

size_t stackee_settings_mask(const char *text, char *out, size_t cap) {
    sink_t s = {out, cap, 0};
    if (text == NULL) {
        sink_end(&s);
        return 0;
    }
    const char *line = text;
    bool first = true;
    while (true) {
        const char *end = strchr(line, '\n');
        size_t len = (end == NULL) ? strlen(line) : (size_t)(end - line);
        if (!first) {
            put_str(&s, "\n");
        }
        first = false;

        char key[STACKEE_SETTINGS_KEY_MAX];
        size_t lead = 0;
        bool have = split_key(line, len, key, sizeof(key), &lead);
        if (!have) {
            // コメント行の中の値も伏せる。無効化済みのパスワード行に
            // 生値が残っていることがあるため (現行 mask_settings と同じ)。
            size_t i = 0;
            while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '#')) {
                i++;
            }
            have = split_key(line + i, len - i, key, sizeof(key), NULL);
            lead = have ? i : 0;
        }
        if (have && stackee_settings_key_secret(key)) {
            put(&s, line, lead);
            put_str(&s, key);
            put_str(&s, " = \"***\"");
        } else {
            put(&s, line, len);
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }
    sink_end(&s);
    return s.at;
}

size_t stackee_settings_rewrite(const char *text,
                                const char *const *keys,
                                const char *const *values,
                                int n, char *out, size_t cap) {
    sink_t s = {out, cap, 0};
    if (text == NULL) {
        text = "";
    }
    bool done[STACKEE_SETTINGS_MAX_KEYS];
    memset(done, 0, sizeof(done));
    if (n > STACKEE_SETTINGS_MAX_KEYS) {
        n = STACKEE_SETTINGS_MAX_KEYS;
    }

    // 1) 既にある行はその場で書き換える (順番は変えない)。
    //    ★ 末尾の空行は「新しいキーを足す位置」なので、そこまでの長さ
    //      (mark) を覚えておく。空行を数えて引き算すると、空のファイルで
    //      0 から引いて下へ回り込む (ASan が捕まえた)。
    const char *line = text;
    bool first = true;
    size_t mark = 0;        // 中身のある行を書き終えた位置
    while (true) {
        const char *end = strchr(line, '\n');
        size_t len = (end == NULL) ? strlen(line) : (size_t)(end - line);

        char key[STACKEE_SETTINGS_KEY_MAX];
        bool have = split_key(line, len, key, sizeof(key), NULL);
        int hit = -1;
        for (int i = 0; have && i < n; i++) {
            if (keys[i] != NULL && strcmp(keys[i], key) == 0) {
                hit = i;
                break;
            }
        }
        if (hit >= 0) {
            done[hit] = true;
            if (values[hit] != NULL) {
                if (!first) { put_str(&s, "\n"); }
                first = false;
                put_str(&s, keys[hit]);
                put_str(&s, " = ");
                put_quoted(&s, values[hit]);
                mark = s.at;
            }
            // 値が NULL なら行ごと消す (= 何も書かない。mark も動かさない)
        } else {
            if (!first) { put_str(&s, "\n"); }
            first = false;
            put(&s, line, len);
            if (len > 0) {
                mark = s.at;
            }
        }
        if (end == NULL) {
            break;
        }
        line = end + 1;
    }

    // 2) 無かったキーは末尾に足す。末尾の空行の「前」に入れて、
    //    最後の改行を 1 つに保つ (現行 rewrite_settings と同じ)。
    bool any = false;
    for (int i = 0; i < n; i++) {
        if (!done[i] && keys[i] != NULL && values[i] != NULL) {
            any = true;
            break;
        }
    }
    if (any) {
        s.at = mark;                // 末尾の空行を落とす
        first = (mark == 0);
        for (int i = 0; i < n; i++) {
            if (done[i] || keys[i] == NULL || values[i] == NULL) {
                continue;
            }
            if (!first) { put_str(&s, "\n"); }
            first = false;
            put_str(&s, keys[i]);
            put_str(&s, " = ");
            put_quoted(&s, values[i]);
        }
        put_str(&s, "\n");
    }
    sink_end(&s);
    return s.at;
}
