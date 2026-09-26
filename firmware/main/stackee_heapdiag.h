// 内蔵 RAM の内訳を読む (console の heap.info、2026-09-27)。
//
// ★ 読むだけ。確保も解放もしない。heap_caps_walk で内蔵 RAM の塊を
//   大きさ別に数え、大きい使用中の塊を並べ、主なタスクのスタックの
//   残り (最小) を出す。「どの確保が内蔵に居座ったか」を後から追うため。
// ★ 目印 (mark) は起動の終わりと、内蔵 RAM が 1 KB 以上減った HTTP の往復で
//   置く。ログのリングが一周しても heap.info で読める。
#pragma once

#include <stdint.h>

void stackee_heapdiag_start(void);

// いまの内蔵 RAM の空き / 最大の塊を what という名前で残す (最新 12 件)。
// ★ 呼び元のタスクで数 µs。割り込みからは呼ばない。
void stackee_heapdiag_mark(const char *what);
