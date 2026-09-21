// 独自キーの処理と、QMK の MT() で表せない HoldTap。
//
// 1. 独自キー (STK_TALK / STK_VOLUP / ...)
//    process_record_kb で受け止めて false を返す。**HID には 1 バイトも
//    出さない** — ホスト側から見て存在しないキーである、というのが
//    KMK 版と同じ振る舞い。中身 (録音・音量・BLE) は段階 3 / 4。
//
//    ★ **例外が 1 つだけ: MIC(kc)。** 中のキーは**そのままホストへ送る**。
//      足したのは「押している間だけ本体の顔を聞き取り中にする」ことだけ。
//      ホストから見た振る舞いを変えないのが肝なので、送るのは
//      register_code/unregister_code — 普通のキーとまったく同じ道。
//
// 2. 修飾つきタップの HoldTap (STK_MT_n)
//    KMK の KC.HT(KC.LSFT(KC.SCLN), KC.LSFT) のように「タップ側が修飾つき」
//    の HoldTap は、QMK の MT() がタップを 1 バイトしか持てないので表せない。
//    ここに小さな状態機械を置いて同じ動きをさせる。
//
//    KMK (kmk/modules/holdtap.py) の動き:
//      prefer_hold=True … 他のキーが押された時点で hold 側を確定
//      tap_time 経過   … hold 側を確定
//      それより前に離す … tap 側を送る
#include <string.h>

#include "qmk_port.h"
#include "quantum.h"
#include "stackee_keycodes.h"

#define STACKEE_EXT_MT_MAX 8

// VIA の名前付きの入口 (MIC_F13..MIC_F24 / MIC_F1..MIC_F12) を MIC(kc) に
// 直す。入口でなければそのまま返す。
//
// ★ 表は生成物 (stackee_keycodes.h)。並びは via/stackee.json の
//   customKeycodes と同じ順で、**うしろにしか足さない** — 番号は
//   保存済みの配列に入っているため。
static uint16_t mic_resolve(uint16_t keycode) {
    static const uint8_t inner[STACKEE_MIC_ALIAS_COUNT] =
        STACKEE_MIC_ALIAS_KEYCODES;
    if (keycode >= STACKEE_MIC_ALIAS_FIRST && keycode <= STACKEE_MIC_ALIAS_LAST) {
        return MIC(inner[keycode - STACKEE_MIC_ALIAS_FIRST]);
    }
    return keycode;
}

typedef struct {
    bool     pending;       // 押されたが tap/hold が決まっていない
    bool     held;          // hold 側で確定して修飾キーを押している
    uint16_t start_ms;
} ext_mt_state_t;

static ext_mt_state_t s_ext[STACKEE_EXT_MT_MAX];
static uint32_t       s_custom_presses;

static void ext_mt_settle_hold(uint8_t index) {
    if (!s_ext[index].pending) {
        return;
    }
    s_ext[index].pending = false;
    s_ext[index].held = true;
    register_mods(stackee_ext_mt[index].mods);
}

// 他のキーが押された。prefer_hold のものだけ hold 側で確定する。
static void ext_mt_interrupt(void) {
    for (uint8_t i = 0; i < stackee_ext_mt_count && i < STACKEE_EXT_MT_MAX; i++) {
        if (s_ext[i].pending && stackee_ext_mt[i].hold_on_other_key_press) {
            ext_mt_settle_hold(i);
        }
    }
}

// tap_time を過ぎたら hold 側で確定する。matrix_scan_kb から毎周回。
void matrix_scan_kb(void) {
    uint16_t now = timer_read();
    for (uint8_t i = 0; i < stackee_ext_mt_count && i < STACKEE_EXT_MT_MAX; i++) {
        if (!s_ext[i].pending) {
            continue;
        }
        if (TIMER_DIFF_16(now, s_ext[i].start_ms) >= stackee_ext_mt[i].tapping_term) {
            ext_mt_settle_hold(i);
        }
    }
    matrix_scan_user();
}

bool process_record_kb(uint16_t keycode, keyrecord_t *record) {
    // ここより先へ進むキー (= 普通のキー) が押されたら、待機中の
    // 修飾つき HoldTap を割り込みとして扱う。
    if (record->event.pressed &&
        !(keycode >= STACKEE_KEYCODE_FIRST && keycode <= STACKEE_KEYCODE_LAST) &&
        !(keycode >= STACKEE_MIC_FIRST && keycode <= STACKEE_MIC_LAST)) {
        ext_mt_interrupt();
    }

    if (keycode >= STK_MT_BASE &&
        keycode < STK_MT_BASE + stackee_ext_mt_count) {
        uint8_t index = (uint8_t)(keycode - STK_MT_BASE);
        if (index >= STACKEE_EXT_MT_MAX) {
            return false;
        }
        if (record->event.pressed) {
            // 別の修飾つき HoldTap が待機中なら、それは割り込まれた。
            ext_mt_interrupt();
            s_ext[index].pending = true;
            s_ext[index].held = false;
            s_ext[index].start_ms = timer_read();
        } else if (s_ext[index].held) {
            s_ext[index].held = false;
            unregister_mods(stackee_ext_mt[index].mods);
        } else if (s_ext[index].pending) {
            s_ext[index].pending = false;
            tap_code16(stackee_ext_mt[index].tap);
        }
        return false;
    }

    // ★ MIC(kc) — 顔を変えつつ、中のキーは普通のキーとして送る。
    //   VIA の名前付きの入口 (MIC_F13..) もここで同じ形に直す。
    uint16_t mic = mic_resolve(keycode);
    if (mic >= STACKEE_MIC_FIRST && mic <= STACKEE_MIC_LAST) {
        uint8_t inner = STACKEE_MIC_INNER(mic);
        if (record->event.pressed) {
            s_custom_presses++;
            register_code(inner);
        } else {
            unregister_code(inner);
        }
        stackee_qmk_custom_key(STACKEE_KEY_MIC, record->event.pressed);
        return false;   // 送るのはこちらで済ませた
    }

    // QMK のキーコードを「何をしたいか」に翻訳してからアプリへ渡す。
    // 翻訳表はここ 1 か所だけ (tools/keycodes.md の並びと同じ)。
    static const struct {
        uint16_t             keycode;
        stackee_key_action_t action;
    } actions[] = {
        {STK_TALK, STACKEE_KEY_TALK},
        {STK_VOLUP, STACKEE_KEY_VOLUP},
        {STK_VOLDN, STACKEE_KEY_VOLDN},
        {STK_HID_SWITCH, STACKEE_KEY_HID_SWITCH},
        {STK_BLE_REFRESH, STACKEE_KEY_BLE_REFRESH},
        {STK_CAMERA, STACKEE_KEY_CAMERA},
        {STK_TOUCH_SCROLL, STACKEE_KEY_TOUCH_SCROLL},
    };
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++) {
        if (actions[i].keycode != keycode) {
            continue;
        }
        if (record->event.pressed) {
            s_custom_presses++;
        }
        stackee_qmk_custom_key(actions[i].action, record->event.pressed);
        return false;   // ★ HID へは出さない
    }
    return process_record_user(keycode, record);
}

uint32_t stackee_qmk_custom_key_count(void) {
    return s_custom_presses;
}

const char *stackee_key_action_name(stackee_key_action_t action) {
    switch (action) {
        case STACKEE_KEY_TALK: return "TALK";
        case STACKEE_KEY_VOLUP: return "VOLUP";
        case STACKEE_KEY_VOLDN: return "VOLDN";
        case STACKEE_KEY_HID_SWITCH: return "HID_SWITCH";
        case STACKEE_KEY_BLE_REFRESH: return "BLE_REFRESH";
        case STACKEE_KEY_CAMERA: return "CAMERA";
        case STACKEE_KEY_TOUCH_SCROLL: return "TOUCH_SCROLL";
        case STACKEE_KEY_MIC: return "MIC_KEY";
        default: return "?";
    }
}
