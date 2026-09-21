#include "stackee_draw.h"

#include <string.h>

#include "stackee_font8x8.h"

uint16_t stackee_draw_rgb565(uint32_t rgb) {
    uint32_t r = (rgb >> 16) & 0xFF;
    uint32_t g = (rgb >> 8) & 0xFF;
    uint32_t b = rgb & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline void put(const stackee_canvas_t *c, int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= c->width || y >= c->height) {
        return;
    }
    uint8_t *p = c->fb + (size_t)y * (size_t)c->stride + (size_t)x * 2;
    p[0] = (uint8_t)(color >> 8);
    p[1] = (uint8_t)(color & 0xFF);
}

void stackee_draw_fill(const stackee_canvas_t *c, int x, int y, int w, int h,
                       uint16_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->width)  { w = c->width - x; }
    if (y + h > c->height) { h = c->height - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    for (int row = y; row < y + h; row++) {
        uint8_t *p = c->fb + (size_t)row * (size_t)c->stride + (size_t)x * 2;
        for (int col = 0; col < w; col++) {
            *p++ = hi;
            *p++ = lo;
        }
    }
}

// ---- 顔 (4bpp、16 段のグレー) ---------------------------------------------
//
// パレットは現行 CircuitPython 版と同じ「i*17 の等間隔グレー」
// (stackee_face.py: palette[i] = (i * 17) * 0x010101)。
void stackee_draw_face(const stackee_canvas_t *c, const uint8_t *sheet, int size,
                       int rows, int frame, int dx, int dy, int sx, int sy,
                       int w, int h) {
    if (sheet == NULL || w <= 0 || h <= 0) {
        return;
    }
    uint16_t grey[16];
    for (int i = 0; i < 16; i++) {
        uint32_t v = (uint32_t)(i * 17);
        grey[i] = stackee_draw_rgb565((v << 16) | (v << 8) | v);
    }
    size_t row_bytes = (size_t)size / 2;             // 4bpp
    const uint8_t *base = sheet + (size_t)frame * (size_t)rows * row_bytes;
    for (int row = 0; row < h; row++) {
        int y = sy + row;
        if (y < 0 || y >= rows) {
            continue;
        }
        const uint8_t *src = base + (size_t)y * row_bytes;
        int dest_y = dy + y;
        if (dest_y < 0 || dest_y >= c->height) {
            continue;
        }
        uint8_t *p = c->fb + (size_t)dest_y * (size_t)c->stride;
        for (int col = 0; col < w; col++) {
            int x = sx + col;
            if (x < 0 || x >= size) {
                continue;
            }
            int dest_x = dx + x;
            if (dest_x < 0 || dest_x >= c->width) {
                continue;
            }
            uint8_t byte = src[x >> 1];
            // ★ 画素 0 が上位ニブル (tools/import_faces.py の
            //   `(pixels[i] << 4) | pixels[i+1]`)。
            int level = (x & 1) ? (byte & 0x0F) : (byte >> 4);
            uint16_t color = grey[level];
            p[dest_x * 2]     = (uint8_t)(color >> 8);
            p[dest_x * 2 + 1] = (uint8_t)(color & 0xFF);
        }
    }
}

// ---- アイコン (2bpp、0 = 透明) --------------------------------------------
void stackee_draw_icon(const stackee_canvas_t *c, const uint8_t *sheet, int tile,
                       uint32_t color, uint32_t bg, int x, int y) {
    if (sheet == NULL || tile < 0 || tile >= STACKEE_ICON_TILES) {
        return;
    }
    uint16_t shade[4];
    for (int level = 0; level < 4; level++) {
        shade[level] = stackee_draw_rgb565(stackee_icons_shade(color, level, bg));
    }
    int per_row = STACKEE_ICON_SIZE * STACKEE_ICON_BPP / 8;      // 6 バイト
    const uint8_t *base = sheet + (size_t)tile * (size_t)per_row * STACKEE_ICON_SIZE;
    for (int row = 0; row < STACKEE_ICON_SIZE; row++) {
        const uint8_t *src = base + (size_t)row * per_row;
        for (int col = 0; col < STACKEE_ICON_SIZE; col++) {
            // ★ 画素 0 が最上位 2 ビット
            //   (tools/generate_status_assets.py の (a<<6)|(b<<4)|(c<<2)|d)。
            uint8_t byte = src[col >> 2];
            int level = (byte >> (6 - 2 * (col & 3))) & 0x03;
            if (level == 0) {
                continue;       // 透明。背景 (黒帯) をそのまま残す
            }
            put(c, x + col, y + row, shade[level]);
        }
    }
}

// ---- 文字 -----------------------------------------------------------------

// 内蔵 8x8 を 1 文字 12x24 へ引き伸ばす代役。
//
// ★ 「3 倍」は縦だけ。横 3 倍 (24px) にすると 4 文字で 96px になり、
//   電池の数字に取ってある 48px (CHAR_WIDTH 12 x 4) に収まらないため、
//   横は 12px (1.5 倍) にしてある。BDF が読めたときは使わない道。
static void draw_fallback_char(const stackee_canvas_t *c, char ch, uint32_t color,
                               int x, int top) {
    unsigned char code = (unsigned char)ch;
    if (code < STACKEE_FONT_FIRST || code > STACKEE_FONT_LAST) {
        code = '?';
    }
    const uint8_t *glyph = stackee_font8x8[code - STACKEE_FONT_FIRST];
    uint16_t fg = stackee_draw_rgb565(color);
    for (int row = 0; row < 24; row++) {
        int src_y = row / 3;
        for (int col = 0; col < STACKEE_CHAR_WIDTH; col++) {
            int src_x = col * 8 / STACKEE_CHAR_WIDTH;
            if (glyph[src_y] & (1 << src_x)) {
                put(c, x + col, top + row, fg);
            }
        }
    }
}

void stackee_draw_text(const stackee_canvas_t *c, const stackee_bdf_font_t *font,
                       const char *text, uint32_t color, int x, int y_mid,
                       int box_h) {
    if (text == NULL) {
        return;
    }
    if (font == NULL) {
        int top = y_mid - 12;
        for (const char *p = text; *p; p++) {
            draw_fallback_char(c, *p, color, x, top);
            x += STACKEE_CHAR_WIDTH;
        }
        return;
    }
    uint16_t fg = stackee_draw_rgb565(color);
    // firmware/kmk/tools/preview_status_bar.py の draw_text と同じ置き方。
    int baseline = y_mid - box_h / 2 + font->ascent;
    for (const char *p = text; *p; p++) {
        const stackee_bdf_glyph_t *g = stackee_bdf_glyph(font, *p);
        if (g == NULL) {
            g = stackee_bdf_glyph(font, '?');
            if (g == NULL) {
                continue;
            }
        }
        int top = baseline - (g->h + g->yo);
        for (int row = 0; row < g->h && row < STACKEE_BDF_MAX_ROWS; row++) {
            uint16_t bits = g->rows[row];
            for (int col = 0; col < g->w && col < 16; col++) {
                if (bits & (uint16_t)(0x8000u >> col)) {
                    put(c, x + g->xo + col, top + row, fg);
                }
            }
        }
        x += g->adv;
    }
}

// ---- 字幕の帯 -------------------------------------------------------------
//
// ★ 1 回の描き直しは 240x70 = 16,800 画素 (33.6 KB) の塗りつぶしと、多くても
//   3 行 x 15 字ぶんの 16x16 の点打ち。割り当ても検索表の構築もしない
//   (字形は font16.bin の中を指すポインタのまま使う)。
//
// 行の区切りは改行 (\n)。4 行目以降は捨てる (頁めくりは呼び手の仕事)。
static void draw_subtitle_line(const stackee_canvas_t *c,
                               const stackee_font16_t *font,
                               const char *utf8, size_t len, int top) {
    uint16_t fg = stackee_draw_rgb565(STACKEE_SUB_FG);
    int x = 0;
    int limit = (c->width < STACKEE_SUB_WIDTH) ? c->width : STACKEE_SUB_WIDTH;
    uint32_t cp = 0;
    const char *end = utf8 + len;
    for (const char *p = utf8; p < end; ) {
        const char *next = stackee_font16_utf8(p, &cp);
        if (next == NULL || next > end) {
            break;
        }
        p = next;
        stackee_font16_glyph_t g;
        if (!stackee_font16_glyph_or_tofu(font, cp, &g)) {
            continue;                   // 〓 すら無い (font16.bin が壊れている)
        }
        if (x + g.width > limit) {
            break;                      // はみ出す字は描かない (途中で切らない)
        }
        for (int row = 0; row < STACKEE_FONT16_HEIGHT; row++) {
            uint32_t bits = (g.width == 8)
                                ? (uint32_t)g.rows[row]
                                : (((uint32_t)g.rows[row * 2] << 8) |
                                   (uint32_t)g.rows[row * 2 + 1]);
            if (bits == 0) {
                continue;
            }
            for (int col = 0; col < g.width; col++) {
                if (bits & (1u << (g.width - 1 - col))) {
                    put(c, x + col, top + row, fg);
                }
            }
        }
        x += g.width;
    }
}

int stackee_draw_subtitle_px(const stackee_font16_t *font, const char *utf8) {
    if (utf8 == NULL) {
        return 0;
    }
    int widest = 0;
    const char *at = utf8;
    for (int line = 0; line < STACKEE_SUB_LINES && *at != '\0'; line++) {
        const char *nl = strchr(at, '\n');
        int px = 0;
        uint32_t cp = 0;
        const char *end = (nl != NULL) ? nl : at + strlen(at);
        for (const char *p = at; p < end; ) {
            const char *next = stackee_font16_utf8(p, &cp);
            if (next == NULL || next > end) {
                break;
            }
            p = next;
            px += stackee_font16_advance(font, cp);
        }
        if (px > widest) {
            widest = px;
        }
        if (nl == NULL) {
            break;
        }
        at = nl + 1;
    }
    return widest;
}

void stackee_draw_subtitle(const stackee_canvas_t *c,
                           const stackee_font16_t *font, const char *utf8) {
    bool empty = (utf8 == NULL || utf8[0] == '\0');
    // ★ 帯はいつでも黒。空でも地の色に戻さない (文字だけ消える)。
    stackee_draw_fill(c, 0, STACKEE_SUB_Y, c->width, STACKEE_SUB_HEIGHT,
                      stackee_draw_rgb565(STACKEE_SUB_BG));
    if (empty || !stackee_font16_ready(font)) {
        return;
    }
    const char *at = utf8;
    for (int line = 0; line < STACKEE_SUB_LINES && *at != '\0'; line++) {
        const char *nl = strchr(at, '\n');
        size_t len = (nl != NULL) ? (size_t)(nl - at) : strlen(at);
        draw_subtitle_line(c, font, at, len,
                           STACKEE_SUB_Y + STACKEE_SUB_MARGIN +
                               line * STACKEE_SUB_LINE_H + STACKEE_SUB_PAD);
        if (nl == NULL) {
            break;
        }
        at = nl + 1;
    }
}

// ---- 上段バー -------------------------------------------------------------
void stackee_draw_bar(const stackee_canvas_t *c, const stackee_bar_state_t *st,
                      const uint8_t *icons, const stackee_bdf_font_t *font) {
    stackee_icons_layout_t pos;
    stackee_icons_layout(c->width, &pos);

    stackee_draw_fill(c, 0, 0, c->width, STACKEE_BAR_AREA_HEIGHT,
                      stackee_draw_rgb565(STACKEE_ICON_BG));

    uint32_t volume_color = stackee_icons_volume_color(st->volume);
    uint32_t wifi_color = stackee_icons_wifi_color(st->wifi);
    uint32_t link_color = stackee_icons_link_color(st->link, st->ble_connected);
    uint32_t battery_color = stackee_icons_battery_color(st->battery, st->charging);

    // 並びは stackee_icons.py の SLOTS と同じ。
    stackee_draw_icon(c, icons, stackee_icons_volume_tile(st->volume),
                      volume_color, STACKEE_ICON_BG, pos.volume_x, pos.volume_y);
    stackee_draw_icon(c, icons, stackee_icons_wifi_tile(st->wifi),
                      wifi_color, STACKEE_ICON_BG, pos.wifi_x, pos.icon_y);
    stackee_draw_icon(c, icons, stackee_icons_link_tile(st->link, st->ble_connected),
                      link_color, STACKEE_ICON_BG, pos.link_x, pos.icon_y);
    stackee_draw_icon(c, icons, stackee_icons_battery_tile(st->battery, st->charging),
                      battery_color, STACKEE_ICON_BG, pos.battery_x, pos.icon_y);

    char text[16];
    stackee_icons_volume_text(st->volume, text, sizeof(text));
    stackee_draw_text(c, font, text, volume_color, pos.volume_text_x,
                      pos.volume_text_y, STACKEE_BAR_HEIGHT);
    stackee_icons_battery_text(st->battery, text, sizeof(text));
    stackee_draw_text(c, font, text, battery_color, pos.battery_text_x,
                      pos.battery_text_y, STACKEE_BAR_HEIGHT);
}
