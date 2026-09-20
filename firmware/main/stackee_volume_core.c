// 音量の「値の決め方」と「保存のタイミング」だけ。ESP-IDF に依存しない。
// 実機の口 (NVS / FAT / AW88298) は stackee_volume.c と stackee_audio.c。
#include "stackee_volume.h"

#include <math.h>

void stackee_volume_state_init(stackee_volume_state_t *v, int loaded, uint32_t now) {
    if (loaded < 0)   { loaded = 0; }
    if (loaded > 100) { loaded = 100; }
    v->percent = loaded;
    v->saved = loaded;
    v->changed_at = now;
    v->last_input_at = now;
    v->changes = 0;
    v->saves = 0;
}

void stackee_volume_state_set(stackee_volume_state_t *v, int percent, uint32_t now) {
    if (percent < 0)   { percent = 0; }
    if (percent > 100) { percent = 100; }
    if (percent == v->percent) {
        return;             // 変わらないなら時刻も触らない (現行と同じ)
    }
    v->percent = percent;
    v->changed_at = now;
    v->changes++;
}

void stackee_volume_state_step(stackee_volume_state_t *v, int delta, uint32_t now) {
    stackee_volume_state_set(v, v->percent + delta, now);
}

void stackee_volume_state_note_input(stackee_volume_state_t *v, uint32_t now) {
    v->last_input_at = now;
}

bool stackee_volume_state_pending(const stackee_volume_state_t *v) {
    return v->percent != v->saved;
}

bool stackee_volume_state_should_save(const stackee_volume_state_t *v,
                                      uint32_t now, bool audio_busy) {
    if (!stackee_volume_state_pending(v) || audio_busy) {
        return false;
    }
    if ((uint32_t)(now - v->changed_at) < STACKEE_VOLUME_SAVE_IDLE_MS) {
        return false;
    }
    if ((uint32_t)(now - v->last_input_at) < STACKEE_VOLUME_SAVE_IDLE_MS) {
        return false;
    }
    return true;
}

void stackee_volume_state_mark_saved(stackee_volume_state_t *v) {
    v->saved = v->percent;
    v->saves++;
}

uint16_t stackee_volume_bits(int percent) {
    // stackee_speaker.py の volume_bits と同じ式。
    //   half_db = round(40 * log10(100 / percent))   (0.5 dB きざみ)
    //   上位バイト = (粗調 6 dB) << 4 | (微調 0.5 dB)
    int half_db;
    if (percent <= 0) {
        half_db = 192;
    } else {
        if (percent > 100) { percent = 100; }
        double db = -40.0 * log10((double)percent / 100.0);
        long rounded = lround(db);
        if (rounded < 0)   { rounded = 0; }
        if (rounded > 192) { rounded = 192; }
        half_db = (int)rounded;
    }
    int coarse = half_db / 12;
    int fine = half_db % 12;
    if (coarse > 15) {
        coarse = 15;
        fine = 12;
    }
    return (uint16_t)(((coarse << 4) | fine) << 8);
}
