// stackee_console.c の応答の組み立てを Mac 上でそのまま走らせ、
// 出てきた枠が本当に JSON として読めるかを確かめる。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char g_out[8192]; int g_out_len;

// 実機側モジュールの代わり
#include "stackee_perf.h"
#include "stackee_assets.h"
#include "stackee_board.h"
#include "stackee_cryptocheck.h"
#include "stackee_ui.h"
#include "stackee_ble.h"
#include "stackee_hid_dest.h"
#include "stackee_input.h"
#include "stackee_lcd.h"
#include "stackee_logbuf.h"
#include "stackee_nvs.h"
#include "stackee_report_queue.h"
#include "stackee_usb.h"
int stackee_board_battery_percent(void){return 77;}
int stackee_board_charging(void){return 0;}
bool stackee_lcd_ready(void){return true;}
void stackee_lcd_stats(uint32_t*a,uint32_t*b,uint32_t*c){*a=3;*b=320;*c=41;}
// 段階 1 で足した口。ここでは固定値を返すだけ (見たいのは文字列の組み立て)。
void stackee_input_stats(stackee_input_stats_t *out){
    out->tca_connected=true; out->key_events=12; out->keys_down=1;
    out->overflows=0; out->io_fails=0; out->stray=0; out->custom_keys=2;
}
void stackee_usb_request_rom_download(void){printf("ROM\n");}
void stackee_usb_request_restart(void){printf("RESET\n");}
// 段階 1b (BLE) の口。実機の挙動は真似しない。見たいのは文字列の組み立て。
void stackee_ble_stats(stackee_ble_stats_t *out){
    out->started=true; out->err=""; out->err_code=0;
    out->ready=true; out->connected=true; out->advertising=false;
    out->interval_ms=15; out->connects=1; out->disconnects=0;
    out->sent=42; out->failed=0;
    out->adv_starts=3; out->adv_fails=0; out->adv_revived=1;
    out->svc_changed_handle=12; out->svc_changed_sent=1;
    out->svc_changed_acked=1; out->svc_changed_rc=0;
    out->adv_kind="undirected"; out->adv_directed=2; out->have_bond_peer=true;
}
void stackee_ble_refresh(void){printf("BLE_REFRESH\n");}
bool stackee_ble_send_service_changed(void){printf("SVC_CHANGED\n");return true;}
void stackee_ble_clear_bonds(void){printf("BLE_CLEAR_BONDS\n");}
int stackee_nvs_drop_nimble_cccd(void){return 2;}
// key.inject の代役。実機側は入力タスクが TCA8418 のイベントとして流すが、
// ここで見たいのは応答の組み立てだけ (打鍵の道そのものは
// tools/test_keyseq_host.py の KeyInjectTest が実物で確かめている)。
bool stackee_input_inject(uint16_t keycode, uint32_t hold_ms,
                          stackee_inject_result_t *out){
    out->ok=true; out->keycode=keycode; out->press_us=1234;
    out->release_us=hold_ms*1000+2345; out->pushed=2;
    out->sent_usb=0; out->sent_ble=2;
    snprintf(out->dest, sizeof(out->dest), "BLE");
    return true;
}
static stackee_assets_info_t s_ai = {.mounted=true,.manifest_ok=true,.version=1,.size=240,.faces=32};
const stackee_assets_info_t *stackee_assets_info(void){return &s_ai;}
// 段階 3 の口。ここでも見たいのは文字列の組み立てだけなので固定値を返す。
bool stackee_wifi_connected(void){return true;}
const char *stackee_wifi_state_name(void){return "up";}
const char *stackee_wifi_ssid(void){return "home";}
const char *stackee_wifi_ip(void){return "192.168.0.42";}
int stackee_wifi_net_count(void){return 2;}
uint32_t stackee_wifi_connect_ms(void){return 420;}
uint32_t stackee_wifi_up_ms(void){return 2405;}
int stackee_volume_percent(void){return 20;}
bool stackee_volume_save_pending(void){return false;}
const char *stackee_volume_source(void){return "nvs";}
const char *stackee_audio_talk_state(void){return "idle";}
bool stackee_audio_null(void){return false;}
bool stackee_audio_busy(void){return false;}
bool stackee_http_configured(void){return true;}
bool stackee_http_has_token(void){return true;}
// ★ 返答文には日本語と引用符が入る。JSON として壊れないことを見るために
//   わざと両方入れてある。
const char *stackee_ui_screen(void){return "こんにちは \"なのだ\"";}

// ---- 段階 4 の口 -----------------------------------------------------------
// ★ FAT には書かない (ここで見たいのは応答の組み立てだけ)。settings.set は
//   「書けた」ことにして、書こうとした中身を標準出力へ出す。
#include "stackee_fat.h"
#include "stackee_uac.h"
static stackee_fat_stats_t g_fat = {.writes=1,.bytes=123,.last_ms=42,
                                    .flash_erases=2};
static char g_settings[4096] =
    "# stackee\n"
    "STACKEE_HOST = \"192.168.0.5\"\n"
    "STACKEE_PORT = \"8765\"\n"
    "STACKEE_WIFI_PASSWORD = \"himitsu\"\n"
    "STACKEE_TALK_URL = \"https://pi400.local/talk\"\n"
    "STACKEE_TALK_TOKEN = \"abcdef\"\n";
char *stackee_assets_read_root(const char *name, size_t *out_len){
    if (strcmp(name, "settings.toml") != 0) { return NULL; }
    size_t n = strlen(g_settings);
    char *buf = malloc(n + 1);
    memcpy(buf, g_settings, n + 1);
    if (out_len) { *out_len = n; }
    return buf;
}
esp_err_t stackee_fat_write_root(const char *name, const void *data, size_t len,
                                 bool atomic){
    (void)atomic;
    fprintf(stderr, "WROTE %s %zu\n", name, len);   // 枠の外を汚さない
    if (strcmp(name, "settings.toml") == 0 && len < sizeof(g_settings)) {
        memcpy(g_settings, data, len);
        g_settings[len] = 0;
    }
    g_fat.writes++;
    g_fat.bytes += (uint32_t)len;
    return 0;
}
esp_err_t stackee_fat_mkdir_root(const char *name){(void)name;return 0;}
void stackee_fat_stats(stackee_fat_stats_t *out){*out=g_fat;}
uint8_t *stackee_lcd_framebuffer(void){return NULL;}
size_t stackee_lcd_framebuffer_size(void){return 240*320*2;}
void stackee_lcd_mark_rows(int y,int h){(void)y;(void)h;}
void stackee_lcd_flush(void){}
const char *stackee_usb_profile(void){return "dev";}
bool stackee_usb_mounted(void){return true;}
void raw_hid_send(uint8_t *data, uint8_t length){(void)data;(void)length;}
bool stackee_usb_has_cdc(void){return true;}
void stackee_uac_stats(stackee_uac_stats_t *out){memset(out,0,sizeof(*out));}

// --- 段階 4 で増えた代役 (電源キー / PMIC / 暗号の自己診断) ---------------
// ここで見ているのは枠の組み立てだけなので、中身は固定値でよい。
void stackee_ui_pwrkey_stats(int *long_presses, int *short_presses, int *boot_irq){
    if (long_presses) *long_presses = 0;
    if (short_presses) *short_presses = 0;
    if (boot_irq) *boot_irq = 0;
}
void stackee_board_i2c_stats(uint32_t *fail, uint32_t *recovered){
    if (fail) *fail = 0;
    if (recovered) *recovered = 0;
}
int stackee_board_battery_mv(void){return 4050;}
int stackee_board_axp_read(uint8_t reg){(void)reg;return 0;}
int stackee_board_axp_write(uint8_t reg, uint8_t value){(void)reg;(void)value;return 0;}
bool stackee_cryptocheck_run(stackee_cryptocheck_result_t *out){
    if (out) {
        memset(out, 0, sizeof(*out));
        out->sha_abc = out->sha_internal_4k = out->sha_psram_4k = 1;
        out->x509_parse1 = out->x509_parse2 = 0;
        out->x509_sig_ok = 1;
        out->ok = true;
    }
    return true;
}
void stackee_cryptocheck_stats(stackee_cryptocheck_stats_t *out){
    if (out) { memset(out, 0, sizeof(*out)); out->last_ok = true; }
}
bool stackee_cryptocheck_broken(void){return false;}

#include "stackee_console.c"

// 取り出した文字列を JSON の値として安全に出す (`"` と `\` を逃がす)。
static size_t append_screen_like(char *buf, size_t cap, size_t at, const char *text) {
    for (const char *p = text; *p && at + 3 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            buf[at++] = '\\';
        }
        buf[at++] = *p;
    }
    buf[at] = '\0';
    return at;
}

// 溜まった枠を吐き出して数え直す。
static void emit(void) {
    fwrite(g_out, 1, (size_t)g_out_len, stdout);
    g_out_len = 0;
}

int main(void) {
    stackee_console_init();
    stackee_perf_init();
    for (int i = 0; i < 300; i++) stackee_perf_sample(STACKEE_PERF_MAIN, 1000 + i);
    emit(); reply_hello(7);
    emit(); reply_status(8);
    stackee_logbuf_install();
    // 起動ログのリングバッファ。改行や引用符が JSON として壊れないか。
    emit();
    ESP_LOGI("t", "%s", "起動ログ \"1\"");
    ESP_LOGE("t", "%s", "BLE 失敗\n2 行目\t制御文字");
    reply_log_tail(11, "{\"id\":11,\"cmd\":\"log.tail\",\"bytes\":200}");
    emit();
    reply_key_inject(12, "{\"id\":12,\"cmd\":\"key.inject\"}");
    emit();
    reply_key_inject(13, "{\"id\":13,\"cmd\":\"key.inject\",\"kc\":\"LANG1\",\"hold_ms\":50}");
    emit();
    reply_key_inject(14, "{\"id\":14,\"cmd\":\"key.inject\",\"kc\":\"NOPE\"}");
    // ★ 文字列の取り出し。stackee_console_client.py は ensure_ascii=True で
    //   送るので、日本語の SSID は \uXXXX で来る。パスワードに `"` が入る
    //   こともある。枠の外へ出すのでホスト側では「ログ」として読める。
    emit();
    {
        static const char *REQ =
            "{\"ssid\":\"\\u3042\\u3044\",\"password\":\"a\\\"b\\\\c\"}";
        char ssid[64] = {0};
        char password[64] = {0};
        stackee_console_str(REQ, "ssid", ssid, sizeof(ssid));
        stackee_console_str(REQ, "password", password, sizeof(password));
        char frame[256];
        size_t at = (size_t)snprintf(frame, sizeof(frame),
                                     "{\"id\":99,\"ssid\":\"%s\",\"password\":\"",
                                     ssid);
        at = append_screen_like(frame, sizeof(frame), at, password);
        snprintf(frame + at, sizeof(frame) - at, "\"}");
        send_frame(frame);
    }
    // ---- 段階 4 のコマンド ------------------------------------------------
    emit(); handle_line("{\"id\":20,\"cmd\":\"settings.get\"}");
    emit(); handle_line("{\"id\":21,\"cmd\":\"settings.raw\"}");
    emit(); handle_line("{\"id\":22,\"cmd\":\"settings.set\","
                        "\"kv\":{\"STACKEE_HOST\":\"10.0.0.9\"}}");
    emit(); handle_line("{\"id\":23,\"cmd\":\"settings.set\","
                        "\"kv\":{\"CIRCUITPY_WIFI_SSID\":\"ie\"}}");
    emit(); handle_line("{\"id\":24,\"cmd\":\"settings.set\"}");
    emit(); handle_line("{\"id\":25,\"cmd\":\"bench\",\"n\":5}");
    emit(); handle_line("{\"id\":26,\"cmd\":\"log.burst\",\"n\":3}");
    emit(); handle_line("{\"id\":27,\"cmd\":\"lcd.status\"}");
    emit(); handle_line("{\"id\":28,\"cmd\":\"lcd.full\"}");
    emit(); handle_line("{\"id\":29,\"cmd\":\"usb.status\"}");
    emit(); handle_line("{\"id\":30,\"cmd\":\"loop.stats\"}");
    // fs.put: 3 つに割って送り、最後にまとめて書く ("hello" の base64)。
    emit(); handle_line("{\"id\":31,\"cmd\":\"fs.put\","
                        "\"path\":\"stackee_assets/x.bin\",\"off\":0,"
                        "\"b64\":\"aGVs\"}");
    emit(); handle_line("{\"id\":32,\"cmd\":\"fs.put\",\"off\":3,"
                        "\"b64\":\"bG8=\",\"final\":true}");
    emit(); handle_line("{\"id\":33,\"cmd\":\"fs.put\",\"off\":99,"
                        "\"b64\":\"aGVs\"}");
    emit(); handle_line("{\"id\":9,\"cmd\":\"nope\"}");
    emit(); handle_line("garbage");
    // 目録が読めなかったとき
    emit();
    s_ai.manifest_ok = false;
    strcpy(s_ai.error, "manifest.json が開けない");
    reply_status(10);
    emit();
    return 0;
}
