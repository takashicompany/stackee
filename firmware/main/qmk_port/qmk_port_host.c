// QMK の host_driver_t。作られたレポートを送信キューへ積むだけ。
//
// ★ ここで USB を触らない。入力タスクが USB の都合で待たされたら、
//   遅延をなくすという段階 1 の目的が崩れる (DESIGN.md §3)。実際に送るのは
//   hid_out タスク (stackee_hid_out.c)。
#include <string.h>

#include "action.h"
#include "action_util.h"
#include "host.h"
#include "host_driver.h"
#include "qmk_port.h"
#include "report.h"
#include "stackee_report_queue.h"
#ifdef MOUSEKEY_ENABLE
#include "mousekey.h"
#endif

static uint8_t s_led_state;     // ホストから届いた LED (CapsLock など)

// ---------------------------------------------------------------------------
// 設定メニューのキーの横取り (qmk_port.h の「キーの横取り」)
// ---------------------------------------------------------------------------
static volatile stackee_gate_state_t s_gate;
static uint32_t s_gate_drain_at;            // DRAIN に入った時刻 [ms]
static uint8_t  s_gate_prev[KEYBOARD_REPORT_KEYS];  // 直前に読んだレポートのキー
static stackee_qmk_gate_stats_t s_gate_stats;

bool stackee_qmk_gate_capturing(void) {
    return s_gate != STACKEE_GATE_OFF;
}

stackee_gate_state_t stackee_qmk_gate_state(void) {
    return s_gate;
}

const char *stackee_qmk_gate_name(stackee_gate_state_t state) {
    switch (state) {
        case STACKEE_GATE_OPEN: return "open";
        case STACKEE_GATE_DRAIN: return "drain";
        default: return "off";
    }
}

void stackee_qmk_gate_stats(stackee_qmk_gate_stats_t *out) {
    if (out != NULL) {
        *out = s_gate_stats;
    }
}

void stackee_qmk_gate_note_swallowed(void) {
    s_gate_stats.swallowed++;
}

void stackee_qmk_gate_set(bool open) {
    if (open) {
        if (s_gate == STACKEE_GATE_OFF) {
            // ★ 横取りを始める**前に**、全部離したレポートを PC へ送る。
            //   開いた瞬間に押していたキー (Settings の前に押していた文字や、
            //   MT で効いていた Shift) が PC に押しっぱなしで残らない。
            //   QMK の clear_keyboard() と同じだが、マウスは「押していた /
            //   動いていた」ときだけ離す (何も無いのに空のマウスレポートを
            //   PC へ出さない)。キーボードとコンシューマは QMK が差分を見て、
            //   変わっていなければ送らない。
#ifdef MOUSEKEY_ENABLE
            report_mouse_t mouse = mousekey_get_report();
            bool mouse_busy = mouse.buttons || mouse.x || mouse.y || mouse.v || mouse.h;
#endif
            clear_mods();
            clear_keys();
            clear_weak_mods();
            send_keyboard_report();
#ifdef EXTRAKEY_ENABLE
            host_system_send(0);
            host_consumer_send(0);
#endif
#ifdef MOUSEKEY_ENABLE
            mousekey_clear();
            if (mouse_busy) {
                mousekey_send();
            }
#endif
        }
        if (s_gate != STACKEE_GATE_OPEN) {
            s_gate_stats.opens++;
        }
        memset(s_gate_prev, 0, sizeof(s_gate_prev));
        s_gate = STACKEE_GATE_OPEN;
        return;
    }
    if (s_gate == STACKEE_GATE_OPEN) {
        s_gate = STACKEE_GATE_DRAIN;
        s_gate_drain_at = stackee_qmk_now_ms();
        s_gate_stats.closes++;
    }
}

void stackee_qmk_gate_step(void) {
    if (s_gate != STACKEE_GATE_DRAIN) {
        return;
    }
    bool released = stackee_qmk_matrix_pressed_count() == 0;
    bool timeout = (uint32_t)(stackee_qmk_now_ms() - s_gate_drain_at) >=
                   STACKEE_GATE_DRAIN_MAX_MS;
    if (!released && !timeout) {
        return;
    }
    if (!released) {
        s_gate_stats.drain_timeouts++;
    }
    // ★ まだ横取り中のうちに QMK のレポートを空にする (これは PC へは出ない)。
    //   PC が最後に見たのは開いたときの「全部離した」レポートなので、
    //   ここで QMK 側も空にしておけば、あとで離しが来ても送る差分が無い。
    clear_keyboard();
    s_gate = STACKEE_GATE_OFF;
}

// 新しく押されたキーだけを取り出して渡す (離しは要らない)。
static void gate_capture_keyboard(const report_keyboard_t *report) {
    s_gate_stats.captured++;
    for (int i = 0; i < KEYBOARD_REPORT_KEYS; i++) {
        uint8_t key = report->keys[i];
        if (key == 0) {
            continue;
        }
        bool seen = false;
        for (int j = 0; j < KEYBOARD_REPORT_KEYS; j++) {
            if (s_gate_prev[j] == key) {
                seen = true;
                break;
            }
        }
        if (!seen && s_gate == STACKEE_GATE_OPEN) {
            s_gate_stats.keys++;
            stackee_qmk_menu_key(key, report->mods);
        }
    }
    memcpy(s_gate_prev, report->keys, sizeof(s_gate_prev));
}

static uint8_t port_keyboard_leds(void) {
    return s_led_state;
}

void stackee_qmk_set_led_state(uint8_t leds) {
    s_led_state = leds;
}

static void port_send_keyboard(report_keyboard_t *report) {
    if (stackee_qmk_gate_capturing()) {
        gate_capture_keyboard(report);      // ★ PC へは出さない
        return;
    }
    s_gate_stats.to_pc++;
    stackee_report_queue_push(STACKEE_REPORT_KEYBOARD, report,
                              sizeof(report_keyboard_t));
}

static void port_send_nkro(report_nkro_t *report) {
    // NKRO は使わない (記述子に無い)。捨てる。
    (void)report;
}

static void port_send_mouse(report_mouse_t *report) {
    if (stackee_qmk_gate_capturing()) {
        s_gate_stats.dropped++;             // マウスキーはメニュー中は動かさない
        return;
    }
    s_gate_stats.to_pc++;
    stackee_report_queue_push(STACKEE_REPORT_MOUSE, report,
                              sizeof(report_mouse_t));
}

static void port_send_extra(report_extra_t *report) {
    if (stackee_qmk_gate_capturing()) {
        s_gate_stats.dropped++;
        return;
    }
    s_gate_stats.to_pc++;
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
