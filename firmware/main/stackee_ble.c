#include "stackee_ble.h"

#include <string.h>

#include "esp_bt.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "store/config/ble_store_config.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#include "qmk_port.h"
#include "stackee_usb.h"

static const char *TAG = "ble";

// NimBLE のボンド保存 (components/bt/.../store/config)。ヘッダが無いので
// 例と同じく前方宣言する (esp_hid_device 例の app_main と同じ)。
void ble_store_config_init(void);

static esp_hidd_dev_t *s_dev;
static bool            s_ready;            // NimBLE が同期して HID が立った
static bool            s_connected;
static bool            s_advertising;
static uint32_t        s_adv_starts;      // アドバタイズを始めた回数
static uint32_t        s_adv_fails;       // 始められなかった回数
static uint32_t        s_adv_revived;     // 「出ているつもり」が止まっていた回数

// ---------------------------------------------------------------------------
// GATT Service Changed (macOS の GATT キャッシュ対策)
// ---------------------------------------------------------------------------
// 2026-09-16 の実機で分かったこと:
//   Mac は **CircuitPython 版の GATT の並び** (adaf0001 ほか) を覚えたまま
//   繋ぎに来る。ボンドは引き継げているので暗号化までは通るのに、HID の
//   属性ハンドルが当時のものと違うため、ホスト側の HID が繋がらない
//   (自動接続もしない / 打鍵も届かない)。Mac から bleak で明示的に繋ぐと
//   こちらの GATT を読み直すので繋がる、という切り分けまで済んでいる。
//
// 直し方は Bluetooth の仕様どおり: **GATT Service Changed の indication を
// 送って「並びが変わった」と伝える**。相手はキャッシュを捨てて読み直す。
//
// ★ ただの ble_svc_gatt_changed() では足りない。あれは
//   ble_gatts_chr_updated() を呼ぶだけで、**相手が Service Changed を購読
//   している記録** (CCCD) が無いと何も送らない。こちらのハンドルでの購読記録は
//   当然無い (相手が覚えているのは CircuitPython 版のハンドル)。
//   そこで
//     1. その相手の CCCD の記録を、**こちらの**ハンドルで書いておく
//        (次に繋いだときは NimBLE の普通の道で送られる)
//     2. 今回ぶんは ble_gatts_indicate_custom() で**直接**送る
//        (nimble/host/src/ble_gattc.c の実装は購読を見ない)
//   の二段にしてある。
#define SVC_CHANGED_UUID 0x2A05
#define GATT_SVC_UUID    0x1801

static uint16_t s_svc_changed_handle;
static uint32_t s_svc_changed_sent;     // 送った回数
static uint32_t s_svc_changed_acked;    // 相手が受け取ったと返した回数
static int      s_svc_changed_rc;       // 直近の送信結果 (0 = 成功)
static uint16_t        s_conn_handle;
static uint8_t         s_own_addr_type;
static uint32_t        s_connects;
static uint32_t        s_disconnects;
static uint32_t        s_sent;
static uint32_t        s_failed;

// ★ どこで失敗したかを status に出す。起動直後のログは CDC が繋がる前に
//   流れてしまうので、「エラーで return した」という事実だけでも
//   数字として残しておく (2026-09-16 に、まさにこれが無くて詰まった)。
static const char *s_err = "not_started";
static int         s_err_code;
static bool        s_started;

// ---------------------------------------------------------------------------
// HID レポート記述子
// ---------------------------------------------------------------------------
// ★ USB と **同じ配列そのもの** を使う。「同一内容」を約束と文章で守るのでは
//   なく、実体を 1 つにして構造的に守る。現行 CircuitPython 版で
//   「BLE だけ英数/かなが効かない」が起きたのは、USB と BLE で別々の記述子を
//   持っていて片方だけ Usage Maximum が 0x89 だったため (code.py の長い注釈)。
static esp_hid_raw_report_map_t s_report_maps[1];

// ---------------------------------------------------------------------------
// アドバタイズ
// ---------------------------------------------------------------------------
#define GATT_SVC_HID_UUID 0x1812

static const ble_uuid16_t s_hid_uuid = BLE_UUID16_INIT(GATT_SVC_HID_UUID);
// ---------------------------------------------------------------------------
// アドバタイズの段取り (ボンド済みの相手へまず名指しで撒く)
// ---------------------------------------------------------------------------
// 2026-09-16 の実機: GATT のキャッシュを入れ替えたあとも、**Mac が自分から
// 繋ぎ直さない**。スキャンには見えていて (RSSI -48、広告に 0x1812)、
// bleak から繋げば繋がる。つまり「見えていないから繋がらない」のではなく、
// **Mac がこちらへ繋ぎに来る気になっていない**。
//
// Bluetooth の仕様には、まさにこのための撒き方がある:
//   ADV_DIRECT_IND (directed advertising) = 「あなた (このアドレス) に
//   繋ぎに来てほしい」という名指しの広告。ホストは自分宛だと分かるので
//   すぐ繋ぎに来る。ZMK も QMK の BLE も、切断後はまずこれを撒く。
//
//   high duty cycle … 3.75 ms 間隔。**コントローラが 1.28 秒で自動的に止める**
//                     (Core 仕様。NimBLE は ADV_COMPLETE か、接続失敗
//                      status = BLE_ERR_DIR_ADV_TMO (0x3C) で知らせる)
//   low duty cycle  … 普通の間隔で長く撒ける
//
// 段取り: directed(高) 1.28 秒 → directed(低) 30 秒 → 以後ずっと undirected。
// ★ directed の間は**誰からも見つけられない** (広告データを載せられない
//   撒き方なので)。だから短く切り上げて undirected に戻す。そうしないと
//   新しいホストとペアリングできなくなる。
// ★ やり直すのは 起動 / 切断 / ble.refresh のときだけ。
typedef enum {
    ADV_OFF = 0,
    ADV_DIRECT_HIGH,
    ADV_DIRECT_LOW,
    ADV_UNDIRECTED,
} adv_kind_t;

#define ADV_DIRECT_HIGH_MS  2000        // 1.28 秒 + 取りこぼしの保険
#define ADV_DIRECT_LOW_MS   30000

static adv_kind_t s_adv_kind;
static ble_addr_t s_bond_peer;
static bool       s_have_bond_peer;
static int64_t    s_adv_started_us;
static uint32_t   s_adv_directed;       // 名指しで撒いた回数

static const char *adv_kind_name(adv_kind_t kind) {
    switch (kind) {
        case ADV_DIRECT_HIGH: return "directed_high";
        case ADV_DIRECT_LOW:  return "directed_low";
        case ADV_UNDIRECTED:  return "undirected";
        default:              return "off";
    }
}

// 最後にボンドした相手の identity address を覚える。
static void refresh_bond_peer(void) {
    ble_addr_t peers[1];
    int count = 0;
    s_have_bond_peer = false;
    if (ble_store_util_bonded_peers(peers, &count, 1) == 0 && count > 0) {
        s_bond_peer = peers[0];
        s_have_bond_peer = true;
        ESP_LOGI(TAG, "ボンド済みの相手 %02X:%02X:%02X:%02X:%02X:%02X (type %u)",
                 s_bond_peer.val[5], s_bond_peer.val[4], s_bond_peer.val[3],
                 s_bond_peer.val[2], s_bond_peer.val[1], s_bond_peer.val[0],
                 s_bond_peer.type);
    } else {
        ESP_LOGI(TAG, "ボンド済みの相手は居ない。名指しでは撒かない");
    }
}

static struct ble_hs_adv_fields s_adv_fields;
static struct ble_hs_adv_fields s_scan_fields;

static void build_adv_fields(void) {
    memset(&s_adv_fields, 0, sizeof(s_adv_fields));
    // BLE のみ (BR/EDR 非対応) + 一般発見可能。
    // ★ 現行 CircuitPython 版と**同じ構成**にする。
    //   adafruit_ble の ProvideServicesAdvertisement(hid) は
    //     広告   … flags (general_discovery | le_only = 0x06) + 16bit UUID 0x1812
    //     応答   … complete name + tx power
    //   だけを載せる (adafruit_ble/advertising/standard.py の
    //   ProvideServicesAdvertisement.__init__ と adafruit_ble/__init__.py の
    //   BLERadio.start_advertising)。appearance も名前も広告には入れない。
    //   Mac が覚えているのはこの形なので、余計なものを足さない。
    s_adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // 外観はキーボード。Mac のペアリング画面でキーボードの絵になる。
    s_adv_fields.uuids16 = (ble_uuid16_t *)&s_hid_uuid;
    s_adv_fields.num_uuids16 = 1;
    s_adv_fields.uuids16_is_complete = 1;

    // スキャン応答: 名前と送信出力 (adafruit_ble の既定の応答と同じ)。
    memset(&s_scan_fields, 0, sizeof(s_scan_fields));
    s_scan_fields.name = (uint8_t *)STACKEE_BLE_NAME;
    s_scan_fields.name_len = (uint8_t)strlen(STACKEE_BLE_NAME);
    s_scan_fields.name_is_complete = 1;
    s_scan_fields.tx_pwr_lvl_is_present = 1;
    s_scan_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
}

static int gap_event(struct ble_gap_event *event, void *arg);

// Service Changed 特性のハンドルを探す。GATT が立ってから 1 回だけ。
static void find_svc_changed_handle(void) {
    if (s_svc_changed_handle != 0) {
        return;
    }
    const ble_uuid16_t svc = BLE_UUID16_INIT(GATT_SVC_UUID);
    const ble_uuid16_t chr = BLE_UUID16_INIT(SVC_CHANGED_UUID);
    uint16_t out = 0;
    int rc = ble_gatts_find_chr(&svc.u, &chr.u, NULL, &out);
    if (rc != 0 || out == 0) {
        // ここに来るのは ble_svc_gatt_init() が呼ばれていないとき
        // (CONFIG_BT_NIMBLE_SVC_GATT 相当が無効)。キャッシュは直せない。
        ESP_LOGE(TAG, "★ Service Changed 特性が GATT に無い (rc=%d)。"
                      "相手の GATT キャッシュを更新できない", rc);
        return;
    }
    s_svc_changed_handle = out;
    ESP_LOGI(TAG, "Service Changed 特性 handle=%u", out);
}

// 相手の CCCD の記録を「indication を購読している」にしておく。
// ★ これは **次の接続**のためのもの。今回ぶんは下の直接送信が受け持つ
//   (NimBLE は接続時に記録を RAM へ写すので、あとから書いても今回は効かない)。
static void remember_svc_changed_subscription(const ble_addr_t *peer) {
    find_svc_changed_handle();
    if (s_svc_changed_handle == 0) {
        return;
    }
    struct ble_store_value_cccd value;
    memset(&value, 0, sizeof(value));
    value.peer_addr = *peer;
    value.chr_val_handle = s_svc_changed_handle;
    value.flags = BLE_GATT_CHR_F_INDICATE;
    value.value_changed = 0;
    int rc = ble_store_write_cccd(&value);
    if (rc != 0) {
        ESP_LOGW(TAG, "Service Changed の購読記録を書けない (rc=%d)", rc);
    }
}

// 「1 番から 0xFFFF 番まで変わった」= 全部読み直せ、と伝える。
static void send_service_changed(uint16_t conn_handle) {
    // GATT の登録が START_EVENT より後になる構成でも拾えるよう、ここでも探す。
    find_svc_changed_handle();
    if (s_svc_changed_handle == 0 || conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    // 標準 API も呼んでおく。ble_svc_gatt_changed() は範囲を覚えるので、
    // 相手が読み取りに来たときに正しい値を返せる。
    ble_svc_gatt_changed(0x0001, 0xFFFF);

    uint8_t payload[4] = {0x01, 0x00, 0xFF, 0xFF};      // little-endian
    struct os_mbuf *om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
    if (om == NULL) {
        ESP_LOGW(TAG, "Service Changed の領域が取れない");
        return;
    }
    int rc = ble_gatts_indicate_custom(conn_handle, s_svc_changed_handle, om);
    s_svc_changed_rc = rc;
    if (rc == 0) {
        s_svc_changed_sent++;
        ESP_LOGI(TAG, "Service Changed を送った (handle=%u, %lu 回目)。"
                      "相手はここで GATT を読み直すはず",
                 s_svc_changed_handle, (unsigned long)s_svc_changed_sent);
    } else {
        ESP_LOGW(TAG, "Service Changed を送れない (rc=%d)", rc);
    }
}

// 指定の撒き方で始める。directed は広告データを載せられないので、
// ble_gap_adv_set_fields を呼ぶのは undirected のときだけ。
static void start_advertising_kind(adv_kind_t kind) {
    if (!s_ready || s_connected) {
        return;
    }
    if ((kind == ADV_DIRECT_HIGH || kind == ADV_DIRECT_LOW) && !s_have_bond_peer) {
        kind = ADV_UNDIRECTED;      // 名指しの相手が居ない
    }
    if (s_advertising) {
        ble_gap_adv_stop();
        s_advertising = false;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    int32_t duration = BLE_HS_FOREVER;
    const ble_addr_t *peer = NULL;

    switch (kind) {
        case ADV_DIRECT_HIGH:
            params.conn_mode = BLE_GAP_CONN_MODE_DIR;
            params.disc_mode = BLE_GAP_DISC_MODE_NON;   // 広告データが無い
            params.high_duty_cycle = 1;
            // ★ 間隔は 0 のまま。high duty の ADV_DIRECT_IND_HD は
            //   コントローラが 3.75 ms 固定で撒き、1.28 秒で自分から止める。
            peer = &s_bond_peer;
            duration = BLE_HS_FOREVER;   // 止めるのはコントローラ側
            break;
        case ADV_DIRECT_LOW:
            params.conn_mode = BLE_GAP_CONN_MODE_DIR;
            params.disc_mode = BLE_GAP_DISC_MODE_NON;
            params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
            params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
            peer = &s_bond_peer;
            duration = ADV_DIRECT_LOW_MS;
            break;
        case ADV_UNDIRECTED:
        default:
            kind = ADV_UNDIRECTED;
            params.conn_mode = BLE_GAP_CONN_MODE_UND;
            params.disc_mode = BLE_GAP_DISC_MODE_GEN;
            params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
            params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);
            // ★ 期限なし。キーボードは「置いてある間ずっと繋がるのを待つ」。
            duration = BLE_HS_FOREVER;
            break;
    }

    if (kind == ADV_UNDIRECTED) {
        int rc = ble_gap_adv_set_fields(&s_adv_fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "アドバタイズ内容を設定できない (rc=%d)", rc);
            return;
        }
        rc = ble_gap_adv_rsp_set_fields(&s_scan_fields);
        if (rc != 0) {
            ESP_LOGW(TAG, "スキャン応答を設定できない (rc=%d)", rc);
        }
    }

    int rc = ble_gap_adv_start(s_own_addr_type, peer, duration, &params,
                               gap_event, NULL);
    if (rc != 0) {
        s_adv_fails++;
        ESP_LOGW(TAG, "アドバタイズ (%s) を始められない (rc=%d)",
                 adv_kind_name(kind), rc);
        // 名指しが駄目なら、せめて普通に撒く。
        if (kind != ADV_UNDIRECTED) {
            start_advertising_kind(ADV_UNDIRECTED);
        }
        return;
    }
    s_advertising = true;
    s_adv_kind = kind;
    s_adv_starts++;
    s_adv_started_us = esp_timer_get_time();
    if (kind != ADV_UNDIRECTED) {
        s_adv_directed++;
    }
    ESP_LOGI(TAG, "アドバタイズ開始 %s (%lu 回目、NimBLE の申告 %d)",
             adv_kind_name(kind), (unsigned long)s_adv_starts,
             ble_gap_adv_active());
}

// 次の段へ。directed(高) -> directed(低) -> undirected。
static void advance_advertising(void) {
    switch (s_adv_kind) {
        case ADV_DIRECT_HIGH:
            start_advertising_kind(ADV_DIRECT_LOW);
            break;
        case ADV_DIRECT_LOW:
            start_advertising_kind(ADV_UNDIRECTED);
            break;
        default:
            start_advertising_kind(ADV_UNDIRECTED);
            break;
    }
}

// 最初から (起動 / 切断 / ble.refresh)。ボンド済みの相手が居れば名指しから。
static void start_advertising(void) {
    if (!s_ready || s_connected || s_advertising) {
        return;
    }
    refresh_bond_peer();
    start_advertising_kind(s_have_bond_peer ? ADV_DIRECT_HIGH : ADV_UNDIRECTED);
}

static void stop_advertising(void) {
    if (s_advertising) {
        ESP_LOGI(TAG, "アドバタイズ停止");
        ble_gap_adv_stop();
        s_advertising = false;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            s_advertising = false;
            if (event->connect.status == 0) {
                s_conn_handle = event->connect.conn_handle;
                s_connected = true;
                s_connects++;
                ESP_LOGI(TAG, "接続 (handle %u)", s_conn_handle);
                // ★ 暗号化を必須にする。HID のレポートは暗号化された接続でしか
                //   読ませない (ble_hs_cfg.sm_* でボンディングを有効にしてある)。
                ble_gap_security_initiate(s_conn_handle);
            } else if (event->connect.status == BLE_ERR_DIR_ADV_TMO) {
                // 名指しの広告 (高) の 1.28 秒切れ。**普段はここに来ない** —
                // NimBLE は ble_gap.c:3287 で ADV_COMPLETE に変えて渡すので、
                // 下の ADV_COMPLETE が拾う。版が変わったときの保険。
                ESP_LOGI(TAG, "名指しの広告 (高) が 1.28 秒で終わった。次へ");
                advance_advertising();
            } else {
                ESP_LOGW(TAG, "接続に失敗 (status %d)", event->connect.status);
                advance_advertising();
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            s_connected = false;
            s_disconnects++;
            s_advertising = false;
            s_adv_kind = ADV_OFF;
            ESP_LOGI(TAG, "切断 (reason %d)。名指しの広告からやり直す",
                     event->disconnect.reason);
            start_advertising();
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            s_advertising = false;
            ESP_LOGI(TAG, "アドバタイズ (%s) が終わった (reason %d)",
                     adv_kind_name(s_adv_kind), event->adv_complete.reason);
            advance_advertising();
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE: {
            ESP_LOGI(TAG, "暗号化 status=%d", event->enc_change.status);
            if (event->enc_change.status != 0) {
                return 0;
            }
            // ★ 暗号化が済んでから送る。Service Changed は暗号化された
            //   接続でしか受け取らないホストがある。
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                remember_svc_changed_subscription(&desc.peer_id_addr);
            }
            send_service_changed(event->enc_change.conn_handle);
            return 0;
        }
        case BLE_GAP_EVENT_NOTIFY_TX:
            // indication の行き先を数える (相手が受け取ったかが分かる唯一の印)。
            if (event->notify_tx.indication &&
                event->notify_tx.attr_handle == s_svc_changed_handle) {
                if (event->notify_tx.status == BLE_HS_EDONE) {
                    s_svc_changed_acked++;
                    ESP_LOGI(TAG, "Service Changed を相手が受け取った (%lu 回目)",
                             (unsigned long)s_svc_changed_acked);
                } else if (event->notify_tx.status != 0) {
                    ESP_LOGW(TAG, "Service Changed の応答が来ない (status=%d)",
                             event->notify_tx.status);
                }
            }
            return 0;
        case BLE_GAP_EVENT_SUBSCRIBE:
        case BLE_GAP_EVENT_CONN_UPDATE:
        case BLE_GAP_EVENT_MTU:
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// esp_hid のイベント
// ---------------------------------------------------------------------------
static void hidd_event(void *args, esp_event_base_t base, int32_t id,
                       void *data) {
    (void)args;
    (void)base;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)data;
    switch ((esp_hidd_event_t)id) {
        case ESP_HIDD_START_EVENT:
            // NimBLE が同期して GATT が立った。ここから撒き始める。
            //
            // ★ アイデンティティアドレスの決め方も CircuitPython と同じに
            //   する。_bleio/Adapter.c の _on_sync() が
            //     ble_hs_util_ensure_addr(false)   // false = 公開アドレスを優先
            //   を呼び、アドレス取得のたびに
            //     ble_hs_id_infer_auto(0, &address_type)
            //   を使っている。ESP32-S3 には efuse の MAC から作られる公開
            //   アドレスが必ずあるので、両方とも **public** になる。
            //   = Mac から見て CircuitPython 版と同じ機器アドレス。
            if (ble_hs_util_ensure_addr(false) != 0) {
                ESP_LOGW(TAG, "公開アドレスを用意できない");
            }
            if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
                s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
            }
            s_ready = true;
            s_err = "";
            find_svc_changed_handle();
            ESP_LOGI(TAG, "BLE HID 準備完了 (アドレス種別 %u)", s_own_addr_type);
            start_advertising();
            break;
        case ESP_HIDD_CONNECT_EVENT:
            break;
        case ESP_HIDD_DISCONNECT_EVENT:
            s_connected = false;
            start_advertising();
            break;
        case ESP_HIDD_OUTPUT_EVENT:
            // キーボードの LED (CapsLock など)。USB と同じ口へ渡す。
            if (param != NULL && param->output.length >= 1 &&
                param->output.data != NULL) {
                stackee_qmk_set_led_state(param->output.data[0]);
            }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// 立ち上げ
// ---------------------------------------------------------------------------
static void ble_host_task(void *param) {
    (void)param;
    ESP_LOGI(TAG, "NimBLE ホストタスク開始");
    nimble_port_run();      // nimble_port_stop() までは戻らない
    nimble_port_freertos_deinit();
}

esp_err_t stackee_ble_start(void) {
    size_t len = 0;
    const uint8_t *map = stackee_usb_hid_report_desc(STACKEE_HID_ITF_KEYS, &len);
    if (map == NULL || len == 0) {
        s_err = "report_map";
        ESP_LOGE(TAG, "HID 記述子が取れない");
        return ESP_FAIL;
    }
    s_report_maps[0].data = map;
    s_report_maps[0].len = (uint16_t)len;

    // ---- コントローラと NimBLE ------------------------------------------
    // ★ **nimble_port_init() を 1 回呼ぶだけ。** これは
    //   「コントローラの init → enable → esp_nimble_init」をこの IDF の
    //   正しい順番でまとめてやってくれる関数
    //   (components/bt/host/nimble/nimble/porting/nimble/src/nimble_port.c)。
    //   現行 CircuitPython 版 (_bleio/Adapter.c) もこれを呼んでいる =
    //   **この基板・この IDF で動いた実績があるのはこの道**。
    //
    //   2026-09-16 の失敗: 自分で
    //     esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)
    //     → esp_bt_controller_init → enable → esp_nimble_init
    //   と並べていた (esp_hid_device 例の書き方)。ESP32-S3 に Classic BT は
    //   無いので、その mem_release は IDF 自身の nimble_port_init でも
    //   「ESP32 のときだけ」と条件が付いている。BLE 側が使うメモリまで
    //   返してしまうと、あとの init が静かに失敗する。例のコードは
    //   Classic BT のある ESP32 も想定した書き方で、S3 にそのまま
    //   当てはめてはいけなかった。
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        s_err = "nimble_port_init";
        s_err_code = err;
        ESP_LOGE(TAG, "NimBLE を初期化できない: %s", esp_err_to_name(err));
        return err;
    }

    // ---- ペアリングの設定 ----------------------------------------------
    // ★ **現行 CircuitPython 版と 1 行ずつ同じにしてある。**
    //   出所: firmware/cp-uac/circuitpython/ports/espressif/common-hal/_bleio/
    //         Adapter.c の common_hal_bleio_adapter_set_enabled()
    //
    //   ここがずれると、CircuitPython 版で Mac と作ったボンド (nvs の
    //   "nimble_bond") を引き継げず、Mac 側でペアリングを削除して
    //   やり直す羽目になる。ユーザーの手を借りずに置き換えるには、
    //   鍵の作り方と配り方が完全に一致している必要がある。
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;     // 画面もテンキーも無い
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;                          // Just Works (CP と同じ)
    // ★ sm_sc は **0**。CircuitPython 版が 0 なので合わせる。1 にすると
    //   新しくペアリングするときに LE Secure Connections を使い、既存の
    //   レガシーボンドと鍵の作り方が変わる。
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist |= BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    build_adv_fields();
    ble_svc_gap_device_name_set(STACKEE_BLE_NAME);

    esp_hid_device_config_t cfg = {
        .vendor_id = 0x303A,
        .product_id = 0x811A,
        .version = 0x0100,
        .device_name = STACKEE_BLE_NAME,
        .manufacturer_name = "M5Stack",
        .serial_number = "stackee",
        .report_maps = s_report_maps,
        .report_maps_len = 1,
    };
    // ★ GATT を組み立ててから NimBLE のタスクを起こす、という順番でないと
    //   HID サービスが登録されない (esp_hid_device 例の app_main のコメント
    //   「Starting nimble task after gatts is initialized」)。
    err = esp_hidd_dev_init(&cfg, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev);
    if (err != ESP_OK) {
        s_err = "esp_hidd_dev_init";
        s_err_code = err;
        ESP_LOGE(TAG, "HID デバイスを作れない: %s", esp_err_to_name(err));
        return err;
    }

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // ★ "waiting_sync" は **esp_nimble_enable() より前**に置く。
    //   ホストタスクは起こした直後に同期し、ESP_HIDD_START_EVENT が
    //   すぐ飛んでくる。あとで代入すると、START_EVENT が立てた "" を
    //   上書きしてしまい、繋がっているのに waiting_sync のままになる
    //   (2026-09-16 の実機で発生)。
    s_started = true;
    s_err = "waiting_sync";      // ここから ESP_HIDD_START_EVENT を待つ

    err = esp_nimble_enable(ble_host_task);
    if (err != ESP_OK) {
        s_err = "esp_nimble_enable";
        s_err_code = err;
        ESP_LOGE(TAG, "NimBLE を起動できない: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "BLE HID 立ち上げ (名前 \"%s\")", STACKEE_BLE_NAME);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
void stackee_ble_tick(void) {
    // 現行 KMK の BLEHID.ble_monitor と同じ: 未接続なら撒き直す。
    //
    // ★ **自分の旗を信じない。** NimBLE に「いま本当に出ているか」を聞く
    //   (ble_gap_adv_active)。2026-09-16 の実機で、Wi-Fi が繋がったあと
    //   blex.adv が true のまま 160 秒たっても Mac が繋がらなかった。
    //   こちらの旗は ble_gap_adv_start が成功したときに立てるだけなので、
    //   そのあと NimBLE 側 (共存の都合など) で止まっても下りない。
    //   旗だけを見ていると「出ているつもりで黙ったまま」になる。
    if (!s_ready || s_connected) {
        return;
    }
    if (ble_gap_adv_active()) {
        s_advertising = true;
        // ★ 名指しの広告 (高) は 1.28 秒で終わるはずのもの。イベントが
        //   取りこぼされても止まったままにしないよう、締め切りを持っておく。
        if (s_adv_kind == ADV_DIRECT_HIGH &&
            esp_timer_get_time() - s_adv_started_us > ADV_DIRECT_HIGH_MS * 1000) {
            ESP_LOGW(TAG, "名指しの広告 (高) が終わらない。次へ進める");
            advance_advertising();
        }
        return;
    }
    if (s_advertising) {
        s_advertising = false;
        s_adv_revived++;
        ESP_LOGW(TAG, "アドバタイズ (%s) が止まっていた (%lu 回目)。次へ",
                 adv_kind_name(s_adv_kind), (unsigned long)s_adv_revived);
        advance_advertising();
        return;
    }
    start_advertising();
}

bool stackee_ble_ready(void) {
    return s_ready;
}

bool stackee_ble_connected(void) {
    return s_connected && s_dev != NULL && esp_hidd_dev_connected(s_dev);
}

int stackee_ble_interval_ms(void) {
    if (!s_connected) {
        return -1;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn_handle, &desc) != 0) {
        return -1;
    }
    // conn_itvl の単位は 1.25 ms。四捨五入せず切り捨てでよい (目安の数字)。
    return (int)((uint32_t)desc.conn_itvl * 125 / 100);
}

bool stackee_ble_send(uint8_t report_id, const uint8_t *data, size_t len) {
    if (s_dev == NULL || !s_connected) {
        s_failed++;
        return false;
    }
    esp_err_t err = esp_hidd_dev_input_set(s_dev, 0, report_id,
                                           (uint8_t *)data, len);
    if (err != ESP_OK) {
        s_failed++;
        return false;
    }
    s_sent++;
    return true;
}

bool stackee_ble_send_service_changed(void) {
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
        remember_svc_changed_subscription(&desc.peer_id_addr);
    }
    send_service_changed(s_conn_handle);
    return s_svc_changed_rc == 0;
}

void stackee_ble_refresh(void) {
    // 現行 KMK の ble_refresh と同じ: 撒き直すだけ。★ ボンドは消さない。
    // 別のホストに繋ぎ直したいときに使う。
    ESP_LOGI(TAG, "アドバタイズを撒き直す");
    if (s_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_connected = false;
    }
    stop_advertising();
    s_adv_kind = ADV_OFF;
    // ★ 名指しの広告からやり直す (ボンド済みの相手が居れば)。
    start_advertising();
}

void stackee_ble_clear_bonds(void) {
    // ★ 消すと Mac 側でもペアリングを削除しないと繋がらない。だから
    //   キーには割り当てず、コンソールからだけ呼べるようにしてある。
    ESP_LOGW(TAG, "ボンドを全部消す");
    ble_store_clear();
    stackee_ble_refresh();
}

void stackee_ble_stats(stackee_ble_stats_t *out) {
    if (out == NULL) {
        return;
    }
    out->ready = s_ready;
    out->connected = stackee_ble_connected();
    // ★ 自分の旗ではなく NimBLE の申告を返す。
    out->advertising = s_ready ? (ble_gap_adv_active() != 0) : false;
    out->adv_starts = s_adv_starts;
    out->adv_fails = s_adv_fails;
    out->adv_revived = s_adv_revived;
    out->svc_changed_handle = s_svc_changed_handle;
    out->svc_changed_sent = s_svc_changed_sent;
    out->svc_changed_acked = s_svc_changed_acked;
    out->svc_changed_rc = s_svc_changed_rc;
    out->adv_kind = adv_kind_name(s_adv_kind);
    out->adv_directed = s_adv_directed;
    out->have_bond_peer = s_have_bond_peer;
    out->interval_ms = stackee_ble_interval_ms();
    out->connects = s_connects;
    out->disconnects = s_disconnects;
    out->sent = s_sent;
    out->failed = s_failed;
    out->started = s_started;
    out->err = s_err;
    out->err_code = s_err_code;
}
