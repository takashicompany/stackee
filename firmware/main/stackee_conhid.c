#include "stackee_conhid.h"

#include <string.h>

// ★ quantum.h を include しない。QMK の bits.h が定義する BIT32 / BIT64 が
//   ESP-IDF の esp_bit_defs.h とぶつかるため (qmk_port.h の注意書きと同じ)。
//   要るのは次の 2 つの宣言だけなので、ここで書く。
//   via.c の側は via_command_kb を weak で持っているので、こちらの
//   強いシンボルが勝つ。
bool via_command_kb(uint8_t *data, uint8_t length);
void raw_hid_send(uint8_t *data, uint8_t length);

#define RAW_SIZE 32

// ---------------------------------------------------------------------------
// 環状バッファ (単一生産者・単一消費者)
// ---------------------------------------------------------------------------
// tx: 書くのは console タスク (送信錠の中)、読むのは入力タスク。
// rx: 書くのは入力タスク、読むのはメインループ。
// どちらも「書き手が head、読み手が tail」しか触らないのでロックが要らない。
// ★ 3 KB。内蔵 RAM の .bss を食うので大きくしない (カメラの DMA バッファが
//   内蔵の連続 30 KB を要求して失敗した件。README §17-2)。
//   status の応答が 1,800 バイトなので、1 往復ぶんには十分足りる。
#define TX_RING 3072
#define RX_RING 1024
// ホストが読んでいると見なす時間 [ms]。これを過ぎたらログは捨てる。
#define LISTEN_WINDOW_MS 3000

static uint8_t           s_tx[TX_RING];
static volatile uint32_t s_tx_head, s_tx_tail;
static volatile uint32_t s_tx_dropped;

static uint8_t           s_rx[RX_RING];
static volatile uint32_t s_rx_head, s_rx_tail;
static volatile uint32_t s_rx_dropped;

static volatile uint32_t s_reports_in, s_reports_out, s_polls;
static volatile uint32_t s_tx_overrun;
// ★ 時計は外から渡ってくる (stackee_conhid_tick)。このファイルは
//   ESP-IDF に依存しない = ホストでそのままビルドできる。
static volatile uint32_t s_now_ms;
static volatile uint32_t s_last_poll_ms;
static volatile bool     s_ever_polled;

static uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t size) {
    return (head >= tail) ? (head - tail) : (size - tail + head);
}

void stackee_conhid_tick(uint32_t now_ms) {
    s_now_ms = now_ms;
}

bool stackee_conhid_host_listening(void) {
    if (!s_ever_polled) {
        return false;
    }
    return (uint32_t)(s_now_ms - s_last_poll_ms) <= LISTEN_WINDOW_MS;
}

// 1 バイト積む。入らなければ false。
static bool push_byte(uint8_t b) {
    uint32_t next = (s_tx_head + 1) % TX_RING;
    if (next == s_tx_tail) {
        return false;
    }
    s_tx[s_tx_head] = b;
    s_tx_head = next;
    return true;
}

// 応答。**必ず入れる**。場所が無ければ古いものを捨てて空ける。
//
// ★ 古いほうを捨てる理由: ホスト側の切り分け器は次の 0x1E で必ず枠を
//   取り直すので、古いログや壊れた枠が消えても立ち直れる。逆に**新しい
//   応答**を捨てると、いま投げたコマンドの返事が永久に来ない。
void stackee_conhid_write(const void *data, size_t len) {
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        if (!push_byte(p[i])) {
            // いちばん古い 1 バイトを押し出してもう一度。
            s_tx_tail = (s_tx_tail + 1) % TX_RING;
            s_tx_overrun++;
            (void)push_byte(p[i]);
        }
    }
}

// ログ。**ホストが読んでいないときは 1 バイトも溜めない。**
void stackee_conhid_write_log(const void *data, size_t len) {
    if (!stackee_conhid_host_listening()) {
        s_tx_dropped += (uint32_t)len;
        return;
    }
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        if (!push_byte(p[i])) {
            s_tx_dropped += (uint32_t)(len - i);
            return;         // ★ ログは新しいほうを捨てる (応答を守る)
        }
    }
}

bool stackee_conhid_read(uint8_t *out) {
    if (s_rx_tail == s_rx_head) {
        return false;
    }
    *out = s_rx[s_rx_tail];
    s_rx_tail = (s_rx_tail + 1) % RX_RING;
    return true;
}

static uint32_t rx_push(const uint8_t *data, uint32_t len) {
    uint32_t took = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t next = (s_rx_head + 1) % RX_RING;
        if (next == s_rx_tail) {
            s_rx_dropped++;
            break;
        }
        s_rx[s_rx_head] = data[i];
        s_rx_head = next;
        took++;
    }
    return took;
}

void stackee_conhid_stats(stackee_conhid_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->tx_pending = ring_used(s_tx_head, s_tx_tail, TX_RING);
    out->tx_dropped = s_tx_dropped;
    out->tx_overrun = s_tx_overrun;
    out->rx_dropped = s_rx_dropped;
    out->reports_in = s_reports_in;
    out->reports_out = s_reports_out;
    out->polls = s_polls;
    out->listening = stackee_conhid_host_listening();
}

void stackee_conhid_init(void) {
    s_tx_head = s_tx_tail = 0;
    s_rx_head = s_rx_tail = 0;
    s_ever_polled = false;
}

// ---------------------------------------------------------------------------
// VIA の独自 command id
// ---------------------------------------------------------------------------
// ★ ここは**入力タスク**から呼ばれる。ブロックしないこと。
//   raw_hid_send() は送信キューへ積むだけなので待たない。
bool via_command_kb(uint8_t *data, uint8_t length) {
    if (length < 3) {
        return false;
    }
    uint8_t id = data[0];
    if (id != STACKEE_CONHID_CMD_TX && id != STACKEE_CONHID_CMD_RX &&
        id != STACKEE_CONHID_CMD_INFO) {
        return false;       // VIA の本来のコマンド。via.c に任せる
    }
    s_reports_in++;

    uint8_t reply[RAW_SIZE];
    memset(reply, 0, sizeof(reply));
    reply[0] = id;

    if (id == STACKEE_CONHID_CMD_TX) {
        uint8_t len = data[1];
        if (len > STACKEE_CONHID_PAYLOAD) {
            len = STACKEE_CONHID_PAYLOAD;
        }
        if (len > (uint8_t)(length - 3)) {
            len = (uint8_t)(length - 3);
        }
        uint32_t took = rx_push(&data[3], len);
        reply[1] = (uint8_t)took;
        reply[2] = (took == len) ? 0x00 : 0x01;     // bit0 = 入り切らなかった
    } else if (id == STACKEE_CONHID_CMD_RX) {
        // ★ ここでホストが「読んでいる」と分かる。以後 3 秒はログも溜める。
        s_last_poll_ms = s_now_ms;
        s_ever_polled = true;
        s_polls++;
        uint8_t n = 0;
        while (n < STACKEE_CONHID_PAYLOAD && s_tx_tail != s_tx_head) {
            reply[3 + n] = s_tx[s_tx_tail];
            s_tx_tail = (s_tx_tail + 1) % TX_RING;
            n++;
        }
        reply[1] = n;
        reply[2] = (s_tx_tail != s_tx_head) ? 0x01 : 0x00;  // bit0 = 続きあり
    } else {
        uint32_t pending = ring_used(s_tx_head, s_tx_tail, TX_RING);
        reply[1] = STACKEE_CONHID_PROTO;
        reply[2] = 0;
        reply[3] = (uint8_t)(pending & 0xFF);
        reply[4] = (uint8_t)((pending >> 8) & 0xFF);
        reply[5] = (uint8_t)((pending >> 16) & 0xFF);
        reply[6] = (uint8_t)((pending >> 24) & 0xFF);
        uint32_t dropped = s_tx_dropped;
        reply[7] = (uint8_t)(dropped & 0xFF);
        reply[8] = (uint8_t)((dropped >> 8) & 0xFF);
        reply[9] = (uint8_t)((dropped >> 16) & 0xFF);
        reply[10] = (uint8_t)((dropped >> 24) & 0xFF);
    }
    raw_hid_send(reply, RAW_SIZE);
    s_reports_out++;
    return true;            // via.c には渡さない (応答も済ませた)
}
