// ★ 生成物。手で編集しない。
//
//     python3 firmware/tools/gen_keymap.py
//
// 出所は firmware/kmk/keymap.py (KEYMAP / ASSIGN / COORD_MAPPING)。
// KMK と QMK の対応表は firmware/tools/keycodes.md。
//
// 配線があるのは 5x10 = 50 スロットのうち 43 個。残り 7 個は KC_NO で埋めて
// ある (そこからイベントが来ることは無い。来たら配線異常)。
#include "quantum.h"

#include "stackee_keycodes.h"

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    // ---- レイヤー 0: BASE ----
    [0] = {
        /* row 0 */ { LT(4, KC_Q)           , KC_W                  , KC_E                  , LT(3, KC_R)           , KC_T                  , KC_Y                  , KC_U                  , KC_I                  , KC_O                  , KC_P },
        /* row 1 */ { KC_A                  , KC_S                  , KC_D                  , KC_F                  , KC_G                  , KC_H                  , KC_J                  , LT(3, KC_K)           , KC_L                  , KC_ENTER },
        /* row 2 */ { MT(MOD_LSFT, KC_Z)    , MT(MOD_LGUI, KC_X)    , KC_C                  , KC_V                  , KC_B                  , KC_N                  , KC_M                  , KC_COMMA              , MT(MOD_LCTL, KC_DOT)  , KC_BACKSPACE },
        /* row 3 */ { STK_VOLDN             , STK_VOLUP             , KC_LEFT_GUI           , MT(MOD_LALT, KC_LANGUAGE_2), MT(MOD_LSFT, KC_TAB)  , LT(1, KC_LANGUAGE_1)  , LT(1, KC_LANGUAGE_1)  , KC_RIGHT_GUI          , KC_NO                 , KC_F13 },
        /* row 4 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , STK_TOUCH_SCROLL      , STK_TALK              , LT(2, KC_SPACE)       , KC_NO                 , KC_NO                 , KC_NO },
    },
    // ---- レイヤー 1: 記号 (JIS) ----
    [1] = {
        /* row 0 */ { KC_1                  , KC_2                  , KC_3                  , KC_4                  , KC_5                  , KC_6                  , KC_7                  , KC_8                  , KC_9                  , KC_0 },
        /* row 1 */ { MT(MOD_LCTL, KC_EQUAL), KC_LEFT_BRACKET       , KC_SLASH              , KC_MINUS              , KC_INTERNATIONAL_1    , KC_SEMICOLON          , KC_QUOTE              , KC_RIGHT_BRACKET      , KC_NONUS_HASH         , KC_INTERNATIONAL_3 },
        /* row 2 */ { STK_MT_0              , LSFT(KC_LEFT_BRACKET) , LSFT(KC_SLASH)        , LSFT(KC_INTERNATIONAL_1), LSFT(KC_INTERNATIONAL_1), LSFT(KC_SEMICOLON)    , LSFT(KC_QUOTE)        , LSFT(KC_RIGHT_BRACKET), LSFT(KC_NONUS_HASH)   , LSFT(KC_INTERNATIONAL_3) },
        /* row 3 */ { KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS },
        /* row 4 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_NO                 , KC_NO                 , KC_NO },
    },
    // ---- レイヤー 2: 記号 2 / 修飾 ----
    [2] = {
        /* row 0 */ { LSFT(KC_1)            , LSFT(KC_2)            , LSFT(KC_3)            , LSFT(KC_4)            , LSFT(KC_5)            , LSFT(KC_6)            , LSFT(KC_7)            , LSFT(KC_8)            , LSFT(KC_9)            , LGUI(KC_INTERNATIONAL_3) },
        /* row 1 */ { LSFT(KC_EQUAL)        , LSFT(KC_LEFT_BRACKET) , LSFT(KC_SLASH)        , LSFT(KC_MINUS)        , LSFT(KC_INTERNATIONAL_1), LSFT(KC_SEMICOLON)    , LSFT(KC_QUOTE)        , LSFT(KC_RIGHT_BRACKET), LSFT(KC_NONUS_HASH)   , LSFT(KC_INTERNATIONAL_3) },
        /* row 2 */ { KC_LEFT_SHIFT         , KC_LEFT_GUI           , KC_LEFT_ALT           , KC_LANGUAGE_2         , KC_LEFT_SHIFT         , KC_SPACE              , KC_LANGUAGE_1         , KC_TRNS               , KC_TRNS               , KC_DELETE },
        /* row 3 */ { KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_NO },
        /* row 4 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_NO                 , KC_NO                 , KC_NO },
    },
    // ---- レイヤー 3: ナビ ----
    [3] = {
        /* row 0 */ { KC_ESCAPE             , KC_TAB                , KC_UP                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_UP                 , KC_NO                 , KC_NO                 , KC_NO },
        /* row 1 */ { KC_LEFT_CTRL          , KC_LEFT               , KC_DOWN               , KC_RIGHT              , KC_NO                 , KC_LEFT               , KC_DOWN               , KC_RIGHT              , KC_NO                 , KC_NO },
        /* row 2 */ { KC_LEFT_SHIFT         , KC_LEFT_GUI           , KC_LEFT_ALT           , KC_LANGUAGE_2         , KC_TRNS               , KC_NO                 , KC_LANGUAGE_1         , KC_NO                 , KC_NO                 , KC_DELETE },
        /* row 3 */ { KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS },
        /* row 4 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_NO                 , KC_NO                 , KC_NO },
    },
    // ---- レイヤー 4: F キー ----
    [4] = {
        /* row 0 */ { KC_NO                 , KC_TAB                , KC_NO                 , KC_NO                 , KC_F1                 , KC_F2                 , KC_F3                 , KC_F4                 , KC_F5                 , KC_F6 },
        /* row 1 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_F7                 , KC_F8                 , KC_F9                 , KC_F10                , KC_F11                , KC_F12 },
        /* row 2 */ { KC_LEFT_SHIFT         , KC_NO                 , KC_NO                 , KC_NO                 , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_NO                 , MO(5)                 , KC_NO },
        /* row 3 */ { KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS },
        /* row 4 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_NO                 , KC_NO                 , KC_NO },
    },
    // ---- レイヤー 5: システム ----
    [5] = {
        /* row 0 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO },
        /* row 1 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO },
        /* row 2 */ { KC_NO                 , KC_NO                 , KC_NO                 , STK_BLE_REFRESH       , STK_HID_SWITCH        , QK_BOOT               , KC_NO                 , KC_NO                 , KC_NO                 , KC_NO },
        /* row 3 */ { KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_TRNS },
        /* row 4 */ { KC_NO                 , KC_NO                 , KC_NO                 , KC_NO                 , KC_TRNS               , KC_TRNS               , KC_TRNS               , KC_NO                 , KC_NO                 , KC_NO },
    },
};

// --------------------------------------------------------------------------
// HoldTap のキーごとの設定 (KMK の prefer_hold / tap_interrupted / tap_time)
// --------------------------------------------------------------------------
// ★ 位置 (row/col) とキーコードの両方を持つ。同じキーコードでも設定が違う
//   ことがあるため: レイヤー 0 のキー 36 と 37 はどちらも LT(1, LANG1) だが、
//   36 だけ prefer_hold=True。キーコードだけで引くと片方が黙って消える。
typedef struct {
    uint8_t  row;
    uint8_t  col;
    uint16_t keycode;
    uint16_t tapping_term;
    bool     hold_on_other_key_press;   // KMK の prefer_hold
    bool     permissive_hold;           // KMK の tap_interrupted
} stackee_holdtap_t;

static const stackee_holdtap_t s_holdtap[] = {
    { 0, 0, LT(4, KC_Q)               , 300, false, false },
    { 0, 3, LT(3, KC_R)               , 300, false, false },
    { 1, 7, LT(3, KC_K)               , 300, false, false },
    { 2, 0, MT(MOD_LSFT, KC_Z)        , 200, true , false },
    { 2, 1, MT(MOD_LGUI, KC_X)        , 200, true , false },
    { 2, 8, MT(MOD_LCTL, KC_DOT)      , 200, true , false },
    { 3, 3, MT(MOD_LALT, KC_LANGUAGE_2), 200, true , false },
    { 3, 4, MT(MOD_LSFT, KC_TAB)      , 200, true , false },
    { 4, 6, LT(2, KC_SPACE)           , 300, true , false },
    { 3, 5, LT(1, KC_LANGUAGE_1)      , 300, true , false },
    { 3, 6, LT(1, KC_LANGUAGE_1)      , 300, false, false },
    { 1, 0, MT(MOD_LCTL, KC_EQUAL)    , 200, true , false },
    { 2, 0, STK_MT_0                  , 200, true , false },
};

#define STACKEE_HOLDTAP_COUNT (sizeof(s_holdtap) / sizeof(s_holdtap[0]))

// 引き方は 2 段:
//   1. 位置とキーコードが両方一致するもの (= 既定配列のまま使っている)
//   2. キーコードだけ一致するもの (= VIA でキーを別の場所へ動かした場合)
// どちらも無ければ QMK の既定 (TAPPING_TERM / false / false)。
static const stackee_holdtap_t *find_holdtap(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *by_keycode = NULL;
    for (size_t i = 0; i < STACKEE_HOLDTAP_COUNT; i++) {
        if (s_holdtap[i].keycode != keycode) {
            continue;
        }
        if (record != NULL && s_holdtap[i].row == record->event.key.row &&
            s_holdtap[i].col == record->event.key.col) {
            return &s_holdtap[i];
        }
        if (by_keycode == NULL) {
            by_keycode = &s_holdtap[i];
        }
    }
    return by_keycode;
}

uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *entry = find_holdtap(keycode, record);
    return entry ? entry->tapping_term : TAPPING_TERM;
}

bool get_hold_on_other_key_press(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *entry = find_holdtap(keycode, record);
    return entry ? entry->hold_on_other_key_press : false;
}

bool get_permissive_hold(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *entry = find_holdtap(keycode, record);
    return entry ? entry->permissive_hold : false;
}

// --------------------------------------------------------------------------
// 修飾つきタップの HoldTap (QMK の MT() では表せないもの)
// --------------------------------------------------------------------------
// 例: KMK の KC.HT(KC.LSFT(KC.SCLN), KC.LSFT) — 離せば「+」(JIS)、押さえれば
// Shift。QMK の MT() はタップ側を 1 バイトしか持てないので、独自キーコードに
// 逃がして qmk_port/stackee_holdtap.c が処理する。
const stackee_ext_mt_t stackee_ext_mt[] = {
    { MOD_LSFT                , LSFT(KC_SEMICOLON)    , 200, true  },
};

const uint8_t stackee_ext_mt_count = 1;
