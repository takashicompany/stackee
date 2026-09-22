#include "stackee_faceanim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const stackee_face_state_names[STACKEE_FACE_STATES] = {
    "awake", "idle", "listening", "thinking", "speaking", "camera", "microphone",
};

// ---- manifest.json の cases ------------------------------------------------
static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
        p++;
    }
    return p;
}

static bool parse_case(const char **pp, stackee_face_case_t *out) {
    const char *p = skip_ws(*pp);
    if (*p != '[') {
        return false;
    }
    p++;
    out->group_count = 0;
    for (;;) {
        p = skip_ws(p);
        if (*p == ']') { p++; break; }
        if (*p == ',') { p++; continue; }
        if (*p != '[') { return false; }
        p++;
        if (out->group_count >= STACKEE_FACE_MAX_GROUPS) { return false; }
        uint8_t g = out->group_count++;
        out->frame_count[g] = 0;
        for (;;) {
            p = skip_ws(p);
            if (*p == ']') { p++; break; }
            if (*p == ',') { p++; continue; }
            char *end = NULL;
            long v = strtol(p, &end, 10);
            if (end == p || v < 0 || v >= STACKEE_FACE_MAX_COUNT) { return false; }
            if (out->frame_count[g] >= STACKEE_FACE_MAX_FRAMES) { return false; }
            out->frames[g][out->frame_count[g]++] = (uint8_t)v;
            p = end;
        }
        if (out->frame_count[g] == 0) { return false; }
    }
    *pp = p;
    return out->group_count > 0;
}

bool stackee_face_parse_cases(const char *json, stackee_face_cases_t *out) {
    const char *cases = strstr(json, "\"cases\"");
    if (cases == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    for (int s = 0; s < STACKEE_FACE_STATES; s++) {
        char key[24];
        snprintf(key, sizeof(key), "\"%s\":", stackee_face_state_names[s]);
        const char *at = strstr(cases, key);
        if (at == NULL) {
            return false;
        }
        at += strlen(key);
        if (!parse_case(&at, &out->cases[s])) {
            return false;
        }
    }
    return true;
}

// KMK の ticks_diff と同じ「巻き戻りに強い差」。
static inline int32_t ticks_diff(uint32_t now, uint32_t then) {
    return (int32_t)(now - then);
}

static uint32_t next_random(stackee_face_anim_t *a) {
    // xorshift32。Python 版の random.getrandbits(16) の代わり。
    uint32_t x = a->rng;
    if (x == 0) {
        x = 0x2545F491u;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    a->rng = x;
    return (x >> 16) & 0xFFFFu;
}

// stackee_face.py の _different: 前回と同じ番号を避ける。
static int pick_different(stackee_face_anim_t *a, int count, int previous) {
    if (count <= 0) {
        return 0;
    }
    int index = (int)(next_random(a) % (uint32_t)count);
    if (count > 1 && index == previous) {
        index = (index + 1) % count;
    }
    return index;
}

static const stackee_face_case_t *case_of(const stackee_face_anim_t *a, int state) {
    return &a->cases->cases[state];
}

static void anim_frame(stackee_face_anim_t *a, uint32_t now, bool sequential) {
    const stackee_face_case_t *c = case_of(a, a->state);
    int count = c->frame_count[a->group];
    if (count <= 0) {
        count = 1;
    }
    if (sequential) {
        a->frame = (a->frame + 1) % count;
    } else {
        a->frame = pick_different(a, count, a->frame);
    }
    a->frame_at = now;
}

static void anim_group(stackee_face_anim_t *a, uint32_t now) {
    const stackee_face_case_t *c = case_of(a, a->state);
    a->group = pick_different(a, c->group_count, a->group);
    a->frame = -1;
    a->group_at = now;
    anim_frame(a, now, false);
}

static bool state_uses_groups(int state) {
    return state == STACKEE_FACE_IDLE || state == STACKEE_FACE_THINKING ||
           state == STACKEE_FACE_SPEAKING;
}

static bool state_uses_frames(int state) {
    return state == STACKEE_FACE_LISTENING || state == STACKEE_FACE_THINKING ||
           state == STACKEE_FACE_SPEAKING || state == STACKEE_FACE_MICROPHONE;
}

static int anim_update(stackee_face_anim_t *a, int state, uint32_t now) {
    if (state != a->state) {
        a->state = state;
        a->group = -1;
        anim_group(a, now);
        // 受付サインは毎回1コマ目から。同じ状態の間はグループを選び直さない。
        if (state == STACKEE_FACE_MICROPHONE) {
            a->group = 0;
            a->frame = 0;
        }
    } else if (state_uses_groups(state) &&
               ticks_diff(now, a->group_at) >= STACKEE_FACE_GROUP_MS) {
        anim_group(a, now);
    } else if (state_uses_frames(state)) {
        int32_t interval = (state == STACKEE_FACE_THINKING) ? STACKEE_FACE_THINKING_MS
                                                            : STACKEE_FACE_FRAME_MS;
        if (state == STACKEE_FACE_MICROPHONE) {
            interval = (a->frame == 2) ? STACKEE_FACE_MIC_LAST_MS
                                       : STACKEE_FACE_MIC_STEP_MS;
        }
        if (ticks_diff(now, a->frame_at) >= interval) {
            anim_frame(a, now, state != STACKEE_FACE_SPEAKING);
        }
    }
    const stackee_face_case_t *c = case_of(a, a->state);
    return c->frames[a->group][a->frame];
}

// ---------------------------------------------------------------------------

void stackee_face_view_init(stackee_face_view_t *v,
                            const stackee_face_cases_t *cases,
                            const uint8_t *changes, int count, uint32_t now) {
    memset(v, 0, sizeof(*v));
    v->anim.cases = cases;
    v->anim.state = -1;
    v->anim.group = -1;
    v->anim.frame = -1;
    v->anim.rng = 0x2545F491u ^ now;
    v->changes = changes;
    v->count = count;
    v->target = -1;
    v->started = now;
    // 起動時は awake を 1 枚選んで「もう出ている」ことにする
    // (stackee_face.py の __init__ が全面を blit しているのと同じ)。
    v->current = anim_update(&v->anim, STACKEE_FACE_AWAKE, now);
}

void stackee_face_view_note_key(stackee_face_view_t *v, uint32_t now) {
    v->have_last_key = true;
    v->last_key = now;
}

int stackee_face_pick_state(stackee_face_view_t *v,
                            const stackee_face_inputs_t *in, uint32_t now) {
    bool camera_active = in->camera_active;
    if (v->camera_active && !camera_active) {
        v->camera_done = now;
        v->camera_done_valid = true;
    }
    v->camera_active = camera_active;

    if (in->speaking) {
        return STACKEE_FACE_SPEAKING;
    }
    if (in->talk_recording) {
        return STACKEE_FACE_LISTENING;
    }
    // PC 側はマイクを持つ受付サイン。本体の録音より下、考え中より上。
    if (in->mic_held) {
        return STACKEE_FACE_MICROPHONE;
    }
    if (in->talk_busy) {
        return STACKEE_FACE_THINKING;
    }
    if (camera_active ||
        (v->camera_done_valid &&
         ticks_diff(now, v->camera_done) < STACKEE_FACE_CAMERA_HOLD_MS)) {
        return STACKEE_FACE_CAMERA;
    }
    return (ticks_diff(now, v->started) < STACKEE_FACE_AWAKE_MS)
               ? STACKEE_FACE_AWAKE : STACKEE_FACE_IDLE;
}

bool stackee_face_view_busy(const stackee_face_view_t *v) {
    return v->target >= 0;
}

// stackee_face.py の _paint。1 回で最大 16 行。
static bool paint_step(stackee_face_view_t *v, int frame, stackee_face_rect_t *rect) {
    if (v->target < 0) {
        if (frame == v->current) {
            return false;
        }
        size_t offset = (size_t)(v->current * v->count + frame) * 4;
        v->bx      = v->changes[offset + 0];
        v->brow    = v->changes[offset + 1];
        v->bright  = v->changes[offset + 2];
        v->bbottom = v->changes[offset + 3];
        if (v->bbottom == 0) {
            // 変化なし (bbox が空)。画面はそのままで現在値だけ進める。
            v->current = frame;
            return false;
        }
        v->target = frame;
    }
    int end = v->brow + STACKEE_FACE_CHUNK_ROWS;
    if (end > v->bbottom) {
        end = v->bbottom;
    }
    rect->frame = v->target;
    rect->x = v->bx;
    rect->y = v->brow;
    rect->w = v->bright - v->bx;
    rect->h = end - v->brow;
    v->brow = end;
    if (end == v->bbottom) {
        v->current = v->target;
        v->target = -1;
    }
    v->paints++;
    // 1 周ぶん進んだ = true。矩形が空かどうかは描く側が見る
    // (ここで false を返すと、呼び出し側の「終わるまで回す」輪が止まらない)。
    return true;
}

bool stackee_face_view_step_to(stackee_face_view_t *v, int frame,
                               stackee_face_rect_t *rect) {
    return paint_step(v, frame, rect);
}

bool stackee_face_view_tick(stackee_face_view_t *v,
                            const stackee_face_inputs_t *in, uint32_t now,
                            stackee_face_rect_t *rect) {
    v->updates++;
    if (v->frozen) {
        // face.set 中。いまの目標へ寄せるだけ (状態機械は動かさない)。
        return paint_step(v, v->current, rect);
    }
    int state = stackee_face_pick_state(v, in, now);
    // 打鍵の直後は待機中のまばたきを始めない (遷移の途中なら続ける)。
    if (state == STACKEE_FACE_IDLE && v->target < 0 && v->have_last_key &&
        ticks_diff(now, v->last_key) < STACKEE_FACE_TYPING_PAUSE_MS &&
        v->anim.state == STACKEE_FACE_IDLE) {
        v->skipped++;
        return false;
    }
    int frame = anim_update(&v->anim, state, now);
    return paint_step(v, frame, rect);
}
