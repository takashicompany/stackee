#include "stackee_jsonlite.h"

#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *at) {
    while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') {
        at++;
    }
    return at;
}

// 文字列の終わり (閉じ引用符の次) を返す。at は開き引用符を指していること。
static const char *skip_string(const char *at) {
    at++;                       // 開き引用符
    while (*at) {
        if (*at == '\\' && at[1]) {
            at += 2;
            continue;
        }
        if (*at == '"') {
            return at + 1;
        }
        at++;
    }
    return NULL;                // 閉じていない
}

// 値ひとつ (文字列 / 数 / true / false / null / 配列 / オブジェクト) を飛ばす。
static const char *skip_value(const char *at) {
    at = skip_ws(at);
    if (*at == '"') {
        return skip_string(at);
    }
    if (*at == '{' || *at == '[') {
        int depth = 0;
        for (; *at; at++) {
            if (*at == '"') {
                const char *end = skip_string(at);
                if (end == NULL) {
                    return NULL;
                }
                at = end - 1;
                continue;
            }
            if (*at == '{' || *at == '[') {
                depth++;
            } else if (*at == '}' || *at == ']') {
                if (--depth == 0) {
                    return at + 1;
                }
            }
        }
        return NULL;
    }
    // 数 / true / false / null。区切りまで進む。
    while (*at && *at != ',' && *at != '}' && *at != ']' &&
           *at != ' ' && *at != '\t' && *at != '\n' && *at != '\r') {
        at++;
    }
    return at;
}

// 逃がしの入ったキーと、逃がしの無い name を比べる。
static bool key_equals(const char *quoted, const char *name) {
    const char *at = quoted + 1;        // 開き引用符の次
    for (;;) {
        if (*at == '"') {
            return *name == '\0';
        }
        if (*at == '\0') {
            return false;
        }
        char c = *at++;
        if (c == '\\' && *at) {
            // 設定とサーバの応答のキーに逃がしは出てこない。出てきたら不一致扱い。
            return false;
        }
        if (*name == '\0' || *name != c) {
            return false;
        }
        name++;
    }
}

bool stackee_json_raw(const char *json, const char *key,
                      const char **at_out, size_t *len_out) {
    if (json == NULL || key == NULL) {
        return false;
    }
    const char *at = skip_ws(json);
    if (*at != '{') {
        return false;
    }
    at = skip_ws(at + 1);
    while (*at && *at != '}') {
        if (*at != '"') {
            return false;                       // 鍵は必ず文字列
        }
        const char *key_at = at;
        const char *key_end = skip_string(at);
        if (key_end == NULL) {
            return false;
        }
        at = skip_ws(key_end);
        if (*at != ':') {
            return false;
        }
        const char *value = skip_ws(at + 1);
        const char *value_end = skip_value(value);
        if (value_end == NULL) {
            return false;
        }
        if (key_equals(key_at, key)) {
            if (at_out) { *at_out = value; }
            if (len_out) { *len_out = (size_t)(value_end - value); }
            return true;
        }
        at = skip_ws(value_end);
        if (*at == ',') {
            at = skip_ws(at + 1);
        }
    }
    return false;
}

static size_t put_utf8(uint32_t cp, char *out, size_t cap, size_t at) {
    char tmp[4];
    size_t n;
    if (cp < 0x80) {
        tmp[0] = (char)cp; n = 1;
    } else if (cp < 0x800) {
        tmp[0] = (char)(0xC0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    // 途中で切れた多バイト文字を残さない (画面と JSON の両方が壊れるため)。
    if (at + n + 1 > cap) {
        return at;
    }
    memcpy(out + at, tmp, n);
    return at + n;
}

static uint32_t hex4(const char *at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = at[i];
        uint32_t d;
        if (c >= '0' && c <= '9')      { d = (uint32_t)(c - '0'); }
        else if (c >= 'a' && c <= 'f') { d = (uint32_t)(c - 'a' + 10); }
        else if (c >= 'A' && c <= 'F') { d = (uint32_t)(c - 'A' + 10); }
        else { return 0xFFFFFFFFu; }
        v = (v << 4) | d;
    }
    return v;
}

// 入れ物が尽きて途中で止めたとき、末尾に残った「多バイト文字の途中」を
// 落とす。UTF-8 の後続バイトは 10xxxxxx、先頭バイトは長さを自分で名乗るので、
// 末尾から最大 3 バイト戻れば判定できる。
// ★ 呼ぶのは**切り詰めたときだけ**。最後まで入ったものには触らない
//   (UTF-8 でない入力を黙って削らないため)。
static size_t trim_partial_utf8(const char *out, size_t pos) {
    size_t back = 0;
    while (back < 3 && back < pos) {
        unsigned char c = (unsigned char)out[pos - 1 - back];
        if ((c & 0xC0) == 0x80) {
            back++;                 // 後続バイト。もう 1 つ前を見る
            continue;
        }
        size_t need;
        if ((c & 0x80) == 0x00)      { need = 1; }
        else if ((c & 0xE0) == 0xC0) { need = 2; }
        else if ((c & 0xF0) == 0xE0) { need = 3; }
        else if ((c & 0xF8) == 0xF0) { need = 4; }
        else                         { return pos; }    // UTF-8 でない。触らない
        if (need > back + 1) {
            return pos - back - 1;  // 足りない = 途中で切れている
        }
        return pos;
    }
    return pos;
}

size_t stackee_json_unescape(const char *at, size_t len, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    size_t pos = 0;
    bool truncated = false;
    if (len >= 2 && at[0] == '"') {         // 引用符は剥がす
        at++;
        len -= 2;
    }
    for (size_t i = 0; i < len; i++) {
        if (pos + 1 >= cap) {
            truncated = true;
            break;
        }
        char c = at[i];
        if (c != '\\') {
            out[pos++] = c;
            continue;
        }
        if (i + 1 >= len) {
            break;
        }
        char esc = at[++i];
        switch (esc) {
            case 'n': out[pos++] = '\n'; break;
            case 'r': out[pos++] = '\r'; break;
            case 't': out[pos++] = '\t'; break;
            case 'b': out[pos++] = '\b'; break;
            case 'f': out[pos++] = '\f'; break;
            case 'u': {
                if (i + 4 >= len) { i = len; break; }
                uint32_t cp = hex4(at + i + 1);
                if (cp == 0xFFFFFFFFu) { break; }
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < len &&
                    at[i + 1] == '\\' && at[i + 2] == 'u') {
                    uint32_t lo = hex4(at + i + 3);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                pos = put_utf8(cp, out, cap, pos);
                break;
            }
            default: out[pos++] = esc; break;   // \" \\ \/ はそのまま
        }
    }
    if (truncated) {
        pos = trim_partial_utf8(out, pos);
    }
    out[pos] = '\0';
    return pos;
}

bool stackee_json_str(const char *json, const char *key, char *out, size_t cap) {
    const char *at = NULL;
    size_t len = 0;
    if (!stackee_json_raw(json, key, &at, &len) || len < 2 || at[0] != '"') {
        return false;
    }
    stackee_json_unescape(at, len, out, cap);
    return true;
}

bool stackee_json_int(const char *json, const char *key, long *out) {
    const char *at = NULL;
    size_t len = 0;
    if (!stackee_json_raw(json, key, &at, &len) || len == 0) {
        return false;
    }
    if (at[0] == '"' || at[0] == '{' || at[0] == '[') {
        return false;
    }
    char *end = NULL;
    long value = strtol(at, &end, 10);
    if (end == at) {
        return false;
    }
    if (out) { *out = value; }
    return true;
}

bool stackee_json_bool(const char *json, const char *key, bool *out) {
    const char *at = NULL;
    size_t len = 0;
    if (!stackee_json_raw(json, key, &at, &len)) {
        return false;
    }
    if (len == 4 && strncmp(at, "true", 4) == 0)  { if (out) *out = true;  return true; }
    if (len == 5 && strncmp(at, "false", 5) == 0) { if (out) *out = false; return true; }
    return false;
}
