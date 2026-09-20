#include "stackee_wifism.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const stackee_wifi_state_names[STACKEE_WIFI_S_COUNT] = {
    "boot", "off", "load", "wait", "radio", "scan_start",
    "scan_wait", "scan_read", "connect", "up", "linkup",
};

static uint32_t now(const stackee_wifi_t *w) {
    return w->ops->now_ms();
}

static uint32_t since(const stackee_wifi_t *w, uint32_t mark) {
    return (uint32_t)(now(w) - mark);
}

static void logf_(const stackee_wifi_t *w, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void logf_(const stackee_wifi_t *w, const char *fmt, ...) {
    if (w->ops->log == NULL) {
        return;
    }
    char line[192];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    w->ops->log(line);
}

// 状態を進める。★ 遷移は必ずここを通す (ログとアイコンが漏れないように)。
static void to(stackee_wifi_t *w, int state, const char *note) {
    uint32_t t = now(w);
    uint32_t spent = (uint32_t)(t - w->entered);
    int old = w->state;
    w->state = state;
    w->entered = t;
    if (old != state || note != NULL) {
        logf_(w, "[wifi] t=%lu %s->%s (+%lums)%s%s",
              (unsigned long)(t - w->boot_ms), stackee_wifi_state_names[old],
              stackee_wifi_state_names[state], (unsigned long)spent,
              note ? " " : "", note ? note : "");
    }
    if (w->ops->ui != NULL && old != state) {
        w->ops->ui(stackee_wifi_state_names[state]);
    }
}

static void stop_scan(stackee_wifi_t *w) {
    if (w->ops->scan_stop != NULL) {
        w->ops->scan_stop();
    }
}

static void radio_off(stackee_wifi_t *w) {
    w->radio_held = false;
    if (w->ops->radio_off != NULL) {
        w->ops->radio_off();
    }
}

// 今回の探索を諦める。無線を落として次の時刻を決める。
static void fail_pass(stackee_wifi_t *w, const char *why) {
    stop_scan(w);
    radio_off(w);
    w->have_target = false;
    memset(&w->target, 0, sizeof(w->target));
    w->ssid[0] = '\0';
    w->ip[0] = '\0';
    w->failures++;
    w->next_at = now(w) + (uint32_t)STACKEE_WIFI_RETRY_S * 1000u;
    char note[160];
    snprintf(note, sizeof(note), "%s / 次は %ds 後", why, STACKEE_WIFI_RETRY_S);
    to(w, STACKEE_WIFI_S_WAIT, note);
}

void stackee_wifi_sm_init(stackee_wifi_t *w, const stackee_wifi_ops_t *ops) {
    memset(w, 0, sizeof(*w));
    w->ops = ops;
    w->boot_ms = ops->now_ms();
    w->entered = w->boot_ms;
    w->next_at = w->boot_ms + STACKEE_WIFI_FIRST_DELAY_MS;
    w->state = STACKEE_WIFI_S_BOOT;
    w->note = NULL;
    logf_(w, "[wifi] 起動。%dms 後に最初の探索", STACKEE_WIFI_FIRST_DELAY_MS);
}

// ---------------------------------------------------------------------------
static void do_load(stackee_wifi_t *w) {
    const char *note = NULL;
    stackee_wifi_list_t nets;
    memset(&nets, 0, sizeof(nets));
    if (w->ops->load == NULL || !w->ops->load(&nets, &note)) {
        if (note == NULL) {
            note = "error";
        }
    }
    w->nets = nets;
    w->note = note;
    w->loaded = true;
    if (nets.count == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "登録 0 件 (%s)", note ? note : "empty");
        to(w, STACKEE_WIFI_S_OFF, msg);
        return;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "登録 %d 件", nets.count);
    to(w, STACKEE_WIFI_S_RADIO, msg);
}

static void do_radio_on(stackee_wifi_t *w) {
    if (w->ops->audio_busy != NULL && w->ops->audio_busy()) {
        return;             // 録音・再生中は無線に触らない。次の周期で出直す
    }
    if (w->ops->radio_on == NULL || !w->ops->radio_on()) {
        fail_pass(w, "無線を点けられない");
        return;
    }
    w->radio_held = true;
    w->ch_count = stackee_wifi_channel_order(&w->nets, w->last_channel,
                                             w->chs, STACKEE_WIFI_CHANNELS_MAX);
    w->idx = 0;
    w->seen_count = 0;
    w->passes++;
    char msg[48];
    snprintf(msg, sizeof(msg), "%dch を順に走査", w->ch_count);
    to(w, STACKEE_WIFI_S_SCAN_START, msg);
}

static void do_scan_start(stackee_wifi_t *w) {
    if (w->idx >= w->ch_count) {
        char why[64];
        snprintf(why, sizeof(why), "見つからない (%dch 走査)", w->ch_count);
        fail_pass(w, why);
        return;
    }
    if (w->ops->audio_busy != NULL && w->ops->audio_busy()) {
        // ★ 粘りすぎない。会話 1 往復は最長 390 秒あるので、無線を点けた
        //   まま待ち続けると BLE と RF を取り合ったままになる。
        if (since(w, w->entered) >= STACKEE_WIFI_CONNECT_DEFER_MAX_MS) {
            fail_pass(w, "音が続くので出直す");
        }
        return;
    }
    int ch = w->chs[w->idx];
    if (!w->ops->scan_start(ch)) {
        fail_pass(w, "走査を始められない");
        return;
    }
    w->scan_started = now(w);
    w->settle = stackee_wifi_settle_ms(ch);
    // 状態遷移そのものは毎チャネル起きるので、ログは 1 行にまとめる。
    logf_(w, "[wifi] t=%lu scan ch=%d settle=%dms",
          (unsigned long)(w->scan_started - w->boot_ms), ch, w->settle);
    w->state = STACKEE_WIFI_S_SCAN_WAIT;
    w->entered = w->scan_started;
}

static void do_scan_read(stackee_wifi_t *w) {
    int room = STACKEE_WIFI_SEEN_MAX - w->seen_count;
    if (room > 0) {
        int got = w->ops->scan_read(w->seen + w->seen_count, room);
        if (got > 0) {
            w->seen_count += got;
        }
    }
    stop_scan(w);
    w->idx++;

    stackee_wifi_pick_t target;
    if (!stackee_wifi_pick(w->seen, w->seen_count, &w->nets, &target)) {
        w->state = STACKEE_WIFI_S_SCAN_START;   // 次のチャネルへ (ログは出さない)
        w->entered = now(w);
        return;
    }
    // ★ 登録簿の channel より「たった今スキャンで見えた channel」が強い。
    //   登録簿の値は人が手で入れたヒントで、古い / 間違っていることがある。
    int seen_ch = stackee_wifi_seen_channel(w->seen, w->seen_count, target.ssid);
    if (seen_ch > 0 && seen_ch != target.channel) {
        logf_(w, "[wifi] 登録簿の ch=%d は実際と違う (走査で ch=%d)。走査のほうを使う",
              target.channel, seen_ch);
        target.channel = seen_ch;
    }
    if (target.channel > 0) {
        w->last_channel = target.channel;
    }
    w->target = target;                 // ★ password 入り。ログ厳禁
    w->have_target = true;
    char msg[96];
    snprintf(msg, sizeof(msg), "seen ssid=%s ch=%d rssi=%d (%dch目)",
             target.ssid, target.channel, target.rssi, w->idx);
    to(w, STACKEE_WIFI_S_CONNECT, msg);
}

static void link_up(stackee_wifi_t *w, uint32_t connect_ms) {
    snprintf(w->ssid, sizeof(w->ssid), "%s", w->target.ssid);
    w->failures = 0;
    w->have_target = false;
    memset(&w->target, 0, sizeof(w->target));
    w->ip[0] = '\0';
    if (w->ops->get_ip != NULL) {
        w->ops->get_ip(w->ip, (int)sizeof(w->ip));
    }
    w->connect_ms = connect_ms;
    if (!w->up_recorded) {
        w->up_recorded = true;
        w->up_at_ms = since(w, w->boot_ms);
    }
    w->next_at = now(w) + STACKEE_WIFI_UP_POLL_MS;
    char msg[96];
    snprintf(msg, sizeof(msg), "up ssid=%s ip=%s connect_ms=%lu",
             w->ssid, w->ip, (unsigned long)connect_ms);
    to(w, STACKEE_WIFI_S_UP, msg);
}

static void do_connect(stackee_wifi_t *w) {
    // ★ 打鍵が続く間は絶対に撃たない。粘りすぎもしない。
    if (w->ops->keys_idle != NULL && !w->ops->keys_idle()) {
        if (since(w, w->entered) >= STACKEE_WIFI_CONNECT_DEFER_MAX_MS) {
            fail_pass(w, "打鍵が続くので出直す");
        }
        return;
    }
    if (!w->have_target) {
        fail_pass(w, "接続先が無い");
        return;
    }
    w->connects++;
    if (!w->ops->connect_start(w->target.ssid, w->target.password,
                               w->target.channel)) {
        // ★ ここに password は入らない (SSID と理由だけ)。
        char why[80];
        snprintf(why, sizeof(why), "接続を始められない ssid=%s", w->target.ssid);
        fail_pass(w, why);
        return;
    }
    w->connect_started = now(w);
    w->connect_ms = 0;
    to(w, STACKEE_WIFI_S_LINKUP, "接続待ち");
}

static void do_linkup(stackee_wifi_t *w) {
    uint32_t elapsed = since(w, w->connect_started);
    int state = w->ops->connect_state();
    if (state == 1) {
        link_up(w, elapsed);
        return;
    }
    if (state != 0) {
        // 2 以上は切断理由 (201=AP が居ない / 202=認証失敗 / 205=接続失敗)。
        char why[96];
        snprintf(why, sizeof(why), "接続失敗 ssid=%s reason=%d (connect_ms=%lu)",
                 w->target.ssid, state, (unsigned long)elapsed);
        fail_pass(w, why);
        return;
    }
    if (elapsed >= STACKEE_WIFI_CONNECT_TIMEOUT_MS) {
        char why[96];
        snprintf(why, sizeof(why), "接続がタイムアウト ssid=%s (%lums)",
                 w->target.ssid, (unsigned long)elapsed);
        fail_pass(w, why);
    }
}

static void check_link(stackee_wifi_t *w) {
    if (w->ops->link_alive != NULL && w->ops->link_alive()) {
        w->next_at = now(w) + STACKEE_WIFI_UP_POLL_MS;
        w->state = STACKEE_WIFI_S_UP;       // 毎回ログを出さない
        return;
    }
    w->ssid[0] = '\0';
    w->ip[0] = '\0';
    w->next_at = now(w) + STACKEE_WIFI_RELINK_DELAY_MS;
    to(w, STACKEE_WIFI_S_WAIT, "接続が切れた。やり直す");
}

void stackee_wifi_sm_step(stackee_wifi_t *w) {
    if (w->paused || w->state == STACKEE_WIFI_S_OFF) {
        return;
    }
    switch (w->state) {
        case STACKEE_WIFI_S_BOOT:
        case STACKEE_WIFI_S_WAIT:
            if ((int32_t)(now(w) - w->next_at) < 0) {
                return;                     // ここは何もしない = 0 ms
            }
            to(w, w->loaded ? STACKEE_WIFI_S_RADIO : STACKEE_WIFI_S_LOAD, NULL);
            return;
        case STACKEE_WIFI_S_LOAD:
            do_load(w);
            return;
        case STACKEE_WIFI_S_RADIO:
            do_radio_on(w);
            return;
        case STACKEE_WIFI_S_SCAN_START:
            do_scan_start(w);
            return;
        case STACKEE_WIFI_S_SCAN_WAIT: {
            bool ready = (w->ops->scan_ready != NULL) && w->ops->scan_ready();
            if (!ready && since(w, w->scan_started) < (uint32_t)w->settle) {
                return;
            }
            w->state = STACKEE_WIFI_S_SCAN_READ;
            w->entered = now(w);
            return;
        }
        case STACKEE_WIFI_S_SCAN_READ:
            do_scan_read(w);
            return;
        case STACKEE_WIFI_S_CONNECT:
            do_connect(w);
            return;
        case STACKEE_WIFI_S_LINKUP:
            do_linkup(w);
            return;
        case STACKEE_WIFI_S_UP:
            if ((int32_t)(now(w) - w->next_at) < 0) {
                return;
            }
            check_link(w);
            return;
        default:
            return;
    }
}

const char *stackee_wifi_sm_kick(stackee_wifi_t *w) {
    stop_scan(w);
    w->loaded = false;              // 登録が増えているかもしれない
    w->failures = 0;
    // ★ paused は触らない。console の wifi.scan が無線を借りている最中に
    //   wifi.connect が来ても、その借用を勝手に取り上げない
    //   (stackee_wifi.py の kick() も _paused を触らない)。
    w->next_at = now(w);
    to(w, STACKEE_WIFI_S_LOAD, "kick");
    return stackee_wifi_sm_state_name(w);
}

bool stackee_wifi_sm_suspend(stackee_wifi_t *w) {
    bool connected = stackee_wifi_sm_connected(w);
    if (w->paused) {
        return connected;
    }
    w->paused = true;
    if (w->state == STACKEE_WIFI_S_SCAN_START ||
        w->state == STACKEE_WIFI_S_SCAN_WAIT ||
        w->state == STACKEE_WIFI_S_SCAN_READ ||
        w->state == STACKEE_WIFI_S_LINKUP) {
        stop_scan(w);
        w->next_at = now(w);
        to(w, STACKEE_WIFI_S_WAIT, "suspend");
    } else {
        logf_(w, "[wifi] suspend (%s)", stackee_wifi_sm_state_name(w));
    }
    return connected;
}

void stackee_wifi_sm_resume(stackee_wifi_t *w) {
    if (!w->paused) {
        return;
    }
    w->paused = false;
    logf_(w, "[wifi] resume (%s)", stackee_wifi_sm_state_name(w));
    if (w->state == STACKEE_WIFI_S_UP) {
        check_link(w);
    }
}

const char *stackee_wifi_sm_state_name(const stackee_wifi_t *w) {
    return stackee_wifi_state_names[w->state];
}

bool stackee_wifi_sm_connected(const stackee_wifi_t *w) {
    return w->state == STACKEE_WIFI_S_UP;
}
