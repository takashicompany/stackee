// Stackee ネイティブファーム 段階 0 (骨組み)。DESIGN.md §6 の表の「0」。
//
// ここで作るのは「戻れる道が通ること」を確かめるための最小限:
//   起動 → USB (CDC + HID 列挙) → LCD に起動表示 → user_fs の目録を読む
//   → CDC コンソールが hello/status に答える
// HID は列挙されるだけで何も送らない。キー処理・BLE・音声・Wi-Fi は段階 1 以降。
//
// 起動の順番には理由がある。CDC が答えるまでの時間 (目標 3 秒以内) を縮めるため、
// USB を先に立ち上げてから、時間のかかる LCD の初期化 (約 490 ms のウェイト) と
// FAT のマウントを行う。
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "stackee_assets.h"
#include "stackee_audio.h"
#include "stackee_board.h"
#include "stackee_http.h"
#include "stackee_settings.h"
#include "stackee_talksm.h"
#include "stackee_volume.h"
#include "stackee_wifi.h"
#include "stackee_console.h"
#include "stackee_ble.h"
#include "stackee_hid_dest.h"
#include "stackee_hid_out.h"
#include "stackee_input.h"
#include "stackee_nvs.h"
#include "stackee_lcd.h"
#include "stackee_logbuf.h"
#include "stackee_cryptocheck.h"
#include <string.h>
#include "stackee_perf.h"
#include "stackee_ui.h"
#include "stackee_usb.h"
#include "stackee_camera.h"
#include "stackee_touch.h"
#include "stackee_uac.h"

static const char *TAG = "stackee";

// ★ 内蔵 RAM の「いちばん大きい空き塊」を要所で残す。カメラの DMA が
//   連続した内蔵 RAM を要求するので、**空き合計ではなくこの数字**で
//   撮れるかどうかが決まる (段階 4 で踏んだ。README §17-2)。
static void log_internal(const char *when) {
    ESP_LOGI(TAG, "内蔵RAM [%s] 空き %u B / 最大の塊 %u B / DMA の塊 %u B",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

#define STATUS_BAR_H  28

static void draw_boot_screen(void) {
    if (!stackee_lcd_ready()) {
        return;
    }
    // 画面全体を白 (顔の素材が白地前提。段階 2 でそのまま使う)。
    stackee_lcd_fill(0, 0, STACKEE_LCD_WIDTH, STACKEE_LCD_HEIGHT, STACKEE_LCD_WHITE);
    // 上段のステータスバーは黒地 (顔とバーが出るまでの 1.8 秒間、これだけ)。
    // ★ 文字 ("phase 2" / "stackee-idf") は出さない (2026-09-17 ユーザー指示)。
    stackee_lcd_fill(0, 0, STACKEE_LCD_WIDTH, STATUS_BAR_H, STACKEE_LCD_BLACK);
    stackee_lcd_flush();
}

// ---------------------------------------------------------------------------
// /settings.toml (FAT の根っこ)。会話の相手とトークンはここから読む。
// ---------------------------------------------------------------------------
// ★ **値は 1 文字もログに出さない。** 出してよいのは「入っているか」だけ。
static char s_talk_path[STACKEE_TALK_PATH_MAX] = "/talk";

static const char *talk_path(void) {
    return s_talk_path;
}

static void load_settings(void) {
    size_t len = 0;
    char *text = stackee_assets_read_root("settings.toml", &len);
    if (text == NULL) {
        ESP_LOGW(TAG, "settings.toml が読めない。会話は使えない");
        return;
    }
    int keys = stackee_settings_load_text(text);
    free(text);
    ESP_LOGI(TAG, "settings.toml: %d キー", keys);

    const char *url = stackee_settings_get("STACKEE_TALK_URL");
    if (url == NULL || url[0] == '\0') {
        ESP_LOGW(TAG, "STACKEE_TALK_URL が無い。会話は使えない");
        return;
    }
    char base[192];
    if (!stackee_talk_split_url(url, base, sizeof(base),
                                s_talk_path, sizeof(s_talk_path))) {
        ESP_LOGE(TAG, "STACKEE_TALK_URL の形が不正");
        s_talk_path[0] = '\0';
        return;
    }
    const char *token = stackee_settings_get("STACKEE_TALK_TOKEN");
    if (stackee_http_configure(base, token) != ESP_OK) {
        s_talk_path[0] = '\0';
    }
}

void app_main(void) {
    int64_t t0 = esp_timer_get_time();
    // ★ いちばん最初。ここから先のログを全部溜める (console の log.tail で
    //   あとから読める)。CDC がまだ無くても溜まる。
    stackee_logbuf_install();
    stackee_perf_init();
    stackee_console_init();
    stackee_uac_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // ★ 消さない。nvs には CircuitPython 版の設定と BLE のボンドが入って
        //   いる可能性がある。読めないなら読めないまま続ける。
        ESP_LOGW(TAG, "nvs が読めない (%s)。既定値で続ける", esp_err_to_name(err));
    }

    // 1. USB を最初に。ここから先の時間はホストから見える。
    stackee_usb_start();
    stackee_console_attach_log();
    ESP_LOGI(TAG, "起動 (USB まで %ld ms)", (long)((esp_timer_get_time() - t0) / 1000));

    // 2. 電源まわり。ここを飛ばすと LCD のバックライトが点かない。
    if (stackee_board_init() != ESP_OK) {
        ESP_LOGE(TAG, "電源まわりの初期化に失敗。画面なしで続ける");
    } else if (stackee_lcd_init() == ESP_OK) {
        draw_boot_screen();
    }
    ESP_LOGI(TAG, "画面まで %ld ms", (long)((esp_timer_get_time() - t0) / 1000));

    // 3. キー入力。TCA8418 → QMK → 送信キュー → USB。
    //    ★ 画面と素材より先に立ち上げる。キーボードとして使えるまでの時間を
    //      縮めるため (DESIGN.md §2 の起動時間の行)。
    //    送信先の既定は BLE (現行 CircuitPython 版と同じ)。前回 USB を
    //    選んでいれば NVS から復元する。
    static const stackee_hid_dest_io_t dest_io = {
        .usb_connected = stackee_usb_mounted,
        .load = stackee_nvs_load_hid_dest,
        .save = stackee_nvs_save_hid_dest,
    };
    stackee_hid_dest_init(&dest_io);
    stackee_input_start();
    stackee_hid_out_start();
    ESP_LOGI(TAG, "キー入力まで %ld ms (送信先 %s)",
             (long)((esp_timer_get_time() - t0) / 1000),
             stackee_hid_dest_name(stackee_hid_dest_selected()));

    // 4. BLE HID。★ キー入力より後に立ち上げる。コントローラの初期化に
    //    時間がかかるので、先にやるとキーボードとして使えるまでが遅くなる。
    //    繋がるまでの間の打鍵は送信キューで捨てられるだけで、入力は止まらない。
    if (stackee_ble_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE を立ち上げられない。USB だけで続ける");
    }
    ESP_LOGI(TAG, "BLE まで %ld ms", (long)((esp_timer_get_time() - t0) / 1000));
    log_internal("BLE のあと");

    // 5. 素材。読めなくても止まらない。
    stackee_assets_mount();
    const stackee_assets_info_t *assets = stackee_assets_info();
    if (assets->manifest_ok) {
        ESP_LOGI(TAG, "素材: size=%d faces=%d", assets->size, assets->faces);
    } else {
        ESP_LOGE(TAG, "素材を読めない: %s", assets->error);
    }
    // 6. 設定 (/settings.toml) と音量。★ 画面より**先**に読む。ui は起動時に
    //    音量をバーへ描くので、後にすると一瞬 0% が出る。
    //    ★ 値 (トークン・パスワード) はログに出さない。
    load_settings();
    stackee_volume_init();

    // 7. 画面。素材を読んで ui タスク (CPU0 / 中優先度) を起こす。
    //    ★ キー入力・BLE より後。顔の素材 (展開後 900 KB) を読むあいだ
    //      キーボードとして使えない時間を作らないため。
    if (stackee_ui_start() != ESP_OK) {
        ESP_LOGE(TAG, "画面を出せない。キーボードとしては動く");
    }
    ESP_LOGI(TAG, "顔とバーまで %ld ms", (long)((esp_timer_get_time() - t0) / 1000));

    // 8. Wi-Fi。自動接続は net タスクが 2 秒後から 1 周 1 段で進める。
    //    ★ 画面より後。起動から「キーボードとして使えるまで」を延ばさない。
    if (stackee_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi を立ち上げられない。会話は使えない");
    }
    log_internal("Wi-Fi のあと");

    // 8b. タッチパッド (段階 4)。★ 画面より後・Wi-Fi と同じ並び。
    //    居なければ present=false のまま先へ進む (キーボードは動く)。
    stackee_touch_start();

    // 9. 通信ワーカーと音。
    stackee_http_start();
    if (stackee_audio_start(talk_path()) != ESP_OK) {
        ESP_LOGE(TAG, "音を立ち上げられない。キーボードとしては動く");
    }
    ESP_LOGI(TAG, "音と通信まで %ld ms", (long)((esp_timer_get_time() - t0) / 1000));

    // 9b. カメラ (段階 4)。★ ここでは撮らない。ALDO3 を切って待つだけ。
    stackee_camera_init();
    log_internal("音と通信のあと");

    // ★ 内蔵 RAM の残りをここで 1 回残す。TLS の入出力バッファが取れるかは
    //   この数字で決まる (PSRAM 込みの heap_free では分からない)。
    ESP_LOGI(TAG, "起動おわり (%ld ms) 内蔵RAM 空き %u B / 最大の塊 %u B / DMA %u B / PSRAM %u B",
             (long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // 10. メインループ。キーは専用タスク (CPU1・最優先) が見ているので、
    //    ここはコンソールの面倒を見るだけ。
    int64_t last = esp_timer_get_time();
    int64_t broken_seen = 0;
    for (;;) {
        stackee_console_poll();
        int64_t now = esp_timer_get_time();
        stackee_perf_sample(STACKEE_PERF_MAIN, (uint32_t)(now - last));
        last = now;
        // ★ 暗号の自己診断は定期には回さない (ユーザー: 動作が重くなることはしない)。
        //   走るのは HTTPS が TLS で失敗したときだけ (stackee_http.c)。README §20。
        // 壊れていたら、会話が終わって 5 秒たったところで再起動して戻す。
        if (stackee_cryptocheck_broken()) {
            if (strcmp(stackee_audio_talk_state(), "idle") != 0) {
                broken_seen = 0;
            } else if (broken_seen == 0) {
                broken_seen = now;
            } else if (now - broken_seen >= 5 * 1000000LL) {
                ESP_LOGW(TAG, "★ 暗号の自己診断が NG のまま。会話の合間に再起動する (README §20)");
                vTaskDelay(pdMS_TO_TICKS(200));
                stackee_usb_request_restart();
            }
        }
        vTaskDelay(1);      // FREERTOS_HZ=1000 なので 1 ms
    }
}
