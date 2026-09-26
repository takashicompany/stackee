// カメラ (GC0308 @ CoreS3)。DESIGN.md §3 の「camera」タスク。
//
//   AXP2101 の ALDO3 で電源 → esp32-camera で撮る → JPEG にする
//   → HTTP で送る / console から取り出す
//
// ★ 現行 CircuitPython 版 (firmware/kmk/stackee_camera.py) の知見を
//   そのまま持ってきてある。特に次の 3 つは実測で決まった値なので動かさない:
//
//   1. **ALDO3 を入れてから 1 秒置く**。50ms では素子が起きず、
//      2 枚目以降が必ず失敗する (2026-09-11 実測)。
//   2. **捨て駒 30 枚**。1 枚だと色が決まらない。60 / 120 枚に増やしても
//      撮るたびのばらつき (±0.005) より小さい差しか無く、時間だけ倍になる。
//   3. **撮り終えたら必ず ALDO3 を切る**。切らずにハードリセットすると
//      GC0308 が自走したままになり、ブートループになる。
//
// ★ XCLK は出さない (pin_xclk = -1)。CoreS3 の XCLK は GPIO2 = PORT.A の
//   SDA で、そこには TCA8418 (キーマトリクス) がぶら下がっている。
//   ここを掴むとキーボードが死ぬ。現行 CircuitPython 版も
//   external_clock_pin を渡していない (それで撮れている)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 撮る大きさ。現行と同じ 2 つだけ。
typedef enum {
    STACKEE_CAM_QVGA = 0,   // 320x240
    STACKEE_CAM_QQVGA,      // 160x120
} stackee_camera_size_t;

typedef struct {
    stackee_camera_size_t size;
    bool hmirror;           // 既定 true (基板が 180 度回って載っている)
    bool vflip;             // 既定 true
    int  warmup;            // 捨て駒の枚数 (既定 30 / 上限 120)
    int  quality;           // JPEG の品質 (1..63、小さいほど高品質。既定 12)
} stackee_camera_req_t;

void stackee_camera_req_default(stackee_camera_req_t *out);

// console を登録するだけ。カメラの電源には触らない。
esp_err_t stackee_camera_init(void);

// 撮る。**この関数はブロックする** (捨て駒 30 枚で約 4 秒)。
// 呼んでよいのは console タスクと camera タスクだけ。
esp_err_t stackee_camera_capture(const stackee_camera_req_t *req);

// STK_CAMERA キー。入力タスクから呼ばれるので**ブロックしない**
// (印を立てるだけ。実際の撮影は camera タスクが進める)。
// 撮って POST /look で AI に見せ、返答を音声と字幕で鳴らす (2026-09-26)。
// ★ 会話中 (録音/送信/待ち/再生) に押されたら撮らずに無視する。
void stackee_camera_key(void);

// 撮影中か、画像を見せる流れを camera タスクが進めている最中か。
// ★ 再起動の判断 (撮影中にリセットしない) に使う。
bool stackee_camera_busy(void);

// ALDO3 (カメラの 3.3V) の入り切り。
// ★ ハードリセットの前に off にすること。
esp_err_t stackee_camera_power(bool on);

// G45 (PCLK) / G46 (VSYNC) = ストラッピングピンを出力 Low + hold で固定する。
// ★ esp_restart() の直前に必ず呼ぶ (撮影後のリセットがブートループになる)。
//   撮影の後片付けでも自動で呼ばれる。カメラを開いていなくても害は無い。
void stackee_camera_pin_strap_safe(void);
int stackee_camera_power_is_on(void);      // 1 / 0 / -1 (読めない)

// 直近の JPEG。撮れていなければ NULL。
const uint8_t *stackee_camera_jpeg(size_t *out_len);

typedef struct {
    const char *state;      // "idle" / "capturing" / "sending" / "done" / "error"
    uint32_t job;
    uint32_t ms;            // 直近の撮影にかかった時間
    int      w, h;
    size_t   raw_bytes;     // RGB565 のバイト数
    size_t   jpeg_bytes;
    int      warmup;
    bool     hmirror, vflip;
    int      quality;
    uint32_t captures, failures;
    bool     psram_dma;         // PSRAM DMA で開けたか (内蔵 RAM をほぼ使わない)
    uint32_t dma_largest;       // 撮る直前の「DMA に使える内蔵の最大の塊」
    uint32_t internal_free;     // 撮る直前の内蔵 RAM の空き
    uint32_t sent;          // POST /look に渡した回数 (受理ではなく送り始めた数)
    char     error[48];
    uint32_t t_power_ms, t_init_ms, t_warm_ms, t_take_ms, t_jpeg_ms, t_stop_ms;
} stackee_camera_stats_t;

void stackee_camera_stats(stackee_camera_stats_t *out);
