#include "stackee_keymap_migrate.h"

#include "dynamic_keymap.h"
#include "eeconfig.h"
#include "quantum.h"
#include "stackee_keycodes.h"

// 移行番号は 4 バイトの下 16 bit だけ使う。上 16 bit は将来のために空けておく
// (いまは 0 のまま書く)。
#define LEVEL_MASK 0xFFFFu

// 1 段ぶん。書き換えたキーの数を返す。
typedef int (*migration_fn)(void);

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
//   だったため。いまは MIC_F13 (MIC(KC_F13) の名前付きの入口) が同じ番号に
//   いるので意味は変わらないが、保存済みの配列は 1 つの形にそろえておく。
// ---------------------------------------------------------------------------
static int migration_1(void) {
    static const uint16_t was[] = {
        KC_F13,                 // 旧い既定
        (uint16_t)(QK_KB_0 + 8) // 一度だけ存在した STK_MIC_KEY (= いまの MIC_F13)
    };
    return replace_if_default(0, 3, 9, was, sizeof(was) / sizeof(was[0]),
                              MIC(KC_F13));
}

static const migration_fn MIGRATIONS[STACKEE_KEYMAP_MIGRATION_LATEST] = {
    migration_1,
};

// ---------------------------------------------------------------------------
uint32_t stackee_keymap_migration_level(void) {
    return eeconfig_read_kb() & LEVEL_MASK;
}

int stackee_keymap_migrate(void) {
    uint32_t level = stackee_keymap_migration_level();
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
    return changed;
}
