// HID の送信先の選択。DESIGN.md §3 の hid_out / §5 の STK_HID_SWITCH。
//
// 現行 CircuitPython 版の決まりをそのまま持ってくる:
//
//   * 既定は **BLE**（ワイヤレス）。USB ケーブルを挿していても BLE で打つ
//     （充電しながら BLE、という普段の使い方を変えない。code.py の
//      hid_type=HIDModes.BLE / secondary_hid_type=HIDModes.USB）
//   * STK_HID_SWITCH で BLE ⇄ USB をトグル（KMK の hid_switch と同じ）
//   * 選んだ先は NVS に覚えて、再起動後も維持する（現行には無い。
//     CircuitPython 版は毎回 BLE から始まっていたので、ここは改良）
//   * USB を選んでいるのにケーブルが繋がっていなければ、送るときだけ
//     BLE に倒す。**選択そのものは書き換えない**（挿し直せば USB に戻る）
//
// ハードに触らないので、そのままホストビルドでテストできる
// （tools/test_hid_dest_host.py）。
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STACKEE_HID_BLE = 0,        // 既定。NVS が空のときもこれ
    STACKEE_HID_USB = 1,
} stackee_hid_dest_t;

// 外の世界（USB が繋がっているか / NVS）への口。実機と테스트で差し替える。
typedef struct {
    bool (*usb_connected)(void);
    bool (*load)(uint8_t *out);         // 保存された選択。無ければ false
    bool (*save)(uint8_t value);
} stackee_hid_dest_io_t;

// 起動時に 1 回。NVS から選択を読む。
void stackee_hid_dest_init(const stackee_hid_dest_io_t *io);

// いま「選ばれている」送信先（USB が抜けていても変わらない）。
stackee_hid_dest_t stackee_hid_dest_selected(void);

// いま「実際に使う」送信先。USB を選んでいてもケーブルが無ければ BLE。
stackee_hid_dest_t stackee_hid_dest_effective(void);

// STK_HID_SWITCH。BLE ⇄ USB をトグルして NVS に保存し、新しい選択を返す。
stackee_hid_dest_t stackee_hid_dest_toggle(void);

// 明示的に選ぶ（コンソールから）。
void stackee_hid_dest_set(stackee_hid_dest_t dest);

// status に出す文字列。現行 stackee_console.py と同じ "BLE" / "USB"。
const char *stackee_hid_dest_name(stackee_hid_dest_t dest);
