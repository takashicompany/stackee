#include "stackee_console.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tusb.h"

#include "stackee_assets.h"
#include "stackee_board.h"
#include "stackee_lcd.h"
#include "stackee_logbuf.h"
#include "stackee_perf.h"
#include "stackee_ble.h"
#include "stackee_hid_dest.h"
#include "stackee_input.h"
#include "stackee_jsonlite.h"
#include "stackee_nvs.h"
#include "stackee_report_queue.h"
#include "stackee_usb.h"
#include "stackee_audio.h"
#include "stackee_http.h"
#include "sdkconfig.h"
#include "stackee_cryptocheck.h"
#include "stackee_ui.h"
#include "stackee_volume.h"
#include "stackee_wifi.h"
#include "stackee_conhid.h"
#include "stackee_fat.h"
#include "stackee_settings.h"
#include "stackee_uac.h"

#define MARKER      0x1E
// ★ 512 では `fs.put` (base64 360 バイト = 480 文字 + 枠) が入り切らず、
//   黙って捨てていた (2026-09-16 full 像で発覚。応答が無いので 30 秒待ちになる)。
#define CONSOLE_LINE_MAX  1024
#define REPLY_MAX   1800

// このファームの版。CircuitPython 版は "stackee-console/2"。
// ★ 段階ごとに上げる。実機にどちらの像が載っているかが hello / status で
//    すぐ分かるようにするため (sha256 を照合しなくても分かる)。
#define STACKEE_FW  "stackee-idf/5"
#define STACKEE_PROTO 2


static char s_line[CONSOLE_LINE_MAX];
// 大きい応答 (settings.* と、拡張コマンドの ui.selftest など) の置き場。
// ★ **1 つだけ**。コマンドの処理はメインループ 1 本でしか走らないので
//   使い回してよい。段階 4 の最初の版は分岐ごとに static を持っていて、
//   内蔵 RAM の .bss を 3 個ぶん (4.8 KB) 余計に食っていた。
static char s_wide[1600];
static size_t s_len;
static bool s_in_frame;
static uint32_t s_cmds;

// ---------------------------------------------------------------------------
// 脱出路 (reset / bootloader)
// ---------------------------------------------------------------------------
// ★ 応答を返し切ってから落ちる。すぐ落とすと呼んだ側に返事が届かず、
//   「効いたのか固まったのか分からない」という一番困る壊れ方になる。
typedef enum {
    CONSOLE_ACTION_NONE = 0,
    CONSOLE_ACTION_RESET,
    CONSOLE_ACTION_ROM,
} console_action_t;

static console_action_t s_action;
static int64_t          s_action_at_us;
static uint32_t s_drops;
static int64_t s_boot_us;

// CDC への書き込みを 1 本にまとめる錠。応答は console タスクから、ログは
// どのタスクからでも来るので、TinyUSB の FIFO を同時に触らせない。
static SemaphoreHandle_t s_tx_lock;

static bool tx_take(void) {
    return s_tx_lock != NULL &&
           xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(20)) == pdTRUE;
}

static void tx_give(void) {
    if (s_tx_lock != NULL) {
        xSemaphoreGive(s_tx_lock);
    }
}

// ---------------------------------------------------------------------------
// 送信: 必ず 1 回の書き込みで 1 枠ぶん
// ---------------------------------------------------------------------------
// ★ 出口は 2 つある。
//   dev  … CDC (と、同じ内容を Raw HID にも積む。console_hid.py は
//           dev でも full でも同じように読める)
//   full … Raw HID だけ (CDC が無い)
// どちらへ流すバイト列も**同じ**なので、ホスト側の切り分け器は 1 つで済む。
static void out_bytes(const char *data, size_t len, bool is_log) {
    // ★ 応答とログで扱いが違う。ログはホストが読んでいないときに捨てる
    //   (溜めると環状バッファが起動ログで埋まり、応答が入らなくなる)。
    if (is_log) {
        stackee_conhid_write_log(data, len);
    } else {
        stackee_conhid_write(data, len);
    }
#if CFG_TUD_CDC
    if (tud_cdc_connected()) {
        tud_cdc_write(data, (uint32_t)len);
        tud_cdc_write_flush();
    }
#endif
}

static void send_frame(const char *body) {
    if (!tx_take()) {
        s_drops++;      // ログが握ったまま。応答を捨てて次へ (止まらない)
        return;
    }
    static const char marker = MARKER;
    static const char nl = '\n';
    out_bytes(&marker, 1, false);
    out_bytes(body, strlen(body), false);
    out_bytes(&nl, 1, false);
    tx_give();
}

// ---------------------------------------------------------------------------
// 受信した 1 行から id と cmd を拾う
//
// cJSON は ESP-IDF v6 の標準構成に無い。段階 0 で要るのは整数 1 つと短い
// 文字列 1 つだけなので、丸ごとの構文解析はしない。段階 4 でコマンドが
// 増えるときに、ちゃんとした解析器へ差し替える。
// ---------------------------------------------------------------------------
static const char *value_of(const char *json, const char *key) {
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *at = strstr(json, pattern);
    if (at == NULL) {
        return NULL;
    }
    at += strlen(pattern);
    while (*at == ' ' || *at == ':') {
        at++;
    }
    return at;
}

// ---- 拡張コマンド (段階 2 の画面、段階 3 の音・Wi-Fi) が使う小道具 ---------
//
// ★ 登録は**複数**受ける。段階 2 は画面 1 つだけだったが、段階 3 で
//   音 (stackee_audio) と Wi-Fi (stackee_wifi) が増えた。先に登録した
//   ものから順に聞き、0 を返したら次へ回す。
#define CONSOLE_EXT_MAX 8
static stackee_console_ext_t s_ext[CONSOLE_EXT_MAX];
static int s_ext_count;

void stackee_console_register(stackee_console_ext_t handler) {
    if (handler == NULL || s_ext_count >= CONSOLE_EXT_MAX) {
        return;
    }
    s_ext[s_ext_count++] = handler;
}

const char *stackee_console_value(const char *json, const char *key) {
    return value_of(json, key);
}

long stackee_console_int(const char *json, const char *key, long fallback) {
    const char *at = value_of(json, key);
    if (at == NULL) {
        return fallback;
    }
    char *end = NULL;
    long value = strtol(at, &end, 0);
    return (end == at) ? fallback : value;
}

bool stackee_console_bool(const char *json, const char *key, bool fallback) {
    const char *at = value_of(json, key);
    if (at == NULL) {
        return fallback;
    }
    if (strncmp(at, "true", 4) == 0)  { return true; }
    if (strncmp(at, "false", 5) == 0) { return false; }
    if (strncmp(at, "null", 4) == 0)  { return fallback; }
    return strtol(at, NULL, 0) != 0;
}

bool stackee_console_str(const char *json, const char *key, char *out, size_t cap) {
    const char *at = value_of(json, key);
    if (at == NULL || *at != '"' || cap == 0) {
        return false;
    }
    // ★ 逃がしを戻す。stackee_console_client.py は ensure_ascii=True で送るので、
    //   日本語の SSID は \uXXXX で来る。パスワードに `"` が入ることもある。
    //   閉じ引用符を探すときも `\"` を数え間違えない。
    const char *end = at + 1;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1]) {
            end++;
        }
        end++;
    }
    if (*end != '"') {
        return false;
    }
    stackee_json_unescape(at, (size_t)(end + 1 - at), out, cap);
    return true;
}

static bool parse_id(const char *json, long *out) {
    const char *at = value_of(json, "id");
    if (at == NULL) {
        return false;
    }
    char *end = NULL;
    long value = strtol(at, &end, 10);
    if (end == at) {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_cmd(const char *json, char *out, size_t cap) {
    const char *at = value_of(json, "cmd");
    if (at == NULL || *at != '"') {
        return false;
    }
    at++;
    size_t i = 0;
    while (*at && *at != '"' && i + 1 < cap) {
        out[i++] = *at++;
    }
    out[i] = '\0';
    return i > 0;
}

// ---------------------------------------------------------------------------
// 応答の組み立て
// ---------------------------------------------------------------------------
static size_t append(char *buf, size_t cap, size_t at, const char *fmt, ...) {
    if (at >= cap) {
        return at;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + at, cap - at, fmt, args);
    va_end(args);
    if (n < 0) {
        return at;
    }
    return at + (size_t)n;
}

// 画面の 1 行 (返答文)。任意の日本語が入るので JSON として逃がし、
// 1 枠に収まるよう 200 バイトで切る (多バイト文字の途中では切らない)。
#define SCREEN_BYTES 200

static size_t append_screen(char *buf, size_t cap, size_t at, const char *text) {
    at = append(buf, cap, at, ",\"screen\":\"");
    if (text == NULL) {
        return append(buf, cap, at, "\"");
    }
    size_t len = strlen(text);
    if (len > SCREEN_BYTES) {
        len = SCREEN_BYTES;
        // UTF-8 の続きバイト (10xxxxxx) の上で切らない。
        while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) {
            len--;
        }
    }
    for (size_t i = 0; i < len && at + 8 < cap; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            at = append(buf, cap, at, "\\%c", c);
        } else if (c < 0x20) {
            at = append(buf, cap, at, "\\u%04X", c);
        } else {
            at = append(buf, cap, at, "%c", c);
        }
    }
    return append(buf, cap, at, "\"");
}

// 任意の文字列を JSON の文字列として書く (長さの上限なし。切れるのは
// 呼び手の buf が尽きたとき)。settings.raw の全文などに使う。
static size_t append_json_string(char *buf, size_t cap, size_t at,
                                 const char *text) {
    at = append(buf, cap, at, "\"");
    for (const char *p = text; p != NULL && *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            at = append(buf, cap, at, "\\%c", c);
        } else if (c == '\n') {
            at = append(buf, cap, at, "\\n");
        } else if (c == '\r') {
            at = append(buf, cap, at, "\\r");
        } else if (c == '\t') {
            at = append(buf, cap, at, "\\t");
        } else if (c < 0x20 || c == 0x7F) {
            at = append(buf, cap, at, "\\u%04X", c);
        } else {
            at = append(buf, cap, at, "%c", c);
        }
    }
    return append(buf, cap, at, "\"");
}

static size_t append_perf(char *buf, size_t cap, size_t at) {
    at = append(buf, cap, at, ",\"perf\":{");
    bool first = true;
    for (int ch = 0; ch < STACKEE_PERF_CHANNELS; ch++) {
        stackee_perf_stats_t st;
        stackee_perf_stats((stackee_perf_channel_t)ch, &st);
        if (st.count == 0) {
            continue;       // まだ使っていないチャンネルは出さない
        }
        at = append(buf, cap, at,
                    "%s\"%s\":{\"n\":%lu,\"max_us\":%lu,\"med_us\":%lu}",
                    first ? "" : ",", stackee_perf_name((stackee_perf_channel_t)ch),
                    (unsigned long)st.count, (unsigned long)st.max_us,
                    (unsigned long)st.median_us);
        first = false;
    }
    return append(buf, cap, at, "}");
}

static void reply_hello(long id) {
    char buf[REPLY_MAX];
    size_t at = 0;
    const esp_app_desc_t *app = esp_app_get_description();
    at = append(buf, sizeof(buf), at,
                "{\"id\":%ld,\"proto\":%d,\"fw\":\"%s\",\"idf\":\"%s\","
                "\"app\":\"%s\",\"board\":\"esp32s3\","
                "\"features\":[\"hello\",\"status\",\"reset\",\"bootloader\","
                "\"hid.switch\",\"hid.set\",\"ble.refresh\",\"ble.clear_bonds\","
                "\"ble.drop_cccd\",\"ble.svc_changed\",\"log.tail\",\"key.inject\","
                "\"lcd.crc\",\"lcd.dump\",\"face.set\",\"face.auto\",\"bar.set\",\"bar.auto\","
                "\"ui.selftest\",\"ui.status\",\"ui.assets\",\"ui.subtitle\","
                "\"wifi.scan\",\"wifi.list\",\"wifi.add\",\"wifi.remove\","
                "\"wifi.connect\",\"wifi.status\",\"wifi.off\",\"wifi.on\","
                "\"audio.selftest\",\"audio.null\",\"audio.play\",\"audio.status\","
                "\"talk.inject\",\"talk.status\","
                "\"settings.get\",\"settings.raw\",\"settings.set\","
                "\"fs.put\",\"bench\",\"log.burst\","
                "\"lcd.status\",\"lcd.full\",\"usb.status\",\"axp.read\",\"axp.write\",\"crypto.selftest\","
                "\"touch.status\",\"touch.inject\",\"touch.scroll\","
                "\"camera.capture\",\"camera.power\",\"camera.dump\","
                "\"camera.status\",\"camera.look\",\"camera.look_status\","
                "\"app.info\",\"app.boot_factory\",\"ota.begin\",\"ota.status\","
                "\"ota.end\",\"ota.commit\",\"ota.abort\"],"
                "\"profile\":\"%s\",\"cdc\":%s}",
                id, STACKEE_PROTO, STACKEE_FW, IDF_VER,
                app ? app->version : "?", stackee_usb_profile(),
                stackee_usb_has_cdc() ? "true" : "false");
    (void)at;
    send_frame(buf);
}

static void reply_crypto_selftest(long id) {
    char buf[REPLY_MAX];
    stackee_cryptocheck_result_t r;
    stackee_cryptocheck_run(&r);
    stackee_cryptocheck_stats_t st;
    stackee_cryptocheck_stats(&st);
    snprintf(buf, sizeof(buf),
             "{\"id\":%ld,\"ok\":%d,\"sha_abc\":%d,\"sha_internal_4k\":%d,\"sha_psram_4k\":%d,"
             "\"x509\":{\"parse\":[%d,%d],\"rc\":%d,\"flags\":%lu,\"sig_ok\":%d},"
             "\"hw_sha\":%d,\"hw_mpi\":%d,\"runs\":%lu,\"fails\":%lu,\"broken\":%s}",
             id, r.ok ? 1 : 0, r.sha_abc, r.sha_internal_4k, r.sha_psram_4k,
             r.x509_parse1, r.x509_parse2, r.x509_rc, (unsigned long)r.x509_flags, r.x509_sig_ok,
#ifdef CONFIG_MBEDTLS_HARDWARE_SHA
             1,
#else
             0,
#endif
#ifdef CONFIG_MBEDTLS_HARDWARE_MPI
             1,
#else
             0,
#endif
             (unsigned long)st.runs, (unsigned long)st.fails, st.broken ? "true" : "false");
    send_frame(buf);
}

static void reply_status(long id) {
    char buf[REPLY_MAX];
    size_t at = 0;
    int bat = stackee_board_battery_percent();
    uint32_t i2c_fail = 0, i2c_recovered = 0;
    stackee_board_i2c_stats(&i2c_fail, &i2c_recovered);
    uint32_t transfers = 0, rows = 0, worker_ms = 0;
    stackee_lcd_stats(&transfers, &rows, &worker_ms);
    const stackee_assets_info_t *assets = stackee_assets_info();
    // ★ 浮動小数の書式は使わない。picolibc の printf が %f を持たない構成が
    //   あるため、0.1 秒きざみの整数 2 つで組み立てる。
    long up_ds = (long)((esp_timer_get_time() - s_boot_us) / 100000);

    at = append(buf, sizeof(buf), at,
                "{\"id\":%ld,\"fw\":\"%s\",\"up\":%ld.%ld,\"rst\":%d,\"cmds\":%lu,\"drops\":%lu",
                id, STACKEE_FW, up_ds / 10, up_ds % 10,
                (int)esp_reset_reason(),      // esp_reset_reason_t (3=SW 4=PANIC 5=INT_WDT 6=TASK_WDT 9=BROWNOUT 1=POWERON)
                (unsigned long)s_cmds, (unsigned long)s_drops);
    at = append(buf, sizeof(buf), at, ",\"i2c\":{\"fail\":%lu,\"recovered\":%lu}",
                (unsigned long)i2c_fail, (unsigned long)i2c_recovered);
    at = append(buf, sizeof(buf), at, ",\"bat_mv\":%d", stackee_board_battery_mv());
    {
        stackee_cryptocheck_stats_t cs;
        stackee_cryptocheck_stats(&cs);
        at = append(buf, sizeof(buf), at,
                    ",\"crypto\":{\"runs\":%lu,\"fails\":%lu,\"broken\":%s}",
                    (unsigned long)cs.runs, (unsigned long)cs.fails, cs.broken ? "true" : "false");
    }
    {
        int pk_long = 0, pk_short = 0, pk_boot = 0;
        stackee_ui_pwrkey_stats(&pk_long, &pk_short, &pk_boot);
        at = append(buf, sizeof(buf), at,
                    ",\"pwrkey\":{\"long\":%d,\"short\":%d,\"boot_irq\":%d}",
                    pk_long, pk_short, pk_boot);
    }
    stackee_input_stats_t input;
    stackee_input_stats(&input);
    stackee_report_stats_t queue;
    stackee_report_queue_stats(&queue);
    stackee_ble_stats_t ble;
    stackee_ble_stats(&ble);
    // ★ キー名と値は現行 stackee_console.py に合わせる ("BLE" / "USB")。
    //   hid = いま実際に出している先、hid_sel = 選ばれている先 (USB を選んで
    //   いてもケーブルが無ければ hid は BLE になる)。
    at = append(buf, sizeof(buf), at,
                ",\"hid\":\"%s\",\"hid_sel\":\"%s\",\"ble\":%s",
                stackee_hid_dest_name(stackee_hid_dest_effective()),
                stackee_hid_dest_name(stackee_hid_dest_selected()),
                ble.connected ? "true" : "false");
    if (ble.interval_ms < 0) {
        at = append(buf, sizeof(buf), at, ",\"ble_interval_ms\":null");
    } else {
        at = append(buf, sizeof(buf), at, ",\"ble_interval_ms\":%d",
                    ble.interval_ms);
    }
    at = append(buf, sizeof(buf), at,
                ",\"blex\":{\"started\":%s,\"ready\":%s,\"adv\":%s,"
                "\"err\":\"%s\",\"err_code\":%d,"
                "\"conn\":%lu,\"disc\":%lu,\"sent\":%lu,\"failed\":%lu,"
                "\"adv_starts\":%lu,\"adv_fails\":%lu,\"adv_revived\":%lu,"
                "\"svc_changed_handle\":%u,\"svc_changed_sent\":%lu,"
                "\"svc_changed_acked\":%lu,\"svc_changed_rc\":%d,"
                "\"adv_kind\":\"%s\",\"adv_directed\":%lu,\"bond_peer\":%s}",
                ble.started ? "true" : "false",
                ble.ready ? "true" : "false",
                ble.advertising ? "true" : "false",
                ble.err ? ble.err : "", ble.err_code,
                (unsigned long)ble.connects, (unsigned long)ble.disconnects,
                (unsigned long)ble.sent, (unsigned long)ble.failed,
                (unsigned long)ble.adv_starts, (unsigned long)ble.adv_fails,
                (unsigned long)ble.adv_revived, ble.svc_changed_handle,
                (unsigned long)ble.svc_changed_sent,
                (unsigned long)ble.svc_changed_acked, ble.svc_changed_rc,
                ble.adv_kind ? ble.adv_kind : "?",
                (unsigned long)ble.adv_directed,
                ble.have_bond_peer ? "true" : "false");
    at = append(buf, sizeof(buf), at,
                ",\"keys\":{\"tca\":%s,\"events\":%lu,\"down\":%u,"
                "\"ovf\":%lu,\"iofail\":%lu,\"stray\":%lu,\"custom\":%lu}",
                input.tca_connected ? "true" : "false",
                (unsigned long)input.key_events, input.keys_down,
                (unsigned long)input.overflows, (unsigned long)input.io_fails,
                (unsigned long)input.stray, (unsigned long)input.custom_keys);
    at = append(buf, sizeof(buf), at,
                ",\"hidq\":{\"pushed\":%lu,\"usb\":%lu,\"ble\":%lu,"
                "\"dropped\":%lu,\"failed\":%lu,\"depth\":%u}",
                (unsigned long)queue.pushed, (unsigned long)queue.sent_usb,
                (unsigned long)queue.sent_ble, (unsigned long)queue.dropped,
                (unsigned long)queue.send_failed, queue.depth);
    if (bat < 0) {
        at = append(buf, sizeof(buf), at, ",\"bat\":null");
    } else {
        at = append(buf, sizeof(buf), at, ",\"bat\":%d", bat);
    }
    int chg = stackee_board_charging();
    at = append(buf, sizeof(buf), at, ",\"chg\":%s",
                chg < 0 ? "null" : (chg ? "true" : "false"));
    // Wi-Fi。★ キー名は現行 stackee_console.py と同じ
    //   ("wifi" = up/on/off、"wifi_state" = 状態機械の名前)。
    at = append(buf, sizeof(buf), at,
                ",\"wifi\":\"%s\",\"wifi_state\":\"%s\",\"ssid\":\"%s\","
                "\"ip\":\"%s\",\"nets\":%d,\"connect_ms\":%lu,\"wifi_up_ms\":%lu",
                stackee_wifi_connected() ? "up" : "off",
                stackee_wifi_state_name(), stackee_wifi_ssid(), stackee_wifi_ip(),
                stackee_wifi_net_count(),
                (unsigned long)stackee_wifi_connect_ms(),
                (unsigned long)stackee_wifi_up_ms());
    at = append(buf, sizeof(buf), at,
                ",\"volume\":%d,\"volume_save_pending\":%s,\"volume_src\":\"%s\"",
                stackee_volume_percent(),
                stackee_volume_save_pending() ? "true" : "false",
                stackee_volume_source());
    at = append(buf, sizeof(buf), at,
                ",\"talk\":\"%s\",\"audio_null\":%s,\"audio_busy\":%s,"
                "\"talk_url\":%s,\"talk_token\":%s",
                stackee_audio_talk_state(),
                stackee_audio_null() ? "true" : "false",
                stackee_audio_busy() ? "true" : "false",
                stackee_http_configured() ? "true" : "false",
                stackee_http_has_token() ? "true" : "false");
    at = append_screen(buf, sizeof(buf), at, stackee_ui_screen());
    // ★ heap_free は PSRAM 込み。TLS が確保できるかを決めるのは**内蔵 RAM**
    //   なので、そちらを別に出す (2026-09-16: mbedtls_ssl_setup が
    //   -0x008D = ALLOC_FAILED で落ちたときに、この数字が無くて切り分けに
    //   手間取った)。heap_dma は DMA に使える内蔵 RAM。
    at = append(buf, sizeof(buf), at,
                ",\"heap_free\":%lu,\"heap_min\":%lu,\"psram_free\":%lu,"
                "\"heap_internal\":%lu,\"heap_internal_min\":%lu,"
                "\"heap_internal_largest\":%lu,\"heap_dma\":%lu",
                (unsigned long)esp_get_free_heap_size(),
                (unsigned long)esp_get_minimum_free_heap_size(),
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA));
    at = append(buf, sizeof(buf), at,
                ",\"lcd\":{\"ready\":%s,\"transfers\":%lu,\"rows\":%lu,\"worker_ms\":%lu}",
                stackee_lcd_ready() ? "true" : "false",
                (unsigned long)transfers, (unsigned long)rows,
                (unsigned long)worker_ms);
    at = append(buf, sizeof(buf), at,
                ",\"assets\":{\"mounted\":%s,\"manifest\":%s,\"size\":%d,\"faces\":%d",
                assets->mounted ? "true" : "false",
                assets->manifest_ok ? "true" : "false",
                assets->size, assets->faces);
    if (assets->error[0]) {
        at = append(buf, sizeof(buf), at, ",\"error\":\"%s\"", assets->error);
    }
    at = append(buf, sizeof(buf), at, "}");
    at = append_perf(buf, sizeof(buf), at);
    at = append(buf, sizeof(buf), at, "}");
    if (at >= sizeof(buf)) {
        s_drops++;          // 入り切らなかった。次から短くする合図
        return;
    }
    send_frame(buf);
}

// ログの末尾を返す。`{"cmd":"log.tail","bytes":800}` / 既定 600 バイト。
//
// ★ 1 枠に収まる量しか返さない。全部読みたいときは bytes を変えながら
//   何度か呼ぶ (溜めているのは 16 KB)。
static void reply_log_tail(long id, const char *request) {
    long want = 0;
    const char *at = value_of(request, "bytes");
    if (at != NULL) {
        want = strtol(at, NULL, 10);
    }
    if (want <= 0 || want > 600) {
        want = 600;
    }
    // back = 末尾から何バイト手前で終わるか。古い分を順に読むときに使う。
    long back = 0;
    at = value_of(request, "back");
    if (at != NULL) {
        back = strtol(at, NULL, 10);
        if (back < 0) { back = 0; }
    }

    static char raw[600];
    size_t got = stackee_logbuf_read_back(raw, sizeof(raw), (size_t)want, (size_t)back);
    stackee_logbuf_stats_t stats;
    stackee_logbuf_stats(&stats);

    char   buf[REPLY_MAX];
    size_t pos = 0;
    pos = append(buf, sizeof(buf), pos,
                 "{\"id\":%ld,\"held\":%u,\"written\":%lu,\"dropped\":%lu,"
                 "\"text\":\"",
                 id, stats.held, (unsigned long)stats.written,
                 (unsigned long)stats.dropped);
    // JSON の文字列として出せるように逃がす。制御文字は \uXXXX。
    for (size_t i = 0; i < got && pos + 8 < sizeof(buf) - 4; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (c == '"' || c == '\\') {
            pos = append(buf, sizeof(buf), pos, "\\%c", c);
        } else if (c == '\n') {
            pos = append(buf, sizeof(buf), pos, "\\n");
        } else if (c == '\r') {
            pos = append(buf, sizeof(buf), pos, "\\r");
        } else if (c < 0x20 || c == 0x7F) {
            pos = append(buf, sizeof(buf), pos, "\\u%04X", c);
        } else {
            pos = append(buf, sizeof(buf), pos, "%c", c);
        }
    }
    pos = append(buf, sizeof(buf), pos, "\"}");
    if (pos >= sizeof(buf)) {
        s_drops++;
        send_frame("{\"id\":null,\"error\":\"toolong\"}");
        return;
    }
    send_frame(buf);
}

// ---------------------------------------------------------------------------
// 打鍵の注入 (人手ゼロの検証用)
// ---------------------------------------------------------------------------
// `{"cmd":"key.inject"}`                       既定 (F24 を 30 ms)
// `{"cmd":"key.inject","kc":"F13","hold_ms":50}`
// `{"cmd":"key.inject","kc":115}`              数値でも指定できる
// `{"cmd":"key.inject","kc":32264,"hold_ms":1500,"wait":false}`
//                                              押し始めてすぐ返る (最大 3000 ms)
//
// ★ 既定を F24 にしてあるのは、**ホスト側で何も起きないキー**だから。
//   検証のたびにエディタへ文字が入ったりしない。
static const struct { const char *name; uint16_t code; } KEY_NAMES[] = {
    {"F13", 0x0068}, {"F14", 0x0069}, {"F15", 0x006A}, {"F16", 0x006B},
    {"F17", 0x006C}, {"F18", 0x006D}, {"F19", 0x006E}, {"F20", 0x006F},
    {"F21", 0x0070}, {"F22", 0x0071}, {"F23", 0x0072}, {"F24", 0x0073},
    {"LANG1", 0x0090}, {"LANG2", 0x0091},
};

static bool lookup_keycode(const char *request, uint16_t *out) {
    const char *at = value_of(request, "kc");
    if (at == NULL) {
        *out = 0x0073;      // F24
        return true;
    }
    if (*at == '"') {
        at++;
        for (size_t i = 0; i < sizeof(KEY_NAMES) / sizeof(KEY_NAMES[0]); i++) {
            size_t len = strlen(KEY_NAMES[i].name);
            if (strncmp(at, KEY_NAMES[i].name, len) == 0 && at[len] == '"') {
                *out = KEY_NAMES[i].code;
                return true;
            }
        }
        return false;
    }
    char *end = NULL;
    long value = strtol(at, &end, 0);
    if (end == at || value <= 0 || value > 0xFFFF) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static void reply_key_inject(long id, const char *request) {
    uint16_t keycode = 0;
    if (!lookup_keycode(request, &keycode)) {
        char err[96];
        snprintf(err, sizeof(err),
                 "{\"id\":%ld,\"error\":\"badkeycode\"}", id);
        send_frame(err);
        return;
    }
    long hold_ms = 30;
    const char *at = value_of(request, "hold_ms");
    if (at != NULL) {
        hold_ms = strtol(at, NULL, 10);
    }

    // ★ `"wait":false` … 押し始めて **すぐ返る**。押している最中に
    //   `ui.status` や `lcd.crc` を読みたいとき (MIC(kc) の表情の確認)
    //   に使う。遅延は返らないので、遅延を測るときは既定のまま。
    if (!stackee_console_bool(request, "wait", true)) {
        bool started = stackee_input_inject_begin((uint16_t)keycode,
                                                  (uint32_t)hold_ms);
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"id\":%ld,\"ok\":%s,\"started\":%s,\"kc\":%u,"
                 "\"hold_ms\":%ld,\"wait\":false}",
                 id, started ? "1" : "0", started ? "true" : "false",
                 keycode, hold_ms);
        send_frame(buf);
        return;
    }

    stackee_inject_result_t result;
    bool ok = stackee_input_inject(keycode, (uint32_t)hold_ms, &result);

    char   buf[REPLY_MAX];
    size_t pos = 0;
    pos = append(buf, sizeof(buf), pos,
                 "{\"id\":%ld,\"ok\":%s,\"kc\":%u,\"hold_ms\":%ld,"
                 "\"dest\":\"%s\"",
                 id, ok ? "1" : "0", result.keycode, hold_ms, result.dest);
    // 注入から HID 送出までの遅れ [ms]。0.001 ms きざみで整数 2 つに割る。
    pos = append(buf, sizeof(buf), pos,
                 ",\"press_ms\":%lu.%03lu,\"release_ms\":%lu.%03lu",
                 (unsigned long)(result.press_us / 1000),
                 (unsigned long)(result.press_us % 1000),
                 (unsigned long)(result.release_us / 1000),
                 (unsigned long)(result.release_us % 1000));
    pos = append(buf, sizeof(buf), pos,
                 ",\"pushed\":%lu,\"sent_usb\":%lu,\"sent_ble\":%lu}",
                 (unsigned long)result.pushed, (unsigned long)result.sent_usb,
                 (unsigned long)result.sent_ble);
    if (pos >= sizeof(buf)) {
        s_drops++;
        return;
    }
    send_frame(buf);
}


// ---------------------------------------------------------------------------
// 段階 4: /settings.toml の読み書き
// ---------------------------------------------------------------------------
// ★ **値は絶対に外へ出さない**キーがある (パスワード・トークン)。
//   settings.get はそれを true / false に置き換え、settings.raw は
//   行ごと "***" に置き換える (現行 stackee_console.py と同じ)。
#define SETTINGS_TEXT_MAX 3072

static char *read_settings_text(size_t *out_len) {
    size_t len = 0;
    char *text = stackee_assets_read_root("settings.toml", &len);
    if (out_len != NULL) {
        *out_len = (text != NULL) ? len : 0;
    }
    return text;
}

static size_t reply_settings_get(long id, char *buf, size_t cap) {
    size_t len = 0;
    char *text = read_settings_text(&len);
    if (text == NULL) {
        return (size_t)snprintf(buf, cap,
                                "{\"id\":%ld,\"keys\":{},\"missing\":1}", id);
    }
    stackee_settings_load_text(text);
    size_t at = append(buf, cap, 0, "{\"id\":%ld,\"keys\":{", id);
    const char *const *keys = stackee_settings_report_keys();
    bool first = true;
    for (int i = 0; keys[i] != NULL; i++) {
        const char *value = stackee_settings_get(keys[i]);
        if (value == NULL) {
            continue;       // その設定が無ければ出さない (現行と同じ)
        }
        if (stackee_settings_key_secret(keys[i])) {
            at = append(buf, cap, at, "%s\"%s\":%s", first ? "" : ",", keys[i],
                        (value[0] != '\0') ? "true" : "false");
        } else {
            at = append(buf, cap, at, "%s\"%s\":", first ? "" : ",", keys[i]);
            at = append_json_string(buf, cap, at, value);
        }
        first = false;
    }
    at = append(buf, cap, at, "},\"secret\":[");
    const char *const *secret = stackee_settings_secret_keys();
    for (int i = 0; secret[i] != NULL; i++) {
        at = append(buf, cap, at, "%s\"%s\"", (i == 0) ? "" : ",", secret[i]);
    }
    at = append(buf, cap, at, "],\"bytes\":%u,\"allowed\":[", (unsigned)len);
    // ★ ページが「書ける欄」を出すために要る (現行は固定表を持っていたが、
    //   C 版は会話の相手 (STACKEE_TALK_URL) も書けるので数が違う)。
    int shown = 0;
    for (int i = 0; keys[i] != NULL; i++) {
        if (!stackee_settings_key_allowed(keys[i])) {
            continue;
        }
        at = append(buf, cap, at, "%s\"%s\"", (shown++ == 0) ? "" : ",", keys[i]);
    }
    at = append(buf, cap, at, "]}");
    free(text);
    return at;
}

static size_t reply_settings_raw(long id, char *buf, size_t cap) {
    size_t len = 0;
    char *text = read_settings_text(&len);
    if (text == NULL) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"read\"}", id);
    }
    char *masked = malloc(SETTINGS_TEXT_MAX);
    if (masked == NULL) {
        free(text);
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nomem\"}", id);
    }
    size_t need = stackee_settings_mask(text, masked, SETTINGS_TEXT_MAX);
    size_t at = append(buf, cap, 0, "{\"id\":%ld,\"bytes\":%u,\"text\":",
                       id, (unsigned)len);
    at = append_json_string(buf, cap, at, masked);
    at = append(buf, cap, at, ",\"truncated\":%s}",
                (need >= SETTINGS_TEXT_MAX) ? "true" : "false");
    free(masked);
    free(text);
    return at;
}

// `{"cmd":"settings.set","kv":{"STACKEE_HOST":"192.168.0.2","STACKEE_PORT":"8080"}}`
// 値を null にするとその行を消す。
#define SET_MAX_KEYS 6

static size_t reply_settings_set(long id, const char *line, char *buf, size_t cap) {
    const char *at = stackee_console_value(line, "kv");
    if (at == NULL || *at != '{') {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nokv\"}", id);
    }
    static char keys[SET_MAX_KEYS][STACKEE_SETTINGS_KEY_MAX];
    static char vals[SET_MAX_KEYS][STACKEE_SETTINGS_VALUE_MAX];
    const char *kp[SET_MAX_KEYS];
    const char *vp[SET_MAX_KEYS];
    int n = 0;

    const char *p = at + 1;
    while (*p && *p != '}') {
        while (*p == ' ' || *p == ',') { p++; }
        if (*p != '"') { break; }
        const char *kend = p + 1;
        while (*kend && *kend != '"') { kend++; }
        if (*kend != '"') { break; }
        size_t klen = (size_t)(kend - p - 1);
        if (n >= SET_MAX_KEYS || klen == 0 || klen >= STACKEE_SETTINGS_KEY_MAX) {
            return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nokv\"}", id);
        }
        memcpy(keys[n], p + 1, klen);
        keys[n][klen] = '\0';
        p = kend + 1;
        while (*p == ' ' || *p == ':') { p++; }
        if (strncmp(p, "null", 4) == 0) {
            vp[n] = NULL;
            p += 4;
        } else if (*p == '"') {
            const char *vend = p + 1;
            while (*vend && *vend != '"') {
                if (*vend == '\\' && vend[1]) { vend++; }
                vend++;
            }
            if (*vend != '"') { break; }
            stackee_json_unescape(p, (size_t)(vend + 1 - p), vals[n],
                                  STACKEE_SETTINGS_VALUE_MAX);
            vp[n] = vals[n];
            p = vend + 1;
        } else {
            return (size_t)snprintf(buf, cap,
                                    "{\"id\":%ld,\"error\":\"notstr:%.40s\"}",
                                    id, keys[n]);
        }
        if (!stackee_settings_key_allowed(keys[n])) {
            return (size_t)snprintf(buf, cap,
                                    "{\"id\":%ld,\"error\":\"denied:%.40s\"}",
                                    id, keys[n]);
        }
        kp[n] = keys[n];
        n++;
    }
    if (n == 0) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nokv\"}", id);
    }

    int64_t t0 = esp_timer_get_time();
    size_t len = 0;
    char *text = read_settings_text(&len);
    char *out = malloc(SETTINGS_TEXT_MAX);
    if (out == NULL) {
        free(text);
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nomem\"}", id);
    }
    size_t need = stackee_settings_rewrite(text ? text : "", kp, vp, n, out,
                                           SETTINGS_TEXT_MAX);
    free(text);
    if (need >= SETTINGS_TEXT_MAX) {
        free(out);
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"toolong\"}", id);
    }
    // ★ 一時ファイルに書いて → 読み直して照合 → rename (現行と同じ段取り)。
    esp_err_t err = stackee_fat_write_root("settings.toml", out, need, true);
    free(out);
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    stackee_fat_stats_t fs;
    stackee_fat_stats(&fs);
    if (err != ESP_OK) {
        return (size_t)snprintf(buf, cap,
                                "{\"id\":%ld,\"error\":\"write:%s\"}",
                                id, fs.last_error);
    }
    return (size_t)snprintf(buf, cap,
                            "{\"id\":%ld,\"ok\":1,\"bytes\":%u,\"changed\":%d,"
                            "\"us\":%lu,\"erases\":%lu,"
                            "\"note\":\"reset で反映される\"}",
                            id, (unsigned)need, n, (unsigned long)us,
                            (unsigned long)fs.flash_erases);
}

// ---------------------------------------------------------------------------
// 段階 4: FAT へファイルを置く (素材の更新)
// ---------------------------------------------------------------------------
// `{"cmd":"fs.put","path":"stackee_assets/faces.bin","off":0,"b64":"....","final":true}`
//
// ★ CircuitPython の USB ドライブが無いので、素材の差し替え口がこれしか
//   無い。1 回に載せられるのは枠の都合で 1 KB ほど。off=0 で溜め始め、
//   final=true で **1 回だけ** FAT に書く (書き込みのたびにマウントを
//   付け替えると遅いし、フラッシュも余計に消える)。
//
// ★ 溜め場は PSRAM。いちばん大きい素材は一次回答 (ack_04.pcmz = 76 KB) で、
//   内蔵 RAM には置けない。256 KB あれば現行の素材は全部入る
//   (顔 faces.bin は zlib で 34 KB)。
#define FSPUT_MAX (256 * 1024)

static uint8_t *s_fsput;
static size_t   s_fsput_len;
static char     s_fsput_path[96];

static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') { return c - 'A'; }
    if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
    if (c >= '0' && c <= '9') { return c - '0' + 52; }
    if (c == '+') { return 62; }
    if (c == '/') { return 63; }
    return -1;
}

static int b64_decode(const char *src, size_t len, uint8_t *out, size_t cap) {
    uint32_t acc = 0;
    int bits = 0;
    size_t at = 0;
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '=') { break; }
        int v = b64_val(src[i]);
        if (v < 0) { return -1; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (at >= cap) { return -1; }
            out[at++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    return (int)at;
}

static size_t reply_fs_put(long id, const char *line, char *buf, size_t cap) {
    char path[96];
    bool have_path = stackee_console_str(line, "path", path, sizeof(path));
    long off = stackee_console_int(line, "off", -1);
    bool final = stackee_console_bool(line, "final", false);
    const char *b64 = stackee_console_value(line, "b64");

    if (off == 0) {
        if (!have_path) {
            return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nopath\"}", id);
        }
        free(s_fsput);
        s_fsput = heap_caps_malloc(FSPUT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_fsput_len = 0;
        snprintf(s_fsput_path, sizeof(s_fsput_path), "%s", path);
        if (s_fsput == NULL) {
            return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nomem\"}", id);
        }
    }
    if (s_fsput == NULL) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nostart\"}", id);
    }
    if (off >= 0 && (size_t)off != s_fsput_len) {
        return (size_t)snprintf(buf, cap,
                                "{\"id\":%ld,\"error\":\"offset\",\"have\":%u}",
                                id, (unsigned)s_fsput_len);
    }
    if (b64 != NULL && *b64 == '"') {
        const char *end = b64 + 1;
        while (*end && *end != '"') { end++; }
        int got = b64_decode(b64 + 1, (size_t)(end - b64 - 1),
                             s_fsput + s_fsput_len, FSPUT_MAX - s_fsput_len);
        if (got < 0) {
            return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"badb64\"}", id);
        }
        s_fsput_len += (size_t)got;
    }
    if (!final) {
        return (size_t)snprintf(buf, cap,
                                "{\"id\":%ld,\"ok\":1,\"have\":%u}",
                                id, (unsigned)s_fsput_len);
    }
    esp_err_t err = stackee_fat_write_root(s_fsput_path, s_fsput, s_fsput_len,
                                           true);
    stackee_fat_stats_t fs;
    stackee_fat_stats(&fs);
    size_t wrote = s_fsput_len;
    free(s_fsput);
    s_fsput = NULL;
    s_fsput_len = 0;
    if (err != ESP_OK) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"write:%s\"}",
                                id, fs.last_error);
    }
    return (size_t)snprintf(buf, cap,
                            "{\"id\":%ld,\"ok\":1,\"path\":\"%s\",\"bytes\":%u,"
                            "\"ms\":%lu,\"erases\":%lu}",
                            id, s_fsput_path, (unsigned)wrote,
                            (unsigned long)fs.last_ms,
                            (unsigned long)fs.flash_erases);
}

// ---------------------------------------------------------------------------
// 段階 4: bench / log.burst / lcd.status / lcd.full
// ---------------------------------------------------------------------------
// ★ 現行 (CircuitPython) の bench は「1 スキャンあたりの受信コスト」を
//   測るものだった。C 版に「スキャン」は無いので、**同じ意味の数字**
//   (何もしていないときの console の 1 周のコスト) を返す。
static size_t reply_bench(long id, const char *line, char *buf, size_t cap) {
    long n = stackee_console_int(line, "n", 200);
    if (n < 1) { n = 1; }
    if (n > 2000) { n = 2000; }

    int64_t t0 = esp_timer_get_time();
    for (long i = 0; i < n; i++) {
#if CFG_TUD_CDC
        (void)tud_cdc_available();
#else
        (void)tud_mounted();
#endif
    }
    uint32_t avail_ns = (uint32_t)(((esp_timer_get_time() - t0) * 1000) / n);

    t0 = esp_timer_get_time();
    for (long i = 0; i < n; i++) {
        uint8_t b = 0;
        (void)stackee_conhid_read(&b);
    }
    uint32_t poll_ns = (uint32_t)(((esp_timer_get_time() - t0) * 1000) / n);

    stackee_perf_stats_t main_st;
    stackee_perf_stats(STACKEE_PERF_MAIN, &main_st);
    return (size_t)snprintf(buf, cap,
                            "{\"id\":%ld,\"n\":%ld,\"in_waiting_ns\":%lu,"
                            "\"idle_poll_ns\":%lu,\"main_med_us\":%lu,"
                            "\"main_max_us\":%lu}",
                            id, n, (unsigned long)avail_ns,
                            (unsigned long)poll_ns,
                            (unsigned long)main_st.median_us,
                            (unsigned long)main_st.max_us);
}

// ログの出口が詰まらないことを確かめる (自己診断)。音も画面も触らない。
#define BURST_MAX 500

static size_t reply_log_burst(long id, const char *line, char *buf, size_t cap) {
    long n = stackee_console_int(line, "n", 10);
    if (n < 1) { n = 1; }
    if (n > BURST_MAX) { n = BURST_MAX; }
    long w = stackee_console_int(line, "w", 81);
    if (w < 20) { w = 20; }
    if (w > 200) { w = 200; }

    stackee_logbuf_stats_t before;
    stackee_logbuf_stats(&before);
    char fill[200];
    int pad = (int)w - 19;
    if (pad < 1) { pad = 1; }
    if (pad > (int)sizeof(fill) - 1) { pad = (int)sizeof(fill) - 1; }
    memset(fill, '-', (size_t)pad);
    fill[pad] = '\0';

    int64_t t0 = esp_timer_get_time();
    for (long i = 0; i < n; i++) {
        ESP_LOGI("burst", "%04ld %s", i, fill);
    }
    uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
    stackee_logbuf_stats_t after;
    stackee_logbuf_stats(&after);
    stackee_conhid_stats_t hid;
    stackee_conhid_stats(&hid);
    return (size_t)snprintf(buf, cap,
                            "{\"id\":%ld,\"n\":%ld,\"us\":%lu,\"w\":%d,"
                            "\"log_written\":%lu,\"log_drops\":%lu,"
                            "\"burst_bytes\":%lu,\"burst_drops\":%lu,"
                            "\"hid_tx_pending\":%lu,\"hid_tx_dropped\":%lu}",
                            id, n, (unsigned long)us, pad + 19,
                            (unsigned long)after.written,
                            (unsigned long)after.dropped,
                            (unsigned long)(after.written - before.written),
                            (unsigned long)(after.dropped - before.dropped),
                            (unsigned long)hid.tx_pending,
                            (unsigned long)hid.tx_dropped);
}

static size_t reply_lcd_status(long id, char *buf, size_t cap) {
    uint32_t transfers = 0, rows = 0, worker_ms = 0;
    stackee_lcd_stats(&transfers, &rows, &worker_ms);
    // ★ 現行は displayio の width/height/rotation を返していた。C 版は
    //   向きをパネルの MADCTL で決めているので、その結果の見え方を返す。
    return (size_t)snprintf(buf, cap,
                            "{\"id\":%ld,\"ok\":1,\"ready\":%s,"
                            "\"width\":%d,\"height\":%d,\"rotation\":270,"
                            "\"lcd\":[%lu,%lu,%lu],"
                            "\"transfers\":%lu,\"rows_sent\":%lu,"
                            "\"worker_ms\":%lu,\"fb_bytes\":%u}",
                            id, stackee_lcd_ready() ? "true" : "false",
                            STACKEE_LCD_WIDTH, STACKEE_LCD_HEIGHT,
                            (unsigned long)transfers, (unsigned long)rows,
                            (unsigned long)worker_ms,
                            (unsigned long)transfers, (unsigned long)rows,
                            (unsigned long)worker_ms,
                            (unsigned)stackee_lcd_framebuffer_size());
}

// 全面を送り直す。「部分更新が止まっているのか、そもそも描かれていないのか」
// を切り分けるための窓口 (現行の lcd.full と同じ役目)。
static size_t reply_lcd_full(long id, char *buf, size_t cap) {
    if (!stackee_lcd_ready()) {
        return (size_t)snprintf(buf, cap, "{\"id\":%ld,\"error\":\"nodisplay\"}", id);
    }
    uint32_t t0 = 0, r0 = 0, w0 = 0;
    stackee_lcd_stats(&t0, &r0, &w0);
    int64_t started = esp_timer_get_time();
    stackee_lcd_mark_rows(0, STACKEE_LCD_HEIGHT);
    stackee_lcd_flush();
    uint32_t t1 = 0, r1 = 0, w1 = 0;
    stackee_lcd_stats(&t1, &r1, &w1);
    return (size_t)snprintf(buf, cap,
                            "{\"id\":%ld,\"ok\":1,\"before\":[%lu,%lu,%lu],"
                            "\"after\":[%lu,%lu,%lu],\"us\":%lu}",
                            id, (unsigned long)t0, (unsigned long)r0,
                            (unsigned long)w0, (unsigned long)t1,
                            (unsigned long)r1, (unsigned long)w1,
                            (unsigned long)(esp_timer_get_time() - started));
}

// USB の構成と、Raw HID コンソールの様子。
static size_t reply_usb_status(long id, char *buf, size_t cap) {
    stackee_conhid_stats_t hid;
    stackee_conhid_stats(&hid);
    stackee_uac_stats_t uac;
    stackee_uac_stats(&uac);
    return (size_t)snprintf(buf, cap,
                            "{\"id\":%ld,\"ok\":1,\"profile\":\"%s\","
                            "\"cdc\":%s,\"mounted\":%s,"
                            "\"conhid\":{\"proto\":%d,\"tx_pending\":%lu,"
                            "\"tx_dropped\":%lu,\"tx_overrun\":%lu,"
                            "\"rx_dropped\":%lu,\"polls\":%lu,"
                            "\"listening\":%s,"
                            "\"reports_in\":%lu,\"reports_out\":%lu},"
                            "\"uac\":{\"enabled\":%s,\"streaming\":%s,"
                            "\"opens\":%lu,\"frames\":%lu,\"silence\":%lu,"
                            "\"underruns\":%lu,\"muted\":%s}}",
                            id, stackee_usb_profile(),
                            stackee_usb_has_cdc() ? "true" : "false",
                            stackee_usb_mounted() ? "true" : "false",
                            STACKEE_CONHID_PROTO,
                            (unsigned long)hid.tx_pending,
                            (unsigned long)hid.tx_dropped,
                            (unsigned long)hid.tx_overrun,
                            (unsigned long)hid.rx_dropped,
                            (unsigned long)hid.polls,
                            hid.listening ? "true" : "false",
                            (unsigned long)hid.reports_in,
                            (unsigned long)hid.reports_out,
                            uac.enabled ? "true" : "false",
                            uac.streaming ? "true" : "false",
                            (unsigned long)uac.opens,
                            (unsigned long)uac.frames,
                            (unsigned long)uac.silence,
                            (unsigned long)uac.underruns,
                            uac.muted ? "true" : "false");
}

static void handle_line(const char *line) {
    long id = 0;
    char cmd[32];
    s_cmds++;
    if (!parse_cmd(line, cmd, sizeof(cmd))) {
        send_frame("{\"id\":null,\"error\":\"badjson\"}");
        return;
    }
    if (!parse_id(line, &id)) {
        id = 0;
    }
    if (strcmp(cmd, "hello") == 0) {
        reply_hello(id);
    } else if (strcmp(cmd, "status") == 0) {
        reply_status(id);
    } else if (strcmp(cmd, "reset") == 0) {
        // 現行 stackee_console.py と同じ応答。先に返事を出し切ってから落ちる。
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1,\"in_ms\":300}", id);
        send_frame(buf);
        s_action_at_us = esp_timer_get_time() + 300000;
        s_action = CONSOLE_ACTION_RESET;
    } else if (strcmp(cmd, "key.inject") == 0) {
        reply_key_inject(id, line);
    } else if (strcmp(cmd, "log.tail") == 0) {
        reply_log_tail(id, line);
    } else if (strcmp(cmd, "axp.read") == 0) {
        // `{"cmd":"axp.read","reg":48}` → AXP2101 のレジスタ 1 バイト (診断用)
        char buf[96];
        long reg = stackee_console_int(line, "reg", -1);
        if (reg < 0 || reg > 0xFF) {
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"error\":\"reg\"}", id);
        } else {
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"reg\":%ld,\"value\":%d}",
                     id, reg, stackee_board_axp_read((uint8_t)reg));
        }
        send_frame(buf);
    } else if (strcmp(cmd, "crypto.selftest") == 0) {
        reply_crypto_selftest(id);
    } else if (strcmp(cmd, "axp.write") == 0) {
        // `{"cmd":"axp.write","reg":24,"value":24}` → 1 バイト書いて読み直す (診断用)。
        // ★ 0x10 (電源の切/再投入) と 0x17 (ゲージのリセット) は受けない。
        char buf[96];
        long reg = stackee_console_int(line, "reg", -1);
        long val = stackee_console_int(line, "value", -1);
        if (reg < 0 || reg > 0xFF || val < 0 || val > 0xFF || reg == 0x10 || reg == 0x17) {
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"error\":\"denied\"}", id);
        } else {
            int rc = stackee_board_axp_write((uint8_t)reg, (uint8_t)val);
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"reg\":%ld,\"wrote\":%ld,\"rc\":%d,\"value\":%d}",
                     id, reg, val, rc, stackee_board_axp_read((uint8_t)reg));
        }
        send_frame(buf);
    } else if (strcmp(cmd, "hid.switch") == 0) {
        char buf[96];
        stackee_hid_dest_t dest = stackee_hid_dest_toggle();
        snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1,\"hid_sel\":\"%s\"}",
                 id, stackee_hid_dest_name(dest));
        send_frame(buf);
    } else if (strcmp(cmd, "ble.refresh") == 0) {
        char buf[64];
        stackee_ble_refresh();
        snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1}", id);
        send_frame(buf);
    } else if (strcmp(cmd, "ble.svc_changed") == 0) {
        // ★ macOS が古い GATT の並びを覚えたままのときの手当て。
        //   「1 番から 0xFFFF 番まで変わった」と伝えて読み直させる。
        char buf[128];
        bool ok = stackee_ble_send_service_changed();
        stackee_ble_stats_t st;
        stackee_ble_stats(&st);
        snprintf(buf, sizeof(buf),
                 "{\"id\":%ld,\"ok\":%d,\"handle\":%u,\"sent\":%lu,"
                 "\"acked\":%lu,\"rc\":%d,\"connected\":%s}",
                 id, ok ? 1 : 0, st.svc_changed_handle,
                 (unsigned long)st.svc_changed_sent,
                 (unsigned long)st.svc_changed_acked, st.svc_changed_rc,
                 st.connected ? "true" : "false");
        send_frame(buf);
    } else if (strcmp(cmd, "hid.set") == 0) {
        // ★ hid.switch はトグルなので、検証のあと「必ず BLE に戻す」が
        //   書きにくい。行き先を名指しで決める口を足しておく。
        char want[8];
        char buf[128];
        if (!stackee_console_str(line, "dest", want, sizeof(want))) {
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"error\":\"nodest\"}", id);
        } else if (strcmp(want, "BLE") == 0 || strcmp(want, "ble") == 0) {
            stackee_hid_dest_set(STACKEE_HID_BLE);
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1,\"hid_sel\":\"%s\"}",
                     id, stackee_hid_dest_name(stackee_hid_dest_selected()));
        } else if (strcmp(want, "USB") == 0 || strcmp(want, "usb") == 0) {
            stackee_hid_dest_set(STACKEE_HID_USB);
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1,\"hid_sel\":\"%s\"}",
                     id, stackee_hid_dest_name(stackee_hid_dest_selected()));
        } else {
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"error\":\"baddest\"}", id);
        }
        send_frame(buf);
    } else if (strcmp(cmd, "ble.drop_cccd") == 0) {
        // 鍵は残して「通知の購読」の記録だけ捨てる。CircuitPython 版から
        // 引き継いだボンドで、通知が来ない (キーが届かない) ときの逃げ道。
        char buf[96];
        int dropped = stackee_nvs_drop_nimble_cccd();
        snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1,\"dropped\":%d}",
                 id, dropped);
        send_frame(buf);
    } else if (strcmp(cmd, "ble.clear_bonds") == 0) {
        // ★ ボンドを消すと Mac 側でもペアリングを削除しないと繋がらない。
        //   だからキーには割り当てず、ここからだけ呼べるようにしてある。
        char buf[64];
        stackee_ble_clear_bonds();
        snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1}", id);
        send_frame(buf);
    } else if (strcmp(cmd, "bootloader") == 0) {
        // ROM の USB ダウンロードモードへ。tools/flash.py の行き先と同じ。
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"id\":%ld,\"ok\":1,\"in_ms\":300}", id);
        send_frame(buf);
        s_action_at_us = esp_timer_get_time() + 300000;
        s_action = CONSOLE_ACTION_ROM;
    } else if (strcmp(cmd, "settings.get") == 0) {
        size_t len = reply_settings_get(id, s_wide, sizeof(s_wide));
        send_frame((len < sizeof(s_wide)) ? s_wide
                                          : "{\"id\":null,\"error\":\"toolong\"}");
    } else if (strcmp(cmd, "settings.raw") == 0) {
        size_t len = reply_settings_raw(id, s_wide, sizeof(s_wide));
        send_frame((len < sizeof(s_wide)) ? s_wide
                                          : "{\"id\":null,\"error\":\"toolong\"}");
    } else if (strcmp(cmd, "settings.set") == 0) {
        char buf[256];
        reply_settings_set(id, line, buf, sizeof(buf));
        send_frame(buf);
    } else if (strcmp(cmd, "fs.put") == 0) {
        char buf[256];
        reply_fs_put(id, line, buf, sizeof(buf));
        send_frame(buf);
    } else if (strcmp(cmd, "bench") == 0) {
        char buf[256];
        reply_bench(id, line, buf, sizeof(buf));
        send_frame(buf);
    } else if (strcmp(cmd, "log.burst") == 0) {
        char buf[320];
        reply_log_burst(id, line, buf, sizeof(buf));
        send_frame(buf);
    } else if (strcmp(cmd, "lcd.status") == 0) {
        char buf[320];
        reply_lcd_status(id, buf, sizeof(buf));
        send_frame(buf);
    } else if (strcmp(cmd, "lcd.full") == 0) {
        char buf[256];
        reply_lcd_full(id, buf, sizeof(buf));
        send_frame(buf);
    } else if (strcmp(cmd, "usb.status") == 0) {
        char buf[512];
        reply_usb_status(id, buf, sizeof(buf));
        send_frame(buf);
    } else if (strncmp(cmd, "loop.", 5) == 0) {
        // ★ loop.* は CircuitPython のメインループ (KMK のスキャン 1 周) を
        //   測るためのもの。C 版にそれに当たるものは無い (入力は専用タスクで
        //   1 ms 周期)。同じ意味の数字は status の perf に出ているので、
        //   ここでは「無い」と正直に返す。
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"id\":%ld,\"error\":\"unsupported\","
                 "\"note\":\"C版にCircuitPythonのメインループは無い。"
                 "status の perf.input / perf.main を見ること\"}", id);
        send_frame(buf);
    } else if (s_ext_count > 0) {
        // 段階 2 の画面 (lcd.crc / face.set / ui.selftest ...) と
        // 段階 3 の音・Wi-Fi (audio.* / talk.* / wifi.*)。
        // ★ 応答が 32 表情ぶんの CRC を並べるので、ここだけ枠を大きく取る
        //   (CDC の送信 FIFO は 2048 B、ホスト側の上限は 8192 B)。
        size_t len = 0;
        for (int i = 0; i < s_ext_count && len == 0; i++) {
            len = s_ext[i](cmd, line, id, s_wide, sizeof(s_wide));
        }
        if (len == 0 || len >= sizeof(s_wide)) {
            char buf[96];
            snprintf(buf, sizeof(buf), "{\"id\":%ld,\"error\":\"%s\"}", id,
                     len ? "toolong" : "unsupported");
            send_frame(buf);
        } else {
            send_frame(s_wide);
        }
    } else {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"id\":%ld,\"error\":\"unsupported\"}", id);
        send_frame(buf);
    }
}

// ---------------------------------------------------------------------------
// 受信の枠取り
// ---------------------------------------------------------------------------
static void run_pending_action(void) {
    if (s_action == CONSOLE_ACTION_NONE || esp_timer_get_time() < s_action_at_us) {
        return;
    }
    console_action_t action = s_action;
    s_action = CONSOLE_ACTION_NONE;
    if (action == CONSOLE_ACTION_ROM) {
        stackee_usb_request_rom_download();
    } else {
        stackee_usb_request_restart();
    }
}

// 1 バイト受け取って枠を組み立てる。揃ったら handle_line。
static void feed_byte(uint8_t b);

void stackee_console_poll(void) {
    run_pending_action();
    // ★ conhid に時計を渡す (あちらは ESP-IDF に依存しない作りなので、
    //   「ホストが最近読んだか」を判断する時刻をここから渡す)。
    stackee_conhid_tick((uint32_t)(esp_timer_get_time() / 1000));
#if CFG_TUD_CDC
    if (tud_cdc_available()) {
        uint8_t chunk[64];
        uint32_t got = tud_cdc_read(chunk, sizeof(chunk));
        for (uint32_t i = 0; i < got; i++) {
            feed_byte(chunk[i]);
        }
    }
#endif
    // Raw HID (VIA の独自 command id 0xC0)。★ 入力タスクが溜めたものを
    //   ここで処理する。コマンドの中身を入力タスクで走らせない。
    uint8_t b = 0;
    int guard = 0;
    while (guard++ < 512 && stackee_conhid_read(&b)) {
        feed_byte(b);
    }
}

static void feed_byte(uint8_t b) {
    if (b == MARKER) {
        s_in_frame = true;      // 枠の中に枠頭。前半は捨ててやり直す
        s_len = 0;
        return;
    }
    if (!s_in_frame) {
        return;                 // 枠の外は読み捨てる (REPL は無い)
    }
    if (b == '\n') {
        s_line[s_len] = '\0';
        s_in_frame = false;
        if (s_len > 0) {
            handle_line(s_line);
        }
        s_len = 0;
        return;
    }
    if (s_len + 1 < sizeof(s_line)) {
        s_line[s_len++] = (char)b;
    } else {
        s_drops++;
        s_in_frame = false;     // 長すぎる。閉じるまで捨てる
        s_len = 0;
    }
}

// ---------------------------------------------------------------------------
// ログの出口
// ---------------------------------------------------------------------------
// ★ esp_log の出口そのものは stackee_logbuf が持っている
//   (起動のいちばん最初から溜めるため)。こちらはその後ろに繋ぐ「素通し」。
static int log_to_cdc(const char *text, int len) {
    if (len > 0 && tx_take()) {
        // 枠 (0x1E) を付けない = ホスト側ではログとして扱われる。
        out_bytes(text, (size_t)len, true);
        tx_give();
    }
    return len;
}

void stackee_console_attach_log(void) {
    stackee_logbuf_set_sink(log_to_cdc);
}

void stackee_console_init(void) {
    stackee_conhid_init();
    s_boot_us = 0;      // esp_timer は起動時に 0 から始まる
    s_len = 0;
    s_in_frame = false;
    if (s_tx_lock == NULL) {
        s_tx_lock = xSemaphoreCreateMutex();
    }
}
