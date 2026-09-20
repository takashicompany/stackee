#include "stackee_touch.h"

#include <stdio.h>
#include <stdlib.h>      // strtol (touch.inject の xy を読む)
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "stackee_board.h"
#include "stackee_console.h"
#include "stackee_report_queue.h"
#include "stackee_touch_core.h"

static const char *TAG = "touch";

#define FT_ADDR        0x38
#define FT_REG_DATA    0x00
#define FT_REG_VENDOR  0xA8
#define FT_READ_LEN    16       // 3 + 6*2 = 15 バイトあれば 2 点ぶん足りる

// 読み出しの周期 [ms]。移植元の update_interval=5 と同じ。
#define TOUCH_PERIOD_MS 5
// タスクは CPU0。入力タスク (CPU1・最高優先度) と取り合わない。
#define TOUCH_TASK_CPU  0
#define TOUCH_TASK_PRIO 3

static struct {
    i2c_master_dev_handle_t  dev;
    bool                     present;
    uint8_t                  vendor;
    stackee_touch_state_t    state;
    SemaphoreHandle_t        lock;   // touch タスクと console (inject) の間
    uint32_t reads, read_fails;
    uint32_t moves, scrolls, clicks;
    int32_t  moved_x, moved_y;
    int      last_x, last_y;
    uint32_t loop_max_us, loop_n;
    // クリックの保持 (押してから click_hold_ms たったら離す)
    uint8_t  held_button;
    int64_t  release_at_us;
} tp;

static bool take(void) {
    return tp.lock != NULL && xSemaphoreTake(tp.lock, pdMS_TO_TICKS(20)) == pdTRUE;
}

static void give(void) {
    if (tp.lock != NULL) {
        xSemaphoreGive(tp.lock);
    }
}

// ---------------------------------------------------------------------------
// レポートの送出
// ---------------------------------------------------------------------------
// report_mouse_t と同じ並び (buttons, x, y, v, h) の 5 バイト。
// ★ QMK の構造体を直接使わない。quantum.h の BIT32/BIT64 が ESP-IDF の
//   esp_bit_defs.h とぶつかるので、この翻訳単位に QMK を持ち込まない
//   (qmk_port.h の注意書きと同じ理由)。
typedef struct __attribute__((packed)) {
    uint8_t buttons;
    int8_t  x, y, v, h;
} touch_mouse_report_t;

static void send_mouse(uint8_t buttons, int dx, int dy, int v, int h) {
    touch_mouse_report_t rep = {
        .buttons = buttons,
        .x = (int8_t)dx, .y = (int8_t)dy,
        .v = (int8_t)v,  .h = (int8_t)h,
    };
    stackee_report_queue_push(STACKEE_REPORT_MOUSE, &rep, sizeof(rep));
}

// core が出したイベントを送信キューへ流す。ロックを持ったまま呼ぶ。
static int emit(const stackee_touch_ev_t *evs, int n) {
    int sent = 0;
    for (int i = 0; i < n; i++) {
        switch (evs[i].kind) {
            case STACKEE_TOUCH_EV_MOVE:
                send_mouse(tp.held_button, evs[i].dx, evs[i].dy, 0, 0);
                tp.moves++;
                tp.moved_x += evs[i].dx;
                tp.moved_y += evs[i].dy;
                sent++;
                break;
            case STACKEE_TOUCH_EV_SCROLL:
                send_mouse(tp.held_button, 0, 0, evs[i].v, evs[i].h);
                tp.scrolls++;
                sent++;
                break;
            case STACKEE_TOUCH_EV_CLICK: {
                // ★ 押す → click_hold_ms → 離す。移植元が実機で
                //   「5/5 届く」を確かめた 120ms 保持をそのまま使う。
                uint8_t mask = (evs[i].button == 2) ? 0x02 : 0x01;
                tp.held_button = mask;
                send_mouse(mask, 0, 0, 0, 0);
                tp.clicks++;
                sent++;
                tp.release_at_us = esp_timer_get_time() +
                                   (int64_t)tp.state.cfg.click_hold_ms * 1000;
                break;
            }
            default:
                break;
        }
    }
    return sent;
}

// 保持中のクリックを離す時刻が来ていたら離す。ロックを持ったまま呼ぶ。
static int release_due(int64_t now_us) {
    if (tp.held_button == 0 || now_us < tp.release_at_us) {
        return 0;
    }
    tp.held_button = 0;
    send_mouse(0, 0, 0, 0, 0);
    tp.clicks++;
    return 1;
}

// ---------------------------------------------------------------------------
// FT6336
// ---------------------------------------------------------------------------
static bool ft_read(uint8_t reg, uint8_t *buf, size_t len) {
    if (tp.dev == NULL) {
        return false;
    }
    return stackee_board_i2c_write_read(tp.dev, &reg, 1, buf, len) == ESP_OK;
}

static bool ft_probe(void) {
    uint8_t vid = 0;
    if (!ft_read(FT_REG_VENDOR, &vid, 1)) {
        return false;
    }
    tp.vendor = vid;
    return true;
}

// ---------------------------------------------------------------------------
// タスク
// ---------------------------------------------------------------------------
static void touch_task(void *arg) {
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint8_t buf[FT_READ_LEN];
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        if (take()) {
            release_due(t0);
            tp.reads++;
            if (ft_read(FT_REG_DATA, buf, sizeof(buf))) {
                int x = 0, y = 0, count = 0;
                bool have = stackee_touch_parse(buf, &x, &y, &count);
                if (have) {
                    tp.last_x = x;
                    tp.last_y = y;
                } else {
                    tp.last_x = -1;
                    tp.last_y = -1;
                }
                stackee_touch_ev_t evs[STACKEE_TOUCH_EV_MAX];
                int n = stackee_touch_step(&tp.state, have, x, y,
                                           (uint32_t)(t0 / 1000), evs,
                                           STACKEE_TOUCH_EV_MAX);
                emit(evs, n);
            } else {
                // ★ 読めなかった回は「何もしない」。離した扱いにすると、
                //   I2C が一瞬混んだだけでポインタが飛ぶ。
                tp.read_fails++;
            }
            uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            if (us > tp.loop_max_us) {
                tp.loop_max_us = us;
            }
            tp.loop_n++;
            give();
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(TOUCH_PERIOD_MS));
    }
}

// ---------------------------------------------------------------------------
// console
// ---------------------------------------------------------------------------
static size_t reply_status(long id, char *buf, size_t cap) {
    stackee_touch_stats_t st;
    stackee_touch_stats(&st);
    return (size_t)snprintf(
        buf, cap,
        "{\"id\":%ld,\"ok\":1,\"present\":%s,\"vendor\":%u,"
        "\"reads\":%lu,\"read_fails\":%lu,\"points\":%lu,\"releases\":%lu,"
        "\"taps_left\":%lu,\"taps_right\":%lu,\"taps_rejected\":%lu,"
        "\"moves\":%lu,\"scrolls\":%lu,\"clicks\":%lu,"
        "\"moved_x\":%ld,\"moved_y\":%ld,\"last_x\":%d,\"last_y\":%d,"
        "\"scroll\":%s,\"loop_n\":%lu,\"loop_max_us\":%lu}",
        id, st.present ? "true" : "false", st.vendor,
        (unsigned long)st.reads, (unsigned long)st.read_fails,
        (unsigned long)st.points, (unsigned long)st.releases,
        (unsigned long)st.taps_left, (unsigned long)st.taps_right,
        (unsigned long)st.taps_rejected,
        (unsigned long)st.moves, (unsigned long)st.scrolls,
        (unsigned long)st.clicks,
        (long)st.moved_x, (long)st.moved_y, st.last_x, st.last_y,
        st.scroll_mode ? "true" : "false",
        (unsigned long)st.loop_n, (unsigned long)st.loop_max_us);
}

// `{"cmd":"touch.inject","xy":[x0,y0,x1,y1,...],"step_ms":5,"release":true}`
//
// ★ 既定は「左上から右下へ 8 点なぞる」。xy を省いても動くようにしてある
//   ので、check スクリプトから 1 行で撃てる。
#define INJECT_MAX_PAIRS 32

static int parse_xy(const char *line, int16_t *out, int max_pairs) {
    const char *at = stackee_console_value(line, "xy");
    if (at == NULL || *at != '[') {
        return -1;
    }
    at++;
    int n = 0;
    while (*at && *at != ']' && n < max_pairs * 2) {
        while (*at == ' ' || *at == ',') {
            at++;
        }
        if (*at == ']' || *at == '\0') {
            break;
        }
        char *end = NULL;
        long v = strtol(at, &end, 10);
        if (end == at) {
            return -1;
        }
        out[n++] = (int16_t)v;
        at = end;
    }
    return n / 2;
}

static size_t reply_inject(long id, const char *line, char *buf, size_t cap) {
    int16_t xy[INJECT_MAX_PAIRS * 2];
    int pairs = parse_xy(line, xy, INJECT_MAX_PAIRS);
    if (pairs < 0) {
        // 既定のなぞり: 生座標で右下へ 8 点。
        pairs = 8;
        for (int i = 0; i < pairs; i++) {
            xy[i * 2]     = (int16_t)(80 + i * 10);
            xy[i * 2 + 1] = (int16_t)(60 + i * 10);
        }
    }
    if (pairs == 0) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"badxy\"}", id);
    }
    long step = stackee_console_int(line, "step_ms", TOUCH_PERIOD_MS);
    if (step < 1 || step > 1000) {
        step = TOUCH_PERIOD_MS;
    }
    bool release = stackee_console_bool(line, "release", true);

    stackee_touch_stats_t before;
    stackee_touch_stats(&before);
    int64_t t0 = esp_timer_get_time();
    int sent = stackee_touch_inject(xy, pairs, (uint32_t)step, release);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    stackee_touch_stats_t after;
    stackee_touch_stats(&after);

    return (size_t)snprintf(
        buf, cap,
        "{\"id\":%ld,\"ok\":1,\"pairs\":%d,\"step_ms\":%ld,\"reports\":%d,"
        "\"moves\":%lu,\"scrolls\":%lu,\"clicks\":%lu,"
        "\"moved_x\":%ld,\"moved_y\":%ld,\"us\":%lu}",
        id, pairs, step, sent,
        (unsigned long)(after.moves - before.moves),
        (unsigned long)(after.scrolls - before.scrolls),
        (unsigned long)(after.clicks - before.clicks),
        (long)(after.moved_x - before.moved_x),
        (long)(after.moved_y - before.moved_y),
        (unsigned long)us);
}

static size_t touch_console(const char *cmd, const char *line, long id,
                            char *buf, size_t cap) {
    if (strcmp(cmd, "touch.status") == 0) {
        return reply_status(id, buf, cap);
    }
    if (strcmp(cmd, "touch.inject") == 0) {
        return reply_inject(id, line, buf, cap);
    }
    if (strcmp(cmd, "touch.scroll") == 0) {
        stackee_touch_set_scroll(stackee_console_bool(line, "on", true));
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"ok\":1,\"scroll\":%s}",
                                id, stackee_touch_scroll() ? "true" : "false");
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 口
// ---------------------------------------------------------------------------
int stackee_touch_inject(const int16_t *xy, int pairs, uint32_t step_ms,
                         bool release) {
    if (xy == NULL || pairs <= 0 || !take()) {
        return 0;
    }
    int sent = 0;
    // ★ 実時間ではなく「時刻を進めたことにする」。検証で 8 点 × 5 ms
    //   待たされないし、タップ判定の境目 (10 / 200 ms) を 1 ms 単位で狙える。
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    stackee_touch_ev_t evs[STACKEE_TOUCH_EV_MAX];
    for (int i = 0; i < pairs; i++) {
        int n = stackee_touch_step(&tp.state, true, xy[i * 2], xy[i * 2 + 1],
                                   now, evs, STACKEE_TOUCH_EV_MAX);
        sent += emit(evs, n);
        now += step_ms;
    }
    if (release) {
        int n = stackee_touch_step(&tp.state, false, 0, 0, now, evs,
                                   STACKEE_TOUCH_EV_MAX);
        sent += emit(evs, n);
    }
    give();
    return sent;
}

void stackee_touch_set_scroll(bool on) {
    if (!take()) {
        return;
    }
    tp.state.scroll_mode = on;
    if (!on) {
        stackee_touch_reset_motion(&tp.state);
    }
    give();
}

bool stackee_touch_scroll(void) {
    return tp.state.scroll_mode;
}

void stackee_touch_stats(stackee_touch_stats_t *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->present = tp.present;
    out->vendor = tp.vendor;
    out->reads = tp.reads;
    out->read_fails = tp.read_fails;
    out->points = tp.state.points;
    out->releases = tp.state.releases;
    out->taps_left = tp.state.taps_left;
    out->taps_right = tp.state.taps_right;
    out->taps_rejected = tp.state.taps_rejected;
    out->moves = tp.moves;
    out->scrolls = tp.scrolls;
    out->clicks = tp.clicks;
    out->moved_x = tp.moved_x;
    out->moved_y = tp.moved_y;
    out->last_x = tp.last_x;
    out->last_y = tp.last_y;
    out->scroll_mode = tp.state.scroll_mode;
    out->loop_max_us = tp.loop_max_us;
    out->loop_n = tp.loop_n;
}

esp_err_t stackee_touch_start(void) {
    stackee_touch_cfg_t cfg;
    stackee_touch_cfg_default(&cfg);
    stackee_touch_state_init(&tp.state, &cfg);
    tp.last_x = -1;
    tp.last_y = -1;
    if (tp.lock == NULL) {
        tp.lock = xSemaphoreCreateMutex();
    }
    // ★ console にはタッチが居なくても登録する。居ないことを
    //   `touch.status` の present=false で外から読めるようにするため。
    stackee_console_register(touch_console);

    esp_err_t err = stackee_board_i2c_add(FT_ADDR, &tp.dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FT6336 を I2C に足せない (%s)", esp_err_to_name(err));
        return err;
    }
    if (!ft_probe()) {
        ESP_LOGW(TAG, "FT6336 @0x%02X が応答しない。タッチ無しで続ける", FT_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    tp.present = true;
    if (xTaskCreatePinnedToCore(touch_task, "touch", 3072, NULL,
                                TOUCH_TASK_PRIO, NULL, TOUCH_TASK_CPU)
            != pdPASS) {
        ESP_LOGE(TAG, "touch タスクを作れない");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "タッチ FT6336 @0x%02X (vendor 0x%02X) 有効 / %d ms 周期",
             FT_ADDR, tp.vendor, TOUCH_PERIOD_MS);
    return ESP_OK;
}
