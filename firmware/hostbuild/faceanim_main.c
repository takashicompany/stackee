// 顔の状態機械を Mac の上で回す。打鍵列テスト (keyseq_main.c) と同じ流儀で、
// **時刻を 1 ms 単位でこちらが決める**ので、2000 / 1000 / 3000 / 700 / 250 /
// 1500 ms の境目をそのまま狙える。
//
//   ./faceanim <manifest.json> < 台本
//
// 台本 (1 行 1 つ):
//   t <ms>        その時刻で 1 周まわす (tick)
//   key           そのとき打鍵を見た
//   speak 0|1     再生中か
//   rec 0|1       録音中か
//   busy 0|1      録音以外で会話中か
//   cam 0|1       撮影中 / 送信中か
//   show <n>      画面に出ている顔を n ということにする (差分の出発点)
//
// t のたびに 1 行出す:
//   t=<ms> state=<名前> group=<g> frame=<f> face=<顔番号> cur=<現在> tgt=<目標>
//   rect=<x,y,w,h|->  skipped=<回数>
//
// 差分の bbox は「全部の組み合わせで (0,0,240,240)」の作り物を使う。
// 本物の changes.bin を当てるのは tools/test_render_host.py の役目
// (あちらは実際に画素を塗って CRC で見る)。ここで見たいのは時刻だけ。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_faceanim.h"

static uint8_t changes[STACKEE_FACE_MAX_COUNT * STACKEE_FACE_MAX_COUNT * 4];

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "使い方: faceanim <manifest.json>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        fprintf(stderr, "開けない: %s\n", argv[1]);
        return 1;
    }
    static char json[65536];
    size_t n = fread(json, 1, sizeof(json) - 1, f);
    json[n] = '\0';
    fclose(f);

    static stackee_face_cases_t cases;
    if (!stackee_face_parse_cases(json, &cases)) {
        fprintf(stderr, "cases を読めない\n");
        return 1;
    }
    for (int s = 0; s < STACKEE_FACE_STATES; s++) {
        printf("case %s groups=%d frames", stackee_face_state_names[s],
               cases.cases[s].group_count);
        for (int g = 0; g < cases.cases[s].group_count; g++) {
            printf("%s%d", g ? "," : "=", cases.cases[s].frame_count[g]);
        }
        printf("\n");
    }
    // 全組み合わせで顔全体が変わる、という作り物の差分表。
    for (size_t i = 0; i < sizeof(changes); i += 4) {
        changes[i + 0] = 0;
        changes[i + 1] = 0;
        changes[i + 2] = 240;
        changes[i + 3] = 240;
    }

    stackee_face_view_t view;
    stackee_face_view_init(&view, &cases, changes, STACKEE_FACE_MAX_COUNT, 0);
    stackee_face_inputs_t in = {0};

    char line[128];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char word[32];
        long value = 0;
        if (sscanf(line, "%31s %ld", word, &value) < 1) {
            continue;
        }
        if (strcmp(word, "key") == 0) {
            stackee_face_view_note_key(&view, (uint32_t)value);
            continue;
        }
        if (strcmp(word, "speak") == 0) { in.speaking = value != 0; continue; }
        if (strcmp(word, "rec") == 0)   { in.talk_recording = value != 0; continue; }
        if (strcmp(word, "mic") == 0)   { in.mic_held = value != 0; continue; }
        if (strcmp(word, "busy") == 0)  { in.talk_busy = value != 0; continue; }
        if (strcmp(word, "cam") == 0)   { in.camera_active = value != 0; continue; }
        if (strcmp(word, "show") == 0)  { view.current = (int)value; view.target = -1; continue; }
        if (strcmp(word, "t") != 0) {
            continue;
        }
        stackee_face_rect_t rect = {0};
        bool painted = stackee_face_view_tick(&view, &in, (uint32_t)value, &rect);
        int face = -1;
        if (view.anim.state >= 0 && view.anim.group >= 0 && view.anim.frame >= 0) {
            face = cases.cases[view.anim.state].frames[view.anim.group][view.anim.frame];
        }
        printf("t=%ld state=%s group=%d frame=%d face=%d cur=%d tgt=%d rect=",
               value,
               view.anim.state >= 0 ? stackee_face_state_names[view.anim.state] : "-",
               view.anim.group, view.anim.frame, face, view.current, view.target);
        if (painted) {
            printf("%d,%d,%d,%d", rect.x, rect.y, rect.w, rect.h);
        } else {
            printf("-");
        }
        printf(" skipped=%u paints=%u\n", view.skipped, view.paints);
    }
    return 0;
}
