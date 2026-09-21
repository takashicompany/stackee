// 字幕まわりを Mac の上でそのまま走らせる。
//
// 実機と**同じ実体** (main/stackee_font16.c / stackee_draw.c / stackee_crc32.c)
// に、実機と同じ素材 (assets/font16.bin) を通して、帯 (240x70) の CRC32 を出す。
//
//   ./subtitle <font16.bin のパス>      ← 命令は標準入力から 1 行 1 つ
//
//     info                 版・字数・区画の位置
//     glyph <16 進の符号>  その字の幅と 16 行の点 (無ければ "-")
//     px <文字列>          その文字列を描くのに要る画素数
//     fit <画素数> <文字列> そこに収まる**バイト数**
//     band <文字列>        帯を描いて CRC32 (文字列なしなら帯を消す)
//     bandpx <文字列>      帯に要る幅 = いちばん長い行の画素数
//
// ★ band / bandpx の引数の `\n` (逆斜線 + n) は本物の改行に直す。
//   標準入力は 1 行 1 命令なので、4 行の帯もこれで渡せる。
//
// 出したものを tools/test_subtitle_host.py が tools/subtitle_expected.py の
// 期待値と突き合わせる。ここが一致していれば「C の描画」と「Python の期待値」
// は同じ絵を作っているので、実機の `ui.subtitle` がずれたときは素材の読み込み
// (FAT) かフレームバッファの並びを疑えばよい、という切り分けができる。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_crc32.h"
#include "stackee_draw.h"
#include "stackee_font16.h"

#define WIDTH   240
#define HEIGHT  320
#define STRIDE  (WIDTH * 2)

static uint8_t g_fb[STRIDE * HEIGHT];
static stackee_font16_t g_font;
static bool g_ready;

static void *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "開けない: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size + 1);
    if (buf == NULL || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "読めない: %s\n", path);
        exit(1);
    }
    fclose(f);
    buf[size] = '\0';
    *len = (size_t)size;
    return buf;
}

// `\n` を本物の改行に直す (その場で詰める)。
static void unescape_newlines(char *text) {
    char *w = text;
    for (const char *r = text; *r != '\0'; r++) {
        if (r[0] == '\\' && r[1] == 'n') {
            *w++ = '\n';
            r++;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static void cmd_band(const char *text) {
    stackee_canvas_t canvas = {
        .fb = g_fb, .stride = STRIDE, .width = WIDTH, .height = HEIGHT};
    stackee_draw_subtitle(&canvas, g_ready ? &g_font : NULL, text);
    uint32_t crc = stackee_crc32(0, g_fb + (size_t)STACKEE_SUB_Y * STRIDE,
                                 (size_t)STACKEE_SUB_HEIGHT * STRIDE);
    printf("band %u %d %u\n", crc,
           stackee_draw_subtitle_px(g_ready ? &g_font : NULL, text),
           (unsigned)strlen(text));
}

static void cmd_glyph(uint32_t cp) {
    stackee_font16_glyph_t g;
    if (!stackee_font16_glyph(g_ready ? &g_font : NULL, cp, &g)) {
        printf("glyph %04X - -\n", cp);
        return;
    }
    printf("glyph %04X %d ", cp, g.width);
    int bytes = (g.width == 8) ? STACKEE_FONT16_HEIGHT
                               : STACKEE_FONT16_HEIGHT * 2;
    for (int i = 0; i < bytes; i++) {
        printf("%02X", g.rows[i]);
    }
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "使い方: subtitle <font16.bin>\n");
        return 2;
    }
    size_t len = 0;
    void *blob = read_file(argv[1], &len);
    g_ready = stackee_font16_open(&g_font, blob, len);

    char line[1024];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *arg = strchr(line, ' ');
        if (arg) { *arg++ = '\0'; }
        if (strcmp(line, "info") == 0) {
            printf("info %s %d %d %d %u\n", g_ready ? "ok" : "bad",
                   STACKEE_FONT16_HEIGHT, g_font.narrow_count, g_font.wide_count,
                   (unsigned)len);
            printf("band_geom %d %d %d %d %d %d %d %d\n",
                   STACKEE_SUB_Y, STACKEE_SUB_HEIGHT, STACKEE_SUB_COLS,
                   STACKEE_SUB_WIDTH, STACKEE_SUB_LINES, STACKEE_SUB_LINE_H,
                   STACKEE_SUB_PAD, STACKEE_SUB_MARGIN);
            printf("face_geom %d %d %d %d\n", STACKEE_FACE_SIZE,
                   STACKEE_FACE_TRIM_TOP, STACKEE_FACE_TRIM_BOTTOM,
                   STACKEE_FACE_ROWS);
        } else if (strcmp(line, "glyph") == 0) {
            cmd_glyph(arg ? (uint32_t)strtoul(arg, NULL, 16) : 0);
        } else if (strcmp(line, "px") == 0) {
            printf("px %d\n",
                   stackee_font16_text_px(g_ready ? &g_font : NULL, arg ? arg : ""));
        } else if (strcmp(line, "fit") == 0) {
            char *text = arg ? strchr(arg, ' ') : NULL;
            if (text) { *text++ = '\0'; }
            printf("fit %zu\n",
                   stackee_font16_fit(g_ready ? &g_font : NULL, text ? text : "",
                                      arg ? atoi(arg) : 0));
        } else if (strcmp(line, "band") == 0) {
            if (arg) { unescape_newlines(arg); }
            cmd_band(arg ? arg : "");
        } else if (strcmp(line, "bandpx") == 0) {
            if (arg) { unescape_newlines(arg); }
            printf("bandpx %d\n",
                   stackee_draw_subtitle_px(g_ready ? &g_font : NULL,
                                            arg ? arg : ""));
        } else {
            fprintf(stderr, "unknown command: %s\n", line);
            return 2;
        }
        fflush(stdout);
    }
    free(blob);
    return 0;
}
