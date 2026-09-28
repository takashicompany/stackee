#include "stackee_selftest.h"

static const stackee_bar_state_t BARS[STACKEE_SELFTEST_BARS] = {
    // battery / charging / volume / wifi / link / ble_connected / battery_absent / mic_ready
    {100, false, 100, "up",        STACKEE_LINK_BLE,  true,  false, false},
    { 55, true,   50, "scan_wait", STACKEE_LINK_BLE,  false, false, false},
    { 15, false,   0, "wait",      STACKEE_LINK_USB,  false, false, false},
    { 72, false,  25, "up",        STACKEE_LINK_USB,  false, false, true },
    { -1, false,   5, "boot",      STACKEE_LINK_NONE, false, false, false},
    // 電池なし (DIN BASE を外した実機の値 = 0%)。電池の場所は空いたまま。
    {  0, false,  20, "up",        STACKEE_LINK_USB,  false, true,  true},
    { -1, false,  20, "off",       STACKEE_LINK_BLE,  false, false, false},
};

static const char *const NAMES[STACKEE_SELFTEST_BARS] = {
    "bat100/vol100/wifi-up/ble-connected",
    "bat55-charging/vol50/wifi-scan/ble-adv",
    "bat15/vol0/wifi-off/usb",
    "bat72/vol25/wifi-up/usb/mic",
    "bat-unknown/vol5/wifi-boot/link-unknown",
    "bat-absent/vol20/wifi-up/usb/mic",
    "boot-default",
};

const stackee_bar_state_t *stackee_selftest_bar(int index) {
    if (index < 0 || index >= STACKEE_SELFTEST_BARS) {
        return &BARS[0];
    }
    return &BARS[index];
}

const char *stackee_selftest_bar_name(int index) {
    if (index < 0 || index >= STACKEE_SELFTEST_BARS) {
        return "?";
    }
    return NAMES[index];
}
