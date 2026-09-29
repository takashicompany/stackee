#include "stackee_fat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>

#include "diskio_impl.h"
#include "esp_heap_caps.h"
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
// 4 KB。いま触っているフラッシュセクタの写し (書き戻し式の 1 枚キャッシュ)。
// ★ 内蔵 RAM から取る (esp_flash_write は PSRAM の元バッファを 32 B ずつしか
//   書けない)。書ける形で付けている間だけ持ち、外したら返す (常駐させない)。
static uint8_t *s_cache;
static size_t   s_cache_base = SIZE_MAX;   // s_cache が写しているセクタ (無ければ SIZE_MAX)
static bool     s_cache_dirty;            // 写しを書き換えた (まだフラッシュに無い)
static int      s_depth;                  // 書ける形で付けている入れ子の数

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

// 書き戻し式の 1 枚キャッシュ (2026-09-30、クリップの取り込みで足した)。
// ★ FAT のクラスタは 2 KB (CIRCUITPY の 12 MB を f_mkfs の既定で作ると 4 セクタ)
//   で、FatFs の f_write は 1 回の disk_write をクラスタの切れ目で止める。
//   以前の「来るたびに 読む → 消す → 書く」だと、続けて書くだけで同じ 4 KB の
//   フラッシュセクタを 2 回ずつ消していた。いま触っているセクタを 1 枚だけ
//   手元に置き、**別のセクタへ移るとき / CTRL_SYNC / 外すとき**に 1 回だけ
//   消して書く (CircuitPython の internal_flash.c の _cache と同じ考え方)。
static bool cache_flush(void) {
    if (!s_cache_dirty || s_cache == NULL || s_cache_base == SIZE_MAX) {
        s_cache_dirty = false;
        return true;
    }
    if (esp_partition_erase_range(s_part, s_cache_base, FLASH_SECTOR) != ESP_OK ||
        esp_partition_write(s_part, s_cache_base, s_cache, FLASH_SECTOR) != ESP_OK) {
        return false;
    }
    s_stats.flash_erases++;
    s_cache_dirty = false;
    return true;
}

static bool cache_load(size_t base) {
    if (s_cache_base == base) {
        return true;
    }
    if (!cache_flush()) {
        return false;
    }
    s_cache_base = SIZE_MAX;
    if (esp_partition_read(s_part, base, s_cache, FLASH_SECTOR) != ESP_OK) {
        return false;
    }
    s_cache_base = base;
    return true;
}

static DRESULT rw_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count) {
    (void)pdrv;
    size_t off = (size_t)sector * s_sector_size;
    size_t len = (size_t)count * s_sector_size;
    if (esp_partition_read(s_part, off, buff, len) != ESP_OK) {
        return RES_ERROR;
    }
    // まだフラッシュに書いていない写しと重なるところは、写しのほうが正しい。
    if (s_cache_dirty && s_cache_base != SIZE_MAX &&
        off < s_cache_base + FLASH_SECTOR && s_cache_base < off + len) {
        size_t from = (off > s_cache_base) ? off : s_cache_base;
        size_t to = (off + len < s_cache_base + FLASH_SECTOR) ? off + len
                                                              : s_cache_base + FLASH_SECTOR;
        memcpy(buff + (from - off), s_cache + (from - s_cache_base), to - from);
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
    while (len > 0) {
        size_t base = off & ~((size_t)FLASH_SECTOR - 1);
        size_t in = off - base;
        size_t chunk = FLASH_SECTOR - in;
        if (chunk > len) {
            chunk = len;
        }
        if (!cache_load(base)) {
            return RES_ERROR;
        }
        // 同じ中身なら触らない (消さずに済む)。
        if (memcmp(s_cache + in, buff, chunk) != 0) {
            memcpy(s_cache + in, buff, chunk);
            s_cache_dirty = true;
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
            return cache_flush() ? RES_OK : RES_ERROR;
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
        s_cache = heap_caps_malloc(FLASH_SECTOR, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_cache == NULL) {
            note_error("nomem");
            return ESP_ERR_NO_MEM;
        }
    }
    s_cache_base = SIZE_MAX;
    s_cache_dirty = false;
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
    // ★ 外す前に写しを書き出す (ファイルは閉じてあるので FatFs からは
    //   CTRL_SYNC が来ているはずだが、念のため)。
    if (s_part != NULL && !cache_flush()) {
        note_error("flush");
        ESP_LOGE(TAG, "書き戻しに失敗した (セクタ 0x%x)", (unsigned)s_cache_base);
    }
    s_cache_base = SIZE_MAX;
    s_cache_dirty = false;
    free(s_cache);
    s_cache = NULL;
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
// 書ける形で付けておく区間 (入れ子にできる)
// ---------------------------------------------------------------------------
esp_err_t stackee_fat_session_begin(void) {
    if (s_depth > 0) {
        s_depth++;
        return ESP_OK;
    }
    esp_err_t err = rw_begin();
    if (err != ESP_OK) {
        // ★ 途中で失敗しても読み取り専用には必ず戻す (素材が読めなくなるため)。
        rw_end();
        return err;
    }
    s_depth = 1;
    return ESP_OK;
}

void stackee_fat_session_end(void) {
    if (s_depth <= 0) {
        return;
    }
    if (--s_depth == 0) {
        rw_end();
    }
}

bool stackee_fat_session_open(void) {
    return s_depth > 0;
}

const char *stackee_fat_base(void) {
    return (s_depth > 0) ? RW_MOUNT : STACKEE_ASSETS_MOUNT;
}

esp_err_t stackee_fat_space(uint64_t *free_bytes, uint32_t *cluster) {
    if (s_depth <= 0 || s_fs == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    DWORD nclst = 0;
    FATFS *fs = NULL;
    if (f_getfree(s_drv, &nclst, &fs) != FR_OK || fs == NULL) {
        return ESP_FAIL;
    }
#if FF_MAX_SS != FF_MIN_SS
    uint32_t ss = fs->ssize;
#else
    uint32_t ss = FF_MAX_SS;
#endif
    uint32_t cl = (uint32_t)fs->csize * ss;
    if (free_bytes) { *free_bytes = (uint64_t)nclst * cl; }
    if (cluster)    { *cluster = cl; }
    return ESP_OK;
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
    esp_err_t err = stackee_fat_session_begin();
    if (err == ESP_OK) {
        err = write_one(name, data, len, atomic);
        stackee_fat_session_end();
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
    esp_err_t err = stackee_fat_session_begin();
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
    stackee_fat_session_end();
    return err;
}

void stackee_fat_stats(stackee_fat_stats_t *out) {
    if (out != NULL) {
        *out = s_stats;
    }
}
