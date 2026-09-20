#include "stackee_font16.h"

#include <string.h>

#include "stackee_crc32.h"

#define MAGIC          "STKFNT16"
#define MAGIC_LEN      8
#define HEADER_BYTES   40
#define VERSION        1

// ★ 非整列のまま読む。font16.bin は PSRAM に読んだ生のバイト列で、
//   区画の先頭が 2 の倍数であることしか保証していない。u16* に
//   キャストしないので、どこに置かれても安全に読める。
static inline uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool stackee_font16_open(stackee_font16_t *f, const void *data, size_t len) {
    if (f == NULL) {
        return false;
    }
    memset(f, 0, sizeof(*f));
    const uint8_t *p = (const uint8_t *)data;
    if (p == NULL || len < HEADER_BYTES || memcmp(p, MAGIC, MAGIC_LEN) != 0) {
        return false;
    }
    if (rd16(p + 8) != VERSION || rd16(p + 10) != STACKEE_FONT16_HEIGHT) {
        return false;
    }
    int narrow = (int)rd16(p + 12);
    int wide = (int)rd16(p + 14);
    uint32_t narrow_codes = rd32(p + 16);
    uint32_t narrow_bits = rd32(p + 20);
    uint32_t wide_codes = rd32(p + 24);
    uint32_t wide_bits = rd32(p + 28);
    uint32_t total = rd32(p + 32);
    uint32_t crc = rd32(p + 36);
    if (total != len || total < HEADER_BYTES) {
        return false;
    }
    // 区画がファイルの中に収まっているか (ここを抜かすと壊れた素材で落ちる)。
    if (narrow_codes != HEADER_BYTES ||
        narrow_bits != narrow_codes + (uint32_t)narrow * 2 ||
        wide_codes != narrow_bits + (uint32_t)narrow * STACKEE_FONT16_HEIGHT ||
        wide_bits != wide_codes + (uint32_t)wide * 2 ||
        total != wide_bits + (uint32_t)wide * STACKEE_FONT16_HEIGHT * 2) {
        return false;
    }
    if (stackee_crc32(0, p + HEADER_BYTES, len - HEADER_BYTES) != crc) {
        return false;
    }
    f->data = p;
    f->len = len;
    f->narrow_count = narrow;
    f->wide_count = wide;
    f->narrow_codes = p + narrow_codes;
    f->narrow_bits = p + narrow_bits;
    f->wide_codes = p + wide_codes;
    f->wide_bits = p + wide_bits;
    f->ok = true;
    return true;
}

// Unicode 昇順の表を二分探索する。見つからなければ -1。
static int find(const uint8_t *codes, int count, uint32_t cp) {
    int low = 0;
    int high = count;
    while (low < high) {
        int mid = low + (high - low) / 2;
        uint32_t here = rd16(codes + (size_t)mid * 2);
        if (here < cp) {
            low = mid + 1;
        } else if (here > cp) {
            high = mid;
        } else {
            return mid;
        }
    }
    return -1;
}

bool stackee_font16_glyph(const stackee_font16_t *f, uint32_t cp,
                          stackee_font16_glyph_t *out) {
    if (!stackee_font16_ready(f) || out == NULL || cp > 0xFFFFu) {
        return false;
    }
    int i = find(f->narrow_codes, f->narrow_count, cp);
    if (i >= 0) {
        out->rows = f->narrow_bits + (size_t)i * STACKEE_FONT16_HEIGHT;
        out->width = 8;
        return true;
    }
    i = find(f->wide_codes, f->wide_count, cp);
    if (i >= 0) {
        out->rows = f->wide_bits + (size_t)i * STACKEE_FONT16_HEIGHT * 2;
        out->width = 16;
        return true;
    }
    return false;
}

bool stackee_font16_glyph_or_tofu(const stackee_font16_t *f, uint32_t cp,
                                  stackee_font16_glyph_t *out) {
    if (stackee_font16_glyph(f, cp, out)) {
        return true;
    }
    return stackee_font16_glyph(f, STACKEE_FONT16_TOFU, out);
}

int stackee_font16_advance(const stackee_font16_t *f, uint32_t cp) {
    stackee_font16_glyph_t g;
    if (!stackee_font16_glyph_or_tofu(f, cp, &g)) {
        return 0;
    }
    return g.width;
}

const char *stackee_font16_utf8(const char *p, uint32_t *cp) {
    if (p == NULL || *p == '\0') {
        return NULL;
    }
    const uint8_t *s = (const uint8_t *)p;
    uint32_t c = s[0];
    int extra;
    uint32_t value;
    if (c < 0x80u)              { extra = 0; value = c; }
    else if ((c & 0xE0u) == 0xC0u) { extra = 1; value = c & 0x1Fu; }
    else if ((c & 0xF0u) == 0xE0u) { extra = 2; value = c & 0x0Fu; }
    else if ((c & 0xF8u) == 0xF0u) { extra = 3; value = c & 0x07u; }
    else {
        // 継続バイトか不正な先頭。1 バイト捨てる (止まらない)。
        *cp = 0xFFFDu;
        return p + 1;
    }
    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xC0u) != 0x80u) {
            *cp = 0xFFFDu;
            return p + 1;       // 途中で切れている。1 バイトだけ捨てる
        }
        value = (value << 6) | (uint32_t)(s[i] & 0x3Fu);
    }
    *cp = value;
    return p + 1 + extra;
}

int stackee_font16_text_px(const stackee_font16_t *f, const char *utf8) {
    int px = 0;
    uint32_t cp = 0;
    for (const char *p = utf8; (p = stackee_font16_utf8(p, &cp)) != NULL; ) {
        px += stackee_font16_advance(f, cp);
    }
    return px;
}

size_t stackee_font16_fit(const stackee_font16_t *f, const char *utf8, int max_px) {
    if (utf8 == NULL) {
        return 0;
    }
    int px = 0;
    uint32_t cp = 0;
    const char *p = utf8;
    for (;;) {
        const char *next = stackee_font16_utf8(p, &cp);
        if (next == NULL) {
            break;
        }
        int adv = stackee_font16_advance(f, cp);
        if (px + adv > max_px) {
            break;
        }
        px += adv;
        p = next;
    }
    return (size_t)(p - utf8);
}
