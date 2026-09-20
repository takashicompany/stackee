// 段階 3 の「ESP-IDF に依存しない部分」を Mac 上でそのまま走らせる入り口。
//
//   settings.toml の読み / Wi-Fi 登録簿 / 選び方 / チャネルの順番 /
//   音量のレジスタ値 / JSON の読み取り / 会話 URL の割り方
//
// 使い方はどれも「引数で何をするか決め、標準入力から本文を受ける」。
// tools/test_cfg_host.py が、現行 CircuitPython 版の同じ関数と突き合わせる。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_jsonlite.h"
#include "stackee_settings.h"
#include "stackee_talksm.h"
#include "stackee_volume.h"
#include "stackee_wifistore.h"

static char g_in[65536];

static void read_stdin(void) {
    size_t n = fread(g_in, 1, sizeof(g_in) - 1, stdin);
    g_in[n] = '\0';
}

// 値に改行やタブが入りうるので逃がして出す (行と欄が壊れないように)。
static void print_escaped(const char *text) {
    for (const char *p = text; *p; p++) {
        if (*p == '\\')      { printf("\\\\"); }
        else if (*p == '\t') { printf("\\t"); }
        else if (*p == '\n') { printf("\\n"); }
        else if (*p == '\r') { printf("\\r"); }
        else                { putchar(*p); }
    }
}

static void print_list(const stackee_wifi_list_t *list) {
    for (int i = 0; i < list->count; i++) {
        printf("net\t%s\t%s\t%d\n", list->nets[i].ssid, list->nets[i].password,
               list->nets[i].channel);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: cfg <what> [args]\n");
        return 2;
    }
    const char *what = argv[1];

    if (strcmp(what, "settings") == 0) {
        read_stdin();
        int n = stackee_settings_load_text(g_in);
        printf("count\t%d\n", n);
        for (int i = 0; i < n; i++) {
            const char *key = stackee_settings_key_at(i);
            // 値に改行やタブが入りうるので逃がして出す (行と欄が壊れないように)。
            printf("kv\t%s\t", key);
            print_escaped(stackee_settings_get(key));
            printf("\n");
        }
        return 0;
    }

    // ---- 段階 4: settings.raw / settings.set の中身 ----------------------
    if (strcmp(what, "mask") == 0) {
        read_stdin();
        static char out[65536];
        size_t need = stackee_settings_mask(g_in, out, sizeof(out));
        printf("need\t%zu\n", need);
        printf("text\t");
        print_escaped(out);
        printf("\n");
        return 0;
    }

    // rewrite KEY VALUE [KEY VALUE ...]   VALUE が "--null" ならその行を消す
    if (strcmp(what, "rewrite") == 0) {
        read_stdin();
        const char *keys[8];
        const char *vals[8];
        int n = 0;
        for (int i = 2; i + 1 < argc && n < 8; i += 2) {
            keys[n] = argv[i];
            vals[n] = (strcmp(argv[i + 1], "--null") == 0) ? NULL : argv[i + 1];
            n++;
        }
        static char out[65536];
        size_t need = stackee_settings_rewrite(g_in, keys, vals, n, out,
                                               sizeof(out));
        printf("need\t%zu\n", need);
        printf("text\t");
        print_escaped(out);
        printf("\n");
        return 0;
    }

    if (strcmp(what, "allowed") == 0) {
        for (int i = 2; i < argc; i++) {
            printf("allowed\t%s\t%d\t%d\n", argv[i],
                   stackee_settings_key_allowed(argv[i]) ? 1 : 0,
                   stackee_settings_key_secret(argv[i]) ? 1 : 0);
        }
        return 0;
    }

    if (strcmp(what, "wifiparse") == 0) {
        read_stdin();
        stackee_wifi_list_t list;
        const char *note = NULL;
        bool ok = stackee_wifi_parse(g_in, &list, &note);
        printf("ok\t%d\nnote\t%s\ncount\t%d\n", ok ? 1 : 0,
               note ? note : "", list.count);
        print_list(&list);
        return 0;
    }

    if (strcmp(what, "wifidumps") == 0) {
        // 引数で入れ物の大きさを指定できる (あふれたときの振る舞いの検査用)。
        read_stdin();
        stackee_wifi_list_t list;
        const char *note = NULL;
        stackee_wifi_parse(g_in, &list, &note);
        static char out[4096];
        size_t cap = sizeof(out);
        if (argc >= 3) {
            long want = atol(argv[2]);
            if (want > 0 && (size_t)want < sizeof(out)) {
                cap = (size_t)want;
            }
        }
        size_t n = stackee_wifi_dumps(&list, out, cap);
        printf("len\t%zu\njson\t%s\n", n, out);
        return 0;
    }

    if (strcmp(what, "wifipublic") == 0) {
        // console の wifi.list が出すのと**同じ実体**。
        read_stdin();
        stackee_wifi_list_t list;
        const char *note = NULL;
        stackee_wifi_parse(g_in, &list, &note);
        static char out[4096];
        size_t cap = sizeof(out);
        if (argc >= 3) {
            long want = atol(argv[2]);
            if (want > 0 && (size_t)want < sizeof(out)) {
                cap = (size_t)want;
            }
        }
        size_t n = stackee_wifi_public_json(&list, out, cap);
        printf("len\t%zu\njson\t%s\n", n, out);
        return 0;
    }

    if (strcmp(what, "wifiedit") == 0) {
        // wifiedit add <ssid> <password> <channel> / wifiedit del <ssid>
        read_stdin();
        stackee_wifi_list_t list;
        const char *note = NULL;
        stackee_wifi_parse(g_in, &list, &note);
        const char *err = NULL;
        if (argc >= 6 && strcmp(argv[2], "add") == 0) {
            err = stackee_wifi_upsert(&list, argv[3], argv[4], atoi(argv[5]));
        } else if (argc >= 4 && strcmp(argv[2], "del") == 0) {
            err = stackee_wifi_remove_ssid(&list, argv[3]);
        } else {
            return 2;
        }
        printf("err\t%s\ncount\t%d\n", err ? err : "", list.count);
        print_list(&list);
        return 0;
    }

    if (strcmp(what, "wifipick") == 0) {
        // 標準入力は {"networks":[...]}、引数は ssid ch rssi の 3 つ組の並び。
        read_stdin();
        stackee_wifi_list_t list;
        const char *note = NULL;
        stackee_wifi_parse(g_in, &list, &note);
        stackee_wifi_seen_t seen[STACKEE_WIFI_SEEN_MAX];
        int n = 0;
        for (int i = 2; i + 2 < argc && n < STACKEE_WIFI_SEEN_MAX; i += 3) {
            snprintf(seen[n].ssid, sizeof(seen[n].ssid), "%s", argv[i]);
            seen[n].channel = atoi(argv[i + 1]);
            seen[n].rssi = atoi(argv[i + 2]);
            n++;
        }
        stackee_wifi_pick_t pick;
        if (!stackee_wifi_pick(seen, n, &list, &pick)) {
            printf("pick\t-\n");
            return 0;
        }
        printf("pick\t%s\t%s\t%d\t%d\t%d\n", pick.ssid, pick.password,
               pick.channel, pick.rssi, pick.index);
        printf("seen_ch\t%d\n",
               stackee_wifi_seen_channel(seen, n, pick.ssid));
        return 0;
    }

    if (strcmp(what, "chorder") == 0) {
        read_stdin();
        stackee_wifi_list_t list;
        const char *note = NULL;
        stackee_wifi_parse(g_in, &list, &note);
        int last = (argc >= 3) ? atoi(argv[2]) : 0;
        int out[STACKEE_WIFI_CHANNELS_MAX];
        int n = stackee_wifi_channel_order(&list, last, out,
                                           STACKEE_WIFI_CHANNELS_MAX);
        printf("chs");
        for (int i = 0; i < n; i++) {
            printf("\t%d", out[i]);
        }
        printf("\n");
        for (int i = 0; i < n; i++) {
            printf("settle\t%d\t%d\n", out[i], stackee_wifi_settle_ms(out[i]));
        }
        return 0;
    }

    if (strcmp(what, "volbits") == 0) {
        for (int p = 0; p <= 100; p++) {
            printf("bits\t%d\t%u\n", p, (unsigned)stackee_volume_bits(p));
        }
        return 0;
    }

    if (strcmp(what, "volume") == 0) {
        // 音量の保存タイミング。引数は「now delta audio_busy input」の並び。
        stackee_volume_state_t v;
        stackee_volume_state_init(&v, atoi(argv[2]), 0);
        for (int i = 3; i + 3 < argc; i += 4) {
            uint32_t now = (uint32_t)strtoul(argv[i], NULL, 10);
            int delta = atoi(argv[i + 1]);
            bool busy = atoi(argv[i + 2]) != 0;
            bool input = atoi(argv[i + 3]) != 0;
            if (input) {
                stackee_volume_state_note_input(&v, now);
            }
            if (delta != 0) {
                stackee_volume_state_step(&v, delta, now);
            }
            bool save = stackee_volume_state_should_save(&v, now, busy);
            if (save) {
                stackee_volume_state_mark_saved(&v);
            }
            printf("step\t%lu\t%d\t%d\t%d\n", (unsigned long)now, v.percent,
                   save ? 1 : 0, stackee_volume_state_pending(&v) ? 1 : 0);
        }
        printf("saves\t%lu\n", (unsigned long)v.saves);
        return 0;
    }

    if (strcmp(what, "json") == 0) {
        read_stdin();
        for (int i = 2; i < argc; i++) {
            char text[1024];
            long value = 0;
            bool flag = false;
            if (stackee_json_str(g_in, argv[i], text, sizeof(text))) {
                printf("str\t%s\t", argv[i]);
                print_escaped(text);
                printf("\n");
            } else if (stackee_json_int(g_in, argv[i], &value)) {
                printf("int\t%s\t%ld\n", argv[i], value);
            } else if (stackee_json_bool(g_in, argv[i], &flag)) {
                printf("bool\t%s\t%d\n", argv[i], flag ? 1 : 0);
            } else {
                printf("none\t%s\n", argv[i]);
            }
        }
        return 0;
    }

    // 4 KB 級の値を切らずに読む (字幕の本文は done の JSON に混ざってくる)。
    //   cfg jsonbig <key...>
    // ★ 返答文の 256 バイトの道 (jsoncut) とは別物。こちらは
    //   stackee_json_raw で値の範囲を取り、丸ごと逃がしを戻す。
    if (strcmp(what, "jsonbig") == 0) {
        read_stdin();
        static char text[16384];
        for (int i = 2; i < argc; i++) {
            const char *at = NULL;
            size_t len = 0;
            if (!stackee_json_raw(g_in, argv[i], &at, &len)) {
                printf("none\t%s\n", argv[i]);
                continue;
            }
            memset(text, 0, sizeof(text));
            size_t need = stackee_json_unescape(at, len, text, sizeof(text));
            printf("raw\t%s\t%zu\n", argv[i], len);
            printf("big\t%s\t%zu\t%zu\t", argv[i], need, strlen(text));
            print_escaped(text);
            printf("\n");
        }
        return 0;
    }

    // 入れ物を小さくして切り詰めを起こす (返答文は 256 バイトで切れる)。
    //   cfg jsoncut <cap> <key...>
    if (strcmp(what, "jsoncut") == 0) {
        read_stdin();
        size_t cap = (size_t)atoi(argv[2]);
        if (cap == 0 || cap > 1024) {
            cap = 1024;
        }
        for (int i = 3; i < argc; i++) {
            char text[1024];
            memset(text, 0x7F, sizeof(text));   // はみ出しを見つけやすくする
            if (stackee_json_str(g_in, argv[i], text, cap)) {
                printf("str\t%s\t", argv[i]);
                print_escaped(text);
                printf("\n");
            } else {
                printf("none\t%s\n", argv[i]);
            }
        }
        return 0;
    }

    if (strcmp(what, "url") == 0) {
        char base[192];
        char path[STACKEE_TALK_PATH_MAX];
        if (!stackee_talk_split_url(argv[2], base, sizeof(base), path, sizeof(path))) {
            printf("bad\n");
            return 0;
        }
        printf("base\t%s\npath\t%s\n", base, path);
        return 0;
    }

    fprintf(stderr, "unknown: %s\n", what);
    return 2;
}
