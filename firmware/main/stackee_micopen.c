#include "stackee_micopen.h"

#include <string.h>

void stackee_micopen_reset(stackee_micopen_t *m) {
    memset(m, 0, sizeof(*m));
}

void stackee_micopen_invalidate(stackee_micopen_t *m) {
    m->dirty = true;
}

bool stackee_micopen_needs_full(const stackee_micopen_t *m, bool alive) {
    // まだ 1 度も書いていない / 誰かが触った / 書いた値が残っていない。
    return !m->configured || m->dirty || !alive;
}

void stackee_micopen_done(stackee_micopen_t *m, bool did_full, bool ok) {
    if (!ok) {
        // ★ 転んだら印を全部下ろす。次は必ず全設定から。
        m->configured = false;
        m->dirty = true;
        return;
    }
    if (did_full) {
        m->configured = true;
        m->dirty = false;
        m->full++;
    } else {
        m->light++;
    }
}
