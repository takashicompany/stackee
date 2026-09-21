// 「マイクを開けるとき、ES7210 を全部設定し直すか」を決める小さな印。
//
// ★ **なぜ要るのか。** 押すたびに ES7210 の全レジスタを書き直していて、
//   それだけで **I2C に 44 ms** かかっていた (実測。押下 → 使える音までの
//   約 144 ms のうちの 44 ms がこれ)。レジスタは電源を切らないかぎり残るので、
//   **全設定は 1 回で足りる**。2 回目からは電源を上げ直すぶんだけでよい。
//
// ★ ただし「1 回書いたからもう安心」にはしない。ほかの経路 (UAC のマイク、
//   復旧のやり直し) が IC を触った / 触ったかもしれないときは **印を立てて
//   次にマイクを開けるとき書き直す**。古い設定のまま録らない、が約束。
//
// ★ ESP-IDF に依存しない。判断だけを切り出してあるので、ホストビルドで
//   「何回フル設定したか」「印が立ったら次で書き直すか」を確かめられる
//   (tools/test_micopen_host.py)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     configured;    // 全設定を 1 度でも通した
    bool     dirty;         // ほかの経路が触った → 次で書き直す
    uint32_t full;          // 全設定した回数 (status に出す)
    uint32_t light;         // 電源を上げ直すだけで済んだ回数
} stackee_micopen_t;

void stackee_micopen_reset(stackee_micopen_t *m);

// 「ほかの経路が IC を触った (かもしれない)」。次にマイクを開けるとき
// 全設定からやり直す。★ 触ったほうが自分で申告する。
void stackee_micopen_invalidate(stackee_micopen_t *m);

// いま全設定が要るか。alive は「IC が生きていて、書いた値が残っているか」
// (呼ぶ側がレジスタを 1 つ読んで確かめた結果)。
bool stackee_micopen_needs_full(const stackee_micopen_t *m, bool alive);

// 開け終わった。did_full = 全設定を通したか、ok = 成功したか。
// ★ **失敗したら configured を下ろす。** 次は必ず全設定からやり直す。
void stackee_micopen_done(stackee_micopen_t *m, bool did_full, bool ok);
