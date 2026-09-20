// QMK と Stackee のあいだの橋渡し層。DESIGN.md §2 / §3。
//
// QMK の quantum は「キーの押し離しの列」を受け取って「HID レポートの列」を
// 返す純粋な部品として使う。ハードに触るところ (I2C、USB、NVS、時計) は
// すべてこの層で受け止め、third_party/qmk のファイルは 1 文字も直さない。
//
// 依存の向き:
//
//   stackee_input.c ──> qmk_port ──> third_party/qmk
//        (TCA8418)                        │
//                                         ↓ host_driver_t
//                       stackee_report_queue ──> stackee_hid_out.c (USB/BLE)
//
// esp32-qmk-lucky65 (MIT, https://github.com/chcbaram/esp32-qmk-lucky65) の
// main/ap/modules/qmk/port/ を参考にしたが、実装は自前。参考にしたのは
// 「どの関数を用意すれば quantum が動くか」という一覧と、eeprom を RAM の
// 影に載せて書き戻しを遅延させるという考え方。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// 時計
// ---------------------------------------------------------------------------
// QMK が見る唯一の時刻源。実機では esp_timer、ホストビルドではテストが
// 進める偽の時計 (stackee_qmk_set_now_ms)。
uint32_t stackee_qmk_now_ms(void);

// ホストビルド専用。実機側の実装 (qmk_port_time_idf.c) は持たない。
void stackee_qmk_set_now_ms(uint32_t ms);

// ---------------------------------------------------------------------------
// マトリクス (TCA8418 のスロット -> QMK の行/列)
// ---------------------------------------------------------------------------
#define STACKEE_MATRIX_ROWS 5
#define STACKEE_MATRIX_COLS 10
#define STACKEE_SLOT_COUNT  (STACKEE_MATRIX_ROWS * STACKEE_MATRIX_COLS)

// TCA8418 のスロット番号 (0..49 = row * 10 + col) をそのまま渡す。
// 呼べるのは input タスクだけ。
void stackee_qmk_matrix_event(uint8_t slot, bool pressed);

// FIFO 溢れなどで同期を失ったとき、押下中を全部離す。
void stackee_qmk_matrix_release_all(void);

// いま押されているスロット数 (デバウンス前)。status 用。
uint8_t stackee_qmk_matrix_pressed_count(void);

// ---------------------------------------------------------------------------
// EEPROM (NVS のブロブ)
// ---------------------------------------------------------------------------
// 起動時に 1 回。NVS から影を読み込む。
void stackee_qmk_eeprom_init(void);

// 書き込みが溜まっていれば NVS へ流す。input タスク以外 (優先度の低い所) から
// 定期的に呼ぶ。1 回の呼び出しで最大 1 回しか NVS を触らない。
void stackee_qmk_eeprom_task(void);

// 影が書き換わってから NVS へ流すまでの静止時間 [ms]。VIA の配列書き込みは
// 1 キーずつ来るので、まとめてから 1 回で書く。
#define STACKEE_EEPROM_COMMIT_QUIET_MS 400

// 保存先。実体は実機 (stackee_nvs.c) とホストビルドで別。
bool stackee_qmk_eeprom_backend_load(uint8_t *buf, size_t len);
bool stackee_qmk_eeprom_backend_save(const uint8_t *buf, size_t len);

// 保存の様子 (status / テスト用)。
typedef struct {
    uint32_t writes;        // eeprom_write_byte が呼ばれた回数
    uint32_t commits;       // NVS へ流した回数
    bool     dirty;
} stackee_qmk_eeprom_stats_t;

void stackee_qmk_eeprom_stats(stackee_qmk_eeprom_stats_t *out);

// ---------------------------------------------------------------------------
// ハードに触る出口 (実機は stackee_input.c 側、ホストビルドはテストが持つ)
// ---------------------------------------------------------------------------
void stackee_qmk_delay_ms(uint32_t ms);

// QK_BOOT (KMK の KC.RESET と同じ位置) の行き先。ROM のダウンロードモードへ。
void stackee_qmk_enter_rom_download(void);
void stackee_qmk_restart(void);

// ホストから届いた LED の状態 (CapsLock など) を QMK に渡す。
void stackee_qmk_set_led_state(uint8_t leds);

// 独自キーが押された / 離された。**HID には出さない**
// (process_record_kb が false を返す)。
//
// ★ ここで渡すのは QMK のキーコードではなく「何をしたいか」。
//   アプリ側 (stackee_input.c) に quantum.h を持ち込まないため。
//   QMK の bits.h は BIT32 / BIT64 を定義していて、ESP-IDF の
//   esp_bit_defs.h (BLE のヘッダが引っぱってくる) とぶつかる。
//   翻訳単位を分けておけば、そもそもぶつかりようがない。
typedef enum {
    STACKEE_KEY_TALK = 0,
    STACKEE_KEY_VOLUP,
    STACKEE_KEY_VOLDN,
    STACKEE_KEY_HID_SWITCH,
    STACKEE_KEY_BLE_REFRESH,
    STACKEE_KEY_CAMERA,
    STACKEE_KEY_TOUCH_SCROLL,
} stackee_key_action_t;

const char *stackee_key_action_name(stackee_key_action_t action);
void stackee_qmk_custom_key(stackee_key_action_t action, bool pressed);

// 独自キーが押された回数 (status / テスト用)。
uint32_t stackee_qmk_custom_key_count(void);

// ---------------------------------------------------------------------------
// 立ち上げ
// ---------------------------------------------------------------------------
// QMK 側 (eeconfig / keymap / host driver) をまとめて起こす。
void stackee_qmk_init(void);

// 1 周ぶん回す (keyboard_task + eeprom の後片付け)。
void stackee_qmk_task(void);
