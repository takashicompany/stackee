// /settings.toml の読み取り。CircuitPython の supervisor が読むのと同じ
// 「行頭のベアキー = 値」だけを見る (supervisor/shared/settings.c と同じ範囲)。
//
// ★ 段階 3 で要るのは読むほうだけ。settings.set (書き込み) は段階 4。
//   FAT は読み取り専用でマウントしているので、ここからは書けない。
//
// ★ 値にパスワードやトークンが入る。**ログにも応答にも出さない。**
//   外へ出してよいのは stackee_settings_has() の真偽だけ。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define STACKEE_SETTINGS_MAX_KEYS   24
#define STACKEE_SETTINGS_KEY_MAX    40
#define STACKEE_SETTINGS_VALUE_MAX  200

// 全文を読み込む (呼び出し側が持つ文字列。中身はここへ写す)。
// 返すのは拾えたキーの数。
int stackee_settings_load_text(const char *text);

// 無ければ NULL。
const char *stackee_settings_get(const char *key);

// 値が空でなく存在するか (秘密のキーはこれだけを外へ出す)。
bool stackee_settings_has(const char *key);

// 整数として読む。無い / 数でないなら fallback。
long stackee_settings_int(const char *key, long fallback);

int stackee_settings_count(void);

// テスト用: 読み込んだ n 番目のキー名。
const char *stackee_settings_key_at(int index);

// ---------------------------------------------------------------------------
// 段階 4: settings.get / settings.raw / settings.set
// ---------------------------------------------------------------------------
// ★ 対応表は現行 firmware/kmk/stackee_console.py と**同じ並び**にしてある。
//   ページ (docs) はこの名前を見て欄を出すので、増減させない。

// settings.set で書き換えてよいキーか。
bool stackee_settings_key_allowed(const char *key);
// 値を外へ出してはいけないキーか (「設定済みか」だけを返す)。
bool stackee_settings_key_secret(const char *key);
// settings.get / settings.raw で見せるキーの並び (NULL 終端)。
const char *const *stackee_settings_report_keys(void);
const char *const *stackee_settings_secret_keys(void);

// パスワードの値だけを伏せた全文を out へ書く。返すのは**必要だった**長さ
// (cap を超えていたら切れている)。
size_t stackee_settings_mask(const char *text, char *out, size_t cap);

// keys[i] = values[i] だけを差し替えた全文を out へ書く。
// values[i] が NULL ならその行を消す。無いキーは末尾に足す。
// コメント行 (#KEY = ...) はキーとして扱わないので絶対に触らない。
// 返すのは必要だった長さ。
size_t stackee_settings_rewrite(const char *text,
                                const char *const *keys,
                                const char *const *values,
                                int n, char *out, size_t cap);
