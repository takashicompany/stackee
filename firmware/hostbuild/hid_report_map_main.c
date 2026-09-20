// 我々の HID レポート記述子を、**IDF の esp_hid のパーサそのもの**に通す。
//
// 2026-09-16: 実機で esp_hidd_dev_init() が ESP_FAIL を返して BLE が
// 立ち上がらなかった。esp_hid_parse_report_map() は純粋な C なので、
// 実機に触らずここで同じ判定ができる。
//
// 出力:
//   PARSE ok|fail
//   USAGE <n> APPEARANCE <hex>
//   REPORT <id> <type> <protocol> <usage> <len>
#include <stdio.h>
#include <stdlib.h>

#include "esp_hid_common.h"

// 記述子は stackee_usb.c が持っている実体をそのまま使う (USB と BLE で
// 同じ配列である、という約束を崩さないため)。tusb.h を引っ張らずに
// 配列だけ切り出すのは難しいので、テスト側から渡してもらう。
extern const uint8_t *stackee_test_report_map(size_t *len);

int main(void) {
    size_t len = 0;
    const uint8_t *map = stackee_test_report_map(&len);
    esp_hid_report_map_t *parsed = esp_hid_parse_report_map(map, len);
    if (parsed == NULL) {
        printf("PARSE fail\n");
        return 0;
    }
    printf("PARSE ok\n");
    printf("USAGE %d APPEARANCE 0x%04X\n", parsed->usage, parsed->appearance);
    for (uint8_t i = 0; i < parsed->reports_len; i++) {
        esp_hid_report_item_t *r = &parsed->reports[i];
        printf("REPORT %d type=%d proto=%d usage=%d len=%d\n",
               r->report_id, r->report_type, r->protocol_mode, r->usage,
               r->value_len);
    }
    return 0;
}
