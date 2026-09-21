#include "stackee_talksm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "stackee_jsonlite.h"

const char *const stackee_talk_state_names[STACKEE_TALK_STATES] = {
    "idle", "recording", "upload", "poll_wait", "poll", "subs", "audio",
    "play_wait", "playing",
};

const char *const stackee_talk_sub_src_names[STACKEE_TALK_SUB_SRCS] = {
    "none", "inline", "url",
};

static uint32_t now(const stackee_talk_t *t) {
    return t->ops->now_ms();
}

static uint32_t since_ms(const stackee_talk_t *t, uint32_t mark) {
    return (uint32_t)(now(t) - mark);
}

static void logf_(const stackee_talk_t *t, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logf_(const stackee_talk_t *t, const char *fmt, ...) {
    if (t->ops->log == NULL) {
        return;
    }
    char line[STACKEE_TALK_TEXT_MAX + 128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    t->ops->log(line);
}

static void show(stackee_talk_t *t, const char *text) {
    snprintf(t->screen, sizeof(t->screen), "%s", text);
    logf_(t, "[talk] %s", text);
    if (t->ops->show) {
        t->ops->show(text);
    }
}

// ---------------------------------------------------------------------------
// 字幕
// ---------------------------------------------------------------------------
// ★ ここに「音声のどこを喋っているか」の推定は置かない。区切りと開始時刻は
//   サーバが文ごとの合成 PCM の長さから厳密に出して渡してくる
//   (scratchpad/subtitle_design.md)。本体がするのは引き算と比較だけ。
static void subtitle(stackee_talk_t *t, const char *text) {
    if (t->ops->subtitle != NULL) {
        t->ops->subtitle(text);
    }
}

static void subtitle_clear(stackee_talk_t *t) {
    if (t->page_shown >= 0) {
        subtitle(t, NULL);
    }
    t->page_shown = -1;
    t->page_count = 0;
    t->sub_bytes = 0;
    t->sub_dropped = 0;
    t->sub_src = STACKEE_TALK_SUB_NONE;
}

int stackee_talk_page_at(const stackee_talk_t *t, uint32_t ms) {
    int found = -1;
    for (int i = 0; i < t->page_count; i++) {
        if (t->pages[i].start_ms > ms) {
            break;              // 開始時刻は単調非減少なので、ここから先は未来
        }
        found = i;
    }
    return found;
}

int stackee_talk_band(const stackee_talk_t *t, int page, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (page < 0 || page >= t->page_count) {
        return 0;
    }
    int first = page - (page % STACKEE_TALK_SUB_LINES);
    size_t at = 0;
    int lines = 0;
    for (int i = first; i <= page; i++) {
        const char *text = t->pages[i].text;
        size_t len = strlen(text);
        size_t need = len + ((lines > 0) ? 1u : 0u);
        if (at + need + 1 > cap) {
            break;              // 入らない行は置かない (途中で切らない)
        }
        if (lines > 0) {
            out[at++] = '\n';
        }
        memcpy(out + at, text, len);
        at += len;
        out[at] = '\0';
        lines++;
    }
    return lines;
}

// 1 行 "<start_ms> TAB <本文>" をページにする。採れたら true。
// ★ 行の出どころ (生の本文 / JSON の文字列) によらずここだけが形を知る。
static bool add_page(stackee_talk_t *t, const char *line, size_t len) {
    if (len > 0 && line[len - 1] == '\r') {
        len--;                              // CRLF でも読める
    }
    size_t p = 0;
    uint32_t ms = 0;
    int digits = 0;
    while (p < len && line[p] >= '0' && line[p] <= '9') {
        if (digits >= 9) {                  // 10^9 ms = 11 日。溢れさせない
            return false;
        }
        ms = ms * 10u + (uint32_t)(line[p] - '0');
        digits++;
        p++;
    }
    if (digits == 0 || p >= len || line[p] != '\t') {
        return false;
    }
    p++;                                    // タブを飛ばす
    size_t n = len - p;
    if (n == 0) {
        return false;                       // 本文が空の行は捨てる
    }
    if (n >= STACKEE_TALK_SUB_TEXT_MAX) {
        n = STACKEE_TALK_SUB_TEXT_MAX - 1;
        // UTF-8 の途中で切らない (切れた字は帯で 〓 になる)。
        while (n > 0 && ((unsigned char)line[p + n] & 0xC0u) == 0x80u) {
            n--;
        }
    }
    if (t->page_count >= STACKEE_TALK_SUB_PAGES) {
        return false;                       // 上限。残りは捨てる
    }
    // 開始時刻は単調非減少。逆行は前の時刻に揃える (page_at が前提にしている)。
    uint32_t last_ms = (t->page_count > 0)
                           ? t->pages[t->page_count - 1].start_ms : 0;
    if (ms < last_ms) {
        ms = last_ms;
    }
    stackee_talk_page_t *page = &t->pages[t->page_count];
    page->start_ms = ms;
    memcpy(page->text, line + p, n);
    page->text[n] = '\0';
    t->page_count++;
    return true;
}

static void subtitles_begin(stackee_talk_t *t) {
    t->page_count = 0;
    t->sub_dropped = 0;
    t->sub_bytes = 0;
}

int stackee_talk_parse_subtitles(stackee_talk_t *t, const char *text, size_t len) {
    subtitles_begin(t);
    if (text == NULL || len == 0) {
        return 0;
    }
    if (len > STACKEE_TALK_SUB_BYTES) {
        len = STACKEE_TALK_SUB_BYTES;       // 念のため (通信側でも切っている)
    }
    t->sub_bytes = (uint32_t)len;
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') {
            end++;
        }
        if (!add_page(t, text + at, end - at)) {
            t->sub_dropped++;
        }
        at = end + 1;
    }
    return t->page_count;
}

// ---------------------------------------------------------------------------
// done の JSON に混ざってきた本文 ("subtitles")
// ---------------------------------------------------------------------------
// ★ 4 KB の中継ぎを持たない。逃がしを解きながら 1 行ぶん
//   (STACKEE_TALK_SUB_LINE_MAX = 128 B、スタック) だけ組み立てて add_page へ渡す。
//   内蔵 RAM の静的な使用量は 1 バイトも増えない。
// ★ 逃がしは JSON の 2 文字のものだけを見る。\uXXXX は本文には出てこない
//   (サーバは ensure_ascii=False で、日本語は素の UTF-8 のまま) ので、
//   その並びは**そのまま本文の一部**として写す (読めない字は帯で 〓 になる)。
static int decode_escape(char c, char *out) {
    switch (c) {
        case 'n':  *out = '\n'; return 1;
        case 't':  *out = '\t'; return 1;
        case 'r':  *out = '\r'; return 1;
        case 'b':  *out = '\b'; return 1;
        case 'f':  *out = '\f'; return 1;
        case '"':  *out = '"';  return 1;
        case '\\': *out = '\\'; return 1;
        case '/':  *out = '/';  return 1;
        default:   return 0;
    }
}

int stackee_talk_parse_subtitles_json(stackee_talk_t *t, const char *at, size_t len) {
    subtitles_begin(t);
    if (at == NULL || len < 2 || at[0] != '"' || at[len - 1] != '"') {
        return 0;                           // 文字列でないなら字幕なし扱い
    }
    at++;                                   // 引用符を剥がす
    len -= 2;
    char line[STACKEE_TALK_SUB_LINE_MAX];
    size_t fill = 0;
    bool overflow = false;
    uint32_t decoded = 0;
    for (size_t i = 0; i < len; i++) {
        char c = at[i];
        if (c == '\\' && i + 1 < len) {
            char got = 0;
            if (decode_escape(at[i + 1], &got)) {
                i++;
                c = got;
            }
            // 知らない逃がし (\uXXXX など) は素通しで写す。
        }
        decoded++;
        if (decoded > STACKEE_TALK_SUB_BYTES) {
            break;                          // 念のため (契約は 4 KB 以下)
        }
        if (c == '\n') {
            if (overflow || !add_page(t, line, fill)) {
                t->sub_dropped++;
            }
            fill = 0;
            overflow = false;
            continue;
        }
        if (fill < sizeof(line)) {
            line[fill++] = c;
        } else {
            overflow = true;                // 長すぎる行。改行まで捨てる
        }
    }
    if (fill > 0 || overflow) {             // 末尾に改行が無い最後の行
        if (overflow || !add_page(t, line, fill)) {
            t->sub_dropped++;
        }
    }
    t->sub_bytes = decoded;
    return t->page_count;
}

static void face(stackee_talk_t *t) {
    if (t->ops->face == NULL) {
        return;
    }
    bool recording = (t->state == STACKEE_TALK_RECORDING);
    bool speaking = (t->state == STACKEE_TALK_PLAYING) ||
                    (t->ops->ack_active != NULL && t->ops->ack_active());
    bool busy = (t->state != STACKEE_TALK_IDLE) && !recording && !speaking;
    t->ops->face(recording, busy, speaking);
}

static void to(stackee_talk_t *t, int state) {
    t->state = state;
    t->since = now(t);
    face(t);
}

// 通信と録音の後始末。どんな終わり方でも必ずここを通す。
static void cleanup(stackee_talk_t *t) {
    if (t->http_open) {
        t->ops->http_close();
        t->http_open = false;
    }
    t->audio = NULL;
    t->audio_len = 0;
    // 終わり方によらず帯は消す (失敗・中断・鳴り終わり)。
    subtitle_clear(t);
    if (t->samples != NULL) {
        t->ops->record_release();
        t->samples = NULL;
    }
    t->count = 0;
}

static void fail(stackee_talk_t *t, const char *why) {
    snprintf(t->error, sizeof(t->error), "%s", why);
    t->errors++;
    // マイクの復帰は必ず試す (stackee_halfduplex の 7..9 段と同じ気持ち)。
    t->ops->record_end();
    cleanup(t);
    to(t, STACKEE_TALK_IDLE);
    char text[STACKEE_TALK_TEXT_MAX];
    snprintf(text, sizeof(text), "会話エラー: %s", why);
    show(t, text);
}

static bool valid_path(const char *path) {
    if (path == NULL || path[0] != '/' || path[1] == '/') {
        return false;
    }
    for (const char *p = path; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == ' ') {
            return false;
        }
    }
    return true;
}

void stackee_talk_init(stackee_talk_t *t, const stackee_talk_ops_t *ops,
                       const char *post_path) {
    memset(t, 0, sizeof(*t));
    t->ops = ops;
    snprintf(t->path, sizeof(t->path), "%s", post_path ? post_path : "/talk");
    t->page_shown = -1;
    t->state = STACKEE_TALK_IDLE;
    t->since = ops->now_ms();
    t->min_ms = STACKEE_TALK_MIN_MS_DEFAULT;
    t->voice_rms = STACKEE_TALK_VOICE_RMS_DEFAULT;
    t->voice_windows = STACKEE_TALK_VOICE_WINDOWS_DEFAULT;
}

void stackee_talk_set_pressed(stackee_talk_t *t, bool pressed) {
    t->pressed = pressed;
}

// ---------------------------------------------------------------------------
// 録音
// ---------------------------------------------------------------------------
static bool start_recording(stackee_talk_t *t) {
    // ★ 押下 → 最初のサンプルまでを測る起点。マイクを開ける前に取る。
    t->rec_request = now(t);
    t->first_sample_ms = 0;
    if (!valid_path(t->path)) {
        fail(t, "STACKEE_TALK_URL が未設定です");
        return false;
    }
    if (!t->ops->net_ready()) {
        fail(t, "Wi-Fi 未接続です");
        return false;
    }
    // ★ 先に半二重を取る (stackee_talk.py も hd.acquire が先)。あとにすると
    //   「鳴っている最中に 960 KB 確保してから断る」という無駄が出る。
    if (!t->ops->record_begin()) {
        fail(t, "マイクを使えません (再生中かマイクの初期化に失敗)");
        return false;
    }
    t->samples = t->ops->record_alloc(STACKEE_TALK_MAX_SAMPLES);
    if (t->samples == NULL) {
        fail(t, "録音の領域を確保できません");
        return false;
    }
    t->count = 0;
    t->error[0] = '\0';
    to(t, STACKEE_TALK_RECORDING);
    show(t, "録音中… 離すと送信");
    return true;
}

// 20 ms の窓ごとの RMS を数える。窓は重ねない。
// ★ 掛け算は 1 サンプルあたり 1 回。16 kHz x 30 秒で 48 万回 = audio タスクの
//   上で数 ms。1 往復に 1 度しか走らない。
void stackee_talk_voice_scan(const int16_t *pcm, int count,
                             uint32_t threshold, stackee_talk_voice_t *out) {
    memset(out, 0, sizeof(*out));
    out->max_at = -1;
    if (pcm == NULL || count < STACKEE_TALK_RMS_WINDOW) {
        return;                 // 窓 1 つに満たない = 測りようがない
    }
    uint64_t sum = 0;
    int windows = count / STACKEE_TALK_RMS_WINDOW;
    for (int w = 0; w < windows; w++) {
        const int16_t *at = pcm + (size_t)w * STACKEE_TALK_RMS_WINDOW;
        uint64_t square = 0;
        for (int i = 0; i < STACKEE_TALK_RMS_WINDOW; i++) {
            square += (uint64_t)((int32_t)at[i] * at[i]);
        }
        uint32_t mean = (uint32_t)(square / STACKEE_TALK_RMS_WINDOW);
        // 整数平方根 (浮動小数を使わない)。
        uint32_t root = 0;
        while ((uint64_t)(root + 1) * (root + 1) <= mean) {
            root++;
        }
        sum += root;
        if (threshold > 0 && root >= threshold) {
            out->loud++;
        }
        if (root > out->max) {
            out->second = out->max;
            out->max = root;
            out->max_at = w;
        } else if (root > out->second) {
            out->second = root;
        }
    }
    out->windows = windows;
    out->mean = (uint32_t)(sum / (uint64_t)windows);
}

void stackee_talk_set_gate(stackee_talk_t *t, uint32_t min_ms,
                           uint32_t voice_rms, uint32_t voice_windows) {
    t->min_ms = min_ms;
    t->voice_rms = voice_rms;
    t->voice_windows = voice_windows;
}

// 録音を捨てるか決める。捨てるなら理由を返す (NULL = 進んでよい)。
// ★ ここで数えた長さと RMS は talk.status に出る (閾値を決める材料)。
static const char *gate_recording(stackee_talk_t *t) {
    t->last_rec_ms = (uint32_t)((int64_t)t->count * 1000 / STACKEE_TALK_RATE);
    stackee_talk_voice_t voice;
    stackee_talk_voice_scan(t->samples, t->count, t->voice_rms, &voice);
    t->last_rms_max = voice.max;
    t->last_rms_at = voice.max_at;
    t->last_rms_mean = voice.mean;
    t->last_rms_2nd = voice.second;
    t->last_loud = voice.loud;
    // サーバが 0.3 秒未満を断るので、設定によらずそこは必ず切る。
    if (t->count < STACKEE_TALK_MIN_SAMPLES ||
        (t->min_ms > 0 && t->last_rec_ms < t->min_ms)) {
        t->dropped_short++;
        return "短すぎ";
    }
    // ★ 「いちばん大きい窓」では判定しない。マイクを開けた直後の跳ねが
    //   毎回 3,000 を超えるので、最大だけ見ると必ず声ありになってしまう
    //   (stackee_talksm.h の ★★)。越えた**窓の数**で見る。
    if (t->voice_rms > 0 && t->voice_windows > 0 &&
        t->last_loud < t->voice_windows) {
        t->dropped_silent++;
        return "無音";
    }
    return NULL;
}

static void finish_recording(stackee_talk_t *t) {
    t->turn_started = now(t);
    t->turn_valid = true;
    t->accepted_ms = t->reply_ready_ms = t->audio_ready_ms = 0;
    t->play_setup_ms = t->complete_ms = 0;
    t->polls = 0;
    t->reply[0] = '\0';
    t->ops->record_end();

    // ★ 誤って触れただけなら**何も起こさない**。一次回答も鳴らさず、
    //   送信もせず、「考え中」の顔にもならずに idle へ戻る。
    const char *why = gate_recording(t);
    if (why != NULL) {
        logf_(t, "[talk] %sのため破棄 (len_ms=%lu, loud=%lu, rms_max=%lu, "
                 "rms_at=%d, rms_2nd=%lu, rms_mean=%lu, "
                 "min_ms=%lu, voice_rms=%lu, voice_windows=%lu)",
              why, (unsigned long)t->last_rec_ms, (unsigned long)t->last_loud,
              (unsigned long)t->last_rms_max, t->last_rms_at,
              (unsigned long)t->last_rms_2nd, (unsigned long)t->last_rms_mean,
              (unsigned long)t->min_ms, (unsigned long)t->voice_rms,
              (unsigned long)t->voice_windows);
        cleanup(t);
        to(t, STACKEE_TALK_IDLE);       // listening → idle (thinking を通らない)
        show(t, "送信しませんでした");
        return;
    }
    t->ops->wav_header(t->samples, t->count);
    const uint8_t *body = (const uint8_t *)t->samples - 44;
    size_t body_len = 44 + (size_t)t->count * 2;
    if (!t->ops->http_start("POST", t->path, body, body_len, 8192)) {
        fail(t, "送信を始められません");
        return;
    }
    t->http_open = true;
    to(t, STACKEE_TALK_UPLOAD);
    show(t, "音声を送信中…");
    // 一次回答は返事を待つ前に鳴らし始める。その間も通信は進む。
    t->ops->ack_begin();
    face(t);
}

// ---------------------------------------------------------------------------
// 応答の処理
// ---------------------------------------------------------------------------
// 応答を読み終えたら通信を畳む。★ ここより前に body を使い切っておくこと
// (body は通信側のバッファを指しているので、閉じると無効になる)。
static void close_http(stackee_talk_t *t) {
    if (t->http_open) {
        t->ops->http_close();
        t->http_open = false;
    }
}

static void handle_upload_done(stackee_talk_t *t, const char *json) {
    char job[STACKEE_TALK_PATH_MAX];
    if (!stackee_json_str(json, "status_url", job, sizeof(job)) || !valid_path(job)) {
        fail(t, "受理応答が不正です");
        return;
    }
    snprintf(t->job, sizeof(t->job), "%s", job);
    close_http(t);
    // ★ 390 秒の起点は「受理されたとき」。送信にかかった時間を食わない
    //   (stackee_talk.py が handle_upload_done で self.since を置き直すのと同じ)。
    t->poll_started = now(t);
    // 送り終わったので録音の領域 (最大 960 KB) は返す。返事を待つ間ずっと
    // 抱えていると PSRAM が 1 MB 減ったままになる。
    if (t->samples != NULL) {
        t->ops->record_release();
        t->samples = NULL;
        t->count = 0;
    }
    t->accepted_ms = since_ms(t, t->turn_started);
    t->polled = now(t);
    t->poll_sent = t->polled;
    t->poll_took = 0;           // 最初の 1 回は従来どおり 1 秒あけてから
    to(t, STACKEE_TALK_POLL_WAIT);
    show(t, "Codex が応答中…");
}

// 返答 PCM を取りに行く。★ 受け皿は録音バッファではなく通信側の受信バッファ。
//   ここで頼む長さがそのまま PSRAM の確保量になる (120 秒 = 3.84 MB)。
//   録音の 960 KB は受理の時点で返してあるので同時には持たない。
static void start_audio(stackee_talk_t *t) {
    if (!t->ops->http_start("GET", t->audio_path, NULL, 0,
                            STACKEE_TALK_REPLY_MAX_BYTES)) {
        // ここで断られるのはほぼ PSRAM 不足 (3.84 MB が取れない)。
        // 会話だけを失敗させ、キーボードには触らない。
        fail(t, "返答の受け皿を確保できません (メモリ不足)");
        return;
    }
    t->http_open = true;
    to(t, STACKEE_TALK_AUDIO);
}

static void handle_poll_done(stackee_talk_t *t, const char *json) {
    char state[24];
    if (!stackee_json_str(json, "state", state, sizeof(state))) {
        fail(t, "応答の state を読めません");
        return;
    }
    // json はこの関数を抜けるまでだけ有効 (close_http で無効になる)。
    if (strcmp(state, "done") == 0) {
        long rate = 0, channels = 0, width = 0;
        if (!stackee_json_int(json, "sample_rate", &rate) ||
            !stackee_json_int(json, "channels", &channels) ||
            !stackee_json_int(json, "sample_width", &width) ||
            rate != STACKEE_TALK_RATE || channels != 1 || width != 2) {
            fail(t, "返答の音声形式が違います");
            return;
        }
        char audio[STACKEE_TALK_PATH_MAX];
        if (!stackee_json_str(json, "audio_url", audio, sizeof(audio)) ||
            !valid_path(audio)) {
            fail(t, "返答の audio_url が不正です");
            return;
        }
        // ★ 字幕は「本文が混ざっていればそれを使う。無ければ取りに行く」。
        //   旧サーバにはどちらも無いので、そのときは今までと 1 手も変わらない。
        t->subs_path[0] = '\0';
        char subs[STACKEE_TALK_PATH_MAX];
        if (stackee_json_str(json, "subtitles_url", subs, sizeof(subs)) &&
            valid_path(subs)) {
            snprintf(t->subs_path, sizeof(t->subs_path), "%s", subs);
        }
        subtitle_clear(t);      // 前の往復の名残を捨てる (帯は既に消えている)
        // ★ done の JSON に混ざってきた本文。別 GET は実機で約 8 秒かかる
        //   (要求ごとに TLS を張り直す) ので、あるならそれを使うほうが
        //   喋り始めがその 8 秒ぶん早い。json は close_http まで有効。
        const char *inline_at = NULL;
        size_t inline_len = 0;
        if (stackee_json_raw(json, "subtitles", &inline_at, &inline_len) &&
            inline_len >= 2 && inline_at[0] == '"') {
            int pages = stackee_talk_parse_subtitles_json(t, inline_at, inline_len);
            if (pages > 0) {
                t->sub_src = STACKEE_TALK_SUB_INLINE;
                t->subs_ok++;
            } else {
                t->subs_failed++;
            }
            logf_(t, "[talk-subtitles] {\"src\":\"inline\",\"pages\":%d,"
                     "\"bytes\":%lu,\"dropped\":%d}",
                  pages, (unsigned long)t->sub_bytes, t->sub_dropped);
        }
        stackee_json_str(json, "reply", t->reply, sizeof(t->reply));
        t->reply_ready_ms = since_ms(t, t->turn_started);
        snprintf(t->audio_path, sizeof(t->audio_path), "%s", audio);
        close_http(t);
        show(t, t->reply);
        // 本文が混ざっていなかった (か、1 ページも採れなかった) ときだけ
        // 従来どおり取りに行く。
        if (t->page_count == 0 && t->subs_path[0] != '\0' &&
            t->ops->http_start("GET", t->subs_path, NULL, 0,
                               STACKEE_TALK_SUB_BYTES)) {
            t->http_open = true;
            to(t, STACKEE_TALK_SUBS);
            return;
        }
        start_audio(t);
        return;
    }
    if (strcmp(state, "error") == 0 || strcmp(state, "ignored") == 0) {
        char message[STACKEE_TALK_TEXT_MAX];
        if (!stackee_json_str(json, "error", message, sizeof(message))) {
            snprintf(message, sizeof(message), "音声を認識できませんでした");
        }
        t->ignored++;
        close_http(t);
        cleanup(t);
        to(t, STACKEE_TALK_IDLE);
        show(t, message);
        return;
    }
    close_http(t);
    t->polled = now(t);
    // ★ その 1 往復にかかった時間を覚えておく。ロングポーリングが効いて
    //   いれば 1 秒以上かかるので、次はすぐ投げる (待ちを二重にしない)。
    //   中継が古くて即返る場合は従来どおり 1 秒あける。
    t->poll_took = since_ms(t, t->poll_sent);
    to(t, STACKEE_TALK_POLL_WAIT);
}

// ---------------------------------------------------------------------------
// 1 周
// ---------------------------------------------------------------------------
static void step_http(stackee_talk_t *t) {
    int status = 0;
    const uint8_t *body = NULL;
    size_t len = 0;
    int got = t->ops->http_poll(&status, &body, &len);
    if (got == 0) {
        return;
    }
    // ★ 字幕の失敗で会話を止めない。取れなければ字幕なしで音声へ進む。
    if (t->state == STACKEE_TALK_SUBS) {
        if (got > 0 && status == 200 && body != NULL && len > 0) {
            int pages = stackee_talk_parse_subtitles(t, (const char *)body, len);
            if (pages > 0) {
                t->sub_src = STACKEE_TALK_SUB_URL;
                t->subs_ok++;
            } else {
                t->subs_failed++;
            }
            logf_(t, "[talk-subtitles] {\"src\":\"url\",\"pages\":%d,"
                     "\"bytes\":%u,\"dropped\":%d}",
                  pages, (unsigned)len, t->sub_dropped);
        } else {
            t->page_count = 0;
            t->subs_failed++;
            logf_(t, "[talk-subtitles] {\"src\":\"url\",\"pages\":0,"
                     "\"status\":%d,\"got\":%d}", status, got);
        }
        close_http(t);
        start_audio(t);
        return;
    }
    if (got < 0) {
        fail(t, "通信に失敗しました");
        return;
    }
    if (status != 200 && status != 202) {
        char why[64];
        snprintf(why, sizeof(why), "サーバー HTTP %d", status);
        fail(t, why);
        return;
    }
    if (t->state == STACKEE_TALK_AUDIO) {
        if (body == NULL || len == 0 || (len % 2) != 0) {
            fail(t, "返答の PCM が不正です");
            return;
        }
        // ★ ここでは http_close() を**呼ばない**。返答 PCM は通信側の
        //   バッファをそのまま鳴らすので、再生が終わるまで生かしておく。
        t->audio = body;
        t->audio_len = len;
        t->audio_samples = (int)(len / 2);
        // 120 秒 = 1,920,000 サンプル。int で掛けると 1.92e9 で上限すれすれ
        // なので 64 ビットで割る。
        t->audio_duration_ms =
            (int)((int64_t)t->audio_samples * 1000 / STACKEE_TALK_RATE);
        t->audio_ready_ms = since_ms(t, t->turn_started);
        to(t, STACKEE_TALK_PLAY_WAIT);
        return;
    }
    // JSON の応答。NUL 終端は通信側が保証する (body[len] == 0)。
    // ★ コピーしない。返答文は最大 120 文字 + transcript + timings で数 KB に
    //   なりうるので、スタックへ写すと足りなくなる。読み終えたところで
    //   close_http() を呼ぶ規則にしてある。
    const char *json = (body == NULL) ? "{}" : (const char *)body;
    if (t->state == STACKEE_TALK_UPLOAD) {
        handle_upload_done(t, json);
    } else {
        handle_poll_done(t, json);
    }
}

void stackee_talk_step(stackee_talk_t *t) {
    bool rising = t->pressed && !t->was_pressed;
    t->was_pressed = t->pressed;
    // ★ 毎周知らせる。一次回答が鳴り終わるのは「状態が変わらない変化」なので、
    //   遷移のときだけ知らせると顔が speaking のまま取り残される。
    face(t);

    switch (t->state) {
        case STACKEE_TALK_IDLE:
            if (rising && !t->ops->ack_active()) {
                start_recording(t);
            }
            return;

        case STACKEE_TALK_RECORDING: {
            if (!t->pressed || t->count >= STACKEE_TALK_MAX_SAMPLES ||
                since_ms(t, t->since) >= STACKEE_TALK_RECORD_MAX_MS) {
                finish_recording(t);
                return;
            }
            int want = STACKEE_TALK_CHUNK;
            if (t->count + want > STACKEE_TALK_MAX_SAMPLES) {
                want = STACKEE_TALK_MAX_SAMPLES - t->count;
            }
            int got = t->ops->record_read(t->samples + t->count, want);
            if (got < 0) {
                fail(t, "マイクから音声を取得できません");
                return;
            }
            if (got > 0 && t->first_sample_ms == 0) {
                t->first_sample_ms = since_ms(t, t->rec_request);
                if (t->first_sample_ms == 0) {
                    t->first_sample_ms = 1;     // 0 は「まだ」の意味に使う
                }
            }
            t->count += got;
            return;
        }

        case STACKEE_TALK_POLL_WAIT:
            if (since_ms(t, t->poll_started) > STACKEE_TALK_POLL_TIMEOUT_MS) {
                fail(t, "応答待ちがタイムアウトしました");
                return;
            }
            {
                uint32_t gap = (t->poll_took >= STACKEE_TALK_POLL_MS)
                                   ? 0 : STACKEE_TALK_POLL_MS;
                if (since_ms(t, t->polled) < gap) {
                    return;
                }
                // ★ 返答待ちの GET にだけ "?wait=" を付ける。中継がこの
                //   秒数まで握ってくれるので、TLS の握手が要求ごとに
                //   起きなくなる。POST と /audio は従来どおり。
                char path[STACKEE_TALK_PATH_MAX + 16];
                if (STACKEE_TALK_POLL_WAIT_S > 0) {
                    snprintf(path, sizeof(path), "%s?wait=%d", t->job,
                             STACKEE_TALK_POLL_WAIT_S);
                } else {
                    snprintf(path, sizeof(path), "%s", t->job);
                }
                t->poll_sent = now(t);
                // ★ done には字幕の本文 (4 KB) が混ざる。8 KB では足りない。
                if (!t->ops->http_start("GET", path, NULL, 0,
                                        STACKEE_TALK_POLL_LIMIT)) {
                    fail(t, "状態の取得を始められません");
                    return;
                }
                t->http_open = true;
                t->polls++;
                to(t, STACKEE_TALK_POLL);
            }
            return;

        case STACKEE_TALK_PLAY_WAIT:
            // ★ 最終回答は一次回答の**あと**。ここが両者の排他。
            if (t->ops->ack_active()) {
                return;
            }
            if (t->audio == NULL || t->audio_samples <= 0) {
                fail(t, "返答の PCM が無い");
                return;
            }
            if (t->ops->play_begin((const int16_t *)t->audio, t->audio_samples)) {
                t->play_setup_ms = since_ms(t, t->turn_started);
                to(t, STACKEE_TALK_PLAYING);
                return;
            }
            if (since_ms(t, t->since) > STACKEE_TALK_PLAY_WAIT_MS) {
                fail(t, "スピーカー待機がタイムアウトしました");
            }
            return;

        case STACKEE_TALK_PLAYING:
            if (t->ops->play_active()) {
                // ★ 再生位置は「鳴らし始めてからの経過」。DMA へ**書いた**
                //   サンプル数ではない (先読みで最大 192 ms 進んでいて、
                //   字幕だけが音より先に出てしまう)。playing に入った時刻が
                //   そのまま play_begin の時刻なので、引き算だけで出る。
                if (t->page_count > 0) {
                    int want = stackee_talk_page_at(t, since_ms(t, t->since));
                    if (want != t->page_shown) {
                        t->page_shown = want;
                        if (want < 0) {
                            subtitle(t, NULL);
                        } else {
                            // ★ 帯には「その頁のここまで」を積んで渡す。
                            //   3 行が埋まった次のページで頁がめくれる
                            //   (band が 1 行だけを返すのがその印)。
                            char band[STACKEE_TALK_SUB_BAND_MAX];
                            stackee_talk_band(t, want, band, sizeof(band));
                            subtitle(t, band);
                        }
                    }
                }
                return;
            }
            t->complete_ms = since_ms(t, t->turn_started);
            logf_(t, "[talk-turn-timing] {\"accepted_ms\":%lu,\"reply_ready_ms\":%lu,"
                     "\"audio_ready_ms\":%lu,\"play_setup_ms\":%lu,"
                     "\"audio_duration_ms\":%d,\"polls\":%d,\"complete_ms\":%lu,"
                     "\"sub_pages\":%d,\"sub_src\":\"%s\","
                     "\"first_sample_ms\":%lu,\"rec_ms\":%lu}",
                  (unsigned long)t->accepted_ms, (unsigned long)t->reply_ready_ms,
                  (unsigned long)t->audio_ready_ms, (unsigned long)t->play_setup_ms,
                  t->audio_duration_ms, t->polls, (unsigned long)t->complete_ms,
                  t->page_count, stackee_talk_sub_src_names[t->sub_src],
                  (unsigned long)t->first_sample_ms,
                  (unsigned long)t->last_rec_ms);
            t->turns++;
            cleanup(t);
            to(t, STACKEE_TALK_IDLE);
            if (t->ops->play_failed()) {
                fail(t, "音声再生またはマイク復帰に失敗しました");
            }
            return;

        case STACKEE_TALK_UPLOAD:
        case STACKEE_TALK_POLL:
        case STACKEE_TALK_SUBS:
        case STACKEE_TALK_AUDIO:
            step_http(t);
            return;

        default:
            return;
    }
}

bool stackee_talk_busy(const stackee_talk_t *t) {
    return t->state != STACKEE_TALK_IDLE;
}

bool stackee_talk_inject(stackee_talk_t *t, const int16_t *pcm, int samples) {
    if (t->state != STACKEE_TALK_IDLE || t->ops->ack_active()) {
        return false;
    }
    if (pcm == NULL || samples < STACKEE_TALK_MIN_SAMPLES ||
        samples > STACKEE_TALK_MAX_SAMPLES) {
        return false;
    }
    if (!valid_path(t->path)) {
        fail(t, "STACKEE_TALK_URL が未設定です");
        return false;
    }
    if (!t->ops->net_ready()) {
        fail(t, "Wi-Fi 未接続です");
        return false;
    }
    t->samples = t->ops->record_alloc(STACKEE_TALK_MAX_SAMPLES);
    if (t->samples == NULL) {
        fail(t, "録音の領域を確保できません");
        return false;
    }
    memcpy(t->samples, pcm, (size_t)samples * 2);
    t->count = samples;
    t->error[0] = '\0';
    // ★ 録音の道は通らないので record_begin / record_end は呼ばない。
    //   送信から先は普段の会話とまったく同じ道を歩く。
    t->turn_started = now(t);
    t->turn_valid = true;
    t->accepted_ms = t->reply_ready_ms = t->audio_ready_ms = 0;
    t->play_setup_ms = t->complete_ms = 0;
    t->polls = 0;
    t->reply[0] = '\0';
    t->ops->wav_header(t->samples, t->count);
    const uint8_t *body = (const uint8_t *)t->samples - 44;
    size_t body_len = 44 + (size_t)t->count * 2;
    if (!t->ops->http_start("POST", t->path, body, body_len, 8192)) {
        fail(t, "送信を始められません");
        return false;
    }
    t->http_open = true;
    to(t, STACKEE_TALK_UPLOAD);
    show(t, "音声を送信中…");
    t->ops->ack_begin();
    face(t);
    return true;
}

// ---------------------------------------------------------------------------
// URL を割る (stackee_talk.py の parse_url と同じ検査)
// ---------------------------------------------------------------------------
bool stackee_talk_split_url(const char *url, char *base, size_t bcap,
                            char *path, size_t pcap) {
    if (url == NULL || base == NULL || path == NULL) {
        return false;
    }
    size_t scheme = 0;
    if (strncmp(url, "https://", 8) == 0) {
        scheme = 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        scheme = 7;
    } else {
        return false;
    }
    for (const char *p = url; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == ' ' || *p == '@' || *p == '#') {
            return false;
        }
    }
    const char *authority = url + scheme;
    const char *slash = strchr(authority, '/');
    size_t alen = (slash == NULL) ? strlen(authority) : (size_t)(slash - authority);
    if (alen == 0 || scheme + alen + 1 > bcap) {
        return false;
    }
    // ホストとポートを検査する (":" は 1 つまで、ポートは 1..65535)。
    const char *colon = memchr(authority, ':', alen);
    if (colon != NULL) {
        if (colon == authority) {
            return false;                   // ホストが空
        }
        if (memchr(colon + 1, ':', alen - (size_t)(colon + 1 - authority)) != NULL) {
            return false;                   // ":" が 2 つ以上
        }
        long port = 0;
        for (const char *p = colon + 1; p < authority + alen; p++) {
            if (*p < '0' || *p > '9') {
                return false;
            }
            port = port * 10 + (*p - '0');
            if (port > 65535) {
                return false;
            }
        }
        if (port < 1) {
            return false;
        }
    }
    memcpy(base, url, scheme + alen);
    base[scheme + alen] = '\0';
    if (slash == NULL) {
        snprintf(path, pcap, "/");
    } else {
        if (strlen(slash) + 1 > pcap) {
            return false;
        }
        snprintf(path, pcap, "%s", slash);
    }
    return valid_path(path) || strcmp(path, "/") == 0;
}
