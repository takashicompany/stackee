// 本体の設定メニュー — 決め事 (階層・選択・パスワードの編集・画面の中身)。
// README §17-2g / DESIGN.md §6e。
//
// ★ ESP-IDF に依存しない。ホストビルド (hostbuild/menu_main.c +
//   tools/test_menu_host.py) で実機と同じ実体を走らせる。
//
//   キー (↑↓←→ / Enter / Esc / Backspace / 文字)
//        │ stackee_menu_key()
//        ▼
//   stackee_menu_t (いまの階層・選んでいる項目・入力中のパスワード)
//        │ stackee_menu_build() ← stackee_menu_info_t (外の様子の写し)
//        ▼
//   stackee_menu_view_t (題・行・足もと。描くのは stackee_draw_menu())
//
// 実際の仕事 (Wi-Fi の切り替え・登録・送信先の切り替え・音量 …) は ops で
// 外へ頼むだけ。結果は次の info の写しに載って返ってくる (待たない)。
//
// ★ パスワードはこの構造体の中にしか無い。画面には伏せ字 (*) だけを出し、
//   ops->wifi_add() に渡したら**すぐ消す**。ログにも view にも入れない。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACKEE_MENU_ROWS_MAX    24
#define STACKEE_MENU_TEXT_MAX    64      // 1 欄の UTF-8 (全角 15 字 = 45 B + 余裕)
#define STACKEE_MENU_TITLE_MAX   48
#define STACKEE_MENU_FOOTER_MAX  64
#define STACKEE_MENU_NETS_MAX    12      // 走査で見えたネットワーク (強い順)
#define STACKEE_MENU_SAVED_MAX   8       // 登録簿の上限 (STACKEE_WIFI_MAX_NETWORKS)
#define STACKEE_MENU_SSID_MAX    33
#define STACKEE_MENU_PASS_MAX    64      // 63 文字 + NUL
#define STACKEE_MENU_DEPTH_MAX   8
// 1 画面に見える行の数 (stackee_draw_menu の割り付けと同じ)。
#define STACKEE_MENU_VISIBLE     11

typedef enum {
    STACKEE_MENU_K_NONE = 0,
    STACKEE_MENU_K_UP,
    STACKEE_MENU_K_DOWN,
    STACKEE_MENU_K_LEFT,
    STACKEE_MENU_K_RIGHT,
    STACKEE_MENU_K_ENTER,
    STACKEE_MENU_K_ESC,
    STACKEE_MENU_K_BS,
    STACKEE_MENU_K_CHAR,
} stackee_menu_key_t;

typedef enum {
    STACKEE_MENU_S_ROOT = 0,
    STACKEE_MENU_S_WIFI,
    STACKEE_MENU_S_WIFI_SAVED,      // 登録済み → Enter で接続を切り替える
    STACKEE_MENU_S_WIFI_DELETE,     // 登録を削除 → 選ぶ
    STACKEE_MENU_S_WIFI_CONFIRM,    // 本当に削除するか
    STACKEE_MENU_S_WIFI_SCAN,       // ネットワークを追加 → 走査 → 選ぶ
    STACKEE_MENU_S_WIFI_PASSWORD,   // パスワードを打つ
    STACKEE_MENU_S_WIFI_RESULT,     // 切り替え / 追加の結果
    STACKEE_MENU_S_SERVER,
    STACKEE_MENU_S_CLIPS,
    STACKEE_MENU_S_DEVICE,
    STACKEE_MENU_S_COUNT,
} stackee_menu_screen_t;

extern const char *const stackee_menu_screen_names[STACKEE_MENU_S_COUNT];

// 行の種類 (描き方が変わる)。
typedef enum {
    STACKEE_MENU_ROW_INFO = 0,      // 見出し: 値 (選べない)
    STACKEE_MENU_ROW_ITEM,          // 選べる。Enter で何か起きる
    STACKEE_MENU_ROW_SUB,           // 選べる。Enter で下の階層へ (右端に「＞」)
    STACKEE_MENU_ROW_NOTE,          // お知らせ (選べない。青)
} stackee_menu_row_kind_t;

// Enter / ← → で何をするか。
typedef enum {
    STACKEE_MENU_A_NONE = 0,
    STACKEE_MENU_A_OPEN,            // arg = 開く画面
    STACKEE_MENU_A_SWITCH,          // arg = 登録簿の番号
    STACKEE_MENU_A_DELETE_PICK,     // arg = 登録簿の番号
    STACKEE_MENU_A_DELETE_YES,
    STACKEE_MENU_A_BACK,
    STACKEE_MENU_A_SCAN_PICK,       // arg = 走査結果の番号
    STACKEE_MENU_A_SCAN_AGAIN,
    STACKEE_MENU_A_PASSWORD,        // 入力欄 (Enter で確定)
    STACKEE_MENU_A_HEALTH,
    STACKEE_MENU_A_CLIP_AUTO,
    STACKEE_MENU_A_CLIP_SYNC,
    STACKEE_MENU_A_DEST,
    STACKEE_MENU_A_VOLUME,          // ← → で ±5
} stackee_menu_action_t;

typedef struct {
    uint8_t kind;                   // stackee_menu_row_kind_t
    uint8_t action;                 // stackee_menu_action_t
    int8_t  arg;
    char    label[STACKEE_MENU_TEXT_MAX];
    char    value[STACKEE_MENU_TEXT_MAX];
} stackee_menu_row_t;

typedef struct {
    int  screen;
    char title[STACKEE_MENU_TITLE_MAX];
    char footer[STACKEE_MENU_FOOTER_MAX];
    int  count;
    int  selected;                  // 選んでいる行 (rows の番号)。-1 = 無し
    int  top;                       // いちばん上に見えている行
    stackee_menu_row_t rows[STACKEE_MENU_ROWS_MAX];
} stackee_menu_view_t;

// ---------------------------------------------------------------------------
// 外の様子の写し (glue が数百 ms ごとに埋める。★ パスワードは入れない)
// ---------------------------------------------------------------------------
typedef struct {
    char ssid[STACKEE_MENU_SSID_MAX];
    int  rssi;
    int  channel;
    bool secure;
} stackee_menu_net_t;

typedef enum {
    STACKEE_MENU_JOB_IDLE = 0,
    STACKEE_MENU_JOB_RUNNING,
    STACKEE_MENU_JOB_OK,
    STACKEE_MENU_JOB_FAILED,
} stackee_menu_job_t;

typedef struct stackee_menu_info_s {
    // ---- Wi-Fi ----
    char wifi_state[16];            // stackee_wifism の状態名
    char ssid[STACKEE_MENU_SSID_MAX];
    char ip[16];
    int  rssi;                      // 0 = 分からない
    int  saved_count;
    char saved[STACKEE_MENU_SAVED_MAX][STACKEE_MENU_SSID_MAX];
    // 切り替え・追加で狙っている SSID (stackee_wifi_sm_prefer)。
    char target[STACKEE_MENU_SSID_MAX];
    int  target_result;             // 0 なし / 1 試行中 / 2 接続した / 3 失敗
    int  target_reason;             // 失敗の理由 (esp_wifi の理由コード / -1 = 見つからない)
    // 走査 (ネットワークを追加)
    int  scan_state;                // stackee_menu_job_t
    char scan_error[24];
    int  scan_count;
    stackee_menu_net_t nets[STACKEE_MENU_NETS_MAX];
    // 登録・削除・切り替えの仕事 (メインループ)
    int  job_state;                 // stackee_menu_job_t
    char job_error[24];
    // ---- サーバー ----
    bool server_configured;
    char server_host[64];           // STACKEE_TALK_URL のホスト部 (トークンは無い)
    bool http_seen;                 // 1 回でも通信したか
    bool http_last_ok;
    int  http_last_status;
    uint32_t http_last_ago_ms;
    char inbox_phase[12];           // stackee_talk_watch_phase_names
    bool inbox_on;
    bool inbox_held;                // メニュー中は受け箱を保留している
    int  health_state;              // stackee_menu_job_t
    int  health_status;
    uint32_t health_ms;
    char health_error[32];
    // ---- クリップ ----
    bool clips_ready;
    int  clips_count;
    uint64_t clips_bytes;
    uint64_t clips_free;
    bool clips_auto;
    bool clips_forced_off;
    bool clips_synced;              // 1 回でも同期を終えたか
    uint32_t clips_last_ago_ms;
    char clips_result[12];
    char clips_phase[12];
    bool clips_sync_requested;
    // ---- 本体 ----
    char version[32];
    char profile[8];                // "dev" / "full"
    int  dest_selected;             // 0 BLE / 1 USB
    int  dest_effective;
    bool usb_mounted;
    bool ble_connected;
    bool ble_advertising;
    char ble_peer[20];              // "AA:BB:CC:DD:EE:FF" (分からなければ空)
    int  battery;                   // -1 = 読めない
    int  battery_present;           // -1 まだ分からない / 0 なし / 1 あり
    bool charging;
    bool mic_ready;
    int  volume;
} stackee_menu_info_t;

// ---------------------------------------------------------------------------
// 外への頼みごと (どれも待たない。結果は info に載る)
// ---------------------------------------------------------------------------
typedef struct {
    bool (*wifi_switch)(const char *ssid);
    // ★ password を受け取ったら写し取ってすぐ戻ること (呼び手は戻ったら消す)。
    bool (*wifi_add)(const char *ssid, const char *password, int channel);
    bool (*wifi_remove)(const char *ssid);
    bool (*wifi_scan)(void);
    void (*hid_toggle)(void);
    void (*volume_step)(int delta);
    void (*clip_auto_toggle)(void);
    void (*clip_sync)(void);
    void (*health_test)(void);
    // ルートの Esc。メニューを閉じてほしい。
    void (*close)(void);
} stackee_menu_ops_t;

typedef struct {
    uint8_t screen;
    int8_t  sel;                    // その画面で選んでいた項目 (選べる行の何番目か)
} stackee_menu_frame_t;

typedef struct {
    const stackee_menu_ops_t *ops;
    bool open;
    int  depth;                     // stack[0..depth-1] が上の階層
    stackee_menu_frame_t stack[STACKEE_MENU_DEPTH_MAX];
    int  screen;
    int  sel;                       // 選べる行の何番目か
    char notice[STACKEE_MENU_TEXT_MAX];   // 次のキーまで出すお知らせ
    // 追加・削除の途中の相手
    char pick_ssid[STACKEE_MENU_SSID_MAX];
    int  pick_channel;
    bool pick_secure;
    // ★ 入力中のパスワード。画面には伏せ字だけ。渡したら消す。
    char password[STACKEE_MENU_PASS_MAX];
    int  pass_len;
    uint32_t keys;                  // 受け取ったキーの数
    uint32_t opens;
} stackee_menu_t;

void stackee_menu_init(stackee_menu_t *m, const stackee_menu_ops_t *ops);

// 開く (いつもルートの先頭から)。閉じる (入力中のパスワードも消す)。
void stackee_menu_open(stackee_menu_t *m);
void stackee_menu_close(stackee_menu_t *m);

// キー 1 つ。view は作業用 (呼び手が持つ。いまの画面を組み立てて、選んでいる
// 行が何かを知るのに使う)。何か変わったら true。
bool stackee_menu_key(stackee_menu_t *m, const stackee_menu_info_t *info,
                      stackee_menu_view_t *view, stackee_menu_key_t key, char ch);

// いまの画面を組み立てる。同じ入力なら必ず同じ view (時刻も乱数も使わない)。
void stackee_menu_build(const stackee_menu_t *m, const stackee_menu_info_t *info,
                        stackee_menu_view_t *out);

// HID の使用番号 + 修飾 → メニューのキー。**JIS 配列**として文字を読む
// (この配列は JIS の Mac 向け。README §0-5)。使わないキーは false。
bool stackee_menu_key_from_hid(uint8_t usage, uint8_t mods,
                               stackee_menu_key_t *key, char *ch);

// console の menu.key {"k":"up"} の名前 → キー。
bool stackee_menu_key_parse(const char *name, stackee_menu_key_t *out);
const char *stackee_menu_key_name(stackee_menu_key_t key);

// 見出しに使う小道具 (テストから直に見る)。
void stackee_menu_fmt_ago(uint32_t ms, char *out, size_t cap);
void stackee_menu_fmt_bytes(uint64_t bytes, char *out, size_t cap);
int  stackee_menu_rssi_bars(int rssi);
