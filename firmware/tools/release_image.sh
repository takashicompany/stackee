#!/bin/bash
# ビルドした像を操作盤の配布先へ置き、目録 (manifest.json) を書く。
#
#   firmware/tools/release_image.sh                     # build-full/stackee.bin
#   firmware/tools/release_image.sh --profile dev       # build/stackee.bin
#   firmware/tools/release_image.sh --image path/to.bin --version v1.2.3
#
# 置く先は docs/firmware/ (操作盤と同じオリジン)。ページはここから
# manifest.json を読んで「配布版 vX (sha256 …)」の選択肢を出す。
#
# ★ **コミットはしない。** 置くだけ置いて、公開リポジトリへ載せるかは人が
#   決める。GPL の側は片付いている (DESIGN.md §7: 2026-09-21 にソースも像も
#   公開すると決め、firmware/ を公開リポジトリへ移して GPL-2.0 の全文を
#   同梱した) が、「どの像をいつ配るか」は別の判断なので自動ではやらない。
#
# ★ 実機には触らない。書き込みは tools/ota.mjs (Raw HID) か
#   tools/flash.py (ROM 経由の復旧用)。
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(cd "$HERE/.." && pwd)"           # firmware
BASE="$(cd "$FW/.." && pwd)"           # 公開: リポジトリ直下 / 非公開: public
DOCS="$BASE/docs"

PROFILE="full"
IMAGE=""
VERSION=""
while [ $# -gt 0 ]; do
    case "$1" in
        --profile) PROFILE="$2"; shift 2 ;;
        --image)   IMAGE="$2";   shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "知らない引数: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$IMAGE" ]; then
    if [ "$PROFILE" = "full" ]; then
        IMAGE="$FW/build-full/stackee.bin"
    elif [ "$PROFILE" = "dev" ]; then
        IMAGE="$FW/build/stackee.bin"
    else
        echo "profile は dev か full (いまは '$PROFILE')" >&2
        exit 2
    fi
fi
if [ ! -f "$IMAGE" ]; then
    echo "像が無い: $IMAGE  (先に ./build.sh --profile $PROFILE)" >&2
    exit 1
fi

# --- ESP のアプリ像か (先頭 1 バイトが 0xE9) --------------------------------
MAGIC=$(head -c 1 "$IMAGE" | od -An -tx1 | tr -d ' \n')
if [ "$MAGIC" != "e9" ]; then
    echo "ESP32 のアプリ像ではない (先頭が 0x$MAGIC、0xE9 のはず): $IMAGE" >&2
    exit 1
fi

SIZE=$(stat -f%z "$IMAGE" 2>/dev/null || stat -c%s "$IMAGE")
if [ "$SIZE" -gt 2097152 ]; then
    echo "!! ota_1 (2097152 B) に入らない: $SIZE B" >&2
    exit 1
fi
# ★ sha256 は 2 種類ある。目録には両方書く。
#   sha256       … ファイル全体。転送が化けていないかの照合に使う
#                  (ota.begin に渡す値、ota.end が返す値)
#   image_sha256 … 像の末尾 32 バイト = hash_appended。`esptool image_info` と
#                  同じで、デバイスの esp_partition_get_sha256 が返す値。
#                  **どの区画に何が入っているかを名指しするのはこちら。**
SHA=$(shasum -a 256 "$IMAGE" | cut -d' ' -f1)
IMAGE_SHA=$(tail -c 32 "$IMAGE" | od -An -tx1 | tr -d ' \n')
# 末尾の 32 バイトが本物か (= hash_appended 付きのビルドか) を確かめる。
BODY_SHA=$(head -c $(( $(stat -f%z "$IMAGE" 2>/dev/null || stat -c%s "$IMAGE") - 32 )) \
           "$IMAGE" | shasum -a 256 | cut -d' ' -f1)
if [ "$IMAGE_SHA" != "$BODY_SHA" ]; then
    echo "!! 末尾の SHA-256 が中身と合わない (hash_appended 無しか、像が壊れている)" >&2
    echo "   末尾:   $IMAGE_SHA" >&2
    echo "   計算値: $BODY_SHA" >&2
    exit 1
fi

# 版は像の中の esp_app_desc_t.version (CMakeLists が git describe から作る)。
# 取り出せなければ git describe を直に使う。
if [ -z "$VERSION" ]; then
    VERSION=$(dd if="$IMAGE" bs=1 skip=48 count=32 2>/dev/null \
              | tr -d '\000' | tr -cd '[:print:]')
fi
if [ -z "$VERSION" ]; then
    VERSION=$(git -C "$FW" describe --always --dirty --tags 2>/dev/null || echo "unknown")
fi

DEST_DIR="$DOCS/firmware"
DEST_NAME="stackee-$PROFILE.bin"
mkdir -p "$DEST_DIR"
cp "$IMAGE" "$DEST_DIR/$DEST_NAME"

DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cat > "$DEST_DIR/manifest.json" <<JSON
{
  "builds": [
    {
      "file": "$DEST_NAME",
      "profile": "$PROFILE",
      "version": "$VERSION",
      "size": $SIZE,
      "sha256": "$SHA",
      "image_sha256": "$IMAGE_SHA",
      "date": "$DATE"
    }
  ]
}
JSON

echo "置いた:  $DEST_DIR/$DEST_NAME"
echo "目録:    $DEST_DIR/manifest.json"
echo "version: $VERSION"
echo "size:    $SIZE B  ($((SIZE * 100 / 2097152))% of ota_1)"
echo "sha256:       $SHA          (ファイル全体)"
echo "image_sha256: $IMAGE_SHA  (esptool image_info / esp_partition_get_sha256)"
echo
echo "★ コミットはしていない。どの像をいつ配るかは人が決める"
echo "  (GPL の側は DESIGN.md §7 で片付いている)。"
