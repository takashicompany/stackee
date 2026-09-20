// Wi-Fi 自動接続の状態機械。firmware/kmk/stackee_wifi.py の移植。
//
//   登録簿 (stackee_wifistore) に入っている AP のうち、いま電波が届いていて
//   **いちばん強いもの**へ勝手に繋ぐ。1 周期 1 段しか進まない。
//
//   boot   -> 起動から 2 秒待つ
//   off    -> 登録 0 件。console の wifi.connect で起き直す
//   load   -> 登録簿を読む
//   wait   -> 次の探索時刻まで待つ
//   radio  -> 無線 ON。走査するチャネルの順番を決める
//   scan_start / scan_wait / scan_read -> 1ch ずつ走査する
//   connect -> 打鍵の谷を待って接続を始める (ここは止まらない)
//   linkup -> 接続の完了待ち
//   up     -> 接続済み。10 秒に 1 回だけ生きているか見る
//
// ★ 名前と順番は現行 CircuitPython 版の STATE_NAMES と同じ。ステータスバーの
//   Wi-Fi アイコン (stackee_icons_wifi_tile) がこの名前で引く。
//
// ★ ESP-IDF に依存しない。無線は ops で外から差し込む。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stackee_wifistore.h"

#define STACKEE_WIFI_IDLE_BEFORE_CONNECT_MS 3000
#define STACKEE_WIFI_RETRY_S                60
#define STACKEE_WIFI_FIRST_DELAY_MS         2000
#define STACKEE_WIFI_UP_POLL_MS             10000
#define STACKEE_WIFI_RELINK_DELAY_MS        2000
#define STACKEE_WIFI_CONNECT_TIMEOUT_MS     20000
#define STACKEE_WIFI_CONNECT_DEFER_MAX_MS   30000

typedef enum {
    STACKEE_WIFI_S_BOOT = 0,
    STACKEE_WIFI_S_OFF,
    STACKEE_WIFI_S_LOAD,
    STACKEE_WIFI_S_WAIT,
    STACKEE_WIFI_S_RADIO,
    STACKEE_WIFI_S_SCAN_START,
    STACKEE_WIFI_S_SCAN_WAIT,
    STACKEE_WIFI_S_SCAN_READ,
    STACKEE_WIFI_S_CONNECT,
    STACKEE_WIFI_S_UP,
    STACKEE_WIFI_S_LINKUP,
    STACKEE_WIFI_S_COUNT,
} stackee_wifi_state_t;

extern const char *const stackee_wifi_state_names[STACKEE_WIFI_S_COUNT];

typedef struct {
    uint32_t (*now_ms)(void);
    // 登録簿を読む。読めなければ false (note に "missing" / "corrupt")。
    bool (*load)(stackee_wifi_list_t *out, const char **note);
    bool (*radio_on)(void);
    void (*radio_off)(void);
    bool (*scan_start)(int channel);
    bool (*scan_ready)(void);       // 走査完了の知らせが来たか (来なくてもよい)
    int  (*scan_read)(stackee_wifi_seen_t *out, int max);
    void (*scan_stop)(void);
    bool (*connect_start)(const char *ssid, const char *password, int channel);
    // 0 = 進行中 / 1 = 接続済み / 2 以上 = 切断理由
    int  (*connect_state)(void);
    bool (*link_alive)(void);
    void (*get_ip)(char *out, int cap);
    // 打鍵の谷か (押しているキーが無く、最後の打鍵から 3 秒経ったか)。
    bool (*keys_idle)(void);
    // 録音・再生の最中は無線に触らない (現行の console.wifi.scan と同じ制約)。
    bool (*audio_busy)(void);
    void (*log)(const char *line);
    // 状態名をそのまま渡す ("up" / "scan_wait" / "off" ...)。
    void (*ui)(const char *state_name);
} stackee_wifi_ops_t;

typedef struct {
    const stackee_wifi_ops_t *ops;
    int      state;
    uint32_t entered;
    uint32_t boot_ms;
    uint32_t next_at;

    stackee_wifi_list_t nets;
    bool     loaded;
    const char *note;

    int      chs[STACKEE_WIFI_CHANNELS_MAX];
    int      ch_count;
    int      idx;
    stackee_wifi_seen_t seen[STACKEE_WIFI_SEEN_MAX];
    int      seen_count;
    uint32_t scan_started;
    int      settle;

    stackee_wifi_pick_t target;     // ★ password を含む。ログ厳禁
    bool     have_target;
    uint32_t connect_started;

    char     ssid[STACKEE_WIFI_SSID_MAX];
    char     ip[16];
    int      last_channel;
    uint32_t connect_ms;
    uint32_t up_at_ms;              // 起動から接続までの時間 [ms]
    bool     up_recorded;
    uint32_t passes, connects, failures;
    bool     radio_held;
    bool     paused;
} stackee_wifi_t;

void stackee_wifi_sm_init(stackee_wifi_t *w, const stackee_wifi_ops_t *ops);

// 1 周期 1 段。呼ぶのは net タスクだけ。
void stackee_wifi_sm_step(stackee_wifi_t *w);

// console の wifi.connect。★ ここではブロックしない。
const char *stackee_wifi_sm_kick(stackee_wifi_t *w);

// console の wifi.scan と無線を取り合わないように止まる / 再開する。
// suspend の戻り値は「接続を維持しているか」。
bool stackee_wifi_sm_suspend(stackee_wifi_t *w);
void stackee_wifi_sm_resume(stackee_wifi_t *w);

const char *stackee_wifi_sm_state_name(const stackee_wifi_t *w);
bool        stackee_wifi_sm_connected(const stackee_wifi_t *w);
