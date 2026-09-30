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
    STK_CUSTOM_0         = QK_KB_0 + 8,
    STK_CUSTOM_1         = QK_KB_0 + 9,
    STK_CUSTOM_2         = QK_KB_0 + 10,
    STK_CUSTOM_3         = QK_KB_0 + 11,
    STK_CUSTOM_4         = QK_KB_0 + 12,
    STK_CUSTOM_5         = QK_KB_0 + 13,
    STK_CUSTOM_6         = QK_KB_0 + 14,
    STK_CUSTOM_7         = QK_KB_0 + 15,
    STK_CUSTOM_8         = QK_KB_0 + 16,
    STK_CUSTOM_9         = QK_KB_0 + 17,
    STK_CLIP             = QK_KB_0 + 18,
    STK_CLIP_AUTO        = QK_KB_0 + 19,
    STK_SETTINGS         = QK_KB_0 + 20,
};

#define STACKEE_KEYCODE_FIRST QK_KB_0
#define STACKEE_KEYCODE_LAST  (QK_KB_0 + 20)
#define STK_MT_BASE           (QK_KB_0 + 7)

// stackee 独自キー Custom_0..Custom_9 (押した瞬間にサーバへ POST /key)。
// 並びは customKeycodes と同じ順。STK_MT_* のうしろ (0x7E08..、2026-09-29)。
#define STACKEE_CUSTOM_FIRST  STK_CUSTOM_0
#define STACKEE_CUSTOM_COUNT  10

// ---------------------------------------------------------------
// MIC(kc) — 押している間だけ顔を「聞き取り中」にする包み
// ---------------------------------------------------------------
// QMK の LT(layer, kc) / MT(mod, kc) と同じ発想で、**中のキーを
// 8 bit そのまま**持つ連続領域。押下で中のキーを register_code、
// 離しで unregister_code — ホストから見た振る舞いは素のキーと同じ。
//
// ★ 置き場は QK_USER (0x7E40..0x7FFF) の上半分。QK_KB は 0x7E00..
//   0x7E3F の 64 個しかなく、256 個の連続領域が入らないため。
//   VIA の customKeycodes には出さない。Remap の「カスタム」タブで
//   0x7F00 | kc を 16 進で手入力する (例: MIC(F13) = 7F68)。
#define STACKEE_MIC_BASE      0x7F00u
#define STACKEE_MIC_KC_MIN    0x04u      // KC_A。これ未満は包まない
#define STACKEE_MIC_FIRST     (STACKEE_MIC_BASE | STACKEE_MIC_KC_MIN)
#define STACKEE_MIC_LAST      (STACKEE_MIC_BASE | 0xFFu)
#define MIC(kc)               (STACKEE_MIC_BASE | ((kc) & 0xFFu))
#define STACKEE_MIC_INNER(code) ((uint8_t)((code) & 0xFFu))

// 修飾つきタップの HoldTap (default_keymap.c が中身を持つ)。
typedef struct {
    uint8_t  mods;          // hold で押さえる修飾キー (MOD_*)
    uint16_t tap;           // tap で送るキーコード (16bit)
    uint16_t tapping_term;
    bool     hold_on_other_key_press;
} stackee_ext_mt_t;

extern const stackee_ext_mt_t stackee_ext_mt[];
extern const uint8_t stackee_ext_mt_count;
