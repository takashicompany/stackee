#include "stackee_lcd.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "stackee_font8x8.h"
#include "stackee_perf.h"

static const char *TAG = "lcd";

#define LCD_PIN_CLOCK   36
#define LCD_PIN_MOSI    37
#define LCD_PIN_DC      35
#define LCD_PIN_CS      3
#define LCD_PCLK_HZ     40000000

#define LCD_CMD_CASET   0x2A
#define LCD_CMD_RASET   0x2B
#define LCD_CMD_RAMWR   0x2C

#define LCD_CHUNK_ROWS  16
#define LCD_ROW_BYTES   (STACKEE_LCD_WIDTH * 2)
#define LCD_MASK_BYTES  ((STACKEE_LCD_HEIGHT + 7) / 8)

// ILI9342C Memory Access Control (0x36)。
//
// ★ 未確認: この値は実機の画面を見るまで正しさを保証できない
//   (native-lcd/README.md:146-149 に同じ注意がある)。240x320 の縦になる候補は
//   0xA8 (MY|MV|BGR) と 0x68 (MX|MV|BGR) の 2 つで、どちらになるかは
//   上下左右のどちらが反転するかの違い。鏡像・天地逆に見えたらここを
//   0x68 / 0x28 / 0xE8 に変えて焼き直す。起動画面は上に黒帯・中央に文字と
//   わざと非対称にしてあるので、間違っていれば一目で分かる。
#define LCD_MADCTL      0xA8

// board.c:22-38 と同じ列。0x36 のデータだけ LCD_MADCTL に差し替える。
#define LCD_INIT_DELAY 0x80
static const uint8_t lcd_init_sequence[] = {
    0x01, LCD_INIT_DELAY, 0x80,   // Software reset、128 ms 待つ
    0xC8, 0x03, 0xFF, 0x93, 0x42, // 外部コマンド有効
    0xC0, 0x02, 0x12, 0x12,       // Power Control 1
    0xC1, 0x01, 0x03,             // Power Control 2
    0xC5, 0x01, 0xF2,             // VCOM Control 1
    0xB0, 0x01, 0xE0,             // RGB Interface SYNC Mode
    0xF6, 0x03, 0x01, 0x00, 0x00, // Interface control
    0xE0, 0x0F, 0x00, 0x0C, 0x11, 0x04, 0x11, 0x08, 0x37, 0x89, 0x4C, 0x06, 0x0C, 0x0A, 0x2E, 0x34, 0x0F,
    0xE1, 0x0F, 0x00, 0x0B, 0x11, 0x05, 0x13, 0x09, 0x33, 0x67, 0x48, 0x07, 0x0E, 0x0B, 0x2E, 0x33, 0x0F,
    0xB6, 0x04, 0x08, 0x82, 0x1D, 0x04, // Display Function Control
    0x3A, 0x01, 0x55,             // COLMOD: 16 bit
    0x21, 0x00,                   // 表示反転 ON
    0x36, 0x01, 0x08,             // Memory Access Control (LCD_MADCTL に差し替える)
    0x11, LCD_INIT_DELAY, 0x78,   // Sleep out、120 ms 待つ
    0x29, LCD_INIT_DELAY, 0x78,   // Display on、120 ms 待つ
};

static struct {
    uint8_t *frame;                 // 240*320*2、PSRAM
    uint8_t *chunk;                 // 16 行ぶん、DMA 可能な内蔵 SRAM
    esp_lcd_panel_io_handle_t io;
    TaskHandle_t task;
    portMUX_TYPE lock;
    uint8_t pending[LCD_MASK_BYTES];
    uint8_t dirty[LCD_MASK_BYTES];  // 描画側が積む。flush で pending へ移す
    atomic_bool ready;
    atomic_uint transfers;
    atomic_uint rows_sent;
    atomic_uint worker_ms;
} lcd = {.lock = portMUX_INITIALIZER_UNLOCKED};

// ---------------------------------------------------------------------------
// ワーカー (CPU0)
// ---------------------------------------------------------------------------
static void lcd_send_chunk(uint16_t y, uint16_t rows) {
    uint16_t right = STACKEE_LCD_WIDTH - 1;
    uint16_t last = y + rows - 1;
    uint8_t caset[4] = {0, 0, (uint8_t)(right >> 8), (uint8_t)(right & 0xFF)};
    uint8_t raset[4] = {(uint8_t)(y >> 8), (uint8_t)(y & 0xFF),
                        (uint8_t)(last >> 8), (uint8_t)(last & 0xFF)};
    size_t bytes = (size_t)rows * LCD_ROW_BYTES;

    if (esp_lcd_panel_io_tx_param(lcd.io, LCD_CMD_CASET, caset, sizeof(caset)) != ESP_OK) {
        return;
    }
    if (esp_lcd_panel_io_tx_param(lcd.io, LCD_CMD_RASET, raset, sizeof(raset)) != ESP_OK) {
        return;
    }
    memcpy(lcd.chunk, lcd.frame + (size_t)y * LCD_ROW_BYTES, bytes);
    if (esp_lcd_panel_io_tx_param(lcd.io, LCD_CMD_RAMWR, lcd.chunk, bytes) == ESP_OK) {
        atomic_fetch_add(&lcd.rows_sent, rows);
    }
}

static void lcd_worker(void *unused) {
    (void)unused;
    uint8_t mask[LCD_MASK_BYTES];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        // 汚れた行を「取り出して消す」を 1 手で。ここより後に付いた印は
        // 次の回まで残るので、更新が落ちることはない。
        taskENTER_CRITICAL(&lcd.lock);
        memcpy(mask, lcd.pending, sizeof(mask));
        memset(lcd.pending, 0, sizeof(lcd.pending));
        taskEXIT_CRITICAL(&lcd.lock);

        bool sent = false;
        int64_t started = esp_timer_get_time();
        uint16_t y = 0;
        while (y < STACKEE_LCD_HEIGHT) {
            if (!(mask[y >> 3] & (1 << (y & 7)))) {
                y++;
                continue;
            }
            uint16_t end = y;
            while (end < STACKEE_LCD_HEIGHT && (mask[end >> 3] & (1 << (end & 7)))) {
                end++;
            }
            while (y < end) {
                uint16_t rows = end - y;
                if (rows > LCD_CHUNK_ROWS) {
                    rows = LCD_CHUNK_ROWS;
                }
                lcd_send_chunk(y, rows);
                sent = true;
                y += rows;
            }
        }
        if (sent) {
            int64_t spent = esp_timer_get_time() - started;
            atomic_fetch_add(&lcd.transfers, 1);
            atomic_fetch_add(&lcd.worker_ms, (unsigned)(spent / 1000));
            // 段階 2: 1 回の転送にかかった時間 (status の perf.ui)。
            stackee_perf_sample(STACKEE_PERF_UI, (uint32_t)spent);
        }
    }
}

// ---------------------------------------------------------------------------
// 立ち上げ
// ---------------------------------------------------------------------------
static void lcd_run_init_sequence(void) {
    size_t i = 0;
    while (i < sizeof(lcd_init_sequence)) {
        const uint8_t *entry = lcd_init_sequence + i;
        uint8_t data_size = entry[1];
        bool delay = (data_size & LCD_INIT_DELAY) != 0;
        data_size &= (uint8_t)~LCD_INIT_DELAY;
        if (entry[0] == 0x36 && data_size == 1) {
            uint8_t madctl = LCD_MADCTL;
            esp_lcd_panel_io_tx_param(lcd.io, 0x36, &madctl, 1);
        } else {
            uint8_t param[16];
            if (data_size) {
                memcpy(param, entry + 2,
                       data_size > sizeof(param) ? sizeof(param) : data_size);
            }
            esp_lcd_panel_io_tx_param(lcd.io, entry[0], data_size ? param : NULL, data_size);
        }
        uint16_t delay_ms = 10;
        if (delay) {
            data_size++;
            delay_ms = entry[1 + data_size];
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        i += 2 + data_size;
    }
}

esp_err_t stackee_lcd_init(void) {
    lcd.frame = heap_caps_malloc((size_t)LCD_ROW_BYTES * STACKEE_LCD_HEIGHT,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (lcd.frame == NULL) {
        // PSRAM が無い / 取れないときは内蔵 RAM を試す (150 KB なので厳しい)。
        lcd.frame = heap_caps_malloc((size_t)LCD_ROW_BYTES * STACKEE_LCD_HEIGHT,
                                     MALLOC_CAP_8BIT);
    }
    if (lcd.frame == NULL) {
        ESP_LOGE(TAG, "フレームバッファ %d B を確保できない",
                 LCD_ROW_BYTES * STACKEE_LCD_HEIGHT);
        return ESP_ERR_NO_MEM;
    }
    lcd.chunk = heap_caps_malloc(LCD_CHUNK_ROWS * LCD_ROW_BYTES,
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (lcd.chunk == NULL) {
        ESP_LOGE(TAG, "DMA 用の中継バッファを確保できない");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_CLOCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_CHUNK_ROWS * LCD_ROW_BYTES,
        .intr_flags = 0,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 を開けない: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 2,
        // 完了コールバックは置かない。ワーカーが esp_lcd の中で待つので、
        // SPI 割り込みの中で何かをする必要がない (native-lcd と同じ判断)。
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &lcd.io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "パネル IO を作れない: %s", esp_err_to_name(err));
        return err;
    }

    lcd_run_init_sequence();

    if (xTaskCreatePinnedToCore(lcd_worker, "stackee_lcd", 4096, NULL, 1,
                                &lcd.task, 0) != pdPASS) {
        ESP_LOGE(TAG, "転送タスクを作れない");
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&lcd.ready, true);
    ESP_LOGI(TAG, "ILI9342C %dx%d madctl=0x%02X 起動",
             STACKEE_LCD_WIDTH, STACKEE_LCD_HEIGHT, LCD_MADCTL);
    return ESP_OK;
}

bool stackee_lcd_ready(void) {
    return atomic_load(&lcd.ready);
}

// ---------------------------------------------------------------------------
// 描画 (呼んだタスクの上で走る。SPI には触らない)
// ---------------------------------------------------------------------------
static inline void mark_rows(int y, int h) {
    for (int row = y; row < y + h; row++) {
        if (row >= 0 && row < STACKEE_LCD_HEIGHT) {
            lcd.dirty[row >> 3] |= (uint8_t)(1 << (row & 7));
        }
    }
}

void stackee_lcd_fill(int x, int y, int w, int h, uint16_t color) {
    if (lcd.frame == NULL) {
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > STACKEE_LCD_WIDTH)  { w = STACKEE_LCD_WIDTH - x; }
    if (y + h > STACKEE_LCD_HEIGHT) { h = STACKEE_LCD_HEIGHT - y; }
    if (w <= 0 || h <= 0) {
        return;
    }
    // パネルは上位バイトから受け取るので、そのまま並べておく。
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);
    for (int row = y; row < y + h; row++) {
        uint8_t *p = lcd.frame + (size_t)row * LCD_ROW_BYTES + (size_t)x * 2;
        for (int col = 0; col < w; col++) {
            *p++ = hi;
            *p++ = lo;
        }
    }
    mark_rows(y, h);
}

void stackee_lcd_text(int x, int y, const char *text,
                      uint16_t fg, uint16_t bg, int scale) {
    if (lcd.frame == NULL || text == NULL) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    for (const char *p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < STACKEE_FONT_FIRST || ch > STACKEE_FONT_LAST) {
            ch = '?';
        }
        const uint8_t *glyph = stackee_font8x8[ch - STACKEE_FONT_FIRST];
        for (int row = 0; row < 8; row++) {
            for (int col = 0; col < 8; col++) {
                uint16_t color = (glyph[row] & (1 << col)) ? fg : bg;
                stackee_lcd_fill(x + col * scale, y + row * scale,
                                 scale, scale, color);
            }
        }
        x += 8 * scale;
    }
}

uint8_t *stackee_lcd_framebuffer(void) {
    return lcd.frame;
}

size_t stackee_lcd_framebuffer_size(void) {
    return (size_t)LCD_ROW_BYTES * STACKEE_LCD_HEIGHT;
}

void stackee_lcd_mark_rows(int y, int h) {
    mark_rows(y, h);
}

void stackee_lcd_flush(void) {
    if (!atomic_load(&lcd.ready)) {
        return;
    }
    bool any = false;
    taskENTER_CRITICAL(&lcd.lock);
    for (size_t i = 0; i < LCD_MASK_BYTES; i++) {
        lcd.pending[i] |= lcd.dirty[i];
        if (lcd.pending[i]) {
            any = true;
        }
        lcd.dirty[i] = 0;
    }
    taskEXIT_CRITICAL(&lcd.lock);
    if (any && lcd.task != NULL) {
        xTaskNotifyGive(lcd.task);
    }
}

void stackee_lcd_stats(uint32_t *transfers, uint32_t *rows_sent, uint32_t *worker_ms) {
    if (transfers) { *transfers = atomic_load(&lcd.transfers); }
    if (rows_sent) { *rows_sent = atomic_load(&lcd.rows_sent); }
    if (worker_ms) { *worker_ms = atomic_load(&lcd.worker_ms); }
}
