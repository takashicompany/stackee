#include "stackee_usb.h"

#include <string.h>

#include "stackee_camera.h"


#include "esp32s3/rom/usb/chip_usb_dw_wrapper.h"
#include "esp32s3/rom/usb/usb_persist.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/usb_phy.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"

#include "qmk_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tusb.h"

static const char *TAG = "usb";

// ★ 段階 1 で TinyUSB を cp-uac のツリー参照から IDF Component Registry 版へ
//   切り替えたときの取りこぼし防止。
//
//   段階 0 は components/tinyusb/CMakeLists.txt が CFG_TUSB_MCU と CFG_TUSB_OS を
//   PUBLIC で渡していた。レジストリ版が渡すのは CFG_TUSB_MCU **だけ**で、
//   CFG_TUSB_OS は tusb_config.h 側の責任になる。ここを取りこぼすと
//   CFG_TUSB_OS が 0 (= OPT_OS_NONE) になり、TinyUSB が FreeRTOS の
//   ミューテックスを使わないままビルドされる — しかもコンパイルは通るので、
//   別タスクからの送信が壊れるまで気付けない。
_Static_assert(CFG_TUSB_MCU == OPT_MCU_ESP32S3, "CFG_TUSB_MCU が ESP32-S3 でない");
_Static_assert(CFG_TUSB_OS == OPT_OS_FREERTOS, "CFG_TUSB_OS が FreeRTOS でない");

// CircuitPython 版と同じ値 (boards/m5stack_cores3/mpconfigboard.mk:1-5)。
#define STACKEE_USB_VID     0x303A
#define STACKEE_USB_PID     0x811A
#define STACKEE_USB_MANUF   "M5Stack"
#define STACKEE_USB_PRODUCT "M5Stack Core S3"

// ---------------------------------------------------------------------------
// HID レポート記述子
// ---------------------------------------------------------------------------

// キーボード / マウス / コンシューマを Report ID で 1 インターフェースに同居。
//
// ★ キーボードの Usage Maximum は 0xFF。現行 code.py が adafruit_ble の
//   上限 0x89 を避けるために自前の記述子を使っているのと同じ理由で、
//   JIS の LANG1 (0x90 かな) / LANG2 (0x91 英数) が要る (DESIGN.md §2 の BLE 行)。
static const uint8_t s_hid_keys_report[] = {
    // ---- キーボード (Report ID 1) ----
    0x05, 0x01,                     // Usage Page (Generic Desktop)
    0x09, 0x06,                     // Usage (Keyboard)
    0xA1, 0x01,                     // Collection (Application)
    0x85, STACKEE_REPORT_ID_KEYBOARD,
    //   修飾キー 8 個
    0x05, 0x07,                     //   Usage Page (Keyboard)
    0x19, 0xE0,                     //   Usage Minimum (0xE0)
    0x29, 0xE7,                     //   Usage Maximum (0xE7)
    0x15, 0x00,                     //   Logical Minimum (0)
    0x25, 0x01,                     //   Logical Maximum (1)
    0x95, 0x08,                     //   Report Count (8)
    0x75, 0x01,                     //   Report Size (1)
    0x81, 0x02,                     //   Input (Data, Variable, Absolute)
    //   予約バイト
    0x95, 0x01,                     //   Report Count (1)
    0x75, 0x08,                     //   Report Size (8)
    0x81, 0x03,                     //   Input (Constant)
    //   LED 5 個 + 詰め物
    0x05, 0x08,                     //   Usage Page (LED)
    0x19, 0x01,                     //   Usage Minimum (Num Lock)
    0x29, 0x05,                     //   Usage Maximum (Kana)
    0x95, 0x05,                     //   Report Count (5)
    0x75, 0x01,                     //   Report Size (1)
    0x91, 0x02,                     //   Output (Data, Variable, Absolute)
    0x95, 0x01,                     //   Report Count (1)
    0x75, 0x03,                     //   Report Size (3)
    0x91, 0x03,                     //   Output (Constant)
    //   同時押し 6 キー。Usage Maximum 0xFF は LANG1/LANG2 (0x90/0x91) を含む
    0x05, 0x07,                     //   Usage Page (Keyboard)
    0x19, 0x00,                     //   Usage Minimum (0x00)
    0x2A, 0xFF, 0x00,               //   Usage Maximum (0x00FF)
    0x15, 0x00,                     //   Logical Minimum (0)
    0x26, 0xFF, 0x00,               //   Logical Maximum (255)
    0x95, 0x06,                     //   Report Count (6)
    0x75, 0x08,                     //   Report Size (8)
    0x81, 0x00,                     //   Input (Data, Array, Absolute)
    0xC0,                           // End Collection

    // ---- マウス (Report ID 2) ----
    0x05, 0x01,                     // Usage Page (Generic Desktop)
    0x09, 0x02,                     // Usage (Mouse)
    0xA1, 0x01,                     // Collection (Application)
    0x85, STACKEE_REPORT_ID_MOUSE,
    0x09, 0x01,                     //   Usage (Pointer)
    0xA1, 0x00,                     //   Collection (Physical)
    0x05, 0x09,                     //     Usage Page (Button)
    0x19, 0x01,                     //     Usage Minimum (1)
    0x29, 0x05,                     //     Usage Maximum (5)
    0x15, 0x00,                     //     Logical Minimum (0)
    0x25, 0x01,                     //     Logical Maximum (1)
    0x95, 0x05,                     //     Report Count (5)
    0x75, 0x01,                     //     Report Size (1)
    0x81, 0x02,                     //     Input (Data, Variable, Absolute)
    0x95, 0x01,                     //     Report Count (1)
    0x75, 0x03,                     //     Report Size (3)
    0x81, 0x03,                     //     Input (Constant)
    0x05, 0x01,                     //     Usage Page (Generic Desktop)
    0x09, 0x30,                     //     Usage (X)
    0x09, 0x31,                     //     Usage (Y)
    0x15, 0x81,                     //     Logical Minimum (-127)
    0x25, 0x7F,                     //     Logical Maximum (127)
    0x95, 0x02,                     //     Report Count (2)
    0x75, 0x08,                     //     Report Size (8)
    0x81, 0x06,                     //     Input (Data, Variable, Relative)
    0x09, 0x38,                     //     Usage (Wheel)
    0x95, 0x01,                     //     Report Count (1)
    0x81, 0x06,                     //     Input (Data, Variable, Relative)
    0x05, 0x0C,                     //     Usage Page (Consumer)
    0x0A, 0x38, 0x02,               //     Usage (AC Pan)
    0x95, 0x01,                     //     Report Count (1)
    0x81, 0x06,                     //     Input (Data, Variable, Relative)
    0xC0,                           //   End Collection
    0xC0,                           // End Collection

    // ---- コンシューマ (Report ID 3): 音量など ----
    0x05, 0x0C,                     // Usage Page (Consumer)
    0x09, 0x01,                     // Usage (Consumer Control)
    0xA1, 0x01,                     // Collection (Application)
    0x85, STACKEE_REPORT_ID_CONSUMER,
    0x15, 0x00,                     //   Logical Minimum (0)
    0x26, 0xFF, 0x03,               //   Logical Maximum (1023)
    0x19, 0x00,                     //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,               //   Usage Maximum (1023)
    0x95, 0x01,                     //   Report Count (1)
    0x75, 0x10,                     //   Report Size (16)
    0x81, 0x00,                     //   Input (Data, Array, Absolute)
    0xC0,                           // End Collection
};

// Raw HID: トップレベルコレクションは **1 つだけ**、Report ID なし
// (DESIGN.md §4 / §8b の決定、2026-09-16)。
//
//   Usage Page 0xFF60 / Usage 0x61 … VIA / Remap。32 バイト固定。
//
// ★ 段階 0 ではここにコンソール用の 2 つ目のコレクション (Usage 0x62) を
//   置いていた。コレクションが 2 つあると HID の決まりで Report ID が必須に
//   なるが、VIA / Remap は Report ID 0 で送受信する前提で書かれているので、
//   互換性を失う恐れがある。コンソールは
//     dev プロファイル  … CDC (いまはこちら)
//     full プロファイル … VIA の独自 command id 0xC0〜 に相乗り (段階 4)
//   で運ぶことにして、記述子は素の VIA と同じ形に戻した。
static const uint8_t s_hid_raw_report[] = {
    0x06, 0x60, 0xFF,               // Usage Page (Vendor Defined 0xFF60)
    0x09, 0x61,                     // Usage (0x61)
    0xA1, 0x01,                     // Collection (Application)
    0x09, 0x62,                     //   Usage (0x62) — デバイス → ホスト
    0x15, 0x00,                     //   Logical Minimum (0)
    0x26, 0xFF, 0x00,               //   Logical Maximum (255)
    0x95, STACKEE_RAW_HID_SIZE,     //   Report Count (32)
    0x75, 0x08,                     //   Report Size (8)
    0x81, 0x02,                     //   Input (Data, Variable, Absolute)
    0x09, 0x63,                     //   Usage (0x63) — ホスト → デバイス
    0x15, 0x00,                     //   Logical Minimum (0)
    0x26, 0xFF, 0x00,               //   Logical Maximum (255)
    0x95, STACKEE_RAW_HID_SIZE,     //   Report Count (32)
    0x75, 0x08,                     //   Report Size (8)
    0x91, 0x02,                     //   Output (Data, Variable, Absolute)
    0xC0,                           // End Collection
};

const uint8_t *stackee_usb_hid_report_desc(uint8_t itf, size_t *len) {
    if (itf == STACKEE_HID_ITF_KEYS) {
        if (len) { *len = sizeof(s_hid_keys_report); }
        return s_hid_keys_report;
    }
    if (itf == STACKEE_HID_ITF_RAW) {
        if (len) { *len = sizeof(s_hid_raw_report); }
        return s_hid_raw_report;
    }
    if (len) { *len = 0; }
    return NULL;
}

// ---------------------------------------------------------------------------
// デバイス記述子と構成記述子
// ---------------------------------------------------------------------------
static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // CDC が混ざるので IAD が要る。
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = STACKEE_USB_VID,
    .idProduct          = STACKEE_USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

#if STACKEE_USB_PROFILE_FULL

// full: HID + Raw HID + UAC マイク。IN は 0x81/0x82/0x83 の 3 本 + EP0。
enum {
    ITF_NUM_HID_KEYS = 0,
    ITF_NUM_HID_RAW,
    ITF_NUM_AUDIO_CTL,      // = STACKEE_UAC_ITF_AC
    ITF_NUM_AUDIO_STREAM,   // = STACKEE_UAC_ITF_AS
    ITF_NUM_TOTAL,
};
_Static_assert(ITF_NUM_AUDIO_CTL == STACKEE_UAC_ITF_AC, "UAC の AC 番号がずれた");
_Static_assert(ITF_NUM_AUDIO_STREAM == STACKEE_UAC_ITF_AS, "UAC の AS 番号がずれた");

#define EPNUM_HID_KEYS_IN 0x81
#define EPNUM_HID_RAW_OUT 0x02
#define EPNUM_HID_RAW_IN  0x82
#define EPNUM_AUDIO_IN    0x83

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN \
                           + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN \
                           + TUD_AUDIO20_MIC_ONE_CH_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

    // HID (キーボード / マウス / コンシューマ)。OUT は使わず、LED は EP0 の
    // SET_REPORT で受ける。エンドポイントを 1 本節約するため。
    // ★ boot protocol は名乗らない: Report ID を使う記述子を boot キーボードと
    //   名乗るのは嘘になる (boot は ID 無しの 8 バイト固定)。BIOS 用の boot
    //   対応が要ると分かったら、段階 1 で別インターフェースに分ける。
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_KEYS, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_keys_report), EPNUM_HID_KEYS_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 1),

    // Raw HID (VIA + コンソール)。双方向なので OUT を持つ。
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_RAW, 6, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_hid_raw_report), EPNUM_HID_RAW_OUT,
                             EPNUM_HID_RAW_IN, CFG_TUD_HID_EP_BUFSIZE, 1),

    // UAC2 マイク (16 kHz / モノラル / 16 bit)。記述子は TinyUSB が持って
    // いる既製品をそのまま使う。CircuitPython 版 (firmware/cp-uac の
    // shared-module/usb_audio) が手書きしていたものと同じ構成。
    TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR(ITF_NUM_AUDIO_CTL, 7,
                                      /*nBytesPerSample*/ 2,
                                      /*nBitsUsedPerSample*/ 16,
                                      EPNUM_AUDIO_IN,
                                      CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX),
};

#else   // dev プロファイル

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_HID_KEYS,
    ITF_NUM_HID_RAW,
    ITF_NUM_TOTAL,
};

// IN エンドポイントは 0x81..0x84 の 4 本 + EP0 = 5 本 (上限ちょうど)。
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_HID_KEYS_IN 0x83
#define EPNUM_HID_RAW_OUT 0x04
#define EPNUM_HID_RAW_IN  0x84

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN \
                           + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

    // CDC: コンソールとログ。dev プロファイルはこれが主な通り道。
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8,
                       EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),

    // HID (キーボード / マウス / コンシューマ)。OUT は使わず、LED は EP0 の
    // SET_REPORT で受ける。エンドポイントを 1 本節約するため。
    // ★ boot protocol は名乗らない: Report ID を使う記述子を boot キーボードと
    //   名乗るのは嘘になる (boot は ID 無しの 8 バイト固定)。BIOS 用の boot
    //   対応が要ると分かったら、段階 1 で別インターフェースに分ける。
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_KEYS, 5, HID_ITF_PROTOCOL_NONE,
                       sizeof(s_hid_keys_report), EPNUM_HID_KEYS_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 1),

    // Raw HID (VIA + コンソール)。双方向なので OUT を持つ。
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_RAW, 6, HID_ITF_PROTOCOL_NONE,
                             sizeof(s_hid_raw_report), EPNUM_HID_RAW_OUT,
                             EPNUM_HID_RAW_IN, CFG_TUD_HID_EP_BUFSIZE, 1),
};

#endif

// ---- 文字列記述子 ----------------------------------------------------------
//
// シリアルは CircuitPython と同じ並びにする。CircuitPython は UID の各バイトを
// 「下位ニブル → 上位ニブル」の順で 16 進にするので、MAC ab:cd:ef:01:23:45 なら
// "BADCFE10325" + "4" = "BADCFE103254"。ポート名 /dev/cu.usbmodem<これ>x になる。
// 既存の道具 (UID でポートを選ぶもの) がそのまま通るように合わせてある。
static char s_serial[13];

static void build_serial(void) {
    static const char hex[] = "0123456789ABCDEF";
    uint8_t mac[6] = {0};
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    for (int i = 0; i < 6; i++) {
        s_serial[i * 2] = hex[mac[i] & 0x0F];
        s_serial[i * 2 + 1] = hex[(mac[i] >> 4) & 0x0F];
    }
    s_serial[12] = '\0';
}

static const char *const s_strings[] = {
    (const char[]){0x09, 0x04},     // 0: 英語 (0x0409)
    STACKEE_USB_MANUF,              // 1
    STACKEE_USB_PRODUCT,            // 2
    s_serial,                       // 3
    "Stackee Console",              // 4: CDC (dev プロファイルだけ)
    "Stackee HID",                  // 5
    "Stackee Raw HID",              // 6
    "Stackee Mic",                  // 7: UAC (full プロファイルだけ)
};

static uint16_t s_string_desc[32];

// ---------------------------------------------------------------------------
// TinyUSB からの呼び出し
// ---------------------------------------------------------------------------
const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&s_device_desc;
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return s_config_desc;
}

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
    size_t len = 0;
    return stackee_usb_hid_report_desc(instance, &len);
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    if (index >= sizeof(s_strings) / sizeof(s_strings[0])) {
        return NULL;
    }
    uint8_t count;
    if (index == 0) {
        memcpy(&s_string_desc[1], s_strings[0], 2);
        count = 1;
    } else {
        const char *str = s_strings[index];
        size_t len = strlen(str);
        if (len > 31) {
            len = 31;
        }
        for (size_t i = 0; i < len; i++) {
            s_string_desc[1 + i] = str[i];
        }
        count = (uint8_t)len;
    }
    s_string_desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
    return s_string_desc;
}

// GET_REPORT には空で答える (ホストから読まれることはない)。
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// ---------------------------------------------------------------------------
// Raw HID の受信キュー
// ---------------------------------------------------------------------------
// ★ ここ (USB タスク) から QMK を呼ばない。QMK の状態を触るのは入力タスク
//   1 本だけ、という決めごとを守るため。届いた 32 バイトを積んでおいて、
//   入力タスクが stackee_usb_raw_rx_pop() で取りに来る。
#define RAW_RX_SLOTS 8
static uint8_t  s_raw_rx[RAW_RX_SLOTS][STACKEE_RAW_HID_SIZE];
static volatile uint8_t s_raw_head;
static volatile uint8_t s_raw_tail;
static volatile uint32_t s_raw_dropped;

static void raw_rx_push(const uint8_t *data, uint16_t len) {
    uint8_t next = (uint8_t)((s_raw_head + 1) % RAW_RX_SLOTS);
    if (next == s_raw_tail) {
        s_raw_dropped++;
        return;
    }
    memset(s_raw_rx[s_raw_head], 0, STACKEE_RAW_HID_SIZE);
    memcpy(s_raw_rx[s_raw_head], data,
           len > STACKEE_RAW_HID_SIZE ? STACKEE_RAW_HID_SIZE : len);
    s_raw_head = next;
}

bool stackee_usb_raw_rx_pop(uint8_t *buf32) {
    if (s_raw_tail == s_raw_head) {
        return false;
    }
    memcpy(buf32, s_raw_rx[s_raw_tail], STACKEE_RAW_HID_SIZE);
    s_raw_tail = (uint8_t)((s_raw_tail + 1) % RAW_RX_SLOTS);
    return true;
}

uint32_t stackee_usb_raw_rx_dropped(void) {
    return s_raw_dropped;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
    (void)report_id;
    if (instance == STACKEE_HID_ITF_RAW) {
        // VIA / Remap からの 32 バイト。OUT エンドポイント経由でも EP0 の
        // SET_REPORT 経由でも同じ扱い。
        raw_rx_push(buffer, bufsize);
        return;
    }
    if (instance == STACKEE_HID_ITF_KEYS && report_type == HID_REPORT_TYPE_OUTPUT &&
        bufsize >= 1) {
        // キーボードの LED (CapsLock など)。QMK の host_keyboard_leds() が返す。
        stackee_qmk_set_led_state(buffer[0]);
    }
}

// ---------------------------------------------------------------------------
// 立ち上げ
// ---------------------------------------------------------------------------
static usb_phy_handle_t s_phy;

static void usb_device_task(void *arg) {
    (void)arg;
    for (;;) {
        tud_task();
#if CFG_TUD_CDC
        // CDC の送信は溜めずに出す。ホストが読まないときは TinyUSB の
        // 内側で詰まるだけで、こちらは止まらない。
        tud_cdc_write_flush();
#endif
        vTaskDelay(1);
    }
}

void stackee_usb_start(void) {
    build_serial();

    // CircuitPython の supervisor/usb.c:65-81 と同じ設定。速度を未定にして
    // おくのは macOS との列挙競合を避けるため (tinyusb#2943)。
    usb_phy_config_t phy_config = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_UNDEFINED,
    };
    ESP_ERROR_CHECK(usb_new_phy(&phy_config, &s_phy));

    if (!tud_init(0)) {
        ESP_LOGE(TAG, "TinyUSB を初期化できない");
        return;
    }
    // USB は CPU0。段階 1 以降の入力タスクは CPU1 に置くので、列挙や
    // 転送の都合で打鍵が待たされることがない。
    xTaskCreatePinnedToCore(usb_device_task, "usbd", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "USB %04X:%04X シリアル %s (%s: %s)",
             STACKEE_USB_VID, STACKEE_USB_PID, s_serial,
             stackee_usb_profile(),
#if STACKEE_USB_PROFILE_FULL
             "HID + Raw HID + UAC マイク"
#else
             "CDC + HID + Raw HID"
#endif
             );
}

bool stackee_usb_mounted(void) {
    return tud_mounted();
}

const char *stackee_usb_profile(void) {
#if STACKEE_USB_PROFILE_FULL
    return "full";
#else
    return "dev";
#endif
}

bool stackee_usb_has_cdc(void) {
    return CFG_TUD_CDC != 0;
}

// ---------------------------------------------------------------------------
// 脱出路: 1200bps タッチ → ROM の USB ダウンロードモード
// ---------------------------------------------------------------------------
// CircuitPython と同じ手順にしてある。一次情報:
//   supervisor/shared/usb/usb_device.c  tud_cdc_line_state_cb
//     「DTR が落ちたときに line coding が 1200bps なら reset_to_bootloader()」
//   ports/espressif/common-hal/microcontroller/__init__.c  RUNMODE_BOOTLOADER
//     ESP32-S3 は chip_usb_set_persist_flags(USBDC_BOOT_DFU) と
//     RTC_CNTL_OPTION1_REG = RTC_CNTL_FORCE_DOWNLOAD_BOOT を書いてから再起動
//
// これが効かなくなると、本体をソフトから書き換える道が無くなり、物理的な
// RST 長押しでしか戻せなくなる。段階 1 で必ず入れる (DESIGN.md §8 の
// 「戻し道が壊れた状態で次へ進まない」)。
void stackee_usb_request_rom_download(void) {
    ESP_LOGW(TAG, "ROM ダウンロードモードへ落ちる (303a:0009)");
    stackee_camera_pin_strap_safe();    // ★ 撮影後のリセットがブートループになる対策
    chip_usb_set_persist_flags(USBDC_BOOT_DFU);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}

void stackee_usb_request_restart(void) {
    ESP_LOGW(TAG, "再起動");
    stackee_camera_pin_strap_safe();
    esp_restart();
}

// TinyUSB が CDC の DTR/RTS の変化を教えてくる。
//
// ★★ full プロファイルには CDC が無い = **1200bps タッチの脱出路が無い**。
//   full の脱出路は
//     ・console の `bootloader` コマンド (Raw HID 経由)
//     ・キーマップの QK_BOOT
//     ・物理の RST 長押し
//   の 3 つだけ。README の「戻し方」に同じことが書いてある。
#if CFG_TUD_CDC
void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts) {
    (void)itf;
    (void)rts;
    if (dtr) {
        return;                 // DTR = false が「切断」の合図
    }
    cdc_line_coding_t coding;
    tud_cdc_get_line_coding(&coding);
    if (coding.bit_rate == 1200) {
        stackee_usb_request_rom_download();
    }
}
#endif
