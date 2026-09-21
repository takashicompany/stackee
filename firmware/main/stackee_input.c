#include "stackee_input.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dynamic_keymap.h"
#include "qmk_port.h"
#include "raw_hid.h"
#include "stackee_ble.h"
#include "stackee_audio.h"
#include "stackee_hid_dest.h"
#include "stackee_camera.h"
#include "stackee_touch.h"
#include "stackee_volume.h"
#include "stackee_hid_out.h"
#include "stackee_perf.h"
#include "stackee_report_queue.h"
#include "stackee_tca8418.h"
#include "stackee_usb.h"

static const char *TAG = "input";

// MIC(kc) のキーを押しているか。入力タスクが書き、ui タスクが読む。
static _Atomic bool s_mic_held;

// key.inject を「待たずに」始めた。終わったら入力タスクが待機へ戻す。
static bool s_inject_async;

#define INPUT_TASK_STACK 6144
#define INPUT_TASK_PRIO  (configMAX_PRIORITIES - 2)  // 最高優先度 (IDLE より上)
#define INPUT_TASK_CPU   1
#define MAX_EVENTS_PER_TICK 16

// ---------------------------------------------------------------------------
// 橋渡し層が要求する「ハードに触る出口」
// ---------------------------------------------------------------------------
uint32_t stackee_qmk_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void stackee_qmk_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void stackee_qmk_enter_rom_download(void) {
    // QK_BOOT (KMK の KC.RESET と同じ位置) の行き先。tools/flash.py が
    // つかまえられる ROM のダウンロードモードへ落とす。
    stackee_usb_request_rom_download();
}

void stackee_qmk_restart(void) {
    stackee_usb_request_restart();      // strap の固定込み
}

void stackee_qmk_custom_key(stackee_key_action_t action, bool pressed) {
    // ★ STK_TALK だけは押し離しの両方が要る (押している間だけ録音する)。
    //   ここでは印を立てるだけ。実際の録音は audio タスクが進める。
    if (action == STACKEE_KEY_TALK) {
        stackee_audio_talk_key(pressed);
        return;
    }
    // ★ MIC(kc) も押し離しの両方。印を 1 つ立てるだけで、
    //   中のキーの送出そのものは qmk_port/stackee_holdtap.c が済ませている。
    //   顔を変えるのは ui タスク (ここでは画面に触らない)。
    if (action == STACKEE_KEY_MIC) {
        atomic_store(&s_mic_held, pressed);
        return;
    }
    // ★ STK_TOUCH_SCROLL も押し離しの両方が要る (押している間だけ
    //   なぞりがスクロールになる。現行 stackee_touch.py と同じ)。
    if (action == STACKEE_KEY_TOUCH_SCROLL) {
        stackee_touch_set_scroll(pressed);
        return;
    }
    if (!pressed) {
        return;             // 残りはどれも「押した瞬間」だけ効く
    }
    switch (action) {
        case STACKEE_KEY_VOLUP:
            // ★ NVS へ書くのはここではない (入力タスクを止めない)。
            //   静まってから audio タスクが 1 回だけ書く。
            stackee_volume_bump(STACKEE_VOLUME_STEP);
            break;
        case STACKEE_KEY_VOLDN:
            stackee_volume_bump(-STACKEE_VOLUME_STEP);
            break;
        case STACKEE_KEY_HID_SWITCH: {
            // 現行 KMK の hid_switch と同じトグル。選択は NVS に残る。
            stackee_hid_dest_t dest = stackee_hid_dest_toggle();
            ESP_LOGI(TAG, "送信先を %s にした", stackee_hid_dest_name(dest));
            break;
        }
        case STACKEE_KEY_BLE_REFRESH:
            // 現行 KMK の ble_refresh と同じ。★ ボンドは消さない。
            stackee_ble_refresh();
            break;
        case STACKEE_KEY_CAMERA:
            // ★ ここでは撮らない。印を立てるだけ (撮影は 4 秒かかる)。
            //   実際の撮影は camera タスクが進める。
            stackee_camera_key();
            break;
        default:
            ESP_LOGI(TAG, "独自キー %s 押下 (割り当て無し)",
                     stackee_key_action_name(action));
            break;
    }
}

// ---------------------------------------------------------------------------
// 打鍵の注入 (人手ゼロの検証用)
// ---------------------------------------------------------------------------
// 配線の無いスロット。ROW4 は COL4/5/6 だけが実キーなので COL0 は空いている
// (keymap.py の _SPILL を参照)。ここへ一時的にキーコードを置いて叩く。
#define INJECT_ROW 4
#define INJECT_COL 0
#define INJECT_SLOT (INJECT_ROW * STACKEE_MATRIX_COLS + INJECT_COL)

typedef enum {
    INJECT_IDLE = 0,
    INJECT_REQUESTED,   // コンソールが頼んだ。入力タスクがこれから始める
    INJECT_PRESSED,     // 押した。押下レポートと hold_ms を待っている
    INJECT_RELEASED,    // 離した。解放レポートを待っている
    INJECT_DONE,
} inject_state_t;

static volatile inject_state_t s_inject_state;
static uint16_t                s_inject_keycode;
static uint32_t                s_inject_hold_ms;
static int64_t                 s_inject_t0;
static int64_t                 s_inject_press_at;
static uint32_t                s_inject_pushed_before;  // 直前の段の基準
static uint32_t                s_inject_pushed_start;   // 注入まるごとの基準
static uint32_t                s_inject_usb_before;
static uint32_t                s_inject_ble_before;
static stackee_inject_result_t s_inject_result;

// 入力タスクの中から呼ぶ。1 周につき 1 段だけ進める。
static void inject_step(void) {
    stackee_report_stats_t stats;
    switch (s_inject_state) {
        case INJECT_REQUESTED: {
            stackee_report_queue_stats(&stats);
            s_inject_pushed_before = stats.pushed;
            s_inject_pushed_start = stats.pushed;
            s_inject_usb_before = stats.sent_usb;
            s_inject_ble_before = stats.sent_ble;
            // レイヤー 0 の空きスロットに、叩きたいキーを置く。
            dynamic_keymap_set_keycode(0, INJECT_ROW, INJECT_COL,
                                       s_inject_keycode);
            s_inject_t0 = esp_timer_get_time();
            s_inject_press_at = 0;
            memset(&s_inject_result, 0, sizeof(s_inject_result));
            stackee_report_queue_set_key_time((uint64_t)s_inject_t0);
            stackee_qmk_matrix_event(INJECT_SLOT, true);
            s_inject_state = INJECT_PRESSED;
            break;
        }
        case INJECT_PRESSED: {
            stackee_report_queue_stats(&stats);
            if (s_inject_result.press_us == 0 &&
                stats.pushed != s_inject_pushed_before) {
                s_inject_result.press_us =
                    (uint32_t)(esp_timer_get_time() - s_inject_t0);
                s_inject_press_at = esp_timer_get_time();
            }
            // 押下レポートが出てから hold_ms 数える。出ないまま 500 ms
            // 過ぎたら諦めて離す (押しっぱなしにしない)。
            // ★ 諦めるのは **レポートが出ていないときだけ**。2026-09-21 まで
            //   押下が出ていても 500 ms で離していたので、hold_ms が 500 を
            //   超えると黙って短くなっていた (1500 ms が約 600 ms になった)。
            int64_t since = esp_timer_get_time() - s_inject_t0;
            bool timeout = (s_inject_press_at == 0) && since > 500000;
            bool held_enough = s_inject_press_at != 0 &&
                               (esp_timer_get_time() - s_inject_press_at) >=
                                   (int64_t)s_inject_hold_ms * 1000;
            if (held_enough || timeout) {
                stackee_report_queue_set_key_time((uint64_t)esp_timer_get_time());
                s_inject_t0 = esp_timer_get_time();
                stackee_report_queue_stats(&stats);
                s_inject_pushed_before = stats.pushed;
                stackee_qmk_matrix_event(INJECT_SLOT, false);
                s_inject_state = INJECT_RELEASED;
            }
            break;
        }
        case INJECT_RELEASED: {
            stackee_report_queue_stats(&stats);
            int64_t since = esp_timer_get_time() - s_inject_t0;
            if (stats.pushed != s_inject_pushed_before || since > 500000) {
                if (stats.pushed != s_inject_pushed_before) {
                    s_inject_result.release_us = (uint32_t)since;
                }
                // 空きスロットを元に戻す。戻さないと次の打鍵で出てしまう。
                dynamic_keymap_set_keycode(0, INJECT_ROW, INJECT_COL, 0x0000);
                s_inject_result.keycode = s_inject_keycode;
                s_inject_result.pushed = stats.pushed - s_inject_pushed_start;
                s_inject_result.sent_usb = stats.sent_usb - s_inject_usb_before;
                s_inject_result.sent_ble = stats.sent_ble - s_inject_ble_before;
                s_inject_result.ok = s_inject_result.press_us != 0;
                s_inject_state = INJECT_DONE;
                // ★ 待たずに始めたものは、誰も結果を取りに来ない。
                //   ここで待機へ戻さないと次の注入ができない。
                if (s_inject_async) {
                    s_inject_async = false;
                    s_inject_state = INJECT_IDLE;
                }
            }
            break;
        }
        default:
            break;
    }
}

// 押し始める (どちらの口もここを通る)。
// ★ 上限は 3000 ms。表情 (聞き取り中は 250 ms 送りの 3 コマ) を外から
//   見るには 1 秒では足りない。普段の遅延測定は 20〜30 ms のまま。
static bool inject_begin(uint16_t keycode, uint32_t hold_ms) {
    if (s_inject_state != INJECT_IDLE) {
        return false;       // まだ前のが終わっていない
    }
    if (hold_ms == 0 || hold_ms > 3000) {
        hold_ms = 30;
    }
    s_inject_keycode = keycode;
    s_inject_hold_ms = hold_ms;
    s_inject_state = INJECT_REQUESTED;
    return true;
}

bool stackee_input_inject_begin(uint16_t keycode, uint32_t hold_ms) {
    if (!inject_begin(keycode, hold_ms)) {
        return false;
    }
    s_inject_async = true;      // 終わったら入力タスクが待機へ戻す
    return true;
}

bool stackee_input_inject(uint16_t keycode, uint32_t hold_ms,
                          stackee_inject_result_t *out) {
    if (out == NULL) {
        return false;
    }
    if (!inject_begin(keycode, hold_ms)) {
        return false;
    }
    if (hold_ms == 0 || hold_ms > 3000) {
        hold_ms = 30;           // inject_begin と同じ丸め (待ち時間の計算用)
    }

    // 入力タスクが進めるのを待つ。押下 500ms + 保持 + 解放 500ms + 余裕。
    for (int i = 0; i < (int)(hold_ms + 1200); i++) {
        if (s_inject_state == INJECT_DONE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    bool done = (s_inject_state == INJECT_DONE);
    *out = s_inject_result;
    if (!done) {
        // 取り残さない。空きスロットを戻して待機に返す。
        out->ok = false;
    }
    snprintf(out->dest, sizeof(out->dest), "%s",
             stackee_hid_dest_name(stackee_hid_dest_effective()));
    s_inject_state = INJECT_IDLE;
    return done && out->ok;
}

// ---------------------------------------------------------------------------
// 入力タスク
// ---------------------------------------------------------------------------
static void input_task(void *arg) {
    (void)arg;
    stackee_tca_event_t events[MAX_EVENTS_PER_TICK];
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        int64_t t0 = esp_timer_get_time();

        int count = stackee_tca8418_poll(events, MAX_EVENTS_PER_TICK);
        if (count > 0) {
            // ★ 遅延の起点。ここから「USB へ渡せる形になるまで」を測る。
            stackee_report_queue_set_key_time((uint64_t)t0);
            for (int i = 0; i < count; i++) {
                stackee_qmk_matrix_event(events[i].slot, events[i].pressed);
            }
        }

        uint32_t before = 0;
        stackee_report_stats_t stats;
        stackee_report_queue_stats(&stats);
        before = stats.pushed;

        // VIA から届いた Raw HID をここで QMK に渡す。QMK の状態を触るのは
        // このタスク 1 本だけ、という決めごとを守るため。
        uint8_t raw[STACKEE_RAW_HID_SIZE];
        while (stackee_usb_raw_rx_pop(raw)) {
            raw_hid_receive(raw, STACKEE_RAW_HID_SIZE);
        }

        stackee_qmk_task();
        inject_step();

        stackee_report_queue_stats(&stats);
        if (stats.pushed != before) {
            // レポートが出た。キーを読んだ時刻からの経過を積む。
            uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
            stackee_perf_sample(STACKEE_PERF_INPUT, us);
        }
        stackee_perf_sample(STACKEE_PERF_INPUT_LOOP,
                            (uint32_t)(esp_timer_get_time() - t0));

        // 1 ms 周期。vTaskDelayUntil なので、処理が長引いても位相がずれない。
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1));
    }
}

void stackee_input_start(void) {
    if (stackee_tca8418_init() != ESP_OK) {
        ESP_LOGE(TAG, "TCA8418 の I2C を用意できない。キー入力なしで続ける");
    }
    stackee_qmk_init();
    BaseType_t ok = xTaskCreatePinnedToCore(input_task, "input", INPUT_TASK_STACK,
                                            NULL, INPUT_TASK_PRIO, NULL,
                                            INPUT_TASK_CPU);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "入力タスクを作れない");
        return;
    }
    ESP_LOGI(TAG, "入力タスク開始 (CPU%d 優先度 %d 周期 1 ms)",
             INPUT_TASK_CPU, INPUT_TASK_PRIO);
}

bool stackee_input_mic_held(void) {
    return atomic_load(&s_mic_held);
}

void stackee_input_stats(stackee_input_stats_t *out) {
    if (out == NULL) {
        return;
    }
    stackee_tca_stats_t tca;
    stackee_tca8418_stats(&tca);
    out->tca_connected = tca.connected;
    out->key_events = tca.events;
    out->overflows = tca.overflows;
    out->io_fails = tca.io_fails;
    out->stray = tca.stray;
    out->keys_down = stackee_qmk_matrix_pressed_count();
    out->custom_keys = stackee_qmk_custom_key_count();
}
