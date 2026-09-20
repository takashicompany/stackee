// QMK の EEPROM の保存先 (NVS のブロブ)。
//
// 名前空間 "stackee"、キー "qmk_eep" に 1 KB のブロブを 1 つ。
// パーティション表の nvs (20 KB) は CircuitPython 版と共用なので、
// **既存のキーには一切触らない** (CIRCUITPY の設定と BLE のボンドが入っている)。
//
// 書き込みの回数を減らす仕組み (影 + 遅延書き戻し) は qmk_port/qmk_port_eeprom.c
// 側にある。ここは「1 KB のブロブを読む / 書く」だけ。
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "qmk_port.h"

static const char *TAG = "nvs";

#define STACKEE_NVS_NAMESPACE "stackee"
#define STACKEE_NVS_KEY       "qmk_eep"

bool stackee_qmk_eeprom_backend_load(uint8_t *buf, size_t len) {
    nvs_handle_t handle;
    if (nvs_open(STACKEE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;   // まだ 1 度も保存していない
    }
    size_t    size = len;
    esp_err_t err = nvs_get_blob(handle, STACKEE_NVS_KEY, buf, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != len) {
        ESP_LOGI(TAG, "保存された配列は無い (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool stackee_qmk_eeprom_backend_save(const uint8_t *buf, size_t len) {
    nvs_handle_t handle;
    esp_err_t    err = nvs_open(STACKEE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs を開けない: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, STACKEE_NVS_KEY, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配列を保存できない: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "配列を保存した (%u B)", (unsigned)len);
    return true;
}

// ---------------------------------------------------------------------------
// HID の送信先 (BLE / USB) の保存
// ---------------------------------------------------------------------------
// 同じ名前空間にキーを 1 つ足すだけ。1 バイトなので blob ではなく u8。
#define STACKEE_NVS_KEY_HID_DEST "hid_dest"

bool stackee_nvs_load_hid_dest(uint8_t *out) {
    nvs_handle_t handle;
    if (nvs_open(STACKEE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u8(handle, STACKEE_NVS_KEY_HID_DEST, out);
    nvs_close(handle);
    return err == ESP_OK;
}

bool stackee_nvs_save_hid_dest(uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open(STACKEE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_u8(handle, STACKEE_NVS_KEY_HID_DEST, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "送信先を保存できない: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "送信先を保存した (%u)", value);
    return true;
}

// ---------------------------------------------------------------------------
// NimBLE のボンドのうち、CCCD (通知の購読) の記録だけを消す
// ---------------------------------------------------------------------------
// ★ **鍵 (our_sec / peer_sec / local_irk) は消さない。** だから Mac 側で
//   ペアリングを削除する必要は無く、人手ゼロで効く。
//
// なぜ要るか: CCCD の記録は「相手がどの**属性ハンドル**の通知を有効にしたか」
// なので、GATT の構成が変わると指す先がずれる。CircuitPython 版の HID
// サービス (adafruit_ble) と、こちらの esp_hid の GATT は構成が違う。
// 普通は GATT の Service Changed で相手が discover し直すが、それが効かない
// ときの逃げ道として、購読の記録だけを捨てて貼り直させる。
//
// 名前空間とキーの綴りの出所:
//   components/bt/host/nimble/nimble/nimble/host/store/config/src/ble_store_nvs.c
//   NIMBLE_NVS_NAMESPACE "nimble_bond" / NIMBLE_NVS_CCCD_SEC_KEY "cccd_sec"
#define NIMBLE_NVS_NAMESPACE "nimble_bond"
#define NIMBLE_NVS_CCCD_PREFIX "cccd_sec"

int stackee_nvs_drop_nimble_cccd(void) {
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NIMBLE_NVS_NAMESPACE,
                                   NVS_TYPE_ANY, &it);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "nimble_bond に何も入っていない (%s)", esp_err_to_name(err));
        return 0;
    }
    // 先に名前を集めてから消す。反復中に消すと iterator が壊れる。
    char keys[16][NVS_KEY_NAME_MAX_SIZE];
    int  count = 0;
    while (err == ESP_OK && count < 16) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (strncmp(info.key, NIMBLE_NVS_CCCD_PREFIX,
                    strlen(NIMBLE_NVS_CCCD_PREFIX)) == 0) {
            strncpy(keys[count], info.key, sizeof(keys[0]) - 1);
            keys[count][sizeof(keys[0]) - 1] = '\0';
            count++;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (count == 0) {
        ESP_LOGI(TAG, "消す CCCD の記録は無い");
        return 0;
    }

    nvs_handle_t handle;
    if (nvs_open(NIMBLE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return -1;
    }
    int erased = 0;
    for (int i = 0; i < count; i++) {
        if (nvs_erase_key(handle, keys[i]) == ESP_OK) {
            erased++;
        }
    }
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGW(TAG, "CCCD の記録を %d 件消した (鍵は残してある)", erased);
    return erased;
}
