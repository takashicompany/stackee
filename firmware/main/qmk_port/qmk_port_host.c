// QMK の host_driver_t。作られたレポートを送信キューへ積むだけ。
//
// ★ ここで USB を触らない。入力タスクが USB の都合で待たされたら、
//   遅延をなくすという段階 1 の目的が崩れる (DESIGN.md §3)。実際に送るのは
//   hid_out タスク (stackee_hid_out.c)。
#include <string.h>

#include "host.h"
#include "host_driver.h"
#include "qmk_port.h"
#include "report.h"
#include "stackee_report_queue.h"

static uint8_t s_led_state;     // ホストから届いた LED (CapsLock など)

static uint8_t port_keyboard_leds(void) {
    return s_led_state;
}

void stackee_qmk_set_led_state(uint8_t leds) {
    s_led_state = leds;
}

static void port_send_keyboard(report_keyboard_t *report) {
    stackee_report_queue_push(STACKEE_REPORT_KEYBOARD, report,
                              sizeof(report_keyboard_t));
}

static void port_send_nkro(report_nkro_t *report) {
    // NKRO は使わない (記述子に無い)。捨てる。
    (void)report;
}

static void port_send_mouse(report_mouse_t *report) {
    stackee_report_queue_push(STACKEE_REPORT_MOUSE, report,
                              sizeof(report_mouse_t));
}

static void port_send_extra(report_extra_t *report) {
    // System control と Consumer control は同じ口から来る。記述子側では
    // 別の Report ID なので、ここで分けてキューに積む。
    stackee_report_kind_t kind =
        (report->report_id == REPORT_ID_SYSTEM) ? STACKEE_REPORT_SYSTEM
                                                : STACKEE_REPORT_CONSUMER;
    uint16_t usage = report->usage;
    stackee_report_queue_push(kind, &usage, sizeof(usage));
}

static host_driver_t s_driver = {
    .keyboard_leds = port_keyboard_leds,
    .send_keyboard = port_send_keyboard,
    .send_nkro     = port_send_nkro,
    .send_mouse    = port_send_mouse,
    .send_extra    = port_send_extra,
};

host_driver_t *stackee_qmk_host_driver(void) {
    return &s_driver;
}

// ---------------------------------------------------------------------------
// Raw HID (VIA)
// ---------------------------------------------------------------------------
// QMK の via.c は応答を raw_hid_send() で返す。こちらも送信キューへ積む。
void raw_hid_send(uint8_t *data, uint8_t length) {
    stackee_report_queue_push(STACKEE_REPORT_RAW, data, length);
}
