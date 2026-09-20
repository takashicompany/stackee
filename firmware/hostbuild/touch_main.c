// 段階 4: タッチパッドの判定を Mac 上でそのまま走らせる入り口。
//
// main/stackee_touch_core.c は ESP-IDF に一切依存していないので、
// ホストの libc だけでビルドできる。tools/test_touch_host.py が、
// 現行 CircuitPython 版 (firmware/kmk/stackee_touch.py の TouchpadMouse) に
// **同じ座標列**を流して、出てくる動き・クリックを 1 つずつ突き合わせる。
//
// 台本 (標準入力、1 行 1 手):
//   s 1|0            スクロールモードの入り切り
//   p <x> <y> <dt>   接触点を 1 つ流す。dt [ms] だけ時計を進めてから
//   r <dt>           離す
//   x <hex16>        FT6336 の生 16 バイトを 16 進で流す (解析のテスト)
//
// 出す行 (タブ区切り):
//   move   <dx> <dy>
//   scroll <v> <h>
//   click  <button>          1 = 左 / 2 = 右
//   parse  <have> <x> <y> <count>
//   stats  <points> <releases> <taps_left> <taps_right> <taps_rejected>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_touch_core.h"

static stackee_touch_state_t g_state;
static uint32_t g_now;

static void emit(const stackee_touch_ev_t *evs, int n) {
    for (int i = 0; i < n; i++) {
        switch (evs[i].kind) {
            case STACKEE_TOUCH_EV_MOVE:
                printf("move\t%d\t%d\n", evs[i].dx, evs[i].dy);
                break;
            case STACKEE_TOUCH_EV_SCROLL:
                printf("scroll\t%d\t%d\n", evs[i].v, evs[i].h);
                break;
            case STACKEE_TOUCH_EV_CLICK:
                printf("click\t%u\n", evs[i].button);
                break;
            default:
                break;
        }
    }
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

int main(int argc, char **argv) {
    stackee_touch_cfg_t cfg;
    stackee_touch_cfg_default(&cfg);
    // 引数で既定を上書きできる (回転や反転の違いを試すため)。
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "rotation=", 9) == 0) {
            cfg.rotation = atoi(argv[i] + 9);
        } else if (strncmp(argv[i], "sensitivity=", 12) == 0) {
            cfg.sensitivity = atoi(argv[i] + 12);
        } else if (strncmp(argv[i], "right_click_zone=", 17) == 0) {
            cfg.right_click_zone = (float)atof(argv[i] + 17);
        } else if (strncmp(argv[i], "pointer_invert_x=", 17) == 0) {
            cfg.pointer_invert_x = atoi(argv[i] + 17) != 0;
        } else if (strncmp(argv[i], "pointer_invert_y=", 17) == 0) {
            cfg.pointer_invert_y = atoi(argv[i] + 17) != 0;
        } else if (strncmp(argv[i], "invert_x=", 9) == 0) {
            cfg.invert_x = atoi(argv[i] + 9) != 0;
        } else if (strncmp(argv[i], "invert_y=", 9) == 0) {
            cfg.invert_y = atoi(argv[i] + 9) != 0;
        } else if (strncmp(argv[i], "tap_max_move=", 13) == 0) {
            cfg.tap_max_move = atoi(argv[i] + 13);
        } else if (strncmp(argv[i], "smoothing=", 10) == 0) {
            cfg.smoothing = (float)atof(argv[i] + 10);
        } else if (strncmp(argv[i], "scroll_sensitivity=", 19) == 0) {
            cfg.scroll_sensitivity = (float)atof(argv[i] + 19);
        }
    }
    stackee_touch_state_init(&g_state, &cfg);
    // ★ 0 は「触れていない」の印なので、時計は 1 から始める。
    g_now = 1;

    char line[256];
    stackee_touch_ev_t evs[STACKEE_TOUCH_EV_MAX];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char kind = line[0];
        if (kind == 's') {
            g_state.scroll_mode = atoi(line + 1) != 0;
            if (!g_state.scroll_mode) {
                stackee_touch_reset_motion(&g_state);
            }
        } else if (kind == 'p') {
            int x = 0, y = 0, dt = 0;
            if (sscanf(line + 1, "%d %d %d", &x, &y, &dt) != 3) {
                continue;
            }
            g_now += (uint32_t)dt;
            int n = stackee_touch_step(&g_state, true, x, y, g_now, evs,
                                       STACKEE_TOUCH_EV_MAX);
            emit(evs, n);
        } else if (kind == 'r') {
            int dt = 0;
            if (sscanf(line + 1, "%d", &dt) != 1) {
                dt = 0;
            }
            g_now += (uint32_t)dt;
            int n = stackee_touch_step(&g_state, false, 0, 0, g_now, evs,
                                       STACKEE_TOUCH_EV_MAX);
            emit(evs, n);
        } else if (kind == 'x') {
            uint8_t buf[16];
            memset(buf, 0, sizeof(buf));
            const char *p = line + 1;
            while (*p == ' ') { p++; }
            for (int i = 0; i < 16; i++) {
                int hi = hexval(p[i * 2]);
                int lo = hexval(p[i * 2 + 1]);
                if (hi < 0 || lo < 0) {
                    break;
                }
                buf[i] = (uint8_t)((hi << 4) | lo);
            }
            int x = -1, y = -1, count = 0;
            bool have = stackee_touch_parse(buf, &x, &y, &count);
            printf("parse\t%d\t%d\t%d\t%d\n", have ? 1 : 0, x, y, count);
        }
    }
    printf("stats\t%u\t%u\t%u\t%u\t%u\n", g_state.points, g_state.releases,
           g_state.taps_left, g_state.taps_right, g_state.taps_rejected);
    return 0;
}
