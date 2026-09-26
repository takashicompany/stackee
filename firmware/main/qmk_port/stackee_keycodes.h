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
    MIC_F13              = QK_KB_0 + 8,
    MIC_F14              = QK_KB_0 + 9,
    MIC_F15              = QK_KB_0 + 10,
    MIC_F16              = QK_KB_0 + 11,
    MIC_F17              = QK_KB_0 + 12,
    MIC_F18              = QK_KB_0 + 13,
    MIC_F19              = QK_KB_0 + 14,
    MIC_F20              = QK_KB_0 + 15,
    MIC_F21              = QK_KB_0 + 16,
    MIC_F22              = QK_KB_0 + 17,
    MIC_F23              = QK_KB_0 + 18,
    MIC_F24              = QK_KB_0 + 19,
    MIC_F1               = QK_KB_0 + 20,
    MIC_F2               = QK_KB_0 + 21,
    MIC_F3               = QK_KB_0 + 22,
    MIC_F4               = QK_KB_0 + 23,
    MIC_F5               = QK_KB_0 + 24,
    MIC_F6               = QK_KB_0 + 25,
    MIC_F7               = QK_KB_0 + 26,
    MIC_F8               = QK_KB_0 + 27,
    MIC_F9               = QK_KB_0 + 28,
    MIC_F10              = QK_KB_0 + 29,
    MIC_F11              = QK_KB_0 + 30,
    MIC_F12              = QK_KB_0 + 31,
    STK_CSTM_0           = QK_KB_0 + 32,
    STK_CSTM_1           = QK_KB_0 + 33,
    STK_CSTM_2           = QK_KB_0 + 34,
    STK_CSTM_3           = QK_KB_0 + 35,
    STK_CSTM_4           = QK_KB_0 + 36,
    STK_CSTM_5           = QK_KB_0 + 37,
    STK_CSTM_6           = QK_KB_0 + 38,
    STK_CSTM_7           = QK_KB_0 + 39,
    STK_CSTM_8           = QK_KB_0 + 40,
    STK_CSTM_9           = QK_KB_0 + 41,
};

#define STACKEE_KEYCODE_FIRST QK_KB_0
#define STACKEE_KEYCODE_LAST  (QK_KB_0 + 41)
#define STK_MT_BASE           (QK_KB_0 + 7)

// stackee 独自キー CSTM_0..CSTM_9 (押した瞬間にサーバへ POST /key)。
// 並びは customKeycodes と同じ順。MIC_F1..F12 のうしろ。
#define STACKEE_CSTM_FIRST    STK_CSTM_0
#define STACKEE_CSTM_COUNT    10

// ---------------------------------------------------------------
// MIC(kc) — 押している間だけ顔を「聞き取り中」にする包み
// ---------------------------------------------------------------
// QMK の LT(layer, kc) / MT(mod, kc) と同じ発想で、**中のキーを
// 8 bit そのまま**持つ連続領域。押下で中のキーを register_code、
// 離しで unregister_code — ホストから見た振る舞いは素のキーと同じ。
//
// ★ 置き場は QK_USER (0x7E40..0x7FFF) の上半分。QK_KB は 0x7E00..
//   0x7E3F の 64 個しかなく、256 個の連続領域が入らないため。
//   VIA の Custom タブには上の MIC_F13..MIC_F24 / MIC_F1..MIC_F12 が
//   並び、本体がそれを MIC(kc) に読み替える。Remap の「Any」なら
//   0x7F00 | kc を直に書ける。
#define STACKEE_MIC_BASE      0x7F00u
#define STACKEE_MIC_KC_MIN    0x04u      // KC_A。これ未満は包まない
#define STACKEE_MIC_FIRST     (STACKEE_MIC_BASE | STACKEE_MIC_KC_MIN)
#define STACKEE_MIC_LAST      (STACKEE_MIC_BASE | 0xFFu)
#define MIC(kc)               (STACKEE_MIC_BASE | ((kc) & 0xFFu))
#define STACKEE_MIC_INNER(code) ((uint8_t)((code) & 0xFFu))

// VIA の名前付きの入口 (MIC_F13..MIC_F24、そのうしろに MIC_F1..MIC_F12)。
// 並びは customKeycodes と同じ順。うしろにしか足さない。
#define STACKEE_MIC_ALIAS_FIRST (QK_KB_0 + 8)
#define STACKEE_MIC_ALIAS_LAST  (QK_KB_0 + 31)
#define STACKEE_MIC_ALIAS_COUNT 24
// 入口 i が送る基本キーコード (i = keycode - STACKEE_MIC_ALIAS_FIRST)。
#define STACKEE_MIC_ALIAS_KEYCODES \
    { 0x68u, 0x69u, 0x6Au, 0x6Bu, 0x6Cu, 0x6Du, 0x6Eu, 0x6Fu, 0x70u, 0x71u, 0x72u, 0x73u, 0x3Au, 0x3Bu, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu, 0x40u, 0x41u, 0x42u, 0x43u, 0x44u, 0x45u }

// 修飾つきタップの HoldTap (default_keymap.c が中身を持つ)。
typedef struct {
    uint8_t  mods;          // hold で押さえる修飾キー (MOD_*)
    uint16_t tap;           // tap で送るキーコード (16bit)
    uint16_t tapping_term;
    bool     hold_on_other_key_press;
} stackee_ext_mt_t;

extern const stackee_ext_mt_t stackee_ext_mt[];
extern const uint8_t stackee_ext_mt_count;
