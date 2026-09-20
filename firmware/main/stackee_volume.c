// 音量の実機側。値の決め方と保存のタイミングは stackee_volume_core.c。
//
//   読む   NVS ("stackee" / "volume") → 無ければ FAT の /stackee_volume.json
//          → 無ければ 20
//   書く   NVS だけ。FAT は読み取り専用でマウントしているので触らない。
//
// ★ **初回起動で FAT の現行値を NVS へ移す** (DESIGN.md §8b)。実機には
//   CircuitPython 版が書いた /stackee_volume.json が残っているので、
//   ファームを載せ替えても音量が勝手に変わらない。
#include "stackee_volume.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "stackee_assets.h"
#include "stackee_codec.h"
#include "stackee_jsonlite.h"
#include "stackee_ui.h"

static const char *TAG = "volume";

#define NVS_NAMESPACE  "stackee"
#define NVS_KEY_VOLUME "volume"
#define FAT_PATH       STACKEE_ASSETS_MOUNT "/stackee_volume.json"

static stackee_volume_state_t s_state;
static const char *s_source = "default";
static bool s_init;
// 保存に失敗したときの待ち [ms]。stackee_volume.py の _save_failed_at と同じ
// 5 秒。★ changed_at を触って代用しない (「2 秒静かなら保存」の意味が変わる)。
#define SAVE_RETRY_MS 5000
static uint32_t s_failed_at;
static bool     s_failed;

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool load_nvs(int *out) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(handle, NVS_KEY_VOLUME, &value);
    nvs_close(handle);
    if (err != ESP_OK || value > 100) {
        return false;
    }
    *out = value;
    return true;
}

// CircuitPython 版が書いた {"v":1,"volume":NN}。
static bool load_fat(int *out) {
    FILE *f = fopen(FAT_PATH, "rb");
    if (f == NULL) {
        return false;
    }
    char text[128];
    size_t got = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[got] = '\0';
    long version = 0, value = 0;
    if (!stackee_json_int(text, "v", &version) || version != 1) {
        return false;
    }
    if (!stackee_json_int(text, "volume", &value) || value < 0 || value > 100) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool save_nvs(int percent) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u8(handle, NVS_KEY_VOLUME, (uint8_t)percent);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err == ESP_OK;
}

void stackee_volume_init(void) {
    if (s_init) {
        return;
    }
    s_init = true;
    int percent = STACKEE_VOLUME_DEFAULT;
    bool from_fat = false;
    if (load_nvs(&percent)) {
        s_source = "nvs";
    } else if (load_fat(&percent)) {
        s_source = "fat";
        from_fat = true;
    } else {
        s_source = "default";
    }
    stackee_volume_state_init(&s_state, percent, now_ms());
    if (from_fat) {
        // ★ ここで移す。次からは NVS が正になる。
        if (save_nvs(percent)) {
            ESP_LOGI(TAG, "FAT の音量 %d%% を NVS へ移した", percent);
        } else {
            ESP_LOGW(TAG, "FAT の音量 %d%% を NVS へ移せなかった", percent);
        }
    }
    ESP_LOGI(TAG, "音量 %d%% (%s)", percent, s_source);
    stackee_ui_set_volume(percent);
}

int stackee_volume_percent(void) {
    return s_state.percent;
}

bool stackee_volume_save_pending(void) {
    return stackee_volume_state_pending(&s_state);
}

const char *stackee_volume_source(void) {
    return s_source;
}

// ★ ここは **入力タスク (CPU1 / 最高優先度)** から呼ばれることがある。
//   I2C も NVS も触らない。触るのは audio タスクの
//   stackee_volume_task_step() だけ (1 周 5 ms で追いつく)。
static void after_change(void) {
    stackee_ui_set_volume(s_state.percent);
}

void stackee_volume_set(int percent) {
    stackee_volume_state_set(&s_state, percent, now_ms());
    after_change();
}

void stackee_volume_bump(int delta) {
    stackee_volume_state_step(&s_state, delta, now_ms());
    after_change();
}

void stackee_volume_note_input(void) {
    stackee_volume_state_note_input(&s_state, now_ms());
}

// 電源を切る直前に、保存待ちの音量を今すぐ NVS へ書く。
bool stackee_volume_flush(void) {
    if (!s_init || !stackee_volume_state_pending(&s_state)) {
        return true;
    }
    if (save_nvs(s_state.percent)) {
        stackee_volume_state_mark_saved(&s_state);
        ESP_LOGI(TAG, "保存した %d%% (電源断の前)", s_state.percent);
        return true;
    }
    return false;
}

void stackee_volume_task_step(bool audio_busy) {
    if (!s_init) {
        return;
    }
    // 鳴っている最中はレジスタだけ追従させ、保存は終わってから。
    stackee_aw88298_volume(s_state.percent);
    uint32_t now = now_ms();
    if (!stackee_volume_state_should_save(&s_state, now, audio_busy)) {
        return;
    }
    if (s_failed && (uint32_t)(now - s_failed_at) < SAVE_RETRY_MS) {
        return;             // 直前に失敗した。5 秒置いてから出直す
    }
    if (save_nvs(s_state.percent)) {
        stackee_volume_state_mark_saved(&s_state);
        s_failed = false;
        ESP_LOGI(TAG, "保存した %d%%", s_state.percent);
    } else {
        ESP_LOGW(TAG, "保存に失敗 (%d%%)。%d ms 後にもう一度",
                 s_state.percent, SAVE_RETRY_MS);
        s_failed = true;
        s_failed_at = now;
    }
}
