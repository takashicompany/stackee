// クリップの同期と再生の順番 (main/stackee_clipsm.c) を Mac 上でそのまま走らせる。
//
// 時計・通信・FAT はここが偽物を用意する。FAT は「仕事」を受け取ってから
// fsdelay ms 後に終わる (打ち切りが来ていれば書きかけを残さず ABORTED)。
//
// 台本 (標準入力、1 行 1 命令):
//   t <ms>                 時刻を進める (1 ms ずつ step)
//   can <0|1>              can_sync (暇で通信してよいか。既定 1)
//   resp <status> <body>   次の HTTP の応答。body が "PCM:<bytes>" なら生の音声
//   resperr                次の HTTP は失敗
//   respdelay <ms>         http_start から完了まで (既定 10)
//   fsdelay <ms>           FAT の仕事の時間 (既定 5)
//   fsfree <bytes>         FAT の空き (既定 8 MB)
//   fscluster <bytes>      クラスタ (既定 2048)
//   fsfail <write|remove|load|scan>  次のその仕事を失敗させる
//   have <id> <created> <audio_bytes> [meta_bytes]  起動時に FAT に置いてあるもの
//   abort                  stackee_clip_abort (キーが押された)
//   syncnow                stackee_clip_sync_now
//   clear                  stackee_clip_clear
//   pick                   PICK <番号> <id|->
//   play                   pick → 読み込み (終わるまで回す)。LOAD ok <id> <meta> <pcm> / LOAD fail
//   print                  CLIPINFO …
//   files                  FILE <file> <id> <created> <audio> (FAT の中身)
//   wants                  WANTS <0|1>
//   auto <0|1>             stackee_clip_set_auto。AUTO <ok> <auto>
//   autoinit <on> <forced> 起動時の値 (NVS / settings.toml の代わり)
//   idle <ms>              最後の打鍵からの時間 (NVS へ書く間合い。既定 100000)
//   (入り切りを残すと SAVE <0|1> が出る)
//
// 出てくる行: HTTP / HABORT / FS / FSDONE / LOG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_clipsm.h"
#include "stackee_jsonlite.h"

#define RESP_MAX 32
#define LINE_MAX 70000

static uint32_t g_now;
static stackee_clip_t g_clip;
static bool g_can = true;

// ---- 偽の通信 ---------------------------------------------------------------
static struct {
    int  status;
    bool fail;
    char *body;
    size_t pcm;
} g_resp[RESP_MAX];
static int g_resp_head, g_resp_tail;
static uint32_t g_resp_delay = 10;
static bool g_http_busy;
static uint32_t g_http_done_at;
static size_t g_http_limit;
static uint8_t *g_http_body;
static size_t g_http_len;
static bool g_http_fail;
static int g_http_status;
static bool g_http_loaded;

static uint32_t ops_now(void) { return g_now; }

static bool ops_http_start(const char *method, const char *path, const void *body,
                           size_t body_len, size_t limit, const char *ctype) {
    (void)body;
    (void)body_len;
    (void)ctype;
    if (g_http_busy) {
        printf("HTTP busy\n");
        return false;
    }
    printf("HTTP %s %s %u\n", method, path, (unsigned)limit);
    g_http_busy = true;
    g_http_loaded = false;
    g_http_limit = limit;
    g_http_done_at = g_now + g_resp_delay;
    return true;
}

static int ops_http_poll(int *status, const uint8_t **body, size_t *len) {
    if (!g_http_busy || g_now < g_http_done_at) {
        return 0;
    }
    if (!g_http_loaded) {
        g_http_loaded = true;
        if (g_resp_head == g_resp_tail) {
            g_http_fail = true;
        } else {
            int i = g_resp_head++;
            g_http_fail = g_resp[i].fail;
            g_http_status = g_resp[i].status;
            size_t n = g_resp[i].body ? strlen(g_resp[i].body) : g_resp[i].pcm;
            if (n > g_http_limit) {
                g_http_fail = true;             // 上限超え (通信側は失敗にする)
            } else {
                // ★ 受け皿は limit + 1 (実機と同じ)。NUL 終端つき。
                g_http_body = calloc(g_http_limit + 1, 1);
                if (g_resp[i].body) {
                    memcpy(g_http_body, g_resp[i].body, n);
                }
                g_http_len = n;
            }
            free(g_resp[i].body);
            g_resp[i].body = NULL;
        }
    }
    if (g_http_fail) {
        return -1;
    }
    *status = g_http_status;
    *body = g_http_body;
    *len = g_http_len;
    return 1;
}

static void ops_http_close(void) {
    if (g_http_busy && !g_http_loaded) {
        printf("HABORT %u\n", (unsigned)g_now);
    }
    g_http_busy = false;
    free(g_http_body);
    g_http_body = NULL;
    g_http_len = 0;
}

static uint8_t *ops_http_take(size_t *len) {
    if (!g_http_busy || !g_http_loaded || g_http_fail || g_http_body == NULL) {
        return NULL;
    }
    uint8_t *b = g_http_body;
    *len = g_http_len;
    g_http_body = NULL;
    g_http_len = 0;
    g_http_busy = false;
    return b;
}

// ---- 偽の FAT ---------------------------------------------------------------
typedef struct {
    bool     used;
    uint32_t file;
    char     id[64];
    char     created[64];
    uint32_t audio;
    uint32_t meta_bytes;
} fake_file_t;

static fake_file_t g_files[128];
static uint64_t g_free = 8u * 1024u * 1024u;
static uint32_t g_cluster = 2048;
static uint32_t g_fs_delay = 5;
static stackee_clip_job_t *g_job;
static uint32_t g_job_done_at;
static bool g_job_abort;
static char g_fail_kind[16];
static const char *KIND[] = {"none", "scan", "write", "remove", "load", "clear"};

static uint64_t need(const fake_file_t *f) {
    return stackee_clip_need_bytes(f->audio, f->meta_bytes, g_cluster);
}

static bool ops_fs_submit(stackee_clip_job_t *job) {
    if (g_job != NULL) {
        printf("FS busy\n");
        return false;
    }
    printf("FS %s %s %u\n", KIND[job->kind], job->id[0] ? job->id : "-",
           (unsigned)job->file);
    g_job = job;
    g_job_abort = false;
    g_job_done_at = g_now + g_fs_delay;
    return true;
}

static void ops_fs_abort(void) {
    if (g_job != NULL) {
        printf("FSABORT %u\n", (unsigned)g_now);
        g_job_abort = true;
    }
}

static void ops_log(const char *line) {
    printf("LOG %s\n", line);
}

static bool take_fail(int kind) {
    if (strcmp(g_fail_kind, KIND[kind]) == 0) {
        g_fail_kind[0] = '\0';
        return true;
    }
    return false;
}

static void finish_job(void) {
    stackee_clip_job_t *job = g_job;
    int state = STACKEE_CLIP_JOB_OK;
    bool fail = take_fail(job->kind);
    switch (job->kind) {
        case STACKEE_CLIP_JOB_SCAN: {
            int n = 0;
            uint32_t next = 1;
            for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++) {
                fake_file_t *f = &g_files[i];
                if (!f->used) {
                    continue;
                }
                if (f->file >= next) {
                    next = f->file + 1;
                }
                if (n < job->cap) {
                    stackee_clip_entry_t *e = &job->entries[n++];
                    memset(e, 0, sizeof(*e));
                    snprintf(e->id, sizeof(e->id), "%s", f->id);
                    snprintf(e->created, sizeof(e->created), "%s", f->created);
                    e->audio_bytes = f->audio;
                    e->meta_bytes = f->meta_bytes;
                    e->file = f->file;
                }
            }
            job->count = n;
            job->next_file = next;
            break;
        }
        case STACKEE_CLIP_JOB_WRITE: {
            fake_file_t f;
            memset(&f, 0, sizeof(f));
            f.used = true;
            f.file = job->file;
            snprintf(f.id, sizeof(f.id), "%s", job->id);
            f.audio = (uint32_t)job->pcm_len;
            f.meta_bytes = (uint32_t)job->meta_len;
            // created はメタ (一覧の物そのまま) から読む。
            const char *at = NULL;
            size_t len = 0;
            if (stackee_json_raw(job->meta, "created", &at, &len)) {
                if (at[0] == '"') {
                    stackee_json_str(job->meta, "created", f.created, sizeof(f.created));
                } else {
                    snprintf(f.created, sizeof(f.created), "%.*s", (int)len, at);
                }
            }
            if (g_job_abort) {
                state = STACKEE_CLIP_JOB_ABORTED;       // 書きかけは残さない
            } else if (fail || need(&f) > g_free) {
                state = STACKEE_CLIP_JOB_FAIL;          // 書きかけは残さない
                snprintf(job->error, sizeof(job->error), "%s", fail ? "io" : "full");
            } else {
                for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++) {
                    if (!g_files[i].used) {
                        g_files[i] = f;
                        break;
                    }
                }
                g_free -= need(&f);
            }
            // ★ 持ち主は worker。成否にかかわらず返す (ASan が漏れと二重解放を見る)。
            free(job->meta);
            free(job->pcm);
            job->meta = NULL;
            job->pcm = NULL;
            break;
        }
        case STACKEE_CLIP_JOB_REMOVE:
            if (fail) {
                state = STACKEE_CLIP_JOB_FAIL;
                snprintf(job->error, sizeof(job->error), "io");
                break;
            }
            for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++) {
                if (g_files[i].used && g_files[i].file == job->file) {
                    g_free += need(&g_files[i]);
                    g_files[i].used = false;
                }
            }
            break;
        case STACKEE_CLIP_JOB_LOAD: {
            fake_file_t *hit = NULL;
            for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++) {
                if (g_files[i].used && g_files[i].file == job->file) {
                    hit = &g_files[i];
                }
            }
            if (fail || hit == NULL) {
                state = STACKEE_CLIP_JOB_FAIL;
                snprintf(job->error, sizeof(job->error), "nofile");
                break;
            }
            char meta[256];
            int n = snprintf(meta, sizeof(meta), "{\"id\":\"%s\",\"reply\":\"r-%s\"}",
                             hit->id, hit->id);
            job->meta_out = malloc((size_t)n + 1);
            memcpy(job->meta_out, meta, (size_t)n + 1);
            job->meta_out_len = (size_t)n;
            job->pcm_out = hit->audio ? calloc(hit->audio, 1) : NULL;
            job->pcm_out_len = hit->audio;
            break;
        }
        case STACKEE_CLIP_JOB_CLEAR:
            for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++) {
                if (g_files[i].used) {
                    g_free += need(&g_files[i]);
                    g_files[i].used = false;
                }
            }
            break;
        default:
            break;
    }
    if (job->kind == STACKEE_CLIP_JOB_SCAN && fail) {
        state = STACKEE_CLIP_JOB_FAIL;
        job->count = 0;
    }
    job->space_valid = true;
    job->free_bytes = g_free;
    job->cluster = g_cluster;
    printf("FSDONE %s %d\n", KIND[job->kind], state);
    g_job = NULL;
    atomic_store(&job->state, state);
}

static uint32_t g_idle = 100000;
static uint32_t ops_idle_ms(void) { return g_idle; }
static void ops_save_auto(bool on) { printf("SAVE %d\n", on ? 1 : 0); }

static const stackee_clip_ops_t OPS = {
    .now_ms = ops_now,
    .http_start = ops_http_start,
    .http_poll = ops_http_poll,
    .http_close = ops_http_close,
    .http_take = ops_http_take,
    .fs_submit = ops_fs_submit,
    .fs_abort = ops_fs_abort,
    .log = ops_log,
    .save_auto = ops_save_auto,
    .idle_ms = ops_idle_ms,
};

static void tick(void) {
    if (g_job != NULL && g_now >= g_job_done_at) {
        finish_job();
    }
    stackee_clip_step(&g_clip, g_can);
}

static void run_ms(long ms) {
    for (long i = 0; i < ms; i++) {
        g_now++;
        tick();
    }
}

static void push_resp(int status, bool fail, const char *body) {
    if (g_resp_tail >= RESP_MAX) {
        fprintf(stderr, "too many responses\n");
        exit(2);
    }
    g_resp[g_resp_tail].status = status;
    g_resp[g_resp_tail].fail = fail;
    g_resp[g_resp_tail].body = NULL;
    g_resp[g_resp_tail].pcm = 0;
    if (body && strncmp(body, "PCM:", 4) == 0) {
        g_resp[g_resp_tail].pcm = (size_t)atol(body + 4);
    } else if (body) {
        g_resp[g_resp_tail].body = strdup(body);
    }
    g_resp_tail++;
}

int main(void) {
    stackee_clip_init(&g_clip, &OPS, "/clips");
    static char line[LINE_MAX];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *nl = strchr(line, '\n');
        if (nl) { *nl = '\0'; }
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *arg = strchr(line, ' ');
        if (arg) { *arg++ = '\0'; }
        if (strcmp(line, "t") == 0) {
            run_ms(arg ? atol(arg) : 0);
        } else if (strcmp(line, "can") == 0) {
            g_can = arg && atoi(arg) != 0;
        } else if (strcmp(line, "resp") == 0) {
            char *body = arg ? strchr(arg, ' ') : NULL;
            if (body) { *body++ = '\0'; }
            push_resp(arg ? atoi(arg) : 200, false, body ? body : "");
        } else if (strcmp(line, "resperr") == 0) {
            push_resp(0, true, "");
        } else if (strcmp(line, "respdelay") == 0) {
            g_resp_delay = arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "fsdelay") == 0) {
            g_fs_delay = arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "fsfree") == 0) {
            g_free = arg ? (uint64_t)atoll(arg) : 0;
        } else if (strcmp(line, "fscluster") == 0) {
            g_cluster = arg ? (uint32_t)atol(arg) : 2048;
        } else if (strcmp(line, "fsfail") == 0) {
            snprintf(g_fail_kind, sizeof(g_fail_kind), "%s", arg ? arg : "");
        } else if (strcmp(line, "have") == 0) {
            char id[64] = "", created[64] = "";
            unsigned audio = 0, meta = 200;
            sscanf(arg ? arg : "", "%63s %63s %u %u", id, created, &audio, &meta);
            for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++) {
                if (!g_files[i].used) {
                    fake_file_t *f = &g_files[i];
                    memset(f, 0, sizeof(*f));
                    f->used = true;
                    f->file = (uint32_t)i + 1;
                    snprintf(f->id, sizeof(f->id), "%s", id);
                    snprintf(f->created, sizeof(f->created), "%s", created);
                    f->audio = audio;
                    f->meta_bytes = meta;
                    g_free -= need(f);
                    break;
                }
            }
        } else if (strcmp(line, "auto") == 0) {
            bool ok = stackee_clip_set_auto(&g_clip, arg && atoi(arg) != 0);
            printf("AUTO %d %d\n", ok ? 1 : 0, stackee_clip_auto_active(&g_clip) ? 1 : 0);
        } else if (strcmp(line, "autoinit") == 0) {
            int on = 1, forced = 0;
            sscanf(arg ? arg : "", "%d %d", &on, &forced);
            stackee_clip_auto_init(&g_clip, on != 0, forced != 0);
        } else if (strcmp(line, "idle") == 0) {
            g_idle = arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "abort") == 0) {
            stackee_clip_abort(&g_clip);
        } else if (strcmp(line, "syncnow") == 0) {
            stackee_clip_sync_now(&g_clip);
        } else if (strcmp(line, "clear") == 0) {
            printf("CLEAR %d\n", stackee_clip_clear(&g_clip) ? 1 : 0);
        } else if (strcmp(line, "pick") == 0) {
            int i = stackee_clip_pick(&g_clip);
            printf("PICK %d %s\n", i, i >= 0 ? g_clip.local[i].id : "-");
        } else if (strcmp(line, "play") == 0) {
            int i = stackee_clip_pick(&g_clip);
            if (i < 0 || !stackee_clip_load_begin(&g_clip, i)) {
                printf("LOAD none\n");
                continue;
            }
            char want[64];
            snprintf(want, sizeof(want), "%s", g_clip.local[i].id);
            for (int n = 0; n < 100000; n++) {
                char *meta = NULL;
                uint8_t *pcm = NULL;
                size_t ml = 0, pl = 0;
                int r = stackee_clip_load_poll(&g_clip, &meta, &ml, &pcm, &pl);
                if (r > 0) {
                    printf("LOAD ok %s %u %u\n", want, (unsigned)ml, (unsigned)pl);
                    free(meta);
                    free(pcm);
                    break;
                }
                if (r < 0) {
                    printf("LOAD fail %s\n", want);
                    break;
                }
                g_now++;
                if (g_job != NULL && g_now >= g_job_done_at) {
                    finish_job();
                }
            }
        } else if (strcmp(line, "wants") == 0) {
            printf("WANTS %d\n", stackee_clip_wants_net(&g_clip) ? 1 : 0);
        } else if (strcmp(line, "files") == 0) {
            for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++) {
                if (g_files[i].used) {
                    printf("FILE %u %s %s %u\n", (unsigned)g_files[i].file,
                           g_files[i].id, g_files[i].created,
                           (unsigned)g_files[i].audio);
                }
            }
            printf("FREE %llu\n", (unsigned long long)g_free);
        } else if (strcmp(line, "print") == 0) {
            printf("CLIPINFO phase=%s count=%d scanned=%d rev_done=%ld valid=%d "
                   "syncs=%u ok=%u same=%u fail=%u aborts=%u downloads=%u "
                   "fails=%u removed=%u evicted=%u replaced=%u space=%u bad=%u result=%s "
                   "status=%d next_in=%d auto=%d forced=%d dirty=%d error=%s ids=",
                   stackee_clip_phase_names[g_clip.phase], g_clip.count,
                   g_clip.scanned ? 1 : 0, g_clip.rev_done,
                   g_clip.rev_done_valid ? 1 : 0, (unsigned)g_clip.syncs,
                   (unsigned)g_clip.sync_ok, (unsigned)g_clip.sync_same,
                   (unsigned)g_clip.sync_fail, (unsigned)g_clip.aborts,
                   (unsigned)g_clip.downloads, (unsigned)g_clip.fails,
                   (unsigned)g_clip.removed, (unsigned)g_clip.evicted,
                   (unsigned)g_clip.replaced,
                   (unsigned)g_clip.skipped_space, (unsigned)g_clip.skipped_bad,
                   g_clip.last_result[0] ? g_clip.last_result : "-",
                   g_clip.last_status, (int)(g_clip.next_at - g_now),
                   g_clip.auto_on ? 1 : 0, g_clip.auto_forced_off ? 1 : 0,
                   g_clip.auto_dirty ? 1 : 0,
                   g_clip.last_error[0] ? g_clip.last_error : "-");
            for (int i = 0; i < g_clip.count; i++) {
                printf("%s%s%s", i ? "," : "", g_clip.local[i].id,
                       g_clip.local[i].played ? "*" : "");
            }
            printf("\n");
        } else {
            fprintf(stderr, "unknown command: %s\n", line);
            return 2;
        }
    }
    // 後片付け (ASan の漏れ検査のため)。
    stackee_clip_abort(&g_clip);
    while (g_job != NULL) {
        finish_job();
        stackee_clip_step(&g_clip, false);
    }
    stackee_clip_step(&g_clip, false);
    ops_http_close();
    for (int i = g_resp_head; i < g_resp_tail; i++) {
        free(g_resp[i].body);
    }
    return 0;
}
