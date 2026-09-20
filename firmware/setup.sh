#!/usr/bin/env bash
# Stackee ネイティブファーム — 下ごしらえ (clone した直後に 1 回だけ走らせる)
#
# clone しただけの木には **取得物が入っていない**。この土台をビルドするのに
# 要るのは 3 つで、どれもここで揃える。
#
#   1. ESP-IDF v6.0.3        (upstream espressif/esp-idf のタグ v6.0.3)
#   2. ツールチェーン        (xtensa-esp-elf ほか。IDF の install.sh が入れる)
#   3. ホスト側の Python     (tools/ の道具が使う。requirements.txt)
#
# TinyUSB (managed_components/) は書いていない。**ビルドの初回に IDF の
# コンポーネントマネージャが dependencies.lock どおりに取ってくる**ので、
# ここでやることは無い。
#
#   ./setup.sh                 # 上の 3 つ全部
#   ./setup.sh --dry-run       # 何をするかだけ出す (1 バイトも取りに行かない)
#   ./setup.sh --idf-only      # 1 と 2 だけ
#   ./setup.sh --python-only   # 3 だけ
#   ./setup.sh --venv          # 3 を専用の venv (<IDF の親>/venv) に入れる
#
# 置き場所 (環境変数で変えられる):
#   ESP-IDF         $STACKEE_IDF_PATH         既定 ~/.local/share/stackee/esp-idf-v6.0.3
#   ツールチェーン  $STACKEE_IDF_TOOLS_PATH   既定 <IDF の親>/.idf_tools-v6.0.3
#
# 既定の置き場はリポジトリの**外**にしてある。clone し直しても取り直しに
# ならないし、`git status` が取得物で汚れない。build.sh はこの場所を
# 自分で見つける (build.sh の頭のコメントの一覧の 5 番目)。
#
# **何度走らせても壊れない**。すでに揃っているところは飛ばす。途中で切れた
# clone も、submodule を入れ直して続きから直す。
#
# 動く環境: macOS (Apple Silicon / Intel) と Ubuntu 22.04 以降。
# 実機には一切触らない。書き込みは tools/flash.py (README §5)。

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
IDF_TAG="v6.0.3"
IDF_URL="https://github.com/espressif/esp-idf.git"
TARGET="esp32s3"

DO_IDF=1
DO_PYTHON=1
DRY_RUN=0
USE_VENV=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)     DRY_RUN=1 ;;
        --idf-only)    DO_PYTHON=0 ;;
        --python-only) DO_IDF=0 ;;
        --venv)        USE_VENV=1 ;;
        -h|--help)
            sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "知らない引数: $1  (--help)" >&2
            exit 2 ;;
    esac
    shift
done

IDF_PATH_WANT="${STACKEE_IDF_PATH:-$HOME/.local/share/stackee/esp-idf-$IDF_TAG}"
IDF_TOOLS_WANT="${STACKEE_IDF_TOOLS_PATH:-$(dirname "$IDF_PATH_WANT")/.idf_tools-$IDF_TAG}"
VENV_DIR="$(dirname "$IDF_PATH_WANT")/venv"

say()  { printf '\n=== %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
run()  {
    if [ "$DRY_RUN" = 1 ]; then
        printf '    [dry-run] %s\n' "$*"
        return 0
    fi
    printf '    $ %s\n' "$*"
    "$@"
}

# --- 0. 走る場所を確かめる --------------------------------------------------
say "0. 環境"
case "$(uname -s)" in
    Darwin) OS=macos ;;
    Linux)  OS=linux ;;
    *)
        echo "対応していない OS: $(uname -s) (macOS か Linux)" >&2
        exit 1 ;;
esac
info "OS: $OS ($(uname -m))"

MISSING=""
for cmd in git python3 cmake; do
    command -v "$cmd" > /dev/null 2>&1 || MISSING="$MISSING $cmd"
done
# ESP-IDF の install.sh は cmake / ninja も自分で入れるので、無くても致命傷では
# ない。git と python3 だけは先に要る。
if ! command -v git > /dev/null 2>&1 || ! command -v python3 > /dev/null 2>&1; then
    echo "git と python3 が要る (足りない:$MISSING)" >&2
    if [ "$OS" = linux ]; then
        echo "  sudo apt-get install -y git wget flex bison gperf python3 python3-pip \\" >&2
        echo "       python3-venv cmake ninja-build ccache libffi-dev libssl-dev \\" >&2
        echo "       dfu-util libusb-1.0-0" >&2
    else
        echo "  xcode-select --install   # Command Line Tools" >&2
    fi
    exit 1
fi
PY="$(command -v python3)"
PY_VER="$("$PY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
info "python3: $PY_VER ($PY)"
case "$PY_VER" in
    3.9|3.1[0-9]) : ;;
    *) info "! ESP-IDF v6.0.3 が見ているのは Python 3.9 以降。3.8 以前だと install.sh が断る" ;;
esac
if command -v cc > /dev/null 2>&1; then
    info "cc: $(cc --version 2>&1 | head -1)"
else
    info "! cc が無い。ホスト側のテスト (tools/test_*_host.py) はこれでビルドする"
    if [ "$OS" = linux ]; then
        info "  sudo apt-get install -y build-essential"
    else
        info "  xcode-select --install"
    fi
fi
if [ "$OS" = linux ]; then
    info "Ubuntu で初めてなら先に:"
    info "  sudo apt-get install -y git wget flex bison gperf python3 python3-pip \\"
    info "       python3-venv cmake ninja-build ccache libffi-dev libssl-dev \\"
    info "       dfu-util libusb-1.0-0 build-essential"
fi

# --- 1. ESP-IDF v6.0.3 ------------------------------------------------------
if [ "$DO_IDF" = 1 ]; then
    say "1. ESP-IDF $IDF_TAG -> $IDF_PATH_WANT"
    if [ -f "$IDF_PATH_WANT/export.sh" ] && [ -d "$IDF_PATH_WANT/.git" ]; then
        HAVE="$(git -C "$IDF_PATH_WANT" describe --tags --always 2>/dev/null || echo '?')"
        if [ "$HAVE" = "$IDF_TAG" ]; then
            info "すでにある ($HAVE)。clone は飛ばす"
        else
            info "! そこにあるのは $IDF_TAG ではなく $HAVE"
            info "  別の場所に入れるなら STACKEE_IDF_PATH=... を指定して走らせ直す"
            info "  その木を $IDF_TAG に合わせるなら:"
            info "    git -C $IDF_PATH_WANT fetch --depth 1 origin tag $IDF_TAG"
            info "    git -C $IDF_PATH_WANT checkout $IDF_TAG"
            exit 1
        fi
    elif [ -e "$IDF_PATH_WANT" ] && [ ! -d "$IDF_PATH_WANT/.git" ]; then
        info "! $IDF_PATH_WANT があるが git の木ではない。どけてから走らせ直す"
        exit 1
    else
        run mkdir -p "$(dirname "$IDF_PATH_WANT")"
        # 既定は浅い clone (タグ 1 本ぶん)。submodule も浅くする。
        # 浅い clone で困ったら STACKEE_IDF_FULL_CLONE=1 で通常の clone にする。
        if [ "${STACKEE_IDF_FULL_CLONE:-0}" = 1 ]; then
            run git clone --branch "$IDF_TAG" --recursive "$IDF_URL" "$IDF_PATH_WANT"
        else
            run git clone --branch "$IDF_TAG" --depth 1 --recursive \
                --shallow-submodules "$IDF_URL" "$IDF_PATH_WANT"
        fi
    fi
    # 途中で切れた clone を直す口。全部揃っていれば一瞬で返る。
    if [ -d "$IDF_PATH_WANT/.git" ]; then
        run git -C "$IDF_PATH_WANT" submodule update --init --recursive
    fi

    # --- 2. ツールチェーン --------------------------------------------------
    say "2. ツールチェーン ($TARGET) -> $IDF_TOOLS_WANT"
    info "コンパイラ・gdb・openocd・IDF 自身の Python 環境。4〜5 GB 使う"
    if [ "$DRY_RUN" = 1 ]; then
        printf '    [dry-run] IDF_TOOLS_PATH=%s %s/install.sh %s\n' \
            "$IDF_TOOLS_WANT" "$IDF_PATH_WANT" "$TARGET"
    else
        mkdir -p "$IDF_TOOLS_WANT"
        # install.sh は入っているものを見て、足りないぶんだけ取る (冪等)。
        IDF_TOOLS_PATH="$IDF_TOOLS_WANT" "$IDF_PATH_WANT/install.sh" "$TARGET"
    fi
fi

# --- 3. ホスト側の Python ---------------------------------------------------
if [ "$DO_PYTHON" = 1 ]; then
    say "3. ホスト側の Python (tools/ が使う)"
    REQ="$HERE/requirements.txt"
    if [ ! -f "$REQ" ]; then
        echo "requirements.txt が無い: $REQ" >&2
        exit 1
    fi
    info "これは **ホストの python3** に入れる。IDF が install.sh で作る"
    info "自前の venv とは別物 (tools/ は idf.py を通さずに走るため)"
    if [ "$USE_VENV" = 1 ]; then
        run "$PY" -m venv "$VENV_DIR"
        if [ "$DRY_RUN" = 1 ]; then
            printf '    [dry-run] %s/bin/pip install -r %s\n' "$VENV_DIR" "$REQ"
        else
            "$VENV_DIR/bin/python" -m pip install --upgrade pip > /dev/null
            "$VENV_DIR/bin/python" -m pip install -r "$REQ"
        fi
        info "使うとき: source $VENV_DIR/bin/activate"
    elif [ -n "${VIRTUAL_ENV:-}" ]; then
        info "venv の中にいる: $VIRTUAL_ENV"
        run "$PY" -m pip install -r "$REQ"
    else
        if [ "$DRY_RUN" = 1 ]; then
            printf '    [dry-run] %s -m pip install --user -r %s\n' "$PY" "$REQ"
        elif ! "$PY" -m pip install --user -r "$REQ"; then
            echo "" >&2
            echo "pip が断った。OS 管理の Python だと --user も断られることがある" >&2
            echo "(PEP 668 / externally-managed-environment)。どちらかで逃げる:" >&2
            echo "  ./setup.sh --python-only --venv        # 専用の venv に入れる" >&2
            echo "  $PY -m pip install --break-system-packages --user -r $REQ" >&2
            exit 1
        fi
    fi
    # hidapi は Python パッケージ (hid) から名前で dlopen する C ライブラリ。
    # pip では入らないので OS の口から入れる。無くても --transport serial は動く。
    if [ "$OS" = macos ]; then
        info "Raw HID (console_hid.py --transport hid) を使うなら: brew install hidapi"
    else
        info "Raw HID (console_hid.py --transport hid) を使うなら: sudo apt-get install -y libhidapi-hidraw0"
    fi
fi

# --- 4. できたか見る --------------------------------------------------------
say "4. 確かめ"
if [ "$DRY_RUN" = 1 ]; then
    info "[dry-run] ここまで。実際には何も取っていない"
    exit 0
fi
if [ "$DO_IDF" = 1 ]; then
    set +u
    # shellcheck disable=SC1091
    IDF_TOOLS_PATH="$IDF_TOOLS_WANT" . "$IDF_PATH_WANT/export.sh" > /dev/null 2>&1
    set -u
    info "idf.py: $(idf.py --version 2>/dev/null || echo '見つからない')"
fi
info "次はこれ:"
if [ "${STACKEE_IDF_PATH:-}" != "" ] || [ "$IDF_PATH_WANT" != "$HOME/.local/share/stackee/esp-idf-$IDF_TAG" ]; then
    info "  export STACKEE_IDF_PATH=$IDF_PATH_WANT"
    info "  export STACKEE_IDF_TOOLS_PATH=$IDF_TOOLS_WANT"
fi
info "  $HERE/build.sh                 # dev  -> build/stackee.bin"
info "  $HERE/build.sh --profile full  # full -> build-full/stackee.bin"
info "  for t in $HERE/tools/test_*.py; do python3 \"\$t\"; done"
