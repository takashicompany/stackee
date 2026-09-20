#include "stackee_report_queue.h"

#include <string.h>

static stackee_report_t  s_ring[STACKEE_REPORT_QUEUE_LEN];
static volatile uint32_t s_head;        // 書く側 (input) だけが進める
static volatile uint32_t s_tail;        // 読む側 (hid_out) だけが進める
static uint64_t          s_key_time_us;

static volatile uint32_t s_pushed;
static volatile uint32_t s_dropped;
static volatile uint32_t s_sent_usb;
static volatile uint32_t s_sent_ble;
static volatile uint32_t s_send_failed;

void stackee_report_queue_init(void) {
    s_head = 0;
    s_tail = 0;
    s_key_time_us = 0;
    s_pushed = 0;
    s_dropped = 0;
    s_sent_usb = 0;
    s_sent_ble = 0;
    s_send_failed = 0;
}

void stackee_report_queue_set_key_time(uint64_t us) {
    s_key_time_us = us;
}

bool stackee_report_queue_push(stackee_report_kind_t kind,
                               const void *data, size_t len) {
    if (len > STACKEE_REPORT_MAX_LEN) {
        len = STACKEE_REPORT_MAX_LEN;
    }
    uint32_t head = s_head;
    uint32_t next = (head + 1) % STACKEE_REPORT_QUEUE_LEN;
    if (next == s_tail) {
        s_dropped++;
        return false;
    }
    stackee_report_t *slot = &s_ring[head];
    slot->kind = (uint8_t)kind;
    slot->len = (uint8_t)len;
    slot->key_time_us = s_key_time_us;
    memset(slot->data, 0, sizeof(slot->data));
    if (data != NULL && len > 0) {
        memcpy(slot->data, data, len);
    }
    s_head = next;
    s_pushed++;
    return true;
}

bool stackee_report_queue_pop(stackee_report_t *out) {
    uint32_t tail = s_tail;
    if (tail == s_head) {
        return false;
    }
    if (out != NULL) {
        *out = s_ring[tail];
    }
    s_tail = (tail + 1) % STACKEE_REPORT_QUEUE_LEN;
    return true;
}

bool stackee_report_queue_peek_kind(uint8_t *kind) {
    uint32_t tail = s_tail;
    if (tail == s_head) {
        return false;
    }
    if (kind != NULL) {
        *kind = s_ring[tail].kind;
    }
    return true;
}

void stackee_report_queue_count_sent(bool usb, bool ok) {
    if (!ok) {
        s_send_failed++;
        return;
    }
    if (usb) {
        s_sent_usb++;
    } else {
        s_sent_ble++;
    }
}

void stackee_report_queue_stats(stackee_report_stats_t *out) {
    if (out == NULL) {
        return;
    }
    uint32_t head = s_head;
    uint32_t tail = s_tail;
    out->pushed = s_pushed;
    out->dropped = s_dropped;
    out->sent_usb = s_sent_usb;
    out->sent_ble = s_sent_ble;
    out->send_failed = s_send_failed;
    out->depth = (uint8_t)((head + STACKEE_REPORT_QUEUE_LEN - tail)
                           % STACKEE_REPORT_QUEUE_LEN);
}
