#!/bin/bash
# Stackee native firmware build.
#
# ESP-IDF は **upstream の espressif/esp-idf v6.0.3** を使う。
# 取得物なのでこのリポジトリには入っていない。用意の仕方は README.md §3。
# 2026-09-18 まで使っていた Adafruit fork (v6.0.1 相当) には、ESP32-S3 の
# ハードウェア MPI で署名検証が落ちる不具合の修正 (c41dd724d / 86f6192f1) が
# 入っていなかった。
#
# 場所の決め方 (上から順に見て、export.sh があるものを使う):
#   1. STACKEE_IDF_PATH          この土台だけで使う置き場
#   2. IDF_PATH                  すでに用意してある環境をそのまま使う
#   3. <firmware の隣>/esp-idf-v6.0.3
#   4. <親リポジトリ>/firmware/esp-idf-v6.0.3   (開発元の置き方)
# ツールチェーンも同じ順で STACKEE_IDF_TOOLS_PATH / IDF_TOOLS_PATH /
# 同じ階層の .idf_tools-v6.0.3。どちらも取得物なのでリポジトリには入れない。
#
#   ./build.sh                    # dev プロファイル (CDC + HID + Raw HID)
#   ./build.sh --profile full     # full プロファイル (HID + Raw HID + UAC マイク)
#   ./build.sh clean              # remove the build directory first
#   ./build.sh size               # build then print the size breakdown
#
# ★ プロファイルごとにビルドディレクトリを分ける (build / build-full)。
#   同じディレクトリで切り替えると、tusb_config.h の違いが
#   インクリメンタルビルドに拾われず、混ざった像ができる。
#
# It never talks to the device. Writing to the board is tools/flash.py.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
BASE="$(cd "$HERE/.." && pwd)"          # 公開: リポジトリ直下 / 開発元: public
OUTER="$(cd "$BASE/.." && pwd)"         # 開発元の親リポジトリ (公開側では外)

# --- ESP-IDF の場所 ---------------------------------------------------------
IDF_DEFAULT="$BASE/esp-idf-v6.0.3"
if [ -f "$OUTER/firmware/esp-idf-v6.0.3/export.sh" ] \
   && [ ! -f "$IDF_DEFAULT/export.sh" ]; then
    IDF_DEFAULT="$OUTER/firmware/esp-idf-v6.0.3"
fi
TOOLS_DEFAULT="$(dirname "$IDF_DEFAULT")/.idf_tools-v6.0.3"
export IDF_PATH="${STACKEE_IDF_PATH:-${IDF_PATH:-$IDF_DEFAULT}}"
if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF v6.0.3 が無い: $IDF_PATH" >&2
    echo "用意する: git clone --depth 1 --branch v6.0.3 --recursive \\" >&2
    echo "            https://github.com/espressif/esp-idf.git $IDF_DEFAULT" >&2
    echo "  (別の場所のものを使うなら STACKEE_IDF_PATH=... か IDF_PATH=... を指定)" >&2
    exit 1
fi

# --- ツールチェーンの場所 ---------------------------------------------------
# コンパイラ自体 (xtensa-esp-elf 15.2.0_20251204) は 2026-09-18 まで使っていた
# 土台と同じ版だが、gdb (16.3 → 17.1) / openocd / esp-rom-elfs が違うので
# **別の場所**に入れる (古いほうの .idf_tools は書き換えない)。
export IDF_TOOLS_PATH="${STACKEE_IDF_TOOLS_PATH:-${IDF_TOOLS_PATH:-$TOOLS_DEFAULT}}"
# TinyUSB は IDF Component Registry から取る (main/idf_component.yml で版固定)。
# 初回だけ取りに行き、以後は managed_components/ と dependencies.lock を使う。
export IDF_COMPONENT_MANAGER=1
if ! source "$IDF_PATH/export.sh" > /dev/null 2>&1; then
    echo "export.sh が失敗した。ツールチェーンが未導入なら:" >&2
    echo "  IDF_TOOLS_PATH=$IDF_TOOLS_PATH $IDF_PATH/install.sh esp32s3" >&2
    exit 1
fi

cd "$HERE"

PROFILE="dev"
if [ "$1" = "--profile" ]; then
    PROFILE="$2"
    shift 2
fi
if [ "$PROFILE" != "dev" ] && [ "$PROFILE" != "full" ]; then
    echo "profile は dev か full (いまは '$PROFILE')" >&2
    exit 1
fi
export STACKEE_USB_PROFILE="$PROFILE"
BUILD="build"
if [ "$PROFILE" = "full" ]; then
    BUILD="build-full"
fi

if [ "$1" = "clean" ]; then
    rm -rf "$BUILD"
    shift
fi

echo "=== IDF: $(idf.py --version 2>/dev/null) / profile: $PROFILE / dir: $BUILD ==="
START=$(date +%s)
idf.py -B "$BUILD" build
END=$(date +%s)
echo "=== build took $((END - START))s ==="

APP="$HERE/$BUILD/stackee.bin"
if [ -f "$APP" ]; then
    SIZE=$(stat -f%z "$APP" 2>/dev/null || stat -c%s "$APP")
    echo "app:    $APP"
    echo "bytes:  $SIZE  (ota_0 = 2097152, $((SIZE * 100 / 2097152))% used)"
    echo "sha256: $(shasum -a 256 "$APP" | cut -d' ' -f1)"
    if [ "$SIZE" -gt 2097152 ]; then
        echo "!! app does not fit in ota_0" >&2
        exit 1
    fi
fi

if [ "$1" = "size" ]; then
    idf.py -B "$BUILD" size
fi
