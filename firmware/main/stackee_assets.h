// user_fs (CircuitPython の CIRCUITPY と同じ FAT パーティション) を読み取り
// 専用でマウントし、素材の目録 /stackee_assets/manifest.json を見る。
//
// 段階 0 では「読めること」を確かめるだけ。顔・アイコン・一次回答の実体を
// 使うのは段階 2 以降。素材の転送手段 (stackee_serial.py) は変えない。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define STACKEE_ASSETS_MOUNT "/assets"

typedef struct {
    bool mounted;
    bool manifest_ok;
    int version;        // manifest の "v"
    int size;           // 顔 1 枚の 1 辺 [px]
    int faces;          // "faces" の要素数
    char error[64];     // 失敗の理由 (空なら成功)
} stackee_assets_info_t;

esp_err_t stackee_assets_mount(void);
const stackee_assets_info_t *stackee_assets_info(void);

// 読み取り専用のマウントを外す。段階 4 の書き込み (stackee_fat.c) が
// 「外す → 書ける形で付け直す → 書く → 読み取り専用に戻す」を行うため。
// ★ 外している間 /assets の下は 1 バイトも読めない。console タスクから
//   だけ呼ぶこと (ui タスクは顔の素材を起動時に RAM へ読み終えている)。
void stackee_assets_unmount(void);

// ---------------------------------------------------------------------------
// 段階 2: 素材の実体を読む
// ---------------------------------------------------------------------------
// どちらも呼び出し側が free() する。name は "/stackee_assets/" からの相対
// ("faces.bin" など)。caps は heap_caps_malloc の引数
// (PSRAM に置きたいなら MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)。

// ファイルを丸ごと読む。末尾に NUL を 1 個足す (テキストをそのまま扱える)。
void *stackee_assets_read(const char *name, size_t *out_len, uint32_t caps);

// FAT の **根っこ** にあるファイルを読む (/settings.toml など)。
// 段階 3 で使う。内蔵 RAM に置き、末尾に NUL を足す。free() は呼び手。
char *stackee_assets_read_root(const char *name, size_t *out_len);

// zlib 形式を展開する。ESP32-S3 の **ROM にある tinfl** を使うので、
// 展開器のコードは像に 1 バイトも増えない (miniz は Espressif が ROM に
// 焼いている。esp_rom/esp32s3/ld/esp32s3.rom.ld の tinfl_decompress)。
// expect_len は展開後の丁度のバイト数。違ったら NULL。
void *stackee_assets_inflate(const void *src, size_t src_len, size_t expect_len,
                             uint32_t caps);

// 読む → 展開する をひと続きで。
void *stackee_assets_read_inflate(const char *name, size_t expect_len,
                                  uint32_t caps);
