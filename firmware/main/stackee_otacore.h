// アプリ内 OTA の中核。**ESP-IDF に依存しない。**
//
// 本体が動いたまま Raw HID で像を受け取り、次の更新区画 (ota_1) へ書く。
// ROM の書き込みモードには一切入らないので、途中で切れてもキーボードは
// 生きたままで、otadata を書き換えるまでは古い像で起動し続ける。
// 方式の選定は research/stackee/web_flash_2026-09-20.md §2(B)。
//
// ここに置いてあるのは「ESP-IDF を呼ばずに書ける部分」だけ:
//   ・0xC3 レポートの分解 (先頭バイトで JSON の 0xC0/0xC1 と区別する)
//   ・受信の環状バッファ (単一生産者・単一消費者)
//   ・credit (ホストがどこまで先行して送ってよいか) の材料
//   ・SHA-256 のストリーミング計算と照合
//   ・状態機械 (begin → data → end → commit / abort)
// フラッシュと SHA-256 の実体は **外から差し込む** (下の 2 つの vtable)。
// 実機では stackee_ota.c が esp_ota_* と mbedtls を渡し、ホストでは
// hostbuild/ota_main.c が偽のフラッシュと自前の SHA-256 を渡す。
//
// ★ 誰がどこから呼ぶか (これを崩すと壊れる)
//   feed()  … **入力タスク** (CPU1・1 ms 周期)。環状バッファに積むだけ。
//             フラッシュには触らない。触ると打鍵が数十 ms 止まる。
//   pump()  … **メインループ**。環状バッファから取り出してフラッシュへ書く。
//             ここは止まってよい (メインループはコンソールしか見ていない)。
//   begin() / end() / abort() … メインループ (コンソール命令) から。
//
// ★ フラッシュの消去・書き込み中はキャッシュが止まり、IRAM 非常駐の割り込み
//   (TinyUSB を含む) が数十 ms 止まる。だからホスト側は「1 枠ごとの ack」に
//   してはいけない。本体は「書き終えた累積バイト数」を返し、ホストは
//   そこから STACKEE_OTA_CREDIT バイト先まで送ってよい、という約束にする。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- Raw HID の枠 -----------------------------------------------------------
// 既存のコンソール (0xC0 送信 / 0xC1 受信 / 0xC2 情報) と衝突しない番号。
// VIA の本来のコマンドは 0x00〜0x0F なので、こちらも当たらない。
#define STACKEE_OTA_CMD_DATA    0xC3

//   byte 0      0xC3
//   byte 1      len      (payload の有効バイト数、0..27。0 は「状態だけ返せ」)
//   byte 2..4   offset   (像の先頭からの位置。24 bit little endian = 16 MB まで)
//   byte 5..31  payload  (27 バイト)
//
// ★ **位置を毎枠に書く**。1 バイトの通し番号だと credit 窓 (32 KB = 1200 枠)
//   の中で一周してしまい、取りこぼしたときにどこから送り直せばよいか
//   決められない。位置を持たせると、本体は「期待する位置と違えば捨てる」
//   だけでよく、ホストは「本体が言う accepted から送り直す」だけでよい。
//   代償は本文 29 → 27 バイト (理論値 29 → 27 KB/s、1.37 MB で 47 → 51 秒)。
#define STACKEE_OTA_HEADER      5
#define STACKEE_OTA_PAYLOAD     27
#define STACKEE_OTA_REPORT      32

// 応答 (デバイス → ホスト、32 バイト):
//   byte 0        0xC3
//   byte 1        state   (stackee_ota_state_t)
//   byte 2        err     (stackee_ota_err_t)
//   byte 3..6     accepted 環状バッファに入れた累積バイト数 = 次に送るべき位置
//   byte 7..10    written  esp_ota_write に渡し終えた累積バイト数 (credit の起点)
//   byte 11..14   size     ota.begin で申告された像の大きさ
//   byte 15       flags   (bit0 = この枠は受け取らなかった)
//   byte 16..19   free     環状バッファの空き
#define STACKEE_OTA_REPLY_REJECTED  0x01

// 受け取った累積がこのバイト数を跨ぐたびに 1 枚だけ応答を返す。
// ★ 毎枠返すと Raw HID の送信キュー (32 枚) を OTA だけで埋めてしまい、
//   同時に打鍵した VIA の応答やキーのレポートを押し出す。
#define STACKEE_OTA_ACK_EVERY   1024

// ホストが「書き終えた位置」からどれだけ先行して送ってよいか。
#define STACKEE_OTA_CREDIT      (32u * 1024u)

// 環状バッファの最小の大きさ。credit の 2 倍。
#define STACKEE_OTA_RING_MIN    (64u * 1024u)

// 無通信でこれだけ経ったら勝手に abort する。タブを閉じられても次を始められる。
#define STACKEE_OTA_IDLE_MS     30000u

typedef enum {
    STACKEE_OTA_IDLE      = 0,
    STACKEE_OTA_RECEIVING = 1,
    STACKEE_OTA_DONE      = 2,      // ota.end まで通った (まだ切り替えていない)
    STACKEE_OTA_FAILED    = 3,
} stackee_ota_state_t;

typedef enum {
    STACKEE_OTA_ERR_NONE = 0,
    STACKEE_OTA_ERR_BUSY,           // 既に進行中 (ハンドルは 1 つ)
    STACKEE_OTA_ERR_ARG,            // size / sha256 が無い、または大きすぎる
    STACKEE_OTA_ERR_NOMEM,          // 環状バッファが用意できない
    STACKEE_OTA_ERR_FLASH_BEGIN,    // esp_ota_begin が失敗
    STACKEE_OTA_ERR_OFFSET,         // 期待する位置と違う枠が来た
    STACKEE_OTA_ERR_FULL,           // 環状バッファが溢れた (credit 違反)
    STACKEE_OTA_ERR_FLASH_WRITE,    // esp_ota_write が失敗
    STACKEE_OTA_ERR_FLASH_END,      // esp_ota_end が失敗
    STACKEE_OTA_ERR_SHA,            // 計算した SHA-256 が申告と違う
    STACKEE_OTA_ERR_SIZE,           // 受け取った長さが申告と違う
    STACKEE_OTA_ERR_IDLE,           // 無通信で自動 abort
    STACKEE_OTA_ERR_STATE,          // 受信中でないのにデータが来た
    STACKEE_OTA_ERR_MAGIC,          // 先頭が 0xE9 でない (ESP の像ではない)
    STACKEE_OTA_ERR_COUNT,
} stackee_ota_err_t;

const char *stackee_otacore_err_name(int err);
const char *stackee_otacore_state_name(int state);

// --- 差し込む実体 -----------------------------------------------------------
// 戻り値は 0 で成功、それ以外は失敗 (実機では esp_err_t をそのまま入れる)。
typedef struct {
    int  (*begin)(void *ctx, uint32_t size);
    int  (*write)(void *ctx, const void *data, uint32_t len);
    int  (*end)(void *ctx);
    void (*cancel)(void *ctx);
} stackee_ota_flash_ops_t;

typedef struct {
    void (*init)(void *ctx);
    void (*update)(void *ctx, const void *data, uint32_t len);
    void (*finish)(void *ctx, uint8_t out[32]);
} stackee_ota_hash_ops_t;

void stackee_otacore_attach(const stackee_ota_flash_ops_t *flash, void *flash_ctx,
                            const stackee_ota_hash_ops_t *hash, void *hash_ctx);

// --- 状態 -------------------------------------------------------------------
typedef struct {
    uint8_t  state;
    uint8_t  err;
    uint32_t size;          // 申告された像の大きさ
    uint32_t accepted;      // 環状バッファに入れた累積
    uint32_t written;       // フラッシュへ渡し終えた累積
    uint32_t pending;       // 環状バッファに残っている
    uint32_t ring_size;
    uint32_t rejected;      // 位置違いで捨てた枠の数
    uint32_t overflow;      // 溢れて捨てた枠の数
    uint32_t elapsed_ms;    // begin からの経過
    uint8_t  want_sha[32];
    uint8_t  got_sha[32];
    bool     have_sha;      // got_sha が埋まっているか (ota.end のあと)
} stackee_ota_status_t;

void stackee_otacore_status(stackee_ota_status_t *out);
int  stackee_otacore_state(void);

// --- 手順 -------------------------------------------------------------------
// ring は呼び手が用意する (実機では PSRAM。内蔵 RAM を増やさないため)。
// 大きさは STACKEE_OTA_RING_MIN 以上。0 なら STACKEE_OTA_ERR_NOMEM。
// bounce は **内蔵 RAM** の作業用 (ESP-IDF の esp_flash_write は外部 RAM の
// 元バッファを 32 バイトずつしか扱えず、その都度キャッシュを落とすので
// 桁違いに遅くなる)。4 KB にしておくと 1 回の書き込みが 1 セクタの消去
// ちょうどに揃う。
int  stackee_otacore_begin(uint32_t size, const uint8_t sha256[32],
                           uint8_t *ring, uint32_t ring_size,
                           uint8_t *bounce, uint32_t bounce_len,
                           uint32_t now_ms);

// 入力タスクから。OTA の枠なら true を返し、reply に 32 バイトを組み立てる
// (返す必要があるときだけ *want_reply を true にする)。
// OTA の枠でなければ false (呼び手は VIA / コンソールへ回す)。
bool stackee_otacore_feed(const uint8_t *report, uint32_t report_len,
                          uint8_t *reply, bool *want_reply);

// メインループから。環状バッファから最大 budget バイトをフラッシュへ移す。
// 戻り値は書いたバイト数。
uint32_t stackee_otacore_pump(uint32_t budget);

// 時計を進める (無通信の見張り)。メインループから毎周。
void stackee_otacore_tick(uint32_t now_ms);

// 残りを全部書いてから esp_ota_end。成功したら状態は DONE。
int  stackee_otacore_end(void);

void stackee_otacore_abort(void);

// 環状バッファを忘れる (実機では確保し直さないので、試験からの掃除用)。
void stackee_otacore_reset(void);

// 受信していないときだけ、預かっていた内蔵 RAM の作業バッファを手放す。
// ★ 返してもらってから free すること。受信中に free すると、メインループが
//   書いている最中の領域を返すことになる。
bool stackee_otacore_take_bounce(uint8_t **out);
