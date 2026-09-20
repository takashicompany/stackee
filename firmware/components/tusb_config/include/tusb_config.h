// TinyUSB configuration for Stackee. 2 profiles (DESIGN.md §4).
//
//   dev  : CDC(2 IN) + HID(1 IN) + Raw HID(1 IN) + EP0(1) = 5 IN endpoints
//   full : HID(1 IN) + Raw HID(1 IN) + UAC mic(1 IN)      + EP0(1) = 4 IN
//
// 5 is the ESP32-S3 limit, so `dev` is exactly full. `full` drops CDC and
// puts the console/log on Raw HID instead (VIA custom command ids 0xC0..).
//
// Pick with the build: `./build.sh --profile full` (or STACKEE_USB_PROFILE=full).
// The top-level CMakeLists turns that into -DSTACKEE_USB_PROFILE_FULL=1.
#pragma once

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

// ★ CFG_TUSB_MCU はレジストリ版 TinyUSB の CMakeLists が付ける。
//   CFG_TUSB_OS は付けてくれないのでここで決める (段階 0 では
//   components/tinyusb/CMakeLists.txt が渡していた)。
#include "tusb_option.h"
#ifndef CFG_TUSB_OS
#    define CFG_TUSB_OS OPT_OS_FREERTOS
#endif
#define CFG_TUSB_OS_INC_PATH        freertos/
#define CFG_TUSB_DEBUG              0

#ifndef STACKEE_USB_PROFILE_FULL
#    define STACKEE_USB_PROFILE_FULL 0
#endif

#define CFG_TUD_ENABLED             1
#define CFG_TUH_ENABLED             0
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUD_MAX_SPEED           OPT_MODE_FULL_SPEED

// Same as CircuitPython on this chip (supervisor/shared/usb/tusb_config.h:95).
#define CFG_TUD_DWC2_DMA_ENABLE     1

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN          __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE      64

// ---- classes ---------------------------------------------------------------
#if STACKEE_USB_PROFILE_FULL
#    define CFG_TUD_CDC             0
#    define CFG_TUD_AUDIO           1
#else
#    define CFG_TUD_CDC             1
#    define CFG_TUD_AUDIO           0
#endif
#define CFG_TUD_HID                 2   // 0 = keyboard/mouse/consumer, 1 = raw
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0

// The console frames whole JSON lines; give it room so a status reply is one
// write and the device never blocks waiting for the host to drain.
#define CFG_TUD_CDC_RX_BUFSIZE      512
#define CFG_TUD_CDC_TX_BUFSIZE      2048
#define CFG_TUD_CDC_EP_BUFSIZE      64

// 64 so the 64-byte Stackee console reports fit in one packet.
#define CFG_TUD_HID_EP_BUFSIZE      64

// ---- UAC2 microphone (full profile only) -----------------------------------
//
// 16 kHz / mono / 16-bit signed LE. Same numbers as the CircuitPython build
// (firmware/cp-uac: boot.py calls usb_audio.enable(sample_rate=16000,
// channel_count=1)), and the same shape of configuration as its
// supervisor/shared/usb/tusb_config.h.
//
// ★ SZ_MAX は「1 フレーム (1 ms) ぶん + 1 サンプル」。full speed の等時
//   転送は 1 ms ごとに 1 パケットなので、16000/1000 = 16 サンプル。
//   TinyUSB の TUD_AUDIO_EP_SIZE と同じ式を手で書いてある
//   (この段階では usbd.h をまだ読めないため)。
//     ((16000 + 999) / 1000) * 2 bytes * 1 ch = 32
#if CFG_TUD_AUDIO
#    define CFG_TUD_AUDIO_FUNC_1_N_AS_INT            1
#    define CFG_TUD_AUDIO_CTRL_BUF_SZ                64
#    define CFG_TUD_AUDIO_ENABLE_EP_IN               1
#    define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX        32
// ★ 深めの FIFO。audio タスクは 1 周 5 ms なので、1 ms ごとの吸い出しに
//   対して 16 パケットぶんの余裕を持たせる (CircuitPython 版と同じ 16 倍)。
#    define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ     (16 * CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX)
#    define CFG_TUD_AUDIO_ENABLE_EP_OUT              0
#endif

#ifdef __cplusplus
}
#endif
