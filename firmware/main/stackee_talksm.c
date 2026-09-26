#include "stackee_talksm.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "stackee_jsonlite.h"

const char *const stackee_talk_state_names[STACKEE_TALK_STATES] = {
    "idle", "recording", "upload", "poll_wait", "poll", "subs", "audio",
    "play_wait", "playing",
    "inbox_seq", "key", "inbox_wait", "inbox", "say_text",
};

const char *const stackee_talk_cstm_mode_names[STACKEE_TALK_CSTM_MODES] = {
    "", "prompt", "command",
};

const char *const stackee_talk_watch_phase_names[STACKEE_TALK_WATCH_PHASES] = {
    "off", "wait", "seq", "poll", "say", "sleep",
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

static void watch_backoff(stackee_talk_t *t, const char *why);

static void fail(stackee_talk_t *t, const char *why) {
    if (t->watch_say) {
        // ★ 常時ポーリングで受けた発話の失敗。ユーザーが頼んだものではない
        //   ので「会話エラー」は出さず、ログと inbox.status にだけ残して
        //   間合いをあける。マイクは開けていないので record_end も呼ばない
        //   (呼ぶと USB マイクが使っている I2S を畳んでしまう)。
        logf_(t, "[inbox] 発話を扱えませんでした: %s", why);
        cleanup(t);
        t->watch_say = false;
        to(t, STACKEE_TALK_IDLE);
        watch_backoff(t, why);
        return;
    }
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
    if (!stackee_talk_look_path(t->path, t->look_path, sizeof(t->look_path))) {
        t->look_path[0] = '\0';    // 画像は送れない (reserve が理由を出す)
    }
    // CSTM の送り先も同じ規則 (作れなければ押下のときに理由を出す)。
    if (!stackee_talk_sibling_path(t->path, "key", t->key_path,
                                   sizeof(t->key_path)) ||
        !stackee_talk_sibling_path(t->path, "inbox", t->inbox_path,
                                   sizeof(t->inbox_path))) {
        t->key_path[0] = '\0';
        t->inbox_path[0] = '\0';
    }
    t->say_cur = -1;
    t->page_shown = -1;
    // 常時ポーリングは既定で止めておく (立てるのは audio。hostbuild の
    // 台本は `watch 1` を書いたときだけ受け箱を回す)。
    t->watch_on = false;
    t->watch_play = true;
    t->watch_phase = STACKEE_TALK_WATCH_OFF;
    t->state = STACKEE_TALK_IDLE;
    t->since = ops->now_ms();
    t->guide_rec = STACKEE_TALK_GUIDE_RECORDING;
    t->guide_think = STACKEE_TALK_GUIDE_THINKING;
    t->guide_shown = STACKEE_TALK_GUIDE_NONE;
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
static void watch_abort(stackee_talk_t *t, bool count);

// 接続が無ければ先に張る (返事は捨てる短い GET)。受け箱の URL が作れる
// ときだけ (中継が古くても 404 が返るだけで、TLS は張れる)。
static void prewarm(stackee_talk_t *t) {
    if (t->ops->http_prewarm == NULL || t->inbox_path[0] == '\0') {
        return;
    }
    char path[STACKEE_TALK_PATH_MAX + 24];
    if (t->inbox_seq_valid) {
        snprintf(path, sizeof(path), "%s?after=%lu", t->inbox_path,
                 (unsigned long)t->inbox_seq);
    } else {
        snprintf(path, sizeof(path), "%s", t->inbox_path);
    }
    t->ops->http_prewarm(path);
}

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
    // ★ 録音を始めてから (押下 → 録音開始を 1 µs も遅らせない)、受け箱の
    //   待ちを打ち切り、録音の数秒のあいだに接続を張っておく。離したあとの
    //   POST が TLS の握手 (実機で約 4.5 秒) を待たずに済む。
    watch_abort(t, true);
    prewarm(t);
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

// 1 往復の始まり。数字は前の往復の名残を消してから数え直す
// (talk.status / camera.look_status が「いまの往復」を読めるように)。
static void begin_turn(stackee_talk_t *t, bool look) {
    t->turn_started = now(t);
    t->turn_valid = true;
    t->accepted_ms = t->reply_ready_ms = t->audio_ready_ms = 0;
    t->play_setup_ms = t->complete_ms = 0;
    t->polls = 0;
    t->reply[0] = '\0';
    t->reply_len = 0;
    t->job[0] = '\0';
    t->audio_samples = 0;
    t->audio_duration_ms = 0;
    t->look = look;
    t->cstm = false;            // CSTM は stackee_talk_cstm() が立て直す
}

static void finish_recording(stackee_talk_t *t) {
    begin_turn(t, false);
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
    if (!t->ops->http_start("POST", t->path, body, body_len, 8192,
                            STACKEE_TALK_CTYPE_WAV)) {
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
                            STACKEE_TALK_REPLY_MAX_BYTES, NULL)) {
        // ここで断られるのはほぼ PSRAM 不足 (3.84 MB が取れない)。
        // 会話だけを失敗させ、キーボードには触らない。
        fail(t, "返答の受け皿を確保できません (メモリ不足)");
        return;
    }
    t->http_open = true;
    to(t, STACKEE_TALK_AUDIO);
}

// done の JSON (と受け箱の発話 "say") から返答を読み取る。
// 戻り値: 1 = audio_url があり audio_path に入れた / 0 = 音が無い (字幕だけ) /
//         -1 = 失敗 (fail 済み)。
// ★ close_http はしない (json は呼び手が閉じるまで有効)。
// ★ need_audio (会話の done) では、音声形式 → audio_url の順に確かめる
//   (受け箱の発話で分けるまで 1 か所に書いてあった手順そのまま)。
static int take_reply(stackee_talk_t *t, const char *json, bool need_audio) {
    char audio[STACKEE_TALK_PATH_MAX];
    bool has_audio = stackee_json_str(json, "audio_url", audio, sizeof(audio));
    if (need_audio || has_audio) {
        long rate = 0, channels = 0, width = 0;
        if (!stackee_json_int(json, "sample_rate", &rate) ||
            !stackee_json_int(json, "channels", &channels) ||
            !stackee_json_int(json, "sample_width", &width) ||
            rate != STACKEE_TALK_RATE || channels != 1 || width != 2) {
            fail(t, "返答の音声形式が違います");
            return -1;
        }
        if (!has_audio || !valid_path(audio)) {
            fail(t, "返答の audio_url が不正です");
            return -1;
        }
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
    t->reply[0] = '\0';
    stackee_json_str(json, "reply", t->reply, sizeof(t->reply));
    t->reply_len = (int)strlen(t->reply);
    t->reply_ready_ms = since_ms(t, t->turn_started);
    if (!has_audio) {
        return 0;
    }
    snprintf(t->audio_path, sizeof(t->audio_path), "%s", audio);
    return 1;
}

static void handle_poll_done(stackee_talk_t *t, const char *json) {
    char state[24];
    if (!stackee_json_str(json, "state", state, sizeof(state))) {
        fail(t, "応答の state を読めません");
        return;
    }
    // json はこの関数を抜けるまでだけ有効 (close_http で無効になる)。
    if (strcmp(state, "done") == 0) {
        if (take_reply(t, json, true) < 0) {
            return;
        }
        close_http(t);
        show(t, t->reply);
        // 本文が混ざっていなかった (か、1 ページも採れなかった) ときだけ
        // 従来どおり取りに行く。
        if (t->page_count == 0 && t->subs_path[0] != '\0' &&
            t->ops->http_start("GET", t->subs_path, NULL, 0,
                               STACKEE_TALK_SUB_BYTES, NULL)) {
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
        // CSTM の prompt 方式なら、流れとしては失敗 (帯にも短く出す)。
        if (t->cstm_active) {
            snprintf(t->error, sizeof(t->error), "%s", message);
            t->cstm_final = "error";
        }
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
// 帯の文字 (字幕の頁とお知らせ)
// ---------------------------------------------------------------------------
// UTF-8 の 1 字の長さ (先頭バイトから)。壊れた並びは 1 バイトずつ進める。
static size_t utf8_len(const unsigned char *p) {
    size_t n = 1;
    if (p[0] >= 0xF0 && p[0] <= 0xF7) {
        n = 4;
    } else if (p[0] >= 0xE0) {
        n = 3;
    } else if (p[0] >= 0xC0) {
        n = 2;
    }
    for (size_t i = 1; i < n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            return 1;           // 途中で切れている / 壊れている
        }
    }
    return n;
}

// *pp から 1 行 (cols 字まで、改行で切る) を dst に写す。写したバイト数。
// ★ 字の途中では切らない。dst に入らない字は次の行へ回す。
static size_t take_line(const char **pp, int cols, char *dst, size_t cap) {
    const unsigned char *p = (const unsigned char *)*pp;
    size_t fill = 0;
    int used = 0;
    while (*p != '\0' && *p != '\n' && used < cols) {
        size_t n = utf8_len(p);
        if (fill + n + 1 > cap) {
            break;
        }
        memcpy(dst + fill, p, n);
        fill += n;
        p += n;
        used++;
    }
    if (*p == '\n') {
        p++;
    }
    dst[fill] = '\0';
    *pp = (const char *)p;
    return fill;
}

int stackee_talk_wrap(const char *text, int cols, int lines, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    if (text == NULL || cols <= 0) {
        return 0;
    }
    size_t at = 0;
    int got = 0;
    const char *p = text;
    while (*p != '\0' && got < lines) {
        char line[STACKEE_TALK_SUB_TEXT_MAX];
        size_t n = take_line(&p, cols, line, sizeof(line));
        if (n == 0) {
            continue;           // 空の行は置かない
        }
        size_t need = n + ((got > 0) ? 1u : 0u);
        if (at + need + 1 > cap) {
            break;
        }
        if (got > 0) {
            out[at++] = '\n';
        }
        memcpy(out + at, line, n + 1);
        at += n;
        got++;
    }
    return got;
}

// 帯に短く出すお知らせ。★ idle のときにだけ置く (帯の持ち主が居ない)。
// 消すのは stackee_talk_step の notice_step。
static void notice(stackee_talk_t *t, const char *text) {
    stackee_talk_wrap(text, STACKEE_TALK_BAND_COLS, STACKEE_TALK_SUB_LINES,
                      t->notice, sizeof(t->notice));
    if (t->notice[0] == '\0') {
        return;
    }
    t->notice_on = true;
    t->notice_since = now(t);
    subtitle(t, t->notice);
}

static void notice_step(stackee_talk_t *t) {
    if (!t->notice_on) {
        return;
    }
    if (t->state != STACKEE_TALK_IDLE) {
        t->notice_on = false;   // 次の往復が帯を持った (案内が上書きしている)
        return;
    }
    if (since_ms(t, t->notice_since) < STACKEE_TALK_NOTICE_MS) {
        return;
    }
    t->notice_on = false;
    // ★ そのあいだに別の音 (audio.play の字幕) が帯を持ったなら触らない。
    if (t->ops->ack_active() || t->ops->play_active()) {
        return;
    }
    subtitle(t, NULL);
}

// 返答文を 15 字ずつの頁にする (字幕の本文が無い、音の無い発話のため)。
// 3 行 (= 帯 1 枚) ごとに STACKEE_TALK_SAY_PAGE_MS ずつずらす。
static void pages_from_text(stackee_talk_t *t, const char *text) {
    subtitles_begin(t);
    const char *p = text;
    while (*p != '\0' && t->page_count < STACKEE_TALK_SUB_PAGES) {
        stackee_talk_page_t *page = &t->pages[t->page_count];
        if (take_line(&p, STACKEE_TALK_BAND_COLS, page->text,
                      sizeof(page->text)) == 0) {
            continue;
        }
        page->start_ms = (uint32_t)(t->page_count / STACKEE_TALK_SUB_LINES) *
                         STACKEE_TALK_SAY_PAGE_MS;
        t->page_count++;
    }
    t->sub_bytes = (uint32_t)strlen(text);
}

// 再生位置 ms の頁を帯に出す (変わったときだけ)。返答の再生と字幕だけの
// 発話の両方がここを通る。
static void show_page_at(stackee_talk_t *t, uint32_t ms) {
    if (t->page_count <= 0) {
        return;
    }
    int want = stackee_talk_page_at(t, ms);
    if (want == t->page_shown) {
        return;
    }
    t->page_shown = want;
    if (want < 0) {
        subtitle(t, NULL);
        return;
    }
    // ★ 帯には「その頁のここまで」を積んで渡す。3 行が埋まった次のページで
    //   頁がめくれる (band が 1 行だけを返すのがその印)。
    char band[STACKEE_TALK_SUB_BAND_MAX];
    stackee_talk_band(t, want, band, sizeof(band));
    subtitle(t, band);
}

// ---------------------------------------------------------------------------
// 受け箱 (GET /inbox) と発話 1 件の再生
// ---------------------------------------------------------------------------
// ★ **キー押下に縛られない部品。** 入口は「受け箱を回す状態 (INBOX_WAIT) に
//   入る」ことだけで、発話 (state:"say") が来たら
//     音あり … GET <audio_url> → PLAY_WAIT → PLAYING (会話の再生と同じ部品)
//     音なし … SAY_TEXT (字幕だけ)
//   を通って INBOX_WAIT に戻る。いまは CSTM のコマンド方式だけが入口
//   (job= 付き、job_state が done / error で終わる)。第 2 段の常時ポーリングは
//   job を空にして同じ INBOX_WAIT に入れればよい。

// 1 往復の名残の数字 (押下からの ms)。0 は「まだ」の意味に使うので 1 に寄せる。
static uint32_t mark_ms(const stackee_talk_t *t, uint32_t from) {
    uint32_t ms = since_ms(t, from);
    return ms ? ms : 1;
}

static void inbox_note_seq(stackee_talk_t *t, uint32_t seq) {
    t->inbox_seq = seq;
    t->inbox_seq_valid = true;
    t->inbox_seq_at = now(t);
}

// id は受け箱の URL (job=) にそのまま載せるので、使える字を絞る。
static bool job_id_ok(const char *id) {
    if (id == NULL || id[0] == '\0') {
        return false;
    }
    for (const char *p = id; *p; p++) {
        bool ok = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= 'A' && *p <= 'Z') || *p == '-' || *p == '_' || *p == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// 200 以外の応答。409 は会話と同じ文言。古い中継の 404 はそれと分かるように。
static void fail_status(stackee_talk_t *t, int status, bool inbox) {
    char why[96];
    if (status == 409) {
        snprintf(why, sizeof(why), "サーバーが処理中です (HTTP 409)");
    } else if (inbox && status == 404) {
        snprintf(why, sizeof(why), "中継が /inbox に対応していません (HTTP 404)");
    } else {
        snprintf(why, sizeof(why), "サーバー HTTP %d", status);
    }
    fail(t, why);
}

// 次の GET /inbox?after=<seq>&wait=25[&job=<id>] を撃つ。
static void inbox_poll_start(stackee_talk_t *t) {
    char path[STACKEE_TALK_PATH_MAX + STACKEE_TALK_JOB_ID_MAX + 48];
    int n;
    if (t->cstm_job[0] != '\0' && t->cstm_mode == STACKEE_TALK_CSTM_MODE_COMMAND) {
        n = snprintf(path, sizeof(path), "%s?after=%lu&wait=%d&job=%s",
                     t->inbox_path, (unsigned long)t->inbox_seq,
                     STACKEE_TALK_INBOX_WAIT_S, t->cstm_job);
    } else {
        n = snprintf(path, sizeof(path), "%s?after=%lu&wait=%d",
                     t->inbox_path, (unsigned long)t->inbox_seq,
                     STACKEE_TALK_INBOX_WAIT_S);
    }
    if (n < 0 || (size_t)n >= sizeof(path)) {
        fail(t, "受け箱の URL が長すぎます");
        return;
    }
    t->poll_sent = now(t);
    if (!t->ops->http_start("GET", path, NULL, 0, STACKEE_TALK_POLL_LIMIT, NULL)) {
        fail(t, "受け箱の取得を始められません");
        return;
    }
    t->http_open = true;
    t->inbox_polls++;
    to(t, STACKEE_TALK_INBOX);
}

// 発話を 1 件扱い終えた (鳴らした / 鳴らさず止めた / 字幕を出し終えた)。
// ★ 受信バッファ (PCM) を返し、帯を消してから次を聞きに行く。
static void say_finished(stackee_talk_t *t, bool played) {
    if (t->say_cur >= 0) {
        t->say_log[t->say_cur].played = played;
    }
    if (t->watch_say) {
        if (played) {
            t->watch_played++;
        }
        logf_(t, "[inbox-say] {\"seq\":%lu,\"audio_bytes\":%ld,"
                 "\"sub_pages\":%d,\"played\":%d,\"received\":%lu}",
              (unsigned long)t->inbox_seq, (long)t->audio_samples * 2,
              t->page_count, played ? 1 : 0, (unsigned long)t->watch_received);
        t->watch_say = false;
    } else {
        logf_(t, "[cstm-say] {\"n\":%lu,\"seq\":%lu,\"audio_bytes\":%ld,"
                 "\"sub_pages\":%d,\"played\":%d}",
              (unsigned long)t->says, (unsigned long)t->inbox_seq,
              (long)t->audio_samples * 2, t->page_count, played ? 1 : 0);
    }
    t->say_cur = -1;
    cleanup(t);
    if (!t->inbox_loop) {
        to(t, STACKEE_TALK_IDLE);
        return;
    }
    // 次はすぐ聞く (発話のあいだに次が溜まっているかもしれない)。
    t->polled = now(t);
    t->poll_took = STACKEE_TALK_POLL_MS;
    to(t, STACKEE_TALK_INBOX_WAIT);
}

// 発話 1 件 ({"state":"say", …}) を受け取った。json は close_http まで有効。
static void take_say(stackee_talk_t *t, const char *json) {
    // ★ 常時ポーリングの発話は CSTM の記録 (key.cstm_status) に混ぜない。
    if (!t->watch_say) {
        t->says++;
        t->say_cur = (t->says <= STACKEE_TALK_SAY_LOG) ? (int)t->says - 1 : -1;
        if (t->cstm_first_say_ms == 0) {
            t->cstm_first_say_ms = mark_ms(t, t->cstm_started);
        }
    } else {
        t->say_cur = -1;
    }
    t->audio_samples = 0;
    t->audio_duration_ms = 0;
    int got = take_reply(t, json, false);
    if (got < 0) {
        return;                 // 形式が違う / audio_url が不正 (fail 済み)
    }
    long declared = 0;
    if (!stackee_json_int(json, "audio_bytes", &declared) || declared < 0) {
        declared = 0;
    }
    if (t->say_cur >= 0) {
        stackee_talk_say_t *say = &t->say_log[t->say_cur];
        memset(say, 0, sizeof(*say));
        say->seq = t->inbox_seq;
        say->at_ms = mark_ms(t, t->cstm_started);
        say->sub_bytes = t->sub_bytes;
        say->sub_pages = t->page_count;
        say->audio_bytes = (uint32_t)declared;
        say->reply_len = (uint32_t)t->reply_len;
        say->audio = (got == 1);
    }
    close_http(t);
    // ★ 常時ポーリングの play=0 (数えるだけ) は画面の 1 行にも出さない。
    if (t->reply[0] != '\0' && !(t->watch_say && !t->say_play)) {
        show(t, t->reply);
    }
    if (got == 1) {
        // 音あり。会話の done と同じ手順 (字幕の別 GET → PCM → 再生)。
        if (t->page_count == 0 && t->subs_path[0] != '\0' &&
            t->ops->http_start("GET", t->subs_path, NULL, 0,
                               STACKEE_TALK_SUB_BYTES, NULL)) {
            t->http_open = true;
            to(t, STACKEE_TALK_SUBS);
            return;
        }
        start_audio(t);
        return;
    }
    // 音なし。字幕だけ出す (本文が無ければ返答文を割って出す)。
    // ★ 常時ポーリングの play=0 (数えるだけ) は帯にも出さない。
    if (t->watch_say && !t->say_play) {
        say_finished(t, false);
        return;
    }
    if (t->page_count == 0) {
        pages_from_text(t, t->reply);
        if (t->say_cur >= 0) {
            t->say_log[t->say_cur].sub_pages = t->page_count;
        }
    }
    if (t->page_count == 0) {
        say_finished(t, false);         // 出すものが無い
        return;
    }
    t->say_until = t->pages[t->page_count - 1].start_ms + STACKEE_TALK_SAY_HOLD_MS;
    to(t, STACKEE_TALK_SAY_TEXT);
}

// GET /inbox (seq を取る) と GET /inbox?after= の応答。
static void handle_inbox(stackee_talk_t *t, int status, const char *json) {
    if (status != 200) {
        if (status == 404) {
            t->inbox_seq_valid = false;     // 次の押下で取り直す
        }
        fail_status(t, status, true);
        return;
    }
    char state[16];
    if (!stackee_json_str(json, "state", state, sizeof(state))) {
        fail(t, "受け箱の応答が不正です");
        return;
    }
    long seq = -1;
    bool has_seq = stackee_json_int(json, "seq", &seq) && seq >= 0;
    if (t->state == STACKEE_TALK_INBOX_SEQ) {
        if (!has_seq) {
            fail(t, "受け箱の応答に seq がありません");
            return;
        }
        inbox_note_seq(t, (uint32_t)seq);
        t->cstm_seq_ms = mark_ms(t, t->cstm_started);
        close_http(t);
        // ★ 最後に見た seq が分かったので、ここで初めてキーを送る。
        if (!t->ops->http_start("POST", t->key_path, t->key_body,
                                strlen(t->key_body), 8192,
                                STACKEE_TALK_CTYPE_JSON)) {
            fail(t, "送信を始められません");
            return;
        }
        t->http_open = true;
        to(t, STACKEE_TALK_KEY);
        return;
    }
    // ---- GET /inbox?after= の応答 ----
    t->polled = now(t);
    t->poll_took = since_ms(t, t->poll_sent);
    if (strcmp(state, "say") == 0) {
        if (!has_seq) {
            fail(t, "受け箱の発話に seq がありません");
            return;
        }
        if (t->inbox_seq_valid && (uint32_t)seq <= t->inbox_seq) {
            close_http(t);              // 見たことのある発話。飛ばして次を聞く
            to(t, STACKEE_TALK_INBOX_WAIT);
            return;
        }
        inbox_note_seq(t, (uint32_t)seq);
        take_say(t, json);
        return;
    }
    if (strcmp(state, "empty") != 0) {
        fail(t, "受け箱の state が不明です");
        return;
    }
    // ★ 空のときの seq にはいつも合わせる (サーバが再起動して巻き戻った
    //   ときも、ほかの仕事の発話で進んだときも、それが「いまの最後」)。
    if (has_seq) {
        inbox_note_seq(t, (uint32_t)seq);
    }
    char job_state[16] = "";
    stackee_json_str(json, "job_state", job_state, sizeof(job_state));
    snprintf(t->cstm_job_state, sizeof(t->cstm_job_state), "%s", job_state);
    if (job_state[0] == '\0' || strcmp(job_state, "processing") == 0) {
        close_http(t);
        to(t, STACKEE_TALK_INBOX_WAIT);
        return;
    }
    if (strcmp(job_state, "done") == 0) {
        close_http(t);
        t->cstm_final = "done";
        cleanup(t);
        to(t, STACKEE_TALK_IDLE);       // 音なしで終わる
        return;
    }
    char why[STACKEE_TALK_TEXT_MAX];
    if (strcmp(job_state, "error") == 0) {
        if (!stackee_json_str(json, "error", why, sizeof(why)) || why[0] == '\0') {
            snprintf(why, sizeof(why), "コマンドが失敗しました");
        }
    } else {
        snprintf(why, sizeof(why), "コマンドの状態が不明です (%s)", job_state);
    }
    close_http(t);
    fail(t, why);
}

// ---------------------------------------------------------------------------
// stackee 独自キー CSTM_0〜CSTM_9 (POST /key)
// ---------------------------------------------------------------------------
static void handle_key(stackee_talk_t *t, int status, const char *json) {
    t->cstm_key_ms = mark_ms(t, t->cstm_started);
    if (status == 200) {
        char state[16];
        if (stackee_json_str(json, "state", state, sizeof(state)) &&
            strcmp(state, "ignored") == 0) {
            // 未設定。音も出さず、短く知らせて終わる (cstm_finish が帯に出す)。
            close_http(t);
            t->cstm_final = "ignored";
            cleanup(t);
            to(t, STACKEE_TALK_IDLE);
            char text[48];
            snprintf(text, sizeof(text), "CSTM_%d 未設定", t->cstm_n);
            show(t, text);
            return;
        }
        fail(t, "/key の応答が不正です");
        return;
    }
    if (status != 202) {
        fail_status(t, status, false);
        return;
    }
    char mode[16] = "";
    stackee_json_str(json, "mode", mode, sizeof(mode));
    char id[STACKEE_TALK_JOB_ID_MAX];
    bool has_id = stackee_json_str(json, "id", id, sizeof(id)) && job_id_ok(id);
    if (mode[0] == '\0' || strcmp(mode, "prompt") == 0) {
        // ★ /look と**完全に同じ**後半 (status_url の返答待ち → 音声 + 字幕)。
        t->cstm_mode = STACKEE_TALK_CSTM_MODE_PROMPT;
        if (has_id) {
            snprintf(t->cstm_job, sizeof(t->cstm_job), "%s", id);
        }
        handle_upload_done(t, json);
        // 一次回答も /look と同じく返事を待つ前に鳴らす (play=0 では鳴らさない)。
        if (t->state == STACKEE_TALK_POLL_WAIT && t->cstm_play) {
            t->ops->ack_begin();
            face(t);
        }
        return;
    }
    if (strcmp(mode, "command") != 0) {
        char why[64];
        snprintf(why, sizeof(why), "/key の mode が不明です (%.16s)", mode);
        fail(t, why);
        return;
    }
    if (!has_id) {
        fail(t, "コマンドの id が不正です");
        return;
    }
    snprintf(t->cstm_job, sizeof(t->cstm_job), "%s", id);
    t->cstm_mode = STACKEE_TALK_CSTM_MODE_COMMAND;
    close_http(t);
    t->accepted_ms = since_ms(t, t->turn_started);
    t->cstm_cmd_started = now(t);
    t->inbox_loop = true;
    t->say_play = t->cstm_play;
    t->polled = now(t);
    t->poll_sent = t->polled;
    t->poll_took = STACKEE_TALK_POLL_MS;    // 最初の 1 回はすぐ聞く
    to(t, STACKEE_TALK_INBOX_WAIT);
    show(t, "コマンドを実行中…");
}

// CSTM の流れが idle に戻った。数え、帯に短く知らせ、ログに残す。
// ★ update_guide のあとに呼ぶ (案内の後始末で帯が消されないように)。
static void cstm_finish(stackee_talk_t *t) {
    t->cstm_active = false;
    t->inbox_loop = false;
    t->say_cur = -1;
    t->cstm_end_ms = mark_ms(t, t->cstm_started);
    if (t->cstm_final == NULL) {
        t->cstm_final = (t->error[0] != '\0') ? "error" : "done";
    }
    char text[STACKEE_TALK_TEXT_MAX + 24];
    if (strcmp(t->cstm_final, "ignored") == 0) {
        t->cstm_ignored++;
        snprintf(text, sizeof(text), "CSTM_%d 未設定", t->cstm_n);
        notice(t, text);
    } else if (strcmp(t->cstm_final, "error") == 0) {
        t->cstm_errors++;
        snprintf(text, sizeof(text), "会話エラー: %s", t->error);
        notice(t, text);
    } else {
        t->cstm_done++;
    }
    logf_(t, "[cstm] {\"n\":%d,\"mode\":\"%s\",\"final\":\"%s\",\"says\":%lu,"
             "\"polls\":%d,\"seq_ms\":%lu,\"seq_reused\":%d,\"key_ms\":%lu,"
             "\"first_say_ms\":%lu,\"end_ms\":%lu,\"play\":%d}",
          t->cstm_n, stackee_talk_cstm_mode_names[t->cstm_mode], t->cstm_final,
          (unsigned long)t->says, t->inbox_polls,
          (unsigned long)t->cstm_seq_ms, t->cstm_seq_reused ? 1 : 0,
          (unsigned long)t->cstm_key_ms, (unsigned long)t->cstm_first_say_ms,
          (unsigned long)t->cstm_end_ms, t->cstm_play ? 1 : 0);
}


// ---------------------------------------------------------------------------
// 暇なときに受け箱を見る (常時ポーリング、2026-09-27)
// ---------------------------------------------------------------------------
// ★ 待っている間は state を IDLE のまま動かさない。だから会話キー・カメラ・
//   CSTM・talk.inject はいつでも入れて、入った瞬間にこちらの待ちを打ち切る
//   (watch_abort)。USB マイク・音量の保存・Wi-Fi の走査の「後回し」も
//   待っているだけでは発動しない (どれも busy を見ている)。
// ★ 発話 ("say") を受けたら、CSTM のコマンドと同じ部品 (take_say →
//   字幕の GET → PCM の GET → 再生 / 字幕だけ) に渡す。その間だけ busy。

// 撃ってよいか (暇で、外の都合も許す)。
static bool watch_can_run(stackee_talk_t *t) {
    return t->state == STACKEE_TALK_IDLE && !t->look_reserved && !t->pressed &&
           !t->cstm_active && t->inbox_path[0] != '\0' &&
           !t->ops->ack_active() && !t->ops->play_active() &&
           (t->ops->watch_ok == NULL || t->ops->watch_ok()) &&
           t->ops->net_ready();
}

// 待っている要求を打ち切る (キーが押された・止められた)。通信中なら
// 通信側がソケットを shutdown して起こし、次の要求はすぐ受け付けられる。
static void watch_abort(stackee_talk_t *t, bool count) {
    if (!t->watch_http) {
        return;
    }
    t->ops->http_close();
    t->watch_http = false;
    if (count) {
        t->watch_aborts++;
    }
    if (t->watch_phase == STACKEE_TALK_WATCH_SEQ ||
        t->watch_phase == STACKEE_TALK_WATCH_POLL) {
        t->watch_phase = STACKEE_TALK_WATCH_WAIT;
    }
    // ★ 取りこぼしは無い (seq で続きから聞く)。暇に戻ったらすぐ聞き直す。
    t->watch_next = now(t);
}

static void watch_backoff(stackee_talk_t *t, const char *why) {
    t->watch_fails++;
    snprintf(t->watch_error, sizeof(t->watch_error), "%s", why);
    t->watch_backoff = (t->watch_backoff == 0)
                           ? STACKEE_TALK_WATCH_BACKOFF_MIN_MS
                           : t->watch_backoff * 2;
    if (t->watch_backoff > STACKEE_TALK_WATCH_BACKOFF_MAX_MS) {
        t->watch_backoff = STACKEE_TALK_WATCH_BACKOFF_MAX_MS;
    }
    t->watch_next = now(t) + t->watch_backoff;
    t->watch_phase = STACKEE_TALK_WATCH_WAIT;
    logf_(t, "[inbox] %s (次は %lu ms 後)", why, (unsigned long)t->watch_backoff);
}

void stackee_talk_watch_enable(stackee_talk_t *t, bool on, bool play) {
    t->watch_play = play;
    if (on == t->watch_on) {
        return;
    }
    t->watch_on = on;
    if (!on) {
        watch_abort(t, false);
        t->watch_phase = STACKEE_TALK_WATCH_OFF;
        return;
    }
    t->watch_phase = STACKEE_TALK_WATCH_WAIT;
    t->watch_backoff = 0;
    t->watch_next = now(t);
}

// 受け箱の応答。★ body は通信側の受信バッファ。使い終えたら閉じる。
static void watch_handle(stackee_talk_t *t, int status, const char *json) {
    t->watch_status = status;
    bool was_seq = (t->watch_phase == STACKEE_TALK_WATCH_SEQ);
    if (status == 404) {
        // 古い中継。しばらく聞かない (静かに休む)。
        t->ops->http_close();
        snprintf(t->watch_error, sizeof(t->watch_error),
                 "中継が /inbox に対応していません (HTTP 404)");
        t->watch_phase = STACKEE_TALK_WATCH_SLEEP;
        t->watch_next = now(t) + STACKEE_TALK_WATCH_404_MS;
        logf_(t, "[inbox] 中継が /inbox に対応していない (404)。%lu 秒休む",
              (unsigned long)(STACKEE_TALK_WATCH_404_MS / 1000));
        return;
    }
    if (status != 200) {
        t->ops->http_close();
        char why[48];
        snprintf(why, sizeof(why), "サーバー HTTP %d", status);
        watch_backoff(t, why);
        return;
    }
    char state[16];
    long seq = -1;
    bool has_state = stackee_json_str(json, "state", state, sizeof(state));
    bool has_seq = stackee_json_int(json, "seq", &seq) && seq >= 0;
    if (!has_state) {
        t->ops->http_close();
        watch_backoff(t, "受け箱の応答が不正です");
        return;
    }
    if (was_seq) {
        t->ops->http_close();
        if (!has_seq) {
            watch_backoff(t, "受け箱の応答に seq がありません");
            return;
        }
        // ★ いまの最後を覚えるだけ。これより前の発話は鳴らさない。
        inbox_note_seq(t, (uint32_t)seq);
        t->watch_seq_known = true;
        t->watch_backoff = 0;
        t->watch_error[0] = '\0';
        t->watch_phase = STACKEE_TALK_WATCH_WAIT;
        t->watch_next = now(t);
        return;
    }
    uint32_t took = since_ms(t, t->watch_sent);
    if (strcmp(state, "say") == 0) {
        if (!has_seq) {
            t->ops->http_close();
            watch_backoff(t, "受け箱の発話に seq がありません");
            return;
        }
        t->watch_backoff = 0;
        t->watch_error[0] = '\0';
        t->watch_next = now(t);
        t->watch_phase = STACKEE_TALK_WATCH_WAIT;
        if (t->inbox_seq_valid && (uint32_t)seq <= t->inbox_seq) {
            t->ops->http_close();       // 見たことのある発話
            t->watch_skipped++;
            return;
        }
        inbox_note_seq(t, (uint32_t)seq);
        t->watch_received++;
        logf_(t, "[inbox] 発話を受けた (seq %ld, play=%d)", seq,
              t->watch_play ? 1 : 0);
        // 状態機械に渡す。★ 通信 (受信バッファ) の持ち主もここで移る。
        t->watch_say = true;
        t->watch_phase = STACKEE_TALK_WATCH_SAY;
        t->look = false;
        t->cstm = false;
        t->inbox_loop = false;
        t->say_play = t->watch_play;
        t->error[0] = '\0';
        t->turn_started = now(t);
        t->http_open = true;
        take_say(t, json);
        return;
    }
    t->ops->http_close();
    if (strcmp(state, "empty") != 0) {
        watch_backoff(t, "受け箱の state が不明です");
        return;
    }
    if (has_seq) {
        inbox_note_seq(t, (uint32_t)seq);
    }
    t->watch_backoff = 0;
    t->watch_error[0] = '\0';
    t->watch_phase = STACKEE_TALK_WATCH_WAIT;
    // ★ ロングポーリングなのに 1 秒未満で空が返った = 中継が握ってくれない。
    //   毎秒撃たないよう間合いをあける。張り直しの最初の 1 本 (待たない
    //   要求) なら、すぐロングポーリングに移る。
    // ★ 待たない要求のあとで接続が残らなかった (相手が Connection: close を
    //   返す中継) ときも間合いをあける。あけないと握手を休みなく繰り返す。
    bool warm = (t->ops->http_warm == NULL) || t->ops->http_warm();
    bool quick = t->watch_long ? (took < STACKEE_TALK_POLL_MS) : !warm;
    t->watch_next = now(t) + (quick ? STACKEE_TALK_WATCH_QUICK_MS : 0);
}

static void watch_start(stackee_talk_t *t) {
    char path[STACKEE_TALK_PATH_MAX + 48];
    bool warm = (t->ops->http_warm == NULL) || t->ops->http_warm();
    int n;
    if (!t->watch_seq_known) {
        n = snprintf(path, sizeof(path), "%s", t->inbox_path);
        t->watch_long = false;
    } else if (warm) {
        n = snprintf(path, sizeof(path), "%s?after=%lu&wait=%d", t->inbox_path,
                     (unsigned long)t->inbox_seq, STACKEE_TALK_INBOX_WAIT_S);
        t->watch_long = true;
    } else {
        // ★ 接続が無い (張り直し)。最初の 1 本は待たない要求にする。握手の
        //   最中にキーで打ち切られても、張れた接続をそのまま次の要求 (会話の
        //   POST など) に回せる。
        n = snprintf(path, sizeof(path), "%s?after=%lu", t->inbox_path,
                     (unsigned long)t->inbox_seq);
        t->watch_long = false;
    }
    if (n < 0 || (size_t)n >= sizeof(path)) {
        watch_backoff(t, "受け箱の URL が長すぎます");
        return;
    }
    if (!t->ops->http_start("GET", path, NULL, 0, STACKEE_TALK_POLL_LIMIT, NULL)) {
        watch_backoff(t, "受け箱の取得を始められません");
        return;
    }
    t->watch_http = true;
    t->watch_sent = now(t);
    t->watch_polls++;
    t->watch_phase = t->watch_seq_known ? STACKEE_TALK_WATCH_POLL
                                        : STACKEE_TALK_WATCH_SEQ;
}

static void watch_step(stackee_talk_t *t) {
    if (!t->watch_on) {
        watch_abort(t, false);
        t->watch_phase = STACKEE_TALK_WATCH_OFF;
        return;
    }
    // 発話を扱い終えて idle に戻った (fail / say_finished が watch_say を
    // 下ろす)。
    if (t->watch_phase == STACKEE_TALK_WATCH_SAY && !t->watch_say &&
        t->state == STACKEE_TALK_IDLE) {
        t->watch_phase = STACKEE_TALK_WATCH_WAIT;
    }
    // ★ Wi-Fi が落ちたら、上がったときに最新 seq から聞き直す
    //   (落ちていた間の発話は鳴らさない)。
    bool net = t->ops->net_ready();
    if (!net) {
        t->watch_seq_known = false;
    }
    t->watch_net = net;
    if (t->watch_http) {
        if (!watch_can_run(t)) {
            watch_abort(t, true);       // キーが押された・OTA が始まった など
            return;
        }
        int status = 0;
        const uint8_t *body = NULL;
        size_t len = 0;
        int got = t->ops->http_poll(&status, &body, &len);
        if (got == 0) {
            return;
        }
        t->watch_http = false;
        if (got < 0) {
            t->ops->http_close();
            watch_backoff(t, "通信に失敗しました");
            return;
        }
        watch_handle(t, status, (body == NULL) ? "{}" : (const char *)body);
        return;
    }
    if (t->watch_say || !watch_can_run(t)) {
        return;
    }
    if ((int32_t)(now(t) - t->watch_next) < 0) {
        return;
    }
    if (t->watch_phase == STACKEE_TALK_WATCH_SLEEP) {
        t->watch_phase = STACKEE_TALK_WATCH_WAIT;   // 休み明け
    }
    watch_start(t);
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
    // CSTM と受け箱。★ 応答の読み方 (200 / 202 / 409 / 404) が会話と違うので
    //   ここで分ける。body の NUL 終端は通信側が保証する。
    if (t->state == STACKEE_TALK_KEY || t->state == STACKEE_TALK_INBOX_SEQ ||
        t->state == STACKEE_TALK_INBOX) {
        const char *json = (body == NULL) ? "{}" : (const char *)body;
        if (t->state == STACKEE_TALK_KEY) {
            handle_key(t, status, json);
        } else {
            handle_inbox(t, status, json);
        }
        return;
    }
    if (status != 200 && status != 202) {
        char why[96];
        if (status == 409) {
            // ★ サーバは 1 度に 1 件しか受けない (会話と画像で共有)。
            snprintf(why, sizeof(why), "サーバーが処理中です (HTTP 409)");
        } else {
            snprintf(why, sizeof(why), "サーバー HTTP %d", status);
        }
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
        if (t->inbox_loop && t->say_cur >= 0) {
            t->say_log[t->say_cur].audio_got = (uint32_t)len;
        }
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

static void log_turn_timing(stackee_talk_t *t, bool played) {
    logf_(t, "[talk-turn-timing] {\"accepted_ms\":%lu,\"reply_ready_ms\":%lu,"
             "\"audio_ready_ms\":%lu,\"play_setup_ms\":%lu,"
             "\"audio_duration_ms\":%d,\"polls\":%d,\"complete_ms\":%lu,"
             "\"sub_pages\":%d,\"sub_src\":\"%s\","
             "\"first_sample_ms\":%lu,\"rec_ms\":%lu,"
             "\"look\":%d,\"played\":%d}",
          (unsigned long)t->accepted_ms, (unsigned long)t->reply_ready_ms,
          (unsigned long)t->audio_ready_ms, (unsigned long)t->play_setup_ms,
          t->audio_duration_ms, t->polls, (unsigned long)t->complete_ms,
          t->page_count, stackee_talk_sub_src_names[t->sub_src],
          (unsigned long)t->first_sample_ms,
          (unsigned long)t->last_rec_ms, t->look ? 1 : 0, played ? 1 : 0);
}

static void talk_step_inner(stackee_talk_t *t) {
    bool rising = t->pressed && !t->was_pressed;
    t->was_pressed = t->pressed;
    // ★ 毎周知らせる。一次回答が鳴り終わるのは「状態が変わらない変化」なので、
    //   遷移のときだけ知らせると顔が speaking のまま取り残される。
    face(t);

    switch (t->state) {
        case STACKEE_TALK_IDLE:
            // ★ 画像のために押さえている間 (撮影中) は会話キーを受け付けない。
            if (rising && !t->look_reserved && !t->ops->ack_active()) {
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
                                        STACKEE_TALK_POLL_LIMIT, NULL)) {
                    fail(t, "状態の取得を始められません");
                    return;
                }
                t->http_open = true;
                t->polls++;
                to(t, STACKEE_TALK_POLL);
            }
            return;

        case STACKEE_TALK_PLAY_WAIT:
            // ★ 受け箱の発話で play=0 (検証) なら、PCM を受け取り終えた
            //   ここで止めて次を聞きに行く。音は 1 つも出さない。
            if ((t->inbox_loop || t->watch_say) && !t->say_play) {
                if (t->audio == NULL || t->audio_samples <= 0) {
                    fail(t, "返答の PCM が無い");
                    return;
                }
                say_finished(t, false);
                return;
            }
            // ★ 画像の検証 (play=0) は**ここで止める**。PCM は受け取り終えて
            //   いる (audio_samples が数えてある) ので、鳴らす直前まで
            //   全部の段を通ったことになる。音は 1 つも出さない。
            //   CSTM の prompt 方式の play=0 も同じ (/look と同じ後半)。
            if (!t->inbox_loop && !t->watch_say &&
                ((t->look && !t->look_play) ||
                                   (t->cstm && !t->cstm_play))) {
                if (t->audio == NULL || t->audio_samples <= 0) {
                    fail(t, "返答の PCM が無い");
                    return;
                }
                t->complete_ms = since_ms(t, t->turn_started);
                log_turn_timing(t, false);
                t->turns++;
                if (t->look) {
                    t->looks_done++;
                    t->looks_unplayed++;
                }
                cleanup(t);
                to(t, STACKEE_TALK_IDLE);
                return;
            }
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
                show_page_at(t, since_ms(t, t->since));
                return;
            }
            // 受け箱の発話は鳴り終わったら次を聞きに行く (idle には戻らない)。
            // 常時ポーリングの発話は idle に戻り、受け箱の見張りに返す。
            if (t->inbox_loop || t->watch_say) {
                if (t->ops->play_failed()) {
                    fail(t, "音声再生またはマイク復帰に失敗しました");
                    return;
                }
                say_finished(t, true);
                return;
            }
            t->complete_ms = since_ms(t, t->turn_started);
            log_turn_timing(t, true);
            t->turns++;
            if (t->look) {
                t->looks_done++;
            }
            cleanup(t);
            to(t, STACKEE_TALK_IDLE);
            if (t->ops->play_failed()) {
                fail(t, "音声再生またはマイク復帰に失敗しました");
            }
            return;

        case STACKEE_TALK_INBOX_WAIT:
            // ★ 全体の上限はコマンド方式のときだけ (起点は受理した時刻)。
            if (t->cstm_active && t->cstm_mode == STACKEE_TALK_CSTM_MODE_COMMAND &&
                since_ms(t, t->cstm_cmd_started) > STACKEE_TALK_CSTM_TIMEOUT_MS) {
                fail(t, "コマンドの応答待ちがタイムアウトしました");
                return;
            }
            {
                // 返答待ちと同じ間合い: ロングポーリングが効いていれば
                // (1 往復が 1 秒以上なら) すぐ、即返る中継なら 1 秒あける。
                uint32_t gap = (t->poll_took >= STACKEE_TALK_POLL_MS)
                                   ? 0 : STACKEE_TALK_POLL_MS;
                if (since_ms(t, t->polled) < gap) {
                    return;
                }
            }
            inbox_poll_start(t);
            return;

        case STACKEE_TALK_SAY_TEXT: {
            uint32_t at = since_ms(t, t->since);
            show_page_at(t, at);
            if (at >= t->say_until) {
                // 字幕だけの発話は「出し終えた」を played に数えるのは
                // 常時ポーリングだけ (CSTM の記録は従来どおり音の有無)。
                say_finished(t, t->watch_say && t->say_play);
            }
            return;
        }

        case STACKEE_TALK_UPLOAD:
        case STACKEE_TALK_POLL:
        case STACKEE_TALK_SUBS:
        case STACKEE_TALK_AUDIO:
        case STACKEE_TALK_INBOX_SEQ:
        case STACKEE_TALK_KEY:
        case STACKEE_TALK_INBOX:
            step_http(t);
            return;

        default:
            return;
    }
}

// ---------------------------------------------------------------------------
// 案内の字幕
// ---------------------------------------------------------------------------
// ★ **帯の持ち主は 1 人。** 一次回答 (ack) が鳴っている間は audio が、
//   返答の字幕が出ている間は上の PLAYING が持っている。案内はそのどちらも
//   いないときだけ出す。持ち主が居るときは **触らない** (消しもしない)。
static int guide_want(const stackee_talk_t *t) {
    if (t->state == STACKEE_TALK_IDLE) {
        return STACKEE_TALK_GUIDE_NONE;
    }
    // ★ 常時ポーリングで届いた発話は「考えています…」ではない (頼まれて
    //   考えているのではなく、知らせが届いた)。字幕が出るまで帯は空。
    if (t->watch_say && t->page_shown < 0 &&
        !(t->state == STACKEE_TALK_PLAYING || t->state == STACKEE_TALK_SAY_TEXT)) {
        return STACKEE_TALK_GUIDE_NONE;
    }
    if (t->state == STACKEE_TALK_RECORDING) {
        return STACKEE_TALK_GUIDE_REC;
    }
    // 一次回答が鳴っている / 返答の字幕が出ている → そちらが持ち主。
    if (t->ops->ack_active != NULL && t->ops->ack_active()) {
        return STACKEE_TALK_GUIDE_HELD;
    }
    if (t->page_shown >= 0) {
        return STACKEE_TALK_GUIDE_HELD;
    }
    if (t->state == STACKEE_TALK_PLAYING || t->state == STACKEE_TALK_SAY_TEXT) {
        // ★ 字幕を持っているなら触らない。1 ページ目が出るまでの 1 周で
        //   帯が黒くちらつかないように、**ページがあるかどうか**で見る
        //   (page_shown はまだ -1 でも、次の周で出る)。
        if (t->page_count > 0) {
            return STACKEE_TALK_GUIDE_HELD;
        }
        // 返答を鳴らしているのに字幕が無い (旧サーバ / 取れなかった)。
        // 「考えています…」はもう嘘なので、帯は空にする。
        return STACKEE_TALK_GUIDE_NONE;
    }
    return STACKEE_TALK_GUIDE_THINK;
}

static void update_guide(stackee_talk_t *t) {
    int want = guide_want(t);
    if (want == t->guide_shown) {
        return;
    }
    int had = t->guide_shown;
    t->guide_shown = want;
    switch (want) {
        case STACKEE_TALK_GUIDE_REC:
            subtitle(t, t->guide_rec);
            break;
        case STACKEE_TALK_GUIDE_THINK:
            subtitle(t, t->guide_think);
            break;
        case STACKEE_TALK_GUIDE_HELD:
            break;              // 持ち主が描く。こちらは何もしない
        default:
            // 自分が出していたものだけ消す。持ち主が居たなら向こうが消す。
            if (had == STACKEE_TALK_GUIDE_REC ||
                had == STACKEE_TALK_GUIDE_THINK) {
                subtitle(t, NULL);
            }
            break;
    }
}

void stackee_talk_set_guides(stackee_talk_t *t, const char *recording,
                             const char *thinking) {
    if (recording != NULL) { t->guide_rec = recording; }
    if (thinking != NULL)  { t->guide_think = thinking; }
}

void stackee_talk_step(stackee_talk_t *t) {
    if (t->look_notice) {
        t->look_notice = false;
        char text[STACKEE_TALK_TEXT_MAX];
        snprintf(text, sizeof(text), "会話エラー: %.200s", t->error);
        show(t, text);
    }
    talk_step_inner(t);
    watch_step(t);
    update_guide(t);
    // ★ 案内の後始末 (帯を消す) のあとで知らせを置く。順が逆だと消される。
    if (t->cstm_active && t->state == STACKEE_TALK_IDLE) {
        cstm_finish(t);
    }
    notice_step(t);
}

bool stackee_talk_busy(const stackee_talk_t *t) {
    return t->state != STACKEE_TALK_IDLE || t->look_reserved;
}

bool stackee_talk_audio_busy(const stackee_talk_t *t) {
    if (!stackee_talk_busy(t)) {
        return false;
    }
    if (t->watch_say) {
        return t->state == STACKEE_TALK_PLAY_WAIT ||
               t->state == STACKEE_TALK_PLAYING || t->look_reserved;
    }
    return true;
}

bool stackee_talk_uses_audio(const stackee_talk_t *t) {
    // 常時ポーリングで届いた発話は、PCM を取っている間は音を使わない。
    if (t->watch_say &&
        (t->state == STACKEE_TALK_SUBS || t->state == STACKEE_TALK_AUDIO)) {
        return t->look_reserved;
    }
    switch (t->state) {
        case STACKEE_TALK_IDLE:
        case STACKEE_TALK_INBOX_SEQ:
        case STACKEE_TALK_KEY:
        case STACKEE_TALK_INBOX_WAIT:
        case STACKEE_TALK_INBOX:
        case STACKEE_TALK_SAY_TEXT:
            // 音を使わない待ち。撮影のために押さえている間だけは従来どおり。
            return t->look_reserved;
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// stackee 独自キー CSTM_0〜CSTM_9 の入口
// ---------------------------------------------------------------------------
bool stackee_talk_cstm(stackee_talk_t *t, int n, bool play) {
    if (n < 0 || n >= STACKEE_TALK_CSTM_COUNT) {
        return false;
    }
    // ★ 会話・画像・他の CSTM の途中は黙って無視する (カメラと同じ約束)。
    if (t->state != STACKEE_TALK_IDLE || t->look_reserved || t->pressed ||
        t->ops->ack_active()) {
        t->cstm_busy++;
        return false;
    }
    begin_turn(t, false);
    t->cstm = true;
    t->cstm_active = true;
    t->cstm_play = play;
    t->cstm_n = n;
    t->cstm_mode = STACKEE_TALK_CSTM_MODE_NONE;
    t->cstm_job[0] = '\0';
    t->cstm_job_state[0] = '\0';
    t->cstm_final = NULL;
    t->cstm_started = now(t);
    t->cstm_cmd_started = 0;
    t->cstm_seq_ms = t->cstm_key_ms = 0;
    t->cstm_first_say_ms = t->cstm_end_ms = 0;
    t->cstm_seq_reused = false;
    t->cstm_count++;
    t->inbox_loop = false;
    t->inbox_polls = 0;
    t->says = 0;
    t->say_cur = -1;
    t->say_play = play;
    memset(t->say_log, 0, sizeof(t->say_log));
    t->error[0] = '\0';
    t->notice_on = false;       // 前の知らせは案内 (考えています…) が上書きする
    snprintf(t->key_body, sizeof(t->key_body), "{\"key\":\"CSTM_%d\"}", n);
    if (t->key_path[0] == '\0') {
        fail(t, valid_path(t->path) ? "STACKEE_TALK_URL が /talk で終わっていません"
                                    : "STACKEE_TALK_URL が未設定です");
        return true;
    }
    if (!t->ops->net_ready()) {
        fail(t, "Wi-Fi 未接続です");
        return true;
    }
    watch_abort(t, true);       // 受け箱の待ちを打ち切って通信を空ける
    char text[48];
    snprintf(text, sizeof(text), "CSTM_%d を送信中…", n);
    // ★ 覚えている seq が新しければ使い回す (GET /inbox の握手を 1 回省く)。
    if (t->inbox_seq_valid &&
        since_ms(t, t->inbox_seq_at) < STACKEE_TALK_INBOX_SEQ_TTL_MS) {
        t->cstm_seq_reused = true;
        if (!t->ops->http_start("POST", t->key_path, t->key_body,
                                strlen(t->key_body), 8192,
                                STACKEE_TALK_CTYPE_JSON)) {
            fail(t, "送信を始められません");
            return true;
        }
        t->http_open = true;
        to(t, STACKEE_TALK_KEY);
        show(t, text);
        return true;
    }
    if (!t->ops->http_start("GET", t->inbox_path, NULL, 0, 2048, NULL)) {
        fail(t, "送信を始められません");
        return true;
    }
    t->http_open = true;
    to(t, STACKEE_TALK_INBOX_SEQ);
    show(t, text);
    return true;
}

bool stackee_talk_inject(stackee_talk_t *t, const int16_t *pcm, int samples) {
    if (t->state != STACKEE_TALK_IDLE || t->look_reserved || t->ops->ack_active()) {
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
    watch_abort(t, true);       // 受け箱の待ちを打ち切って通信を空ける
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
    begin_turn(t, false);
    t->ops->wav_header(t->samples, t->count);
    const uint8_t *body = (const uint8_t *)t->samples - 44;
    size_t body_len = 44 + (size_t)t->count * 2;
    if (!t->ops->http_start("POST", t->path, body, body_len, 8192,
                            STACKEE_TALK_CTYPE_WAV)) {
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
// 画像を見せる (POST /look、2026-09-26)
// ---------------------------------------------------------------------------
// ★ 受理より後ろ (ポーリング・done・/audio・字幕・再生) は会話と 1 行も
//   違わない。ここにあるのは「送る前」だけ。
bool stackee_talk_sibling_path(const char *talk_path, const char *name,
                               char *out, size_t cap) {
    static const char TAIL[] = "/talk";
    const size_t tail = sizeof(TAIL) - 1;
    if (talk_path == NULL || name == NULL || out == NULL || cap == 0) {
        return false;
    }
    out[0] = '\0';
    size_t len = strlen(talk_path);
    size_t nlen = strlen(name);
    if (len < tail || !valid_path(talk_path) ||
        strcmp(talk_path + len - tail, TAIL) != 0 ||
        nlen == 0 || (len - tail) + 1 + nlen + 1 > cap) {
        return false;
    }
    memcpy(out, talk_path, len - tail);
    out[len - tail] = '/';
    memcpy(out + len - tail + 1, name, nlen + 1);
    return true;
}

bool stackee_talk_look_path(const char *talk_path, char *out, size_t cap) {
    return stackee_talk_sibling_path(talk_path, "look", out, cap);
}

// 送れないと分かっている理由を残す。★ fail() と違ってマイクにも通信にも
// 触らない (idle のまま。何も持っていない)。画面とログは次の step
// (audio タスク) が出す — 呼び手の camera タスクはスタックが小さいので、
// ここでは文字列を 1 つ写すだけにする。
static void refuse_look(stackee_talk_t *t, const char *why) {
    snprintf(t->error, sizeof(t->error), "%s", why);
    t->errors++;
    t->look_notice = true;
}

int stackee_talk_look_reserve(stackee_talk_t *t) {
    // ★ 会話中 (録音/送信/待ち/再生) と、一次回答が鳴っている間と、
    //   STK_TALK を押している間は撮らない。画面にも何も出さない。
    if (t->state != STACKEE_TALK_IDLE || t->look_reserved || t->pressed ||
        t->ops->ack_active()) {
        return STACKEE_TALK_LOOK_BUSY;
    }
    // 送れないなら撮らない (撮影は 4 秒かかり、ALDO3 も入れることになる)。
    if (t->look_path[0] == '\0') {
        refuse_look(t, valid_path(t->path)
                           ? "STACKEE_TALK_URL が /talk で終わっていません"
                           : "STACKEE_TALK_URL が未設定です");
        return STACKEE_TALK_LOOK_ERROR;
    }
    if (!t->ops->net_ready()) {
        refuse_look(t, "Wi-Fi 未接続です");
        return STACKEE_TALK_LOOK_ERROR;
    }
    t->look_reserved = true;
    t->error[0] = '\0';
    // ★ カメラのキーも最優先。受け箱の待ちを打ち切って通信を空ける
    //   (撮影の 4 秒のあいだに通信側が畳み終える)。ログにも画面にも
    //   触らない (camera タスクから呼ばれる)。
    watch_abort(t, true);
    return STACKEE_TALK_LOOK_OK;
}

void stackee_talk_look_release(stackee_talk_t *t) {
    t->look_reserved = false;
}

bool stackee_talk_look(stackee_talk_t *t, const uint8_t *jpeg, size_t len,
                       bool play) {
    if (t->state != STACKEE_TALK_IDLE) {
        t->look_reserved = false;
        return false;
    }
    // ★ 押さえはここで必ず下ろす。この先は状態が idle でなくなる (= busy)
    //   か、fail で idle に戻るかのどちらか。
    t->look_reserved = false;
    begin_turn(t, true);
    t->look_play = play;
    t->look_bytes = (uint32_t)len;
    t->looks++;
    t->error[0] = '\0';
    if (t->look_path[0] == '\0') {
        fail(t, valid_path(t->path) ? "STACKEE_TALK_URL が /talk で終わっていません"
                                    : "STACKEE_TALK_URL が未設定です");
        return false;
    }
    // JPEG は SOI (FF D8) で始まる。空や壊れたものは送らない。
    if (jpeg == NULL || len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        fail(t, "画像が不正です");
        return false;
    }
    if (len > STACKEE_TALK_LOOK_MAX_BYTES) {
        fail(t, "画像が大きすぎます (512 KiB まで)");
        return false;
    }
    if (!t->ops->net_ready()) {
        fail(t, "Wi-Fi 未接続です");
        return false;
    }
    // ★ 録音と同じ領域 (PSRAM) へ写す。録音の道は通らないので、その領域が
    //   空いていることは idle であることが保証している。受理で返すのも
    //   録音と同じ (handle_upload_done)。
    t->samples = t->ops->record_alloc((int)((len + 1) / 2));
    if (t->samples == NULL) {
        fail(t, "画像の領域を確保できません");
        return false;
    }
    memcpy(t->samples, jpeg, len);
    t->count = 0;
    if (!t->ops->http_start("POST", t->look_path, t->samples, len, 8192,
                            STACKEE_TALK_CTYPE_JPEG)) {
        fail(t, "送信を始められません");
        return false;
    }
    t->http_open = true;
    to(t, STACKEE_TALK_UPLOAD);
    show(t, "画像を送信中…");
    // 一次回答は会話と同じく返事を待つ前に鳴らす。★ play=0 (検証) では
    // 何も鳴らさない。
    if (play) {
        t->ops->ack_begin();
    }
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
