# Stackee native firmware (ESP-IDF). See DESIGN.md / README.md.
#
# The app is built for the ota_0 slot (0x10000, 2048K) of the CircuitPython
# 16MB partition table, so tools/flash.py can swap it in and out without
# touching the partition table, nvs or user_fs.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# TinyUSB (レジストリ版) は tusb_config.h を「インクルードパスのどこかに
# ある」前提で読む。自分から components/tusb_config に依存してはくれないので、
# ビルド全体のインクルードパスに足しておく。CFG_TUSB_MCU はレジストリ版の
# CMakeLists が自分で付ける。CFG_TUSB_OS は tusb_config.h 側で決める。
idf_build_set_property(COMPILE_OPTIONS
    "-I${CMAKE_CURRENT_LIST_DIR}/components/tusb_config/include" APPEND)

# ---------------------------------------------------------------------------
# USB プロファイル (DESIGN.md §4)
# ---------------------------------------------------------------------------
#   dev  … CDC + HID + Raw HID           (既定。コンソールとログをシリアルで読む)
#   full … HID + Raw HID + UAC マイク    (本番。コンソールは Raw HID に相乗り)
#
# 環境変数 STACKEE_USB_PROFILE か -DSTACKEE_USB_PROFILE=full で切り替える。
# tusb_config.h と stackee_usb.c がこの 1 つの定義だけを見ている。
if(NOT DEFINED STACKEE_USB_PROFILE AND DEFINED ENV{STACKEE_USB_PROFILE})
    set(STACKEE_USB_PROFILE "$ENV{STACKEE_USB_PROFILE}")
endif()
if(NOT STACKEE_USB_PROFILE)
    set(STACKEE_USB_PROFILE "dev")
endif()
if(STACKEE_USB_PROFILE STREQUAL "full")
    idf_build_set_property(COMPILE_OPTIONS "-DSTACKEE_USB_PROFILE_FULL=1" APPEND)
elseif(NOT STACKEE_USB_PROFILE STREQUAL "dev")
    message(FATAL_ERROR "STACKEE_USB_PROFILE は dev か full (いまは '${STACKEE_USB_PROFILE}')")
endif()
message(STATUS "Stackee USB profile: ${STACKEE_USB_PROFILE}")

# ---------------------------------------------------------------------------
# 版 (esp_app_desc_t.version)
# ---------------------------------------------------------------------------
# ★ ビルドした木の `git describe` をそのまま像に焼く。操作盤の
#   「ファームウェア更新」は **いま載っている版 / これから書く版 / sha256** を
#   並べて見せるので、像が自分の出所を名乗れないと突き合わせられない。
#   esp_app_desc_t.version は 32 バイト。長すぎるものは切る。
#   sdkconfig の CONFIG_APP_PROJECT_VER_FROM_CONFIG は **切ってある**
#   (立っていると、こちらの PROJECT_VER が捨てられる)。
#
# ★ 評価されるのは cmake の構成時だけ。コミットしたあとに版を入れ直すには
#   `./build.sh clean` か `idf.py -B <dir> reconfigure` を挟むこと。
execute_process(
    COMMAND git describe --always --dirty --tags
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE STACKEE_GIT_VER
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT STACKEE_GIT_VER)
    set(STACKEE_GIT_VER "nogit")
endif()
string(SUBSTRING "${STACKEE_GIT_VER}" 0 31 STACKEE_GIT_VER)
set(PROJECT_VER "${STACKEE_GIT_VER}")
message(STATUS "Stackee version: ${PROJECT_VER}")

project(stackee)
