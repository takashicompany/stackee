// ごく小さな JSON の読み取り。段階 3 でサーバの応答と設定を読むためだけのもの。
//
// ★ **最上位のキーしか見ない。** stackee_console.c / stackee_assets.c の
//   strstr 方式は、入れ子や文字列の中に同じ名前があると拾い違える。会話の
//   返答文はサーバ (= Codex) が書いた任意の日本語なので、`"state"` や
//   `"reply"` という並びが本文に混ざる可能性がある。だから深さ 1 のキーだけを
//   走査する本物の字句解析にしてある。
//
// ★ ESP-IDF に依存しない。hostbuild でそのまま動く。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 最上位オブジェクトの key の値を指す。見つかれば true。
// *at は値の先頭 (引用符を含む)、*len はその値のバイト数。
bool stackee_json_raw(const char *json, const char *key,
                      const char **at, size_t *len);

// "key":"文字列" を取り出して逃がしを戻す (\" \\ \/ \b \f \n \r \t \uXXXX)。
// \uXXXX は UTF-8 に直す (サロゲート対も扱う)。入り切らなければ切り詰める。
bool stackee_json_str(const char *json, const char *key, char *out, size_t cap);

// "key":整数。小数点以下は捨てる。
bool stackee_json_int(const char *json, const char *key, long *out);

// "key":true / false。
bool stackee_json_bool(const char *json, const char *key, bool *out);

// 値そのもの (引用符つき文字列) を逃がしを戻して取り出す。
// stackee_json_str は内部でこれを使う。
size_t stackee_json_unescape(const char *at, size_t len, char *out, size_t cap);
