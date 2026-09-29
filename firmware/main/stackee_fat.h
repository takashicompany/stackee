// user_fs (CircuitPython の CIRCUITPY と同じ FAT) への**書き込み**。
//
// ★ 普段は読み取り専用でマウントしてある (stackee_assets.c)。壊れ方を
//   減らすためで、それは変えない。書くときだけここが
//   「いったん外して → 書ける形でマウントし直して → 書いて → 戻す」を行う。
//
// ★ 書き方は CircuitPython 本体と同じにしてある。CircuitPython の
//   ports/espressif/supervisor/internal_flash.c は
//   esp_partition_erase_range + esp_partition_write で **ウェアレベリング
//   無しの生のパーティション**へ書いている。ESP-IDF の
//   esp_vfs_fat_spiflash_mount_rw_wl を使うと WL のヘッダを前提にするので
//   **絶対に使わない** (使うと CIRCUITPY の中身ごと壊れる)。
//
// ★ 1 回の書き込みは 4 KB のフラッシュセクタ単位で「読む → 差し替える →
//   消す → 書く」。途中で電源が切れればそのセクタは壊れるが、それは
//   CircuitPython で書いているときと同じ危険度。
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// FAT の根っこにファイルを 1 つ置く。name は "settings.toml" のような
// **先頭の / を含まない** 名前 (サブディレクトリ可: "stackee_assets/x.bin")。
//
// atomic=true なら "<name>.part" に書いてから rename する
// (現行 stackee_console.py の settings.set と同じ段取り)。
//
// 中でマウントし直すので、呼んでいる間 /assets からの読み出しは止まる。
// console タスクからだけ呼ぶこと。
esp_err_t stackee_fat_write_root(const char *name, const void *data, size_t len,
                                 bool atomic);

// 上と同じ手順で、ディレクトリを作る (すでにあれば成功扱い)。
esp_err_t stackee_fat_mkdir_root(const char *name);

// ---- 書ける形で付けておく区間 (2026-09-30、クリップの取り込み) ------------
// begin 〜 end の間は /rw に書ける形で付いていて、/assets は外れている
// (読むなら stackee_fat_base() の下を読む)。入れ子にできる (fs.put が
// 区間の中から来ても付け直さない)。★ 呼ぶのはメインループ (console タスク) だけ。
// ★ 書き込みは 4 KB の写しに溜め、別のセクタへ移るとき / CTRL_SYNC / end で
//   1 回だけ消して書く。写しの 4 KB (内蔵 RAM) は区間の間だけ持つ。
esp_err_t   stackee_fat_session_begin(void);
void        stackee_fat_session_end(void);
bool        stackee_fat_session_open(void);
// いま読める根っこ ("/rw" か "/assets")。
const char *stackee_fat_base(void);
// 区間の中でだけ使える。FAT の空き [B] とクラスタの大きさ [B]。
esp_err_t   stackee_fat_space(uint64_t *free_bytes, uint32_t *cluster);

typedef struct {
    uint32_t writes;        // 成功した書き込み回数
    uint32_t fails;
    uint32_t bytes;         // 書いたバイト数の累計
    uint32_t last_ms;       // 直近の所要 [ms]
    uint32_t flash_erases;  // 消した 4 KB セクタの数
    char     last_error[48];
} stackee_fat_stats_t;

void stackee_fat_stats(stackee_fat_stats_t *out);
