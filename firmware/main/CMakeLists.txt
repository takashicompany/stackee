# Stackee 本体 + QMK の橋渡し層。
#
# QMK の quantum は third_party/qmk に**コピー**してある (サブモジュールでは
# ない。IMPORT.md に版とファイル一覧)。ここではそのファイルを 1 文字も
# 直さずにビルドする。設定は QMK と同じ作法で -include で差し込む。
set(QMK "${CMAKE_CURRENT_LIST_DIR}/../third_party/qmk")
set(PORT "${CMAKE_CURRENT_LIST_DIR}/qmk_port")

idf_component_register(
    SRCS
        # --- Stackee 本体 -----------------------------------------------
        "stackee_main.c"
        "stackee_usb.c"
        "stackee_console.c"
        "stackee_cryptocheck.c"
        "stackee_lcd.c"
        "stackee_font8x8.c"
        "stackee_board.c"
        "stackee_assets.c"
        "stackee_perf.c"
        "stackee_logbuf.c"
        "stackee_tca8418.c"
        "stackee_input.c"
        "stackee_hid_out.c"
        "stackee_hid_dest.c"
        "stackee_report_queue.c"
        "stackee_nvs.c"
        "stackee_ble.c"
        # --- 段階 2: 画面 ------------------------------------------------
        "stackee_ui.c"
        "stackee_draw.c"
        "stackee_icons.c"
        "stackee_bdf.c"
        "stackee_font16.c"
        "stackee_faceanim.c"
        "stackee_selftest.c"
        "stackee_crc32.c"
        # --- 段階 3: 音声と通信 ------------------------------------------
        "stackee_jsonlite.c"
        "stackee_settings.c"
        "stackee_wifistore.c"
        "stackee_wifism.c"
        "stackee_wifi.c"
        "stackee_talksm.c"
        "stackee_http.c"
        "stackee_codec.c"
        "stackee_audio.c"
        "stackee_volume_core.c"
        "stackee_volume.c"
        # --- 段階 4: 周辺機能と本番構成 ----------------------------------
        "stackee_touch_core.c"
        "stackee_touch.c"
        "stackee_camera.c"
        "stackee_fat.c"
        "stackee_conhid.c"
        "stackee_uac.c"
        # --- アプリ内 OTA (Raw HID から ota_1 へ) ------------------------
        "stackee_otacore.c"
        "stackee_ota.c"
        # --- 橋渡し層 (自前) --------------------------------------------
        "qmk_port/qmk_port_timer.c"
        "qmk_port/qmk_port_matrix.c"
        "qmk_port/qmk_port_host.c"
        "qmk_port/qmk_port_eeprom.c"
        "qmk_port/qmk_port_stubs.c"
        "qmk_port/qmk_port_init.c"
        "qmk_port/stackee_holdtap.c"
        # --- QMK quantum (third_party、無改変) --------------------------
        # keymaps/default_keymap.c はここに並べない。QMK の作法どおり
        # keymap_introspection.c が KEYMAP_C として include する。
        "${QMK}/quantum/quantum.c"
        "${QMK}/quantum/keyboard.c"
        "${QMK}/quantum/action.c"
        "${QMK}/quantum/action_tapping.c"
        "${QMK}/quantum/action_util.c"
        "${QMK}/quantum/action_layer.c"
        "${QMK}/quantum/keycode_config.c"
        "${QMK}/quantum/keymap_common.c"
        "${QMK}/quantum/keymap_introspection.c"
        "${QMK}/quantum/matrix_common.c"
        "${QMK}/quantum/eeconfig.c"
        "${QMK}/quantum/dynamic_keymap.c"
        "${QMK}/quantum/via.c"
        "${QMK}/quantum/bitwise.c"
        "${QMK}/quantum/led.c"
        "${QMK}/quantum/mousekey.c"
        "${QMK}/quantum/sync_timer.c"
        "${QMK}/quantum/process_keycode/process_quantum.c"
        "${QMK}/quantum/process_keycode/process_default_layer.c"
        "${QMK}/quantum/debounce/sym_defer_pk.c"
        "${QMK}/quantum/send_string/send_string.c"
        "${QMK}/quantum/logging/debug.c"
        "${QMK}/quantum/logging/print.c"
        "${QMK}/quantum/logging/sendchar.c"
        "${QMK}/quantum/nvm/eeprom/nvm_eeconfig.c"
        "${QMK}/quantum/nvm/eeprom/nvm_dynamic_keymap.c"
        "${QMK}/quantum/nvm/eeprom/nvm_via.c"
        "${QMK}/tmk_core/protocol/host.c"
        "${QMK}/tmk_core/protocol/report.c"
        "${QMK}/tmk_core/protocol/usb_device_state.c"
        "${QMK}/platforms/suspend.c"
    INCLUDE_DIRS
        "."
        "${QMK}/platforms"
        "${QMK}/quantum"
        "${QMK}/quantum/keymap_extras"
        "${QMK}/quantum/logging"
        "${QMK}/quantum/nvm"
        "${QMK}/quantum/nvm/eeprom"
        "${QMK}/quantum/process_keycode"
        "${QMK}/quantum/send_string"
        "${QMK}/quantum/sequencer"
        "${QMK}/tmk_core/protocol"
        # ★ qmk_port は QMK の後ろ。quantum/logging/print.h が
        #   include_next "_print.h" で「自分より後ろ」を探すため。
        "qmk_port"
    REQUIRES
        tinyusb
        esp_driver_i2c
        esp_driver_spi
        esp_lcd
        esp_timer
        esp_hw_support
        fatfs
        nvs_flash
        esp_app_format
        app_update
        esp_partition
        efuse
        bt
        esp_hid
        # --- 段階 3 ------------------------------------------------------
        esp_driver_i2s
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_coex
        esp-tls
        mbedtls
        # --- 段階 4 ------------------------------------------------------
        espressif__esp32-camera
)

# QMK の作法: キーボードの config.h を全翻訳単位へ -include で差し込む
# (qmk_firmware の build_keyboard.mk と同じ)。KEYMAP_C は既定配列の置き場。
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include" "${PORT}/stackee_qmk_config.h"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    KEYMAP_C="${CMAKE_CURRENT_LIST_DIR}/keymaps/default_keymap.c"
)

# QMK 本体は第三者のコード。警告でビルドを止めない。
set_source_files_properties(
    "${QMK}/quantum/quantum.c"
    "${QMK}/quantum/keyboard.c"
    "${QMK}/quantum/action.c"
    "${QMK}/quantum/action_tapping.c"
    "${QMK}/quantum/action_util.c"
    "${QMK}/quantum/action_layer.c"
    "${QMK}/quantum/keycode_config.c"
    "${QMK}/quantum/keymap_common.c"
    "${QMK}/quantum/keymap_introspection.c"
    "${QMK}/quantum/matrix_common.c"
    "${QMK}/quantum/eeconfig.c"
    "${QMK}/quantum/dynamic_keymap.c"
    "${QMK}/quantum/via.c"
    "${QMK}/quantum/bitwise.c"
    "${QMK}/quantum/led.c"
    "${QMK}/quantum/mousekey.c"
    "${QMK}/quantum/sync_timer.c"
    "${QMK}/quantum/process_keycode/process_quantum.c"
    "${QMK}/quantum/process_keycode/process_default_layer.c"
    "${QMK}/quantum/debounce/sym_defer_pk.c"
    "${QMK}/quantum/send_string/send_string.c"
    "${QMK}/quantum/logging/debug.c"
    "${QMK}/quantum/logging/print.c"
    "${QMK}/quantum/logging/sendchar.c"
    "${QMK}/quantum/nvm/eeprom/nvm_eeconfig.c"
    "${QMK}/quantum/nvm/eeprom/nvm_dynamic_keymap.c"
    "${QMK}/quantum/nvm/eeprom/nvm_via.c"
    "${QMK}/tmk_core/protocol/host.c"
    "${QMK}/tmk_core/protocol/report.c"
    "${QMK}/tmk_core/protocol/usb_device_state.c"
    "${QMK}/platforms/suspend.c"
    PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-unused-parameter;-Wno-unused-function;-Wno-unused-variable"
)
