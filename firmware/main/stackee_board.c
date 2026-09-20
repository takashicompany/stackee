#include "stackee_board.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "board";

#define I2C_SDA_GPIO        12
#define I2C_SCL_GPIO        11
#define I2C_FREQ_HZ         100000      // CircuitPython の board I2C と同じ

#define AXP2101_ADDR        0x34
#define AW9523B_ADDR        0x58

#define AXP_REG_STATUS2         0x01    // bit6:5 電池電流の向き
#define AXP_REG_CHG_GAUGE_WDT   0x18    // bit3 ゲージ有効
#define AXP_REG_BAT_PERCENT     0xA4    // 残量 [%]
#define AXP_REG_ADC_EN          0x30    // ADC channel enable (bit0 = VBAT)
#define AXP_ADC_VBAT_EN         0x01
#define AXP_REG_VBAT_H          0x34    // VBAT [13:8]
#define AXP_REG_VBAT_L          0x35    // VBAT [7:0]
#define AXP_GAUGE_EN            0x08

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_axp;
static i2c_master_dev_handle_t s_aw;
static bool s_ready;
static SemaphoreHandle_t s_lock;

static bool take(void) {
    return s_lock != NULL && xSemaphoreTake(s_lock, pdMS_TO_TICKS(200)) == pdTRUE;
}

static void give(void) {
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

// ★ I2C の 1 往復。失敗したらバスをリセット (SCL を 9 回叩いて、途中で
//   止まった従属側に SDA を放させる) して 1 回だけやり直す。
//   2026-09-16: 撮影→reset→撮影で AXP2101 への書き込みが 1 回失敗した
//   (再起動の瞬間に I2C の途中だった可能性)。回数は status の i2c に出る。
static uint32_t s_i2c_fail, s_i2c_recovered;

static esp_err_t xfer_w(i2c_master_dev_handle_t dev, const uint8_t *buf, size_t len) {
    esp_err_t err = i2c_master_transmit(dev, buf, len, 100);
    if (err != ESP_OK) {
        s_i2c_fail++;
        ESP_LOGW(TAG, "I2C 送信失敗 %s → バスをリセットしてやり直す", esp_err_to_name(err));
        i2c_master_bus_reset(s_bus);
        err = i2c_master_transmit(dev, buf, len, 100);
        if (err == ESP_OK) { s_i2c_recovered++; }
    }
    return err;
}

static esp_err_t xfer_wr(i2c_master_dev_handle_t dev, const uint8_t *w, size_t wlen,
                         uint8_t *r, size_t rlen) {
    esp_err_t err = i2c_master_transmit_receive(dev, w, wlen, r, rlen, 100);
    if (err != ESP_OK) {
        s_i2c_fail++;
        ESP_LOGW(TAG, "I2C 送受信失敗 %s → バスをリセットしてやり直す", esp_err_to_name(err));
        i2c_master_bus_reset(s_bus);
        err = i2c_master_transmit_receive(dev, w, wlen, r, rlen, 100);
        if (err == ESP_OK) { s_i2c_recovered++; }
    }
    return err;
}

void stackee_board_i2c_stats(uint32_t *fail, uint32_t *recovered) {
    if (fail != NULL) { *fail = s_i2c_fail; }
    if (recovered != NULL) { *recovered = s_i2c_recovered; }
}

esp_err_t stackee_board_i2c_add(uint8_t address, i2c_master_dev_handle_t *out) {
    if (s_bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &config, out);
}

esp_err_t stackee_board_i2c_write(i2c_master_dev_handle_t dev,
                                  const uint8_t *buf, size_t len) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = xfer_w(dev, buf, len);
    give();
    return err;
}

esp_err_t stackee_board_i2c_write_read(i2c_master_dev_handle_t dev,
                                       const uint8_t *w, size_t wlen,
                                       uint8_t *r, size_t rlen) {
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take()) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = xfer_wr(dev, w, wlen, r, rlen);
    give();
    return err;
}

static esp_err_t write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return stackee_board_i2c_write(dev, buf, sizeof(buf));
}

static int read_reg(i2c_master_dev_handle_t dev, uint8_t reg) {
    uint8_t out = 0;
    if (dev == NULL) {
        return -1;
    }
    if (stackee_board_i2c_write_read(dev, &reg, 1, &out, 1) != ESP_OK) {
        return -1;
    }
    return out;
}

// AW9523B の P0 の 1 ビット。read-modify-write なので錠の中でまとめてやる。
int stackee_board_aw9523_p0(uint8_t mask, bool on) {
    if (!s_ready || s_aw == NULL || !take()) {
        return -1;
    }
    int result = -1;
    uint8_t reg = 0x02;
    uint8_t value = 0;
    if (xfer_wr(s_aw, &reg, 1, &value, 1) == ESP_OK) {
        result = (value & mask) ? 1 : 0;
        uint8_t want = on ? (uint8_t)(value | mask) : (uint8_t)(value & ~mask);
        if (want != value) {
            uint8_t buf[2] = {reg, want};
            if (xfer_w(s_aw, buf, sizeof(buf)) != ESP_OK) {
                result = -1;
            }
        }
    }
    give();
    return result;
}

// board.c:91-166 と同じ順・同じ値。
static esp_err_t axp2101_init(void) {
    static const uint8_t seq[][2] = {
        {0x90, 0xBF},   // LDOS ON/OFF control 0
        {0x92, 0x0D},   // ALDO1 = 1.8V (AW88298)
        {0x93, 0x1C},   // ALDO2 = 3.3V (ES7210)
        {0x94, 0x1C},   // ALDO3 = 3.3V (カメラ)
        {0x95, 0x1C},   // ALDO4 = 3.3V (TF カード)
        {0x99, 0x18},   // DLDO1 = 2.9V (TFT バックライト)
        {0x27, 0x00},   // PowerKey Hold=1s / PowerOff=4s
        {0x69, 0x11},   // CHGLED
        {0x10, 0x30},   // PMU common config
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        esp_err_t err = write_reg(s_axp, seq[i][0], seq[i][1]);
        if (err != ESP_OK) {
            return err;
        }
    }
    // 残量ゲージが止まっていたら起こす (stackee_ui.Axp2101.enable_gauge と同じ)。
    int v = read_reg(s_axp, AXP_REG_CHG_GAUGE_WDT);
    if (v >= 0 && !(v & AXP_GAUGE_EN)) {
        write_reg(s_axp, AXP_REG_CHG_GAUGE_WDT, (uint8_t)(v | AXP_GAUGE_EN));
    }
    // ★ REG 0x22 (PWROFF_EN) は**書かない**。2026-09-17 に bit1 を書いたあと
    //   再起動で電源が落ちた (README §19)。長押し OFF は stackee_ui の
    //   IRQ 0x49 監視 + REG 0x10 bit0 で行う。
    // ★ 電池電圧の ADC (REG 0x30 bit0 = VBAT)。2026-09-17 に「無線で残量 % が
    //   減らない」と言われ、% と電圧を並べて記録するために入れた。
    //   XPowersLib の enableBattVoltageMeasure() と同じビット。
    v = read_reg(s_axp, AXP_REG_ADC_EN);
    if (v >= 0 && !(v & AXP_ADC_VBAT_EN)) {
        write_reg(s_axp, AXP_REG_ADC_EN, (uint8_t)(v | AXP_ADC_VBAT_EN));
    }
    return ESP_OK;
}

// ★ 電源を切る (AXP2101 REG 0x10 bit0 = Soft PWROFF)。全レールが落ち、
//   戻すには電源ボタン。bit1 (Restart) は同時に立てない。
//   呼び手は stackee_ui の長押し検出だけ。書いた瞬間に応答が無くなる。
void stackee_board_power_off(void) {
    int v = read_reg(s_axp, 0x10);
    if (v < 0) {
        v = 0x30;   // 初期化列で書いている既定値
    }
    ESP_LOGW(TAG, "電源を切る (REG 0x10 <- 0x%02X)", (unsigned)((v & ~0x02) | 0x01));
    write_reg(s_axp, 0x10, (uint8_t)((v & ~0x02) | 0x01));
}

int stackee_board_axp_write(uint8_t reg, uint8_t value) {
    if (!s_ready) {
        return -1;
    }
    return write_reg(s_axp, reg, value) == ESP_OK ? 0 : -1;
}

int stackee_board_axp_read(uint8_t reg) {
    if (!s_ready) {
        return -1;
    }
    return read_reg(s_axp, reg);
}

// VBAT = REG 0x34 の下位 6 bit を上位、0x35 を下位にした 14 bit。1 mV/LSB
// (XPowersLib getBattVoltage と同じ読み方)。
int stackee_board_battery_mv(void) {
    if (!s_ready) {
        return -1;
    }
    int hi = read_reg(s_axp, AXP_REG_VBAT_H);
    int lo = read_reg(s_axp, AXP_REG_VBAT_L);
    if (hi < 0 || lo < 0) {
        return -1;
    }
    return ((hi & 0x1F) << 8) | lo;     // M5Unified readRegister14 / XPowersLib H5L8 と同じ 13 bit
}

// board.c:168-217 と同じ。0x03 の LCD_RST がここで解除される。
static esp_err_t aw9523b_init(void) {
    static const uint8_t seq[][2] = {
        {0x02, 0x07},   // AW_RST, BUD_OUT_EN, TOUCH_RST
        {0x03, 0x83},   // BOOST_EN, CAM_RST, LCD_RST
        {0x04, 0x18},   // TF_SW, ES_INT を入力に
        {0x05, 0x0C},   // AW_INT, TOUCH_INT を入力に
        {0x11, 0x10},   // P0 を push-pull に
    };
    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        esp_err_t err = write_reg(s_aw, seq[i][0], seq[i][1]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t stackee_board_init(void) {
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "内部 I2C を開けない: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    dev_config.device_address = AXP2101_ADDR;
    err = i2c_master_bus_add_device(s_bus, &dev_config, &s_axp);
    if (err != ESP_OK) {
        return err;
    }
    dev_config.device_address = AW9523B_ADDR;
    err = i2c_master_bus_add_device(s_bus, &dev_config, &s_aw);
    if (err != ESP_OK) {
        return err;
    }

    err = axp2101_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 の初期化に失敗: %s", esp_err_to_name(err));
        return err;
    }
    err = aw9523b_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW9523B の初期化に失敗: %s", esp_err_to_name(err));
        return err;
    }
    // AW9523B が LCD_RST を離してからパネルに話しかけるまでの待ち。
    vTaskDelay(pdMS_TO_TICKS(20));
    s_ready = true;
    ESP_LOGI(TAG, "AXP2101 / AW9523B 初期化ずみ (残量 %d%%)",
             stackee_board_battery_percent());
    return ESP_OK;
}

int stackee_board_axp_bit(uint8_t reg, uint8_t mask, int on) {
    if (!s_ready || s_axp == NULL || !take()) {
        return -1;
    }
    int result = -1;
    uint8_t value = 0;
    if (xfer_wr(s_axp, &reg, 1, &value, 1) == ESP_OK) {
        result = value;
        if (on >= 0) {
            uint8_t want = on ? (uint8_t)(value | mask) : (uint8_t)(value & ~mask);
            if (want != value) {
                uint8_t buf[2] = {reg, want};
                if (xfer_w(s_axp, buf, sizeof(buf)) != ESP_OK) {
                    result = -1;
                } else if (xfer_wr(s_axp, &reg, 1, &value, 1) == ESP_OK) {
                    result = value;     // 書けたことを読み直して確かめる
                } else {
                    result = -1;
                }
            }
        }
    }
    give();
    return result;
}

int stackee_board_battery_percent(void) {
    if (!s_ready) {
        return -1;
    }
    int v = read_reg(s_axp, AXP_REG_BAT_PERCENT);
    if (v < 0 || v > 100) {
        return -1;      // ゲージ未学習。0..100 を外れたら捨てる
    }
    return v;
}

int stackee_board_charging(void) {
    if (!s_ready) {
        return -1;
    }
    int v = read_reg(s_axp, AXP_REG_STATUS2);
    if (v < 0) {
        return -1;
    }
    return ((v >> 5) & 0x03) == 0x01;
}
