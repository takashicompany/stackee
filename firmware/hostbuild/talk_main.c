// 会話の状態機械 (main/stackee_talksm.c) を Mac 上でそのまま走らせる。
//
// 時計もマイクもスピーカーも通信も、ぜんぶここが偽物を用意する。**本物の
// 状態機械**に台本を流し込んで、出てきた出来事の列を見る。だから
// 「ポーリングが 1 秒おきか」「一次回答が終わるまで最終回答を鳴らさないか」
// 「390 秒でタイムアウトするか」を、実機も待ち時間も無しで確かめられる。
//
// 台本 (標準入力、1 行 1 命令):
//   t <ms>            時刻を進める (1 ms ずつ step を回す)
//   press / release   STK_TALK の押し離し
//   inject <samples>  録音の代わりに PCM を注入する
//   mic <n>           record_read が 1 回に返すサンプル数 (-1 で失敗)
//   net <0|1>         Wi-Fi が繋がっているか
//   resp <status> <body>   次の HTTP 完了で返すもの。body が "PCM:<n>" なら
//                          n サンプルぶんの生 PCM
//   subs <status> <body>   同じだが body の \t \n を本物のタブ・改行に直す
//                          (字幕の本文 "<start_ms>\t<text>\n" を書くため)
//   resperr           次の HTTP は失敗する
//   respdelay <ms>    http_start から完了までの時間
//   ack <0|1>         一次回答を鳴らすか
//   ackms <ms>        一次回答の長さ
//   allocfail         録音の領域を確保できないことにする
//   sticky <status> <body> 予約を使い切ったあと、ずっとこれを返す
//   playblock         再生を始められないことにする
//   playfail          再生が失敗したことにする
//   print             いまの状態名を出す
//
// 出てくる行: STATE / SHOW / HTTP / ACK / PLAY / REC / TIME / SUB
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_talksm.h"

#define RESP_MAX 16
// ★ done の JSON には字幕の本文 (最大 4 KB、逃がしで少し増える) が混ざる。
//   台本の 1 行も応答の入れ物も、それが丸ごと入る大きさにしておくこと。
//   ここが小さいと JSON が途中で切れ、「解析が壊れた」ように見える。
#define BODY_MAX 8192
#define LINE_MAX 16384

static uint32_t g_now;
static stackee_talk_t g_talk;

static int  g_mic_chunk = 240;
static bool g_net = true;
static bool g_ack_enabled = true;
static uint32_t g_ack_ms = 0;
static bool g_play_fail;

// 偽の一次回答 / 再生
static uint32_t g_ack_until;
static bool     g_ack_running;
static uint32_t g_play_until;
static bool     g_play_running;

// 偽の HTTP
static struct {
    int      status;
    bool     fail;
    char     body[BODY_MAX];
    int      pcm_samples;
} g_resp[RESP_MAX];
static int g_resp_head, g_resp_tail;
static uint32_t g_resp_delay = 10;
// 予約した応答を使い切ったあと、ずっと返し続けるもの (ポーリングの検査用)。
static bool g_sticky_set;
static int  g_sticky_status;
static char g_sticky_body[BODY_MAX];
// play_begin を必ず失敗させる (スピーカー待ちのタイムアウトの検査用)。
static bool g_play_block;

static bool     g_http_busy;
static uint32_t g_http_done_at;
static int      g_http_status;
static bool     g_http_fail;
static uint8_t *g_http_body;
static size_t   g_http_len;

// ★ 録音の領域は **本当に malloc する**。実機では 960 KB あるので、
//   取りっぱなし・二重解放をここで捕まえたい (ASan つきでビルドしている)。
static uint8_t *g_record;
static int g_alloc_count, g_release_count;
static bool g_alloc_fail;

static uint32_t ops_now(void) { return g_now; }

static int16_t *ops_record_alloc(int max_samples) {
    if (g_alloc_fail) {
        return NULL;
    }
    if (g_record != NULL) {
        return (int16_t *)(g_record + 44);      // 既に持っている
    }
    g_record = malloc(44 + (size_t)max_samples * 2);
    if (g_record == NULL) {
        return NULL;
    }
    g_alloc_count++;
    return (int16_t *)(g_record + 44);
}

static void ops_record_release(void) {
    if (g_record == NULL) {
        return;
    }
    free(g_record);
    g_record = NULL;
    g_release_count++;
}

static bool ops_record_begin(void) {
    printf("REC begin\n");
    return true;
}

static int ops_record_read(int16_t *dst, int max) {
    if (g_mic_chunk < 0) {
        return -1;
    }
    int n = g_mic_chunk < max ? g_mic_chunk : max;
    for (int i = 0; i < n; i++) {
        dst[i] = 0;
    }
    return n;
}

static void ops_record_end(void) {
    printf("REC end\n");
}

static bool ops_ack_begin(void) {
    if (!g_ack_enabled || g_ack_ms == 0) {
        printf("ACK skip\n");
        return false;
    }
    g_ack_running = true;
    g_ack_until = g_now + g_ack_ms;
    printf("ACK %u\n", (unsigned)g_ack_ms);
    return true;
}

static bool ops_ack_active(void) {
    return g_ack_running;
}

static bool ops_play_begin(const int16_t *pcm, int samples) {
    (void)pcm;
    if (g_ack_running || g_play_block) {
        return false;
    }
    g_play_running = true;
    g_play_until = g_now + (uint32_t)(samples * 1000 / STACKEE_TALK_RATE);
    printf("PLAY %d\n", samples);
    return true;
}

static bool ops_play_active(void) { return g_play_running; }
static bool ops_play_failed(void) { return g_play_fail; }
static bool ops_net_ready(void)   { return g_net; }

static bool ops_http_start(const char *method, const char *path,
                           const void *body, size_t body_len, size_t limit) {
    (void)body;
    if (g_http_busy) {
        printf("HTTP busy\n");
        return false;
    }
    printf("HTTP %s %s %u %u\n", method, path, (unsigned)body_len, (unsigned)limit);
    g_http_busy = true;
    g_http_done_at = g_now + g_resp_delay;
    return true;
}

static int ops_http_poll(int *status, const uint8_t **body, size_t *len) {
    if (!g_http_busy || g_now < g_http_done_at) {
        return 0;
    }
    if (g_http_body == NULL) {
        // 予約してある応答を取り出す。
        if (g_resp_head == g_resp_tail && g_sticky_set) {
            g_http_fail = false;
            g_http_status = g_sticky_status;
            g_http_len = strlen(g_sticky_body);
            g_http_body = calloc(g_http_len + 1, 1);
            memcpy(g_http_body, g_sticky_body, g_http_len);
        } else if (g_resp_head == g_resp_tail) {
            g_http_fail = true;
        } else {
            int i = g_resp_head++;
            g_http_fail = g_resp[i].fail;
            g_http_status = g_resp[i].status;
            if (g_resp[i].pcm_samples > 0) {
                g_http_len = (size_t)g_resp[i].pcm_samples * 2;
                g_http_body = calloc(g_http_len + 1, 1);
            } else {
                g_http_len = strlen(g_resp[i].body);
                g_http_body = calloc(g_http_len + 1, 1);
                memcpy(g_http_body, g_resp[i].body, g_http_len);
            }
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
    g_http_busy = false;
    g_http_fail = false;
    free(g_http_body);
    g_http_body = NULL;
    g_http_len = 0;
}

static void ops_wav_header(int16_t *pcm, int samples) {
    // 実機と同じ場所 (PCM の 44 バイト前) に触る。はみ出していれば ASan が言う。
    uint8_t *h = (uint8_t *)pcm - 44;
    memcpy(h, "RIFF", 4);
    uint32_t size = (uint32_t)samples * 2;
    memcpy(h + 40, &size, 4);
}

static void ops_show(const char *text) {
    printf("SHOW %s\n", text);
}

static void ops_log(const char *line) {
    printf("LOG %s\n", line);
}

// 字幕の帯。ページが変わった時にだけ来るはず (毎周来たらそれが欠陥)。
// ★ 帯は最大 4 行。1 行 1 出力の形を崩さないよう、改行は `\n` に逃がす。
static void ops_subtitle(const char *text) {
    if (text == NULL || text[0] == '\0') {
        printf("SUB -\n");
        return;
    }
    printf("SUB ");
    for (const char *p = text; *p != '\0'; p++) {
        if (*p == '\n') { printf("\\n"); } else { putchar(*p); }
    }
    printf("\n");
}

static void ops_face(bool recording, bool busy, bool speaking) {
    (void)recording;
    (void)busy;
    (void)speaking;
}

static const stackee_talk_ops_t OPS = {
    .now_ms = ops_now,
    .record_alloc = ops_record_alloc,
    .record_release = ops_record_release,
    .record_begin = ops_record_begin,
    .record_read = ops_record_read,
    .record_end = ops_record_end,
    .ack_begin = ops_ack_begin,
    .ack_active = ops_ack_active,
    .play_begin = ops_play_begin,
    .play_active = ops_play_active,
    .play_failed = ops_play_failed,
    .net_ready = ops_net_ready,
    .http_start = ops_http_start,
    .http_poll = ops_http_poll,
    .http_close = ops_http_close,
    .wav_header = ops_wav_header,
    .show = ops_show,
    .log = ops_log,
    .face = ops_face,
    .subtitle = ops_subtitle,
};

static int g_last_state = -1;

static void tick(void) {
    if (g_ack_running && g_now >= g_ack_until) {
        g_ack_running = false;
        printf("ACK done\n");
    }
    if (g_play_running && g_now >= g_play_until) {
        g_play_running = false;
        printf("PLAY done\n");
    }
    stackee_talk_step(&g_talk);
    if (g_talk.state != g_last_state) {
        g_last_state = g_talk.state;
        printf("STATE %s %u\n", stackee_talk_state_names[g_talk.state],
               (unsigned)g_now);
    }
}

int main(void) {
    stackee_talk_init(&g_talk, &OPS, "/talk");
    g_last_state = g_talk.state;
    printf("STATE %s 0\n", stackee_talk_state_names[g_talk.state]);

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
            long ms = arg ? atol(arg) : 0;
            for (long i = 0; i < ms; i++) {
                g_now++;
                tick();
            }
        } else if (strcmp(line, "press") == 0) {
            stackee_talk_set_pressed(&g_talk, true);
        } else if (strcmp(line, "release") == 0) {
            stackee_talk_set_pressed(&g_talk, false);
        } else if (strcmp(line, "inject") == 0) {
            int n = arg ? atoi(arg) : 16000;
            static int16_t pcm[STACKEE_TALK_MAX_SAMPLES];
            printf("INJECT %d\n", stackee_talk_inject(&g_talk, pcm, n) ? 1 : 0);
        } else if (strcmp(line, "mic") == 0) {
            g_mic_chunk = arg ? atoi(arg) : 240;
        } else if (strcmp(line, "net") == 0) {
            g_net = arg && atoi(arg) != 0;
        } else if (strcmp(line, "ack") == 0) {
            g_ack_enabled = arg && atoi(arg) != 0;
        } else if (strcmp(line, "ackms") == 0) {
            g_ack_ms = arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "allocfail") == 0) {
            g_alloc_fail = true;
        } else if (strcmp(line, "playblock") == 0) {
            g_play_block = true;
        } else if (strcmp(line, "sticky") == 0) {
            char *body = arg ? strchr(arg, ' ') : NULL;
            if (body) { *body++ = '\0'; }
            g_sticky_set = true;
            g_sticky_status = arg ? atoi(arg) : 200;
            snprintf(g_sticky_body, sizeof(g_sticky_body), "%s", body ? body : "{}");
        } else if (strcmp(line, "playfail") == 0) {
            g_play_fail = true;
        } else if (strcmp(line, "respdelay") == 0) {
            g_resp_delay = arg ? (uint32_t)atol(arg) : 0;
        } else if (strcmp(line, "resperr") == 0) {
            if (g_resp_tail < RESP_MAX) {
                g_resp[g_resp_tail].fail = true;
                g_resp[g_resp_tail].pcm_samples = 0;
                g_resp[g_resp_tail].body[0] = '\0';
                g_resp_tail++;
            }
        } else if (strcmp(line, "resp") == 0) {
            char *body = arg ? strchr(arg, ' ') : NULL;
            if (body) { *body++ = '\0'; }
            if (g_resp_tail < RESP_MAX) {
                g_resp[g_resp_tail].fail = false;
                g_resp[g_resp_tail].status = arg ? atoi(arg) : 200;
                g_resp[g_resp_tail].pcm_samples = 0;
                g_resp[g_resp_tail].body[0] = '\0';
                if (body && strncmp(body, "PCM:", 4) == 0) {
                    g_resp[g_resp_tail].pcm_samples = atoi(body + 4);
                } else if (body) {
                    snprintf(g_resp[g_resp_tail].body,
                             sizeof(g_resp[g_resp_tail].body), "%s", body);
                }
                g_resp_tail++;
            }
        } else if (strcmp(line, "subs") == 0) {
            // body の "\t" と "\n" を本物のタブ・改行に直してから積む。
            char *body = arg ? strchr(arg, ' ') : NULL;
            if (body) { *body++ = '\0'; }
            if (g_resp_tail < RESP_MAX) {
                g_resp[g_resp_tail].fail = false;
                g_resp[g_resp_tail].status = arg ? atoi(arg) : 200;
                g_resp[g_resp_tail].pcm_samples = 0;
                char *out = g_resp[g_resp_tail].body;
                size_t cap = sizeof(g_resp[g_resp_tail].body);
                size_t at = 0;
                for (const char *s = body ? body : ""; *s && at + 1 < cap; s++) {
                    if (*s == '\\' && s[1] == 't') { out[at++] = '\t'; s++; }
                    else if (*s == '\\' && s[1] == 'n') { out[at++] = '\n'; s++; }
                    else { out[at++] = *s; }
                }
                out[at] = '\0';
                g_resp_tail++;
            }
        } else if (strcmp(line, "pageat") == 0) {
            // 再生位置 [ms] に出るページの番号を直に聞く (ms -> index の検査)。
            long ms = arg ? atol(arg) : 0;
            int i = stackee_talk_page_at(&g_talk, (uint32_t)ms);
            printf("PAGEAT %ld %d %s\n", ms, i,
                   (i >= 0) ? g_talk.pages[i].text : "-");
        } else if (strcmp(line, "band") == 0) {
            // ページ番号 -> 帯へ渡す行の集合 (頁めくりの検査)。
            int page = arg ? atoi(arg) : 0;
            char band[STACKEE_TALK_SUB_BAND_MAX];
            int lines = stackee_talk_band(&g_talk, page, band, sizeof(band));
            printf("BAND %d %d ", page, lines);
            if (band[0] == '\0') {
                printf("-");
            }
            for (const char *p = band; *p != '\0'; p++) {
                if (*p == '\n') { printf("\\n"); } else { putchar(*p); }
            }
            printf("\n");
        } else if (strcmp(line, "print") == 0) {
            printf("NOW %u STATE %s POLLS %d ALLOC %d RELEASE %d "
                   "PAGES %d PAGE %d DROPPED %d SUBBYTES %u SRC %s "
                   "REPLY %s ERROR %s\n",
                   (unsigned)g_now, stackee_talk_state_names[g_talk.state],
                   g_talk.polls, g_alloc_count, g_release_count,
                   g_talk.page_count, g_talk.page_shown, g_talk.sub_dropped,
                   (unsigned)g_talk.sub_bytes,
                   stackee_talk_sub_src_names[g_talk.sub_src],
                   g_talk.reply, g_talk.error);
        } else {
            fprintf(stderr, "unknown command: %s\n", line);
            return 2;
        }
    }
    return 0;
}
