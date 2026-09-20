// QMK の QK_BOOT / QK_RBT の出口。実体は qmk_port_stubs.c。
#pragma once

void bootloader_jump(void);
void mcu_reset(void);
