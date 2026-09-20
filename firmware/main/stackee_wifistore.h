// Wi-Fi の登録簿。firmware/kmk/stackee_wifi_store.py の移植。
//
// 形 (JSON) は現行と 1 文字も同じにしてある:
//   {"v":1,"networks":[{"ssid":"..","password":"..","channel":10}, ...]}
//
// ★ 置き場は現行と違う。CircuitPython 版は FAT の /wifi_networks.json に
//   書いていたが、こちらは **FAT を読み取り専用でマウントしている**
//   (user_fs は摩耗平準化なしの生 FAT で、ESP-IDF の書き込み用 API は
//    WL 層を挟むので載せ替えられない)。そこで
//      初回起動  … FAT の /wifi_networks.json を読んで NVS へ移す
//      以後      … NVS ("stackee" / "wifi_nets") が正
//   とする。音量と同じ移し方 (DESIGN.md §8b) で、設定を失わない。
//
// ★ password は外へ出さない。console へ返してよいのは stackee_wifi_public()
//   の形だけ (has_password の真偽のみ)。ログにも書かない。
//
// ★ ESP-IDF に依存しない。hostbuild でそのまま動く。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define STACKEE_WIFI_MAX_NETWORKS 8
#define STACKEE_WIFI_SSID_MAX     33    // 32 オクテット + NUL
#define STACKEE_WIFI_PASS_MAX     64    // 63 文字 + NUL
#define STACKEE_WIFI_PASS_MIN     8
#define STACKEE_WIFI_CH_MIN       1
#define STACKEE_WIFI_CH_MAX       14
// 1 回の探索で覚えておくスキャン結果の上限 (stackee_wifi.py の SEEN_MAX)。
#define STACKEE_WIFI_SEEN_MAX     40
// 1 回の探索で回るチャネル数の上限 (登録簿の ch + 前回 + 走査順)。
#define STACKEE_WIFI_CHANNELS_MAX 20

typedef struct {
    char ssid[STACKEE_WIFI_SSID_MAX];
    char password[STACKEE_WIFI_PASS_MAX];
    int  channel;               // 0 = 不明 (Python 版の None)
} stackee_wifi_net_t;

typedef struct {
    stackee_wifi_net_t nets[STACKEE_WIFI_MAX_NETWORKS];
    int count;
} stackee_wifi_list_t;

typedef struct {
    char ssid[STACKEE_WIFI_SSID_MAX];
    int  channel;
    int  rssi;
} stackee_wifi_seen_t;

typedef struct {
    char ssid[STACKEE_WIFI_SSID_MAX];
    char password[STACKEE_WIFI_PASS_MAX];    // ★ ログ厳禁
    int  channel;
    int  rssi;
    int  index;                 // 登録簿での位置 (同じ強さなら小さいほうが勝つ)
} stackee_wifi_pick_t;

// 問題なければ NULL。あれば console にそのまま返すエラー名
// ("bad_ssid" / "bad_password" / "bad_channel")。
const char *stackee_wifi_validate(const char *ssid, const char *password,
                                  int channel);

// 全文から登録簿を作る。壊れていても例外にしない (読めない要素は捨てる)。
// note には NULL か "corrupt" が入る。
bool stackee_wifi_parse(const char *json, stackee_wifi_list_t *out,
                        const char **note);

// ファイル / NVS に入れる全文。書けた長さ (NUL を含まない)。
// ★ 入れ物に収まらなければ **0 を返して out を空にする**。半端な JSON を
//   保存すると登録簿を丸ごと失うので、呼び手は 0 を失敗として扱うこと。
size_t stackee_wifi_dumps(const stackee_wifi_list_t *list, char *out, size_t cap);

int  stackee_wifi_find(const stackee_wifi_list_t *list, const char *ssid);

// console の wifi.list がそのまま出す配列。
//   [{"ssid":"..","channel":10,"has_password":true}, ...]
// ★ **password は 1 文字も入らない**。現行 stackee_wifi_store.public() と
//   同じ形。ここを 1 か所にしてあるのは、ホストテストで
//   「パスワードが出ていないこと」を直接確かめられるようにするため
//   (実機の wifi.list は esp_wifi を要るので Mac では動かせない)。
// 入り切らなければ 0 を返して out を空にする。
size_t stackee_wifi_public_json(const stackee_wifi_list_t *list,
                                char *out, size_t cap);

// 同じ SSID があれば位置を保ったまま差し替える。無ければ末尾へ。
// 上限に達していて新規なら "full"。
const char *stackee_wifi_upsert(stackee_wifi_list_t *list, const char *ssid,
                                const char *password, int channel);

// 無ければ "not_found"。
const char *stackee_wifi_remove_ssid(stackee_wifi_list_t *list, const char *ssid);

// スキャン結果のうち登録済みのものから 1 つ選ぶ。いちばん強いもの、
// 同じ強さなら登録簿で先に出てくるほう。★ 戻り値に password が入る。
bool stackee_wifi_pick(const stackee_wifi_seen_t *seen, int seen_count,
                       const stackee_wifi_list_t *list,
                       stackee_wifi_pick_t *out);

// その探索でその SSID がいちばん強く見えたチャネル。無ければ 0。
int stackee_wifi_seen_channel(const stackee_wifi_seen_t *seen, int seen_count,
                              const char *ssid);

// 走査するチャネルの順番。登録簿の channel → 前回のチャネル →
// CircuitPython と同じ走査順 (6,1,11,3,9,13,2,4,8,12,5,7,10,14)。
int stackee_wifi_channel_order(const stackee_wifi_list_t *list, int last_channel,
                               int *out, int cap);

// そのチャネルで走査完了を待つべき時間 [ms] (12ch 以上はパッシブで 3 倍)。
int stackee_wifi_settle_ms(int channel);
