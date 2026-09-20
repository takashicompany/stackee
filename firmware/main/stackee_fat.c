#include "stackee_fat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "diskio_impl.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#include "stackee_assets.h"

static const char *TAG = "fatrw";

// 書くときだけ使うマウント先。読み取り専用の /assets とは別の名前にして、
// 「いま書ける状態か」がパスから分かるようにしてある。
#define RW_MOUNT "/rw"
#define FLASH_SECTOR 4096

static const esp_partition_t *s_part;
static size_t   s_sector_size;
static size_t   s_sector_count;
static BYTE     s_pdrv = 0xFF;
static FATFS   *s_fs;
static char     s_drv[3] = {'0', ':', 0};
static uint8_t *s_cache;            // 4 KB。読む → 差し替える → 書く に使う

static stackee_fat_stats_t s_stats;

static void note_error(const char *why) {
    snprintf(s_stats.last_error, sizeof(s_stats.last_error), "%s", why);
}

// ---------------------------------------------------------------------------
// 生パーティションの diskio (読み書き)
// ---------------------------------------------------------------------------
// ESP-IDF の diskio_rawflash は write が RES_WRPRT で固定なので使えない。
// 読み出しと ioctl はあちらと同じ作りにしてある (ブートセクタから
// セクタ長と総数を拾う)。
#define BPB_BytsPerSec 11
#define BPB_TotSec16   19
#define BPB_TotSec32   32

static DSTATUS rw_initialize(BYTE pdrv) {
    (void)pdrv;
    uint16_t ss = 0;
    uint16_t n16 = 0;
    uint32_t n32 = 0;
    if (esp_partition_read(s_part, BPB_BytsPerSec, &ss, sizeof(ss)) != ESP_OK) {
        return RES_ERROR;
    }
    if (esp_partition_read(s_part, BPB_TotSec16, &n16, sizeof(n16)) != ESP_OK) {
        return RES_ERROR;
    }
    s_sector_size = ss;
    s_sector_count = n16;
    if (n16 == 0) {
        if (esp_partition_read(s_part, BPB_TotSec32, &n32, sizeof(n32)) != ESP_OK) {
            return RES_ERROR;
        }
        s_sector_count = n32;
    }
    if (s_sector_size == 0 || s_sector_size > FLASH_SECTOR ||
        (FLASH_SECTOR % s_sector_size) != 0) {
        // ブートセクタが読めない / 形が違う。**書かない**。
        return STA_NOINIT | STA_NODISK;
    }
    return 0;           // ★ STA_PROTECT を立てない = 書ける
}

static DSTATUS rw_status(BYTE pdrv) {
    (void)pdrv;
    return (s_sector_size == 0) ? (STA_NOINIT | STA_NODISK) : 0;
}

static DRESULT rw_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (esp_partition_read(s_part, (size_t)sector * s_sector_size, buff,
                           (size_t)count * s_sector_size) != ESP_OK) {
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT rw_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    if (s_cache == NULL) {
        return RES_ERROR;
    }
    size_t off = (size_t)sector * s_sector_size;
    size_t len = (size_t)count * s_sector_size;
    if (off + len > s_part->size) {
        return RES_PARERR;
    }
    // 4 KB のフラッシュセクタごとに「読む → 差し替える → 消す → 書く」。
    // ★ CircuitPython の internal_flash.c と同じ考え方。
    while (len > 0) {
        size_t base = off & ~((size_t)FLASH_SECTOR - 1);
        size_t in = off - base;
        size_t chunk = FLASH_SECTOR - in;
        if (chunk > len) {
            chunk = len;
        }
        if (esp_partition_read(s_part, base, s_cache, FLASH_SECTOR) != ESP_OK) {
            return RES_ERROR;
        }
        if (memcmp(s_cache + in, buff, chunk) != 0) {
            memcpy(s_cache + in, buff, chunk);
            if (esp_partition_erase_range(s_part, base, FLASH_SECTOR) != ESP_OK) {
                return RES_ERROR;
            }
            if (esp_partition_write(s_part, base, s_cache, FLASH_SECTOR) != ESP_OK) {
                return RES_ERROR;
            }
            s_stats.flash_erases++;
        }
        off += chunk;
        buff += chunk;
        len -= chunk;
    }
    return RES_OK;
}

static DRESULT rw_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *((DWORD *)buff) = (DWORD)s_sector_count;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *((WORD *)buff) = (WORD)s_sector_size;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *((DWORD *)buff) = (DWORD)(FLASH_SECTOR / s_sector_size);
            return RES_OK;
        default:
            return RES_ERROR;
    }
}

static const ff_diskio_impl_t s_impl = {
    .init = rw_initialize,
    .status = rw_status,
    .read = rw_read,
    .write = rw_write,
    .ioctl = rw_ioctl,
};

// ---------------------------------------------------------------------------
// マウントの付け替え
// ---------------------------------------------------------------------------
static esp_err_t rw_begin(void) {
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      ESP_PARTITION_SUBTYPE_DATA_FAT, "user_fs");
    if (s_part == NULL) {
        note_error("nopart");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_cache == NULL) {
        s_cache = malloc(FLASH_SECTOR);
        if (s_cache == NULL) {
            note_error("nomem");
            return ESP_ERR_NO_MEM;
        }
    }
    // 読み取り専用のマウントを外す。ここから先 /assets は読めない。
    stackee_assets_unmount();

    esp_err_t err = ff_diskio_get_drive(&s_pdrv);
    if (err != ESP_OK || s_pdrv == 0xFF) {
        note_error("nodrive");
        return ESP_FAIL;
    }
    ff_diskio_register(s_pdrv, &s_impl);
    s_drv[0] = (char)('0' + s_pdrv);
    esp_vfs_fat_conf_t conf = {
        .base_path = RW_MOUNT,
        .fat_drive = s_drv,
        .max_files = 4,
    };
    err = esp_vfs_fat_register(&conf, &s_fs);
    if (err != ESP_OK) {
        note_error("register");
        ff_diskio_unregister(s_pdrv);
        s_pdrv = 0xFF;
        return err;
    }
    FRESULT fr = f_mount(s_fs, s_drv, 1);
    if (fr != FR_OK) {
        note_error("mount");
        esp_vfs_fat_unregister_path(RW_MOUNT);
        ff_diskio_unregister(s_pdrv);
        s_pdrv = 0xFF;
        s_fs = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void rw_end(void) {
    if (s_fs != NULL) {
        f_mount(NULL, s_drv, 0);
        s_fs = NULL;
    }
    esp_vfs_fat_unregister_path(RW_MOUNT);
    if (s_pdrv != 0xFF) {
        ff_diskio_unregister(s_pdrv);
        s_pdrv = 0xFF;
    }
    s_sector_size = 0;
    // 読み取り専用に戻す。ここが失敗すると素材が読めなくなるので、
    // 失敗したらログに出す (キーボードとしては動き続ける)。
    if (stackee_assets_mount() != ESP_OK) {
        ESP_LOGE(TAG, "読み取り専用に戻せない。再起動で直る");
    }
}

// ---------------------------------------------------------------------------
// 書く
// ---------------------------------------------------------------------------
static esp_err_t write_one(const char *name, const void *data, size_t len,
                           bool atomic) {
    char path[160];
    char tmp[168];
    snprintf(path, sizeof(path), RW_MOUNT "/%s", name);
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    const char *target = atomic ? tmp : path;

    FILE *f = fopen(target, "wb");
    if (f == NULL) {
        note_error("open");
        return ESP_FAIL;
    }
    size_t wrote = (len > 0) ? fwrite(data, 1, len, f) : 0;
    int closed = fclose(f);
    if (wrote != len || closed != 0) {
        note_error("write");
        return ESP_FAIL;
    }
    if (!atomic) {
        return ESP_OK;
    }
    // 読み直して照合してから置き換える (現行 settings.set と同じ)。
    f = fopen(tmp, "rb");
    if (f == NULL) {
        note_error("verify_open");
        return ESP_FAIL;
    }
    bool same = true;
    const uint8_t *want = data;
    uint8_t chunk[256];
    size_t at = 0;
    for (;;) {
        size_t got = fread(chunk, 1, sizeof(chunk), f);
        if (got == 0) {
            break;
        }
        if (at + got > len || memcmp(chunk, want + at, got) != 0) {
            same = false;
            break;
        }
        at += got;
    }
    fclose(f);
    if (!same || at != len) {
        note_error("verify");
        return ESP_FAIL;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        note_error("rename");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t stackee_fat_write_root(const char *name, const void *data, size_t len,
                                 bool atomic) {
    if (name == NULL || name[0] == '\0' || name[0] == '/' ||
        strstr(name, "..") != NULL) {
        note_error("badname");
        s_stats.fails++;
        return ESP_ERR_INVALID_ARG;
    }
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = rw_begin();
    if (err == ESP_OK) {
        err = write_one(name, data, len, atomic);
        rw_end();
    }
    s_stats.last_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        s_stats.writes++;
        s_stats.bytes += (uint32_t)len;
        s_stats.last_error[0] = '\0';
        ESP_LOGI(TAG, "%s に %u バイト書いた (%lu ms)", name, (unsigned)len,
                 (unsigned long)s_stats.last_ms);
    } else {
        s_stats.fails++;
        ESP_LOGE(TAG, "%s を書けない (%s)", name, s_stats.last_error);
    }
    return err;
}

esp_err_t stackee_fat_mkdir_root(const char *name) {
    if (name == NULL || name[0] == '\0' || name[0] == '/' ||
        strstr(name, "..") != NULL) {
        note_error("badname");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = rw_begin();
    if (err != ESP_OK) {
        return err;
    }
    char path[160];
    snprintf(path, sizeof(path), RW_MOUNT "/%s", name);
    // VFS 経由で作る (FATFS の f_mkdir を直接呼ぶとドライブ番号の扱いが要る)。
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        note_error("mkdir");
        err = ESP_FAIL;
    }
    rw_end();
    return err;
}

void stackee_fat_stats(stackee_fat_stats_t *out) {
    if (out != NULL) {
        *out = s_stats;
    }
}
