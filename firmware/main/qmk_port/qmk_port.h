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
    // ★ MIC(kc) だけは「独自キーなのに HID へも出る」。PC 側の
    //   プッシュトゥトーク (F13) を押している間、本体の顔も「聞き取り中」に
    //   したい、というだけのキー。キーそのものの役目は変えない。
    STACKEE_KEY_MIC,
    // ★ stackee 独自キー Custom_0..Custom_9 (2026-09-27)。押した瞬間に 1 回だけ
    //   サーバへ知らせる。番号は STACKEE_KEY_CUSTOM_0 からの連番で渡す。
    STACKEE_KEY_CUSTOM_0,
    STACKEE_KEY_CUSTOM_LAST = STACKEE_KEY_CUSTOM_0 + 9,
    // ★ クリップ (2026-09-30)。押した瞬間に 1 回、FAT のクリップを 1 件鳴らす
    //   (通信しない。README §17-2f)。
    STACKEE_KEY_CLIP,
    // ★ クリップの自動取得の入り切り (2026-09-30)。押した瞬間に 1 回。
    STACKEE_KEY_CLIP_AUTO,
    // ★ 本体の設定メニューを開く / 閉じる (2026-10-01)。押した瞬間に 1 回。
    //   **メニュー中もこのキーだけは効く** (下の「キーの横取り」)。
    STACKEE_KEY_SETTINGS,
} stackee_key_action_t;

const char *stackee_key_action_name(stackee_key_action_t action);
void stackee_qmk_custom_key(stackee_key_action_t action, bool pressed);

// 独自キーが押された回数 (status / テスト用)。
uint32_t stackee_qmk_custom_key_count(void);

// ---------------------------------------------------------------------------
// 設定メニューのキーの横取り (2026-10-01、README §17-2g)
// ---------------------------------------------------------------------------
// メニューを開いている間は、QMK が作ったキーボードのレポートを **PC へ
// 送らず本体が読む**。キーの処理そのもの (レイヤー・MT/LT・Shift) は QMK が
// いつもどおりやるので、矢印がレイヤー 3 にあっても、Shift が MT でも、
// 普段と同じ指でメニューを動かせる。
//
//   OFF   … 普段。レポートは PC へ。
//   OPEN  … メニュー中。キーボードのレポートは stackee_qmk_menu_key() へ
//            (新しく押されたキーだけ)。マウス・コンシューマ・システムは捨てる。
//            独自キー (Talk / 音量 / Custom / Clip / MIC(kc) …) の**押下**と
//            QK_BOOT などの特殊キーは握りつぶす (離しは通す。押しっぱなしの
//            印を下ろすため)。Settings だけは効く。
//   DRAIN … 閉じた直後。そのとき押されていたキーが全部離れるまで (最大
//            STACKEE_GATE_DRAIN_MAX_MS) は、まだ PC へ出さない。離れたら
//            QMK の状態を空にして (これも PC へは出さない) OFF へ。
//
// ★ 開くときは、**先に全部離したレポートを PC へ送ってから**横取りを始める
//   (clear_keyboard)。開いた瞬間に押していたキーが PC に押しっぱなしで
//   残らない。閉じたあとは、メニュー中に押していたキーの離しも PC には
//   出ない (QMK の最後のレポートが空のまま = 送る差分が無い)。
// ★ 呼べるのは input タスクだけ (QMK の状態を触るのはこのタスク 1 本)。
typedef enum {
    STACKEE_GATE_OFF = 0,
    STACKEE_GATE_OPEN,
    STACKEE_GATE_DRAIN,
} stackee_gate_state_t;

#define STACKEE_GATE_DRAIN_MAX_MS 1500

void stackee_qmk_gate_set(bool open);
// 1 周ごと (keyboard_task のあと)。DRAIN を終わらせる。
void stackee_qmk_gate_step(void);
stackee_gate_state_t stackee_qmk_gate_state(void);
bool stackee_qmk_gate_capturing(void);
const char *stackee_qmk_gate_name(stackee_gate_state_t state);

typedef struct {
    uint32_t opens, closes;         // 横取りを始めた / DRAIN へ移った回数
    uint32_t drain_timeouts;        // 離れるのを待ちきれずに終えた回数
    uint32_t captured;              // 本体が読んだキーボードのレポート
    uint32_t keys;                  // そこから取り出した「押されたキー」
    uint32_t dropped;               // 捨てたマウス・コンシューマ・システム
    uint32_t swallowed;             // 握りつぶした独自キー・特殊キー
    uint32_t to_pc;                 // PC へ出したキーボード / マウス /
                                    // コンシューマ / システムのレポート (累計)
} stackee_qmk_gate_stats_t;

void stackee_qmk_gate_stats(stackee_qmk_gate_stats_t *out);
void stackee_qmk_gate_note_swallowed(void);

// アプリ (stackee_input.c / ホストの台本) が用意する。メニュー中に新しく
// 押されたキー (HID の使用番号) と、そのときの修飾 (レポートの mods)。
void stackee_qmk_menu_key(uint8_t usage, uint8_t mods);

// ---------------------------------------------------------------------------
// 立ち上げ
// ---------------------------------------------------------------------------
// QMK 側 (eeconfig / keymap / host driver) をまとめて起こす。
void stackee_qmk_init(void);

// 1 周ぶん回す (keyboard_task + eeprom の後片付け)。
void stackee_qmk_task(void);
