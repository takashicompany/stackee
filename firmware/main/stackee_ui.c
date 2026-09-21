#include "stackee_ui.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "stackee_assets.h"
#include "stackee_ble.h"
#include "stackee_board.h"
#include "stackee_console.h"
#include "stackee_crc32.h"
#include "stackee_draw.h"
#include "stackee_faceanim.h"
#include "stackee_font16.h"
#include "stackee_hid_dest.h"
#include "stackee_icons.h"
#include "stackee_input.h"
#include "stackee_lcd.h"
#include "stackee_perf.h"
#include "stackee_selftest.h"
#include "stackee_talksm.h"
#include "stackee_volume.h"

static const char *TAG = "ui";

#define FACE_SIZE        STACKEE_FACE_SIZE
#define FACE_X           0
// stackee_face.py の TileGrid: y=(height-size)//2 + 10 = (320-240)/2+10 = 50
#define FACE_Y           50
// ★ 元絵と faces.bin は 240x240 のまま。画面に出すのは上 29 行・下 11 行を
//   捨てた 240x200 で、空いた 40 px は字幕の帯 (3 行) に回る。顔は y=50..249。
//   29 / 11 は 32 コマ全部の余白の最小値で、**1 画素も落ちない**
//   (stackee_draw.h の ★★)。
#define FACE_TRIM        STACKEE_FACE_TRIM_TOP
#define FACE_ROWS        STACKEE_FACE_ROWS
#define FACE_COUNT       STACKEE_FACE_MAX_COUNT
// 展開した直後の丈 (シートの検証はここ。素材の形は変わっていない)。
#define FACE_SHEET_BYTES ((size_t)FACE_SIZE * FACE_SIZE / 2 * FACE_COUNT)
// 切り詰めたあと PSRAM に残す丈。
#define FACE_KEPT_BYTES  ((size_t)FACE_SIZE * FACE_ROWS / 2 * FACE_COUNT)
#define CHANGES_BYTES    ((size_t)FACE_COUNT * FACE_COUNT * 4)

#define UI_TICK_MS       5
#define BATTERY_EVERY_MS 10000
#define PWRKEY_EVERY_MS  200
#define AXP_REG_IRQ_STATUS1  0x49   // bit3 短押し / bit2 長押し / bit1 離す / bit0 押す (RW1C)
#define AXP_IRQ_PWRON_SHORT  0x08
#define AXP_IRQ_PWRON_LONG   0x04

#define STATUS_GLYPHS    "0123456789%-? "

// 帯に出す文字列。**改行区切りで最大 3 行**。1 行 15 全角 = 45 バイト、
// 上限は 1 ページ 63 バイト x 3 + 改行 2 + NUL
// (stackee_talksm の STACKEE_TALK_SUB_BAND_MAX と揃えてある)。
#define SUB_TEXT_MAX     STACKEE_TALK_SUB_BAND_MAX

#define SELFTEST_STEPS   (FACE_COUNT + STACKEE_SELFTEST_BARS)

typedef enum { SELFTEST_IDLE = 0, SELFTEST_RUNNING, SELFTEST_DONE } selftest_state_t;

static struct {
    uint8_t *faces;             // 4bpp シート (PSRAM)
    uint8_t *changes;           // 差分 bbox
    uint8_t *icons;             // 2bpp アイコンシート
    size_t   faces_len, changes_len, icons_len;
    uint32_t faces_crc, changes_crc, icons_crc;
    bool     have_font;
    stackee_bdf_font_t font;
    // 字幕用の 16 px 日本語フォント (assets/font16.bin を PSRAM へ丸ごと)。
    uint8_t *font16_data;
    size_t   font16_len;
    uint32_t font16_crc;
    stackee_font16_t font16;
    bool     have_font16;
    stackee_face_cases_t cases;
    stackee_face_view_t view;

    stackee_canvas_t canvas;
    SemaphoreHandle_t lock;     // フレームバッファと下の状態を守る
    TaskHandle_t task;

    // いま出しているバーの中身 (変わった時だけ描き直す)。
    stackee_bar_state_t shown;
    bool shown_valid;
    bool bar_forced;
    stackee_bar_state_t forced;

    // 字幕。★ 書き手 (audio タスク) と ui タスクの間だけを守る小さな錠。
    //   ui.lock (描画の錠) を会話の状態機械に取らせないためのもの。
    //   錠の順番は必ず ui.lock -> sub_lock (逆に取る道は無い)。
    SemaphoreHandle_t sub_lock;
    char     sub_want[SUB_TEXT_MAX];
    char     sub_shown[SUB_TEXT_MAX];
    bool     sub_shown_valid;
    uint32_t sub_paints;

    // 外から渡される値 (段階 3 で音声・Wi-Fi が書く)。
    _Atomic int volume;
    char wifi[16];
    char screen[256];
    _Atomic bool talk_recording;
    _Atomic bool talk_busy;
    _Atomic bool talk_speaking;

    uint32_t last_key_events;
    int64_t  battery_at_us;
    int      battery;
    int      battery_log_tick;
    int64_t  pwrkey_at_us;
    bool     pwrkey_seen_first;
    int      pwrkey_boot_irq;
    int      pwrkey_long, pwrkey_short;
    bool     charging;

    atomic_int selftest;        // selftest_state_t
    int        selftest_step;
    uint32_t   selftest_ms;
    int64_t    selftest_started;
    uint32_t   face_crc[FACE_COUNT];
    uint32_t   bar_crc[STACKEE_SELFTEST_BARS];

    atomic_bool ready;
    atomic_bool paused;         // 段階 4: 撮影中は描かない
    uint32_t face_frames;       // 描き切った顔の枚数
} ui;

// ---------------------------------------------------------------------------
// 描画 (すべて ui.lock の中で呼ぶ)
// ---------------------------------------------------------------------------
// ★ changes.bin の bbox は元の 240x240 の座標。切り詰めたぶんだけ上へ寄せ、
//   捨てた行にかかる部分は落とす。素材 (changes.bin) は変えていない。
static void paint_face_rect(const stackee_face_rect_t *rect) {
    if (ui.faces == NULL || rect->w <= 0 || rect->h <= 0) {
        return;
    }
    int sy = rect->y - FACE_TRIM;
    int h = rect->h;
    if (sy < 0) { h += sy; sy = 0; }
    if (sy + h > FACE_ROWS) { h = FACE_ROWS - sy; }
    if (h <= 0) {
        return;                 // 捨てた行だけの矩形。描くものが無い
    }
    stackee_draw_face(&ui.canvas, ui.faces, FACE_SIZE, FACE_ROWS, rect->frame,
                      FACE_X, FACE_Y, rect->x, sy, rect->w, h);
    stackee_lcd_mark_rows(FACE_Y + sy, h);
}

static void paint_face_full(int frame) {
    if (ui.faces == NULL) {
        return;
    }
    stackee_draw_face(&ui.canvas, ui.faces, FACE_SIZE, FACE_ROWS, frame,
                      FACE_X, FACE_Y, 0, 0, FACE_SIZE, FACE_ROWS);
    stackee_lcd_mark_rows(FACE_Y, FACE_ROWS);
}

static void paint_bar(const stackee_bar_state_t *state) {
    int64_t t0 = esp_timer_get_time();
    stackee_draw_bar(&ui.canvas, state, ui.icons,
                     ui.have_font ? &ui.font : NULL);
    stackee_lcd_mark_rows(0, STACKEE_BAR_AREA_HEIGHT);
    stackee_perf_sample(STACKEE_PERF_UI_BAR, (uint32_t)(esp_timer_get_time() - t0));
}

// 字幕の帯 (y=250..319)。★ 顔 (y=50..249) と領域が重ならないので、
//   顔の差分描画と互いに描き直さない。塗るのは 240x70 = 33.6 KB だけ。
static void paint_subtitle(const char *text) {
    int64_t t0 = esp_timer_get_time();
    stackee_draw_subtitle(&ui.canvas, ui.have_font16 ? &ui.font16 : NULL, text);
    stackee_lcd_mark_rows(STACKEE_SUB_Y, STACKEE_SUB_HEIGHT);
    stackee_perf_sample(STACKEE_PERF_UI_SUB, (uint32_t)(esp_timer_get_time() - t0));
    snprintf(ui.sub_shown, sizeof(ui.sub_shown), "%s", text ? text : "");
    ui.sub_shown_valid = true;
    ui.sub_paints++;
}

// 顔を frame へ「差分で」寄せ切る (face.set / ui.selftest 用)。
static void step_face_to(int frame) {
    stackee_face_rect_t rect;
    int guard = 0;
    int64_t t0 = esp_timer_get_time();
    while (stackee_face_view_step_to(&ui.view, frame, &rect)) {
        paint_face_rect(&rect);
        // ★ 1 回の遷移は最大 15 周 (240 行 / 16 行。捨てた行も周回に入る)。
        //   ここに入ってきたとき
        //   前の遷移が途中なら、それを終わらせてから新しい遷移を始めるので
        //   2 回ぶん見ておく。それを超えたら changes.bin が壊れている。
        if (++guard > 2 * (FACE_SIZE / STACKEE_FACE_CHUNK_ROWS) + 4) {
            break;
        }
    }
    if (guard > 0) {
        stackee_perf_sample(STACKEE_PERF_UI_FACE,
                            (uint32_t)(esp_timer_get_time() - t0));
        ui.face_frames++;
    }
}

// ---------------------------------------------------------------------------
// いまの状態を集める
// ---------------------------------------------------------------------------
void stackee_ui_set_volume(int percent) {
    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }
    atomic_store(&ui.volume, percent);
}

void stackee_ui_set_talk(bool recording, bool busy, bool speaking) {
    atomic_store(&ui.talk_recording, recording);
    atomic_store(&ui.talk_busy, busy);
    atomic_store(&ui.talk_speaking, speaking);
}

void stackee_ui_set_screen(const char *text) {
    if (text == NULL) {
        return;
    }
    // ★ 錠は取らない。ui タスクは読まない (console だけが読む) ので、
    //   ここで待たせると会話の状態機械が止まる。
    snprintf(ui.screen, sizeof(ui.screen), "%s", text);
}

const char *stackee_ui_screen(void) {
    return ui.screen;
}

void stackee_ui_set_subtitle(const char *utf8) {
    if (ui.sub_lock == NULL) {
        return;
    }
    // ★ 置くだけ。描くのは ui タスク。会話の状態機械 (audio タスク) を
    //   描画の都合で待たせない。
    if (xSemaphoreTake(ui.sub_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (utf8 == NULL) {
        ui.sub_want[0] = '\0';
    } else {
        snprintf(ui.sub_want, sizeof(ui.sub_want), "%s", utf8);
    }
    xSemaphoreGive(ui.sub_lock);
}

void stackee_ui_set_wifi(const char *state_name) {
    if (state_name == NULL) {
        return;
    }
    // 文字列は ui タスクしか読まないので、錠の中で書き換える。
    if (ui.lock != NULL && xSemaphoreTake(ui.lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(ui.wifi, sizeof(ui.wifi), "%s", state_name);
        xSemaphoreGive(ui.lock);
    }
}

void stackee_ui_pwrkey_stats(int *long_presses, int *short_presses, int *boot_irq) {
    if (long_presses)  { *long_presses = ui.pwrkey_long; }
    if (short_presses) { *short_presses = ui.pwrkey_short; }
    if (boot_irq)      { *boot_irq = ui.pwrkey_boot_irq; }
}

static void collect_bar(stackee_bar_state_t *out) {
    memset(out, 0, sizeof(*out));
    out->battery = ui.battery;
    out->charging = ui.charging;
    out->volume = atomic_load(&ui.volume);
    snprintf(out->wifi, sizeof(out->wifi), "%s", ui.wifi);
    // 送信先は「実際に使っている先」を出す (status の hid と同じ決め方)。
    out->link = (stackee_hid_dest_effective() == STACKEE_HID_USB)
                    ? STACKEE_LINK_USB : STACKEE_LINK_BLE;
    out->ble_connected = stackee_ble_connected();
}

static bool bar_changed(const stackee_bar_state_t *a, const stackee_bar_state_t *b) {
    return a->battery != b->battery || a->charging != b->charging ||
           a->volume != b->volume || a->link != b->link ||
           a->ble_connected != b->ble_connected ||
           strcmp(a->wifi, b->wifi) != 0;
}

// ---------------------------------------------------------------------------
// 自己テスト (ui タスクの上で 1 周 1 手ずつ進める)
// ---------------------------------------------------------------------------
// ★ 一気にやらない。32 表情 + バー 6 状態を 1 周 1 つずつ描くので、
//   走っている間もコンソールが答えられる = key.inject で打鍵の遅延を
//   同時に測れる (段階 2 の合否のひとつ)。
static uint32_t region_crc(int y, int h) {
    const uint8_t *fb = ui.canvas.fb;
    return stackee_crc32(0, fb + (size_t)y * ui.canvas.stride,
                         (size_t)h * ui.canvas.stride);
}

static void selftest_step(void) {
    int step = ui.selftest_step;
    if (step == 0) {
        // 出発点をそろえる: 背景 (白) → バー (代表の最後 = 起動時の既定) → 顔 0。
        stackee_draw_fill(&ui.canvas, 0, 0, ui.canvas.width, ui.canvas.height,
                          stackee_draw_rgb565(STACKEE_SCREEN_BG));
        stackee_lcd_mark_rows(0, ui.canvas.height);
        paint_bar(stackee_selftest_bar(STACKEE_SELFTEST_BARS - 1));
        paint_face_full(0);
        ui.view.current = 0;
        ui.view.target = -1;
        ui.face_crc[0] = region_crc(FACE_Y, FACE_ROWS);
    } else if (step < FACE_COUNT) {
        // 以後は changes.bin を使った差分で寄せる。全面で描いた絵と
        // 同じになることが、この検査でいちばん見たいところ。
        step_face_to(step);
        ui.face_crc[step] = region_crc(FACE_Y, FACE_ROWS);
    } else {
        int i = step - FACE_COUNT;
        if (i == 0) {
            paint_face_full(0);         // 顔を 0 に戻す (上段だけを見るので念のため)
            ui.view.current = 0;
            ui.view.target = -1;
        }
        paint_bar(stackee_selftest_bar(i));
        ui.bar_crc[i] = region_crc(0, STACKEE_BAR_AREA_HEIGHT);
    }
    stackee_lcd_flush();
    ui.selftest_step++;
    if (ui.selftest_step >= SELFTEST_STEPS) {
        ui.selftest_ms = (uint32_t)((esp_timer_get_time() - ui.selftest_started) / 1000);
        atomic_store(&ui.selftest, SELFTEST_DONE);
        // 普段の絵に戻す (検査のあと画面が固まったままにならないように)。
        ui.view.frozen = (ui.faces == NULL || ui.changes == NULL);
        ui.bar_forced = false;
        ui.shown_valid = false;
        // 自己テストの 1 手目で画面を白く塗っている。帯も消えているので
        // 「出しているもの」を忘れて描き直させる。
        ui.sub_shown_valid = false;
        paint_face_full(ui.view.current);
        stackee_lcd_flush();
    }
}

// ---------------------------------------------------------------------------
// ui タスク
// ---------------------------------------------------------------------------
static void ui_task(void *unused) {
    (void)unused;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UI_TICK_MS));
        if (xSemaphoreTake(ui.lock, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        // 段階 4: 撮影中は描かない (DESIGN.md §3 の「撮影中は ui を止めてよい」)。
        // ★ 錠は持ったまま抜ける。ここで錠を手放すと console の lcd.crc が
        //   撮影中の中途半端なフレームを読む。
        if (atomic_load(&ui.paused)) {
            xSemaphoreGive(ui.lock);
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        uint32_t now_ms = (uint32_t)(now_us / 1000);

        if (atomic_load(&ui.selftest) == SELFTEST_RUNNING) {
            selftest_step();
            xSemaphoreGive(ui.lock);
            continue;
        }

        // 打鍵を見張る。★ 入力タスクには触らない (数えた数を読むだけ)。
        stackee_input_stats_t input;
        stackee_input_stats(&input);
        if (input.key_events != ui.last_key_events) {
            ui.last_key_events = input.key_events;
            stackee_face_view_note_key(&ui.view, now_ms);
        }

        // ★ 電源ボタン。AXP2101 の IRQ 状態 (REG 0x49、RW1C) を 200 ms ごとに読む。
        //   bit2 = 長押し (OFFLEVEL=4 秒)、bit3 = 短押し。電源 IC の「長押しで
        //   切る」機能 (REG 0x22) はこの個体では再起動を長押しと誤検出して電源を
        //   落とすので使わない (README §19)。段階 1 = 検出して数えるだけ。
        //   起動直後の 1 回目は「再起動の前から立っていた印」なので、数えずに
        //   ログへ出して消す (再起動が長押しに化けるかを見るため)。
        if (now_us - ui.pwrkey_at_us >= PWRKEY_EVERY_MS * 1000) {
            ui.pwrkey_at_us = now_us;
            int irq = stackee_board_axp_read(AXP_REG_IRQ_STATUS1);
            if (irq > 0) {
                if (!ui.pwrkey_seen_first) {
                    ESP_LOGI(TAG, "起動時の電源ボタン IRQ 0x%02X (長押し=%d 短押し=%d)",
                             irq, (irq >> 2) & 1, (irq >> 3) & 1);
                    ui.pwrkey_boot_irq = irq;
                } else if (irq & (AXP_IRQ_PWRON_LONG | AXP_IRQ_PWRON_SHORT)) {
                    if (irq & AXP_IRQ_PWRON_LONG) { ui.pwrkey_long++; }
                    if (irq & AXP_IRQ_PWRON_SHORT) { ui.pwrkey_short++; }
                    ESP_LOGI(TAG, "電源ボタン IRQ 0x%02X (長押し 累計 %d / 短押し 累計 %d)",
                             irq, ui.pwrkey_long, ui.pwrkey_short);
                }
                // 読んだ印は消す (1 を書いた bit だけ消える)。設定レジスタではない。
                stackee_board_axp_write(AXP_REG_IRQ_STATUS1, (uint8_t)irq);
                // ★ 段階 2: 起動後に来た長押しなら電源を切る。起動直後に残っていた
                //   印 (再起動前のもの) では切らない。再起動 3 回で印が残らない
                //   ことは確認済み (README §19)。
                if (ui.pwrkey_seen_first && (irq & AXP_IRQ_PWRON_LONG)) {
                    ESP_LOGW(TAG, "長押し → 電源を切る");
                    stackee_volume_flush();
                    vTaskDelay(pdMS_TO_TICKS(50));
                    stackee_board_power_off();
                }
            }
            ui.pwrkey_seen_first = true;
        }

        // 電池は 10 秒に 1 回。I2C は数バイトだけ読む。
        if (now_us - ui.battery_at_us >= BATTERY_EVERY_MS * 1000) {
            ui.battery_at_us = now_us;
            ui.battery = stackee_board_battery_percent();
            int chg = stackee_board_charging();
            ui.charging = (chg > 0);
            // ★ 1 分に 1 回、% と電池電圧をログに残す (無線で % が減らない
            //   と言われた 2026-09-17 の確認用。リングは 16 KB ≒ 5 時間分)。
            if (++ui.battery_log_tick >= 6) {
                ui.battery_log_tick = 0;
                ESP_LOGI(TAG, "電池 %d%% %d mV chg=%d", ui.battery,
                         stackee_board_battery_mv(), chg);
            }
            // 音量は stackee_volume が持っている値をそのまま使う
            // (NVS を 10 秒ごとに読みに行かない)。
            atomic_store(&ui.volume, stackee_volume_percent());
        }

        stackee_bar_state_t want;
        if (ui.bar_forced) {
            want = ui.forced;
        } else {
            collect_bar(&want);
        }
        if (!ui.shown_valid || bar_changed(&want, &ui.shown)) {
            paint_bar(&want);
            ui.shown = want;
            ui.shown_valid = true;
        }

        // 字幕。★ 同じ文字列なら 1 画素も触らない。取れなければこの周は
        //   見送る (次の 5 ms でまた見る)。
        if (xSemaphoreTake(ui.sub_lock, 0) == pdTRUE) {
            char sub[SUB_TEXT_MAX];
            memcpy(sub, ui.sub_want, sizeof(sub));
            xSemaphoreGive(ui.sub_lock);
            if (!ui.sub_shown_valid || strcmp(sub, ui.sub_shown) != 0) {
                paint_subtitle(sub);
            }
        }

        stackee_face_inputs_t inputs = {
            .speaking = atomic_load(&ui.talk_speaking),
            .talk_recording = atomic_load(&ui.talk_recording),
            // ★ PC 側のプッシュトゥトーク (STK_MIC_KEY)。入力タスクが立てた
            //   印を読むだけ (打鍵の道には何も足さない)。
            .mic_held = stackee_input_mic_held(),
            .talk_busy = atomic_load(&ui.talk_busy),
            .camera_active = false,             // カメラは段階 4
        };
        stackee_face_rect_t rect;
        bool busy_before = stackee_face_view_busy(&ui.view);
        static int64_t face_started;
        if (!busy_before) {
            face_started = now_us;
        }
        if (stackee_face_view_tick(&ui.view, &inputs, now_ms, &rect)) {
            paint_face_rect(&rect);
            if (!stackee_face_view_busy(&ui.view)) {
                stackee_perf_sample(STACKEE_PERF_UI_FACE,
                                    (uint32_t)(esp_timer_get_time() - face_started));
                ui.face_frames++;
            }
        }
        stackee_lcd_flush();
        xSemaphoreGive(ui.lock);
    }
}

// ---------------------------------------------------------------------------
// コンソール
// ---------------------------------------------------------------------------
static size_t put(char *buf, size_t cap, size_t at, const char *fmt, ...) {
    if (at >= cap) {
        return at;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + at, cap - at, fmt, args);
    va_end(args);
    return (n < 0) ? at : at + (size_t)n;
}

static bool lock(void) {
    return ui.lock != NULL && xSemaphoreTake(ui.lock, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static void unlock(void) {
    xSemaphoreGive(ui.lock);
}

static size_t reply_crc(long id, const char *line, char *buf, size_t cap) {
    long y = stackee_console_int(line, "y", -1);
    long h = stackee_console_int(line, "h", -1);
    if (!lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    size_t at;
    if (y >= 0 && h > 0 && y + h <= ui.canvas.height) {
        at = put(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"y\":%ld,\"h\":%ld,\"crc\":%lu}",
                 id, y, h, (unsigned long)region_crc((int)y, (int)h));
    } else {
        at = put(buf, cap, 0,
                 "{\"id\":%ld,\"ok\":1,\"all\":%lu,\"face\":%lu,\"bar\":%lu,"
                 "\"face_y\":%d,\"face_h\":%d,\"bar_h\":%d,\"stride\":%d}",
                 id,
                 (unsigned long)stackee_crc32(0, ui.canvas.fb,
                                              (size_t)ui.canvas.height * ui.canvas.stride),
                 (unsigned long)region_crc(FACE_Y, FACE_ROWS),
                 (unsigned long)region_crc(0, STACKEE_BAR_AREA_HEIGHT),
                 FACE_Y, FACE_ROWS, STACKEE_BAR_AREA_HEIGHT, ui.canvas.stride);
    }
    unlock();
    return at;
}

// 不一致のときに「どこがどう違うか」を見るための生バイト。
static size_t reply_dump(long id, const char *line, char *buf, size_t cap) {
    long y = stackee_console_int(line, "y", 0);
    long x = stackee_console_int(line, "x", 0);
    long n = stackee_console_int(line, "n", 16);
    if (n < 1)   { n = 1; }
    if (n > 128) { n = 128; }
    if (y < 0 || y >= ui.canvas.height || x < 0 || x * 2 >= ui.canvas.stride) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"range\"}", id);
    }
    size_t offset = (size_t)y * ui.canvas.stride + (size_t)x * 2;
    size_t total = (size_t)ui.canvas.height * ui.canvas.stride;
    if (offset + (size_t)n > total) {
        n = (long)(total - offset);
    }
    if (!lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"y\":%ld,\"x\":%ld,\"n\":%ld,\"off\":%zu,\"hex\":\"",
                    id, y, x, n, offset);
    for (long i = 0; i < n; i++) {
        at = put(buf, cap, at, "%02X", ui.canvas.fb[offset + (size_t)i]);
    }
    at = put(buf, cap, at, "\"}");
    unlock();
    return at;
}

static int resolve_face_frame(const char *line, int *out) {
    long direct = stackee_console_int(line, "i", -1);
    if (direct >= 0 && direct < FACE_COUNT) {
        *out = (int)direct;
        return 0;
    }
    char state[16];
    if (!stackee_console_str(line, "state", state, sizeof(state))) {
        return -1;
    }
    for (int s = 0; s < STACKEE_FACE_STATES; s++) {
        if (strcmp(state, stackee_face_state_names[s]) != 0) {
            continue;
        }
        const stackee_face_case_t *c = &ui.cases.cases[s];
        long group = stackee_console_int(line, "group", 0);
        long frame = stackee_console_int(line, "frame", 0);
        if (group < 0 || group >= c->group_count) { return -2; }
        if (frame < 0 || frame >= c->frame_count[group]) { return -3; }
        *out = c->frames[group][frame];
        return 0;
    }
    return -1;
}

static size_t reply_face_set(long id, const char *line, char *buf, size_t cap) {
    int frame = 0;
    int err = resolve_face_frame(line, &frame);
    if (err != 0) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"badface\",\"why\":%d}", id, err);
    }
    if (!lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    ui.view.frozen = true;
    step_face_to(frame);
    stackee_lcd_flush();
    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"frame\":%d,\"face\":%lu,\"all\":%lu}",
                    id, frame, (unsigned long)region_crc(FACE_Y, FACE_ROWS),
                    (unsigned long)stackee_crc32(0, ui.canvas.fb,
                                                 (size_t)ui.canvas.height * ui.canvas.stride));
    unlock();
    return at;
}

static size_t reply_bar_set(long id, const char *line, char *buf, size_t cap) {
    stackee_bar_state_t state;
    long preset = stackee_console_int(line, "i", -1);
    if (preset >= 0 && preset < STACKEE_SELFTEST_BARS) {
        state = *stackee_selftest_bar((int)preset);
    } else {
        memset(&state, 0, sizeof(state));
        state.battery = (int)stackee_console_int(line, "bat", -1);
        state.charging = stackee_console_bool(line, "chg", false);
        state.volume = (int)stackee_console_int(line, "vol", 20);
        if (!stackee_console_str(line, "wifi", state.wifi, sizeof(state.wifi))) {
            snprintf(state.wifi, sizeof(state.wifi), "off");
        }
        char link[8];
        state.link = STACKEE_LINK_NONE;
        if (stackee_console_str(line, "link", link, sizeof(link))) {
            if (strcmp(link, "ble") == 0) { state.link = STACKEE_LINK_BLE; }
            else if (strcmp(link, "usb") == 0) { state.link = STACKEE_LINK_USB; }
        }
        state.ble_connected = stackee_console_bool(line, "ble", false);
    }
    if (!lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    ui.bar_forced = true;
    ui.forced = state;
    paint_bar(&state);
    ui.shown = state;
    ui.shown_valid = true;
    stackee_lcd_flush();
    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"bar\":%lu,\"bat\":%d,\"vol\":%d,"
                    "\"wifi\":\"%s\",\"link\":%d,\"ble\":%s}",
                    id, (unsigned long)region_crc(0, STACKEE_BAR_AREA_HEIGHT),
                    state.battery, state.volume, state.wifi, (int)state.link,
                    state.ble_connected ? "true" : "false");
    unlock();
    return at;
}

// ui.subtitle text=... — 帯を描いて、帯の領域の CRC32 を返す。
// ★ 顔の lcd.crc とまったく同じ方式。tools/subtitle_expected.py が同じ
//   font16.bin から描いた期待値と tools/check_phase2.py で突き合わせる。
static size_t reply_subtitle(long id, const char *line, char *buf, size_t cap) {
    char text[SUB_TEXT_MAX];
    if (!stackee_console_str(line, "text", text, sizeof(text))) {
        text[0] = '\0';                 // text 無し = 帯を消す
    }
    if (!lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    paint_subtitle(text);
    if (ui.sub_lock != NULL &&
        xSemaphoreTake(ui.sub_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        // ui タスクがすぐ描き直さないように、狙いの側も揃えておく。
        snprintf(ui.sub_want, sizeof(ui.sub_want), "%s", text);
        xSemaphoreGive(ui.sub_lock);
    }
    stackee_lcd_flush();
    stackee_perf_stats_t st;
    bool have_perf = stackee_perf_stats(STACKEE_PERF_UI_SUB, &st);
    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"y\":%d,\"h\":%d,\"crc\":%lu,"
                    "\"px\":%d,\"bytes\":%u,\"font16\":%s,\"paints\":%lu,"
                    "\"us\":%lu,\"max_us\":%lu}",
                    id, STACKEE_SUB_Y, STACKEE_SUB_HEIGHT,
                    (unsigned long)region_crc(STACKEE_SUB_Y, STACKEE_SUB_HEIGHT),
                    stackee_draw_subtitle_px(ui.have_font16 ? &ui.font16 : NULL, text),
                    (unsigned)strlen(text),
                    ui.have_font16 ? "true" : "false",
                    (unsigned long)ui.sub_paints,
                    (unsigned long)(have_perf ? st.last_us : 0),
                    (unsigned long)(have_perf ? st.max_us : 0));
    unlock();
    return at;
}

static size_t reply_selftest(long id, const char *line, char *buf, size_t cap) {
    bool restart = stackee_console_bool(line, "restart", false);
    if (!lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    selftest_state_t state = (selftest_state_t)atomic_load(&ui.selftest);
    if (state == SELFTEST_IDLE || (restart && state == SELFTEST_DONE)) {
        ui.selftest_step = 0;
        ui.selftest_started = esp_timer_get_time();
        ui.view.frozen = true;
        ui.bar_forced = true;
        ui.forced = *stackee_selftest_bar(STACKEE_SELFTEST_BARS - 1);
        atomic_store(&ui.selftest, SELFTEST_RUNNING);
        state = SELFTEST_RUNNING;
    }
    size_t at;
    if (state == SELFTEST_RUNNING) {
        at = put(buf, cap, 0,
                 "{\"id\":%ld,\"ok\":1,\"state\":\"running\",\"step\":%d,\"n\":%d}",
                 id, ui.selftest_step, SELFTEST_STEPS);
    } else {
        at = put(buf, cap, 0,
                 "{\"id\":%ld,\"ok\":1,\"state\":\"done\",\"ms\":%lu,\"faces\":[",
                 id, (unsigned long)ui.selftest_ms);
        for (int i = 0; i < FACE_COUNT; i++) {
            at = put(buf, cap, at, "%s%lu", i ? "," : "", (unsigned long)ui.face_crc[i]);
        }
        at = put(buf, cap, at, "],\"bars\":[");
        for (int i = 0; i < STACKEE_SELFTEST_BARS; i++) {
            at = put(buf, cap, at, "%s%lu", i ? "," : "", (unsigned long)ui.bar_crc[i]);
        }
        at = put(buf, cap, at, "]}");
    }
    unlock();
    return at;
}

static size_t reply_ui_status(long id, char *buf, size_t cap) {
    if (!lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"ready\":%s,\"frozen\":%s,\"bar_forced\":%s,"
                    "\"state\":%d,\"group\":%d,\"frame\":%d,\"current\":%d,\"target\":%d,"
                    "\"updates\":%lu,\"paints\":%lu,\"skipped\":%lu,\"frames\":%lu,"
                    "\"font\":\"%s\",\"selftest\":%d,"
                    "\"sub_paints\":%lu,\"sub_len\":%u,\"font16\":%s,"
                    "\"mic_held\":%s}",
                    id, atomic_load(&ui.ready) ? "true" : "false",
                    ui.view.frozen ? "true" : "false",
                    ui.bar_forced ? "true" : "false",
                    ui.view.anim.state, ui.view.anim.group, ui.view.anim.frame,
                    ui.view.current, ui.view.target,
                    (unsigned long)ui.view.updates, (unsigned long)ui.view.paints,
                    (unsigned long)ui.view.skipped, (unsigned long)ui.face_frames,
                    ui.have_font ? "h24" : "8x8",
                    atomic_load(&ui.selftest),
                    (unsigned long)ui.sub_paints, (unsigned)strlen(ui.sub_shown),
                    ui.have_font16 ? "true" : "false",
                    stackee_input_mic_held() ? "true" : "false");
    unlock();
    return at;
}

// 展開した素材そのものの CRC。実機の ROM tinfl と Mac の zlib が
// 同じものを作っているかを、絵を経由せずに直接見る。
static size_t reply_assets(long id, char *buf, size_t cap) {
    return put(buf, cap, 0,
               "{\"id\":%ld,\"ok\":1,"
               "\"faces_len\":%zu,\"faces_crc\":%lu,"
               "\"changes_len\":%zu,\"changes_crc\":%lu,"
               "\"icons_len\":%zu,\"icons_crc\":%lu,"
               "\"glyphs\":%d,\"ascent\":%d,\"font\":\"%s\","
               "\"font16_len\":%zu,\"font16_crc\":%lu,\"font16\":%s,"
               "\"font16_narrow\":%d,\"font16_wide\":%d}",
               id, ui.faces_len, (unsigned long)ui.faces_crc,
               ui.changes_len, (unsigned long)ui.changes_crc,
               ui.icons_len, (unsigned long)ui.icons_crc,
               ui.have_font ? ui.font.count : 0,
               ui.have_font ? ui.font.ascent : 0,
               ui.have_font ? "h24" : "8x8",
               ui.font16_len, (unsigned long)ui.font16_crc,
               ui.have_font16 ? "true" : "false",
               ui.have_font16 ? ui.font16.narrow_count : 0,
               ui.have_font16 ? ui.font16.wide_count : 0);
}

static size_t ui_console(const char *cmd, const char *line, long id,
                         char *buf, size_t cap) {
    if (!atomic_load(&ui.ready)) {
        return 0;       // まだ画面が無い。console は unsupported を返す
    }
    if (strcmp(cmd, "lcd.crc") == 0)      { return reply_crc(id, line, buf, cap); }
    if (strcmp(cmd, "lcd.dump") == 0)     { return reply_dump(id, line, buf, cap); }
    if (strcmp(cmd, "face.set") == 0)     { return reply_face_set(id, line, buf, cap); }
    if (strcmp(cmd, "bar.set") == 0)      { return reply_bar_set(id, line, buf, cap); }
    if (strcmp(cmd, "ui.selftest") == 0)  { return reply_selftest(id, line, buf, cap); }
    if (strcmp(cmd, "ui.status") == 0)    { return reply_ui_status(id, buf, cap); }
    if (strcmp(cmd, "ui.subtitle") == 0)  { return reply_subtitle(id, line, buf, cap); }
    if (strcmp(cmd, "ui.assets") == 0)    { return reply_assets(id, buf, cap); }
    if (strcmp(cmd, "face.auto") == 0) {
        if (!lock()) { return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id); }
        ui.view.frozen = false;
        unlock();
        return put(buf, cap, 0, "{\"id\":%ld,\"ok\":1}", id);
    }
    if (strcmp(cmd, "bar.auto") == 0) {
        if (!lock()) { return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id); }
        ui.bar_forced = false;
        ui.shown_valid = false;
        unlock();
        return put(buf, cap, 0, "{\"id\":%ld,\"ok\":1}", id);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 立ち上げ
// ---------------------------------------------------------------------------
// 展開した 240x240 のシートを、各コマの上 FACE_TRIM_TOP 行・
// 下 FACE_TRIM_BOTTOM 行を落として 240x200 に詰め直す (同じ入れ物の前へ
// 寄せるだけ)。元絵も faces.bin も変えない。落ちる画素は **0**
// (tools/render_expected.py --json の "face_lost" が空。RESULTS.md に実測)。
static void trim_faces(uint8_t *sheet) {
    const size_t row_bytes = (size_t)FACE_SIZE / 2;
    for (int frame = 0; frame < FACE_COUNT; frame++) {
        const uint8_t *src = sheet + (size_t)frame * FACE_SIZE * row_bytes +
                             (size_t)FACE_TRIM * row_bytes;
        uint8_t *dst = sheet + (size_t)frame * FACE_ROWS * row_bytes;
        memmove(dst, src, (size_t)FACE_ROWS * row_bytes);
    }
}

static void load_assets(void) {
    size_t len = 0;
    char *manifest = stackee_assets_read("manifest.json", &len, MALLOC_CAP_8BIT);
    if (manifest != NULL) {
        if (!stackee_face_parse_cases(manifest, &ui.cases)) {
            ESP_LOGE(TAG, "manifest.json の cases を読めない。顔は出ない");
            memset(&ui.cases, 0, sizeof(ui.cases));
        }
        free(manifest);
    }

    ui.faces = stackee_assets_read_inflate("faces.bin", FACE_SHEET_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ui.faces != NULL) {
        // ★ 素材は 240x240 のまま展開して丈を確かめ、**その場で** 上 29 / 下 11 行を
        //   捨てて 240x200 に詰め直す。行は前へしか動かないので同じ入れ物で
        //   済む (余った 153,600 B は realloc で PSRAM へ返す)。
        trim_faces(ui.faces);
        ui.faces_len = FACE_KEPT_BYTES;
        ui.faces_crc = stackee_crc32(0, ui.faces, FACE_KEPT_BYTES);
        uint8_t *smaller = heap_caps_realloc(ui.faces, FACE_KEPT_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (smaller != NULL) {
            ui.faces = smaller;
        }
    }
    ui.changes = stackee_assets_read_inflate("changes.bin", CHANGES_BYTES,
                                             MALLOC_CAP_8BIT);
    if (ui.changes != NULL) {
        ui.changes_len = CHANGES_BYTES;
        ui.changes_crc = stackee_crc32(0, ui.changes, CHANGES_BYTES);
    }
    ui.icons = stackee_assets_read_inflate("status_icons.bin",
                                           STACKEE_ICON_SHEET_BYTES, MALLOC_CAP_8BIT);
    if (ui.icons != NULL) {
        ui.icons_len = STACKEE_ICON_SHEET_BYTES;
        ui.icons_crc = stackee_crc32(0, ui.icons, STACKEE_ICON_SHEET_BYTES);
    }

    size_t font_len = 0;
    char *bdf = stackee_assets_read("status_h24.bdf", &font_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bdf != NULL) {
        ui.have_font = stackee_bdf_parse(bdf, font_len, STATUS_GLYPHS, &ui.font);
        free(bdf);
    }
    if (!ui.have_font) {
        ESP_LOGW(TAG, "status_h24.bdf を使えない。内蔵 8x8 で代用する");
    }

    // 字幕用の 16 px 日本語フォント。顔と同じ流儀で **起動時に一度だけ**
    // PSRAM へ丸ごと読む (描画中にファイルを読まない)。無ければ字幕は
    // 帯だけになり、会話も画面も止まらない。
    ui.font16_data = stackee_assets_read("font16.bin", &ui.font16_len,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ui.font16_data != NULL) {
        // ★ stackee_assets_read は末尾に NUL を 1 個足す。font16.bin 自身の
        //   長さで開くこと (足された 1 バイトを数えない)。
        ui.have_font16 = stackee_font16_open(&ui.font16, ui.font16_data,
                                             ui.font16_len);
        if (ui.have_font16) {
            ui.font16_crc = stackee_crc32(0, ui.font16_data, ui.font16_len);
        } else {
            ESP_LOGW(TAG, "font16.bin が壊れている (%zu B)", ui.font16_len);
            free(ui.font16_data);
            ui.font16_data = NULL;
            ui.font16_len = 0;
        }
    } else {
        ESP_LOGW(TAG, "font16.bin が無い。字幕は出ない");
    }
}

esp_err_t stackee_ui_start(void) {
    if (!stackee_lcd_ready() || stackee_lcd_framebuffer() == NULL) {
        ESP_LOGE(TAG, "LCD が無いので画面は出さない");
        return ESP_ERR_INVALID_STATE;
    }
    int64_t t0 = esp_timer_get_time();
    ui.lock = xSemaphoreCreateMutex();
    ui.sub_lock = xSemaphoreCreateMutex();
    if (ui.lock == NULL || ui.sub_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // 起動直後は字幕の文字が無い (帯そのものは黒。下で塗る)。
    ui.sub_want[0] = '\0';
    ui.sub_shown[0] = '\0';
    ui.sub_shown_valid = false;
    ui.canvas.fb = stackee_lcd_framebuffer();
    ui.canvas.stride = STACKEE_LCD_WIDTH * 2;
    ui.canvas.width = STACKEE_LCD_WIDTH;
    ui.canvas.height = STACKEE_LCD_HEIGHT;
    ui.battery = -1;
    ui.charging = false;
    atomic_store(&ui.volume, stackee_volume_percent());
    snprintf(ui.wifi, sizeof(ui.wifi), "off");      // 段階 3 まで Wi-Fi は無い

    load_assets();

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    stackee_face_view_init(&ui.view, &ui.cases, ui.changes, FACE_COUNT, now_ms);

    // 最初の 1 枚。背景 (白) → バー → 顔を全面。
    stackee_draw_fill(&ui.canvas, 0, 0, ui.canvas.width, ui.canvas.height,
                      stackee_draw_rgb565(STACKEE_SCREEN_BG));
    stackee_lcd_mark_rows(0, ui.canvas.height);
    stackee_bar_state_t state;
    collect_bar(&state);
    paint_bar(&state);
    ui.shown = state;
    ui.shown_valid = true;
    if (ui.faces != NULL && ui.changes != NULL) {
        paint_face_full(ui.view.current);
    } else {
        // 素材が無いなら状態機械を止める (差分表を引けないので描けない)。
        ui.view.frozen = true;
    }
    // ★ 帯は起動直後から黒 (字幕が無くても白に戻さない)。1 枚目から出す。
    paint_subtitle("");
    stackee_lcd_flush();

    atomic_store(&ui.ready, true);
    stackee_console_register(ui_console);

    // CPU0 / 中優先度。入力 (CPU1 / 最高) と取り合わない。
    if (xTaskCreatePinnedToCore(ui_task, "stackee_ui", 4096, NULL, 3, &ui.task, 0)
            != pdPASS) {
        ESP_LOGE(TAG, "ui タスクを作れない");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "画面おきた (%lld ms, 顔 %s / アイコン %s / フォント %s / 字幕 %s)",
             (esp_timer_get_time() - t0) / 1000,
             ui.faces ? "あり" : "なし", ui.icons ? "あり" : "なし",
             ui.have_font ? "h24" : "8x8",
             ui.have_font16 ? "font16" : "なし");
    return ESP_OK;
}

bool stackee_ui_ready(void) {
    return atomic_load(&ui.ready);
}

// ---------------------------------------------------------------------------
// 段階 4: 撮影中の一時停止
// ---------------------------------------------------------------------------
// ★ 画面を消すのではなく「描き直さない」だけ。撮影が終われば次の周で
//   いまの状態を描き直すので、明示的な復帰処理は要らない。
void stackee_ui_pause(bool on) {
    atomic_store(&ui.paused, on);
}

bool stackee_ui_paused(void) {
    return atomic_load(&ui.paused);
}
