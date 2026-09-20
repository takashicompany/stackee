// CDC 上の常駐コンソール。枠も応答も CircuitPython 版 (firmware/kmk/
// stackee_console.py) と同じにしてあるので、firmware/kmk/tools/
// stackee_console_client.py がそのまま使える。
//
//   ホスト → デバイス   \x1e{"id":1,"cmd":"status"}\n
//   デバイス → ホスト   \x1e{"id":1, ... }\n
//   枠の外に流れるものは全部ログ (esp_log の出力もここに混ざる)。
//
// 段階 0 で答えるのは hello と status の 2 つだけ。
#pragma once

#include <stdbool.h>
#include <stddef.h>

void stackee_console_init(void);

// メインタスクから毎周呼ぶ。CDC を読んで、揃った行にだけ答える。ブロックしない。
void stackee_console_poll(void);

// esp_log の出力を CDC へも流す (枠の外 = ログ扱いになる)。
void stackee_console_attach_log(void);

// ---------------------------------------------------------------------------
// 段階 2: 画面まわりのコマンドを別のファイルから足す
// ---------------------------------------------------------------------------
// ★ 関数ポインタで受け取る。こうしておくと、ホストビルド
//   (hostbuild/console_main.c) は何も登録しないだけで済み、画面の実装
//   (ESP-IDF に依存する stackee_ui.c) をリンクせずにコンソールを試せる。
//
// 戻り値は buf に書いた長さ。0 なら「このコマンドは知らない」。
typedef size_t (*stackee_console_ext_t)(const char *cmd, const char *line,
                                        long id, char *buf, size_t cap);

void stackee_console_register(stackee_console_ext_t handler);

// 拡張コマンドの側が使う小道具 ("key": のうしろを指す。無ければ NULL)。
const char *stackee_console_value(const char *json, const char *key);
long        stackee_console_int(const char *json, const char *key, long fallback);
bool        stackee_console_bool(const char *json, const char *key, bool fallback);
// "key":"文字列" を取り出す。取れたら true。
bool        stackee_console_str(const char *json, const char *key,
                                char *out, size_t cap);
