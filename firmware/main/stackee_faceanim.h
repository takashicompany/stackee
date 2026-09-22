// 顔の状態機械と差分描画の段取り。firmware/kmk/stackee_face.py の移植。
//
//   FaceAnimation … 状態 → グループ → フレームの選び方と切り替えの時刻
//   FaceView      … いまどの状態か (会話・カメラ・起動直後) と、
//                   changes.bin の bbox を 1 周 16 行ずつ塗る段取り
//
// ★ 画素には触らない。「どの顔のどの矩形を塗るか」を返すだけ。
//   実際に塗るのは stackee_draw.c、呼ぶのは stackee_ui.c。
//   ESP-IDF に依存しないので、Mac のホストビルドで打鍵列テストと同じ流儀で
//   タイミングを検証できる (tools/test_faceanim_host.py)。
#pragma once

#include <stdbool.h>
#include <stdint.h>

// manifest.json の cases のキーの順。
typedef enum {
    STACKEE_FACE_AWAKE = 0,
    STACKEE_FACE_IDLE,
    STACKEE_FACE_LISTENING,
    STACKEE_FACE_THINKING,
    STACKEE_FACE_SPEAKING,
    STACKEE_FACE_CAMERA,
    STACKEE_FACE_MICROPHONE,
    STACKEE_FACE_STATES,
} stackee_face_state_t;

#define STACKEE_FACE_MAX_GROUPS  12
#define STACKEE_FACE_MAX_FRAMES  12
#define STACKEE_FACE_MAX_COUNT   35

// 打鍵のあと、この時間は待機中の表情切り替え (まばたき) を始めない [ms]。
#define STACKEE_FACE_TYPING_PAUSE_MS 1000
// 待機・考え中・発話中の表情グループを選び直す間隔 [ms]。
#define STACKEE_FACE_GROUP_MS        3000
#define STACKEE_FACE_THINKING_MS     700
#define STACKEE_FACE_FRAME_MS        250
// PC マイク: 案11の呼吸 + 3本線の短い/長い/消灯。
#define STACKEE_FACE_MIC_STEP_MS     550
#define STACKEE_FACE_MIC_LAST_MS     650
// 起動直後に awake を出す時間 [ms] と、撮影のあと camera を保つ時間 [ms]。
#define STACKEE_FACE_AWAKE_MS        2000
#define STACKEE_FACE_CAMERA_HOLD_MS  1500
// 1 周で塗る行数。
#define STACKEE_FACE_CHUNK_ROWS      16

typedef struct {
    uint8_t group_count;
    uint8_t frame_count[STACKEE_FACE_MAX_GROUPS];
    uint8_t frames[STACKEE_FACE_MAX_GROUPS][STACKEE_FACE_MAX_FRAMES];
} stackee_face_case_t;

typedef struct {
    stackee_face_case_t cases[STACKEE_FACE_STATES];
} stackee_face_cases_t;

typedef struct {
    const stackee_face_cases_t *cases;
    int      state;             // -1 = まだ何も選んでいない
    int      group;
    int      frame;
    uint32_t group_at;
    uint32_t frame_at;
    uint32_t rng;               // xorshift32。テストで固定できる
} stackee_face_anim_t;

// 外の世界の様子 (段階 3 までは speaking / talk / camera は全部 false)。
typedef struct {
    bool speaking;              // stackee_halfduplex の再生中
    bool talk_recording;        // 録音中
    // ★ PC 側のプッシュトゥトーク (MIC(kc)。既定は MIC(KC_F13)) を押している。
    //   本体の会話録音とは別のPC向け音声入力。
    //   AIへの録音 (listening) と区別して microphone を表示する。
    bool mic_held;
    bool talk_busy;             // 録音以外で会話中 (送信・待ち・受信)
    bool camera_active;         // 撮影中 / 送信中
} stackee_face_inputs_t;

typedef struct {
    int frame;                  // 塗る顔の番号
    int x, y, w, h;             // 顔 1 枚 (240x240) の中の矩形
} stackee_face_rect_t;

typedef struct {
    stackee_face_anim_t anim;
    const uint8_t *changes;     // count*count*4 (x, y, right, bottom)
    int      count;
    int      current;           // いま画面に出ている顔
    int      target;            // -1 = 遷移していない
    int      bx, brow, bright, bbottom;
    uint32_t started;
    bool     camera_active;
    bool     camera_done_valid;
    uint32_t camera_done;
    bool     have_last_key;
    uint32_t last_key;
    bool     frozen;            // face.set 中。状態機械を止める
    uint32_t updates, paints, skipped;
} stackee_face_view_t;

// manifest.json の "cases" を読む。
//   "cases":{"awake":[[0,1]],"idle":[[2],[3],...],...}
// 目録の他の部分は使わないので、丸ごとの構文解析はしない (数字と括弧だけ)。
bool stackee_face_parse_cases(const char *json, stackee_face_cases_t *out);

// 状態名 ("awake" / "idle" / ...)。manifest の cases のキーと同じ並び。
extern const char *const stackee_face_state_names[STACKEE_FACE_STATES];

void stackee_face_view_init(stackee_face_view_t *v,
                            const stackee_face_cases_t *cases,
                            const uint8_t *changes, int count, uint32_t now);

// 最後に打鍵を見た時刻 [ms]。
void stackee_face_view_note_key(stackee_face_view_t *v, uint32_t now);

// いまどの状態か (stackee_face.py の FaceView.update の前半)。
int stackee_face_pick_state(stackee_face_view_t *v,
                            const stackee_face_inputs_t *in, uint32_t now);

// 1 周ぶん。塗るものがあれば rect を埋めて true。
bool stackee_face_view_tick(stackee_face_view_t *v,
                            const stackee_face_inputs_t *in, uint32_t now,
                            stackee_face_rect_t *rect);

// 指定の顔へ差分で寄せる 1 周 (face.set / ui.selftest 用。状態機械を通さない)。
// 遷移が終わっていれば false。
bool stackee_face_view_step_to(stackee_face_view_t *v, int frame,
                               stackee_face_rect_t *rect);

// 遷移の途中か。
bool stackee_face_view_busy(const stackee_face_view_t *v);
