#include "stackee_clipfs.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "stackee_fat.h"
#include "stackee_input.h"

static const char *TAG = "clipfs";

#define CLIP_DIR     "clips"
#define SLICE_BYTES  4096
#define PATH_MAX_    96

// ★ フラッシュの消去・書き込みの間は ESP-IDF が両コアのキャッシュを止め、
//   入力タスク (CPU1) も止まる (4 KB で約 10 ms、RESULTS.md「アプリ内 OTA」)。
//   だから**打鍵が止まって 300 ms たってから**しか書かない。書きかけの途中で
//   打鍵が来たら、次の 4 KB は静かになるまで待つ。
#define TYPING_QUIET_US (300 * 1000)
// 打鍵が続いてこれだけ書けなければ、その仕事はあきらめる (同期が受け箱の
// 待ちを譲ってもらったまま止まり続けないように)。書きかけは静かになってから消す。
#define GIVE_UP_US      (30LL * 1000 * 1000)

static stackee_clip_job_t *_Atomic s_job;
static _Atomic bool s_abort;

// 書き込みの途中 (メインループだけが触る)。
typedef struct {
    int      step;          // 0 = まだ / 1 = 音声 / 2 = メタ
    FILE    *f;
    size_t   at;
    int64_t  t0;
    bool     session;
    char     pcm[PATH_MAX_];
} write_state_t;

// ★ 内蔵 RAM の .bss に置かない (約 250 B)。最初の仕事で PSRAM に取る。
//   触るのはメインループ (worker と clips.status) だけで、割り込みからは触らない。
static struct clipfs_state {
    write_state_t         w;
    // 打ち切った書き込みの後始末 (閉じる・書きかけを消す・外す) が、打鍵中で
    // まだ済んでいない。静かになったら最初にやる。その間の読み込み (LOAD) は
    // /rw の下から読めるので待たせない。
    write_state_t         pend;
    bool                  cleanup;
    stackee_clipfs_stats_t stats;
    uint32_t              last_events;
    int64_t               last_key_us;
    int64_t               defer_since;  // 見送り続けている起点 (0 = 見送っていない)
} *S;

#define w        (S->w)
#define s_stats  (S->stats)

bool stackee_clipfs_submit(stackee_clip_job_t *job) {
    stackee_clip_job_t *expected = NULL;
    // ★ 先に打ち切りの旗を下ろしてから載せる (載せたあとに下ろすと、載せた
    //   直後に来た打ち切りを消してしまう)。持っている仕事があるときは触らない。
    if (atomic_load(&s_job) != NULL) {
        return false;
    }
    atomic_store(&s_abort, false);
    return atomic_compare_exchange_strong(&s_job, &expected, job);
}

void stackee_clipfs_abort(void) {
    if (atomic_load(&s_job) != NULL) {
        atomic_store(&s_abort, true);
    }
}

void stackee_clipfs_stats(stackee_clipfs_stats_t *out) {
    if (S == NULL) {
        memset(out, 0, sizeof(*out));
    } else {
        *out = s_stats;
    }
    out->cleanup_pending = (S != NULL) && S->cleanup;
    stackee_clip_job_t *job = atomic_load(&s_job);
    out->busy_kind = job ? job->kind : 0;
}

static void path_of(char *out, size_t cap, uint32_t file, const char *ext) {
    snprintf(out, cap, "%s/" CLIP_DIR "/%08lX.%s", stackee_fat_base(),
             (unsigned long)file, ext);
}

static void note(stackee_clip_job_t *job, const char *why) {
    snprintf(job->error, sizeof(job->error), "%s", why);
    snprintf(s_stats.last_error, sizeof(s_stats.last_error), "%s", why);
}

// 区間の中なら空きを添える。
static void fill_space(stackee_clip_job_t *job) {
    uint64_t free_bytes = 0;
    uint32_t cluster = 0;
    if (stackee_fat_space(&free_bytes, &cluster) == ESP_OK) {
        job->space_valid = true;
        job->free_bytes = free_bytes;
        job->cluster = cluster;
    }
}

static void finish(stackee_clip_job_t *job, int state) {
    s_stats.jobs++;
    s_stats.last_ms = (uint32_t)((esp_timer_get_time() - w.t0) / 1000);
    job->took_ms = s_stats.last_ms;
    if (state == STACKEE_CLIP_JOB_FAIL) {
        s_stats.fails++;
    }
    memset(&w, 0, sizeof(w));
    atomic_store(&s_abort, false);
    atomic_store(&s_job, NULL);
    // ★ 状態は最後に置く (audio タスクはこれを見て結果を読む)。
    atomic_store(&job->state, state);
}

// ---------------------------------------------------------------------------
// 読む
// ---------------------------------------------------------------------------
static void *read_file(const char *path, size_t max, size_t *out_len, bool nul) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size < 0 || (size_t)st.st_size > max) {
        fclose(f);
        return NULL;
    }
    size_t size = (size_t)st.st_size;
    // ★ 大きい (PCM は数 MB)。内蔵 RAM には置かない。
    uint8_t *buf = heap_caps_malloc(size + (nul ? 1 : 0) + (size == 0 ? 1 : 0),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t got = (size > 0) ? fread(buf, 1, size, f) : 0;
    fclose(f);
    if (got != size) {
        free(buf);
        return NULL;
    }
    if (nul) {
        buf[size] = '\0';
    }
    *out_len = size;
    return buf;
}

static void do_load(stackee_clip_job_t *job) {
    char path[PATH_MAX_];
    path_of(path, sizeof(path), job->file, "jsn");
    size_t meta_len = 0;
    char *meta = read_file(path, STACKEE_CLIP_META_MAX, &meta_len, true);
    if (meta == NULL) {
        note(job, "meta");
        finish(job, STACKEE_CLIP_JOB_FAIL);
        return;
    }
    uint8_t *pcm = NULL;
    size_t pcm_len = 0;
    if (job->expect_audio > 0) {
        path_of(path, sizeof(path), job->file, "pcm");
        pcm = read_file(path, STACKEE_CLIP_AUDIO_MAX, &pcm_len, false);
        if (pcm == NULL || pcm_len != job->expect_audio) {
            free(pcm);
            free(meta);
            note(job, pcm ? "pcmlen" : "pcm");
            finish(job, STACKEE_CLIP_JOB_FAIL);
            return;
        }
    }
    job->meta_out = meta;
    job->meta_out_len = meta_len;
    job->pcm_out = pcm;
    job->pcm_out_len = pcm_len;
    finish(job, STACKEE_CLIP_JOB_OK);
}

// ---------------------------------------------------------------------------
// 目録 (起動後に 1 回)
// ---------------------------------------------------------------------------
static bool name_file(const char *name, uint32_t *file, char *ext, size_t ecap) {
    // "0000001A.jsn" (大文字小文字は問わない)。
    const char *dot = strchr(name, '.');
    if (dot == NULL || dot - name != 8) {
        return false;
    }
    char hex[9];
    memcpy(hex, name, 8);
    hex[8] = '\0';
    char *end = NULL;
    unsigned long v = strtoul(hex, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *file = (uint32_t)v;
    snprintf(ext, ecap, "%s", dot + 1);
    return true;
}

static void do_scan(stackee_clip_job_t *job) {
    // ★ 書ける形で付ける (空きとクラスタを数え、半端物を消すため)。
    //   起動のあとメインループが回り始めてからなので、素材は読み終えている。
    if (stackee_fat_session_begin() != ESP_OK) {
        note(job, "session");
        finish(job, STACKEE_CLIP_JOB_FAIL);
        return;
    }
    char dir[PATH_MAX_];
    snprintf(dir, sizeof(dir), "%s/" CLIP_DIR, stackee_fat_base());
    job->count = 0;
    job->next_file = 1;
    DIR *d = opendir(dir);
    if (d == NULL) {
        // まだ 1 件も無い (clips/ は最初の書き込みで作る)。
        fill_space(job);
        stackee_fat_session_end();
        finish(job, STACKEE_CLIP_JOB_OK);
        return;
    }
    char *meta = heap_caps_malloc(STACKEE_CLIP_META_MAX + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 消すものは読み終えてから消す (読みながら消すと readdir が乱れる)。
    enum { DOOMED_MAX = 32 };
    uint32_t doomed[DOOMED_MAX];
    char doomed_ext[DOOMED_MAX][4];
    int ndoomed = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        uint32_t file = 0;
        char ext[8];
        if (!name_file(e->d_name, &file, ext, sizeof(ext))) {
            continue;
        }
        if (file >= job->next_file) {
            job->next_file = file + 1;
        }
        if (strcasecmp(ext, "jsn") == 0) {
            char path[PATH_MAX_];
            path_of(path, sizeof(path), file, "jsn");
            size_t len = 0;
            bool ok = false;
            stackee_clip_entry_t entry;
            FILE *f = (meta != NULL) ? fopen(path, "rb") : NULL;
            if (f != NULL) {
                len = fread(meta, 1, STACKEE_CLIP_META_MAX, f);
                fclose(f);
                meta[len] = '\0';
                ok = stackee_clip_meta_entry(meta, len, &entry);
            }
            if (ok && entry.audio_bytes > 0) {
                struct stat st;
                path_of(path, sizeof(path), file, "pcm");
                ok = stat(path, &st) == 0 && (uint32_t)st.st_size == entry.audio_bytes;
            }
            // 同じ id が 2 つ = 入れ替えの途中で切れた。**番号の大きいほう
            // (あとから書いた新しい版)** を残し、古いほうを消す。
            if (ok) {
                int dup = -1;
                for (int i = 0; i < job->count; i++) {
                    if (strcmp(job->entries[i].id, entry.id) == 0) {
                        dup = i;
                    }
                }
                if (dup >= 0) {
                    uint32_t older = file;
                    if (file > job->entries[dup].file) {
                        older = job->entries[dup].file;
                        entry.file = file;
                        job->entries[dup] = entry;
                    }
                    if (ndoomed < DOOMED_MAX) {
                        doomed[ndoomed] = older;
                        snprintf(doomed_ext[ndoomed], sizeof(doomed_ext[0]), "jsn");
                        ndoomed++;
                    }
                    continue;
                }
            }
            if (ok && job->count < job->cap) {
                entry.file = file;
                job->entries[job->count++] = entry;
                continue;
            }
            if (ok) {
                continue;               // 上限を超えたぶん (消さずに見ないだけ)
            }
            if (ndoomed < DOOMED_MAX) {
                doomed[ndoomed] = file;
                snprintf(doomed_ext[ndoomed], sizeof(doomed_ext[0]), "jsn");
                ndoomed++;
            }
        }
    }
    closedir(d);
    free(meta);
    // .pcm / .tmp で、生きている .jsn の無いものは半端物 (書きかけ・消しかけ)。
    d = opendir(dir);
    while (d != NULL && (e = readdir(d)) != NULL) {
        uint32_t file = 0;
        char ext[8];
        if (!name_file(e->d_name, &file, ext, sizeof(ext)) ||
            strcasecmp(ext, "jsn") == 0) {
            continue;
        }
        // .pcm は、対の .jsn があって消す予定に入っていなければ生きている。
        bool live = false;
        if (strcasecmp(ext, "pcm") == 0) {
            char path[PATH_MAX_];
            struct stat st;
            path_of(path, sizeof(path), file, "jsn");
            live = stat(path, &st) == 0;
            for (int i = 0; live && i < ndoomed; i++) {
                if (doomed[i] == file) {
                    live = false;
                }
            }
        }
        if (!live && ndoomed < DOOMED_MAX) {
            doomed[ndoomed] = file;
            snprintf(doomed_ext[ndoomed], sizeof(doomed_ext[0]), "%.3s", ext);
            ndoomed++;
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    for (int i = 0; i < ndoomed; i++) {
        char path[PATH_MAX_];
        path_of(path, sizeof(path), doomed[i], doomed_ext[i]);
        if (remove(path) == 0) {
            job->cleaned++;
        }
        // 壊れた .jsn なら対の .pcm も (次の周の読み直しで拾うが、ここで消す)。
        if (strcasecmp(doomed_ext[i], "jsn") == 0) {
            path_of(path, sizeof(path), doomed[i], "pcm");
            remove(path);
        }
    }
    fill_space(job);
    stackee_fat_session_end();
    ESP_LOGI(TAG, "目録: %d 件、半端物 %lu 件を消した", job->count,
             (unsigned long)job->cleaned);
    finish(job, STACKEE_CLIP_JOB_OK);
}

// ---------------------------------------------------------------------------
// 消す
// ---------------------------------------------------------------------------
static void do_remove(stackee_clip_job_t *job) {
    if (stackee_fat_session_begin() != ESP_OK) {
        note(job, "session");
        finish(job, STACKEE_CLIP_JOB_FAIL);
        return;
    }
    char path[PATH_MAX_];
    // ★ .jsn (確定の印) を先に消す。途中で切れても .pcm だけが残り、次の
    //   起動の目録で半端物として消える。
    path_of(path, sizeof(path), job->file, "jsn");
    bool ok = (remove(path) == 0) || errno == ENOENT;
    path_of(path, sizeof(path), job->file, "pcm");
    remove(path);
    path_of(path, sizeof(path), job->file, "tmp");
    remove(path);
    fill_space(job);
    stackee_fat_session_end();
    if (!ok) {
        note(job, "remove");
    }
    finish(job, ok ? STACKEE_CLIP_JOB_OK : STACKEE_CLIP_JOB_FAIL);
}

static void do_clear(stackee_clip_job_t *job) {
    if (stackee_fat_session_begin() != ESP_OK) {
        note(job, "session");
        finish(job, STACKEE_CLIP_JOB_FAIL);
        return;
    }
    char dir[PATH_MAX_];
    snprintf(dir, sizeof(dir), "%s/" CLIP_DIR, stackee_fat_base());
    // 1 周で読んで、名前を控えてから消す (readdir と remove を混ぜない)。
    for (int round = 0; round < 8; round++) {
        enum { BATCH = 32 };
        uint32_t files[BATCH];
        char exts[BATCH][4];
        int n = 0;
        DIR *d = opendir(dir);
        if (d == NULL) {
            break;
        }
        struct dirent *e;
        while (n < BATCH && (e = readdir(d)) != NULL) {
            uint32_t file = 0;
            char ext[8];
            if (name_file(e->d_name, &file, ext, sizeof(ext))) {
                files[n] = file;
                snprintf(exts[n], sizeof(exts[0]), "%.3s", ext);
                n++;
            }
        }
        closedir(d);
        if (n == 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            char path[PATH_MAX_];
            path_of(path, sizeof(path), files[i], exts[i]);
            remove(path);
        }
    }
    fill_space(job);
    stackee_fat_session_end();
    finish(job, STACKEE_CLIP_JOB_OK);
}

// ---------------------------------------------------------------------------
// 書く (1 周に 4 KB ずつ)
// ---------------------------------------------------------------------------
static void cleanup_state(write_state_t *st, bool remove_pcm) {
    if (st->f != NULL) {
        fclose(st->f);
        st->f = NULL;
    }
    if (remove_pcm && st->pcm[0] != '\0') {
        remove(st->pcm);
        st->pcm[0] = '\0';
    }
    if (st->session) {
        stackee_fat_session_end();
        st->session = false;
    }
}

static void write_cleanup(bool remove_pcm) {
    cleanup_state(&w, remove_pcm);
}

static void write_done(stackee_clip_job_t *job, int state) {
    free(job->meta);
    free(job->pcm);
    job->meta = NULL;
    job->pcm = NULL;
    finish(job, state);
}

static void write_fail(stackee_clip_job_t *job, const char *why) {
    // ★ 書きかけは残さない (.pcm を消す。.jsn はまだ作っていない)。
    write_cleanup(true);
    note(job, why);
    ESP_LOGW(TAG, "書けない: %s (%s)", job->id, why);
    write_done(job, STACKEE_CLIP_JOB_FAIL);
}

static bool typing_now(void) {
    stackee_input_stats_t in;
    stackee_input_stats(&in);
    int64_t t = esp_timer_get_time();
    if (in.key_events != S->last_events || in.keys_down > 0) {
        S->last_events = in.key_events;
        S->last_key_us = t;
    }
    return (t - S->last_key_us) < TYPING_QUIET_US;
}

// 打ち切り (キー・あきらめ)。静かなら後始末をすぐ、打鍵中なら後回しにして
// 結果だけ先に返す (次の読み込みを待たせない)。
static void write_abort(stackee_clip_job_t *job, bool quiet, const char *why) {
    if (quiet) {
        write_cleanup(true);
    } else {
        S->pend = w;
        S->cleanup = true;
        memset(&w, 0, sizeof(w));
    }
    s_stats.aborts++;
    note(job, why);
    ESP_LOGI(TAG, "書き込みを打ち切った: %s (%s、書きかけは%s)", job->id, why,
             quiet ? "消した" : "静かになってから消す");
    write_done(job, STACKEE_CLIP_JOB_ABORTED);
}

static void do_write_step(stackee_clip_job_t *job) {
    if (w.step == 0) {
        if (stackee_fat_session_begin() != ESP_OK) {
            note(job, "session");
            write_done(job, STACKEE_CLIP_JOB_FAIL);
            return;
        }
        w.session = true;
        char dir[PATH_MAX_];
        snprintf(dir, sizeof(dir), "%s/" CLIP_DIR, stackee_fat_base());
        if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
            write_fail(job, "mkdir");
            return;
        }
        // 容量の最後の確かめ (同期の側でも見ているが、実物で念を押す)。
        uint64_t free_bytes = 0;
        uint32_t cluster = 0;
        if (stackee_fat_space(&free_bytes, &cluster) == ESP_OK &&
            free_bytes < stackee_clip_need_bytes((uint32_t)job->pcm_len,
                                                 (uint32_t)job->meta_len, cluster)) {
            write_fail(job, "full");
            return;
        }
        if (job->pcm_len > 0) {
            path_of(w.pcm, sizeof(w.pcm), job->file, "pcm");
            w.f = fopen(w.pcm, "wb");
            if (w.f == NULL) {
                write_fail(job, "open");
                return;
            }
            setvbuf(w.f, NULL, _IONBF, 0);
        }
        w.step = (job->pcm_len > 0) ? 1 : 2;
        return;
    }
    if (w.step == 1) {
        size_t n = job->pcm_len - w.at;
        if (n > SLICE_BYTES) {
            n = SLICE_BYTES;
        }
        int64_t t0 = esp_timer_get_time();
        size_t wrote = fwrite(job->pcm + w.at, 1, n, w.f);
        uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
        if (us > s_stats.max_slice_us) {
            s_stats.max_slice_us = us;
        }
        s_stats.slices++;
        if (wrote != n) {
            write_fail(job, "write");
            return;
        }
        w.at += n;
        s_stats.bytes += n;
        if (w.at < job->pcm_len) {
            return;                 // 次の周へ (間にコンソールと 1 ms の休み)
        }
        int closed = fclose(w.f);
        w.f = NULL;
        struct stat st;
        if (closed != 0 || stat(w.pcm, &st) != 0 || (size_t)st.st_size != job->pcm_len) {
            write_fail(job, "verify");
            return;
        }
        w.step = 2;
        return;
    }
    // w.step == 2: メタを .tmp に書いて .jsn へ改名 (ここが確定)。
    char tmp[PATH_MAX_], jsn[PATH_MAX_];
    path_of(tmp, sizeof(tmp), job->file, "tmp");
    path_of(jsn, sizeof(jsn), job->file, "jsn");
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        write_fail(job, "open_meta");
        return;
    }
    size_t wrote = fwrite(job->meta, 1, job->meta_len, f);
    if (fclose(f) != 0 || wrote != job->meta_len) {
        remove(tmp);
        write_fail(job, "write_meta");
        return;
    }
    remove(jsn);
    if (rename(tmp, jsn) != 0) {
        remove(tmp);
        write_fail(job, "rename");
        return;
    }
    s_stats.bytes += job->meta_len;
    s_stats.writes++;
    fill_space(job);
    w.pcm[0] = '\0';
    write_cleanup(false);
    write_done(job, STACKEE_CLIP_JOB_OK);
}

void stackee_clipfs_poll(void) {
    // 打ち切った書き込みの後始末 (静かになってから)。
    if (S != NULL && S->cleanup && !typing_now()) {
        cleanup_state(&S->pend, true);
        S->cleanup = false;
        ESP_LOGI(TAG, "打ち切った書き込みの後始末を済ませた");
    }
    stackee_clip_job_t *job = atomic_load(&s_job);
    if (job == NULL) {
        return;
    }
    if (S == NULL) {
        S = heap_caps_calloc(1, sizeof(*S), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (S == NULL) {
            S = heap_caps_calloc(1, sizeof(*S), MALLOC_CAP_8BIT);
        }
        if (S == NULL) {
            snprintf(job->error, sizeof(job->error), "nomem");
            atomic_store(&s_job, NULL);
            atomic_store(&job->state, STACKEE_CLIP_JOB_FAIL);
            return;
        }
    }
    if (w.t0 == 0) {
        w.t0 = esp_timer_get_time();
    }
    // ★ フラッシュに書く仕事は、打鍵が止まって 300 ms たってから。読むだけ
    //   (LOAD) は待たない (押したクリップをすぐ鳴らす)。
    if (job->kind != STACKEE_CLIP_JOB_LOAD) {
        bool quiet = !typing_now();
        if (job->kind == STACKEE_CLIP_JOB_WRITE && atomic_load(&s_abort)) {
            write_abort(job, quiet, "abort");
            S->defer_since = 0;
            return;
        }
        if (!quiet || S->cleanup) {
            int64_t t = esp_timer_get_time();
            s_stats.deferred++;
            if (S->defer_since == 0) {
                S->defer_since = t;
            } else if (t - S->defer_since > GIVE_UP_US) {
                S->defer_since = 0;
                s_stats.gave_up++;
                if (job->kind == STACKEE_CLIP_JOB_WRITE) {
                    write_abort(job, false, "typing");
                } else {
                    note(job, "typing");
                    finish(job, STACKEE_CLIP_JOB_FAIL);
                }
            }
            return;
        }
        S->defer_since = 0;
    }
    switch (job->kind) {
        case STACKEE_CLIP_JOB_SCAN:
            do_scan(job);
            return;
        case STACKEE_CLIP_JOB_LOAD:
            do_load(job);
            return;
        case STACKEE_CLIP_JOB_REMOVE:
            do_remove(job);
            return;
        case STACKEE_CLIP_JOB_CLEAR:
            do_clear(job);
            return;
        case STACKEE_CLIP_JOB_WRITE:
            do_write_step(job);
            return;
        default:
            note(job, "kind");
            finish(job, STACKEE_CLIP_JOB_FAIL);
            return;
    }
}
