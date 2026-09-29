// クリップの FAT の仕事 (stackee_clipsm.h の stackee_clip_job_t) をこなす worker。
// **メインループ** (console タスク、CPU0・優先度 1 = いちばん低い) が
// stackee_clipfs_poll() で 1 周に 1 切れずつ進める (README §17-2f)。
//
// ★ なぜメインループか: FAT を書けるのはここだけ (fs.put / settings.set と
//   同じ持ち主)。いちばん低い優先度なので、入力 (CPU1・最高)・音・画面・
//   BLE・通信のどれよりも後回しになる。1 周に書くのは 4 KB (フラッシュ 1
//   セクタ) だけで、間に 1 ms 休んでコンソールも回す。
// ★ フラッシュの消去・書き込みの間は ESP-IDF が両コアのキャッシュを止め、
//   入力タスク (CPU1) も止まる (4 KB で約 10 ms)。だから**打鍵が止まって
//   300 ms たってから**しか書かない (目録の半端物消し・消す・全部消すも同じ)。
//   書きかけの途中で打鍵が来たら、次の 4 KB は静かになるまで待つ。
//   読むだけ (LOAD) は待たない。打鍵が 30 秒続いたらその仕事はあきらめる。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stackee_clipsm.h"

// audio タスク (talk の錠の中) から。worker が空いていなければ false。
bool stackee_clipfs_submit(stackee_clip_job_t *job);
// 持っている仕事を打ち切る (書きかけは消す)。すぐ戻る。どのタスクからでも。
void stackee_clipfs_abort(void);
// メインループから毎周。
void stackee_clipfs_poll(void);

typedef struct {
    uint32_t jobs;          // 終えた仕事の数
    uint32_t writes;        // 書き終えたクリップ
    uint32_t aborts;        // 打ち切った書き込み
    uint32_t fails;
    uint32_t slices;        // 書いた 4 KB の切れの数
    uint64_t bytes;         // 書いたバイト数の累計 (音声 + メタ)
    uint32_t max_slice_us;  // 1 切れ (fwrite 4 KB) にかかった最長
    uint32_t last_ms;       // 直近の仕事にかかった時間
    uint32_t deferred;      // 打鍵の直後で見送った周
    uint32_t gave_up;       // 打鍵が 30 秒続いてあきらめた仕事
    bool     cleanup_pending;   // 打ち切った書き込みの後始末が静かになるのを待っている
    int      busy_kind;     // いま持っている仕事 (0 = なし)
    char     last_error[48];
} stackee_clipfs_stats_t;

void stackee_clipfs_stats(stackee_clipfs_stats_t *out);
