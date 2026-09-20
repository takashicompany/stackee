#include "stackee_assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "miniz.h"

static const char *TAG = "assets";

#define MANIFEST_PATH  STACKEE_ASSETS_MOUNT "/stackee_assets/manifest.json"
// 目録は 32 枚ぶんの名前と sha256 で 5 KB ほど。取りこぼさない程度に大きく。
#define MANIFEST_MAX   16384

static stackee_assets_info_t s_info;
static wl_handle_t s_wl = WL_INVALID_HANDLE;

static void fail(const char *why) {
    snprintf(s_info.error, sizeof(s_info.error), "%s", why);
}

// ---- ごく小さな JSON の拾い読み --------------------------------------------
//
// cJSON は ESP-IDF v6 の標準構成に入っていない。目録から要るのは整数 2 つと
// 配列の要素数だけなので、丸ごとの構文解析はしない。

static const char *find_key(const char *json, const char *key) {
    char pattern[32];
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

static int read_int(const char *json, const char *key, int fallback) {
    const char *at = find_key(json, key);
    if (at == NULL) {
        return fallback;
    }
    char *end = NULL;
    long value = strtol(at, &end, 10);
    return (end == at) ? fallback : (int)value;
}

// "faces": [ {...}, {...} ] の要素数。入れ子の括弧と文字列の中の括弧を数え
// 違えないように、深さと引用符を見ながら進む。
static int count_array_items(const char *json, const char *key) {
    const char *at = find_key(json, key);
    if (at == NULL || *at != '[') {
        return -1;
    }
    at++;
    int depth = 0;
    int items = 0;
    bool in_string = false;
    bool saw_value = false;
    for (; *at; at++) {
        if (in_string) {
            if (*at == '\\' && at[1]) {
                at++;
            } else if (*at == '"') {
                in_string = false;
            }
            continue;
        }
        if (*at == '"') {
            in_string = true;
            saw_value = true;
        } else if (*at == '{' || *at == '[') {
            depth++;
            saw_value = true;
        } else if (*at == '}' || *at == ']') {
            if (depth == 0) {
                return saw_value ? items + 1 : 0;    // 配列の終わり
            }
            depth--;
        } else if (*at == ',' && depth == 0) {
            items++;
        } else if (*at != ' ' && *at != '\n' && *at != '\r' && *at != '\t') {
            saw_value = true;
        }
    }
    return -1;      // 閉じていない
}

// ---------------------------------------------------------------------------

esp_err_t stackee_assets_mount(void) {
    memset(&s_info, 0, sizeof(s_info));

    // CircuitPython は user_fs をそのまま FAT として使う (摩耗平準化を
    // 挟まない: supervisor/internal_flash.c が esp_partition を直に読み書き
    // している)。だから読み取り専用の生マウントで開く。
    esp_vfs_fat_mount_config_t config = {
        .max_files = 4,
        .format_if_mount_failed = false,
        .allocation_unit_size = 0,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_ro(STACKEE_ASSETS_MOUNT, "user_fs", &config);
    if (err != ESP_OK) {
        fail(esp_err_to_name(err));
        ESP_LOGE(TAG, "user_fs をマウントできない: %s (続行する)", esp_err_to_name(err));
        return err;
    }
    s_info.mounted = true;
    (void)s_wl;

    FILE *f = fopen(MANIFEST_PATH, "rb");
    if (f == NULL) {
        fail("manifest.json が開けない");
        ESP_LOGE(TAG, "%s が開けない (続行する)", MANIFEST_PATH);
        return ESP_ERR_NOT_FOUND;
    }
    char *buf = malloc(MANIFEST_MAX);
    if (buf == NULL) {
        fclose(f);
        fail("目録を読む領域が無い");
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, MANIFEST_MAX - 1, f);
    fclose(f);
    buf[got] = '\0';

    s_info.version = read_int(buf, "v", -1);
    s_info.size = read_int(buf, "size", -1);
    s_info.faces = count_array_items(buf, "faces");
    free(buf);

    if (s_info.size < 0 || s_info.faces < 0) {
        fail("目録の中身が読めない");
        ESP_LOGE(TAG, "manifest.json を読めたが中身が想定と違う (%zu B)", got);
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_info.manifest_ok = true;
    ESP_LOGI(TAG, "manifest.json: v=%d size=%d faces=%d (%zu B)",
             s_info.version, s_info.size, s_info.faces, got);
    return ESP_OK;
}

const stackee_assets_info_t *stackee_assets_info(void) {
    return &s_info;
}

// ---------------------------------------------------------------------------
// 段階 2: 素材の実体を読む
// ---------------------------------------------------------------------------
// ★ tinfl_decompressor は約 10.6 KB ある。ROM の
//   tinfl_decompress_mem_to_mem() はそれをスタックに積むので、呼ぶタスクの
//   スタックを 12 KB 以上にしないと静かに壊れる。そこでここでは
//   低水準の tinfl_decompress() を直に呼び、作業領域はヒープに取る。

void *stackee_assets_read(const char *name, size_t *out_len, uint32_t caps) {
    char path[128];
    snprintf(path, sizeof(path), STACKEE_ASSETS_MOUNT "/stackee_assets/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "%s が開けない", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)size + 1, caps);
    if (buf == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "%s のための %ld B を確保できない", name, size);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(buf);
        ESP_LOGE(TAG, "%s を読み切れない (%zu/%ld)", name, got, size);
        return NULL;
    }
    buf[size] = '\0';
    if (out_len) {
        *out_len = (size_t)size;
    }
    return buf;
}

char *stackee_assets_read_root(const char *name, size_t *out_len) {
    char path[128];
    snprintf(path, sizeof(path), STACKEE_ASSETS_MOUNT "/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    rewind(f);
    // 設定は数 KB。壊れたファイルで無制限に読まない。
    if (size < 0 || size > 16384) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)size + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) {
        *out_len = got;
    }
    return buf;
}

void *stackee_assets_inflate(const void *src, size_t src_len, size_t expect_len,
                             uint32_t caps) {
    uint8_t *out = heap_caps_malloc(expect_len, caps);
    if (out == NULL) {
        ESP_LOGE(TAG, "展開先の %zu B を確保できない", expect_len);
        return NULL;
    }
    tinfl_decompressor *state = heap_caps_malloc(sizeof(tinfl_decompressor),
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (state == NULL) {
        free(out);
        ESP_LOGE(TAG, "展開器の作業領域 %zu B を確保できない", sizeof(tinfl_decompressor));
        return NULL;
    }
    tinfl_init(state);
    size_t in_bytes = src_len;
    size_t out_bytes = expect_len;
    tinfl_status status = tinfl_decompress(
        state, (const mz_uint8 *)src, &in_bytes, out, out, &out_bytes,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(state);
    if (status != TINFL_STATUS_DONE || out_bytes != expect_len) {
        ESP_LOGE(TAG, "展開に失敗 (status=%d, %zu/%zu B)", (int)status, out_bytes, expect_len);
        free(out);
        return NULL;
    }
    return out;
}

void *stackee_assets_read_inflate(const char *name, size_t expect_len, uint32_t caps) {
    size_t raw_len = 0;
    // 圧縮されたものは小さいので内蔵 RAM に置き、展開が終わったら返す。
    void *raw = stackee_assets_read(name, &raw_len, MALLOC_CAP_8BIT);
    if (raw == NULL) {
        return NULL;
    }
    void *out = stackee_assets_inflate(raw, raw_len, expect_len, caps);
    free(raw);
    return out;
}

// ---------------------------------------------------------------------------
// 段階 4: 書き込みのためにいったん外す
// ---------------------------------------------------------------------------
// ★ ここは **stackee_assets_mount より後ろ**に置くこと。
//   tools/test_console_host.py が「find_key 〜 stackee_assets_mount」の
//   範囲を切り出してホストでビルドするので、その中に ESP-IDF の呼び出しが
//   入るとホストテストが通らなくなる。
void stackee_assets_unmount(void) {
    if (!s_info.mounted) {
        return;
    }
    esp_vfs_fat_spiflash_unmount_ro(STACKEE_ASSETS_MOUNT, "user_fs");
    s_info.mounted = false;
}
