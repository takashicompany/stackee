// 会話の状態機械。firmware/kmk/stackee_talk.py の TalkLink の移植。
//
//   42 キー (STK_TALK) を押している間だけ録音し、離すと送信する。
//   idle → recording → upload → poll_wait ⇄ poll → audio → play_wait → playing
//
// 相手とやりとりは firmware/native-http/README.md と public/server/
// stackee_server.py のとおり:
//   POST /talk (audio/wav)      → 202 {"id":..,"status_url":"/jobs/<id>"}
//   GET  /jobs/<id>?wait=25     → 中継が最大 25 秒握る (ロングポーリング)。
//                                 {"state":"processing"} が返ったら投げ直す
//                               → {"state":"done","reply":..,"audio_url":..,
//                                  "sample_rate":16000,"channels":1,"sample_width":2}
//                               → {"state":"error"|"ignored", "error":..}
//   GET  /jobs/<id>/audio       → 16 kHz mono 16bit PCM (生バイト)
//
// ★ ESP-IDF に依存しない。録音・再生・通信・画面は ops で外から差し込む。
//   hostbuild (tools/test_talk_host.py) が偽の ops を挿して、タイムアウト・
//   ポーリング間隔・一次回答と最終回答の排他を時刻つきで確かめる。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACKEE_TALK_RATE            16000
// 録音の上限 [秒]。マイクから取る側だけの値で、返答の長さとは別。
#define STACKEE_TALK_RECORD_SECONDS  30
#define STACKEE_TALK_MAX_SAMPLES     (STACKEE_TALK_RATE * STACKEE_TALK_RECORD_SECONDS)
// これより短い録音は送らない (stackee_talk.py の RATE * .3)。
#define STACKEE_TALK_MIN_SAMPLES     (STACKEE_TALK_RATE * 3 / 10)
#define STACKEE_TALK_RECORD_MAX_MS   (STACKEE_TALK_RECORD_SECONDS * 1000)

// ---- 返答音声の上限 -------------------------------------------------------
// ★ 録音 (30 秒) とは別の値。返答の受け皿は録音バッファではなく
//   **通信側の受信バッファ** (stackee_http.c が 1 往復ごとに PSRAM へ取り、
//   再生が終わってから返す) なので、録音の 960 KB と同時には存在しない:
//     録音 960 KB → POST → 受理された時点で解放 → GET /audio で 3.84 MB
//   だから「録音中に返答が壊れる」経路は無い。
// ★ サーバ側も 120 秒で切ることになっている (本体からは申告しない)。
#define STACKEE_TALK_REPLY_SECONDS   120
#define STACKEE_TALK_REPLY_MAX_SAMPLES (STACKEE_TALK_RATE * STACKEE_TALK_REPLY_SECONDS)
#define STACKEE_TALK_REPLY_MAX_BYTES ((size_t)STACKEE_TALK_REPLY_MAX_SAMPLES * 2)
// GET /jobs/<id> が即返ってきたときの再送間隔。ロングポーリングが効いて
// いれば (要求そのものが 1 秒以上かかれば) 待たずに次を投げる。
#define STACKEE_TALK_POLL_MS         1000
// 返答待ちの GET に付ける "?wait=<秒>"。中継 (dev/server/proxy.py) が
// この秒数まで要求を握ったまま上流を見に行くので、TLS の握手が
// 12 回から 1〜2 回に減る。0 にすると従来どおり毎秒ポーリングする。
// ★ 中継側の上限 (MAX_WAIT) と揃えること。
#define STACKEE_TALK_POLL_WAIT_S     25
#define STACKEE_TALK_POLL_TIMEOUT_MS 390000
#define STACKEE_TALK_PLAY_WAIT_MS    15000
// 1 周で I2S から吸い上げるサンプル数 (stackee_talk.py と同じ 240 = 15 ms)。
#define STACKEE_TALK_CHUNK           240

// ---- 短押し / 無音の切り捨て (2026-09-21) ----------------------------------
// ★ **誤って触れただけでは何も起こさない。** キーに指がかすった、ポケットで
//   押された、といったときに一次回答が鳴って送信まで走るのを止める。
//   録音を終えた時点で 2 つとも満たしたときだけ、従来の流れ
//   (一次回答 → 送信) へ進む。満たさなければ**静かに idle へ戻る**
//   (音も出ない・送信もしない・「考え中」の顔にもならない)。
//
//   (a) 録音の長さ ≥ min_ms      … 録音の長さ = キーを押していた長さ
//   (b) 声がある                 … 20 ms の窓のうち RMS が voice_rms 以上の
//                                  ものが voice_windows 個以上
//
// ★★ **「いちばん大きい窓」では判定できない** (2026-09-21 に実測して分かった)。
//   マイクを開けた直後に決まった跳ねがあり、環境音しか無い部屋でも
//   **窓 5 (開けてから 100〜120 ms) の RMS が毎回 3,257〜3,945** になる
//   (6 回測って毎回 窓 5)。窓の平均は 266〜276 しかない。最大だけを見ると、
//   この跳ね 1 つで必ず「声あり」になってしまう。
//   そこで **何個の窓が閾値を越えたか**で見る。跳ねは 1 窓しか無いので
//   越えられない。声は 100 ms も続けば 5 窓ある。
//   (跳ねの出どころは ES7210 の立ち上がり。research/stackee/
//    record_onset_2026-09-21.md)
//
// ★ 判定は**録音バッファ全体**を 1 度なめて行う。将来プリロール (録音の
//   冒頭の取りこぼしを埋める先読み) を足しても、そのぶんを含めて数える。
//   費用は 1 サンプルあたり掛け算 1 回で、走るのは audio タスクの上
//   (打鍵の道には 1 命令も足さない)。
#define STACKEE_TALK_RMS_WINDOW_MS   20
#define STACKEE_TALK_RMS_WINDOW \
    (STACKEE_TALK_RATE * STACKEE_TALK_RMS_WINDOW_MS / 1000)     // 320 サンプル
// 既定。settings.toml の STACKEE_TALK_MIN_MS / STACKEE_TALK_VOICE_RMS で
// 変えられる (README §11-1)。
#define STACKEE_TALK_MIN_MS_DEFAULT      1000
// 環境音の窓の平均が 270 前後。その約 3.7 倍。立ち上がりの跳ね (3,257〜3,945)
// より下だが、跳ねは 1 窓しか無いので下の窓数で落ちる。
#define STACKEE_TALK_VOICE_RMS_DEFAULT   1000
// 越えた窓がいくつ要るか。5 窓 = 100 ms。
#define STACKEE_TALK_VOICE_WINDOWS_DEFAULT 5

#define STACKEE_TALK_PATH_MAX        160
#define STACKEE_TALK_TEXT_MAX        256

// ---- 返答音声の字幕 (scratchpad/subtitle_design.md) ------------------------
// ★ 同期はサーバが決める。本体は「再生位置 (ms) ≥ 開始時刻」の**最後の**
//   ページを出すだけで、推定はしない。
//
//   GET /jobs/<id> の done 応答:
//     "subtitles"     … 本文そのもの (\t \n を JSON で逃がした 1 文字列)。
//                       **これがあれば別 GET をしない**
//     "subtitles_url" … "/jobs/<id>/subtitles" (旧サーバ互換のために残る)
//   GET /jobs/<id>/subtitles … text/plain。1 行 = "<start_ms>\t<text>\n"
//
// ★ 別 GET は**実機で約 8 秒**かかる (要求ごとに TLS を張り直すため。
//   firmware/README.md §23-6 の実測)。本文は 4 KB 以下なので、done の
//   JSON に混ぜてもらえば喋り始めがその 8 秒ぶん早い。本体は
//   「あれば使う、無ければ従来どおり取りに行く」の順で見る。
//
// 旧サーバ (どちらも無し) では字幕なしで従来どおり動く。取得や解析に
// 失敗しても会話は止めない (字幕が出ないだけ)。
#define STACKEE_TALK_SUB_PAGES       48
// 1 ページは 15 桁 (全角 15 字 = 45 バイト)。伸ばさずに切る。
#define STACKEE_TALK_SUB_TEXT_MAX    64
// 本文の上限。通信側にもこの値を渡す (固定の受け皿しか使わない)。
#define STACKEE_TALK_SUB_BYTES       4096
// 逃がしを解くときの中継ぎは **1 行ぶんだけ** (スタック)。4 KB の中継ぎを
// 持たないので、内蔵 RAM の静的な使用量は 1 バイトも増えない。
//   "<10 桁>" + TAB + 本文 (最大 63 B) + 余裕
#define STACKEE_TALK_SUB_LINE_MAX    128
// 帯は 3 行。ページは頭から 1 行ずつ**積む**。3 行が埋まった状態で次の
// ページが来たら帯を空にしてそれを 1 行目に置く (頁めくり)。つまり
// ページ i は必ず帯の i%3 行目に出る (行の集合は i だけで決まる)。
#define STACKEE_TALK_SUB_LINES       3
// 帯へ渡す文字列の上限 (63 B x 3 行 + 改行 2 + NUL = 192)。
#define STACKEE_TALK_SUB_BAND_MAX \
    (STACKEE_TALK_SUB_LINES * STACKEE_TALK_SUB_TEXT_MAX)

// ---- 返答待ちの GET の受け皿 ----------------------------------------------
// ★ done の JSON に字幕の本文 (4 KB) が混ざるので、従来の 8 KB では足りない。
//   x2 は「サーバが ensure_ascii=True に変えても \uXXXX で 2 倍までしか
//   膨らまない」ぶんの余裕 (いまのサーバは ensure_ascii=False)。
//   受け皿は通信側が 1 往復ごとに **PSRAM** へ取って閉じるときに返すので、
//   常駐の使用量は増えない (stackee_http.c)。
#define STACKEE_TALK_POLL_LIMIT      (8192 + 2 * STACKEE_TALK_SUB_BYTES)

// 字幕の出どころ。talk.status の "sub_src" に名前で出る。
typedef enum {
    STACKEE_TALK_SUB_NONE = 0,  // 字幕なし (旧サーバ / 取れなかった / 解けなかった)
    STACKEE_TALK_SUB_INLINE,    // done の JSON の "subtitles" (別 GET なし)
    STACKEE_TALK_SUB_URL,       // "subtitles_url" を GET した
    STACKEE_TALK_SUB_SRCS,
} stackee_talk_sub_src_t;

extern const char *const stackee_talk_sub_src_names[STACKEE_TALK_SUB_SRCS];

// 秒 → バイトの換算を 1 か所に閉じ込めるための念押し (16 kHz / mono / 16 bit)。
// ここを動かすときは README §11-4 の表も一緒に直すこと。
_Static_assert(STACKEE_TALK_REPLY_MAX_BYTES == 3840000u,
               "返答の受け皿は 120 秒ぶん = 16000 * 2 * 120 バイト");
_Static_assert((size_t)STACKEE_TALK_MAX_SAMPLES * 2 == 960000u,
               "録音の上限は 30 秒ぶん = 16000 * 2 * 30 バイト");

typedef enum {
    STACKEE_TALK_IDLE = 0,
    STACKEE_TALK_RECORDING,
    STACKEE_TALK_UPLOAD,
    STACKEE_TALK_POLL_WAIT,
    STACKEE_TALK_POLL,
    STACKEE_TALK_SUBS,      // 字幕を取りに行っている (subtitles_url があるときだけ)
    STACKEE_TALK_AUDIO,
    STACKEE_TALK_PLAY_WAIT,
    STACKEE_TALK_PLAYING,
    STACKEE_TALK_STATES,
} stackee_talk_state_t;

extern const char *const stackee_talk_state_names[STACKEE_TALK_STATES];

typedef struct {
    uint32_t (*now_ms)(void);

    // ---- 録音 ----
    // WAV ヘッダ (44 B) を前に置ける領域つきで確保する。返るのは PCM の先頭。
    int16_t *(*record_alloc)(int max_samples);
    void     (*record_release)(void);
    bool     (*record_begin)(void);         // 半二重を取り、マイクを開く
    int      (*record_read)(int16_t *dst, int max);   // 取れた数 (<0 で失敗)
    void     (*record_end)(void);           // マイクを戻す (必ず呼ばれる)

    // ---- 再生 ----
    bool (*ack_begin)(void);                // 一次回答。鳴らさないなら false
    bool (*ack_active)(void);
    bool (*play_begin)(const int16_t *pcm, int samples);
    bool (*play_active)(void);
    bool (*play_failed)(void);

    // ---- 通信 ----
    bool (*net_ready)(void);
    // body は http_close() まで呼び手が持ち続ける (SM は解放しない)。
    bool (*http_start)(const char *method, const char *path,
                       const void *body, size_t body_len, size_t limit);
    // 0 = 進行中 / 1 = 完了 / -1 = 失敗
    int  (*http_poll)(int *status, const uint8_t **body, size_t *len);
    void (*http_close)(void);
    // WAV ヘッダを PCM の直前 (44 バイト前) に置く。
    void (*wav_header)(int16_t *pcm, int samples);

    // ---- 外の世界 ----
    void (*show)(const char *text);         // status.screen と画面の 1 行
    void (*log)(const char *line);
    void (*face)(bool recording, bool busy, bool speaking);
    // 字幕の帯に出す 1 行。NULL / 空で帯を消す。無くてもよい (NULL 可)。
    // ★ ページが変わった時にだけ呼ぶ (毎周は呼ばない)。
    void (*subtitle)(const char *text);
} stackee_talk_ops_t;

typedef struct {
    uint32_t start_ms;
    char     text[STACKEE_TALK_SUB_TEXT_MAX];
} stackee_talk_page_t;

typedef struct {
    const stackee_talk_ops_t *ops;
    int      state;
    bool     pressed;
    bool     was_pressed;

    int16_t *samples;
    int      count;

    char     path[STACKEE_TALK_PATH_MAX];   // POST 先 ("/talk")
    char     job[STACKEE_TALK_PATH_MAX];    // status_url
    char     audio_path[STACKEE_TALK_PATH_MAX];
    char     subs_path[STACKEE_TALK_PATH_MAX];  // 空なら字幕なし (旧サーバ)
    char     reply[STACKEE_TALK_TEXT_MAX];
    char     screen[STACKEE_TALK_TEXT_MAX];
    char     error[STACKEE_TALK_TEXT_MAX];

    uint32_t since;             // 今の状態に入った時刻
    uint32_t polled;            // 最後の GET /jobs が返ってきた時刻
    uint32_t poll_sent;         // 最後の GET /jobs を投げた時刻
    uint32_t poll_took;         // その 1 往復にかかった時間 [ms]
    uint32_t poll_started;      // 受理された時刻 (390 秒の起点)
    uint32_t turn_started;
    bool     turn_valid;
    int      polls;             // この往復で GET /jobs を撃った回数

    // [talk-turn-timing] に出す累積 [ms]
    uint32_t accepted_ms, reply_ready_ms, audio_ready_ms;
    uint32_t play_setup_ms, complete_ms;
    int      audio_duration_ms;
    int      audio_samples;

    const uint8_t *audio;
    size_t   audio_len;
    bool     http_open;         // http_close() をまだ呼んでいない

    // ---- 字幕 ----
    // ★ 固定の入れ物しか使わない (再生中に malloc しない)。
    stackee_talk_page_t pages[STACKEE_TALK_SUB_PAGES];
    int      page_count;
    int      page_shown;        // いま帯に出ているページ (-1 = 何も出していない)
    uint32_t sub_bytes;         // 読んだ本文の長さ [B] (逃がしを解いたあと)
    int      sub_dropped;       // 捨てた行の数 (不正 + 上限超え)
    int      sub_src;           // stackee_talk_sub_src_t
    uint32_t turns, errors, ignored, subs_ok, subs_failed;
    // 短押し / 無音で捨てた回数と、最後の録音の測り値 (talk.status に出る)。
    uint32_t dropped_short, dropped_silent;
    uint32_t last_rec_ms, last_rms_max;
    int      last_rms_at;       // 最大だった窓の番号 (-1 = 測れなかった)
    uint32_t last_rms_mean;     // 窓ごとの RMS の平均
    // 押下 (録音を頼んだ瞬間) → 最初のサンプルが手に入るまで [ms]。
    // ★ research/stackee/record_onset_2026-09-21.md の ①〜⑩ をまとめた数字。
    //   ここが大きいと録音の頭が欠ける。
    uint32_t rec_request;       // 録音を頼んだ時刻
    uint32_t first_sample_ms;
    uint32_t last_rms_2nd;      // 2 番目に大きい窓 (立ち上がりの跳ねを除く目安)
    uint32_t last_loud;         // voice_rms を越えた窓の数
    // 切り捨ての閾値 (0 = その条件を見ない)。
    uint32_t min_ms, voice_rms, voice_windows;
} stackee_talk_t;

// 再生位置 [ms] に出すページの番号。無ければ -1。
// ★ 「start_ms ≤ ms」の**最後の**ページ。推定も補間もしない。
int stackee_talk_page_at(const stackee_talk_t *t, uint32_t ms);

// page 番のページを出すときに帯へ渡す文字列 (改行区切り、最大 4 行)。
// ★ 行の集合は page だけで決まる: 同じ頁の先頭 (page - page%3) から page まで。
//   page < 0 なら空文字列。戻り値は並べた行数。
int stackee_talk_band(const stackee_talk_t *t, int page, char *out, size_t cap);

// "<start_ms>\t<text>\n" の並びを読む。戻り値は採ったページ数。
// 不正な行 (タブ無し・数字でない・本文が空) は黙って捨てる。
// text は NUL 終端でなくてよい (len で切る)。
int stackee_talk_parse_subtitles(stackee_talk_t *t, const char *text, size_t len);

// 同じものを **JSON の文字列値のまま** 読む (done の "subtitles")。
// at は引用符を含む値の先頭、len はその長さ (stackee_json_raw が返す形)。
// ★ 4 KB の中継ぎを持たない。逃がしを解きながら 1 行ずつ処理する。
int stackee_talk_parse_subtitles_json(stackee_talk_t *t, const char *at, size_t len);

void stackee_talk_init(stackee_talk_t *t, const stackee_talk_ops_t *ops,
                       const char *post_path);

// 短押し / 無音の切り捨ての閾値を差し替える (settings.toml から)。
// 0 を渡すとその条件を見ない。呼ぶのは立ち上げのときだけ。
void stackee_talk_set_gate(stackee_talk_t *t, uint32_t min_ms,
                           uint32_t voice_rms, uint32_t voice_windows);

// 録音を 20 ms の窓に切って測った結果。
typedef struct {
    uint32_t max;           // いちばん大きい窓の RMS
    int      max_at;        // その窓の番号 (-1 = 窓 1 つぶんも無かった)
    uint32_t second;        // 2 番目に大きい窓 (跳ねを除いた目安)
    uint32_t mean;          // 窓ごとの RMS の平均
    uint32_t loud;          // threshold を越えた窓の数
    int      windows;       // 数えた窓の数
} stackee_talk_voice_t;

// ★ 純粋な関数。バッファ全体をなめるので、プリロールを足しても
//   そのぶんを含めて数える。端数の窓 (最後の 20 ms 未満) は数えない
//   — 短すぎる窓は RMS が跳ねやすく、判定が甘くなるため。
void stackee_talk_voice_scan(const int16_t *pcm, int count,
                             uint32_t threshold, stackee_talk_voice_t *out);

// STK_TALK の押し離し。実際の仕事は次の step() で起きる。
void stackee_talk_set_pressed(stackee_talk_t *t, bool pressed);

// 録音の代わりに手持ちの PCM を送る (console の talk.inject)。
// 始められたら true。非同期 — 進み具合は talk.status で見る。
bool stackee_talk_inject(stackee_talk_t *t, const int16_t *pcm, int samples);

// 1 周ぶん。呼ぶのは audio タスクだけ。
void stackee_talk_step(stackee_talk_t *t);

bool stackee_talk_busy(const stackee_talk_t *t);

// STACKEE_TALK_URL を「相手」と「パス」に割る。
//   "https://pi400.example.ts.net:8443/talk"
//     -> base "https://pi400.example.ts.net:8443" / path "/talk"
// 検査は stackee_talk.py の parse_url と同じ範囲 (scheme / 空のホスト /
// ポートの範囲 / URL に混ざってはいけない文字)。
bool stackee_talk_split_url(const char *url, char *base, size_t bcap,
                            char *path, size_t pcap);
