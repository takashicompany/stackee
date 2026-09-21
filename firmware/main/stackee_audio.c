#include "stackee_audio.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "stackee_assets.h"
#include "stackee_console.h"
#include "stackee_codec.h"
#include "stackee_http.h"
#include "stackee_input.h"
#include "stackee_jsonlite.h"
#include "stackee_settings.h"
#include "stackee_talksm.h"
#include "stackee_ui.h"
#include "stackee_uac.h"
#include "stackee_volume.h"
#include "stackee_wifi.h"

static const char *TAG = "audio";

#define TICK_MS          5
#define RECORD_MAX_MS    1000       // audio.selftest の録音時間

// ---- 立ち上がりの計測 -------------------------------------------------------
// 先頭 500 ms を 50 ms ずつ 10 枠。押下 → 使える音までの形を数字で見る。
#define HEAD_SLOT_MS     50
#define HEAD_MS          (HEAD_SLOT_MS * STACKEE_AUDIO_HEAD_SLOTS)
#define CSM_TIMEOUT_MS   300
#define PLAY_CHUNK       512        // 1 回に I2S へ渡すサンプル数
#define PLAY_DRAIN_MS    200        // 書き終えてから DMA が吐き切るまで

typedef enum { MODE_OFF = 0, MODE_MIC, MODE_SPK } audio_mode_t;

typedef struct {
    int16_t *pcm;
    int      samples;
    // 帯に出す行 (改行区切り、最大 STACKEE_TALK_SUB_LINES 行)。PSRAM。
    // manifest の acks[].lines が無い旧素材では NULL = 字幕なし。
    char    *lines;
} ack_t;

static struct {
    bool ready;
    TaskHandle_t task;

    audio_mode_t mode;
    i2s_chan_handle_t rx;
    i2s_chan_handle_t tx;
    bool rx_on, tx_on;          // いま enable してあるか
    bool chans_ready;           // 口を作り終えたか (作るのは一度だけ)
    esp_err_t chans_err;        // 作るのに失敗したときの理由 (作り直さない)
    int  amp_was_on;

    _Atomic bool null_out;      // ヌル出力 (I2S にも AW88298 にも触らない)

    ack_t ack[STACKEE_AUDIO_ACK_MAX];
    int   ack_count;

    // 再生中のもの
    const int16_t *play_pcm;
    int      play_samples;
    int      play_pos;
    bool     play_active;
    bool     play_is_ack;
    int      play_ack;          // 鳴らしている一次回答の番号 (-1 = ちがう)
    bool     play_sub;          // その字幕を帯に出した (終わったら消す)
    bool     play_failed;
    int64_t  play_started_us;
    uint32_t play_ms;
    uint32_t play_done_samples;

    // 録音用の領域 (WAV ヘッダ 44 B を前に置いてある)
    uint8_t *rec_raw;
    int16_t *rec_pcm;

    // 会話。★ 状態機械を進めるのは audio タスクだが、console の
    //   talk.inject / talk.status が同じ構造体を触るので錠をかける。
    //
    // ★★ 実体は **PSRAM** に取る (2026-09-20)。stackee_talk_t は字幕の
    //   ページ表 (48 x 68 B) を抱えて約 4.8 KB あり、.bss に置くと内蔵 RAM を
    //   そのぶん食う。内蔵 RAM は HTTPS の握手 (ハードウェア AES が DMA 可能な
    //   内蔵 RAM を要る) の取り分で、ここが痩せると会話が
    //   `esp-aes: Failed to allocate memory` で失敗する (README §24)。
    //   触るのは audio タスクと console タスクだけで、**割り込みからは触らない**
    //   ので PSRAM でよい。
    SemaphoreHandle_t talk_lock;
    stackee_talk_t *talk;
    _Atomic bool talk_pressed;

    // audio.selftest の依頼と結果
    _Atomic int  selftest_req;      // 0 なし / 1 依頼 / 2 実行中 / 3 完了
    uint32_t selftest_samples;
    uint32_t selftest_rms;
    // 20 ms の窓ごとの RMS の最大値。会話の「無音で捨てる」の閾値を
    // 決める材料 (stackee_talk_voice_rms と同じ測り方)。
    uint32_t selftest_rms_max;
    int      selftest_rms_at;       // 最大だった窓の番号 (-1 = 測れなかった)
    uint32_t selftest_rms_mean;     // 窓ごとの RMS の平均
    // ---- 立ち上がりの計測 (research/stackee/record_onset_2026-09-21.md) ----
    bool     onset_valid;
    bool     onset_was_open;    // 測る前からマイクが開いていた (UAC など)
    uint32_t onset_open_ms;     // enter_mic() 全体
    uint32_t onset_i2c_us;      // そのうち ES7210 の I2C 設定
    uint32_t onset_enable_us;   // そのうち enable(true) ぶん
    uint32_t onset_first_ms;    // i2s_channel_enable → 最初の DMA バッファ
    int      onset_csm_before;  // enable の直後に読んだ CSM_STATE
    int      onset_csm_ms;      // → normal になるまで [ms] (-1 = 届かなかった)
    int      onset_csm_polls;
    uint16_t onset_head_rms[STACKEE_AUDIO_HEAD_SLOTS];  // 50 ms ごとの RMS
    int      selftest_peak;
    uint32_t selftest_ms;
    char     selftest_err[64];

    // audio.play (ヌル出力での再生検査)
    _Atomic int play_req;           // -1 なし / 0.. 一次回答の番号
    // talk.inject。★ console タスクから状態機械を回さない (I2S とコーデックを
    //   別タスクから触ることになる)。ここに置いて audio タスクが拾う。
    _Atomic int  inject_req;        // -1 なし / 0.. 送るサンプル数
    _Atomic int  inject_src;        // 使う一次回答の番号
    _Atomic int  inject_result;     // 0 未処理 / 1 受理 / -1 断られた
    uint32_t stat_records, stat_plays;
    uint32_t last_key_events;
} a;

// ---------------------------------------------------------------------------
// I2S の開け閉め (半二重)
//
// ★★ I2S の口は **一度だけ作って、二度と消さない**。切り替えは
//   i2s_channel_enable / i2s_channel_disable だけでやる。
//   理由: 録音・再生のたびに i2s_del_channel すると GDMA チャネルの状態が
//   残り、次にそのチャネルを掴んだ暗号エンジン (AES/SHA) が壊れた出力を出す
//   (esp-idf#18640)。HTTPS が「署名検証に失敗」で落ちていたのがこれ。
//   i2s_del_channel を呼ぶ道はもう無い (電源が切れるまで持ち続ける)。
//
// ★ BCK (G34) と WS (G33) は 1 組しか無いので、RX と TX は同じポートの
//   **全二重の組**として作る (i2s_new_channel(&cfg, &tx, &rx))。標準モードの
//   フレームは両方 16 kHz / 2 スロット x 16 bit で揃うので、ESP-IDF が自動で
//   全二重に組み、**先に std へ入れた RX がクロックの主**、あとの TX が従に
//   なる。従は BCK/WS を主から受けるので、鳴らしている間も RX は回したままに
//   する (入ってくる音は読まずに捨てる)。運用は今までどおり半二重で、
//   マイクとスピーカーを同時には使わない。
//
// ★ MCLK (G0 = 基板の緑 LED) はチャネルを作った時点から出続ける。使わない間は
//   パッドの出力だけ止めて、今までどおり「録音中だけ薄く光る」ようにする
//   (i2s_channel_reconfig_std_gpio は I2S_GPIO_UNUSED を「変えない」と
//    見なすので、止められるのは GPIO 側だけ)。
// ---------------------------------------------------------------------------
// ★ gpio_output_enable() / gpio_output_disable() は**使わない**。どちらも GPIO
//   マトリクスの出力信号を素の GPIO に付け替える (esp_driver_gpio gpio.c:228 →
//   gpio_hal_matrix_out_default)。信号を繋いだ直後に呼ぶと MCLK が剥がれ、
//   ES7210 が一度もクロックされず DIN が 0 のままになる (2026-09-18 実測、
//   常設化 1〜3 版目はこれで samples は出るのに rms=0 だった)。
//   esp_rom_gpio_connect_out_signal() 自身が出力イネーブルを立てる
//   (esp_rom/patches/esp_rom_gpio.c) ので、付け替えだけでよい。
static void mclk_out(bool on) {
    if (!a.chans_ready) {
        return;
    }
    if (on) {
        esp_rom_gpio_connect_out_signal(STACKEE_I2S_MCLK_GPIO, I2S0_MCLK_OUT_IDX, false, false);
    } else {
        gpio_set_level(STACKEE_I2S_MCLK_GPIO, 0);
        esp_rom_gpio_connect_out_signal(STACKEE_I2S_MCLK_GPIO, SIG_GPIO_OUT_IDX, false, false);
    }
}

// 最初に音を使うときに 1 回だけ通る。起動を遅くしないためにここで作る。
static esp_err_t open_channels(void) {
    if (a.chans_ready) {
        return ESP_OK;
    }
    if (a.chans_err != ESP_OK) {
        return a.chans_err;     // 一度失敗した口は作り直せない (IDF の決まり)
    }
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 8;
    chan.dma_frame_num = 240;   // 15 ms。録音の 1 周ぶんと同じ
    chan.auto_clear = true;     // 送る音が枯れたら 0 を出す (TX にだけ効く)
    esp_err_t err = i2s_new_channel(&chan, &a.tx, &a.rx);
    if (err != ESP_OK) {
        a.tx = NULL;
        a.rx = NULL;
        a.chans_err = err;
        ESP_LOGE(TAG, "I2S の口を作れない (%s)", esp_err_to_name(err));
        return err;
    }
    i2s_std_config_t rx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(STACKEE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = STACKEE_I2S_MCLK_GPIO,
            .bclk = STACKEE_I2S_BCLK_GPIO,
            .ws   = STACKEE_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = STACKEE_I2S_DIN_GPIO,
            .invert_flags = {0},
        },
    };
    // ES7210 は MIC1 を左スロットに載せる。
    rx_std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    i2s_std_config_t tx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(STACKEE_AUDIO_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            // ★ AW88298 は BCK から内部 PLL を回すので MCLK は要らない。
            //   MCLK を出すのは RX (主) の役目。
            //   BCK / WS は RX (主) が出す。従の TX に同じピンを渡しても、IDF は
            //   入力側の接続を足すだけで出力は壊さない (i2s_common.c
            //   i2s_gpio_check_and_set)。IDF の全二重の例と同じ書き方。
            .mclk = I2S_GPIO_UNUSED,
            .bclk = STACKEE_I2S_BCLK_GPIO,
            .ws   = STACKEE_I2S_WS_GPIO,
            .dout = STACKEE_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {0},
        },
    };
    // ★ 順番が意味を持つ。先の RX がクロックの主 (MCLK もここに紐づく)。
    err = i2s_channel_init_std_mode(a.rx, &rx_std);
    if (err == ESP_OK) {
        err = i2s_channel_init_std_mode(a.tx, &tx_std);
    }
    if (err != ESP_OK) {
        a.chans_err = err;
        ESP_LOGE(TAG, "I2S を標準モードにできない (%s)", esp_err_to_name(err));
        return err;
    }
    a.chans_ready = true;
    mclk_out(false);            // 録音を始めるまで緑 LED は点けない
    ESP_LOGI(TAG, "I2S 常設 (RX=主 / TX=従、%d Hz)。以後は enable/disable だけ",
             STACKEE_AUDIO_RATE);
    return ESP_OK;
}

static esp_err_t enter_off(void) {
    if (a.mode == MODE_OFF) {
        return ESP_OK;
    }
    if (a.tx_on) {
        i2s_channel_disable(a.tx);
        a.tx_on = false;
    }
    if (a.rx_on) {
        i2s_channel_disable(a.rx);
        a.rx_on = false;
    }
    mclk_out(false);
    if (a.mode == MODE_SPK) {
        stackee_aw88298_off(a.amp_was_on);
    } else if (a.mode == MODE_MIC) {
        stackee_es7210_enable(false);
    }
    a.mode = MODE_OFF;
    return ESP_OK;
}

// ★ 3 回までやり直す。firmware/kmk/stackee_halfduplex.py も、スピーカーから
//   マイクへ戻す段 (7..9) を RECOVERY_RETRIES = 3 回まで試す。ここを 1 回で
//   諦めると「鳴らしたあとマイクが死んだまま」で終わる。
#define MIC_RETRIES 3

static esp_err_t enter_mic_once(void);

static esp_err_t enter_mic(void) {
    if (a.mode == MODE_MIC) {
        return ESP_OK;
    }
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < MIC_RETRIES; i++) {
        err = enter_mic_once();
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "マイクを開けない (%s)。やり直す (%d/%d)",
                 esp_err_to_name(err), i + 1, MIC_RETRIES);
        enter_off();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "★ マイクを開けなかった。会話は使えない");
    return err;
}

static esp_err_t enter_mic_once(void) {
    enter_off();
    esp_err_t err = open_channels();
    if (err != ESP_OK) {
        return err;
    }
    // ★ MCLK を先に出す。ES7210 は MCLK が無いと初期化 (setup) が効かず、
    //   以後ゼロしか出さない (2026-09-18 実測: 常設化の 1 版目は setup のあとに
    //   MCLK を出していて samples は出るのに rms=0 だった)。
    mclk_out(true);
    err = stackee_es7210_setup();
    if (err != ESP_OK) {
        mclk_out(false);
        return err;
    }
    // ★ enable のたびに DMA の読み位置と受信待ちの行列が空になる
    //   (ESP-IDF が RX の行列を enable で reset する)。だから前の再生中に
    //   溜まった音が録音の頭に混ざらない。
    err = i2s_channel_enable(a.rx);
    if (err != ESP_OK) {
        mclk_out(false);
        return err;
    }
    a.rx_on = true;
    a.mode = MODE_MIC;
    return ESP_OK;
}

static esp_err_t enter_spk(void) {
    if (a.mode == MODE_SPK) {
        return ESP_OK;
    }
    enter_off();
    esp_err_t err = open_channels();
    if (err != ESP_OK) {
        return err;
    }
    err = stackee_aw88298_on(STACKEE_AUDIO_RATE,
                             stackee_volume_percent(), &a.amp_was_on);
    if (err != ESP_OK) {
        return err;
    }
    // ★ TX は全二重の従で、BCK/WS を RX (主) から受ける。だから鳴らす間も
    //   RX を回す。ES7210 は落としてあるので、入ってくるものは読まずに捨てる。
    //   MCLK は出さない (AW88298 には要らない = 緑 LED も点かない)。
    err = i2s_channel_enable(a.rx);
    if (err == ESP_OK) {
        a.rx_on = true;
        err = i2s_channel_enable(a.tx);
    }
    if (err != ESP_OK) {
        if (a.rx_on) {
            i2s_channel_disable(a.rx);
            a.rx_on = false;
        }
        stackee_aw88298_off(a.amp_was_on);
        return err;
    }
    a.tx_on = true;
    a.mode = MODE_SPK;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// 再生 (1 周 1 かたまり)
// ---------------------------------------------------------------------------
static bool play_begin(const int16_t *pcm, int samples, bool is_ack) {
    if (a.play_active || pcm == NULL || samples <= 0) {
        return false;
    }
    a.play_pcm = pcm;
    a.play_samples = samples;
    a.play_pos = 0;
    a.play_is_ack = is_ack;
    a.play_ack = -1;
    a.play_failed = false;
    a.play_started_us = esp_timer_get_time();
    a.play_ms = 0;
    a.play_done_samples = 0;
    if (atomic_load(&a.null_out)) {
        // ★ ヌル出力。I2S も AW88298 も一切触らない。
        a.play_active = true;
        a.stat_plays++;
        return true;
    }
    if (enter_spk() != ESP_OK) {
        ESP_LOGE(TAG, "スピーカーを開けない");
        a.play_pcm = NULL;
        return false;
    }
    a.play_active = true;
    a.stat_plays++;
    return true;
}

static void play_step(void) {
    if (!a.play_active) {
        return;
    }
    if (atomic_load(&a.null_out)) {
        // 実時間ぶんだけカウンタを進める (DMA が出すのと同じ速さ)。
        int64_t elapsed_us = esp_timer_get_time() - a.play_started_us;
        int want = (int)((elapsed_us * STACKEE_AUDIO_RATE) / 1000000);
        if (want > a.play_samples) {
            want = a.play_samples;
        }
        a.play_pos = want;
    } else if (!a.tx_on) {
        // 送り先が無い。ここで諦めないと play_active が下りず、会話が
        // playing のまま固まる。
        a.play_failed = true;
    } else {
        static int16_t stereo[PLAY_CHUNK * 2];
        while (a.play_pos < a.play_samples) {
            int n = a.play_samples - a.play_pos;
            if (n > PLAY_CHUNK) {
                n = PLAY_CHUNK;
            }
            for (int i = 0; i < n; i++) {
                int16_t v = a.play_pcm[a.play_pos + i];
                stereo[i * 2] = v;
                stereo[i * 2 + 1] = v;
            }
            size_t written = 0;
            esp_err_t err = i2s_channel_write(a.tx, stereo, (size_t)n * 4, &written, 0);
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
                a.play_failed = true;
                break;
            }
            a.play_pos += (int)(written / 4);
            if (written < (size_t)n * 4) {
                break;              // DMA が満杯。次の周期へ
            }
        }
    }
    if (a.play_pos < a.play_samples && !a.play_failed) {
        return;
    }
    // 出し切った。DMA が吐き終わるまで待ってから畳む。
    // ★ 「書き終わった」= 「鳴り終わった」ではない。DMA には最大
    //   6 x 512 フレーム (192 ms) 残っている。現行 stackee_halfduplex.py も
    //   .playing が下りてから DRAIN_MS (200 ms) 置いてから解放している。
    //   ここで急ぐと尻切れとノイズになる。
    if (!atomic_load(&a.null_out)) {
        int64_t want_us = (int64_t)a.play_samples * 1000000 / STACKEE_AUDIO_RATE
                          + PLAY_DRAIN_MS * 1000;
        if (esp_timer_get_time() - a.play_started_us < want_us && !a.play_failed) {
            return;
        }
        enter_off();
    } else {
        int64_t want_us = (int64_t)a.play_samples * 1000000 / STACKEE_AUDIO_RATE;
        if (esp_timer_get_time() - a.play_started_us < want_us) {
            return;
        }
        // ★ 途中でヌル出力に切り替えられていても、開いたものは必ず畳む。
        enter_off();
    }
    a.play_ms = (uint32_t)((esp_timer_get_time() - a.play_started_us) / 1000);
    a.play_done_samples = (uint32_t)a.play_samples;
    a.play_active = false;
    a.play_pcm = NULL;
    // ★ 一次回答の字幕はここで消す。返答の字幕は会話の状態機械が持っている
    //   ので触らない (play_sub は一次回答を出したときだけ立つ)。
    if (a.play_sub) {
        stackee_ui_set_subtitle(NULL);
        a.play_sub = false;
    }
    a.play_ack = -1;
}

// ---------------------------------------------------------------------------
// 一次回答 (ack_01..05.pcmz)
// ---------------------------------------------------------------------------
// manifest の 1 つの ack から `"lines":["…","…"]` を取り、帯へ渡せる形
// (改行区切り、最大 STACKEE_TALK_SUB_LINES 行) にして **PSRAM** に置く。
//
// ★ 行の割り方はサーバと同じ規則で **素材を作るときに済ませてある**
//   (tools/ack_lines.py / import_faces.py)。本体は割らない。返答の字幕が
//   サーバの割った行をそのまま出すのと同じ形にそろえてある。
// ★ 帯は 3 行しか無いので **先頭 3 行だけ**使う。いまの 5 文はどれも 2 行
//   なので切られない (README §23-9)。
// ★ `lines` が無い旧い manifest では NULL を返す = 一次回答の字幕は出ない。
//   会話も音も止まらない。
static char *ack_lines(const char *obj) {
    const char *array = NULL;
    size_t array_len = 0;
    if (!stackee_json_raw(obj, "lines", &array, &array_len) || array[0] != '[') {
        return NULL;
    }
    char band[STACKEE_TALK_SUB_BAND_MAX];
    size_t at = 0;
    int used = 0;
    const char *p = array + 1;
    const char *limit = array + array_len;
    while (p < limit && used < STACKEE_TALK_SUB_LINES) {
        const char *quote = memchr(p, '"', (size_t)(limit - p));
        if (quote == NULL) {
            break;
        }
        const char *end = quote + 1;
        while (end < limit && *end != '"') {
            if (*end == '\\' && end + 1 < limit) {
                end++;
            }
            end++;
        }
        if (end >= limit) {
            break;
        }
        char text[STACKEE_TALK_SUB_TEXT_MAX];
        size_t n = stackee_json_unescape(quote, (size_t)(end + 1 - quote),
                                         text, sizeof(text));
        p = end + 1;
        if (n == 0) {
            continue;                   // 空の行は置かない
        }
        size_t need = n + ((used > 0) ? 1u : 0u);
        if (at + need + 1 > sizeof(band)) {
            break;                      // 入らない行は置かない (途中で切らない)
        }
        if (used > 0) {
            band[at++] = '\n';
        }
        memcpy(band + at, text, n);
        at += n;
        band[at] = '\0';
        used++;
    }
    if (used == 0) {
        return NULL;
    }
    char *out = heap_caps_malloc(at + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (out == NULL) {
        return NULL;                    // 字幕が出ないだけ。音は鳴る
    }
    memcpy(out, band, at + 1);
    return out;
}

// 字幕を持っている一次回答の本数 (audio.status の "ack_lines")。
static int ack_lines_ready(void);

// 一次回答を 1 本鳴らし、その字幕を帯に出す。番号を返す (失敗は -1)。
// ★ 消すのは play_step の終わり。会話の返答 (is_ack=false かつ
//   ops_play_begin 経由) の字幕には触らない。
static int start_ack(int index, bool is_ack) {
    if (index < 0 || index >= a.ack_count) {
        return -1;
    }
    if (!play_begin(a.ack[index].pcm, a.ack[index].samples, is_ack)) {
        return -1;
    }
    a.play_ack = index;
    if (a.ack[index].lines != NULL) {
        stackee_ui_set_subtitle(a.ack[index].lines);
        a.play_sub = true;
    }
    return index;
}

static void load_acks(void) {
    size_t len = 0;
    char *manifest = stackee_assets_read("manifest.json", &len, MALLOC_CAP_8BIT);
    if (manifest == NULL) {
        ESP_LOGW(TAG, "manifest.json が読めない。一次回答は無し");
        return;
    }
    // "acks":[{"file":"ack_01.pcmz","samples":53077,...}, ...]
    const char *array = NULL;
    size_t array_len = 0;
    if (!stackee_json_raw(manifest, "acks", &array, &array_len) || array[0] != '[') {
        free(manifest);
        ESP_LOGW(TAG, "manifest.json に acks が無い");
        return;
    }
    char *at = (char *)array + 1;
    char *limit = (char *)array + array_len;
    while (at < limit && a.ack_count < STACKEE_AUDIO_ACK_MAX) {
        char *obj = memchr(at, '{', (size_t)(limit - at));
        if (obj == NULL) {
            break;
        }
        char *end = memchr(obj, '}', (size_t)(limit - obj));
        if (end == NULL) {
            break;
        }
        at = end + 1;
        // ★ この 1 つの物だけを見る。閉じ括弧を一時的に NUL にして、
        //   鍵を探す範囲を物の中に閉じ込める (lines が無い物のとき、
        //   次の物の lines を拾ってしまわないように)。
        char saved = *end;
        *end = '\0';
        char name[48];
        long samples = 0;
        if (!stackee_json_str(obj, "file", name, sizeof(name)) ||
            !stackee_json_int(obj, "samples", &samples) ||
            samples <= 0 || samples > 80000) {
            *end = saved;
            continue;
        }
        size_t want = (size_t)samples * 2;
        void *pcm = stackee_assets_read_inflate(name, want,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pcm == NULL) {
            ESP_LOGW(TAG, "%s を展開できない", name);
            *end = saved;
            continue;
        }
        a.ack[a.ack_count].pcm = (int16_t *)pcm;
        a.ack[a.ack_count].samples = (int)samples;
        a.ack[a.ack_count].lines = ack_lines(obj);
        a.ack_count++;
        *end = saved;
    }
    free(manifest);
    int with_lines = 0;
    for (int i = 0; i < a.ack_count; i++) {
        with_lines += (a.ack[i].lines != NULL);
    }
    ESP_LOGI(TAG, "一次回答 %d 本 (字幕つき %d 本)", a.ack_count, with_lines);
}

static int ack_lines_ready(void) {
    int n = 0;
    for (int i = 0; i < a.ack_count; i++) {
        n += (a.ack[i].lines != NULL);
    }
    return n;
}

// ---------------------------------------------------------------------------
// 会話の状態機械が使う口
// ---------------------------------------------------------------------------
static bool talk_lock(void) {
    return a.talk_lock != NULL &&
           xSemaphoreTake(a.talk_lock, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static void talk_unlock(void) {
    if (a.talk_lock != NULL) {
        xSemaphoreGive(a.talk_lock);
    }
}

static uint32_t ops_now(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int16_t *ops_record_alloc(int max_samples) {
    if (a.rec_raw != NULL) {
        return a.rec_pcm;
    }
    size_t bytes = 44 + (size_t)max_samples * 2;
    a.rec_raw = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (a.rec_raw == NULL) {
        ESP_LOGE(TAG, "録音用の %u B を確保できない", (unsigned)bytes);
        return NULL;
    }
    a.rec_pcm = (int16_t *)(a.rec_raw + 44);
    return a.rec_pcm;
}

static void ops_record_release(void) {
    if (a.rec_raw != NULL) {
        free(a.rec_raw);
        a.rec_raw = NULL;
        a.rec_pcm = NULL;
    }
}

static bool ops_record_begin(void) {
    if (a.play_active) {
        return false;               // 半二重。鳴らしている間は録れない
    }
    if (enter_mic() != ESP_OK) {
        return false;
    }
    a.stat_records++;
    return true;
}

static int ops_record_read(int16_t *dst, int max) {
    if (a.mode != MODE_MIC || a.rx == NULL) {
        return -1;
    }
    size_t got = 0;
    esp_err_t err = i2s_channel_read(a.rx, dst, (size_t)max * 2, &got, 0);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        return -1;
    }
    return (int)(got / 2);
}

static void ops_record_end(void) {
    if (a.mode == MODE_MIC) {
        enter_off();
    }
}

static bool ops_ack_begin(void) {
    if (a.ack_count == 0 || stackee_volume_percent() == 0) {
        return false;               // 現行と同じ: 音量 0 なら鳴らさない
    }
    int index = (int)(esp_random() % (uint32_t)a.ack_count);
    return start_ack(index, true) >= 0;
}

static bool ops_ack_active(void) {
    return a.play_active && a.play_is_ack;
}

static bool ops_play_begin(const int16_t *pcm, int samples) {
    return play_begin(pcm, samples, false);
}

static bool ops_play_active(void) {
    return a.play_active && !a.play_is_ack;
}

static bool ops_play_failed(void) {
    return a.play_failed;
}

static bool ops_net_ready(void) {
    return stackee_wifi_connected() && stackee_http_configured();
}

static bool ops_http_start(const char *method, const char *path,
                           const void *body, size_t body_len, size_t limit) {
    return stackee_http_request(method, path, body, body_len, limit);
}

static int ops_http_poll(int *status, const uint8_t **body, size_t *len) {
    int state = stackee_http_state(status, NULL, NULL, NULL);
    if (state == STACKEE_HTTP_RUNNING) {
        return 0;
    }
    if (state == STACKEE_HTTP_DONE) {
        *body = stackee_http_body(len);
        return 1;
    }
    if (state == STACKEE_HTTP_ERROR) {
        return -1;
    }
    return 0;
}

static void ops_http_close(void) {
    stackee_http_close();
}

// RIFF/WAVE の 44 バイト (stackee_talk.py の wav_header と同じ並び)。
static void ops_wav_header(int16_t *pcm, int samples) {
    uint8_t *h = (uint8_t *)pcm - 44;
    uint32_t size = (uint32_t)samples * 2;
    uint32_t rate = STACKEE_AUDIO_RATE;
    memcpy(h + 0, "RIFF", 4);
    uint32_t riff = size + 36;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    uint32_t fmt_len = 16;
    memcpy(h + 16, &fmt_len, 4);
    uint16_t pcm_tag = 1, channels = 1, align = 2, bits = 16;
    memcpy(h + 20, &pcm_tag, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    uint32_t byte_rate = rate * 2;
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &size, 4);
}

static void ops_show(const char *text) {
    stackee_ui_set_screen(text);
}

static void ops_log(const char *line) {
    ESP_LOGI(TAG, "%s", line);
}

static void ops_face(bool recording, bool busy, bool speaking) {
    stackee_ui_set_talk(recording, busy, speaking);
}

// 字幕の 1 行。★ 描くのは ui タスク。ここは文字列を置くだけなので、
//   再生の周期 (audio タスク) が描画に待たされることはない。
static void ops_subtitle(const char *text) {
    stackee_ui_set_subtitle(text);
}

static const stackee_talk_ops_t TALK_OPS = {
    .now_ms = ops_now,
    .record_alloc = ops_record_alloc,
    .record_release = ops_record_release,
    .record_begin = ops_record_begin,
    .record_read = ops_record_read,
    .record_end = ops_record_end,
    .ack_begin = ops_ack_begin,
    .ack_active = ops_ack_active,
    .play_begin = ops_play_begin,
    .play_active = ops_play_active,
    .play_failed = ops_play_failed,
    .net_ready = ops_net_ready,
    .http_start = ops_http_start,
    .http_poll = ops_http_poll,
    .http_close = ops_http_close,
    .wav_header = ops_wav_header,
    .show = ops_show,
    .log = ops_log,
    .face = ops_face,
    .subtitle = ops_subtitle,
};

// ---------------------------------------------------------------------------
// 立ち上がりの計測 (research/stackee/record_onset_2026-09-21.md)
// ---------------------------------------------------------------------------
// ★ **無音のまま、人手ゼロで**「押してから使える音が録れ始めるまで」の形を
//   数字にする。鳴らすものは何も無い (マイクを開けて 500 ms 録るだけ)。
//
//   onset_open_ms    enter_mic() 全体 (押下の道でそのまま待たされるぶん)
//   onset_i2c_us     そのうち ES7210 を I2C で設定するぶん
//   onset_first_ms   i2s_channel_enable → 最初の DMA バッファが返るまで
//   onset_csm_ms     同 → ES7210 の CSM_STATE が normal になるまで
//                    (0x0B。LRCK を数えて進むので I2S を止めている間は進まない)
//   head_rms[10]     先頭 500 ms を 50 ms ずつ。立ち上がりの跳ねが見える
//
// ★ CSM を読む I2C の往復と DMA の汲み出しは**同じ輪**で回す。汲まずに
//   ポーリングだけすると DMA が溢れて head_rms が壊れる。
static void run_onset(void) {
    a.onset_valid = false;
    memset(a.onset_head_rms, 0, sizeof(a.onset_head_rms));
    a.onset_csm_ms = -1;
    a.onset_csm_polls = 0;
    a.onset_csm_before = -1;
    a.onset_first_ms = 0;

    // ★ 冷えた状態から測る。UAC が握っていたら閉じられないので、その旨を出す。
    a.onset_was_open = (a.mode == MODE_MIC);
    if (!a.onset_was_open) {
        enter_off();
    }
    int64_t t_open = esp_timer_get_time();
    if (enter_mic() != ESP_OK) {
        snprintf(a.selftest_err, sizeof(a.selftest_err), "マイクを開けない");
        return;
    }
    int64_t t_enabled = esp_timer_get_time();
    a.onset_open_ms = (uint32_t)((t_enabled - t_open) / 1000);
    a.onset_i2c_us = stackee_es7210_last_setup_us();
    a.onset_enable_us = stackee_es7210_last_enable_us();
    a.onset_csm_before = stackee_es7210_csm_state();

    static int16_t chunk[512];
    uint64_t slot_square = 0;
    uint32_t slot_count = 0;
    int      slot = 0;
    uint32_t got_total = 0;
    int64_t  next_csm = t_enabled;
    for (;;) {
        int64_t now = esp_timer_get_time();
        uint32_t since_ms = (uint32_t)((now - t_enabled) / 1000);
        if (since_ms >= HEAD_MS) {
            break;
        }
        // CSM を 2 ms おきに 1 回だけ読む (normal になるまで)。
        if (a.onset_csm_ms < 0 && since_ms < CSM_TIMEOUT_MS && now >= next_csm) {
            next_csm = now + 2000;
            a.onset_csm_polls++;
            if (stackee_es7210_csm_state() == STACKEE_ES7210_CSM_NORMAL) {
                a.onset_csm_ms = (int)((esp_timer_get_time() - t_enabled) / 1000);
            }
        }
        size_t got = 0;
        esp_err_t err = i2s_channel_read(a.rx, chunk, sizeof(chunk), &got,
                                         pdMS_TO_TICKS(5));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            snprintf(a.selftest_err, sizeof(a.selftest_err), "i2s_read %s",
                     esp_err_to_name(err));
            break;
        }
        size_t n = got / 2;
        if (n > 0 && got_total == 0) {
            a.onset_first_ms = (uint32_t)((esp_timer_get_time() - t_enabled) / 1000);
        }
        got_total += (uint32_t)n;
        for (size_t i = 0; i < n; i++) {
            slot_square += (uint64_t)((int32_t)chunk[i] * chunk[i]);
            if (++slot_count >= (uint32_t)(STACKEE_AUDIO_RATE * HEAD_SLOT_MS / 1000)) {
                uint32_t mean = (uint32_t)(slot_square / slot_count);
                uint32_t root = 0;
                while ((uint64_t)(root + 1) * (root + 1) <= mean) {
                    root++;
                }
                if (slot < STACKEE_AUDIO_HEAD_SLOTS) {
                    a.onset_head_rms[slot++] = (uint16_t)
                        (root > 0xFFFF ? 0xFFFF : root);
                }
                slot_square = 0;
                slot_count = 0;
            }
        }
    }
    a.selftest_samples = got_total;
    // 測ったら畳む (元から開いていたなら開けたまま)。
    if (!a.onset_was_open) {
        enter_off();
    }
    a.onset_valid = true;
}

// ---------------------------------------------------------------------------
// audio.selftest (無音でよい。I2S の DMA が動いている証拠を数字で出す)
// ---------------------------------------------------------------------------
static void run_selftest(void) {
    atomic_store(&a.selftest_req, 2);
    a.selftest_err[0] = '\0';
    a.selftest_samples = 0;
    a.selftest_rms = 0;
    a.selftest_rms_max = 0;
    a.selftest_rms_at = -1;
    a.selftest_rms_mean = 0;
    a.selftest_peak = 0;
    if (enter_mic() != ESP_OK) {
        snprintf(a.selftest_err, sizeof(a.selftest_err), "マイクを開けない");
        atomic_store(&a.selftest_req, 3);
        return;
    }
    static int16_t chunk[512];
    int64_t t0 = esp_timer_get_time();
    uint64_t square = 0;
    uint32_t count = 0;
    int peak = 0;
    // 窓 (20 ms) ごとの RMS の最大値。chunk をまたいで数え続ける。
    uint64_t win_square = 0;
    uint32_t win_count = 0;
    uint32_t win_best = 0;
    int      win_best_at = -1;
    int      win_index = 0;
    uint64_t win_sum = 0;
    while ((esp_timer_get_time() - t0) < RECORD_MAX_MS * 1000) {
        size_t got = 0;
        esp_err_t err = i2s_channel_read(a.rx, chunk, sizeof(chunk), &got,
                                         pdMS_TO_TICKS(50));
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            snprintf(a.selftest_err, sizeof(a.selftest_err), "i2s_read %s",
                     esp_err_to_name(err));
            break;
        }
        size_t n = got / 2;
        for (size_t i = 0; i < n; i++) {
            int v = chunk[i];
            if (v < 0) { v = -v; }
            if (v > peak) { peak = v; }
            square += (uint64_t)((int64_t)chunk[i] * chunk[i]);
            win_square += (uint64_t)((int32_t)chunk[i] * chunk[i]);
            if (++win_count >= STACKEE_TALK_RMS_WINDOW) {
                uint32_t mean = (uint32_t)(win_square / win_count);
                uint32_t root = 0;
                while ((uint64_t)(root + 1) * (root + 1) <= mean) {
                    root++;
                }
                if (root > win_best) {
                    win_best = root;
                    win_best_at = win_index;
                }
                win_sum += root;
                win_index++;
                win_square = 0;
                win_count = 0;
            }
        }
        count += (uint32_t)n;
    }
    a.selftest_rms_max = win_best;
    a.selftest_rms_at = win_best_at;
    a.selftest_rms_mean = win_index ? (uint32_t)(win_sum / (uint64_t)win_index) : 0;
    a.selftest_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    a.selftest_samples = count;
    a.selftest_peak = peak;
    if (count > 0) {
        uint64_t mean = square / count;
        // 整数平方根。浮動小数の書式を使わずに済ませる。
        uint32_t root = 0;
        while ((uint64_t)(root + 1) * (root + 1) <= mean) {
            root++;
        }
        a.selftest_rms = root;
    }
    enter_off();
    atomic_store(&a.selftest_req, 3);
}

// ---------------------------------------------------------------------------
// audio タスク
// ---------------------------------------------------------------------------
static void audio_task(void *unused) {
    (void)unused;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        if (atomic_load(&a.selftest_req) == 1) {
            run_selftest();
            continue;
        }
        if (atomic_load(&a.selftest_req) == 4) {
            a.selftest_err[0] = '\0';
            run_onset();
            atomic_store(&a.selftest_req, 3);
            continue;
        }
        int want = atomic_exchange(&a.play_req, -1);
        if (want >= 0 && want < a.ack_count && !a.play_active) {
            start_ack(want, false);
        }
        play_step();
        if (talk_lock()) {
            int inject = atomic_exchange(&a.inject_req, -1);
            if (inject > 0) {
                int src = atomic_load(&a.inject_src);
                bool ok = (src >= 0 && src < a.ack_count) &&
                          stackee_talk_inject(a.talk, a.ack[src].pcm, inject);
                atomic_store(&a.inject_result, ok ? 1 : -1);
            }
            stackee_talk_set_pressed(a.talk, atomic_load(&a.talk_pressed));
            stackee_talk_step(a.talk);
            talk_unlock();
        }
        // 打鍵を見張る。★ 入力タスクには触らない (数えた数を読むだけ)。
        // 音量の保存は「打鍵が 2 秒無い」ときにしかしない。
        stackee_input_stats_t input;
        stackee_input_stats(&input);
        if (input.key_events != a.last_key_events || input.keys_down > 0) {
            a.last_key_events = input.key_events;
            stackee_volume_note_input();
        }
        stackee_volume_task_step(stackee_audio_busy());
        // 段階 4: USB マイク (full プロファイル)。★ 会話が優先。
        //   会話中・再生中は無音を送り、マイクは会話に明け渡す。
        stackee_uac_step();
    }
}

// ---------------------------------------------------------------------------
// console
// ---------------------------------------------------------------------------
static size_t put(char *buf, size_t cap, size_t at, const char *fmt, ...) {
    if (at >= cap) {
        return at;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf + at, cap - at, fmt, args);
    va_end(args);
    return (n < 0) ? at : at + (size_t)n;
}

// JSON の文字列として安全に出す (返答文は任意の日本語)。
static size_t put_json_str(char *buf, size_t cap, size_t at, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p && at + 8 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            at = put(buf, cap, at, "\\%c", *p);
        } else if (*p < 0x20) {
            at = put(buf, cap, at, "\\u%04X", *p);
        } else {
            at = put(buf, cap, at, "%c", *p);
        }
    }
    return at;
}

static size_t reply_talk_status(long id, char *buf, size_t cap) {
    if (!talk_lock()) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    const stackee_talk_t *t = a.talk;
    stackee_http_stats_t http;
    stackee_http_stats(&http);
    size_t at = put(buf, cap, 0,
                    "{\"id\":%ld,\"ok\":1,\"state\":\"%s\",\"polls\":%d,"
                    "\"accepted_ms\":%lu,\"reply_ready_ms\":%lu,"
                    "\"audio_ready_ms\":%lu,\"play_setup_ms\":%lu,"
                    "\"complete_ms\":%lu,\"audio_samples\":%d,"
                    "\"audio_duration_ms\":%d,\"turns\":%lu,\"errors\":%lu,"
                    "\"ignored\":%lu,\"http\":{\"state\":%d,\"status\":%d,"
                    "\"err\":%d,\"requests\":%lu,\"failures\":%lu,"
                    "\"last_ms\":%lu,\"last_bytes\":%u},\"null\":%s,"
                    "\"sub_pages\":%d,\"sub_page\":%d,\"sub_bytes\":%lu,"
                    "\"sub_dropped\":%d,\"subs_ok\":%lu,\"subs_failed\":%lu,"
                    "\"sub_src\":\"%s\","
                    "\"dropped_short\":%lu,\"dropped_silent\":%lu,"
                    "\"rec_ms\":%lu,\"first_sample_ms\":%lu,\"rms_max\":%lu,"
                    "\"rms_at\":%d,\"rms_mean\":%lu,\"rms_2nd\":%lu,"
                    "\"loud\":%lu,"
                    "\"min_ms\":%lu,\"voice_rms\":%lu,\"voice_windows\":%lu",
                    id, stackee_talk_state_names[t->state], t->polls,
                    (unsigned long)t->accepted_ms, (unsigned long)t->reply_ready_ms,
                    (unsigned long)t->audio_ready_ms, (unsigned long)t->play_setup_ms,
                    (unsigned long)t->complete_ms, t->audio_samples,
                    t->audio_duration_ms, (unsigned long)t->turns,
                    (unsigned long)t->errors, (unsigned long)t->ignored,
                    http.state, http.status, http.err,
                    (unsigned long)http.requests, (unsigned long)http.failures,
                    (unsigned long)http.last_ms, (unsigned)http.last_bytes,
                    atomic_load(&a.null_out) ? "true" : "false",
                    t->page_count, t->page_shown, (unsigned long)t->sub_bytes,
                    t->sub_dropped, (unsigned long)t->subs_ok,
                    (unsigned long)t->subs_failed,
                    stackee_talk_sub_src_names[t->sub_src],
                    (unsigned long)t->dropped_short,
                    (unsigned long)t->dropped_silent,
                    (unsigned long)t->last_rec_ms,
                    (unsigned long)t->first_sample_ms,
                    (unsigned long)t->last_rms_max,
                    t->last_rms_at, (unsigned long)t->last_rms_mean,
                    (unsigned long)t->last_rms_2nd, (unsigned long)t->last_loud,
                    (unsigned long)t->min_ms, (unsigned long)t->voice_rms,
                    (unsigned long)t->voice_windows);
    at = put(buf, cap, at, ",\"reply\":\"");
    at = put_json_str(buf, cap, at, t->reply);
    at = put(buf, cap, at, "\",\"error\":\"");
    at = put_json_str(buf, cap, at, t->error);
    at = put(buf, cap, at, "\"}");
    talk_unlock();
    return at;
}

static size_t reply_talk_inject(long id, const char *line, char *buf, size_t cap) {
    long ms = stackee_console_int(line, "ms", 1000);
    long which = stackee_console_int(line, "i", 0);
    if (ms < 300)   { ms = 300; }
    if (ms > 30000) { ms = 30000; }
    if (which < 0 || which >= a.ack_count) {
        which = 0;
    }
    if (a.ack_count == 0) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"noack\"}", id);
    }
    int samples = (int)(ms * STACKEE_AUDIO_RATE / 1000);
    if (samples > a.ack[which].samples) {
        samples = a.ack[which].samples;
    }
    // ★ ここでは頼むだけ。実際に状態機械を回すのは audio タスク。
    //   console タスクから回すと、その先で I2S とコーデックを別タスクから
    //   触ることになる (一次回答の再生がすぐ始まるため)。
    if (atomic_load(&a.inject_req) >= 0) {
        return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
    }
    atomic_store(&a.inject_result, 0);
    atomic_store(&a.inject_src, (int)which);
    atomic_store(&a.inject_req, samples);
    for (int i = 0; i < 200 && atomic_load(&a.inject_result) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    int result = atomic_load(&a.inject_result);
    if (result != 1) {
        return put(buf, cap, 0,
                   "{\"id\":%ld,\"error\":\"busy\",\"state\":\"%s\",\"why\":\"%s\"}",
                   id, stackee_talk_state_names[a.talk->state], a.talk->error);
    }
    return put(buf, cap, 0,
               "{\"id\":%ld,\"ok\":1,\"state\":\"%s\",\"samples\":%d,\"ms\":%ld}",
               id, stackee_talk_state_names[a.talk->state], samples, ms);
}

static size_t audio_console(const char *cmd, const char *line, long id,
                            char *buf, size_t cap) {
    if (!a.ready) {
        return 0;
    }
    if (strcmp(cmd, "audio.null") == 0) {
        // ★ 鳴っている最中に切り替えない。途中で経路が入れ替わると
        //   再生が終われなくなる。
        if (a.play_active) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
        }
        bool on = stackee_console_bool(line, "on", true);
        if (stackee_console_value(line, "on") == NULL) {
            // {"cmd":"audio.null","n":1} でも {"cmd":"audio.null"} でも受ける。
            on = stackee_console_int(line, "n", 1) != 0;
        }
        atomic_store(&a.null_out, on);
        ESP_LOGW(TAG, "ヌル出力 %s", on ? "ON (音は出ない)" : "OFF (音が出る)");
        return put(buf, cap, 0, "{\"id\":%ld,\"ok\":1,\"null\":%s}", id,
                   on ? "true" : "false");
    }
    if (strcmp(cmd, "audio.selftest") == 0) {
        // ★ 鳴っている最中は断る。マイクを開くと I2S の TX が消えて、
        //   再生が終われないまま audio タスクが止まる (= 会話も Wi-Fi も
        //   道連れ)。audio.play と同じ守り。
        if (a.play_active || stackee_talk_busy(a.talk)) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
        }
        // ★ `{"onset":1}` … 立ち上がりの計測 (押下 → 使える音までの形)。
        bool onset = stackee_console_bool(line, "onset", false);
        int state = atomic_load(&a.selftest_req);
        if (state == 0 || state == 3) {
            atomic_store(&a.selftest_req, onset ? 4 : 1);
            // audio タスクが 1 秒録る。終わるまでここで待つ (最大 3 秒)。
            for (int i = 0; i < 300; i++) {
                vTaskDelay(pdMS_TO_TICKS(10));
                if (atomic_load(&a.selftest_req) == 3) {
                    break;
                }
            }
        }
        if (atomic_load(&a.selftest_req) != 3) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"timeout\"}", id);
        }
        if (onset) {
            size_t at = put(buf, cap, 0,
                            "{\"id\":%ld,\"ok\":%s,\"onset\":1,\"was_open\":%s,"
                            "\"open_ms\":%lu,\"i2c_us\":%lu,\"enable_us\":%lu,"
                            "\"first_ms\":%lu,\"csm_before\":%d,\"csm_ms\":%d,"
                            "\"csm_polls\":%d,\"samples\":%lu,\"slot_ms\":%d,"
                            "\"head_rms\":[",
                            id, a.selftest_err[0] ? "0" : "1",
                            a.onset_was_open ? "true" : "false",
                            (unsigned long)a.onset_open_ms,
                            (unsigned long)a.onset_i2c_us,
                            (unsigned long)a.onset_enable_us,
                            (unsigned long)a.onset_first_ms,
                            a.onset_csm_before, a.onset_csm_ms,
                            a.onset_csm_polls,
                            (unsigned long)a.selftest_samples, HEAD_SLOT_MS);
            for (int i = 0; i < STACKEE_AUDIO_HEAD_SLOTS; i++) {
                at = put(buf, cap, at, "%s%u", i ? "," : "",
                         (unsigned)a.onset_head_rms[i]);
            }
            return put(buf, cap, at, "],\"error\":\"%s\"}", a.selftest_err);
        }
        return put(buf, cap, 0,
                   "{\"id\":%ld,\"ok\":%s,\"samples\":%lu,\"ms\":%lu,\"rms\":%lu,"
                   "\"rms_max\":%lu,\"rms_at\":%d,\"rms_mean\":%lu,"
                   "\"peak\":%d,\"rate\":%d,\"error\":\"%s\"}",
                   id, a.selftest_err[0] ? "0" : "1",
                   (unsigned long)a.selftest_samples, (unsigned long)a.selftest_ms,
                   (unsigned long)a.selftest_rms,
                   (unsigned long)a.selftest_rms_max, a.selftest_rms_at,
                   (unsigned long)a.selftest_rms_mean, a.selftest_peak,
                   STACKEE_AUDIO_RATE, a.selftest_err);
    }
    if (strcmp(cmd, "audio.play") == 0) {
        long which = stackee_console_int(line, "i", 0);
        if (a.ack_count == 0) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"noack\"}", id);
        }
        if (which < 0 || which >= a.ack_count) {
            which = 0;
        }
        if (a.play_active || stackee_talk_busy(a.talk)) {
            return put(buf, cap, 0, "{\"id\":%ld,\"error\":\"busy\"}", id);
        }
        atomic_store(&a.play_req, (int)which);
        return put(buf, cap, 0,
                   "{\"id\":%ld,\"ok\":1,\"i\":%ld,\"samples\":%d,\"expect_ms\":%d,"
                   "\"null\":%s}",
                   id, which, a.ack[which].samples,
                   a.ack[which].samples * 1000 / STACKEE_AUDIO_RATE,
                   atomic_load(&a.null_out) ? "true" : "false");
    }
    if (strcmp(cmd, "audio.status") == 0) {
        return put(buf, cap, 0,
                   "{\"id\":%ld,\"ok\":1,\"mode\":%d,\"null\":%s,\"playing\":%s,"
                   "\"is_ack\":%s,\"ack\":%d,\"ack_sub\":%s,\"ack_lines\":%d,"
                   "\"pos\":%d,\"samples\":%d,\"played\":%lu,"
                   "\"play_ms\":%lu,\"failed\":%s,\"acks\":%d,\"volume\":%d,"
                   "\"records\":%lu,\"plays\":%lu,\"codec\":%s,\"amp\":%s}",
                   id, (int)a.mode, atomic_load(&a.null_out) ? "true" : "false",
                   a.play_active ? "true" : "false",
                   a.play_is_ack ? "true" : "false",
                   a.play_ack, a.play_sub ? "true" : "false", ack_lines_ready(),
                   a.play_pos, a.play_samples, (unsigned long)a.play_done_samples,
                   (unsigned long)a.play_ms, a.play_failed ? "true" : "false",
                   a.ack_count, stackee_volume_percent(),
                   (unsigned long)a.stat_records, (unsigned long)a.stat_plays,
                   stackee_codec_ready() ? "true" : "false",
                   stackee_aw88298_powered() ? "true" : "false");
    }
    if (strcmp(cmd, "talk.inject") == 0) {
        return reply_talk_inject(id, line, buf, cap);
    }
    if (strcmp(cmd, "talk.status") == 0) {
        return reply_talk_status(id, buf, cap);
    }
    return 0;
}

// ---------------------------------------------------------------------------
esp_err_t stackee_audio_start(const char *post_path) {
    if (a.ready) {
        return ESP_OK;
    }
    atomic_store(&a.play_req, -1);
    atomic_store(&a.inject_req, -1);
    a.play_ack = -1;
    a.talk_lock = xSemaphoreCreateMutex();
    if (a.talk_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    // ★ 会話の状態機械は PSRAM に置く (内蔵 RAM は HTTPS の握手に残す)。
    //   PSRAM が無い / 取れない機体では内蔵 RAM に落ちる (動きは同じ)。
    a.talk = heap_caps_calloc(1, sizeof(*a.talk),
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (a.talk == NULL) {
        a.talk = heap_caps_calloc(1, sizeof(*a.talk), MALLOC_CAP_8BIT);
    }
    if (a.talk == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = stackee_codec_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "コーデックを登録できない。音は出ない");
        return err;
    }
    load_acks();
    stackee_talk_init(a.talk, &TALK_OPS, post_path);
    // ★ 短押し / 無音の切り捨ての閾値は settings.toml で変えられる。
    //   無ければ既定 (stackee_talksm.h)。0 を書くとその条件を見なくなる。
    stackee_talk_set_gate(
        a.talk,
        (uint32_t)stackee_settings_int("STACKEE_TALK_MIN_MS",
                                       STACKEE_TALK_MIN_MS_DEFAULT),
        (uint32_t)stackee_settings_int("STACKEE_TALK_VOICE_RMS",
                                       STACKEE_TALK_VOICE_RMS_DEFAULT),
        (uint32_t)stackee_settings_int("STACKEE_TALK_VOICE_WINDOWS",
                                       STACKEE_TALK_VOICE_WINDOWS_DEFAULT));
    ESP_LOGI(TAG, "会話の切り捨て: 最短 %lu ms / 声の RMS %lu x %lu 窓",
             (unsigned long)a.talk->min_ms, (unsigned long)a.talk->voice_rms,
             (unsigned long)a.talk->voice_windows);
    a.ready = true;
    stackee_console_register(audio_console);
    // CPU0 / 高優先度 (ui = 3 より上、入力 = CPU1 とは別)。
    if (xTaskCreatePinnedToCore(audio_task, "stackee_audio", 6144, NULL, 5, &a.task, 0)
            != pdPASS) {
        a.ready = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool stackee_audio_ready(void) {
    return a.ready;
}

// ★ a.talk は stackee_audio_start が PSRAM に取る。起きる前に外から
//   聞かれても落ちないように、ここで NULL を吸う。
static bool talk_busy(void) {
    return a.talk != NULL && stackee_talk_busy(a.talk);
}

bool stackee_audio_busy(void) {
    return a.play_active || a.mode == MODE_MIC || talk_busy();
}

bool stackee_audio_recording(void) {
    return a.talk != NULL && a.talk->state == STACKEE_TALK_RECORDING;
}

bool stackee_audio_speaking(void) {
    return a.play_active;
}

// ---------------------------------------------------------------------------
// 段階 4: USB マイク (UAC) が使う口
// ---------------------------------------------------------------------------
// ★ 呼んでよいのは **audio タスク**だけ (半二重の切り替えを 1 本に保つ)。
//
// ★ 会話が最優先。STK_TALK で録音中・返答の再生中・一次回答の再生中は
//   -1 を返し、UAC 側は無音を送る。キーボードの本業 (会話) を
//   USB マイクの都合で止めない、という決め方。
int stackee_audio_uac_pull(int16_t *dst, int max_samples) {
    if (dst == NULL || max_samples <= 0 || !a.ready) {
        return -1;
    }
    if (a.play_active || talk_busy() || atomic_load(&a.talk_pressed)) {
        return -1;              // 会話が使っている
    }
    if (a.mode != MODE_MIC) {
        if (enter_mic() != ESP_OK) {
            return -1;
        }
    }
    size_t got = 0;
    if (i2s_channel_read(a.rx, dst, (size_t)max_samples * 2, &got, 0) != ESP_OK) {
        return 0;
    }
    return (int)(got / 2);
}

// UAC が閉じたときに呼ぶ。会話が使っていなければ I2S を畳む。
void stackee_audio_uac_release(void) {
    if (a.mode == MODE_MIC && !a.play_active && !talk_busy() &&
        !atomic_load(&a.talk_pressed)) {
        enter_off();
    }
}

void stackee_audio_talk_key(bool pressed) {
    atomic_store(&a.talk_pressed, pressed);
}

void stackee_audio_set_null(bool on) {
    atomic_store(&a.null_out, on);
}

bool stackee_audio_null(void) {
    return atomic_load(&a.null_out);
}

const char *stackee_audio_talk_state(void) {
    return a.ready ? stackee_talk_state_names[a.talk->state] : "off";
}

const char *stackee_audio_screen(void) {
    return (a.talk != NULL) ? a.talk->screen : "";
}
