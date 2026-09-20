// hid_out タスク。DESIGN.md §3。
//
// 送信キューから取り出して USB (または BLE) へ出す。**送信の都合で入力タスクを
// 待たせない**ためだけに存在する。送信に失敗したら捨てて次へ行く。
//
// 送信先の選び方は stackee_hid_dest.c (既定 BLE / STK_HID_SWITCH でトグル /
// USB が無ければ BLE に倒す)。BLE の実体は stackee_ble.c。
#include "stackee_hid_out.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

#include "qmk_port.h"
#include "stackee_ble.h"
#include "stackee_hid_dest.h"
#include "stackee_report_queue.h"
#include "stackee_usb.h"

static const char *TAG = "hid_out";

#define HID_OUT_TASK_STACK 4096
#define HID_OUT_TASK_PRIO  (configMAX_PRIORITIES - 4)
#define HID_OUT_TASK_CPU   1

static uint32_t s_dropped_no_dest;

static bool use_usb(void) {
    return stackee_hid_dest_effective() == STACKEE_HID_USB;
}

// BLE の口。Report ID は USB と同じ 1/2/3。
static bool send_ble(const stackee_report_t *report) {
    switch (report->kind) {
        case STACKEE_REPORT_KEYBOARD:
            return stackee_ble_send(STACKEE_REPORT_ID_KEYBOARD, report->data, 8);
        case STACKEE_REPORT_MOUSE:
            return stackee_ble_send(STACKEE_REPORT_ID_MOUSE, report->data, 5);
        case STACKEE_REPORT_CONSUMER:
            return stackee_ble_send(STACKEE_REPORT_ID_CONSUMER, report->data, 2);
        case STACKEE_REPORT_RAW:
            // ★ VIA は BLE では運ばない。段階 1b の実験項目 (DESIGN.md §6)。
            //   Raw HID のサービスを BLE 側に足していないので、応答は捨てる。
            return false;
        default:
            return false;
    }
}

static bool send_usb(const stackee_report_t *report) {
    switch (report->kind) {
        case STACKEE_REPORT_KEYBOARD:
            // report_keyboard_t = mods, reserved, keys[6] の 8 バイト。
            return tud_hid_n_report(STACKEE_HID_ITF_KEYS,
                                    STACKEE_REPORT_ID_KEYBOARD,
                                    report->data, 8);
        case STACKEE_REPORT_MOUSE:
            // report_mouse_t = buttons, x, y, v, h の 5 バイト。
            return tud_hid_n_report(STACKEE_HID_ITF_KEYS,
                                    STACKEE_REPORT_ID_MOUSE,
                                    report->data, 5);
        case STACKEE_REPORT_CONSUMER:
            return tud_hid_n_report(STACKEE_HID_ITF_KEYS,
                                    STACKEE_REPORT_ID_CONSUMER,
                                    report->data, 2);
        case STACKEE_REPORT_SYSTEM:
            // ★ 記述子にシステムコントロールのコレクションは無い
            //   (現行の配列に該当キーが 1 つも無いため)。出さずに捨てる。
            return false;
        case STACKEE_REPORT_RAW:
            // Raw HID はトップレベルコレクション 1 つ・Report ID なし
            // (DESIGN.md §4 / §8b)。ID には 0 を渡す。
            return tud_hid_n_report(STACKEE_HID_ITF_RAW, 0,
                                    report->data, STACKEE_RAW_HID_SIZE);
        default:
            return false;
    }
}

static void hid_out_task(void *arg) {
    (void)arg;
    stackee_report_t report;
    int64_t last_ble_tick = 0;
    for (;;) {
        bool did_work = false;
        while (true) {
            uint8_t kind;
            if (!stackee_report_queue_peek_kind(&kind)) {
                break;
            }
            // ★★ Raw HID (VIA と、段階 4 のコンソール) は **送信先の選択に
            //   関わらず必ず USB へ出す**。あれはキー入力ではなく制御の
            //   通り道で、BLE 側には口が無い (DESIGN.md §6 の段階 1b)。
            //   ここを送信先で振り分けると、既定の BLE のままでは
            //   VIA の応答もコンソールの応答も 1 バイトも返らない
            //   (full プロファイルではコンソールが黙る = 手も足も出なくなる)。
            bool usb = (kind == STACKEE_REPORT_RAW) ? true : use_usb();
            if (kind == STACKEE_REPORT_RAW && !stackee_usb_mounted()) {
                // ★ USB が刺さっていない。**捨てて先へ進む**。ここで
                //   break すると、後ろに並んでいるキーのレポートまで
                //   止まって BLE へ出られなくなる。
                if (stackee_report_queue_pop(&report)) {
                    s_dropped_no_dest++;
                    stackee_report_queue_count_sent(true, false);
                    did_work = true;
                }
                continue;
            }
            if (usb) {
                // ★ 種類ごとに出口のインターフェースが違う。VIA の応答が
                //   キーボードの口の混雑で捨てられないよう、**その種類の
                //   口**が空いているかだけを見る。
                uint8_t itf = (kind == STACKEE_REPORT_RAW) ? STACKEE_HID_ITF_RAW
                                                           : STACKEE_HID_ITF_KEYS;
                if (!tud_hid_n_ready(itf)) {
                    break;      // 前のレポートがまだ出ていない。次の周回で
                }
            }
            if (!stackee_report_queue_pop(&report)) {
                break;
            }
            did_work = true;
            if (usb) {
                stackee_report_queue_count_sent(true, send_usb(&report));
            } else if (stackee_ble_connected()) {
                stackee_report_queue_count_sent(false, send_ble(&report));
            } else {
                // BLE がまだ繋がっていない。捨てて数える (入力は止めない)。
                s_dropped_no_dest++;
                stackee_report_queue_count_sent(false, false);
            }
        }
        vTaskDelay(did_work ? 1 : pdMS_TO_TICKS(2));

        // NVS への書き戻し (VIA の配列保存)。入力タスクから外してあるのは、
        // フラッシュ書き込みで打鍵が待たされないようにするため。
        stackee_qmk_eeprom_task();

        // 未接続なら 1 秒ごとにアドバタイズし直す
        // (現行 KMK の BLEHID.ble_monitor と同じ周期)。★ ループの回転数では
        // なく時刻で数える。打鍵中は 1 ms、暇なときは 2 ms で回るので、
        // 回数で数えると周期が打鍵量に引きずられる。
        int64_t now = esp_timer_get_time();
        if (now - last_ble_tick >= 1000000) {
            last_ble_tick = now;
            stackee_ble_tick();
        }
    }
}

void stackee_hid_out_start(void) {
    xTaskCreatePinnedToCore(hid_out_task, "hid_out", HID_OUT_TASK_STACK, NULL,
                            HID_OUT_TASK_PRIO, NULL, HID_OUT_TASK_CPU);
    ESP_LOGI(TAG, "送信タスク開始 (CPU%d 優先度 %d)",
             HID_OUT_TASK_CPU, HID_OUT_TASK_PRIO);
}

uint32_t stackee_hid_out_dropped_no_dest(void) {
    return s_dropped_no_dest;
}
