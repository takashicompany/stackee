// Raw HID の上のコンソール (full プロファイル用)。DESIGN.md §4。
//
// full プロファイルには CDC が無い (IN エンドポイントが 5 本しかなく、
// UAC マイクに 1 本使うため)。そこでコンソールとログを **VIA の Raw HID**
// に相乗りさせる。QMK の via.c は知らない command id を via_command_kb() に
// 回すので、そこを乗っ取る。VIA が使う 0x00〜0x0F とは衝突しない 0xC0〜。
//
//   32 バイトのレポートは必ずこの形:
//     byte 0     command id
//     byte 1     len    (payload の有効バイト数、0..29)
//     byte 2     flags  (bit0 = まだ続きがある)
//     byte 3..31 payload (29 バイト。余りは 0)
//
//   0xC0  ホスト → デバイス。payload は「CDC へ送るのと同じバイト列」。
//         応答は byte1 に受け取ったバイト数。
//   0xC1  ホスト → デバイス (受信ポーリング)。応答の payload は
//         「CDC から流れてくるのと同じバイト列」= 応答枠 (0x1E ... \n) と
//         ログが混ざったもの。**ホスト側は CDC のときと同じ切り分け器を
//         そのまま使える**。
//   0xC2  情報。byte1 = このプロトコルの版、byte3.. = 送信待ちバイト数と
//         取りこぼしたバイト数 (どちらも little endian の uint32)。
//
// ★ via_command_kb() が呼ばれるのは**入力タスク** (CPU1・最高優先度)。
//   ここでコマンドを処理すると、camera.capture のような数秒かかるものが
//   キーボードを止める。だからここでは受信を溜めるだけで、実際の処理は
//   メインループ (stackee_console_poll) が行う。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACKEE_CONHID_PROTO    1
#define STACKEE_CONHID_PAYLOAD  29

// コマンド id。
#define STACKEE_CONHID_CMD_TX   0xC0
#define STACKEE_CONHID_CMD_RX   0xC1
#define STACKEE_CONHID_CMD_INFO 0xC2

void stackee_conhid_init(void);

// デバイス → ホストへ流すバイト列を積む。
// ★ 書き手は 1 本に絞ること (console の送信錠の中から呼ぶ)。
//
// ★★ **応答とログを分ける**。理由 (2026-09-16 実機で踏んだ):
//   ログを無条件に溜めると、ホストが Raw HID を 1 度も読んでいない間に
//   環状バッファが起動ログで満杯になる。そのあと hello を投げても、
//   応答は「入り切らない」で捨てられ、**コンソールが永久に黙る**
//   (実測: tx_pending 6143 / tx_dropped 299 で hello が返らなかった)。
//
//   write()     … 応答。**必ず入れる**。場所が無ければ古いものを捨てて空ける
//   write_log() … ログ。**ホストが最近 0xC1 を撃っていなければ捨てる**
void stackee_conhid_write(const void *data, size_t len);
void stackee_conhid_write_log(const void *data, size_t len);

// ★ 時計は外から渡す (このファイルを ESP-IDF に依存させないため。
//   hostbuild/conhid_main.c がそのままビルドできる)。メインループから毎周。
void stackee_conhid_tick(uint32_t now_ms);

// ホストが最近 (3 秒以内に) 0xC1 を撃ったか。
bool stackee_conhid_host_listening(void);

// ホストから届いたバイト列を 1 バイトずつ取り出す。無ければ false。
// メインループ (stackee_console_poll) から呼ぶ。
bool stackee_conhid_read(uint8_t *out);

typedef struct {
    uint32_t tx_pending;    // ホストへ渡していないバイト数
    uint32_t tx_dropped;    // 入り切らずに捨てたバイト数 (ログ)
    uint32_t tx_overrun;    // 応答を入れるために古いものを押し出した回数
    uint32_t rx_dropped;
    uint32_t reports_in;    // 受け取った 0xC0/0xC1/0xC2 のレポート数
    uint32_t reports_out;
    uint32_t polls;         // 0xC1 を受けた回数
    bool     listening;     // ホストが最近読んでいるか
} stackee_conhid_stats_t;

void stackee_conhid_stats(stackee_conhid_stats_t *out);

// ---------------------------------------------------------------------------
// 別の command id を横取りする口 (アプリ内 OTA の 0xC3)
// ---------------------------------------------------------------------------
// ★ このファイルを ESP-IDF からも OTA からも切り離しておくための関数ポインタ。
//   登録が無ければ何も変わらない (hostbuild/conhid_main.c はそのまま)。
//   呼ばれるのは **入力タスク**。中でフラッシュに触らないこと。
//
//   戻り値 true  … この 32 バイトは横取りした (VIA には渡さない)。
//                   *want_reply が true なら reply の 32 バイトを送り返す。
//   戻り値 false … 知らない id。conhid の本来の処理へ進む。
typedef bool (*stackee_conhid_raw_hook_t)(const uint8_t *report, uint32_t len,
                                          uint8_t *reply, bool *want_reply);

void stackee_conhid_set_raw_hook(stackee_conhid_raw_hook_t hook);
