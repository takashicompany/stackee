#include "stackee_keymap_migrate.h"

#include <string.h>

#include "dynamic_keymap.h"
#include "eeconfig.h"
#include "quantum.h"
#include "stackee_keycodes.h"

// 移行番号は 4 バイトの下 16 bit だけ使う。上 16 bit は将来のために空けておく
// (いまは 0 のまま書く)。
#define LEVEL_MASK 0xFFFFu

// 1 段ぶん。書き換えたキーの数を返す。
typedef int (*migration_fn)(void);

// 最後に stackee_keymap_migrate() が何をしたか (ログと status 用)。
static stackee_keymap_migration_report_t s_report;

// 「旧い既定のままなら差し替える」を 1 か所に。
// ★ `was` のどれかに一致したときだけ書く。ユーザーが別のキーにしていたら
//   触らない。
static int replace_if_default(uint8_t layer, uint8_t row, uint8_t col,
                              const uint16_t *was, size_t was_count,
                              uint16_t now) {
    uint16_t have = dynamic_keymap_get_keycode(layer, row, col);
    if (have == now) {
        return 0;               // もうそうなっている
    }
    for (size_t i = 0; i < was_count; i++) {
        if (have == was[i]) {
            dynamic_keymap_set_keycode(layer, row, col, now);
            return 1;
        }
    }
    return 0;                   // ユーザーが変えたもの。触らない
}

// ---------------------------------------------------------------------------
// 移行 1 (2026-09-21): 右下のキーを MIC(KC_F13) にする。
//
//   旧い既定は素の KC_F13 (0x0068)。PC 側のプッシュトゥトークに使っている
//   キーで、**送るものは変えない** — 押している間だけ顔を「聞き取り中」に
//   する包み (MIC) を被せるだけ。
//
//   0x7E08 も見るのは、同じ日に一度だけ入れた STK_MIC_KEY がその番号
//   だったため (そのあと 2026-09-29 までは MIC_F13 = MIC(KC_F13) の
//   名前付きの入口)。
//
//   ★ 0x7E08 は 2026-09-29 から STK_CUSTOM_0 の番号。それでもここで
//     MIC(KC_F13) に読むのが正しいのは、**番号 0 のまま Custom_0 が入って
//     いる本体は無い**から: Custom_0 を 0x7E08 に置いた像は、起動した時点で
//     番号を 2 まで進める (保存済みの配列を書ける VIA はそのあと)。
//     番号が 0 に戻るのは eeconfig の初期化だけで、そのときは配列も
//     既定に戻る。移行 2 も同じ理屈で 0x7E08 を旧い MIC_F13 として読む。
// ---------------------------------------------------------------------------
static int migration_1(void) {
    static const uint16_t was[] = {
        KC_F13,                 // 旧い既定
        (uint16_t)(QK_KB_0 + 8) // 一度だけ存在した STK_MIC_KEY (= 旧 MIC_F13)
    };
    return replace_if_default(0, 3, 9, was, sizeof(was) / sizeof(was[0]),
                              MIC(KC_F13));
}

// ---------------------------------------------------------------------------
// 移行 2 (2026-09-29): VIA の独自キーの並びを詰める。
//
//   Remap は customKeycodes を **先頭 32 個 (0x7E00..0x7E1F) しか出さない**。
//   並び 8..31 にいた MIC の名前付きの入口 24 個のうしろ (0x7E20..0x7E29)
//   に置いた CSTM_0..9 (同じ日に Custom_0..9 へ改名) が Remap に出なかった
//   ので、入口を消して Custom を STK_MT_0 の直後 (0x7E08..0x7E11) へ詰めた。MIC(kc) の領域
//   (0x7F00 | kc) と本体の振る舞いは変えていない。
//
//     旧 0x7E08..0x7E13 (MIC_F13..MIC_F24) → MIC(KC_F13..KC_F24) = 0x7F68..0x7F73
//     旧 0x7E14..0x7E1F (MIC_F1..MIC_F12)  → MIC(KC_F1..KC_F12)  = 0x7F3A..0x7F45
//     旧 0x7E20..0x7E29 (CSTM_0..CSTM_9)   → Custom_0..Custom_9  = 0x7E08..0x7E11
//
//   ★ **旧 0x7E08 (MIC_F13) と新 Custom_0 (0x7E08) は同じ番号。** 取り違え
//     ないよう、1 キーを 1 回だけ読み、**読んだ値 (旧い番号) だけで**
//     新しい番号を決めて書く。書いた値をもう一度表に通すことはしない
//     (旧 CSTM_0 → 0x7E08 → MIC(F13) の連鎖が起きない)。
//   ★ ここは「ユーザーが置いたキーの番号を付け替える」移行なので、
//     移行 1 と違って位置を問わず全レイヤー・全キーを見る。旧い番号の
//     意味は 1 つしかないので、ユーザーの意図は変わらない。
//   ★ 一度しか当たらない (番号 2)。当て直すと新しい Custom_0 (0x7E08) を
//     旧 MIC_F13 と読んでしまうので、番号で守ること。
// ---------------------------------------------------------------------------
#define OLD_MIC_F13_FIRST 0x7E08u   // MIC_F13..MIC_F24
#define OLD_MIC_F1_FIRST  0x7E14u   // MIC_F1..MIC_F12
#define OLD_CUSTOM_FIRST  0x7E20u   // 旧 CSTM_0..CSTM_9
#define OLD_CUSTOM_LAST   0x7E29u

_Static_assert(STK_CUSTOM_0 == QK_KB_0 + 8, "移行 2 は新しい Custom_0 = 0x7E08 を前提にしている");
_Static_assert(STACKEE_CUSTOM_COUNT == 10, "移行 2 は Custom 10 個を前提にしている");

// 旧い番号 → 新しい番号。旧い番号でなければ 0 を返す。
static uint16_t migration_2_map(uint16_t old, bool *is_custom) {
    if (old >= OLD_MIC_F13_FIRST && old < OLD_MIC_F1_FIRST) {
        *is_custom = false;
        return (uint16_t)MIC(KC_F13 + (old - OLD_MIC_F13_FIRST));
    }
    if (old >= OLD_MIC_F1_FIRST && old < OLD_CUSTOM_FIRST) {
        *is_custom = false;
        return (uint16_t)MIC(KC_F1 + (old - OLD_MIC_F1_FIRST));
    }
    if (old >= OLD_CUSTOM_FIRST && old <= OLD_CUSTOM_LAST) {
        *is_custom = true;
        return (uint16_t)(STK_CUSTOM_0 + (old - OLD_CUSTOM_FIRST));
    }
    return 0;
}

static int migration_2(void) {
    int     changed = 0;
    uint8_t layers = dynamic_keymap_get_layer_count();
    for (uint8_t layer = 0; layer < layers; layer++) {
        for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
            for (uint8_t col = 0; col < MATRIX_COLS; col++) {
                uint16_t have = dynamic_keymap_get_keycode(layer, row, col);
                bool     is_custom = false;
                uint16_t now = migration_2_map(have, &is_custom);
                if (now == 0) {
                    continue;
                }
                dynamic_keymap_set_keycode(layer, row, col, now);
                changed++;
                if (is_custom) {
                    s_report.custom++;
                } else {
                    s_report.mic++;
                }
            }
        }
#ifdef ENCODER_MAP_ENABLE
        // いまの本体にエンコーダは無い。付けたときに黙って漏れないように。
        for (uint8_t enc = 0; enc < NUM_ENCODERS; enc++) {
            for (int cw = 0; cw < 2; cw++) {
                uint16_t have = dynamic_keymap_get_encoder(layer, enc, cw != 0);
                bool     is_custom = false;
                uint16_t now = migration_2_map(have, &is_custom);
                if (now == 0) {
                    continue;
                }
                dynamic_keymap_set_encoder(layer, enc, cw != 0, now);
                changed++;
                if (is_custom) {
                    s_report.custom++;
                } else {
                    s_report.mic++;
                }
            }
        }
#endif
    }
    return changed;
}

static const migration_fn MIGRATIONS[STACKEE_KEYMAP_MIGRATION_LATEST] = {
    migration_1,
    migration_2,
};

// ---------------------------------------------------------------------------
uint32_t stackee_keymap_migration_level(void) {
    return eeconfig_read_kb() & LEVEL_MASK;
}

int stackee_keymap_migrate(void) {
    uint32_t level = stackee_keymap_migration_level();
    memset(&s_report, 0, sizeof(s_report));
    s_report.ran = true;
    s_report.from_level = level;
    s_report.level = level;
    if (level >= STACKEE_KEYMAP_MIGRATION_LATEST) {
        // ★ 先に進んでいる (新しい像で当てたあと古い像で起きた) ときも
        //   何もしない。当て直すと新しい既定を古い既定へ戻してしまう。
        return 0;
    }
    int changed = 0;
    for (uint32_t step = level; step < STACKEE_KEYMAP_MIGRATION_LATEST; step++) {
        changed += MIGRATIONS[step]();
    }
    uint32_t keep = eeconfig_read_kb() & ~(uint32_t)LEVEL_MASK;
    eeconfig_update_kb(keep | STACKEE_KEYMAP_MIGRATION_LATEST);
    s_report.level = STACKEE_KEYMAP_MIGRATION_LATEST;
    s_report.changed = changed;
    return changed;
}

void stackee_keymap_migration_report(stackee_keymap_migration_report_t *out) {
    if (out != NULL) {
        *out = s_report;
    }
}
