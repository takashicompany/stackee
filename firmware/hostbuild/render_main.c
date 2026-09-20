// 段階 2 の描画を Mac の上でそのまま走らせる。
//
// 実機と**同じ実体** (main/stackee_draw.c / stackee_icons.c / stackee_bdf.c /
// stackee_faceanim.c / stackee_crc32.c) に、実機と同じ素材を通して、
// 240x320 の RGB565 フレームバッファを組み立てて CRC32 を出す。
//
//   ./render <firmware/kmk/stackee_assets のパス>
//
// 出したものを tools/test_render_host.py が tools/render_expected.py の
// 期待値と突き合わせる。ここが一致していれば「C の描画」と「Python の期待値」
// は同じ絵を作っているので、実機でずれたときは素材の読み込み (FAT / 展開) を
// 疑えばよい、という切り分けができる。
//
// zlib はホストのものを使う (-lz)。実機は ROM の tinfl。展開結果が同じかは
// 実機側で faces.bin の CRC を返して照合する (console の ui.assets)。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "stackee_bdf.h"
#include "stackee_crc32.h"
#include "stackee_draw.h"
#include "stackee_faceanim.h"
#include "stackee_icons.h"
#include "stackee_selftest.h"

#define WIDTH      240
#define HEIGHT     320
#define STRIDE     (WIDTH * 2)
#define FACE_SIZE  STACKEE_FACE_SIZE
#define FACE_ROWS  STACKEE_FACE_ROWS      // 上下 33 行を捨てた丈 (174)
#define FACE_TRIM  STACKEE_FACE_TRIM_ROWS
#define FACE_X     0
#define FACE_Y     50

static void *read_file(const char *dir, const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "開けない: %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)size + 1);
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "読めない: %s\n", path);
        exit(1);
    }
    fclose(f);
    ((char *)buf)[size] = '\0';
    *len = (size_t)size;
    return buf;
}

static void *inflate_file(const char *dir, const char *name, size_t expect, size_t *out_len) {
    size_t raw_len = 0;
    void *raw = read_file(dir, name, &raw_len);
    uLongf out = (uLongf)expect;
    void *buf = malloc(expect);
    if (uncompress(buf, &out, raw, (uLong)raw_len) != Z_OK || out != expect) {
        fprintf(stderr, "展開できない: %s (%lu != %zu)\n", name, (unsigned long)out, expect);
        exit(1);
    }
    free(raw);
    *out_len = (size_t)out;
    return buf;
}

// 「タイル番号・色・文字・配置の決め方」を全部書き出す。
// tools/test_render_host.py が firmware/kmk/stackee_icons.py と 1 つずつ
// 突き合わせる (しきい値 20/40/60/80 や 0/33/66 の境目を含む)。
static void dump_tables(void) {
    char text[16];
    for (int pct = -1; pct <= 100; pct++) {
        for (int chg = 0; chg < 2; chg++) {
            stackee_icons_battery_text(pct, text, sizeof(text));
            printf("battery %d %d %d %06X %s\n", pct, chg,
                   stackee_icons_battery_tile(pct, chg != 0),
                   stackee_icons_battery_color(pct, chg != 0), text);
        }
    }
    for (int pct = 0; pct <= 100; pct++) {
        stackee_icons_volume_text(pct, text, sizeof(text));
        printf("volume %d %d %06X %s\n", pct, stackee_icons_volume_tile(pct),
               stackee_icons_volume_color(pct), text);
    }
    static const char *const WIFI[] = {
        "off", "boot", "wait", "up", "load", "radio", "scan_start",
        "scan_wait", "scan_read", "connect", "linkup", "nonsense",
    };
    for (size_t i = 0; i < sizeof(WIFI) / sizeof(WIFI[0]); i++) {
        printf("wifi %s %d %06X\n", WIFI[i], stackee_icons_wifi_tile(WIFI[i]),
               stackee_icons_wifi_color(WIFI[i]));
    }
    static const char *const LINK[] = {"none", "ble", "usb"};
    for (int kind = 0; kind < 3; kind++) {
        for (int conn = 0; conn < 2; conn++) {
            printf("link %s %d %d %06X\n", LINK[kind], conn,
                   stackee_icons_link_tile((stackee_link_t)kind, conn != 0),
                   stackee_icons_link_color((stackee_link_t)kind, conn != 0));
        }
    }
    stackee_icons_layout_t pos;
    stackee_icons_layout(WIDTH, &pos);
    printf("layout volume %d %d\n", pos.volume_x, pos.volume_y);
    printf("layout volume_text %d %d\n", pos.volume_text_x, pos.volume_text_y);
    printf("layout wifi %d %d\n", pos.wifi_x, pos.icon_y);
    printf("layout link %d %d\n", pos.link_x, pos.icon_y);
    printf("layout battery %d %d\n", pos.battery_x, pos.icon_y);
    printf("layout battery_text %d %d\n", pos.battery_text_x, pos.battery_text_y);
    static const uint32_t COLORS[] = {
        STACKEE_ICON_FG, STACKEE_ICON_DIM, STACKEE_ICON_LOW, 0x123456u,
    };
    for (size_t i = 0; i < sizeof(COLORS) / sizeof(COLORS[0]); i++) {
        for (int level = 0; level < 4; level++) {
            printf("shade %06X %d %06X\n", COLORS[i], level,
                   stackee_icons_shade(COLORS[i], level, STACKEE_ICON_BG));
        }
    }
    printf("const bar_height %d bar_area %d char_width %d icon %d tiles %d\n",
           STACKEE_BAR_HEIGHT, STACKEE_BAR_AREA_HEIGHT, STACKEE_CHAR_WIDTH,
           STACKEE_ICON_SIZE, STACKEE_ICON_TILES);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "使い方: render <stackee_assets のパス> [tables]\n");
        return 2;
    }
    const char *dir = argv[1];
    if (argc > 2 && strcmp(argv[2], "tables") == 0) {
        dump_tables();
        return 0;
    }

    size_t faces_len = 0, changes_len = 0, icons_len = 0, font_len = 0;
    const int count = STACKEE_FACE_MAX_COUNT;
    uint8_t *faces = inflate_file(dir, "faces.bin",
                                  (size_t)FACE_SIZE * FACE_SIZE / 2 * count, &faces_len);
    // 本体 (stackee_ui.c の trim_faces) と同じ切り詰め。素材は 240x240 の
    // まま展開して丈を確かめ、その場で 240x174 へ詰め直す。
    {
        const size_t row_bytes = (size_t)FACE_SIZE / 2;
        for (int f = 0; f < count; f++) {
            memmove(faces + (size_t)f * FACE_ROWS * row_bytes,
                    faces + (size_t)f * FACE_SIZE * row_bytes +
                        (size_t)FACE_TRIM * row_bytes,
                    (size_t)FACE_ROWS * row_bytes);
        }
        faces_len = (size_t)FACE_SIZE * FACE_ROWS / 2 * count;
    }
    uint8_t *changes = inflate_file(dir, "changes.bin",
                                    (size_t)count * count * 4, &changes_len);
    uint8_t *icons = inflate_file(dir, "status_icons.bin",
                                  STACKEE_ICON_SHEET_BYTES, &icons_len);
    char *font_text = read_file(dir, "status_h24.bdf", &font_len);

    stackee_bdf_font_t font;
    if (!stackee_bdf_parse(font_text, font_len, "0123456789%-? ", &font)) {
        fprintf(stderr, "BDF を読めない\n");
        return 1;
    }

    static uint8_t fb[STRIDE * HEIGHT];
    stackee_canvas_t canvas = {.fb = fb, .stride = STRIDE, .width = WIDTH, .height = HEIGHT};

    const int bar_index = STACKEE_SELFTEST_BARS - 1;

    // 背景 (白) → 上段バー → 顔 0 を全面。ここが ui.selftest の出発点。
    stackee_draw_fill(&canvas, 0, 0, WIDTH, HEIGHT, stackee_draw_rgb565(STACKEE_SCREEN_BG));
    stackee_draw_bar(&canvas, stackee_selftest_bar(bar_index), icons, &font);
    stackee_draw_face(&canvas, faces, FACE_SIZE, FACE_ROWS, 0, FACE_X, FACE_Y,
                      0, 0, FACE_SIZE, FACE_ROWS);

    printf("count %d\n", count);
    printf("face_geom %d %d %d %d\n", FACE_Y, STACKEE_FACE_SIZE,
           STACKEE_FACE_TRIM_ROWS, STACKEE_FACE_ROWS);
    printf("assets faces_len=%zu faces_crc=%u changes_len=%zu changes_crc=%u "
           "icons_len=%zu icons_crc=%u glyphs=%d ascent=%d\n",
           faces_len, stackee_crc32(0, faces, faces_len),
           changes_len, stackee_crc32(0, changes, changes_len),
           icons_len, stackee_crc32(0, icons, icons_len),
           font.count, font.ascent);

    // 顔 0 の CRC を出してから、changes.bin を使って 1 枚ずつ差分で寄せる。
    // 差分で作った絵が「全面で描いた絵」と同じであることが、この検査の肝。
    printf("face 0 %u\n", stackee_crc32(0, fb + (size_t)FACE_Y * STRIDE,
                                        (size_t)FACE_ROWS * STRIDE));

    // ダミーの cases (差分だけを使うので中身は問わない)。
    static stackee_face_cases_t cases;
    for (int s = 0; s < STACKEE_FACE_STATES; s++) {
        cases.cases[s].group_count = 1;
        cases.cases[s].frame_count[0] = 1;
        cases.cases[s].frames[0][0] = 0;
    }
    stackee_face_view_t view;
    stackee_face_view_init(&view, &cases, changes, count, 0);

    for (int frame = 1; frame < count; frame++) {
        stackee_face_rect_t rect;
        int guard = 0;
        while (stackee_face_view_step_to(&view, frame, &rect)) {
            // 本体 (stackee_ui.c の paint_face_rect) と同じ寄せ方と切り落とし。
            int sy = rect.y - FACE_TRIM;
            int rh = rect.h;
            if (sy < 0) { rh += sy; sy = 0; }
            if (sy + rh > FACE_ROWS) { rh = FACE_ROWS - sy; }
            if (rect.w > 0 && rh > 0) {
                stackee_draw_face(&canvas, faces, FACE_SIZE, FACE_ROWS, rect.frame,
                                  FACE_X, FACE_Y, rect.x, sy, rect.w, rh);
            }
            if (++guard > 64) {
                fprintf(stderr, "差分が終わらない (frame=%d)\n", frame);
                return 1;
            }
        }
        printf("face %d %u\n", frame,
               stackee_crc32(0, fb + (size_t)FACE_Y * STRIDE, (size_t)FACE_ROWS * STRIDE));
    }

    // 顔を 0 に戻してからバーの 6 状態 (顔の絵が CRC に混ざらないよう、
    // バーの CRC は上段 28 行だけを見る)。
    stackee_draw_face(&canvas, faces, FACE_SIZE, FACE_ROWS, 0, FACE_X, FACE_Y,
                      0, 0, FACE_SIZE, FACE_ROWS);
    for (int i = 0; i < STACKEE_SELFTEST_BARS; i++) {
        stackee_draw_bar(&canvas, stackee_selftest_bar(i), icons, &font);
        printf("bar %d %u %s\n", i,
               stackee_crc32(0, fb, (size_t)STACKEE_BAR_AREA_HEIGHT * STRIDE),
               stackee_selftest_bar_name(i));
    }

    // 「顔 0 + バー 5」の全面。lcd.crc の "all" と突き合わせる。
    stackee_draw_bar(&canvas, stackee_selftest_bar(bar_index), icons, &font);
    printf("all %u\n", stackee_crc32(0, fb, sizeof(fb)));
    return 0;
}
