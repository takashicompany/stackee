// QMK が呼ぶが Stackee では中身の要らない関数たち。
//
// 「何もしない」ことに理由があるものだけを残してある。理由の無いスタブは
// 置かない (置くと、動いていないのか呼ばれていないのか分からなくなる)。
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "bootloader.h"
#include "qmk_port.h"
#include "suspend.h"
#include "wait.h"

// ---------------------------------------------------------------------------
// 待ち
// ---------------------------------------------------------------------------
// QMK の中で wait を使うのは起動時とブートローダへ飛ぶ直前だけ。入力タスクの
// 経路には出てこない。実体は実機側 (stackee_qmk_delay_ms) が持つ。
void wait_ms(uint32_t ms) {
    stackee_qmk_delay_ms(ms);
}

void wait_us(uint32_t us) {
    stackee_qmk_delay_ms((us + 999) / 1000);
}

// ---------------------------------------------------------------------------
// サスペンド
// ---------------------------------------------------------------------------
// USB のサスペンドで画面を消したり BLE へ切り替えたりするのは段階 2 以降。
// ここでは QMK 側の内部状態だけ整える。
void suspend_power_down(void) {
    suspend_power_down_quantum();
}

void suspend_wakeup_init(void) {
    suspend_wakeup_init_quantum();
}

// ---------------------------------------------------------------------------
// 再起動 / ブートローダ
// ---------------------------------------------------------------------------
// QK_BOOT (KMK の KC.RESET と同じ位置にある) を押したときの行き先。
// CircuitPython 版の KC.RESET は microcontroller.reset() = 普通の再起動
// だったが、こちらは **ROM のダウンロードモード** へ入れる。書き込み用の
// 脱出路を本体側にも持たせるため (tools/flash.py の 1200bps タッチが
// 効かない状況でも、このキーで ROM へ落ちられる)。
void bootloader_jump(void) {
    stackee_qmk_enter_rom_download();
}

void mcu_reset(void) {
    stackee_qmk_restart();
}

// ---------------------------------------------------------------------------
// デバッグ出力の出口 (_print.h)
// ---------------------------------------------------------------------------
// QMK の dprintf / xprintf はここへ来る。sendchar (quantum/logging/sendchar.c)
// が指す先はコンソール側で差し替えられるが、段階 1 ではログに流すだけ。
void stackee_qmk_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
