#include "stackee_touch_core.h"

#include <math.h>
#include <string.h>

// ★ 端数の丸め方だけ移植元と違う。
//   Python の round() は「偶数丸め」(round(0.5) == 0)、こちらは
//   「0 から遠いほうへ」(0.5 -> 1)。1 カウントぶんの差なので操作感には
//   出ないが、ホスト側の期待値を作るときはこちらに合わせること
//   (tools/test_touch_host.py も同じ丸めで書いてある)。
static int round_away(float v) {
    return (int)((v >= 0.0f) ? floorf(v + 0.5f) : ceilf(v - 0.5f));
}

static int clamp127(int v) {
    if (v > 127)  { return 127; }
    if (v < -127) { return -127; }
    return v;
}

void stackee_touch_cfg_default(stackee_touch_cfg_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->rotation = 270;
    out->raw_width = 320;
    out->raw_height = 240;
    out->sensitivity = 4;
    out->deadzone = 0.0f;
    out->smoothing = 0.4f;
    out->tap_time_ms = 200;
    out->tap_min_time_ms = 10;
    out->tap_max_move = 30;
    out->click_hold_ms = 120;
    out->right_click_zone = 0.3f;
    out->scroll_sensitivity = 0.05f;
    out->invert_x = false;
    out->invert_y = false;
    out->pointer_invert_x = true;
    out->pointer_invert_y = true;
}

void stackee_touch_rotate_delta(const stackee_touch_cfg_t *cfg,
                                int dx, int dy, int *out_dx, int *out_dy) {
    switch (cfg->rotation) {
        case 90:  *out_dx = -dy; *out_dy = dx;  break;
        case 180: *out_dx = -dx; *out_dy = -dy; break;
        case 270: *out_dx = dy;  *out_dy = -dx; break;
        default:  *out_dx = dx;  *out_dy = dy;  break;
    }
}

int stackee_touch_screen_x(const stackee_touch_cfg_t *cfg, int x, int y) {
    switch (cfg->rotation) {
        case 90:  return cfg->raw_height - 1 - y;
        case 180: return cfg->raw_width - 1 - x;
        case 270: return y;
        default:  return x;
    }
}

static int screen_width(const stackee_touch_cfg_t *cfg) {
    return (cfg->rotation == 90 || cfg->rotation == 270) ? cfg->raw_height
                                                         : cfg->raw_width;
}

void stackee_touch_state_init(stackee_touch_state_t *st,
                              const stackee_touch_cfg_t *cfg) {
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    if (cfg != NULL) {
        st->cfg = *cfg;
    } else {
        stackee_touch_cfg_default(&st->cfg);
    }
    st->start_screen_x = -1;
}

void stackee_touch_reset_motion(stackee_touch_state_t *st) {
    st->touching = false;
    st->last_x = 0;
    st->last_y = 0;
    st->smoothed_dx = 0.0f;
    st->smoothed_dy = 0.0f;
    st->scroll_acc_x = 0.0f;
    st->scroll_acc_y = 0.0f;
}

// タップ位置から左 / 右クリックを決める。
// ★ 指の位置もポインタと同じ向きで見る。パネルが 180 度ずれているぶんを
//   位置にも掛けないと、「右 30%」が実際には左 30% になる。
static uint8_t tap_button(const stackee_touch_state_t *st) {
    float zone = st->cfg.right_click_zone;
    if (zone == 0.0f || st->start_screen_x < 0) {
        return 1;               // 左
    }
    int w = screen_width(&st->cfg);
    int x = st->start_screen_x;
    if (st->cfg.pointer_invert_x) {
        x = w - 1 - x;
    }
    if (zone > 0.0f) {
        return ((float)x >= (float)w * (1.0f - zone)) ? 2 : 1;
    }
    return ((float)x <= (float)w * (-zone)) ? 2 : 1;
}

static void push_ev(stackee_touch_ev_t *out, int out_max, int *n,
                    const stackee_touch_ev_t *ev) {
    if (*n < out_max) {
        out[(*n)++] = *ev;
    }
}

int stackee_touch_step(stackee_touch_state_t *st, bool have_point,
                       int x, int y, uint32_t now_ms,
                       stackee_touch_ev_t *out, int out_max) {
    int n = 0;
    if (out == NULL || out_max <= 0) {
        return 0;
    }

    if (!have_point) {
        // ---- 離れた ----
        if (st->touch_start_ms != 0) {
            uint32_t held = now_ms - st->touch_start_ms;
            if ((int)held >= st->cfg.tap_min_time_ms &&
                (int)held <= st->cfg.tap_time_ms &&
                st->total_move <= (float)st->cfg.tap_max_move) {
                stackee_touch_ev_t ev = {0};
                ev.kind = STACKEE_TOUCH_EV_CLICK;
                ev.button = tap_button(st);
                if (ev.button == 2) {
                    st->taps_right++;
                } else {
                    st->taps_left++;
                }
                push_ev(out, out_max, &n, &ev);
            } else {
                st->taps_rejected++;
            }
            st->touch_start_ms = 0;
            st->start_screen_x = -1;
            st->total_move = 0.0f;
            st->releases++;
        }
        stackee_touch_reset_motion(st);
        return n;
    }

    // ---- 触れている ----
    st->points++;
    if (st->touch_start_ms == 0) {
        // ★ 0 は「触れていない」の印なので、時刻が本当に 0 のときは 1 にずらす。
        st->touch_start_ms = (now_ms == 0) ? 1u : now_ms;
        st->start_screen_x = stackee_touch_screen_x(&st->cfg, x, y);
        st->start_raw_x = x;
        st->start_raw_y = y;
        st->total_move = 0.0f;
    } else {
        // ★ タップ判定の移動量は「開始点からの実移動距離 (最大値)」。
        //   毎回の |dx|+|dy| を足し込むと、置いただけの揺れが積み上がって
        //   本物のタップまで却下される (移植元の実測)。
        int dispx = x - st->start_raw_x;
        int dispy = y - st->start_raw_y;
        if (dispx < 0) { dispx = -dispx; }
        if (dispy < 0) { dispy = -dispy; }
        float disp = (float)(dispx + dispy);
        if (disp > st->total_move) {
            st->total_move = disp;
        }
    }

    if (st->touching) {
        int raw_dx = x - st->last_x;
        int raw_dy = y - st->last_y;

        int sdx = 0, sdy = 0;
        stackee_touch_rotate_delta(&st->cfg, raw_dx, raw_dy, &sdx, &sdy);
        if (st->cfg.invert_x) { sdx = -sdx; }
        if (st->cfg.invert_y) { sdy = -sdy; }
        float fdx = (float)sdx * (float)st->cfg.sensitivity;
        float fdy = (float)sdy * (float)st->cfg.sensitivity;

        // 指数移動平均。動き出しは即座に反映する (軸ごと独立)。
        if (fabsf(st->smoothed_dx) < 0.1f) {
            st->smoothed_dx = fdx;
        } else {
            st->smoothed_dx = st->smoothed_dx * (1.0f - st->cfg.smoothing) +
                              fdx * st->cfg.smoothing;
        }
        if (fabsf(st->smoothed_dy) < 0.1f) {
            st->smoothed_dy = fdy;
        } else {
            st->smoothed_dy = st->smoothed_dy * (1.0f - st->cfg.smoothing) +
                              fdy * st->cfg.smoothing;
        }
        if (fabsf(st->smoothed_dx) < st->cfg.deadzone) { st->smoothed_dx = 0.0f; }
        if (fabsf(st->smoothed_dy) < st->cfg.deadzone) { st->smoothed_dy = 0.0f; }

        int dx = round_away(st->smoothed_dx);
        int dy = round_away(st->smoothed_dy);

        if (st->scroll_mode) {
            // スクロール。端数を貯めて整数になったぶんだけ送る。
            // ★ 共有の dx/dy をそのまま使う (向きは移植元の実機で正しかった)。
            st->scroll_acc_x += (float)dx * st->cfg.scroll_sensitivity;
            st->scroll_acc_y += (float)dy * st->cfg.scroll_sensitivity;
            int sx = (int)st->scroll_acc_x;      // 0 方向へ切り捨て (Python の int())
            int sy = (int)st->scroll_acc_y;
            if (sx != 0 || sy != 0) {
                stackee_touch_ev_t ev = {0};
                ev.kind = STACKEE_TOUCH_EV_SCROLL;
                if (sx != 0) {
                    ev.h = (int8_t)clamp127(sx);
                    st->scroll_acc_x -= (float)sx;
                }
                if (sy != 0) {
                    // 縦は符号を反転させて自然な向きにする (移植元と同じ)。
                    ev.v = (int8_t)clamp127(-sy);
                    st->scroll_acc_y -= (float)sy;
                }
                push_ev(out, out_max, &n, &ev);
            }
        } else {
            // ★ パネルの 180 度ぶんはここでだけ打ち消す。
            //   dx/dy 自体は書き換えないので、スクロール側は影響を受けない。
            int px = st->cfg.pointer_invert_x ? -dx : dx;
            int py = st->cfg.pointer_invert_y ? -dy : dy;
            if (px != 0 || py != 0) {
                stackee_touch_ev_t ev = {0};
                ev.kind = STACKEE_TOUCH_EV_MOVE;
                ev.dx = (int8_t)clamp127(px);
                ev.dy = (int8_t)clamp127(py);
                push_ev(out, out_max, &n, &ev);
            }
        }
    }

    st->touching = true;
    st->last_x = x;
    st->last_y = y;
    return n;
}

bool stackee_touch_parse(const uint8_t *buf16, int *out_x, int *out_y,
                         int *out_count) {
    if (buf16 == NULL) {
        return false;
    }
    int count = buf16[2] & 0x0F;    // レジスタ 0x02 = 接触点数
    if (count > 2) {
        count = 2;                  // 2 点ぶんしか読んでいない
    }
    if (out_count != NULL) {
        *out_count = count;
    }
    for (int i = 0; i < count; i++) {
        int o = i * 6 + 3;
        uint8_t xh = buf16[o], xl = buf16[o + 1];
        uint8_t yh = buf16[o + 2], yl = buf16[o + 3];
        if (xh == 0xFF && xl == 0xFF && yh == 0xFF && yl == 0xFF) {
            continue;               // 幽霊点
        }
        if (out_x != NULL) { *out_x = ((xh & 0x0F) << 8) | xl; }
        if (out_y != NULL) { *out_y = ((yh & 0x0F) << 8) | yl; }
        return true;
    }
    return false;
}
