#include "stackee_codec.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "stackee_board.h"
#include "stackee_volume.h"

static const char *TAG = "codec";

static i2c_master_dev_handle_t s_es;
static i2c_master_dev_handle_t s_aw;
static bool s_ready;
static bool s_amp_on;
static int  s_applied = -1;

// ---------------------------------------------------------------------------
// 小物
// ---------------------------------------------------------------------------
static esp_err_t w8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t value) {
    uint8_t buf[2] = {reg, value};
    return stackee_board_i2c_write(dev, buf, sizeof(buf));
}

static int r8(i2c_master_dev_handle_t dev, uint8_t reg) {
    uint8_t out = 0;
    if (stackee_board_i2c_write_read(dev, &reg, 1, &out, 1) != ESP_OK) {
        return -1;
    }
    return out;
}

static esp_err_t u8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t mask, uint8_t data) {
    int v = r8(dev, reg);
    if (v < 0) {
        return ESP_FAIL;
    }
    return w8(dev, reg, (uint8_t)((v & ~mask) | (mask & data)));
}

// AW88298 は 16bit レジスタを上位バイト先で書く (M5Unified の bswap16 と同じ)。
static esp_err_t w16(uint8_t reg, uint16_t value) {
    uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return stackee_board_i2c_write(s_aw, buf, sizeof(buf));
}

static int r16(uint8_t reg) {
    uint8_t out[2] = {0, 0};
    if (stackee_board_i2c_write_read(s_aw, &reg, 1, out, 2) != ESP_OK) {
        return -1;
    }
    return (out[0] << 8) | out[1];
}

esp_err_t stackee_codec_init(void) {
    if (s_ready) {
        return ESP_OK;
    }
    esp_err_t err = stackee_board_i2c_add(STACKEE_ES7210_ADDR, &s_es);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 を登録できない: %s", esp_err_to_name(err));
        return err;
    }
    err = stackee_board_i2c_add(STACKEE_AW88298_ADDR, &s_aw);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW88298 を登録できない: %s", esp_err_to_name(err));
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

bool stackee_codec_ready(void) {
    return s_ready;
}

// ---------------------------------------------------------------------------
// ES7210
// ---------------------------------------------------------------------------
#define ES_RESET        0x00
#define ES_CLOCK_OFF    0x01
#define ES_MAINCLK      0x02
#define ES_POWER_DOWN   0x06
#define ES_OSR          0x07
#define ES_MODE_CONFIG  0x08
#define ES_TIME0        0x09
#define ES_TIME1        0x0A
#define ES_SDP1         0x11
#define ES_SDP2         0x12
#define ES_ADC34_HPF2   0x20
#define ES_ADC34_HPF1   0x21
#define ES_ADC12_HPF1   0x22
#define ES_ADC12_HPF2   0x23
#define ES_ANALOG       0x40
#define ES_MIC12_BIAS   0x41
#define ES_MIC34_BIAS   0x42
#define ES_MIC1_GAIN    0x43
#define ES_MIC2_GAIN    0x44
#define ES_MIC1_POWER   0x47
#define ES_MIC12_POWER  0x4B
#define ES_MIC34_POWER  0x4C

// code.py は gain_db=37.5 を渡す。es7210.py の _gain_code(37.5) = 14
// (アナログ PGA の上限)。30 dB では -44dBFS と小さすぎた、というのが
// code.py のコメントに書いてある実測。
#define ES_GAIN_CODE    14

// es7210.py の _mic_select_config()。MIC1 + MIC2 の 2ch = 通常の I2S。
static void es7210_mic_select(void) {
    for (int i = 0; i < 4; i++) {
        u8(s_es, (uint8_t)(ES_MIC1_GAIN + i), 0x10, 0x00);
    }
    w8(s_es, ES_MIC12_POWER, 0xFF);
    w8(s_es, ES_MIC34_POWER, 0xFF);
    // MIC1
    u8(s_es, ES_CLOCK_OFF, 0x0B, 0x00);
    w8(s_es, ES_MIC12_POWER, 0x00);
    u8(s_es, ES_MIC1_GAIN, 0x10, 0x10);
    u8(s_es, ES_MIC1_GAIN, 0x0F, ES_GAIN_CODE);
    // MIC2
    u8(s_es, ES_CLOCK_OFF, 0x0B, 0x00);
    w8(s_es, ES_MIC12_POWER, 0x00);
    u8(s_es, ES_MIC2_GAIN, 0x10, 0x10);
    u8(s_es, ES_MIC2_GAIN, 0x0F, ES_GAIN_CODE);
    w8(s_es, ES_SDP2, 0x00);        // 2ch までは通常の I2S (3ch 以上は TDM)
}

esp_err_t stackee_es7210_setup(void) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = w8(s_es, ES_RESET, 0xFF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES7210 が応答しない: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    w8(s_es, ES_RESET, 0x41);
    w8(s_es, ES_CLOCK_OFF, 0x3F);
    w8(s_es, ES_TIME0, 0x30);
    w8(s_es, ES_TIME1, 0x30);
    w8(s_es, ES_ADC12_HPF2, 0x2A);
    w8(s_es, ES_ADC12_HPF1, 0x0A);
    w8(s_es, ES_ADC34_HPF2, 0x0A);
    w8(s_es, ES_ADC34_HPF1, 0x2A);
    // ★ スレーブ。ESP32 が BCLK/WS を出す (I2S はマスタで開く)。
    u8(s_es, ES_MODE_CONFIG, 0x01, 0x00);
    w8(s_es, ES_ANALOG, 0x43);
    w8(s_es, ES_MIC12_BIAS, 0x70);
    w8(s_es, ES_MIC34_BIAS, 0x70);
    w8(s_es, ES_OSR, 0x20);
    w8(s_es, ES_MAINCLK, 0xC1);
    es7210_mic_select();
    // 16bit / I2S 形式 (es7210.py の _set_bits と _config_fmt)。
    int v = r8(s_es, ES_SDP1);
    if (v >= 0) {
        w8(s_es, ES_SDP1, (uint8_t)((v & 0x1F) | 0x60));
    }
    v = r8(s_es, ES_SDP1);
    if (v >= 0) {
        w8(s_es, ES_SDP1, (uint8_t)(v & 0xFC));
    }
    return stackee_es7210_enable(true);
}

esp_err_t stackee_es7210_enable(bool on) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (on) {
        int off = r8(s_es, ES_CLOCK_OFF);
        if (off >= 0) {
            w8(s_es, ES_CLOCK_OFF, (uint8_t)(off & 0x3F));
        }
        w8(s_es, ES_POWER_DOWN, 0x00);
        w8(s_es, ES_ANALOG, 0x43);
        for (int i = 0; i < 4; i++) {
            w8(s_es, (uint8_t)(ES_MIC1_POWER + i), 0x08);
        }
        es7210_mic_select();
        w8(s_es, ES_ANALOG, 0x43);
        w8(s_es, ES_RESET, 0x71);
        w8(s_es, ES_RESET, 0x41);
    } else {
        for (int i = 0; i < 4; i++) {
            w8(s_es, (uint8_t)(ES_MIC1_POWER + i), 0xFF);
        }
        w8(s_es, ES_MIC12_POWER, 0xFF);
        w8(s_es, ES_MIC34_POWER, 0xFF);
        w8(s_es, ES_ANALOG, 0xC0);
        w8(s_es, ES_CLOCK_OFF, 0x7F);
        w8(s_es, ES_POWER_DOWN, 0x07);
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// AW88298
// ---------------------------------------------------------------------------
// M5Unified.cpp:464 の rate_tbl。
static const uint8_t AW_RATE_TBL[] = {4, 5, 6, 8, 10, 11, 15, 20, 22, 44};

uint16_t stackee_aw88298_reg06(int sample_rate) {
    int rate = (sample_rate + 1102) / 2205;
    size_t idx = 0;
    const size_t limit = sizeof(AW_RATE_TBL) / sizeof(AW_RATE_TBL[0]);
    while (rate > AW_RATE_TBL[idx]) {
        idx++;
        if (idx >= limit) {
            break;
        }
    }
    if (idx >= limit) {
        idx = limit - 1;
    }
    return (uint16_t)(idx | 0x14C0);    // I2SBCK = 16bit x 2ch
}

// stackee_speaker.py の _apply_volume_locked と同じ順序。
// 戻り値は「当てられたか」。★ 失敗を黙って飲まない — 飲むと
// フルボリュームのまま鳴り出す (stackee_speaker.py は例外を投げ、
// stackee_halfduplex.py がその往復を丸ごと諦める)。
static bool apply_volume_locked(int percent) {
    int control = r16(0x05);
    int hold = r16(0x0C);
    if (control < 0 || hold < 0) {
        return false;
    }
    hold &= 0xFF;
    esp_err_t err = ESP_OK;
    if (percent == 0) {
        err = w16(0x05, (uint16_t)(control | 0x10));    // HMUTE
    }
    if (err == ESP_OK) {
        err = w16(0x0C, (uint16_t)(stackee_volume_bits(percent) | hold));
    }
    if (err == ESP_OK && percent > 0) {
        err = w16(0x05, (uint16_t)(control & ~0x10));
    }
    if (err != ESP_OK) {
        return false;
    }
    s_applied = percent;
    return true;
}

esp_err_t stackee_aw88298_on(int sample_rate, int volume_percent, int *was_on) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    int before = stackee_board_aw9523_p0(STACKEE_AW9523_SPK_EN, true);
    if (was_on != NULL) {
        *was_on = before;
    }
    esp_err_t err = w16(0x61, 0x0673);           // boost mode disabled
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AW88298 が応答しない: %s", esp_err_to_name(err));
        return err;
    }
    // ★ 音量を先に決める。M5Unified は 0x0C にフル (0x0064) を書いてから
    //   ソフト側で振幅を絞るが、こちらはレジスタで絞る。だから 0x0C に
    //   フルを書いて**から**絞ると、その一瞬だけ全開になる。ここでは
    //   最初から当てたい値を書く。
    uint16_t bits = (uint16_t)(stackee_volume_bits(volume_percent) | 0x64);
    // 音量 0 のときは HMUTE (0x05 の bit4) も立てたまま起こす。
    uint16_t control = (volume_percent == 0) ? 0x0018 : 0x0008;
    esp_err_t steps[4];
    steps[0] = w16(0x04, 0x4040);                // I2SEN=1 AMPPD=0 PWDN=0
    steps[1] = w16(0x05, control);               // RMSE/HAGCE/HDCCE
    steps[2] = w16(0x06, stackee_aw88298_reg06(sample_rate));
    steps[3] = w16(0x0C, bits);
    for (int i = 0; i < 4; i++) {
        if (steps[i] != ESP_OK) {
            ESP_LOGE(TAG, "AW88298 の設定に失敗 (%d)。鳴らさない", i);
            w16(0x04, 0x4000);                   // I2SEN=0 に戻す
            stackee_board_aw9523_p0(STACKEE_AW9523_SPK_EN, before == 1);
            return steps[i];
        }
    }
    s_amp_on = true;
    s_applied = volume_percent;
    return ESP_OK;
}

esp_err_t stackee_aw88298_off(int was_on) {
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    w16(0x04, 0x4000);                           // I2SEN=0
    s_amp_on = false;
    s_applied = -1;
    // ★ 元から ON だったビットは触らない。ボード定義が起動時に立てている
    //   もので、勝手に落とすと他 (タッチ等) の初期化状態を壊す。
    //   was_on < 0 は「読めなかった」。そのときも落としておく
    //   (こちらが立てたかもしれないビットを上げっぱなしにしない)。
    if (was_on != 1) {
        stackee_board_aw9523_p0(STACKEE_AW9523_SPK_EN, false);
    }
    return ESP_OK;
}

esp_err_t stackee_aw88298_volume(int percent) {
    if (!s_ready || !s_amp_on || percent == s_applied) {
        return ESP_OK;
    }
    return apply_volume_locked(percent) ? ESP_OK : ESP_FAIL;
}

bool stackee_aw88298_powered(void) {
    return s_amp_on;
}
