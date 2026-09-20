#include "stackee_bdf.h"

#include <stdlib.h>
#include <string.h>

// 行単位の読み取り。BDF は 1 行 1 項目なので、行の頭だけ見れば足りる。
typedef struct {
    const char *at;
    const char *end;
} cursor_t;

static bool next_line(cursor_t *c, const char **line, size_t *len) {
    if (c->at >= c->end) {
        return false;
    }
    const char *start = c->at;
    const char *nl = memchr(start, '\n', (size_t)(c->end - start));
    const char *stop = (nl != NULL) ? nl : c->end;
    c->at = (nl != NULL) ? nl + 1 : c->end;
    // 末尾の CR と空白を落とす。
    while (stop > start && (stop[-1] == '\r' || stop[-1] == ' ' || stop[-1] == '\t')) {
        stop--;
    }
    *line = start;
    *len = (size_t)(stop - start);
    return true;
}

static bool starts_with(const char *line, size_t len, const char *word) {
    size_t n = strlen(word);
    return len >= n && memcmp(line, word, n) == 0;
}

// "BBX 12 24 0 -2" のような行から整数を最大 count 個。
static int read_ints(const char *line, size_t len, long *out, int count) {
    char buf[64];
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, line, len);
    buf[len] = '\0';
    int got = 0;
    char *p = buf;
    while (got < count) {
        while (*p && (*p < '0' || *p > '9') && *p != '-') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        out[got++] = v;
        p = end;
    }
    return got;
}

static uint32_t hex_value(const char *line, size_t len, int *nbits) {
    uint32_t v = 0;
    int digits = 0;
    for (size_t i = 0; i < len && digits < 8; i++) {
        char c = line[i];
        int d;
        if (c >= '0' && c <= '9')       { d = c - '0'; }
        else if (c >= 'A' && c <= 'F')  { d = c - 'A' + 10; }
        else if (c >= 'a' && c <= 'f')  { d = c - 'a' + 10; }
        else { break; }
        v = (v << 4) | (uint32_t)d;
        digits++;
    }
    *nbits = digits * 4;
    return v;
}

const stackee_bdf_glyph_t *stackee_bdf_glyph(const stackee_bdf_font_t *font, char c) {
    if (font == NULL) {
        return NULL;
    }
    for (int i = 0; i < font->count; i++) {
        if (font->glyphs[i].code == (int32_t)(unsigned char)c) {
            return &font->glyphs[i];
        }
    }
    return NULL;
}

bool stackee_bdf_parse(const char *text, size_t len, const char *wanted,
                       stackee_bdf_font_t *out) {
    if (text == NULL || wanted == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->ascent = -1;
    out->descent = -1;

    size_t want_n = strlen(wanted);
    if (want_n > STACKEE_BDF_MAX_GLYPHS) {
        return false;
    }

    cursor_t cur = {.at = text, .end = text + len};
    const char *line;
    size_t line_len;

    stackee_bdf_glyph_t glyph;
    bool in_glyph = false;
    bool in_bitmap = false;
    int row = 0;
    memset(&glyph, 0, sizeof(glyph));

    while (next_line(&cur, &line, &line_len)) {
        if (starts_with(line, line_len, "FONT_ASCENT ")) {
            long v[1];
            if (read_ints(line + 11, line_len - 11, v, 1) == 1) { out->ascent = (int)v[0]; }
            continue;
        }
        if (starts_with(line, line_len, "FONT_DESCENT ")) {
            long v[1];
            if (read_ints(line + 12, line_len - 12, v, 1) == 1) { out->descent = (int)v[0]; }
            continue;
        }
        if (starts_with(line, line_len, "STARTCHAR")) {
            memset(&glyph, 0, sizeof(glyph));
            glyph.code = -1;
            in_glyph = true;
            in_bitmap = false;
            row = 0;
            continue;
        }
        if (!in_glyph) {
            continue;
        }
        if (starts_with(line, line_len, "ENDCHAR")) {
            in_glyph = false;
            in_bitmap = false;
            // 欲しい字だったら控える。
            if (glyph.code >= 0 && glyph.code < 128) {
                const char *hit = memchr(wanted, (char)glyph.code, want_n);
                if (hit != NULL && out->count < STACKEE_BDF_MAX_GLYPHS &&
                    stackee_bdf_glyph(out, (char)glyph.code) == NULL) {
                    out->glyphs[out->count++] = glyph;
                }
            }
            continue;
        }
        if (starts_with(line, line_len, "ENCODING ")) {
            long v[1];
            if (read_ints(line + 8, line_len - 8, v, 1) == 1) { glyph.code = (int32_t)v[0]; }
            continue;
        }
        if (starts_with(line, line_len, "DWIDTH ")) {
            long v[2];
            if (read_ints(line + 6, line_len - 6, v, 2) >= 1) { glyph.adv = (int16_t)v[0]; }
            continue;
        }
        if (starts_with(line, line_len, "BBX ")) {
            long v[4];
            if (read_ints(line + 3, line_len - 3, v, 4) == 4) {
                glyph.w = (int16_t)v[0];
                glyph.h = (int16_t)v[1];
                glyph.xo = (int16_t)v[2];
                glyph.yo = (int16_t)v[3];
            }
            continue;
        }
        if (starts_with(line, line_len, "BITMAP")) {
            in_bitmap = true;
            row = 0;
            continue;
        }
        if (in_bitmap && row < STACKEE_BDF_MAX_ROWS && row < glyph.h) {
            int nbits = 0;
            uint32_t v = hex_value(line, line_len, &nbits);
            if (nbits == 0) {
                continue;
            }
            // 左端を bit15 に揃える (preview_status_bar.py の
            // `(int(r,16) >> (nbits-1-x)) & 1` と同じ並び)。
            uint16_t packed = (nbits >= 16) ? (uint16_t)(v >> (nbits - 16))
                                            : (uint16_t)(v << (16 - nbits));
            glyph.rows[row++] = packed;
        }
    }

    if (out->ascent < 0 || out->descent < 0 || out->count != (int)want_n) {
        return false;
    }
    return true;
}
