#include "stackee_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "stackee_board.h"
#include "stackee_console.h"
#include "stackee_http.h"
#include "stackee_settings.h"
#include "stackee_ui.h"

static const char *TAG = "camera";

// ★ esp32-camera の**私的ヘッダ** (driver/private_include/cam_hal.h) にある
//   口。シンボルは外に出ているので、ここで宣言だけして使う
//   (stackee_conhid.c が via_command_kb を宣言しているのと同じやり方)。
//
// ★★ これが段階 4 の一番の山だった。既定 (PSRAM DMA 無効) では
//   LCD_CAM の DMA が**内蔵 RAM の連続 30,720 バイト**を要求する。
//   Wi-Fi と BLE が上がったあとの内蔵 RAM は断片化していて、実機では
//   いちばん大きい空き塊が 4,096 バイトしか無く、必ず失敗した
//   (2026-09-16 実測: "DMA buffer 30720 Byte malloc failed,
//    the current largest free block:4096 Byte")。
//
//   PSRAM DMA を有効にすると DMA は PSRAM のフレームバッファへ直接書き、
//   内蔵に要るのは**記述子 40 個 = 480 バイト**だけになる
//   (cam_hal.c の cam_dma_config は psram_mode のとき内蔵の dma_buffer を
//    そもそも確保しない)。
void cam_set_psram_mode(bool enable);
bool cam_get_psram_mode(void);

// ---- AXP2101 (内部 I2C 0x34) -----------------------------------------------
// REG 0x90 = LDO の ON/OFF ビット。bit2 = ALDO3 = カメラの 3.3V。
// CoreS3 のボード初期化が 0x90 = 0xBF を書くので、電源投入直後は
// **ALDO3 が入っている**。素子はまだ初期化されていない = 自走していない。
#define AXP_REG_LDO_EN 0x90
#define AXP_ALDO3_BIT  0x04

// ---- CoreS3 のカメラ (GC0308) のピン ---------------------------------------
// 出所: CircuitPython の boards/m5stack_cores3/pins.c (CAMERA_*) と
//       works/private/stack-chan の cores3-camera-test/src/main.cpp。
#define CAM_PIN_D0     39
#define CAM_PIN_D1     40
#define CAM_PIN_D2     41
#define CAM_PIN_D3     42
#define CAM_PIN_D4     15
#define CAM_PIN_D5     16
#define CAM_PIN_D6     48
#define CAM_PIN_D7     47
#define CAM_PIN_VSYNC  46
#define CAM_PIN_HREF   38
#define CAM_PIN_PCLK   45
// ★ XCLK は出さない。GPIO2 は PORT.A の SDA = TCA8418 (キーマトリクス)。
#define CAM_PIN_XCLK   (-1)
#define CAM_XCLK_HZ    20000000
#define CAM_I2C_PORT   0            // 内部 I2C (stackee_board.c が開いている)

// ---- GC0308 のレジスタ (向きを直接書くため) --------------------------------
// ★ ドライバの set_hmirror / set_vflip は使わない。あちらは 0x14 を
//   read-modify-write するが、この基板では素子の読み出しが 1 手ぶん古い値を
//   返すことがあり、取り違える (現行 CircuitPython 版の実測)。絶対値を 1 回書く。
#define GC0308_REG_PAGE    0xFE
#define GC0308_REG_MODE1   0x14
#define GC0308_MODE1_BASE  0x10
#define GC0308_HMIRROR     0x01
#define GC0308_VFLIP       0x02

// ---- 実測で決まった時間 -----------------------------------------------------
#define POWER_SETTLE_MS  1000   // ALDO3 を入れてから素子が起きるまで
#define TAKE_TIMEOUT_MS  3000
#define DEFAULT_WARMUP   30
#define MAX_WARMUP       120
#define DEFAULT_QUALITY  12

// console の camera.dump で 1 回に返す上限 [byte]。base64 で約 1.37 倍。
#define DUMP_MAX 1024

static struct {
    SemaphoreHandle_t lock;
    bool      opened;
    uint8_t  *jpeg;
    size_t    jpeg_len;
    size_t    jpeg_cap;
    volatile bool key_pending;
    stackee_camera_stats_t st;
    stackee_camera_req_t   last_req;
    char      send_path[64];
} cam;

static void set_state(const char *s) { cam.st.state = s; }

static void note(const char *why) {
    snprintf(cam.st.error, sizeof(cam.st.error), "%s", why);
}

// ---------------------------------------------------------------------------
// 電源 (ALDO3)
// ---------------------------------------------------------------------------
int stackee_camera_power_is_on(void) {
    int v = stackee_board_axp_bit(AXP_REG_LDO_EN, AXP_ALDO3_BIT, -1);
    if (v < 0) {
        return -1;
    }
    return (v & AXP_ALDO3_BIT) ? 1 : 0;
}

esp_err_t stackee_camera_power(bool on) {
    int v = stackee_board_axp_bit(AXP_REG_LDO_EN, AXP_ALDO3_BIT, on ? 1 : 0);
    if (v < 0) {
        note("aldo3");
        return ESP_FAIL;
    }
    bool now = (v & AXP_ALDO3_BIT) != 0;
    if (now != on) {
        note("aldo3_stuck");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// GC0308 の向き
// ---------------------------------------------------------------------------
static bool gc_write(uint8_t reg, uint8_t value) {
    // ★ mask を 0xFF にすると「読んだ値は全部捨てて、この値をそのまま書く」。
    //   ドライバの set_hmirror / set_vflip のような読んで直す形にしないのは、
    //   この基板では素子の読み出しが 1 手ぶん古い値を返すことがあるため
    //   (0x11 を書いて読むと 0x10、0x12 を書いて読むと 0x11)。
    sensor_t *s = esp_camera_sensor_get();
    if (s == NULL || s->set_reg == NULL) {
        return false;
    }
    return s->set_reg(s, reg, 0xFF, value) >= 0;
}

static void apply_orientation(bool hmirror, bool vflip) {
    uint8_t v = GC0308_MODE1_BASE;
    if (hmirror) { v |= GC0308_HMIRROR; }
    if (vflip)   { v |= GC0308_VFLIP; }
    if (!gc_write(GC0308_REG_PAGE, 0x00) || !gc_write(GC0308_REG_MODE1, v)) {
        ESP_LOGW(TAG, "向き (0x14) を書けない。素子の既定のまま撮る");
        return;
    }
    cam.st.hmirror = hmirror;
    cam.st.vflip = vflip;
}

// ---------------------------------------------------------------------------
// 撮る
// ---------------------------------------------------------------------------
void stackee_camera_req_default(stackee_camera_req_t *out) {
    if (out == NULL) {
        return;
    }
    out->size = STACKEE_CAM_QVGA;
    out->hmirror = true;
    out->vflip = true;
    out->warmup = DEFAULT_WARMUP;
    out->quality = DEFAULT_QUALITY;
}

static esp_err_t open_camera_once(const stackee_camera_req_t *req, bool psram) {
    cam_set_psram_mode(psram);
    camera_config_t config = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = CAM_PIN_XCLK,
        // ★ sccb のピンを -1 にすると、既に開いている I2C ポートを使う
        //   (esp32-camera の SCCB_Use_Port)。内部 I2C は AXP2101 や
        //   ES7210 と共有しているので、二重に開かせない。
        .pin_sccb_sda = -1,
        .pin_sccb_scl = -1,
        .sccb_i2c_port = CAM_I2C_PORT,
        .pin_d0 = CAM_PIN_D0, .pin_d1 = CAM_PIN_D1,
        .pin_d2 = CAM_PIN_D2, .pin_d3 = CAM_PIN_D3,
        .pin_d4 = CAM_PIN_D4, .pin_d5 = CAM_PIN_D5,
        .pin_d6 = CAM_PIN_D6, .pin_d7 = CAM_PIN_D7,
        .pin_vsync = CAM_PIN_VSYNC,
        .pin_href = CAM_PIN_HREF,
        .pin_pclk = CAM_PIN_PCLK,
        .xclk_freq_hz = CAM_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        // ★ GC0308 に JPEG エンコーダは無い。生の RGB565 で撮って、
        //   こちらで JPEG に畳む (frame2jpg)。
        .pixel_format = PIXFORMAT_RGB565,
        .frame_size = (req->size == STACKEE_CAM_QQVGA) ? FRAMESIZE_QQVGA
                                                       : FRAMESIZE_QVGA,
        .jpeg_quality = req->quality,
        .fb_count = 1,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    };
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        return err;
    }
    cam.opened = true;
    return ESP_OK;
}

static esp_err_t open_camera(const stackee_camera_req_t *req) {
    // 撮る直前の内蔵 RAM。失敗したときにここを見れば原因が分かる。
    cam.st.dma_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    cam.st.internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "撮る前: 内蔵 RAM 空き %lu B / DMA の最大の塊 %lu B",
             (unsigned long)cam.st.internal_free,
             (unsigned long)cam.st.dma_largest);

    // 1. まず PSRAM DMA。内蔵 RAM をほとんど使わない。
    esp_err_t err = open_camera_once(req, true);
    if (err == ESP_OK) {
        cam.st.psram_dma = true;
        return ESP_OK;
    }
    ESP_LOGW(TAG, "PSRAM DMA で開けない (%s)。内蔵 DMA でやり直す",
             esp_err_to_name(err));
    esp_camera_deinit();        // 途中まで確保したものを返す
    cam.opened = false;

    // 2. だめなら内蔵 DMA。sdkconfig で DMA バッファを 8 KB に絞ってある
    //    ので、断片化していても入る見込みがある。
    err = open_camera_once(req, false);
    if (err == ESP_OK) {
        cam.st.psram_dma = false;
        return ESP_OK;
    }
    note("init");
    ESP_LOGE(TAG, "カメラを開けない (%s)。内蔵 RAM の最大の塊 %lu B",
             esp_err_to_name(err),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return err;
}

// カメラを畳んで ALDO3 を切る。例外が出ても最後まで進む。
static void shutdown_camera(void) {
    if (cam.opened) {
        esp_camera_deinit();
        cam.opened = false;
    }
    if (stackee_camera_power(false) != ESP_OK) {
        // ★ ここが失敗したらハードリセットは危ない。GC0308 が自走したまま
        //   リセットするとブートループになる (現行版の TRANSFER_NOTES.md)。
        ESP_LOGE(TAG, "★ ALDO3 を切れない。ハードリセット禁止");
    }
    stackee_camera_pin_strap_safe();
}

// ---------------------------------------------------------------------------
// ★ カメラの PCLK (G45) と VSYNC (G46) は ESP32-S3 の**ストラッピングピン**。
//   G45 = VDD_SPI の電圧選択 (High → 1.8V)、G46 = ダウンロードモードの条件。
//   撮影のあと esp_restart() すると ROM がフラッシュを読めず
//   "invalid header: 0xffffff1f" のブートループに落ちる (2026-09-16 実測、
//   README §17)。ALDO3 を切っても線は High のまま (内部 I2C のプルアップ
//   経由で素子が寄生給電されていると見ている)。
//   対処: 撮影が終わったら (そしてどのリセット経路でも直前に) 2 本を
//   出力 Low にして gpio_hold_en() で固定する。hold は RTC 領域なので
//   ソフトリセット/ウォッチドッグを跨いで残り、ROM が strap を読む時点で
//   Low になっている。カメラを次に開くときは hold を外す。
// ---------------------------------------------------------------------------
void stackee_camera_pin_strap_safe(void) {
    const gpio_num_t pins[] = { CAM_PIN_PCLK, CAM_PIN_VSYNC };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_hold_dis(pins[i]);
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLDOWN_ONLY);
        gpio_set_level(pins[i], 0);
        gpio_hold_en(pins[i]);
    }
}

static void release_strap_pins(void) {
    const gpio_num_t pins[] = { CAM_PIN_PCLK, CAM_PIN_VSYNC };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_hold_dis(pins[i]);
        gpio_reset_pin(pins[i]);
    }
}

static bool ensure_jpeg_cap(size_t want) {
    if (cam.jpeg != NULL && cam.jpeg_cap >= want) {
        return true;
    }
    free(cam.jpeg);
    cam.jpeg = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (cam.jpeg == NULL) {
        cam.jpeg_cap = 0;
        return false;
    }
    cam.jpeg_cap = want;
    return true;
}

esp_err_t stackee_camera_capture(const stackee_camera_req_t *req_in) {
    stackee_camera_req_t req;
    if (req_in != NULL) {
        req = *req_in;
    } else {
        stackee_camera_req_default(&req);
    }
    if (req.warmup < 1) { req.warmup = 1; }
    if (req.warmup > MAX_WARMUP) { req.warmup = MAX_WARMUP; }
    if (req.quality < 1) { req.quality = 1; }
    if (req.quality > 63) { req.quality = 63; }

    if (cam.lock == NULL ||
        xSemaphoreTake(cam.lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        note("busy");
        return ESP_ERR_INVALID_STATE;
    }
    cam.last_req = req;
    cam.st.job++;
    cam.st.error[0] = '\0';
    cam.st.jpeg_bytes = 0;
    cam.st.warmup = req.warmup;
    cam.st.quality = req.quality;
    set_state("capturing");
    // ★ 撮影中は画面を止める (DESIGN.md §3)。PSRAM の帯域と CPU0 を
    //   カメラに明け渡す。画面はそのまま残る。
    stackee_ui_pause(true);

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ESP_OK;
    camera_fb_t *fb = NULL;

    // 1. 電源。★ 入れてから 1 秒置く (50 ms では素子が起きない)。
    release_strap_pins();       // 前回の撮影で固定した G45/G46 を返す
    err = stackee_camera_power(true);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(POWER_SETTLE_MS));
    }
    cam.st.t_power_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);

    // 2. 初期化。
    int64_t t1 = esp_timer_get_time();
    if (err == ESP_OK) {
        err = open_camera(&req);
    }
    if (err == ESP_OK) {
        // ★ 1 枚も撮る前に向きを書く。撮ってから書いても、その絵には効かない。
        apply_orientation(req.hmirror, req.vflip);
    }
    cam.st.t_init_ms = (uint32_t)((esp_timer_get_time() - t1) / 1000);

    // 3. 捨て駒。AE / AWB は撮り始めてから数十枚かけて収束する。
    int64_t t2 = esp_timer_get_time();
    int warmed = 0;
    if (err == ESP_OK) {
        for (warmed = 0; warmed < req.warmup; warmed++) {
            camera_fb_t *drop = esp_camera_fb_get();
            if (drop == NULL) {
                break;
            }
            esp_camera_fb_return(drop);
        }
        if (warmed == 0) {
            note("nowarm");
            err = ESP_ERR_TIMEOUT;
        }
    }
    cam.st.t_warm_ms = (uint32_t)((esp_timer_get_time() - t2) / 1000);

    // 4. 本番の 1 枚。
    int64_t t3 = esp_timer_get_time();
    if (err == ESP_OK) {
        fb = esp_camera_fb_get();
        if (fb == NULL) {
            note("noframe");
            err = ESP_ERR_TIMEOUT;
        }
    }
    cam.st.t_take_ms = (uint32_t)((esp_timer_get_time() - t3) / 1000);

    // 5. JPEG に畳む。GC0308 は JPEG を出せないので、ここで software 圧縮。
    int64_t t4 = esp_timer_get_time();
    if (err == ESP_OK && fb != NULL) {
        cam.st.w = fb->width;
        cam.st.h = fb->height;
        cam.st.raw_bytes = fb->len;
        uint8_t *out = NULL;
        size_t out_len = 0;
        if (frame2jpg(fb, req.quality, &out, &out_len) && out != NULL) {
            if (ensure_jpeg_cap(out_len)) {
                memcpy(cam.jpeg, out, out_len);
                cam.jpeg_len = out_len;
                cam.st.jpeg_bytes = out_len;
            } else {
                note("nomem");
                err = ESP_ERR_NO_MEM;
            }
            free(out);
        } else {
            note("jpeg");
            err = ESP_FAIL;
        }
    }
    cam.st.t_jpeg_ms = (uint32_t)((esp_timer_get_time() - t4) / 1000);

    if (fb != NULL) {
        esp_camera_fb_return(fb);
    }

    // 6. 畳む。★ 必ず ALDO3 を切る。
    int64_t t5 = esp_timer_get_time();
    shutdown_camera();
    cam.st.t_stop_ms = (uint32_t)((esp_timer_get_time() - t5) / 1000);

    cam.st.ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        cam.st.captures++;
        set_state("done");
    } else {
        cam.st.failures++;
        set_state("error");
    }
    stackee_ui_pause(false);
    xSemaphoreGive(cam.lock);
    ESP_LOGI(TAG, "撮影 job %lu: %s %dx%d raw %u B -> jpeg %u B (%lu ms)",
             (unsigned long)cam.st.job, (err == ESP_OK) ? "ok" : cam.st.error,
             cam.st.w, cam.st.h, (unsigned)cam.st.raw_bytes,
             (unsigned)cam.st.jpeg_bytes, (unsigned long)cam.st.ms);
    return err;
}

const uint8_t *stackee_camera_jpeg(size_t *out_len) {
    if (out_len != NULL) {
        *out_len = cam.jpeg_len;
    }
    return (cam.jpeg_len > 0) ? cam.jpeg : NULL;
}

void stackee_camera_stats(stackee_camera_stats_t *out) {
    if (out != NULL) {
        *out = cam.st;
    }
}

// ---------------------------------------------------------------------------
// STK_CAMERA キー → camera タスク
// ---------------------------------------------------------------------------
void stackee_camera_key(void) {
    cam.key_pending = true;         // ★ ここでは撮らない (入力タスクを止めない)
}

static void camera_task(void *arg) {
    (void)arg;
    for (;;) {
        if (cam.key_pending) {
            cam.key_pending = false;
            stackee_camera_req_t req;
            stackee_camera_req_default(&req);
            if (stackee_camera_capture(&req) == ESP_OK) {
                // ★ 送るのは Wi-Fi が上がっていて、会話が空いているときだけ。
                //   会話の HTTP ワーカーは 1 本しかないので取り合わない。
                (void)0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ---------------------------------------------------------------------------
// console
// ---------------------------------------------------------------------------
static size_t append_info(char *buf, size_t cap, size_t at, long id) {
    stackee_camera_stats_t st = cam.st;
    int n = snprintf(buf + at, (at < cap) ? (cap - at) : 0,
                     "{\"id\":%ld,\"ok\":1,\"camera\":\"%s\",\"camera_job\":%lu,"
                     "\"camera_ms\":%lu,\"camera_w\":%d,\"camera_h\":%d,"
                     "\"camera_bytes\":%u,\"jpeg_bytes\":%u,"
                     "\"camera_hmirror\":%d,\"camera_vflip\":%d,"
                     "\"camera_warmup\":%d,\"camera_quality\":%d,"
                     "\"captures\":%lu,\"failures\":%lu,\"aldo3\":%d,"
                     "\"psram_dma\":%s,\"dma_largest\":%lu,"
                     "\"internal_free\":%lu,"
                     "\"camera_err\":\"%s\","
                     "\"t\":{\"power\":%lu,\"init\":%lu,\"warm\":%lu,"
                     "\"take\":%lu,\"jpeg\":%lu,\"stop\":%lu}}",
                     id, st.state ? st.state : "idle", (unsigned long)st.job,
                     (unsigned long)st.ms, st.w, st.h,
                     (unsigned)st.raw_bytes, (unsigned)st.jpeg_bytes,
                     st.hmirror ? 1 : 0, st.vflip ? 1 : 0,
                     st.warmup, st.quality,
                     (unsigned long)st.captures, (unsigned long)st.failures,
                     stackee_camera_power_is_on(),
                     st.psram_dma ? "true" : "false",
                     (unsigned long)st.dma_largest,
                     (unsigned long)st.internal_free, st.error,
                     (unsigned long)st.t_power_ms, (unsigned long)st.t_init_ms,
                     (unsigned long)st.t_warm_ms, (unsigned long)st.t_take_ms,
                     (unsigned long)st.t_jpeg_ms, (unsigned long)st.t_stop_ms);
    return at + ((n > 0) ? (size_t)n : 0);
}

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *src, size_t len, char *out, size_t cap) {
    size_t at = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)src[i] << 16;
        size_t have = 1;
        if (i + 1 < len) { v |= (uint32_t)src[i + 1] << 8; have = 2; }
        if (i + 2 < len) { v |= src[i + 2]; have = 3; }
        char q[4] = {
            B64[(v >> 18) & 0x3F], B64[(v >> 12) & 0x3F],
            (have > 1) ? B64[(v >> 6) & 0x3F] : '=',
            (have > 2) ? B64[v & 0x3F] : '=',
        };
        for (int k = 0; k < 4; k++) {
            if (at + 1 < cap) {
                out[at] = q[k];
            }
            at++;
        }
    }
    if (cap > 0) {
        out[(at < cap) ? at : (cap - 1)] = '\0';
    }
    return at;
}

static size_t reply_dump(long id, const char *line, char *buf, size_t cap) {
    size_t total = cam.jpeg_len;
    if (total == 0) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nobuf\"}", id);
    }
    long off = stackee_console_int(line, "off", 0);
    long n = stackee_console_int(line, "n", DUMP_MAX);
    if (off < 0 || (size_t)off >= total) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"range\"}", id);
    }
    if (n > DUMP_MAX) { n = DUMP_MAX; }
    if (n <= 0) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"badarg\"}", id);
    }
    if ((size_t)(off + n) > total) {
        n = (long)(total - (size_t)off);
    }
    int head = snprintf(buf, cap,
                        "{\"id\":%ld,\"ok\":1,\"off\":%ld,\"n\":%ld,\"total\":%u,"
                        "\"w\":%d,\"h\":%d,\"fmt\":\"jpeg\",\"b64\":\"",
                        id, off, n, (unsigned)total, cam.st.w, cam.st.h);
    if (head < 0) {
        return 0;
    }
    size_t at = (size_t)head;
    at += b64_encode(cam.jpeg + off, (size_t)n, buf + at,
                     (at < cap) ? (cap - at) : 0);
    int tail = snprintf(buf + at, (at < cap) ? (cap - at) : 0, "\"}");
    return at + ((tail > 0) ? (size_t)tail : 0);
}

static size_t camera_console(const char *cmd, const char *line, long id,
                             char *buf, size_t cap) {
    if (strcmp(cmd, "camera.capture") == 0) {
        stackee_camera_req_t req;
        stackee_camera_req_default(&req);
        char size[8];
        if (stackee_console_str(line, "size", size, sizeof(size))) {
            if (strcmp(size, "qqvga") == 0) {
                req.size = STACKEE_CAM_QQVGA;
            } else if (strcmp(size, "qvga") != 0) {
                return (size_t)snprintf(buf, cap,
                                        "{\"id\":%ld,\"error\":\"badsize\"}", id);
            }
        }
        long warm = stackee_console_int(line, "warmup", req.warmup);
        if (warm < 1 || warm > MAX_WARMUP) {
            return (size_t)snprintf(buf, cap,
                                    "{\"id\":%ld,\"error\":\"badwarmup\"}", id);
        }
        req.warmup = (int)warm;
        req.quality = (int)stackee_console_int(line, "quality", req.quality);
        req.hmirror = stackee_console_bool(line, "hmirror", req.hmirror);
        req.vflip = stackee_console_bool(line, "vflip", req.vflip);
        esp_err_t err = stackee_camera_capture(&req);
        if (err != ESP_OK) {
            return (size_t)snprintf(buf, cap,
                                    "{\"id\":%ld,\"error\":\"capture:%s\","
                                    "\"camera_err\":\"%s\",\"camera_ms\":%lu}",
                                    id, esp_err_to_name(err), cam.st.error,
                                    (unsigned long)cam.st.ms);
        }
        return append_info(buf, cap, 0, id);
    }
    if (strcmp(cmd, "camera.status") == 0) {
        return append_info(buf, cap, 0, id);
    }
    if (strcmp(cmd, "camera.power") == 0) {
        char state[8];
        if (!stackee_console_str(line, "state", state, sizeof(state)) ||
            (strcmp(state, "on") != 0 && strcmp(state, "off") != 0)) {
            return (size_t)snprintf(buf, cap,
                                    "{\"id\":%ld,\"error\":\"badstate\"}", id);
        }
        bool on = (strcmp(state, "on") == 0);
        esp_err_t err = stackee_camera_power(on);
        return (size_t)snprintf(buf, cap,
                                "{\"id\":%ld,\"ok\":%d,\"aldo3\":%d}",
                                id, (err == ESP_OK) ? 1 : 0,
                                stackee_camera_power_is_on());
    }
    if (strcmp(cmd, "camera.dump") == 0) {
        return reply_dump(id, line, buf, cap);
    }
    return 0;
}

esp_err_t stackee_camera_init(void) {
    if (cam.lock == NULL) {
        cam.lock = xSemaphoreCreateMutex();
    }
    set_state("idle");
    stackee_camera_req_default(&cam.last_req);
    cam.st.warmup = cam.last_req.warmup;
    cam.st.quality = cam.last_req.quality;
    cam.st.hmirror = cam.last_req.hmirror;
    cam.st.vflip = cam.last_req.vflip;
    const char *path = stackee_settings_get("STACKEE_CAMERA_PATH");
    snprintf(cam.send_path, sizeof(cam.send_path), "%s",
             (path != NULL && path[0] != '\0') ? path : "/image");
    stackee_console_register(camera_console);
    if (xTaskCreatePinnedToCore(camera_task, "camera", 4096, NULL, 2, NULL, 0)
            != pdPASS) {
        ESP_LOGE(TAG, "camera タスクを作れない");
        return ESP_ERR_NO_MEM;
    }
    // ★ 起動時に ALDO3 を**切っておく**。ボード初期化が 0x90 = 0xBF を
    //   書いて入れっぱなしにするので、そのままだと撮っていない間も
    //   カメラに電気が流れ続ける。
    stackee_camera_power(false);
    ESP_LOGI(TAG, "カメラ用意 (撮るまで ALDO3 は切っておく)");
    return ESP_OK;
}
