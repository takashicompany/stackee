// QMK の立ち上げと 1 周ぶんの実行。
//
// QMK 本体の main.c (protocol_setup / protocol_pre_init / ...) は使わない。
// あれは「MCU を起こしてから無限ループを回す」ためのもので、ESP-IDF では
// FreeRTOS のタスクがその役をする。ここでは keyboard_init / keyboard_task と
// host_set_driver だけを、必要な順番で呼ぶ。
#include "qmk_port.h"

#include "host.h"
#include "keyboard.h"
#include "quantum.h"
#include "stackee_keymap_migrate.h"
#include "stackee_report_queue.h"

host_driver_t *stackee_qmk_host_driver(void);

void stackee_qmk_init(void) {
    stackee_report_queue_init();
    stackee_qmk_eeprom_init();
    timer_init();
    host_set_driver(stackee_qmk_host_driver());
    // keyboard_init() の中で matrix_init / eeconfig_init / via_init が走る。
    keyboard_init();
    // ★ そのあと。保存済みの配列 (VIA で変えたもの) に、既定を変えたぶんを
    //   当てる。ここより前だと dynamic_keymap がまだ用意できていない。
    stackee_keymap_migrate();
}

void stackee_qmk_task(void) {
    keyboard_task();
}
