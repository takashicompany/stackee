// 送信キュー。DESIGN.md §3 の input -> hid_out のあいだ。
//
// 書くのは input タスク (CPU1・最高優先度) 1 本だけ、読むのは hid_out タスク
// 1 本だけ。単一生産者・単一消費者なのでロックが要らない — input タスクが
// USB の都合で待たされることが構造的に無い、というのがこの設計の目的
// (現行 CircuitPython 版は送信が詰まるとスキャンごと止まっていた)。
//
// 溢れたら **古いものではなく新しいものを捨てて数える**。押しっぱなしの
// 解除 (キーが離れたレポート) を捨てるとスタックキーになるが、それは
// 溢れているという異常のほうを直すべきで、ここで順番を崩さない。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    STACKEE_REPORT_KEYBOARD = 0,
    STACKEE_REPORT_MOUSE,
    STACKEE_REPORT_SYSTEM,
    STACKEE_REPORT_CONSUMER,
    STACKEE_REPORT_RAW,         // Raw HID (VIA の応答)
    STACKEE_REPORT_KINDS,
} stackee_report_kind_t;

#define STACKEE_REPORT_MAX_LEN 32
#define STACKEE_REPORT_QUEUE_LEN 32

typedef struct {
    uint8_t  kind;
    uint8_t  len;
    uint64_t key_time_us;       // このレポートの元になったキーを読んだ時刻
    uint8_t  data[STACKEE_REPORT_MAX_LEN];
} stackee_report_t;

void stackee_report_queue_init(void);

// input タスクから。入らなければ false (捨てた数は stats に出る)。
bool stackee_report_queue_push(stackee_report_kind_t kind,
                               const void *data, size_t len);

// hid_out タスクから。取れなければ false。
bool stackee_report_queue_pop(stackee_report_t *out);

// 次に出るレポートの種類だけを見る (取り出さない)。送信先の口が空いて
// いるかを、その種類に合わせて確かめてから pop するために要る。
bool stackee_report_queue_peek_kind(uint8_t *kind);

// いま作っているレポートの「元になったキーを読んだ時刻」。input タスクが
// keyboard_task を呼ぶ直前に置く。0 なら計測しない。
void stackee_report_queue_set_key_time(uint64_t us);

typedef struct {
    uint32_t pushed;
    uint32_t dropped;
    uint32_t sent_usb;
    uint32_t sent_ble;          // 段階 1 では「捨てた数」(BLE は未実装)
    uint32_t send_failed;
    uint8_t  depth;
} stackee_report_stats_t;

void stackee_report_queue_stats(stackee_report_stats_t *out);
void stackee_report_queue_count_sent(bool usb, bool ok);
