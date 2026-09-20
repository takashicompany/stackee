// 割り込み禁止区間。stackee_qmk_config.h の IGNORE_ATOMIC_BLOCK と同じ理由で
// 何もしない (QMK の状態を触るのは input タスク 1 本だけ)。
#pragma once

#define ATOMIC_BLOCK(t) for (uint8_t __ToDo = 1; __ToDo; __ToDo = 0)
#define ATOMIC_FORCEON
#define ATOMIC_RESTORESTATE
#define ATOMIC_BLOCK_RESTORESTATE ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
#define ATOMIC_BLOCK_FORCEON ATOMIC_BLOCK(ATOMIC_FORCEON)
