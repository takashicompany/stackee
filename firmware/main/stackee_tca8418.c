#include "stackee_tca8418.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "tca";

// --- PORT.A --- CoreS3 の Grove。G1 = SCL / G2 = SDA (code.py の先頭の図)。
// 内部 I2C (G11/G12) とは別のバス。5V は AW9523B の P0_1 で入っていて、
// stackee_board_init() の aw9523b_init が P0 = 0x07 を書いた時点で来ている。
#define PORTA_SCL_GPIO 1
#define PORTA_SDA_GPIO 2
#define PORTA_I2C_PORT I2C_NUM_1
#define PORTA_I2C_HZ   400000

// --- TCA8418 (アドレス 0x34 固定) --- SCPS215G 8.6 の表そのまま。
#define TCA_ADDR         0x34
#define REG_CFG          0x01
#define REG_INT_STAT     0x02
#define REG_KEY_LCK_EC   0x03   // bit3:0 = FIFO に溜まっているイベント数
#define REG_KEY_EVENT_A  0x04   // 読むと FIFO から 1 件 pop される
#define REG_GPIO_INT_EN1 0x1A
#define REG_KP_GPIO1     0x1D   // ROW0-7 をキーパッドに割り当てる
#define REG_KP_GPIO2     0x1E   // COL0-7
#define REG_KP_GPIO3     0x1F   // COL8-9
#define REG_GPI_EM1      0x20

#define KP_GPIO1_VAL 0x1F       // ROW0..ROW4 (本基板は 5 行)
#define KP_GPIO2_VAL 0xFF       // COL0..COL7
#define KP_GPIO3_VAL 0x03       // COL8, COL9

// KE_IEN (bit0) と OVR_FLOW_IEN (bit3)。INT ピンは Grove に出ていないので
// 割り込みは使えないが、**溢れたことを INT_STAT に残させる**ために要る。
#define CFG_VAL      (0x01 | 0x08)
#define INT_K        0x01
#define INT_OVR_FLOW 0x08
#define INT_ALL      0x1F

#define FIFO_DEPTH      10      // TCA8418 の内蔵 FIFO の深さ
#define IO_FAIL_LIMIT   5       // 何回続けて I2C に失敗したら切断とみなすか
#define RETRY_INTERVAL_US 400000    // 未接続のあいだ探し直す間隔

// 配線のあるスロット (43 個)。keymap.py の WIRED_SLOTS と同じ。
// 5x10 = 50 のうち、ROW4 は COL4/COL5/COL6 の 3 つだけ (中央の親指キー)。
static bool slot_is_wired(uint8_t slot) {
    if (slot >= 50) {
        return false;
    }
    if (slot < 40) {
        return true;                    // ROW0..ROW3 は 10 列とも配線あり
    }
    uint8_t col = (uint8_t)(slot - 40);
    return col >= 4 && col <= 6;        // ROW4 は COL4/5/6 だけ
}

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool     s_ready;
static int      s_fails;
static int64_t  s_next_try_us;
static uint8_t  s_held[50];             // 押されているスロット (1 = 押下中)
static stackee_tca_stats_t s_stats;

// ---------------------------------------------------------------------------
// I2C (失敗しても例外を投げず false / -1 を返す)
// ---------------------------------------------------------------------------
static bool tca_write(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return s_dev != NULL && i2c_master_transmit(s_dev, buf, sizeof(buf), 20) == ESP_OK;
}

static int tca_read(uint8_t reg) {
    uint8_t out = 0;
    if (s_dev == NULL) {
        return -1;
    }
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &out, 1, 20) != ESP_OK) {
        return -1;
    }
    return out;
}

// ---------------------------------------------------------------------------
esp_err_t stackee_tca8418_init(void) {
    memset(&s_stats, 0, sizeof(s_stats));
    memset(s_held, 0, sizeof(s_held));
    i2c_master_bus_config_t bus_config = {
        .i2c_port = PORTA_I2C_PORT,
        .sda_io_num = PORTA_SDA_GPIO,
        .scl_io_num = PORTA_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        // ★ プルアップは基板側にしか無い (hardware/design-spec.md)。
        //   内蔵プルアップも足しておくと、基板を挿していないときに
        //   バスが浮いて I2C ドライバが固まるのを避けられる。
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PORT.A の I2C を開けない: %s", esp_err_to_name(err));
        return err;
    }
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA_ADDR,
        .scl_speed_hz = PORTA_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_config, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TCA8418 を登録できない: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = false;
    s_next_try_us = 0;
    return ESP_OK;
}

// 5x10 のキーパッドとして構成する。TCA8418 が居なければ false。
//
// TCA8418 には ID レジスタが無いので、「0x34 に居るのが本当に TCA8418 か」は
// 設定値を書いて読み返せるかで確かめるしかない (code.py と同じ)。
static bool tca_setup(void) {
    static const uint8_t setup[][2] = {
        {REG_KP_GPIO1, KP_GPIO1_VAL},
        {REG_KP_GPIO2, KP_GPIO2_VAL},
        {REG_KP_GPIO3, KP_GPIO3_VAL},
    };
    for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        if (!tca_write(setup[i][0], setup[i][1])) {
            return false;
        }
        if (tca_read(setup[i][0]) != setup[i][1]) {
            return false;
        }
    }
    // キーパッドに割り当てたピンは GPIO イベントを出さないが、念のため
    // GPIO 側のイベント経路を全部閉じておく。
    for (uint8_t i = 0; i < 3; i++) {
        if (!tca_write((uint8_t)(REG_GPIO_INT_EN1 + i), 0x00)) {
            return false;
        }
        if (!tca_write((uint8_t)(REG_GPI_EM1 + i), 0x00)) {
            return false;
        }
    }
    if (!tca_write(REG_CFG, CFG_VAL)) {
        return false;
    }
    // 電源投入から溜まったイベントを捨てる。捨てないと挿した瞬間に
    // 過去の押下が流れ込んで意図しない文字が出る。
    for (int i = 0; i < 2 * FIFO_DEPTH; i++) {
        int count = tca_read(REG_KEY_LCK_EC);
        if (count < 0) {
            return false;
        }
        if (!(count & 0x0F)) {
            break;
        }
        if (tca_read(REG_KEY_EVENT_A) < 0) {
            return false;
        }
    }
    return tca_write(REG_INT_STAT, INT_ALL);
}

// 押下中を全部「離した」として吐き出す。抜いた瞬間に押されていたキーが
// OS 側で押されたままになる (スタックキー) のを防ぐ。
static int release_all(stackee_tca_event_t *events, int max_events, int count) {
    for (uint8_t slot = 0; slot < 50 && count < max_events; slot++) {
        if (s_held[slot]) {
            s_held[slot] = 0;
            events[count].slot = slot;
            events[count].pressed = false;
            count++;
        }
    }
    return count;
}

static int lost(stackee_tca_event_t *events, int max_events, int count) {
    s_ready = false;
    s_fails = 0;
    s_stats.reconnects++;
    s_next_try_us = esp_timer_get_time() + RETRY_INTERVAL_US;
    ESP_LOGW(TAG, "TCA8418 切断 — 再接続を待つ");
    return release_all(events, max_events, count);
}

int stackee_tca8418_poll(stackee_tca_event_t *events, int max_events) {
    int count = 0;
    if (!s_ready) {
        int64_t now = esp_timer_get_time();
        if (now < s_next_try_us) {
            return 0;
        }
        s_next_try_us = now + RETRY_INTERVAL_US;
        if (tca_setup()) {
            s_ready = true;
            s_fails = 0;
            ESP_LOGI(TAG, "TCA8418 接続 (5x10 キーパッド)");
        }
        return 0;
    }

    // INT_STAT を先に読む。**FIFO が溢れたことはここでしか分からない。**
    // 件数レジスタは「今 FIFO に何件あるか」しか答えない。
    int status = tca_read(REG_INT_STAT);
    int pending = tca_read(REG_KEY_LCK_EC);
    if (status < 0 || pending < 0) {
        s_stats.io_fails++;
        if (++s_fails >= IO_FAIL_LIMIT) {
            return lost(events, max_events, count);
        }
        return count;
    }

    int n = pending & 0x0F;
    if (n > FIFO_DEPTH) {
        n = FIFO_DEPTH;
    }
    while (n-- > 0 && count < max_events) {
        int ev = tca_read(REG_KEY_EVENT_A);
        if (ev < 0) {
            s_stats.io_fails++;
            if (++s_fails >= IO_FAIL_LIMIT) {
                return lost(events, max_events, count);
            }
            return count;
        }
        if (ev == 0) {
            break;      // FIFO が空のときは 0 が返る
        }
        // bit7 = 押下(1)/離上(0)、bit6:0 = キー番号 (1..80)。
        bool    pressed = (ev & 0x80) != 0;
        uint8_t slot = (uint8_t)((ev & 0x7F) - 1);
        if (!slot_is_wired(slot)) {
            // 配線していないスロット。行/列の混線かはんだブリッジ。
            s_stats.stray++;
            ESP_LOGW(TAG, "配線の無いスロット %u からイベント", slot);
            continue;
        }
        s_held[slot] = pressed ? 1 : 0;
        events[count].slot = slot;
        events[count].pressed = pressed;
        count++;
        s_stats.events++;
    }

    if (status & INT_OVR_FLOW) {
        // 溢れて取りこぼした。捨てられたのが「離した」だとスタックキーに
        // なる。今どのキーが押されているかを読み直すレジスタは無いので、
        // 分かっているぶんを全部離して同期を取り直す。
        s_stats.overflows++;
        ESP_LOGW(TAG, "FIFO オーバーフロー — 押下中を全解放");
        count = release_all(events, max_events, count);
    }

    uint8_t ack = (uint8_t)(status & (INT_K | INT_OVR_FLOW));
    if (ack && !tca_write(REG_INT_STAT, ack)) {
        s_stats.io_fails++;
        if (++s_fails >= IO_FAIL_LIMIT) {
            return lost(events, max_events, count);
        }
        return count;
    }

    // ★ ここまで全部通ってから初めて「通信は健全」とみなす (code.py と同じ)。
    //   件数の読み出しだけでカウンタを戻すと、イベント読み出しが毎回
    //   失敗する壊れ方で永久に切断判定へ到達しない。
    s_fails = 0;
    return count;
}

void stackee_tca8418_stats(stackee_tca_stats_t *out) {
    if (out == NULL) {
        return;
    }
    *out = s_stats;
    out->connected = s_ready;
}

bool stackee_tca8418_connected(void) {
    return s_ready;
}
