// HID の送信先の選び方を Mac 上でそのまま走らせる。
//
// 実機でしか試せないのは BLE のスタックそのもので、「どちらへ出すか」の
// 判断はただのロジックなので、ここで確かめられる。
//
// 台本 (1 行 1 命令):
//
//   init            起動しなおす (NVS から選択を読む)
//   usb <0|1>       USB ケーブルが繋がっているか
//   toggle          STK_HID_SWITCH
//   set <BLE|USB>   明示的に選ぶ
//   nvs clear       保存を消す (工場出荷の状態)
//   show            いまの状態を出す
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stackee_hid_dest.h"

static bool    s_usb;
static bool    s_saved_valid;
static uint8_t s_saved;
static int     s_saves;

static bool usb_connected(void) {
    return s_usb;
}

static bool load(uint8_t *out) {
    if (!s_saved_valid) {
        return false;
    }
    *out = s_saved;
    return true;
}

static bool save(uint8_t value) {
    s_saved = value;
    s_saved_valid = true;
    s_saves++;
    return true;
}

static const stackee_hid_dest_io_t IO = {
    .usb_connected = usb_connected,
    .load = load,
    .save = save,
};

int main(void) {
    char line[256];
    stackee_hid_dest_init(&IO);
    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *cmd = strtok(line, " \t\r\n");
        if (cmd == NULL) {
            continue;
        }
        if (strcmp(cmd, "init") == 0) {
            stackee_hid_dest_init(&IO);
        } else if (strcmp(cmd, "usb") == 0) {
            s_usb = atoi(strtok(NULL, " \t\r\n")) != 0;
        } else if (strcmp(cmd, "toggle") == 0) {
            stackee_hid_dest_toggle();
        } else if (strcmp(cmd, "set") == 0) {
            const char *what = strtok(NULL, " \t\r\n");
            stackee_hid_dest_set(strcmp(what, "USB") == 0 ? STACKEE_HID_USB
                                                          : STACKEE_HID_BLE);
        } else if (strcmp(cmd, "nvs") == 0) {
            s_saved_valid = false;
            s_saves = 0;
        } else if (strcmp(cmd, "show") == 0) {
            printf("sel=%s eff=%s saves=%d saved=%s\n",
                   stackee_hid_dest_name(stackee_hid_dest_selected()),
                   stackee_hid_dest_name(stackee_hid_dest_effective()),
                   s_saves,
                   s_saved_valid ? stackee_hid_dest_name(
                                       (stackee_hid_dest_t)s_saved)
                                 : "-");
        } else {
            fprintf(stderr, "知らない命令: %s\n", cmd);
            return 2;
        }
    }
    return 0;
}
