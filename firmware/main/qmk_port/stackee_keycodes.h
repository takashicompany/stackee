// ★ 生成物。手で編集しない (tools/gen_keymap.py)。
//
// Stackee の独自キーコード。VIA の customKeycodes は QK_KB_0 (0x7E00) から
// 順に並ぶ約束なので、この並び順と via/stackee.json の並び順は一致させる
// こと (tools/test_gen_keymap.py が確かめる)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "quantum.h"

enum stackee_keycodes {
    STK_TALK             = QK_KB_0 + 0,
    STK_VOLUP            = QK_KB_0 + 1,
    STK_VOLDN            = QK_KB_0 + 2,
    STK_HID_SWITCH       = QK_KB_0 + 3,
    STK_BLE_REFRESH      = QK_KB_0 + 4,
    STK_CAMERA           = QK_KB_0 + 5,
    STK_TOUCH_SCROLL     = QK_KB_0 + 6,
    STK_MT_0             = QK_KB_0 + 7,
    STK_MIC_KEY          = QK_KB_0 + 8,
};

#define STACKEE_KEYCODE_FIRST QK_KB_0
#define STACKEE_KEYCODE_LAST  (QK_KB_0 + 8)
#define STK_MT_BASE           (QK_KB_0 + 7)

// 修飾つきタップの HoldTap (default_keymap.c が中身を持つ)。
typedef struct {
    uint8_t  mods;          // hold で押さえる修飾キー (MOD_*)
    uint16_t tap;           // tap で送るキーコード (16bit)
    uint16_t tapping_term;
    bool     hold_on_other_key_press;
} stackee_ext_mt_t;

extern const stackee_ext_mt_t stackee_ext_mt[];
extern const uint8_t stackee_ext_mt_count;
