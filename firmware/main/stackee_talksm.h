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
// 画像を見せる (2026-09-26、STK_CAMERA / console の camera.look):
//   POST /look (image/jpeg)     → 202 {"id":..,"status_url":"/jobs/<id>"}
//   以降は /talk と**完全に同じ** (ポーリング・done・/audio・字幕)。
//   送り先は STACKEE_TALK_URL の末尾 "/talk" を "/look" にしたもの。
//   受理より後ろは会話の道をそのまま歩く (二重に実装しない)。
//
// stackee 独自キー Custom_0〜Custom_9 (2026-09-27、key.custom):
//   GET  /inbox                 → {"state":"empty","seq":N}  (最後に見た seq の初期値)
//   POST /key (application/json) {"key":"Custom_3"}
//                               → 200 {"state":"ignored"}      未設定 (音なし)
//                               → 202 {id,status_url,mode:"prompt"}  /look と同じ後半
//                               → 202 {id,status_url,mode:"command"} 受け箱を回す
//   GET  /inbox?after=<seq>&wait=25&job=<id>
//                               → {"state":"say",seq,reply,audio_url?,...}  発話 1 件
//                               → {"state":"empty",seq,job_state?}
//   送り先は /look と同じく STACKEE_TALK_URL の末尾 "/talk" を置き換えたもの。
//   ★ 受け箱と発話の再生 (inbox_*) はキー押下に縛られない形にしてある
//     (第 2 段で「暇なときに常に回す」ときに同じ部品を使う)。
//
// 暇なときに受け箱を見る (常時ポーリング、2026-09-27、inbox.status):
//   会話・写真・Custom のどれもしておらず、Wi-Fi が上がっている間
//   GET /inbox (起動時・再接続時。最新 seq を得るだけで、それより前は鳴らさない)
//   GET /inbox?after=<seq>&wait=25   を回し続ける。"say" が来たら
//   Custom のコマンドと同じ部品で 1 件鳴らす / 字幕を出す。
//   ★ 待っている間は状態機械は idle のまま (busy ではない)。キーは最優先で、
//     押した瞬間に待ちを打ち切る (通信側がソケットを shutdown して起こす)。
//     断るのは発話を扱っている間だけ。
//
// クリップ (2026-09-30、STK_CLIP / clips.play、README §17-2f):
//   GET /clips と GET /clips/<id>/audio で暇なときに FAT へ取り込み
//   (stackee_clipsm.c)、キーを押したら**通信せずに** FAT から 1 件読んで、
//   会話と同じ再生部品 (PLAY_WAIT → PLAYING / 字幕だけなら SAY_TEXT) で鳴らす。
//   ★ 同期は受け箱の常時ポーリングと同じ通信 (直列)。キー押下で打ち切る。
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

struct stackee_clip;        // stackee_clipsm.h

// ---- クリップ (2026-09-30) --------------------------------------------------
// 帯に出すお知らせ (音なし、STACKEE_TALK_NOTICE_MS)。
#define STACKEE_TALK_CLIP_EMPTY      "クリップがありません"
#define STACKEE_TALK_CLIP_UNREADABLE "クリップを読めません"
// FAT から読み終えるまでの上限 [ms] (3.84 MB でも 1 秒かからない見込み)。
#define STACKEE_TALK_CLIP_LOAD_MS    10000
#define STACKEE_TALK_CLIP_AUTO_ON    "クリップ自動取得 ON"
#define STACKEE_TALK_CLIP_AUTO_OFF   "クリップ自動取得 OFF"
#define STACKEE_TALK_CLIP_AUTO_FORCED "クリップ自動取得は\n設定で OFF です"
#define STACKEE_TALK_TEXT_MAX        256

// ---- 画像を見せる (POST /look、2026-09-26) ---------------------------------
// ★ サーバとの取り決め: image/jpeg / Content-Length 付き / 上限 512 KiB。
//   応答は /talk と同じ 202 {id, status_url}。409 は会話と共有 (処理中)。
#define STACKEE_TALK_LOOK_MAX_BYTES  (512u * 1024u)
#define STACKEE_TALK_CTYPE_WAV       "audio/wav"
#define STACKEE_TALK_CTYPE_JPEG      "image/jpeg"

// stackee_talk_look_reserve() の答え。
typedef enum {
    STACKEE_TALK_LOOK_OK = 0,   // 押さえた。撮ってよい
    STACKEE_TALK_LOOK_BUSY,     // 会話中 / 再生中 / STK_TALK 押下中。黙って無視する
    STACKEE_TALK_LOOK_ERROR,    // 送れない (URL 未設定・Wi-Fi なし)。画面に出した
} stackee_talk_look_reserve_t;

// ---- stackee 独自キー Custom_0〜Custom_9 (POST /key + 受け箱、2026-09-27) ------
// ★ サーバとの取り決め (変えないこと):
//   - 送り先は STACKEE_TALK_URL の末尾 "/talk" を "/key" / "/inbox" にしたもの
//     (/look と同じ規則。Bearer と HTTPS も同じ)。
//   - 押したら POST の**前に** GET /inbox で「最後に見た seq」を得る。
//     覚えておいて使い回す (TLS の握手が 1 回増えるため)。ただし
//     STACKEE_TALK_INBOX_SEQ_TTL_MS より古ければ取り直す (その間に溜まった
//     古い発話を鳴らさないため)。
//   - 409 は会話と同じ「サーバーが処理中です」。
#define STACKEE_TALK_CUSTOM_COUNT      10
#define STACKEE_TALK_CTYPE_JSON      "application/json"
// 受け箱のロングポーリング。★ 中継側の上限 (MAX_WAIT) と揃えること。
#define STACKEE_TALK_INBOX_WAIT_S    25
// コマンド方式の全体の上限 [ms]。サーバのコマンドの時間切れ (最大 600 秒)
// + 余裕 60 秒。起点はコマンドを受理した時刻。
#define STACKEE_TALK_CUSTOM_TIMEOUT_MS 660000
#define STACKEE_TALK_INBOX_SEQ_TTL_MS 300000
// 画面の帯に「Custom_3 未設定」やエラーを出しておく時間 [ms]。
#define STACKEE_TALK_NOTICE_MS       2500
// 音の無い発話 (字幕だけ) で、最後のページを出しておく時間 [ms]。
#define STACKEE_TALK_SAY_HOLD_MS     3000
// 字幕の本文が無く、返答文を自分で割って出すときの 1 頁 (3 行) の時間 [ms]。
#define STACKEE_TALK_SAY_PAGE_MS     3000
// 帯の 1 行の字数 (サーバの割り方と同じ 15 桁)。
#define STACKEE_TALK_BAND_COLS       15
// key.custom_status に残す発話の記録 (先頭からこの数まで。数は says に全部)。
#define STACKEE_TALK_SAY_LOG         8
#define STACKEE_TALK_JOB_ID_MAX      64

// ---- 暇なときに受け箱を見る (常時ポーリング、2026-09-27) --------------------
// 失敗したら 5 秒 → 10 → 20 → 40 → 60 秒 (上限) あけて聞き直す。
#define STACKEE_TALK_WATCH_BACKOFF_MIN_MS 5000
#define STACKEE_TALK_WATCH_BACKOFF_MAX_MS 60000
// 中継が /inbox を知らない (404)。10 分は聞かない (静かに休む)。
#define STACKEE_TALK_WATCH_404_MS    600000
// ロングポーリングが効いていない (1 秒未満で空が返る) ときの間合い。
#define STACKEE_TALK_WATCH_QUICK_MS  5000

typedef enum {
    STACKEE_TALK_WATCH_OFF = 0, // 無効 (inbox.enable {"on":0})
    STACKEE_TALK_WATCH_WAIT,    // 撃てる時を待っている (暇でない・間合い・休み)
    STACKEE_TALK_WATCH_SEQ,     // GET /inbox (最新 seq を得る)
    STACKEE_TALK_WATCH_POLL,    // GET /inbox?after=
    STACKEE_TALK_WATCH_SAY,     // 受けた発話を扱っている (この間は busy)
    STACKEE_TALK_WATCH_SLEEP,   // 中継が古い (404)。しばらく聞かない
    STACKEE_TALK_WATCH_PHASES,
} stackee_talk_watch_phase_t;

extern const char *const stackee_talk_watch_phase_names[STACKEE_TALK_WATCH_PHASES];

typedef enum {
    STACKEE_TALK_CUSTOM_MODE_NONE = 0,    // まだ分からない (受理の前)
    STACKEE_TALK_CUSTOM_MODE_PROMPT,      // /look と同じ後半 (返答待ち → 音声)
    STACKEE_TALK_CUSTOM_MODE_COMMAND,     // 受け箱を回す
    STACKEE_TALK_CUSTOM_MODES,
} stackee_talk_custom_mode_t;

extern const char *const stackee_talk_custom_mode_names[STACKEE_TALK_CUSTOM_MODES];

// 受け取った発話 1 件の記録 (key.custom_status)。
typedef struct {
    uint32_t seq;
    uint32_t at_ms;         // 押下からこの発話を受け取るまで
    uint32_t sub_bytes;     // 字幕の本文の長さ (逃がしを解いたあと)
    int      sub_pages;
    uint32_t audio_bytes;   // サーバの申告 (audio_bytes。無ければ 0)
    uint32_t audio_got;     // 実際に受け取った PCM のバイト数
    uint32_t reply_len;
    bool     audio;         // audio_url があった
    bool     played;        // 鳴らした (play=0 / 字幕だけなら false)
} stackee_talk_say_t;

// ---- 案内の字幕 (2026-09-22) ----------------------------------------------
// ★ 帯が黒いだけだと「いま何をすればいいか」が分からない。会話の状態を
//   そのまま言葉にして出す。
//
//   録音中 (STK_TALK を押している間) … 「マイクに向かって話しかけてください」
//   考え中 (送信〜返答待ち)          … 「考えています…」
//
// ★ 行の割り方はサーバと同じ規則 (1 行 15 桁、句読点で行を始めない)。
//   tools/ack_lines.py に文を渡して割った結果をそのまま書いてある
//   (ホストテストが突き合わせる)。
// ★ **帯の持ち主は 1 人**。一次回答 (ack) の字幕が出ている間と、返答の字幕が
//   出ている間は案内を出さない (鳴り終わってから / 来たら置き換わる)。
// ★ **MIC(kc) の押下 (PC 側のプッシュトゥトーク) では出さない。** あれは
//   顔だけを変えるもので、会話の状態機械を通らない。
#define STACKEE_TALK_GUIDE_RECORDING "マイクに向かって\n話しかけてください"
#define STACKEE_TALK_GUIDE_THINKING  "考えています…"

// 案内の状態 (talk.status の "guide")。
typedef enum {
    STACKEE_TALK_GUIDE_NONE = 0,    // 帯に案内は出していない
    STACKEE_TALK_GUIDE_REC,         // 録音中の案内を出している
    STACKEE_TALK_GUIDE_THINK,       // 考え中の案内を出している
    STACKEE_TALK_GUIDE_HELD,        // ほかの持ち主 (ack / 返答) がいる。触らない
} stackee_talk_guide_t;

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
    // ---- Custom と受け箱 (2026-09-27)。★ 番号を動かさないよううしろに足す ----
    STACKEE_TALK_INBOX_SEQ, // GET /inbox (最後に見た seq の初期値を取る)
    STACKEE_TALK_KEY,       // POST /key の応答待ち
    STACKEE_TALK_INBOX_WAIT,// 次の GET /inbox?after= を撃つまで
    STACKEE_TALK_INBOX,     // GET /inbox?after=&wait= の応答待ち
    STACKEE_TALK_SAY_TEXT,  // 音の無い発話の字幕を出している
    // ---- クリップ (2026-09-30)。★ 番号を動かさないよううしろに足す ----
    STACKEE_TALK_CLIP_LOAD, // FAT からクリップを読んでいる (通信しない)
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
    // content_type は POST のときだけ意味を持つ (GET では NULL)。
    bool (*http_start)(const char *method, const char *path,
                       const void *body, size_t body_len, size_t limit,
                       const char *content_type);
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

    // ---- 常時ポーリング (2026-09-27)。どちらも NULL 可 ----
    // 使い回せる接続があるか。無ければ最初の 1 本は待たない要求にする
    // (張っている最中にキーで打ち切られても、その接続を次の要求に回せる)。
    // NULL なら「ある」とみなす。
    bool (*http_warm)(void);
    // 外の都合で受け箱を回してはいけないとき false (OTA の書き込み中・
    // マイクの自己診断中など)。NULL なら常に true。
    bool (*watch_ok)(void);
    // 接続が無ければ、返事を捨てる短い GET (path) で先に張っておく。会話キーの
    // 録音中に TLS の握手を済ませるため。NULL 可。
    bool (*http_prewarm)(const char *path);
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
    char     look_path[STACKEE_TALK_PATH_MAX];  // 画像の POST 先 ("/look")。空 = 作れない
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

    // ---- 画像を見せる (POST /look) ----
    // ★ look_reserved は「撮影に入るので会話キーを受け付けない」印。
    //   撮影 (約 4 秒) の間は状態機械は idle のままなので、この印で
    //   STK_TALK と talk.inject を断る。stackee_talk_look() が (成否に
    //   かかわらず) 下ろす。撮れなかったときは look_release() で下ろす。
    bool     look_reserved;
    bool     look_notice;       // reserve で断った理由を、次の step で画面に出す
    bool     look;              // いまの / 直近の往復は画像 (POST /look)
    bool     look_play;         // false = 再生の直前で止める (検証用、音を出さない)
    uint32_t look_bytes;        // 送った JPEG の長さ
    uint32_t looks;             // 画像の往復を始めた回数
    uint32_t looks_done;        // そのうち最後まで行った回数 (鳴らさず止めたものを含む)
    uint32_t looks_unplayed;    // play=0 で再生の直前に止めた回数
    // 直近の画像の往復の結末。NULL = 途中 (またはまだ無い) / "done" /
    // "error" (サーバーの state:error・通信の失敗など) / "ignored"。
    // ★ 2026-09-27: サーバーが state:error を返したとき、どこにも結末が
    //   残らず camera.look_status が submitted のまま止まって見えた。
    const char *look_result;
    uint32_t looks_failed;      // error / ignored で終わった回数
    int      reply_len;         // 返答文の長さ [B] (t->reply に入ったぶん)

    // 案内の字幕。文面は差し替えられるようにしておく (将来 settings から)。
    const char *guide_rec, *guide_think;
    int guide_shown;            // stackee_talk_guide_t

    // ---- Custom_0〜Custom_9 (POST /key、2026-09-27) ----
    char     key_path[STACKEE_TALK_PATH_MAX];   // "/key"。空 = 作れない
    char     inbox_path[STACKEE_TALK_PATH_MAX]; // "/inbox"。空 = 作れない
    char     key_body[32];      // {"key":"Custom_3"}。http_close まで生かす
    bool     custom;              // いまの / 直近の往復は Custom
    bool     custom_active;       // Custom の流れの途中 (押下 → 終わり)
    bool     custom_play;         // false = 鳴らす直前で止める (検証用)
    int      custom_n;
    int      custom_mode;         // stackee_talk_custom_mode_t
    char     custom_job[STACKEE_TALK_JOB_ID_MAX];  // コマンドの id (受け箱の job=)
    const char *custom_final;     // NULL (途中) / "done" / "ignored" / "error"
    char     custom_job_state[16];// 受け箱が最後に返した job_state
    uint32_t custom_started;      // 押下を受けた時刻
    uint32_t custom_cmd_started;  // コマンドを受理した時刻 (全体の上限の起点)
    uint32_t custom_seq_ms;       // 押下 → GET /inbox (seq) の応答 (使い回したら 0)
    uint32_t custom_key_ms;       // 押下 → POST /key の応答
    uint32_t custom_first_say_ms; // 押下 → 最初の発話
    uint32_t custom_end_ms;       // 押下 → 終わり
    bool     custom_seq_reused;   // 覚えていた seq を使った
    uint32_t custom_count, custom_done, custom_ignored, custom_errors;
    uint32_t custom_busy;         // 処理中に押されて黙って無視した回数

    // ---- 受け箱 (キー押下に縛られない部品。第 2 段の常時ポーリングでも使う) ----
    bool     inbox_loop;        // 受け箱を回している
    uint32_t inbox_seq;         // 最後に見た seq
    bool     inbox_seq_valid;
    uint32_t inbox_seq_at;      // その seq を知った時刻
    int      inbox_polls;       // この流れで GET /inbox?after= を撃った回数
    uint32_t says;              // この流れで受け取った発話の数
    int      say_cur;           // いま扱っている発話の記録の番号 (-1 = 記録外)
    bool     say_play;          // いまの発話を鳴らしてよいか
    uint32_t say_until;         // 字幕だけの発話を出し終える時刻 (since からの ms)
    stackee_talk_say_t say_log[STACKEE_TALK_SAY_LOG];

    // ---- 帯に短く出すお知らせ (Custom の「未設定」とエラー) ----
    bool     notice_on;
    uint32_t notice_since;
    char     notice[STACKEE_TALK_SUB_BAND_MAX];

    // ---- 暇なときに受け箱を見る (常時ポーリング、2026-09-27) ----
    // ★ 待っているあいだ state は IDLE のまま。通信は watch_http で持つ
    //   (http_open は会話の往復のもの)。発話を受けたら状態機械に渡す
    //   (watch_say)。その間だけ busy。
    bool     watch_on;          // 有効 (既定は audio が立てる。NVS には残さない)
    bool     watch_play;        // 受けた発話を鳴らす (false = 数えるだけ、検証用)
    // ★ 設定メニューを開いている間は受け箱を見ない (2026-10-01)。待っている要求は
    //   打ち切り、閉じたら seq の続きから聞く (発話はサーバに 5 分・16 件残る)。
    bool     watch_held;
    uint32_t watch_holds;       // 保留した回数
    int      watch_phase;       // stackee_talk_watch_phase_t
    bool     watch_http;        // 受け箱の要求を持っている
    bool     watch_long;        // それは wait= 付き (ロングポーリング)
    bool     watch_seq_known;   // 最新 seq を知っている (起動・再接続で false)
    bool     watch_say;         // いま扱っている発話は常時ポーリングから
    bool     watch_net;         // 前の周で Wi-Fi が上がっていたか
    uint32_t watch_next;        // 次に撃ってよい時刻
    uint32_t watch_backoff;     // 失敗の間合い [ms] (0 = 失敗していない)
    uint32_t watch_sent;        // いまの要求を撃った時刻
    uint32_t watch_polls;       // 撃った数 (seq を含む)
    uint32_t watch_received;    // 受けた発話の数
    uint32_t watch_played;      // そのうち鳴らした / 字幕を出した数
    uint32_t watch_aborts;      // キーなどで待ちを打ち切った数
    uint32_t watch_fails;       // 失敗の数 (通信・応答の不正・発話の扱い)
    uint32_t watch_skipped;     // 見たことのある seq で飛ばした数
    int      watch_status;      // 直近の HTTP の status
    char     watch_error[96];   // 直近のエラー
    // ★ 常時ポーリングの発話は reply / 音の長さ / 各段の時刻を借りて使う。
    //   talk.status と camera.look_status は「直近にユーザーが頼んだ往復」を
    //   読むので、発話を扱う前に写しておき、扱い終えたら戻す。
    struct {
        char     reply[STACKEE_TALK_TEXT_MAX];
        int      reply_len;
        int      audio_samples, audio_duration_ms;
        uint32_t turn_started;
        uint32_t reply_ready_ms, audio_ready_ms, play_setup_ms;
    } watch_keep;

    // ---- クリップ (2026-09-30) ----
    // ★ 同期と目録は clip (stackee_clipsm.c) が持つ。NULL ならクリップは無い。
    //   ここは「押したら 1 件鳴らす」の流れだけ。借りる値 (reply など) は
    //   常時ポーリングの発話と同じく watch_keep に写しておき、終えたら戻す。
    struct stackee_clip *clip;
    bool     clip_active;       // クリップの流れの途中 (押下 → 鳴り終わり)
    bool     clip_play;         // false = 鳴らす直前で止める (検証用、字幕も出さない)
    char    *clip_meta;         // 読んだメタ (持ち主はこちら。cleanup で返す)
    uint8_t *clip_pcm;          // 読んだ PCM (同上。t->audio はここを指す)
    char     clip_id[48];       // いま / 直近のクリップの id
    uint32_t clip_started;      // 押下の時刻
    uint32_t clip_load_ms;      // 押下 → 読み終え
    uint32_t clip_end_ms;       // 押下 → 終わり
    int      clip_pages;        // 字幕のページ数
    uint32_t clip_audio_bytes;
    const char *clip_final;     // NULL (途中) / "done" / "empty" / "error"
    char     clip_error[96];    // 直近のクリップの失敗 (会話の error とは別)
    bool     clip_played;       // 鳴らした / 字幕を出した
    uint32_t clip_count;        // 押下 (受け付けたもの) の数
    uint32_t clip_done, clip_empty, clip_errors, clip_busy, clip_unplayed;
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

// 案内の文面を差し替える。NULL を渡したほうは既定のまま。
void stackee_talk_set_guides(stackee_talk_t *t, const char *recording,
                             const char *thinking);

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

// 会話中か。★ 画像のために押さえている間 (look_reserved) も busy とみなす。
bool stackee_talk_busy(const stackee_talk_t *t);

// 状態機械がマイク / スピーカーを使いうるか (USB マイクに明け渡すかの判断)。
// ★ busy とは別。Custom のコマンドを待っている間 (受け箱のロングポーリング、
//   最大 10 分) は busy だが、音は使わないので USB マイクは止めない。
bool stackee_talk_uses_audio(const stackee_talk_t *t);

// ---- 画像を見せる (POST /look) ---------------------------------------------
// STACKEE_TALK_URL のパスの末尾 "/talk" を "/look" に置き換える。
//   "/talk" → "/look" / "/api/talk" → "/api/look"
// 末尾が "/talk" でなければ false ("/talkx" "/xtalk" "/talk?x=1" も false)。
bool stackee_talk_look_path(const char *talk_path, char *out, size_t cap);

// 撮影の前に呼ぶ。OK なら会話キーと talk.inject を断るようになる。
// ★ 会話中なら BUSY を返すだけで何もしない (撮らずに無視する約束)。
// ★ URL 未設定 / Wi-Fi なしなら ERROR。エラー欄に入れ、画面 (show) には
//   **次の step で** 出す (撮っても送れないので撮らない)。ここは I2S にも
//   画面にもログにも触らないので、audio タスク以外 (camera タスク、
//   スタック 4 KB) から錠を取って呼んでよい。
int  stackee_talk_look_reserve(stackee_talk_t *t);
// 撮れなかったときに押さえを外す。
void stackee_talk_look_release(stackee_talk_t *t);
// 撮れた JPEG を送り始める。中身は record_alloc した領域へ**写す**ので、
// 戻ったあと呼び手のバッファは自由に使ってよい。受理 (202) の時点で返す。
// play=false なら返答の PCM を受け取ったところで止め、鳴らさない
// (一次回答も鳴らさない)。始められたら true。呼ぶのは audio タスクだけ
// (一次回答の再生を始めるため)。
bool stackee_talk_look(stackee_talk_t *t, const uint8_t *jpeg, size_t len,
                       bool play);

// STACKEE_TALK_URL のパスの末尾 "/talk" を "/<name>" に置き換える
// (look_path の一般形。"/key" "/inbox" もこれで作る)。
bool stackee_talk_sibling_path(const char *talk_path, const char *name,
                               char *out, size_t cap);

// ---- stackee 独自キー Custom_0〜Custom_9 -----------------------------------------
// 押下 1 回ぶんの流れを始める (GET /inbox → POST /key → …)。呼ぶのは
// audio タスクだけ。n は 0..9。play=false なら発話・返答の PCM を受け取った
// ところで止めて鳴らさない (検証用。一次回答も鳴らさない)。
// ★ 会話・画像・他の Custom の途中 (録音/送信/待ち/再生、一次回答が鳴っている、
//   STK_TALK を押している、撮影のために押さえている) なら**何もせず** false
//   (custom_busy が増える。画面にも何も出さない)。
// ★ URL 未設定や Wi-Fi なしで始められないときは、会話と同じ「会話エラー: …」を
//   出して true を返す (流れとしては始まって、すぐ error で終わった)。
bool stackee_talk_custom(stackee_talk_t *t, int n, bool play);

// ---- クリップ (2026-09-30) ----------------------------------------------------
// 同期と目録 (stackee_clipsm.c) をつなぐ。以後 stackee_talk_step が同期を回す
// (受け箱の常時ポーリングと直列、暇なときだけ)。NULL で外す。
void stackee_talk_attach_clip(stackee_talk_t *t, struct stackee_clip *clip);

// STK_CLIP を押したのと同じ流れ。呼ぶのは audio タスクだけ。**通信しない。**
// ★ 会話・画像・Custom の途中 (録音/送信/待ち/再生、一次回答が鳴っている、
//   STK_TALK を押している、撮影のために押さえている)・クリップを鳴らしている
//   最中は**何もせず** false (clip_busy が増える。画面にも何も出さない)。
// ★ 1 件も無ければ帯に「クリップがありません」を 2.5 秒 (音なし) 出して true。
// ★ play=false は読み終えたところで止め、鳴らさない (字幕も出さない。検証用)。
bool stackee_talk_clip(stackee_talk_t *t, bool play);

// STK_CLIP_AUTO — 自動取得 (5 分ごとの同期) を入り切りする。呼ぶのは audio タスク。
// 帯に「クリップ自動取得 ON / OFF」を 2.5 秒 (音なし。帯の持ち主が居るとき =
// 会話・再生の途中は出さずに切り替えだけ)。settings.toml で強制 OFF なら
// 切り替えずに「…は設定で OFF です」。戻り値は切り替えたあとの状態 (1 / 0)、
// クリップが無ければ -1。
int  stackee_talk_clip_auto(stackee_talk_t *t);

// ---- 暇なときに受け箱を見る (常時ポーリング) ---------------------------------
// on / play を切り替える (inbox.enable)。off にすると待っている要求を打ち切る。
// 呼ぶのは錠を持った console か audio タスク。
void stackee_talk_watch_enable(stackee_talk_t *t, bool on, bool play);

// 設定メニューを開いている間は受け箱を保留する (待っている要求は打ち切る)。
// 呼ぶのは audio タスク (錠の中)。
void stackee_talk_watch_hold(stackee_talk_t *t, bool held);

// 発話の扱いを含めて「音を使っている」か (stackee_audio_busy に使う)。
// ★ 常時ポーリングの待ちと、受けた発話の PCM を取っている間は false。
//   鳴らしている (PLAY_WAIT / PLAYING) 間だけ true。会話・画像・Custom は
//   従来どおり busy と同じ答え。
bool stackee_talk_audio_busy(const stackee_talk_t *t);

// 字幕の帯に出すため、UTF-8 の文を 1 行 cols 字 (コードポイント数) で割る。
// 最大 lines 行。入らない残りは捨てる。戻り値は行数。
int stackee_talk_wrap(const char *text, int cols, int lines, char *out, size_t cap);

// STACKEE_TALK_URL を「相手」と「パス」に割る。
//   "https://pi400.example.ts.net:8443/talk"
//     -> base "https://pi400.example.ts.net:8443" / path "/talk"
// 検査は stackee_talk.py の parse_url と同じ範囲 (scheme / 空のホスト /
// ポートの範囲 / URL に混ざってはいけない文字)。
bool stackee_talk_split_url(const char *url, char *base, size_t bcap,
                            char *path, size_t pcap);
