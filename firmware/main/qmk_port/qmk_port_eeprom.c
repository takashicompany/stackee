// QMK の EEPROM を NVS のブロブに載せる。
//
// QMK (eeconfig / dynamic_keymap / via) は「バイト単位で読み書きできる
// 小さな不揮発メモリ」を前提にしている。NVS はそういう使い方に向かない
// (1 バイトごとにフラッシュを触ると寿命も速度も持たない) ので、
//
//   * RAM に影 (1 KB) を置き、読み書きは全部そこで済ませる
//   * 書き換わったら印をつけ、**静まってから** まとめて NVS へ 1 回書く
//
// という形にする。VIA の配列変更は 1 キーずつ 600 回近く飛んでくるので、
// 遅延させないとフラッシュを 600 回叩くことになる。
//
// 考え方は esp32-qmk-lucky65 (MIT) の port/platforms/eeprom.c と同じだが、
// あちらは「1 バイトずつキューに積んで外付け EEPROM へ流す」形。こちらは
// 保存先が NVS のブロブなので「ブロブ 1 個を丸ごと書き直す」形にしてある。
#include "eeprom.h"

#include <string.h>

#include "qmk_port.h"

static uint8_t  s_shadow[EEPROM_SIZE];
static bool     s_dirty;
static uint32_t s_dirty_since_ms;
static uint32_t s_writes;
static uint32_t s_commits;

void stackee_qmk_eeprom_init(void) {
    memset(s_shadow, 0xFF, sizeof(s_shadow));
    if (!stackee_qmk_eeprom_backend_load(s_shadow, sizeof(s_shadow))) {
        // 何も入っていない (初回起動 / 読めない)。0xFF のままにしておくと
        // QMK が magic 不一致とみなして既定値で作り直す。
        memset(s_shadow, 0xFF, sizeof(s_shadow));
    }
    s_dirty = false;
    s_writes = 0;
    s_commits = 0;
}

void stackee_qmk_eeprom_task(void) {
    if (!s_dirty) {
        return;
    }
    uint32_t now = stackee_qmk_now_ms();
    if ((uint32_t)(now - s_dirty_since_ms) < STACKEE_EEPROM_COMMIT_QUIET_MS) {
        return;
    }
    // 書けなくても影は正しいので、次の機会にまた試す。
    if (stackee_qmk_eeprom_backend_save(s_shadow, sizeof(s_shadow))) {
        s_dirty = false;
        s_commits++;
    } else {
        s_dirty_since_ms = now;
    }
}

void stackee_qmk_eeprom_stats(stackee_qmk_eeprom_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->writes = s_writes;
    out->commits = s_commits;
    out->dirty = s_dirty;
}

// ---------------------------------------------------------------------------
// QMK が呼ぶ口 (platforms/eeprom.h)
// ---------------------------------------------------------------------------
// アドレスはポインタの形で来るが、中身は 0 からの整数 (QMK の慣習)。
static inline uint32_t addr_of(const void *p) {
    return (uint32_t)(uintptr_t)p;
}

uint8_t eeprom_read_byte(const uint8_t *addr) {
    uint32_t offset = addr_of(addr);
    return (offset < EEPROM_SIZE) ? s_shadow[offset] : 0;
}

uint16_t eeprom_read_word(const uint16_t *addr) {
    const uint8_t *p = (const uint8_t *)addr;
    return (uint16_t)(eeprom_read_byte(p) | ((uint16_t)eeprom_read_byte(p + 1) << 8));
}

uint32_t eeprom_read_dword(const uint32_t *addr) {
    const uint8_t *p = (const uint8_t *)addr;
    return (uint32_t)eeprom_read_byte(p) | ((uint32_t)eeprom_read_byte(p + 1) << 8) |
           ((uint32_t)eeprom_read_byte(p + 2) << 16) |
           ((uint32_t)eeprom_read_byte(p + 3) << 24);
}

void eeprom_read_block(void *dst, const void *addr, size_t len) {
    uint8_t       *out = (uint8_t *)dst;
    const uint8_t *p = (const uint8_t *)addr;
    while (len--) {
        *out++ = eeprom_read_byte(p++);
    }
}

void eeprom_write_byte(uint8_t *addr, uint8_t value) {
    uint32_t offset = addr_of(addr);
    if (offset >= EEPROM_SIZE) {
        return;
    }
    s_writes++;
    if (s_shadow[offset] == value) {
        return;
    }
    s_shadow[offset] = value;
    if (!s_dirty) {
        s_dirty = true;
    }
    // 書き込みが続いているあいだは静止時間を数え直す (まとめて 1 回にする)。
    s_dirty_since_ms = stackee_qmk_now_ms();
}

void eeprom_write_word(uint16_t *addr, uint16_t value) {
    uint8_t *p = (uint8_t *)addr;
    eeprom_write_byte(p, (uint8_t)value);
    eeprom_write_byte(p + 1, (uint8_t)(value >> 8));
}

void eeprom_write_dword(uint32_t *addr, uint32_t value) {
    uint8_t *p = (uint8_t *)addr;
    eeprom_write_byte(p, (uint8_t)value);
    eeprom_write_byte(p + 1, (uint8_t)(value >> 8));
    eeprom_write_byte(p + 2, (uint8_t)(value >> 16));
    eeprom_write_byte(p + 3, (uint8_t)(value >> 24));
}

void eeprom_write_block(const void *src, void *addr, size_t len) {
    const uint8_t *in = (const uint8_t *)src;
    uint8_t       *p = (uint8_t *)addr;
    while (len--) {
        eeprom_write_byte(p++, *in++);
    }
}

void eeprom_update_byte(uint8_t *addr, uint8_t value) {
    eeprom_write_byte(addr, value);
}

void eeprom_update_word(uint16_t *addr, uint16_t value) {
    eeprom_write_word(addr, value);
}

void eeprom_update_dword(uint32_t *addr, uint32_t value) {
    eeprom_write_dword(addr, value);
}

void eeprom_update_block(const void *src, void *addr, size_t len) {
    eeprom_write_block(src, addr, len);
}
