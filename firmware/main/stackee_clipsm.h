// クリップ — サーバーが先に合成しておいた「音声 + 字幕」の短い発話を、
// 暇なときに本体の FAT (user_fs) へ取り込み、専用キー (STK_CLIP) で
// **通信せずに** 1 件ずつ鳴らす (2026-09-30、README §17-2f)。
//
// ここは ESP-IDF に依存しない「決め事」の部分:
//   * 同期の状態機械 (GET /clips → 無いものを消す → 新しい順に取って保存 →
//     容量が足りなければ古いものから消す)
//   * 一覧 (JSON) の読み取り
//   * 再生の順番 (まだ鳴らしていないものを新しい順 → 全部済んだら古い順に循環)
// FAT の読み書きは「仕事」(stackee_clip_job_t) にしてメインループの worker
// (stackee_clipfs.c) へ渡す。通信は会話と同じ部品 (同じ接続・同じ通信タスク)。
// hostbuild/clip_main.c が偽の通信と偽の FAT をつないで台本を流す。
//
// ★ サーバーとの取り決め (変えないこと):
//   GET /clips             → {"rev":N,"clips":[{"id","created","audio_bytes",
//                              "sample_rate":16000,"channels":1,"sample_width":2,
//                              "reply","subtitles"}, …]}  (created の古い順、64 KB 以内)
//   GET /clips/<id>/audio  → 生 PCM (16 kHz / 16 bit / mono)。audio_bytes=0 は字幕だけ
//   id は [A-Za-z0-9_-]{1,40}。送り先は STACKEE_TALK_URL の末尾 "/talk" を
//   "/clips" にしたもの (Bearer・HTTPS は /talk と同じ)。中継が古いと 404 → 10 分休む。
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STACKEE_CLIP_MAX            64          // 本体に置く数の上限 (一覧もここまで見る)
#define STACKEE_CLIP_ID_MAX         40
#define STACKEE_CLIP_CREATED_MAX    39
#define STACKEE_CLIP_LIST_LIMIT     65536u      // GET /clips の受け皿 (取り決めは 64 KB 以内)
// 1 件の音声の上限 = 会話の返答と同じ 120 秒 (3.84 MB、PSRAM に丸ごと読む)。
#define STACKEE_CLIP_AUDIO_MAX      (16000u * 2u * 120u)
// 1 件のメタデータ (一覧の中のそのクリップの JSON 1 個) の上限。字幕 4 KB + 本文。
#define STACKEE_CLIP_META_MAX       16384u
// FAT に残す余白。これを割るなら古いクリップから消す。
#define STACKEE_CLIP_RESERVE_BYTES  (512u * 1024u)
#define STACKEE_CLIP_PERIOD_MS      300000u     // 5 分ごと
#define STACKEE_CLIP_ABORT_RETRY_MS 60000u      // キーで打ち切ったときだけ 1 分後に続きから
#define STACKEE_CLIP_404_MS         600000u     // 中継が /clips を知らない (10 分休む)
#define STACKEE_CLIP_PATH_MAX       160
// 自動取得の入り切りを NVS へ書くのは、打鍵が止まってこれだけたってから
// (NVS の書き込みもフラッシュを書くので、打鍵の最中にはしない)。
#define STACKEE_CLIP_SAVE_QUIET_MS  2000u

// ---- 本体に置いてある 1 件 --------------------------------------------------
typedef struct {
    char     id[STACKEE_CLIP_ID_MAX + 1];
    char     created[STACKEE_CLIP_CREATED_MAX + 1];  // 一覧の値そのまま (引用符は外す)
    uint32_t audio_bytes;
    uint32_t meta_bytes;
    uint32_t file;          // FAT の名前の通し番号 (clips/%08lX.pcm / .jsn)
    bool     played;        // 再生済み (RAM だけ。再起動で全部「まだ」に戻る)
} stackee_clip_entry_t;

// ---- FAT の仕事 (worker へ渡す) ---------------------------------------------
typedef enum {
    STACKEE_CLIP_JOB_NONE = 0,
    STACKEE_CLIP_JOB_SCAN,      // clips/ を読んで目録を作る (半端物は消す)
    STACKEE_CLIP_JOB_WRITE,     // .pcm を書く → .tmp にメタ → .jsn へ改名 (最後が確定)
    STACKEE_CLIP_JOB_REMOVE,    // 1 件消す
    STACKEE_CLIP_JOB_LOAD,      // 1 件をメタ + PCM ごと PSRAM へ読む
    STACKEE_CLIP_JOB_CLEAR,     // 全部消す (検証用)
} stackee_clip_job_kind_t;

typedef enum {
    STACKEE_CLIP_JOB_IDLE = 0,
    STACKEE_CLIP_JOB_BUSY,      // 渡した (worker が持っている)
    STACKEE_CLIP_JOB_OK,
    STACKEE_CLIP_JOB_FAIL,
    STACKEE_CLIP_JOB_ABORTED,   // 打ち切った (書きかけは worker が消してある)
} stackee_clip_job_state_t;

typedef struct {
    int         kind;
    _Atomic int state;
    uint32_t    file;
    char        id[STACKEE_CLIP_ID_MAX + 1];
    // WRITE: ★ meta / pcm の持ち主は渡した時点で worker。成否・打ち切りに
    //   かかわらず worker が free() する (pcm は NULL 可 = 字幕だけ)。
    char       *meta;
    size_t      meta_len;
    uint8_t    *pcm;
    size_t      pcm_len;
    // LOAD: 期待する長さ。結果の持ち主は呼び手 (free() で返す)。
    uint32_t    expect_audio;
    char       *meta_out;
    size_t      meta_out_len;
    uint8_t    *pcm_out;
    size_t      pcm_out_len;
    // SCAN: 結果の入れ物 (呼び手の配列)。
    stackee_clip_entry_t *entries;
    int         cap;
    int         count;
    uint32_t    next_file;
    uint32_t    cleaned;        // 消した半端物 (.pcm だけ / .tmp / 壊れた .jsn)
    // 共通の結果: FAT の空き (worker が分かったときだけ)
    bool        space_valid;
    uint64_t    free_bytes;
    uint32_t    cluster;
    uint32_t    took_ms;
    char        error[48];
} stackee_clip_job_t;

// ---- 外から差し込む口 -------------------------------------------------------
typedef struct {
    uint32_t (*now_ms)(void);
    // 通信 (会話と同じもの。stackee_talk_ops_t と同じ意味)。
    bool (*http_start)(const char *method, const char *path, const void *body,
                       size_t body_len, size_t limit, const char *content_type);
    int  (*http_poll)(int *status, const uint8_t **body, size_t *len);
    void (*http_close)(void);
    // 受け取り終えた本文を呼び手のものにする (以後 free() で返す)。
    // 通信側は空に戻る (http_close は要らない)。NULL = 取れなかった。
    uint8_t *(*http_take)(size_t *len);
    // FAT の仕事を渡す。false = worker が他の仕事を持っている。
    bool (*fs_submit)(stackee_clip_job_t *job);
    // 持っている仕事を打ち切ってもらう (書きかけは消してもらう)。すぐ戻る。
    void (*fs_abort)(void);
    void (*log)(const char *line);
    // ---- 自動取得の入り切り (2026-09-30)。どちらも NULL 可 ----
    // 入り切りを残す (NVS)。静かなときにだけ呼ぶ。
    void (*save_auto)(bool on);
    // 最後の打鍵からの時間 [ms]。NULL なら「ずっと静か」。
    uint32_t (*idle_ms)(void);
} stackee_clip_ops_t;

typedef enum {
    STACKEE_CLIP_SYNC_SCAN = 0, // 起動後、目録を読んでいる / まだ読めていない
    STACKEE_CLIP_SYNC_IDLE,     // 次の同期を待っている
    STACKEE_CLIP_SYNC_LIST,     // GET /clips
    STACKEE_CLIP_SYNC_REMOVE,   // 一覧に無いものを消している
    STACKEE_CLIP_SYNC_FETCH,    // GET /clips/<id>/audio
    STACKEE_CLIP_SYNC_WRITE,    // FAT へ書いている
    STACKEE_CLIP_SYNC_EVICT,    // 容量のために古いものを消している
    STACKEE_CLIP_SYNC_ABORTING, // 打ち切った。worker が書きかけを消し終えるのを待つ
    STACKEE_CLIP_SYNC_SLEEP,    // 中継が古い (404)。しばらく聞かない
    STACKEE_CLIP_SYNC_CLEAR,    // 全部消している (clips.clear)
    STACKEE_CLIP_SYNC_PHASES,
} stackee_clip_sync_phase_t;

extern const char *const stackee_clip_phase_names[STACKEE_CLIP_SYNC_PHASES];

// 一覧の 1 件 (同期の間だけ)。本文は list の中を指す (off / len)。
typedef struct {
    char     id[STACKEE_CLIP_ID_MAX + 1];
    char     created[STACKEE_CLIP_CREATED_MAX + 1];
    uint32_t audio_bytes;
    uint32_t off, len;
    bool     ok;            // 形式が正しく、取り込める
    bool     tried;         // この回でもう扱った (取った / 飛ばした)
} stackee_clip_srv_t;

typedef struct stackee_clip {
    const stackee_clip_ops_t *ops;
    char     list_path[STACKEE_CLIP_PATH_MAX];   // "/clips"。空 = 作れない (同期しない)

    // ---- 本体の目録 ----
    stackee_clip_entry_t local[STACKEE_CLIP_MAX];
    int      count;
    bool     scanned;
    uint32_t next_file;
    bool     space_valid;
    uint64_t free_bytes;
    uint32_t cluster;
    uint64_t total_audio;       // 置いてある音声の合計 [B]

    // ---- 同期 ----
    int      phase;             // stackee_clip_sync_phase_t
    uint32_t next_at;           // 次の同期をしてよい時刻
    bool     now_req;           // clips.sync (今すぐ)
    bool     rev_done_valid;
    long     rev_done;          // 最後に「最後まで」同期できた rev
    long     rev_seen;          // 最後に見た rev (-1 = まだ)
    bool     http_open;
    bool     sync_failed;       // この回で失敗があった (rev_done を進めない)
    uint8_t *list;              // GET /clips の本文 (持ち主はこちら)
    size_t   list_len;
    stackee_clip_srv_t srv[STACKEE_CLIP_MAX];
    int      srv_count;
    int      cur;               // いま扱っている srv の番号 (-1 = なし)
    char     cur_id[STACKEE_CLIP_ID_MAX + 1];   // ダウンロード中の id
    stackee_clip_job_t job;
    int      job_local;         // REMOVE / EVICT の対象 (local の番号)

    // ---- 再生 ----
    int      load_idx;          // 読んでいる local の番号 (-1 = なし)
    uint32_t load_file;
    bool     load_pending;      // 仕事の口が空くのを待っている / 読んでいる
    bool     repeat_valid;      // 全部済んだあとの循環の位置
    char     repeat_created[STACKEE_CLIP_CREATED_MAX + 1];
    uint32_t repeat_file;

    // ---- 自動取得 (5 分ごとの同期) の入り切り ----
    bool     auto_on;           // 既定 true。STK_CLIP_AUTO / clips.auto で切り替え
    bool     auto_forced_off;   // settings.toml の STACKEE_CLIP_SYNC = 0 (キーでは入らない)
    bool     auto_dirty;        // NVS へまだ書いていない
    uint32_t auto_saves;

    // ---- 数字 (clips.status) ----
    uint32_t syncs, sync_ok, sync_same, sync_fail, aborts;
    uint32_t downloads, fails, removed, evicted, skipped_space, skipped_bad;
    uint32_t loads, load_fails;
    uint32_t last_sync_at;      // 最後の同期が終わった時刻 [ms] (0 = まだ)
    const char *last_result;    // "" / "ok" / "same" / "error" / "aborted" / "404"
    int      last_status;       // 直近の HTTP の status
    char     last_error[64];
} stackee_clip_t;

// list_path は "/clips" の形 (空文字列なら同期しない)。
void stackee_clip_init(stackee_clip_t *c, const stackee_clip_ops_t *ops,
                       const char *list_path);

// 1 周。★ can_sync は「暇で、通信を使ってよい」(会話・写真・Custom・再生・
// OTA・撮影のどれもしていない、Wi-Fi が上がっている、受け箱の待ちを
// 持っていない)。false になったら同期中でも打ち切る。
// 目録の読み込み (SCAN) と再生のための読み込みは can_sync に関係なく進む。
void stackee_clip_step(stackee_clip_t *c, bool can_sync);

// 同期が通信を使いたいか (受け箱の常時ポーリングは、これが true の間は
// 次の要求を撃たずに譲る)。
bool stackee_clip_wants_net(const stackee_clip_t *c);

// ユーザーの操作 (会話・カメラ・Custom・クリップのキー) が来た。同期を
// 打ち切る (通信を閉じ、書きかけは worker に消してもらう)。すぐ戻る。
void stackee_clip_abort(stackee_clip_t *c);

// 起動時に NVS / settings.toml の値を入れる (NVS へは書かない)。
void stackee_clip_auto_init(stackee_clip_t *c, bool on, bool forced_off);
// 自動取得を入り切りする。OFF にしたら進行中の同期を打ち切る (書きかけは
// 残さない)。強制 OFF のときに ON を頼まれたら false (変えない)。
// 変わったら、静かになってから ops->save_auto で残す。
bool stackee_clip_set_auto(stackee_clip_t *c, bool on);
// いま自動取得が効いているか (ON かつ強制 OFF でない)。
bool stackee_clip_auto_active(const stackee_clip_t *c);

// clips.sync — 次の周で (暇なら) 同期する。rev が同じでも一覧と突き合わせる。
// ★ 自動取得が OFF でも動く (手で頼んだ 1 回)。
void stackee_clip_sync_now(stackee_clip_t *c);

// clips.clear — 全部消す (検証用)。同期中なら打ち切ってから。
bool stackee_clip_clear(stackee_clip_t *c);

// ---- 再生 ----
// 次に鳴らす 1 件 (local の番号)。無ければ -1。印は付けない。
//   まだ鳴らしていないものがあれば、その中で新しいもの。
//   全部鳴らし終えていれば、古い順に 1 件ずつ (最後まで行ったら最初へ)。
int  stackee_clip_pick(const stackee_clip_t *c);
// 読み始める (同期中なら打ち切る)。false = 番号が不正。
bool stackee_clip_load_begin(stackee_clip_t *c, int idx);
// 0 = まだ / 1 = 読めた (meta / pcm の持ち主は呼び手、free() で返す。
//   pcm は字幕だけなら NULL) / -1 = 読めなかった。
// ★ 読めたら「再生済み」の印を付けて循環の位置を進める。
int  stackee_clip_load_poll(stackee_clip_t *c, char **meta, size_t *meta_len,
                            uint8_t **pcm, size_t *pcm_len);
// 読み込みをやめる (結果が来たら捨てる)。
void stackee_clip_load_cancel(stackee_clip_t *c);

// ---- 決め事 (テストが直に呼ぶ) ----
// id が [A-Za-z0-9_-]{1,40} か。
bool stackee_clip_id_ok(const char *id);
// created の比べ方: 両方が数なら数として、そうでなければ文字列として。
int  stackee_clip_created_cmp(const char *a, const char *b);
// GET /clips の本文を読む (list を書き換える: 物の区切りを一時的に NUL にする)。
// 戻り値: 採った件数 (形式が不正なら -1)。rev が無ければ -1。
int  stackee_clip_parse_list(stackee_clip_t *c, char *list, size_t len, long *rev);
// FAT に置いたメタ (一覧の物 1 個そのまま) から目録の 1 件を作る (SCAN が使う)。
// file / played は触らない (0 / false)。形式が違えば false。
bool stackee_clip_meta_entry(const char *meta, size_t len, stackee_clip_entry_t *out);
// 容量: その 1 件を置くのに要るバイト数 (クラスタに切り上げる)。
uint64_t stackee_clip_need_bytes(uint32_t audio_bytes, uint32_t meta_bytes,
                                 uint32_t cluster);
