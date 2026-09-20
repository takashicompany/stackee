#include "stackee_hid_dest.h"

#include <stddef.h>

static stackee_hid_dest_io_t s_io;
static stackee_hid_dest_t    s_selected = STACKEE_HID_BLE;

void stackee_hid_dest_init(const stackee_hid_dest_io_t *io) {
    if (io != NULL) {
        s_io = *io;
    }
    // ★ 既定は BLE。NVS が読めない / 空 / 知らない値のときも BLE に倒す。
    //   現行 CircuitPython 版が毎回 BLE から始まるのと同じ振る舞いを、
    //   「保存が無いとき」の既定として残してある。
    s_selected = STACKEE_HID_BLE;
    uint8_t saved = 0;
    if (s_io.load != NULL && s_io.load(&saved) && saved == STACKEE_HID_USB) {
        s_selected = STACKEE_HID_USB;
    }
}

stackee_hid_dest_t stackee_hid_dest_selected(void) {
    return s_selected;
}

stackee_hid_dest_t stackee_hid_dest_effective(void) {
    if (s_selected != STACKEE_HID_USB) {
        return STACKEE_HID_BLE;
    }
    // USB を選んでいるがケーブルが無い。送る先が無いので BLE に倒す。
    // ★ 選択 (s_selected) は書き換えない。挿し直せば黙って USB に戻る。
    if (s_io.usb_connected != NULL && !s_io.usb_connected()) {
        return STACKEE_HID_BLE;
    }
    return STACKEE_HID_USB;
}

void stackee_hid_dest_set(stackee_hid_dest_t dest) {
    s_selected = (dest == STACKEE_HID_USB) ? STACKEE_HID_USB : STACKEE_HID_BLE;
    if (s_io.save != NULL) {
        s_io.save((uint8_t)s_selected);
    }
}

stackee_hid_dest_t stackee_hid_dest_toggle(void) {
    stackee_hid_dest_set(s_selected == STACKEE_HID_BLE ? STACKEE_HID_USB
                                                       : STACKEE_HID_BLE);
    return s_selected;
}

const char *stackee_hid_dest_name(stackee_hid_dest_t dest) {
    return (dest == STACKEE_HID_USB) ? "USB" : "BLE";
}
