#!/bin/sh
# 素材と設定を本体の FAT (user_fs = CircuitPython の CIRCUITPY) へ置く。
#
# 現行 CircuitPython 版の install.sh に当たるもの。ただし**やり方が違う**:
#
#   CircuitPython 版  … USB ドライブ /Volumes/CIRCUITPY が見えるので cp
#   ネイティブ版      … USB ドライブが無い (MSC を名乗っていない)。
#                       console の `fs.put` でファイルを送り込む
#
# ★ CircuitPython の像に戻せば /Volumes/CIRCUITPY はまた見える。急ぐときは
#   そちらで cp するほうが速い。この道具は「ネイティブ版を載せたまま
#   素材を差し替えたい」ときのためのもの。
#
# 使い方:
#     firmware/tools/install_assets.sh              # 素材一式 + 目録
#     firmware/tools/install_assets.sh --settings   # settings.toml も
#     firmware/tools/install_assets.sh --only faces.bin
#     firmware/tools/install_assets.sh --transport hid   # full プロファイル
#     firmware/tools/install_assets.sh --dry-run    # 送らずに一覧だけ
#
# ★ 書き込みのたびにフラッシュを消す。顔 (zlib で 34 KB) と一次回答
#   (1 本 70 KB 前後 × 5) を毎回送るとフラッシュの寿命を削るので、
#   **変わったものだけ**送ること (--only を使う)。既定は目録・差分表・
#   アイコン・フォントだけで、顔と一次回答は --all のときにしか送らない。
#
# ★ このスクリプトは**書き込みしかしない**。像の書き込み (tools/flash.py) とは
#   別物で、キーボードは動いたまま。1 ファイルにつき 1 回だけ FAT を
#   読み書き可能で付け直す (詳しくは main/stackee_fat.c)。

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
FW_DIR=$(dirname "$HERE")
BASE=$(cd "$FW_DIR/.." && pwd)
OUTER=$(cd "$BASE/.." && pwd)

# 素材はこのリポジトリの firmware/assets に揃えてある (これだけで足りる)。
SRC="$FW_DIR/assets"
# 開発元では現行 CircuitPython 版の素材 (非公開) が真の出所なので、
# **あればそちらを先に見る**。無ければ上の firmware/assets だけを使う。
SRC2="$OUTER/firmware/kmk/stackee_assets"
if [ -d "$SRC2" ]; then
    tmp="$SRC"; SRC="$SRC2"; SRC2="$tmp"
fi
# settings.toml (パスワードとトークン入り) は非公開側にしかない。
SETTINGS_SRC="$OUTER/firmware/kmk/settings.toml"

TRANSPORT=auto
DRY=0
ALL=0
ONLY=""
SETTINGS=0

while [ $# -gt 0 ]; do
    case "$1" in
        --transport) TRANSPORT="$2"; shift 2 ;;
        --dry-run)   DRY=1; shift ;;
        --all)       ALL=1; shift ;;
        --only)      ONLY="$2"; shift 2 ;;
        --settings)  SETTINGS=1; shift ;;
        -h|--help)   sed -n '2,40p' "$0"; exit 0 ;;
        *) echo "知らない引数: $1" >&2; exit 2 ;;
    esac
done

if [ ! -d "$SRC" ]; then
    echo "素材がありません: $SRC" >&2
    exit 1
fi

# 送る先は FAT の /stackee_assets/。現行 CircuitPython 版と同じ場所なので、
# CircuitPython に戻しても同じ素材がそのまま使える。
put() {
    name="$1"
    path="$SRC/$name"
    if [ ! -f "$path" ] && [ -f "$SRC2/$name" ]; then
        path="$SRC2/$name"
    fi
    if [ ! -f "$path" ]; then
        echo "  (無い: $name)"
        return
    fi
    size=$(wc -c < "$path" | tr -d ' ')
    if [ "$DRY" = "1" ]; then
        echo "  [dry] stackee_assets/$name  $size バイト"
        return
    fi
    echo "  stackee_assets/$name  $size バイト"
    python3 "$HERE/fs_put.py" --transport "$TRANSPORT" \
        --dest "stackee_assets/$name" "$path"
}

# ★ 一覧は手で書く。glob だと LICENSE-*.txt まで送ってしまう
#   (どれもフラッシュを消す。本体には要らない)。
SMALL="manifest.json changes.bin status_h24.bdf status_icons.bin status_icons.json"
# ★ font16.bin は 231 KB。字幕用の日本語フォントで、作り直したときだけ送る
#   (--only font16.bin)。中身は東雲フォント (Public Domain)。
BIG="faces.bin font16.bin ack_01.pcmz ack_02.pcmz ack_03.pcmz ack_04.pcmz ack_05.pcmz"

echo "==> 目録とアイコンとフォント"
for name in $SMALL; do
    put "$name"
done

if [ -n "$ONLY" ]; then
    echo "==> 指定されたもの"
    put "$ONLY"
elif [ "$ALL" = "1" ]; then
    echo "==> 顔と一次回答 (大きい。フラッシュを消す回数が増える)"
    for name in $BIG; do
        put "$name"
    done
else
    echo "==> 顔と一次回答は送っていない (--all で送る)"
fi

if [ "$SETTINGS" = "1" ]; then
    echo "==> settings.toml"
    echo "    ★ パスワードとトークンが入っている。画面にもログにも出さない。"
    if [ ! -f "$SETTINGS_SRC" ]; then
        echo "  (無い: $SETTINGS_SRC)"
    elif [ "$DRY" = "1" ]; then
        echo "  [dry] settings.toml"
    else
        python3 "$HERE/fs_put.py" --transport "$TRANSPORT" \
            --dest settings.toml --quiet "$SETTINGS_SRC"
    fi
fi

echo
echo "できあがり。反映するには本体を再起動してください:"
echo "    python3 firmware/tools/console_hid.py reset"
