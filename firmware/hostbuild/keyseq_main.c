// 打鍵列テストの土台。DESIGN.md §8「キー処理は Mac 上でホストビルドして
// 打鍵列テストを回す」。
//
// QMK の quantum + qmk_port を Mac 用にそのままビルドし、標準入力から
// 「時刻つきの押下 / 解放の列」を流し込んで、出てきた HID レポートの列を
// 印字する。期待値を持つのは Python 側 (tools/test_keyseq_host.py)。
//
// 台本の書き方 (1 行 1 命令、# 以降はコメント):
//
//   t <ms>          時刻を <ms> まで進める。1 ms ずつ keyboard_task を回す
//   d <row> <col>   そのキーを押す
//   u <row> <col>   そのキーを離す
//   D <slot>        スロット番号 (0..49) で押す
//   U <slot>        スロット番号で離す
//   v <hex...>      Raw HID (VIA) の受信 1 パケット
//   kc <row> <col> <keycode>   そのスロットのキーコードを差し替える
//                              (VIA の dynamic keymap 経由。マウスキーなど、
//                               既定配列に無いキーを試すため)
//   ovf             FIFO 溢れ相当。押下中を全部離す
//   mark <文字列>   出力に目印を入れる
//
// 出るもの:
//
//   KB <mods> <k0> <k1> <k2> <k3> <k4> <k5>   キーボードレポート (16 進)
//   CONS <usage> / SYS <usage>                コンシューマ / システム
//   MOUSE <...>                               マウス
//   RAW <hex...>                              Raw HID の応答
//   CUSTOM <名前> <1|0>                       独自キー (HID には出ない)
//   BOOTLOADER / RESET                        QK_BOOT の行き先
//
// ★ ここで通っても実機で動く保証にはならない。見ているのは
//   「同じ押下列に同じレポート列が出るか」だけで、I2C も USB も入っていない。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "keyboard.h"
#include "qmk_port.h"
#include "quantum.h"
#include "dynamic_keymap.h"
#include "raw_hid.h"
#include "stackee_report_queue.h"

// ---------------------------------------------------------------------------
// 時計 (テストが進める)
// ---------------------------------------------------------------------------
static uint32_t s_now_ms;

uint32_t stackee_qmk_now_ms(void) {
    return s_now_ms;
}

void stackee_qmk_set_now_ms(uint32_t ms) {
    s_now_ms = ms;
}

// ---------------------------------------------------------------------------
// ハードに触る出口 (印字するだけ)
// ---------------------------------------------------------------------------
static void drain(void);

void stackee_qmk_delay_ms(uint32_t ms) {
    s_now_ms += ms;
}

void stackee_qmk_enter_rom_download(void) {
    printf("BOOTLOADER\n");
}

void stackee_qmk_restart(void) {
    printf("RESET\n");
}

void stackee_qmk_custom_key(stackee_key_action_t action, bool pressed) {
    printf("CUSTOM %s %d\n", stackee_key_action_name(action), pressed ? 1 : 0);
}

// EEPROM の保存先は RAM。再起動をまたぐ話は実機でしか確かめられない。
static uint8_t s_nvs[1024];
static bool    s_nvs_valid;
static int     s_nvs_saves;

bool stackee_qmk_eeprom_backend_load(uint8_t *buf, size_t len) {
    if (!s_nvs_valid) {
        return false;
    }
    memcpy(buf, s_nvs, len > sizeof(s_nvs) ? sizeof(s_nvs) : len);
    return true;
}

bool stackee_qmk_eeprom_backend_save(const uint8_t *buf, size_t len) {
    memcpy(s_nvs, buf, len > sizeof(s_nvs) ? sizeof(s_nvs) : len);
    s_nvs_valid = true;
    s_nvs_saves++;
    return true;
}

// ---------------------------------------------------------------------------
// レポートの吐き出し
// ---------------------------------------------------------------------------
static void drain(void) {
    stackee_report_t report;
    while (stackee_report_queue_pop(&report)) {
        switch (report.kind) {
            case STACKEE_REPORT_KEYBOARD:
                printf("KB %02X", report.data[0]);
                for (int i = 0; i < 6; i++) {
                    printf(" %02X", report.data[2 + i]);
                }
                printf("\n");
                break;
            case STACKEE_REPORT_CONSUMER:
                printf("CONS %04X\n",
                       (unsigned)(report.data[0] | (report.data[1] << 8)));
                break;
            case STACKEE_REPORT_SYSTEM:
                printf("SYS %04X\n",
                       (unsigned)(report.data[0] | (report.data[1] << 8)));
                break;
            case STACKEE_REPORT_MOUSE:
                printf("MOUSE");
                for (int i = 0; i < report.len; i++) {
                    printf(" %02X", report.data[i]);
                }
                printf("\n");
                break;
            case STACKEE_REPORT_RAW:
                printf("RAW");
                for (int i = 0; i < report.len; i++) {
                    printf(" %02X", report.data[i]);
                }
                printf("\n");
                break;
            default:
                break;
        }
    }
}

// 1 ms 進めて 1 周回す = 実機の input タスク 1 周ぶん。
static void step_1ms(void) {
    s_now_ms++;
    stackee_qmk_task();
    stackee_qmk_eeprom_task();
    drain();
}

static void advance_to(uint32_t target_ms) {
    while (s_now_ms < target_ms) {
        step_1ms();
    }
}

int main(void) {
    char line[512];

    stackee_qmk_init();
    // 立ち上げ直後に出るレポート (あれば) を捨てずに見せる。
    drain();

    while (fgets(line, sizeof(line), stdin) != NULL) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *cmd = strtok(line, " \t\r\n");
        if (cmd == NULL) {
            continue;
        }
        if (strcmp(cmd, "t") == 0) {
            advance_to((uint32_t)strtoul(strtok(NULL, " \t\r\n"), NULL, 10));
        } else if (strcmp(cmd, "d") == 0 || strcmp(cmd, "u") == 0) {
            int row = atoi(strtok(NULL, " \t\r\n"));
            int col = atoi(strtok(NULL, " \t\r\n"));
            stackee_qmk_matrix_event((uint8_t)(row * STACKEE_MATRIX_COLS + col),
                                     cmd[0] == 'd');
        } else if (strcmp(cmd, "D") == 0 || strcmp(cmd, "U") == 0) {
            int slot = atoi(strtok(NULL, " \t\r\n"));
            stackee_qmk_matrix_event((uint8_t)slot, cmd[0] == 'D');
        } else if (strcmp(cmd, "ovf") == 0) {
            stackee_qmk_matrix_release_all();
        } else if (strcmp(cmd, "v") == 0) {
            uint8_t packet[32];
            memset(packet, 0, sizeof(packet));
            int count = 0;
            char *token;
            while (count < 32 && (token = strtok(NULL, " \t\r\n")) != NULL) {
                packet[count++] = (uint8_t)strtoul(token, NULL, 16);
            }
            raw_hid_receive(packet, 32);
            drain();
        } else if (strcmp(cmd, "kc") == 0) {
            int      row = atoi(strtok(NULL, " \t\r\n"));
            int      col = atoi(strtok(NULL, " \t\r\n"));
            uint16_t kc = (uint16_t)strtoul(strtok(NULL, " \t\r\n"), NULL, 0);
            // VIA と同じ道 (dynamic keymap) で差し替える。実機で VIA から
            // 割り当てたのと同じ状態を作るため。
            dynamic_keymap_set_keycode(0, (uint8_t)row, (uint8_t)col, kc);
        } else if (strcmp(cmd, "mark") == 0) {
            char *rest = strtok(NULL, "\r\n");
            printf("MARK %s\n", rest ? rest : "");
        } else if (strcmp(cmd, "nvs") == 0) {
            printf("NVS saves=%d valid=%d\n", s_nvs_saves, s_nvs_valid ? 1 : 0);
        } else {
            fprintf(stderr, "台本に知らない命令: %s\n", cmd);
            return 2;
        }
    }
    drain();
    return 0;
}
