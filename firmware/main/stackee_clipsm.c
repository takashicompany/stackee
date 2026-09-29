#include "stackee_clipsm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_jsonlite.h"

const char *const stackee_clip_phase_names[STACKEE_CLIP_SYNC_PHASES] = {
    "scan", "idle", "list", "remove", "fetch", "write", "evict", "aborting",
    "sleep", "clear",
};

static uint32_t now(const stackee_clip_t *c) {
    return c->ops->now_ms();
}

static void logf_(const stackee_clip_t *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logf_(const stackee_clip_t *c, const char *fmt, ...) {
    if (c->ops->log == NULL) {
        return;
    }
    char line[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    c->ops->log(line);
}

// ---------------------------------------------------------------------------
// 決め事
// ---------------------------------------------------------------------------
bool stackee_clip_id_ok(const char *id) {
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    size_t n = 0;
    for (const char *p = id; *p; p++, n++) {
        bool ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_';
        if (!ok || n >= STACKEE_CLIP_ID_MAX) {
            return false;
        }
    }
    return true;
}

// "-12" "1727600000" "1727600000.25" を数とみなす (指数表記は見ない)。
static bool numeric(const char *s) {
    const char *p = s;
    if (*p == '-') {
        p++;
    }
    bool digits = false, dot = false;
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            digits = true;
        } else if (*p == '.' && !dot) {
            dot = true;
        } else {
            return false;
        }
    }
    return digits;
}

int stackee_clip_created_cmp(const char *a, const char *b) {
    if (numeric(a) && numeric(b)) {
        double x = strtod(a, NULL);
        double y = strtod(b, NULL);
        return (x < y) ? -1 : (x > y) ? 1 : 0;
    }
    int r = strcmp(a, b);
    return (r < 0) ? -1 : (r > 0) ? 1 : 0;
}

uint64_t stackee_clip_need_bytes(uint32_t audio_bytes, uint32_t meta_bytes,
                                 uint32_t cluster) {
    if (cluster == 0) {
        cluster = 4096;
    }
    uint64_t a = ((uint64_t)audio_bytes + cluster - 1) / cluster * cluster;
    uint64_t m = ((uint64_t)meta_bytes + cluster - 1) / cluster * cluster;
    // ★ 字幕だけ (audio_bytes = 0) でも .pcm は作らない。メタだけ。
    return a + (m ? m : cluster);
}

// (created, file) の順。a が b より新しければ正。
static int entry_cmp(const stackee_clip_entry_t *a, const stackee_clip_entry_t *b) {
    int r = stackee_clip_created_cmp(a->created, b->created);
    if (r != 0) {
        return r;
    }
    return (a->file > b->file) - (a->file < b->file);
}

static int find_local_file(const stackee_clip_t *c, uint32_t file) {
    for (int i = 0; i < c->count; i++) {
        if (c->local[i].file == file) {
            return i;
        }
    }
    return -1;
}

static int find_srv(const stackee_clip_t *c, const char *id) {
    for (int i = 0; i < c->srv_count; i++) {
        if (strcmp(c->srv[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

// 手元の 1 件が一覧の物と同じ版か。created・audio_bytes、分かれば本文の版も。
// ★ 生成側は同じ話題を**同じ id のまま**作り直す (2026-09-30 実機で踏んだ)。
static bool same_version(const stackee_clip_entry_t *l, const stackee_clip_srv_t *s) {
    return l->audio_bytes == s->audio_bytes && strcmp(l->created, s->created) == 0 &&
           (l->ver == 0 || s->ver == 0 || l->ver == s->ver);
}

// 一覧の物 s と同じ版を手元に持っているか (番号、無ければ -1)。
static int find_current(const stackee_clip_t *c, const stackee_clip_srv_t *s) {
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->local[i].id, s->id) == 0 && same_version(&c->local[i], s)) {
            return i;
        }
    }
    return -1;
}

static void recount(stackee_clip_t *c) {
    c->total_audio = 0;
    for (int i = 0; i < c->count; i++) {
        c->total_audio += c->local[i].audio_bytes;
    }
}

static void drop_local(stackee_clip_t *c, int i) {
    if (i < 0 || i >= c->count) {
        return;
    }
    if (c->load_pending && c->local[i].file == c->load_file) {
        c->load_idx = -1;       // 読んでいる最中のものが消えた (ふつうは起きない)
    }
    memmove(&c->local[i], &c->local[i + 1],
            (size_t)(c->count - i - 1) * sizeof(c->local[0]));
    c->count--;
    recount(c);
}

// ---------------------------------------------------------------------------
// 一覧 (GET /clips)
// ---------------------------------------------------------------------------
// 値そのもの (文字列なら引用符の中、数ならそのまま) を out へ。
static void raw_token(const char *obj, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    const char *at = NULL;
    size_t len = 0;
    if (!stackee_json_raw(obj, key, &at, &len) || len == 0) {
        return;
    }
    if (at[0] == '"') {
        stackee_json_str(obj, key, out, cap);
        return;
    }
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(out, at, len);
    out[len] = '\0';
    // 数の後ろに付いた空白を落とす。
    while (len > 0 && (out[len - 1] == ' ' || out[len - 1] == '\t' ||
                       out[len - 1] == '\r' || out[len - 1] == '\n')) {
        out[--len] = '\0';
    }
}

// 物 1 個 ({ … }) を読む。obj は NUL 終端にしてある。
static bool parse_entry(stackee_clip_t *c, const char *obj, size_t off, size_t len,
                        stackee_clip_srv_t *out) {
    memset(out, 0, sizeof(*out));
    char id[64];
    if (!stackee_json_str(obj, "id", id, sizeof(id)) || !stackee_clip_id_ok(id)) {
        c->skipped_bad++;
        return false;               // id が読めない物は数えもしない
    }
    snprintf(out->id, sizeof(out->id), "%.40s", id);
    raw_token(obj, "created", out->created, sizeof(out->created));
    out->off = (uint32_t)off;
    out->len = (uint32_t)len;
    out->ver = stackee_clip_meta_version(obj, len);
    long audio = -1;
    bool ok = stackee_json_int(obj, "audio_bytes", &audio) && audio >= 0 &&
              (uint32_t)audio <= STACKEE_CLIP_AUDIO_MAX && (audio % 2) == 0;
    if (ok && audio > 0) {
        long rate = 0, channels = 0, width = 0;
        ok = stackee_json_int(obj, "sample_rate", &rate) && rate == 16000 &&
             stackee_json_int(obj, "channels", &channels) && channels == 1 &&
             stackee_json_int(obj, "sample_width", &width) && width == 2;
    }
    if (len > STACKEE_CLIP_META_MAX) {
        ok = false;
    }
    out->audio_bytes = (audio > 0) ? (uint32_t)audio : 0;
    out->ok = ok;
    if (!ok) {
        c->skipped_bad++;
    }
    return true;
}

uint32_t stackee_clip_meta_version(const char *text, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (uint8_t)text[i]) * 16777619u;
    }
    return h ? h : 1;               // 0 は「不明」に使う
}

bool stackee_clip_meta_entry(const char *meta, size_t len, stackee_clip_entry_t *out) {
    memset(out, 0, sizeof(*out));
    char id[64];
    if (meta == NULL || !stackee_json_str(meta, "id", id, sizeof(id)) ||
        !stackee_clip_id_ok(id)) {
        return false;
    }
    long audio = -1;
    if (!stackee_json_int(meta, "audio_bytes", &audio) || audio < 0 ||
        (uint32_t)audio > STACKEE_CLIP_AUDIO_MAX) {
        return false;
    }
    snprintf(out->id, sizeof(out->id), "%.40s", id);
    raw_token(meta, "created", out->created, sizeof(out->created));
    out->audio_bytes = (uint32_t)audio;
    out->meta_bytes = (uint32_t)len;
    out->ver = stackee_clip_meta_version(meta, len);
    return true;
}

int stackee_clip_parse_list(stackee_clip_t *c, char *list, size_t len, long *rev) {
    c->srv_count = 0;
    if (list == NULL || len == 0) {
        return -1;
    }
    list[len] = '\0';               // ★ 受け皿は len + 1 ある (通信側の約束)
    long r = 0;
    if (!stackee_json_int(list, "rev", &r)) {
        return -1;
    }
    const char *at = NULL;
    size_t alen = 0;
    if (!stackee_json_raw(list, "clips", &at, &alen) || alen < 2 || at[0] != '[') {
        return -1;
    }
    char *p = (char *)at + 1;
    char *end = (char *)at + alen;      // ']' の次
    while (p < end) {
        char ch = *p;
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ',') {
            p++;
            continue;
        }
        if (ch == ']') {
            break;
        }
        if (ch != '{') {
            return -1;
        }
        // 対応する '}' を探す (文字列の中の括弧・逃がしを飛ばす)。
        int depth = 0;
        bool in_str = false;
        char *q = p;
        for (; q < end; q++) {
            if (in_str) {
                if (*q == '\\') {
                    q++;
                } else if (*q == '"') {
                    in_str = false;
                }
                continue;
            }
            if (*q == '"') {
                in_str = true;
            } else if (*q == '{') {
                depth++;
            } else if (*q == '}') {
                if (--depth == 0) {
                    break;
                }
            }
        }
        if (q >= end) {
            return -1;
        }
        char saved = q[1];
        q[1] = '\0';
        stackee_clip_srv_t entry;
        bool keep = parse_entry(c, p, (size_t)(p - list), (size_t)(q + 1 - p), &entry);
        q[1] = saved;
        if (keep) {
            // ★ 一覧は古い順。上限を超えたら古いほうから落とす (新しい 64 件を見る)。
            if (c->srv_count == STACKEE_CLIP_MAX) {
                memmove(&c->srv[0], &c->srv[1],
                        (STACKEE_CLIP_MAX - 1) * sizeof(c->srv[0]));
                c->srv_count--;
            }
            c->srv[c->srv_count++] = entry;
        }
        p = q + 1;
    }
    *rev = r;
    return c->srv_count;
}

// ---------------------------------------------------------------------------
// 同期
// ---------------------------------------------------------------------------
static bool job_busy(const stackee_clip_t *c) {
    return c->job.kind != STACKEE_CLIP_JOB_NONE;
}

static void job_reset(stackee_clip_t *c) {
    memset(&c->job, 0, sizeof(c->job));
    atomic_store(&c->job.state, STACKEE_CLIP_JOB_IDLE);
}

static void take_space(stackee_clip_t *c) {
    if (c->job.space_valid) {
        c->space_valid = true;
        c->free_bytes = c->job.free_bytes;
        if (c->job.cluster > 0) {
            c->cluster = c->job.cluster;
        }
    }
}

static bool submit(stackee_clip_t *c) {
    atomic_store(&c->job.state, STACKEE_CLIP_JOB_BUSY);
    if (!c->ops->fs_submit(&c->job)) {
        atomic_store(&c->job.state, STACKEE_CLIP_JOB_IDLE);
        return false;
    }
    return true;
}

static void free_list(stackee_clip_t *c) {
    free(c->list);
    c->list = NULL;
    c->list_len = 0;
    c->srv_count = 0;
    c->cur = -1;
    c->cur_id[0] = '\0';
}

static void close_http(stackee_clip_t *c) {
    if (c->http_open) {
        c->ops->http_close();
        c->http_open = false;
    }
}

static void finish(stackee_clip_t *c, const char *result, uint32_t next_in) {
    close_http(c);
    free_list(c);
    c->phase = STACKEE_CLIP_SYNC_IDLE;
    c->next_at = now(c) + next_in;
    c->last_sync_at = now(c) ? now(c) : 1;
    c->last_result = result;
    c->now_req = false;
    logf_(c, "[clips] 同期 %s (rev %ld、%d 件、空き %llu B)", result, c->rev_seen,
          c->count, (unsigned long long)c->free_bytes);
}

static void fail_sync(stackee_clip_t *c, const char *why) {
    snprintf(c->last_error, sizeof(c->last_error), "%s", why);
    c->sync_fail++;
    finish(c, "error", STACKEE_CLIP_PERIOD_MS);
}

static void done_sync(stackee_clip_t *c) {
    if (c->sync_failed) {
        c->sync_fail++;
        finish(c, "error", STACKEE_CLIP_PERIOD_MS);
        return;
    }
    bool same = c->rev_done_valid && c->rev_seen == c->rev_done &&
                c->downloads + c->removed + c->evicted == c->work_at_start;
    c->rev_done = c->rev_seen;
    c->rev_done_valid = true;
    c->last_error[0] = '\0';
    if (same) {
        c->sync_same++;
        finish(c, "same", STACKEE_CLIP_PERIOD_MS);
        return;
    }
    c->sync_ok++;
    finish(c, "ok", STACKEE_CLIP_PERIOD_MS);
}

static bool start_remove(stackee_clip_t *c, int i, int phase) {
    job_reset(c);
    c->job.kind = STACKEE_CLIP_JOB_REMOVE;
    c->job.file = c->local[i].file;
    snprintf(c->job.id, sizeof(c->job.id), "%s", c->local[i].id);
    c->job_local = i;
    if (!submit(c)) {
        job_reset(c);
        fail_sync(c, "FAT の仕事を渡せません");
        return false;
    }
    c->phase = phase;
    return true;
}

// 一覧の i 番を書く (音声は pcm。字幕だけなら NULL)。pcm の持ち主は worker へ移る。
static void start_write(stackee_clip_t *c, int i, uint8_t *pcm, size_t pcm_len) {
    const stackee_clip_srv_t *s = &c->srv[i];
    char *meta = malloc((size_t)s->len + 1);
    if (meta == NULL) {
        free(pcm);
        fail_sync(c, "メタデータの領域を確保できません");
        return;
    }
    memcpy(meta, c->list + s->off, s->len);
    meta[s->len] = '\0';
    job_reset(c);
    c->job.kind = STACKEE_CLIP_JOB_WRITE;
    c->job.file = c->next_file;
    snprintf(c->job.id, sizeof(c->job.id), "%s", s->id);
    c->job.meta = meta;
    c->job.meta_len = s->len;
    c->job.pcm = pcm;
    c->job.pcm_len = pcm_len;
    if (!submit(c)) {
        free(meta);
        free(pcm);
        job_reset(c);
        fail_sync(c, "FAT の仕事を渡せません");
        return;
    }
    c->phase = STACKEE_CLIP_SYNC_WRITE;
}

// 次の 1 手を決める (消す → 取る の順)。
static void advance(stackee_clip_t *c) {
    // 1. 一覧に無いものを消す。★ 同じ id で中身が変わったもの (古い版) は
    //   ここでは消さない。新しい版を最後まで書けてから入れ替える (失敗・打ち切り
    //   でも鳴らせる 1 件を失わない)。新しい版がもう手元にある古い版だけ消す。
    for (int i = 0; i < c->count; i++) {
        int s = find_srv(c, c->local[i].id);
        bool keep = (s >= 0);
        if (s >= 0 && c->srv[s].ok && !same_version(&c->local[i], &c->srv[s])) {
            int cur = find_current(c, &c->srv[s]);
            keep = (cur < 0);       // 新しい版がまだ無いなら、古い版で鳴らせるように残す
        }
        if (!keep) {
            logf_(c, "[clips] %s: %s", (s >= 0) ? "古い版を消す" : "一覧に無いので消す",
                  c->local[i].id);
            start_remove(c, i, STACKEE_CLIP_SYNC_REMOVE);
            return;
        }
    }
    // 2. 無いものを新しい順に。
    for (;;) {
        int cand = -1;
        for (int i = 0; i < c->srv_count; i++) {
            const stackee_clip_srv_t *s = &c->srv[i];
            if (!s->ok || s->tried || find_current(c, s) >= 0) {
                continue;
            }
            // 一覧は古い順なので、同じ created ならうしろ (i が大きい) が新しい。
            if (cand < 0 ||
                stackee_clip_created_cmp(s->created, c->srv[cand].created) >= 0) {
                cand = i;
            }
        }
        if (cand < 0) {
            done_sync(c);
            return;
        }
        stackee_clip_srv_t *s = &c->srv[cand];
        uint64_t need = stackee_clip_need_bytes(s->audio_bytes, s->len, c->cluster);
        bool full = c->count >= STACKEE_CLIP_MAX;
        bool tight = c->space_valid &&
                     c->free_bytes < need + STACKEE_CLIP_RESERVE_BYTES;
        if (full || tight) {
            // ★ 消してよいのは、これから入れるものより**古い**ものだけ。
            //   先に「全部消しても入らない」を見て、無駄に消さない。
            uint64_t could = c->free_bytes;
            int victim = -1;
            for (int i = 0; i < c->count; i++) {
                // ★ 同じ id の古い版は消さない (新しい版が書けるまで鳴らせるように)。
                if (stackee_clip_created_cmp(c->local[i].created, s->created) >= 0 ||
                    strcmp(c->local[i].id, s->id) == 0) {
                    continue;
                }
                could += stackee_clip_need_bytes(c->local[i].audio_bytes,
                                                 c->local[i].meta_bytes, c->cluster);
                if (victim < 0 || entry_cmp(&c->local[i], &c->local[victim]) < 0) {
                    victim = i;
                }
            }
            bool fits_after = (!c->space_valid ||
                               could >= need + STACKEE_CLIP_RESERVE_BYTES);
            if (victim >= 0 && fits_after) {
                logf_(c, "[clips] 容量のため古いものを消す: %s (入れるもの %s)",
                      c->local[victim].id, s->id);
                start_remove(c, victim, STACKEE_CLIP_SYNC_EVICT);
                return;
            }
            logf_(c, "[clips] 入らないので飛ばす: %s (%lu B)", s->id,
                  (unsigned long)s->audio_bytes);
            s->tried = true;
            c->skipped_space++;
            continue;
        }
        s->tried = true;
        c->cur = cand;
        snprintf(c->cur_id, sizeof(c->cur_id), "%s", s->id);
        if (s->audio_bytes == 0) {
            start_write(c, cand, NULL, 0);          // 字幕だけ
            return;
        }
        char path[STACKEE_CLIP_PATH_MAX + STACKEE_CLIP_ID_MAX + 16];
        snprintf(path, sizeof(path), "%s/%s/audio", c->list_path, s->id);
        if (!c->ops->http_start("GET", path, NULL, 0, s->audio_bytes, NULL)) {
            fail_sync(c, "音声の取得を始められません");
            return;
        }
        c->http_open = true;
        c->phase = STACKEE_CLIP_SYNC_FETCH;
        return;
    }
}

static void start_list(stackee_clip_t *c) {
    if (!c->ops->http_start("GET", c->list_path, NULL, 0, STACKEE_CLIP_LIST_LIMIT,
                            NULL)) {
        fail_sync(c, "一覧の取得を始められません");
        return;
    }
    c->syncs++;
    c->sync_failed = false;
    c->http_open = true;
    c->phase = STACKEE_CLIP_SYNC_LIST;
}

static void handle_list(stackee_clip_t *c, int got, int status) {
    c->last_status = status;
    if (got < 0) {
        close_http(c);
        fail_sync(c, "通信に失敗しました");
        return;
    }
    if (status == 404) {
        // 古い中継。静かに休む (画面には出さない)。
        close_http(c);
        snprintf(c->last_error, sizeof(c->last_error),
                 "中継が /clips に対応していません (HTTP 404)");
        free_list(c);
        c->phase = STACKEE_CLIP_SYNC_SLEEP;
        c->next_at = now(c) + STACKEE_CLIP_404_MS;
        c->last_sync_at = now(c) ? now(c) : 1;
        c->last_result = "404";
        c->now_req = false;
        logf_(c, "[clips] 中継が /clips に対応していない (404)。%u 秒休む",
              (unsigned)(STACKEE_CLIP_404_MS / 1000));
        return;
    }
    if (status != 200) {
        close_http(c);
        char why[48];
        snprintf(why, sizeof(why), "サーバー HTTP %d", status);
        fail_sync(c, why);
        return;
    }
    size_t len = 0;
    uint8_t *body = c->ops->http_take(&len);
    c->http_open = false;
    if (body == NULL) {
        c->ops->http_close();
        fail_sync(c, "一覧を受け取れません");
        return;
    }
    free_list(c);
    c->list = body;
    c->list_len = len;
    long rev = 0;
    if (stackee_clip_parse_list(c, (char *)body, len, &rev) < 0) {
        fail_sync(c, "一覧の形式が不正です");
        return;
    }
    // ★ ここから先は「一覧の取得に成功した」。消してよい。
    c->rev_seen = rev;
    // ★ rev が同じでも一覧と突き合わせる (一覧はもう手元にあるので通信は
    //   増えない)。サーバーが rev を進めずに同じ id を作り直しても取りこぼさない。
    //   何も変わらなければ結末は "same"。
    c->work_at_start = c->downloads + c->removed + c->evicted;
    advance(c);
}

static void handle_fetch(stackee_clip_t *c, int got, int status, size_t len) {
    c->last_status = status;
    int i = c->cur;
    if (got < 0 || i < 0) {
        close_http(c);
        c->fails++;
        fail_sync(c, "音声の取得に失敗しました");
        return;
    }
    if (status == 404) {
        // その 1 件がサーバーから消えた。飛ばして次へ。
        close_http(c);
        logf_(c, "[clips] 音声が無い (404): %s", c->srv[i].id);
        c->cur = -1;
        c->cur_id[0] = '\0';
        advance(c);
        return;
    }
    if (status != 200 || len != c->srv[i].audio_bytes) {
        close_http(c);
        c->fails++;
        char why[64];
        if (status != 200) {
            snprintf(why, sizeof(why), "音声 HTTP %d", status);
        } else {
            snprintf(why, sizeof(why), "音声の長さが違います (%lu / %lu B)",
                     (unsigned long)len, (unsigned long)c->srv[i].audio_bytes);
        }
        fail_sync(c, why);
        return;
    }
    size_t took = 0;
    uint8_t *pcm = c->ops->http_take(&took);
    c->http_open = false;
    if (pcm == NULL || took != len) {
        free(pcm);
        c->ops->http_close();
        fail_sync(c, "音声を受け取れません");
        return;
    }
    start_write(c, i, pcm, took);
}

// 終わった仕事 (LOAD 以外) を片付ける。
static void job_collect(stackee_clip_t *c) {
    if (!job_busy(c)) {
        return;
    }
    int st = atomic_load(&c->job.state);
    if (st == STACKEE_CLIP_JOB_BUSY) {
        return;
    }
    int kind = c->job.kind;
    if (kind == STACKEE_CLIP_JOB_LOAD) {
        if (!c->load_pending) {
            // やめた読み込みの結果が来た。捨てる。
            free(c->job.meta_out);
            free(c->job.pcm_out);
            job_reset(c);
        }
        return;                     // 読み込み中のものは load_poll が受け取る
    }
    take_space(c);
    bool ok = (st == STACKEE_CLIP_JOB_OK);
    if (kind == STACKEE_CLIP_JOB_SCAN) {
        if (ok) {
            c->count = c->job.count;
            c->next_file = c->job.next_file;
            c->scanned = true;
            for (int i = 0; i < c->count; i++) {
                c->local[i].played = false;
            }
            recount(c);
            logf_(c, "[clips] 目録: %d 件 (音声 %llu B、半端物を %lu 件消した、空き %llu B)",
                  c->count, (unsigned long long)c->total_audio,
                  (unsigned long)c->job.cleaned, (unsigned long long)c->free_bytes);
            c->phase = STACKEE_CLIP_SYNC_IDLE;
        } else {
            snprintf(c->last_error, sizeof(c->last_error), "目録を読めません: %.36s",
                     c->job.error);
            logf_(c, "[clips] %s", c->last_error);
            c->next_at = now(c) + STACKEE_CLIP_PERIOD_MS;   // しばらくしてから読み直す
        }
        job_reset(c);
        return;
    }
    if (kind == STACKEE_CLIP_JOB_CLEAR) {
        if (ok) {
            c->count = 0;
            recount(c);
        }
        c->rev_done_valid = false;      // 次の同期は rev が同じでも取り直す
        c->repeat_valid = false;
        c->phase = STACKEE_CLIP_SYNC_IDLE;
        logf_(c, "[clips] 全部消した (%s)", ok ? "ok" : c->job.error);
        job_reset(c);
        return;
    }
    if (kind == STACKEE_CLIP_JOB_REMOVE) {
        int i = find_local_file(c, c->job.file);
        if (ok && i >= 0) {
            drop_local(c, i);
            if (c->phase == STACKEE_CLIP_SYNC_EVICT) {
                c->evicted++;
            } else {
                c->removed++;
            }
        }
        int phase = c->phase;
        char err[48];
        snprintf(err, sizeof(err), "%s", c->job.error);
        job_reset(c);
        if (phase == STACKEE_CLIP_SYNC_ABORTING) {
            c->phase = STACKEE_CLIP_SYNC_IDLE;
            return;
        }
        if (!ok) {
            char why[80];
            snprintf(why, sizeof(why), "FAT から消せません (%s)", err);
            fail_sync(c, why);
            return;
        }
        advance(c);
        return;
    }
    if (kind == STACKEE_CLIP_JOB_WRITE) {
        int phase = c->phase;
        if (ok) {
            if (c->count < STACKEE_CLIP_MAX) {
                stackee_clip_entry_t *e = &c->local[c->count++];
                memset(e, 0, sizeof(*e));
                int s = find_srv(c, c->job.id);
                snprintf(e->id, sizeof(e->id), "%s", c->job.id);
                if (s >= 0) {
                    memcpy(e->created, c->srv[s].created, sizeof(e->created));
                    e->audio_bytes = c->srv[s].audio_bytes;
                    e->meta_bytes = c->srv[s].len;
                    e->ver = c->srv[s].ver;
                }
                // ★ 入れ替えなら新しい版は「まだ鳴らしていない」(played=false)。
                //   古い版は次の advance が消す (新しい版がそろったので)。
                for (int k = 0; k < c->count - 1; k++) {
                    if (strcmp(c->local[k].id, e->id) == 0) {
                        c->replaced++;
                        break;
                    }
                }
                e->file = c->job.file;
                e->played = false;
                recount(c);
            }
            if (c->job.file >= c->next_file) {
                c->next_file = c->job.file + 1;
            }
            c->downloads++;
            logf_(c, "[clips] 取り込んだ: %s (%lu B、%lu ms)", c->job.id,
                  (unsigned long)c->job.pcm_len, (unsigned long)c->job.took_ms);
        } else if (st == STACKEE_CLIP_JOB_FAIL) {
            c->fails++;             // 1 件の取り込みに失敗した
        }
        char err[48];
        snprintf(err, sizeof(err), "%s", c->job.error);
        job_reset(c);
        c->cur = -1;
        c->cur_id[0] = '\0';
        if (phase == STACKEE_CLIP_SYNC_ABORTING) {
            c->phase = STACKEE_CLIP_SYNC_IDLE;
            return;
        }
        if (st == STACKEE_CLIP_JOB_ABORTED) {
            // ★ worker があきらめた (打鍵が 30 秒続いて書けなかった)。書きかけは
            //   worker が静かになってから消す。1 分後に続きから。
            snprintf(c->last_error, sizeof(c->last_error), "書き込みを見送った (%.30s)", err);
            c->aborts++;
            finish(c, "aborted", STACKEE_CLIP_ABORT_RETRY_MS);
            return;
        }
        if (!ok) {
            char why[80];
            snprintf(why, sizeof(why), "FAT に書けません (%s)", err);
            // ★ 書きかけは worker が消してある。rev_done は進めない。
            fail_sync(c, why);
            return;
        }
        advance(c);
        return;
    }
    job_reset(c);
}

// ---------------------------------------------------------------------------
// 公開
// ---------------------------------------------------------------------------
void stackee_clip_init(stackee_clip_t *c, const stackee_clip_ops_t *ops,
                       const char *list_path) {
    memset(c, 0, sizeof(*c));
    c->ops = ops;
    snprintf(c->list_path, sizeof(c->list_path), "%s", list_path ? list_path : "");
    c->phase = STACKEE_CLIP_SYNC_SCAN;
    c->rev_seen = -1;
    c->cur = -1;
    c->job_local = -1;
    c->load_idx = -1;
    c->cluster = 4096;
    c->next_at = 0;
    c->last_result = "";
    c->auto_on = true;
    job_reset(c);
}

static bool active(const stackee_clip_t *c) {
    switch (c->phase) {
        case STACKEE_CLIP_SYNC_LIST:
        case STACKEE_CLIP_SYNC_REMOVE:
        case STACKEE_CLIP_SYNC_FETCH:
        case STACKEE_CLIP_SYNC_WRITE:
        case STACKEE_CLIP_SYNC_EVICT:
            return true;
        default:
            return false;
    }
}

bool stackee_clip_auto_active(const stackee_clip_t *c) {
    return c->auto_on && !c->auto_forced_off;
}

static bool due(const stackee_clip_t *c) {
    return c->scanned && c->list_path[0] != '\0' &&
           (c->phase == STACKEE_CLIP_SYNC_IDLE || c->phase == STACKEE_CLIP_SYNC_SLEEP) &&
           (c->now_req ||
            (stackee_clip_auto_active(c) && (int32_t)(now(c) - c->next_at) >= 0));
}

void stackee_clip_auto_init(stackee_clip_t *c, bool on, bool forced_off) {
    c->auto_on = on;
    c->auto_forced_off = forced_off;
    c->auto_dirty = false;
}

bool stackee_clip_wants_net(const stackee_clip_t *c) {
    return active(c) || (!c->load_pending && due(c));
}

void stackee_clip_abort(stackee_clip_t *c) {
    if (!active(c)) {
        return;
    }
    close_http(c);
    free_list(c);
    c->aborts++;
    c->last_result = "aborted";
    c->last_sync_at = now(c) ? now(c) : 1;
    c->next_at = now(c) + STACKEE_CLIP_ABORT_RETRY_MS;
    c->now_req = false;
    if (job_busy(c) && atomic_load(&c->job.state) == STACKEE_CLIP_JOB_BUSY) {
        // ★ 書きかけ / 消しかけ。worker が止まって後始末を終えるまで待つ
        //   (その間 FAT は読めない。再生の読み込みはそのあと)。
        c->ops->fs_abort();
        c->phase = STACKEE_CLIP_SYNC_ABORTING;
    } else {
        // 終わったばかりの仕事があれば、打ち切りとして片付ける (次へ進ませない)。
        c->phase = STACKEE_CLIP_SYNC_ABORTING;
        job_collect(c);
        c->phase = STACKEE_CLIP_SYNC_IDLE;
    }
    // ★ ここではログを書かない (camera タスクの撮影の押さえからも呼ばれる。
    //   あちらはスタックが小さく、ログにも画面にも触らない約束)。
    //   打ち切った数は aborts、結末は last_result に残る。
}

bool stackee_clip_set_auto(stackee_clip_t *c, bool on) {
    if (on && c->auto_forced_off) {
        return false;
    }
    if (on != c->auto_on) {
        c->auto_on = on;
        c->auto_dirty = true;
        logf_(c, "[clips] 自動取得 %s", on ? "ON" : "OFF");
        if (on) {
            c->next_at = now(c);    // 入れたら、次に暇になったところで 1 回
        }
    }
    if (!on) {
        stackee_clip_abort(c);      // 進行中の同期は打ち切る (書きかけは worker が消す)
        c->now_req = false;
    }
    return true;
}

void stackee_clip_sync_now(stackee_clip_t *c) {
    c->now_req = true;
    if (c->phase == STACKEE_CLIP_SYNC_SLEEP) {
        c->phase = STACKEE_CLIP_SYNC_IDLE;
    }
}

bool stackee_clip_clear(stackee_clip_t *c) {
    stackee_clip_abort(c);
    if (job_busy(c) || c->load_pending) {
        return false;
    }
    job_reset(c);
    c->job.kind = STACKEE_CLIP_JOB_CLEAR;
    if (!submit(c)) {
        job_reset(c);
        return false;
    }
    c->phase = STACKEE_CLIP_SYNC_CLEAR;
    return true;
}

void stackee_clip_step(stackee_clip_t *c, bool can_sync) {
    job_collect(c);
    // 入り切りを残す (NVS もフラッシュに書くので、打鍵が止まってから)。
    if (c->auto_dirty && c->ops->save_auto != NULL &&
        (c->ops->idle_ms == NULL || c->ops->idle_ms() >= STACKEE_CLIP_SAVE_QUIET_MS)) {
        c->auto_dirty = false;
        c->auto_saves++;
        c->ops->save_auto(c->auto_on);
    }
    switch (c->phase) {
        case STACKEE_CLIP_SYNC_SCAN:
            if (!job_busy(c) && !c->load_pending &&
                (int32_t)(now(c) - c->next_at) >= 0) {
                job_reset(c);
                c->job.kind = STACKEE_CLIP_JOB_SCAN;
                c->job.entries = c->local;
                c->job.cap = STACKEE_CLIP_MAX;
                c->count = 0;               // ★ 読み終えるまで 0 件に見せる
                if (!submit(c)) {
                    job_reset(c);
                }
            }
            return;
        case STACKEE_CLIP_SYNC_SLEEP:
        case STACKEE_CLIP_SYNC_IDLE:
            if (!due(c) || !can_sync || c->load_pending || job_busy(c)) {
                return;
            }
            c->phase = STACKEE_CLIP_SYNC_IDLE;
            start_list(c);
            return;
        case STACKEE_CLIP_SYNC_LIST:
        case STACKEE_CLIP_SYNC_FETCH: {
            if (!can_sync) {
                stackee_clip_abort(c);
                return;
            }
            int status = 0;
            const uint8_t *body = NULL;
            size_t len = 0;
            int got = c->ops->http_poll(&status, &body, &len);
            if (got == 0) {
                return;
            }
            if (c->phase == STACKEE_CLIP_SYNC_LIST) {
                handle_list(c, got, status);
            } else {
                handle_fetch(c, got, status, len);
            }
            return;
        }
        case STACKEE_CLIP_SYNC_REMOVE:
        case STACKEE_CLIP_SYNC_WRITE:
        case STACKEE_CLIP_SYNC_EVICT:
            if (!can_sync) {
                stackee_clip_abort(c);
            }
            return;
        case STACKEE_CLIP_SYNC_ABORTING:
            // 仕事が終われば job_collect が idle に戻す。
            if (!job_busy(c)) {
                c->phase = STACKEE_CLIP_SYNC_IDLE;
            }
            return;
        default:
            return;
    }
}

// ---------------------------------------------------------------------------
// 再生の順番と読み込み
// ---------------------------------------------------------------------------
int stackee_clip_pick(const stackee_clip_t *c) {
    int best = -1;
    for (int i = 0; i < c->count; i++) {
        if (c->local[i].played) {
            continue;
        }
        if (best < 0 || entry_cmp(&c->local[i], &c->local[best]) > 0) {
            best = i;
        }
    }
    if (best >= 0 || c->count == 0) {
        return best;
    }
    // 全部鳴らし終えた。古い順に 1 件ずつ (循環の位置より新しいもののうち最古)。
    stackee_clip_entry_t cursor;
    memset(&cursor, 0, sizeof(cursor));
    snprintf(cursor.created, sizeof(cursor.created), "%s", c->repeat_created);
    cursor.file = c->repeat_file;
    int oldest = -1;
    for (int i = 0; i < c->count; i++) {
        if (oldest < 0 || entry_cmp(&c->local[i], &c->local[oldest]) < 0) {
            oldest = i;
        }
        if (c->repeat_valid && entry_cmp(&c->local[i], &cursor) <= 0) {
            continue;
        }
        if (best < 0 || entry_cmp(&c->local[i], &c->local[best]) < 0) {
            best = i;
        }
    }
    return (best >= 0) ? best : oldest;
}

bool stackee_clip_load_begin(stackee_clip_t *c, int idx) {
    if (idx < 0 || idx >= c->count) {
        return false;
    }
    stackee_clip_abort(c);
    c->load_idx = idx;
    c->load_file = c->local[idx].file;
    c->load_pending = true;
    return true;
}

void stackee_clip_load_cancel(stackee_clip_t *c) {
    if (!c->load_pending) {
        return;
    }
    c->load_pending = false;
    c->load_idx = -1;
    job_collect(c);                 // もう来ていれば捨てる
}

static void mark_played(stackee_clip_t *c, uint32_t file) {
    int i = find_local_file(c, file);
    if (i < 0) {
        return;
    }
    stackee_clip_entry_t *e = &c->local[i];
    if (!e->played) {
        e->played = true;
        c->repeat_valid = false;    // 全部済んだら、また古い順の最初から
        return;
    }
    memcpy(c->repeat_created, e->created, sizeof(c->repeat_created));
    c->repeat_file = e->file;
    c->repeat_valid = true;
}

int stackee_clip_load_poll(stackee_clip_t *c, char **meta, size_t *meta_len,
                           uint8_t **pcm, size_t *pcm_len) {
    if (!c->load_pending) {
        return -1;
    }
    job_collect(c);
    if (job_busy(c) && c->job.kind != STACKEE_CLIP_JOB_LOAD) {
        return 0;                   // 打ち切った書き込みの後始末を待つ
    }
    if (!job_busy(c)) {
        int i = find_local_file(c, c->load_file);
        if (i < 0) {
            c->load_pending = false;
            c->load_fails++;
            return -1;
        }
        job_reset(c);
        c->job.kind = STACKEE_CLIP_JOB_LOAD;
        c->job.file = c->load_file;
        snprintf(c->job.id, sizeof(c->job.id), "%s", c->local[i].id);
        c->job.expect_audio = c->local[i].audio_bytes;
        if (!submit(c)) {
            job_reset(c);
            return 0;               // worker が空くのを待つ
        }
        return 0;
    }
    int st = atomic_load(&c->job.state);
    if (st == STACKEE_CLIP_JOB_BUSY) {
        return 0;
    }
    c->load_pending = false;
    c->load_idx = -1;
    if (st != STACKEE_CLIP_JOB_OK || c->job.meta_out == NULL) {
        logf_(c, "[clips] 読めない: %s (%s)", c->job.id, c->job.error);
        free(c->job.meta_out);
        free(c->job.pcm_out);
        c->load_fails++;
        mark_played(c, c->job.file);    // 同じものを押すたびに踏まない
        job_reset(c);
        return -1;
    }
    *meta = c->job.meta_out;
    *meta_len = c->job.meta_out_len;
    *pcm = c->job.pcm_out;
    *pcm_len = c->job.pcm_out_len;
    c->loads++;
    mark_played(c, c->job.file);
    job_reset(c);
    return 1;
}
