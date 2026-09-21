# firmware — Stackee ネイティブファーム (使い方)

> **ライセンス: GPL-2.0-or-later。**
> この `firmware/` ディレクトリの中身だけが GPL です。QMK (GPL-2.0-or-later)
> のコードを `third_party/qmk/` に取り込んで一緒にビルドしているため、
> 派生物であるこの土台も同じ条件で配っています。全文は
> [LICENSE](LICENSE)、取り込みの範囲は
> [third_party/qmk/IMPORT.md](third_party/qmk/IMPORT.md)。
> **このリポジトリの `firmware/` の外 (`docs/` の操作盤、`server/`、`test/`) は
> 従来どおりで、GPL ではありません。**
> 同梱の素材のライセンスは [assets/](assets/) の `LICENSE-*.txt` と §2 の表。

設計は [DESIGN.md](DESIGN.md)。実機で測った結果は [RESULTS.md](RESULTS.md)。
段階 0 (骨組み) → 1 (キーボード) → 2 (画面) → 3 (音声と通信) → **4 (周辺機能と
本番構成)** まで実装済み。`hello` / `status` の `fw` は **`stackee-idf/4`**。

---

## 0. まずこれだけ

### 0-0. clone しただけの PC で、ビルドできるまで

この木には**取得物が入っていない**。ESP-IDF もツールチェーンも TinyUSB も、
`clone` の後に取ってくる。順番はこれだけ。

```
git clone https://github.com/takashicompany/stackee.git
cd stackee/firmware

./setup.sh                          # ESP-IDF v6.0.3 + ツールチェーン + Python
./build.sh                          # dev  -> build/stackee.bin
./build.sh --profile full           # full -> build-full/stackee.bin
for t in tools/test_*.py; do python3 "$t"; done    # 実機に触らない確認
```

`setup.sh` が入れるのは 3 つ。**何度走らせても壊れない**ので、途中で切れたら
そのまま走らせ直す。中身だけ見たいなら `./setup.sh --dry-run`。

| 何 | どこへ | 変えるには |
|---|---|---|
| ESP-IDF v6.0.3 (upstream espressif/esp-idf のタグ) | `~/.local/share/stackee/esp-idf-v6.0.3` | `STACKEE_IDF_PATH` |
| ツールチェーン (xtensa-esp-elf / gdb / openocd。4〜5 GB) | `~/.local/share/stackee/.idf_tools-v6.0.3` | `STACKEE_IDF_TOOLS_PATH` |
| ホスト側の Python ([requirements.txt](requirements.txt)) | `pip install --user` | `--venv` で専用の venv に |

置き場をリポジトリの**外**にしてあるのは、clone し直しても取り直しにならない
ようにするため。`build.sh` はこの場所を自分で見つける (§3 の表の 5 番目)。
すでに ESP-IDF v6.0.3 を持っているなら `STACKEE_IDF_PATH=... ./build.sh` で
そのまま使える (ツールチェーンは同じ階層の `.idf_tools-v6.0.3` を見る)。

**TinyUSB (`managed_components/`) は何もしなくていい。** 初回のビルドで
IDF のコンポーネントマネージャが [dependencies.lock](dependencies.lock)
どおりに取ってくる。

要るもの:

| | |
|---|---|
| OS | macOS (Apple Silicon / Intel) か Ubuntu 22.04 以降 |
| `git` / `python3` | Python は 3.9 以降 (ESP-IDF v6.0.3 の条件) |
| `cc` | ホスト側のテストがこれで C を直接ビルドする。macOS は `xcode-select --install`、Ubuntu は `build-essential` |
| ディスク | ESP-IDF 約 1 GB + ツールチェーン 4〜5 GB |
| Ubuntu の下ごしらえ | `sudo apt-get install -y git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 build-essential` |

**この木だけで確かめられること / 確かめられないこと**。現行 CircuitPython 版
(`firmware/kmk`) は非公開なので、そこから期待値を出す検査は「SKIP」と出して
飛ばす。**飛ばしても終了コードは 0** で、ビルドにも書き込みにも関係ない。
clone しただけの木での実測 (2026-09-21) は **242 件** — うち 17 件が skip、
別に 5 ファイル (`test_cfg_host` / `test_conhid_host` / `test_console_host` /
`test_render_host` / `test_touch_host`) が丸ごと SKIP。移植元がある開発元の
木では 307 件が走る。所要は 12 秒。

```
python3 tools/gen_font16.py --check      # 走る (字幕フォント)
python3 tools/import_faces.py --check    # 走る (顔の素材。Pillow が要る)
python3 tools/gen_keymap.py --check      # SKIP (生成物そのものは入っている)
```

書き込みは `tools/flash.py` (§5)。**CircuitPython へ戻す道 (§6) には、戻し先の
像が要る。この木には入っていない**ので、書き込む前に自分で吸い出して退避して
おくこと (`tools/flash.py --dry-run` が今の像を退避する。置き場は
`STACKEE_BACKUP_DIR`、既定は `~/.local/share/stackee/backups`)。

### 0-1. ビルドする

```
firmware/build.sh                   # dev プロファイル  -> build/stackee.bin
firmware/build.sh --profile full    # full プロファイル -> build-full/stackee.bin
firmware/build.sh clean             # ビルドし直す
firmware/build.sh size              # 内訳も出す
```

**プロファイルは 2 つある** (DESIGN.md §4)。ESP32-S3 の IN エンドポイントが
EP0 込みで 5 本しかないので、全部は載らない。

| プロファイル | USB の構成 | IN | コンソールとログ | いつ使うか |
|---|---|---|---|---|
| `dev` (既定) | CDC + HID + Raw HID | 4+EP0 | **CDC** (と Raw HID の両方) | 開発中。シリアルでログが流れるので調べやすい |
| `full` | HID + Raw HID + **UAC マイク** | 3+EP0 | **Raw HID だけ** | 本番。Mac の USB マイクとして使える |

★ **`full` には 1200 bps タッチの脱出路が無い** (CDC が無いため)。
戻る道はコンソールの `bootloader` / キーマップの `QK_BOOT` / 物理の RST 長押しの
3 つだけ。§0-6 を読んでから書き込むこと。

★ ビルドディレクトリはプロファイルごとに分けてある (`build` / `build-full`)。
同じディレクトリで切り替えると `tusb_config.h` の違いが拾われず、混ざった像ができる。

### 0-2. 実機に触らずに確かめる

```
cd firmware
for t in tools/test_*.py; do python3 "$t"; done
python3 tools/gen_keymap.py --check      # 生成物が最新か
python3 tools/gen_font16.py --check      # 字幕フォントが元の BDF と合うか
python3 tools/import_faces.py --check    # 顔の素材が元絵と合うか (Pillow が要る)
python3 tools/hid_desc_check.py          # HID 記述子の構成を目で見る
```

どれも実機に触らない。内訳は §4。要るのは **`cc` (clang か gcc) と python3**
だけ — ホスト側の検査は C をその場でホスト向けにビルドして走らせる。
ESP-IDF は要らない。`--check` の 3 つと `import_faces.py` は
[requirements.txt](requirements.txt) の Pillow が要る。

★ 移植元 (現行 CircuitPython 版の `firmware/kmk`) は非公開なので、公開
リポジトリだけの clone では一部が「SKIP」と出て飛ぶ (§0-0)。**飛ばしても
終了コードは 0**。

### 0-3. 書き込む

**書き込み中は約 1 分キーボードが使えない。ユーザーの了承を得てから行う。**

```
# まず書かずに確かめる (退避と照合だけ)
python3 tools/flash.py --dry-run --image build/stackee.bin

# 本番 (--dry-run の直後に、退避を省いて 1 回の ROM 突入で書く)
python3 tools/flash.py --skip-backup --image build/stackee.bin
```

full プロファイルなら `--image build-full/stackee.bin`。
くわしくは §5。**ROM への突入は 1 回の書き込みにつき 1 回**。

### 0-4. コンソールで話す

```
# dev (CDC) でも full (Raw HID) でも、同じコマンドで話せる
python3 tools/console_hid.py hello
python3 tools/console_hid.py status
python3 tools/console_hid.py --transport hid status     # 経路を名指し
python3 tools/console_hid.py devices                    # Raw HID の一覧
python3 tools/console_hid.py watch 10                   # ログを 10 秒流す
python3 tools/console_hid.py camera.capture warmup=30
python3 tools/console_hid.py settings.set 'kv={"STACKEE_HOST":"192.168.0.5"}'
```

`--transport hid` の用意 (macOS):

```
brew install hidapi                        # ライブラリ本体
python3 -m pip install --user hid          # Python の口
```

★ Homebrew は `/opt/homebrew/lib` に置くが、そこは framework Python の
dyld の既定の探索先に入っていない。`hid` パッケージは
`LoadLibrary('libhidapi.dylib')` を**名前だけ**で呼ぶので、そのままだと
import が落ちる。`console_hid.py` は **import する瞬間だけ LoadLibrary を
差し替えて**絶対パスを渡すので、追加の設定は要らない
(`DYLD_LIBRARY_PATH` はプロセス開始前にしか効かないので後から直せない)。

`--transport serial` (既定で先に試す) には `pyserial`。

ブラウザからも同じことができる: <https://takashicompany.github.io/stackee/>
(接続方法で「USB シリアル」と「USB HID」を選べる)。

コマンドの一覧は §0-7。合否をまとめて測るなら:

```
python3 tools/check_phase1.py     # キーボード
python3 tools/check_phase2.py     # 画面
python3 tools/check_phase3.py     # 音声と通信 (★ 最初にヌル出力を立てる)
python3 tools/check_phase4.py     # 周辺機能と本番構成
```

### 0-5. VIA / Remap で配列を変える

1. `via/stackee.json` を用意する (生成物。`tools/gen_keymap.py` が作る)
2. USB でつなぐ。VIA なら Settings → Show Design tab → Design タブで
   `via/stackee.json` を読み込む。Remap なら「ファイルから読み込む」
3. 変えたら本体の NVS に自動で保存される (書き戻しは 1 回にまとまる)
4. 再起動しても残る。元に戻すには VIA の「Reset Keymap」

★ Remap のカタログには登録していない (定義 JSON の手動読み込みで使う)。
★ BLE 越しの VIA は**動かない** (BLE 側に Raw HID を出していない)。
★ full プロファイルでも VIA は動く。コンソールが同じ Raw HID に相乗りするが、
   VIA が使う command id (0x00〜0x0F) とは別の id (0xC0〜) を使っている。

### 0-6. 戻し方 (CircuitPython に戻す)

```
# ソフトから (ふつうはこれ)
python3 tools/console_hid.py bootloader     # ROM ダウンロードモードへ
python3 tools/flash.py --rollback           # CircuitPython の像を書き戻す
```

`bootloader` が効かないとき:

| 状況 | 手 |
|---|---|
| dev プロファイル | 1200 bps タッチ (`flash.py` の `enter_rom()` が自動でやる) |
| full プロファイル | **1200 bps タッチは使えない** (CDC が無い)。`QK_BOOT` キーを押す |
| ROM が `303a:1001` で `invalid header: 0xffffff1f` を延々出す (フラッシュが読めないブートループ) | `python3 tools/pmic_cycle.py <そのポート>` — ROM から AXP2101 に電源再投入を命じる (§18)。`flash.py` の復帰待ちが自動でも呼ぶ |
| どちらも効かない | 物理。§6-2 の RST 長押し |

くわしくは §6 と §18。

### 0-7. コンソールのコマンド一覧 (段階 4 時点)

| 分類 | コマンド |
|---|---|
| 基本 | `hello` `status` `reset` `bootloader` |
| ログ | `log.tail` `log.burst` |
| キー | `key.inject` `hid.switch` `hid.set` |
| BLE | `ble.refresh` `ble.clear_bonds` `ble.drop_cccd` `ble.svc_changed` |
| 画面 | `lcd.crc` `lcd.dump` `lcd.status` `lcd.full` `face.set` `face.auto` `bar.set` `bar.auto` `ui.selftest` `ui.status` `ui.assets` `ui.subtitle` |
| Wi-Fi | `wifi.scan` `wifi.list` `wifi.add` `wifi.remove` `wifi.connect` `wifi.status` `wifi.off` `wifi.on` |
| 音 | `audio.selftest` `audio.null` `audio.play` `audio.status` `talk.inject` `talk.status` |
| 設定 | `settings.get` `settings.raw` `settings.set` |
| ファイル | `fs.put` |
| タッチ | `touch.status` `touch.inject` `touch.scroll` |
| カメラ | `camera.capture` `camera.status` `camera.power` `camera.dump` |
| USB | `usb.status` |
| 計測 | `bench` |
| 未対応 | `loop.*` (CircuitPython のメインループを測るもの。C 版に当たるものが無い。`status` の `perf` を見る) |

### 0-8. 素材を差し替える

ネイティブ版には CircuitPython の USB ドライブ (`/Volumes/CIRCUITPY`) が無い。
`fs.put` でファイルを送り込む。

```
firmware/tools/install_assets.sh --dry-run     # 何を送るか見るだけ
firmware/tools/install_assets.sh               # 目録・差分表・アイコン・フォント
firmware/tools/install_assets.sh --all         # 顔と一次回答も
firmware/tools/install_assets.sh --only faces.bin
firmware/tools/install_assets.sh --only font16.bin   # 字幕のフォント (231 KB)
python3 tools/fs_put.py --dest stackee_assets/x.bin path/to/x.bin
```

★ **出どころは `firmware/assets/`。** ここが「道具が作って検査した
バイト列」で、`tools/import_faces.py` が書き、`tools/render_expected.py` や
`tools/test_subtitle_host.py` がこれを読んで期待値を組む。**送るものと
検査したものを同じにする。** 開発元 (非公開) にしか無いものだけ
`firmware/kmk/stackee_assets/` から拾う。
(2026-09-21 まで逆で、古い manifest が実機へ行っていた。§23-9)

`--only X` は **X だけ**を送る (既定の一式は送らない)。

書き込みのたびにフラッシュを消すので、**変わったものだけ**送ること。
反映には再起動が要る。

### 0-9. 困ったとき

| 症状 | 見るところ |
|---|---|
| コンソールが答えない | §0-4 の `devices` で Raw HID が見えるか。dev なら `ls /dev/cu.usbmodem*` |
| キーが効かない | `status` の `keys.tca` (TCA8418 が居るか) と `keys.iofail` |
| 画面が真っ暗 | `lcd.status` の `ready`。false なら電源まわり (§2 の `stackee_board.c`) |
| BLE で打てない | §15。Mac 側のペアリングを 1 回消す必要がある |
| 会話が失敗する | `status` の `heap_internal` (TLS が確保できるか)。§14-1 |
| カメラが撮れない | `camera.status` の `camera_err` と `aldo3`。§17-2 |
| USB マイクが見えない | `usb.status` の `profile` が `full` か。dev には入っていない |
| 起動しない / 戻せない | §6。ROM は 1 セッション 1 回まで |

---

## 1. 何が入っていて、何が入っていないか

| 入っている | 内容 |
|---|---|
| キー入力 | TCA8418 (PORT.A / I2C 0x34 / 5x10) を 1 ms 周期でポーリング → QMK quantum 0.34.4 → 送信キュー → USB HID |
| 配列 | `firmware/kmk/keymap.py` から生成した 6 層。HoldTap・レイヤー・JIS (LANG1/LANG2)・独自キー。位置ごとの差し替えは `tools/gen_keymap.py` の `KEYMAP_OVERRIDES` (KMK 側を触らずに済ませる) |
| VIA | Raw HID (0xFF60/0x61、32 バイト、**Report ID なし**)。dynamic keymap は NVS のブロブに保存 |
| 脱出路 | 1200 bps タッチ / コンソール `bootloader` / `QK_BOOT` キー → ROM ダウンロードモード。コンソール `reset` → 通常再起動 |
| USB (dev プロファイル) | CDC 1 本 + HID (キーボード / マウス / コンシューマ) + Raw HID |
| コンソール | CDC 上で現行と同じ枠。`hello` / `status` / `reset` / `bootloader` / `log.tail` / `key.inject` / `hid.switch` / `ble.refresh` / `ble.clear_bonds` / `ble.drop_cccd` |
| LCD | ILI9342C。**段階 2**: 白地 + 上段 28px の黒帯 (アイコンと数字) + 中央に顔 240x240 |
| 顔 | 既存素材 `faces.bin` (4bpp / 32 枚) を起動時に PSRAM へ展開。状態機械と差分描画は現行 CircuitPython 版と同じ |
| ステータスバー | `status_icons.bin` (2bpp 18 タイル) と `status_h24.bdf`。電池・音量・Wi-Fi・BLE/USB |
| Wi-Fi | **段階 3**。登録簿の中でいちばん強い AP へ自動接続。1 周期 1 段 |
| 会話 | **段階 3**。42 キー長押しで録音 → pi400 へ HTTPS → 一次回答 → 返答を再生 |
| 音量 | **段階 3**。`STK_VOLUP` / `STK_VOLDN` で 5 きざみ。静まってから NVS に保存 |
| FAT / 電池 | 段階 0 と同じ |
| 起動ログ | 起動のいちばん最初から 16 KB のリングバッファに溜める。`log.tail` であとから読める (CDC が繋がる前のログも残る) |
| 自己計測 | `status` の `perf` に `input` (キー → レポート) と `input_loop` (入力タスクの周期)、`keys` と `hidq` |

| BLE HID | NimBLE + esp_hid。名前 "stackee"、外観はキーボード、ボンディング有効。記述子は **USB と同じ配列そのもの** |
| 送信先 | 既定は **BLE** (現行と同じ)。`STK_HID_SWITCH` で BLE ⇄ USB をトグルし、選択は NVS に残る。USB を選んでいてもケーブルが無ければ BLE に倒す |
| マウスキー | QMK の `MOUSEKEY_ENABLE` (加速つき)。Report ID 2 で USB・BLE 両方に出る |

| タッチパッド | **段階 4**。FT6336 を 5 ms 周期で読み、なぞり = ポインタ / タップ = クリック / `STK_TOUCH_SCROLL` 中はスクロール。Report ID 2 で USB・BLE 両方に出る |
| カメラ | **段階 4**。GC0308 を ALDO3 で起こして撮り、software で JPEG に畳む。`camera.*` と `STK_CAMERA` キー |
| USB マイク (UAC) | **段階 4 / full プロファイルのみ**。16 kHz モノラル。会話が優先で、録音中は無音を送る |
| Raw HID コンソール | **段階 4**。VIA の独自 command id 0xC0〜。**dev でも有効** (CDC と同じバイト列を両方へ流す) |
| FAT への書き込み | **段階 4**。`settings.set` と `fs.put`。書くときだけ読み書き可能で付け直す |

| 入っていない | どこで入るか |
|---|---|
| BLE 越しの VIA | 段階 1b の実験項目。BLE 側に Raw HID を出していないので応答は捨てる |
| カメラの画像を Wi-Fi で送る | 撮って JPEG にするところまで。送り先の取り決めが pi400 側で未定 (§17-2) |
| カメラの表情 (顔の `camera`) | 顔の状態機械には口があるが、`STK_CAMERA` からは繋いでいない |
| 旧 VoiceLink (Mac の 5555 番へ TCP、`stackee_voice.py`) | **移植しない。** 現行でも使われていない経路 (会話は HTTPS の `stackee_talk` に移っている)。`STACKEE_HOST` / `STACKEE_PORT` は読まない |
| カメラの表情 | 段階 4 (顔の状態機械には `camera` の口が既にある) |
| コンソールの残りのコマンド、UAC (full プロファイル) | 段階 4 |
| ESP-IDF v6.0.3 とツールチェーン | 取得物。`setup.sh` が取ってくる (§0-0 / §3) |
| TinyUSB (`managed_components/`) | 取得物。**初回のビルドで** `dependencies.lock` どおりに取れる |
| 移植元 (`firmware/kmk`、現行 CircuitPython 版) | 非公開。**無くてもビルドも書き込みもできる**。突き合わせの検査だけ SKIP になる (§0-0) |
| CircuitPython へ戻すための像 | 非公開。**書き込む前に自分で吸い出して退避する** (§0-0 / §6) |

---

## 2. 中身の地図

| ファイル | 役割 |
|---|---|
| `setup.sh` | **下ごしらえ**。ESP-IDF v6.0.3 + ツールチェーン + ホスト側の Python を揃える (§0-0)。冪等 |
| `requirements.txt` | ホスト側 (自分の PC) の Python パッケージ。**ビルドには要らない**。`tools/` の道具が使う |
| `build.sh` | ビルド。ESP-IDF の場所を決めるのもここ (§3)。実機には触らない |
| `third_party/qmk/` | **QMK 0.34.4 のコピー (無改変)**。版・コミット・ファイル一覧は [`third_party/qmk/IMPORT.md`](third_party/qmk/IMPORT.md)。ライセンスは GPL-2.0 |
| `main/qmk_port/` | QMK と Stackee の橋渡し。時計 / EEPROM (NVS) / host driver / マトリクス / 待ち / 再起動 / デバッグ出口 |
| `main/qmk_port/stackee_qmk_config.h` | QMK の `config.h` に当たるもの。`-include` で全翻訳単位に差し込む (QMK と同じ作法) |
| `main/qmk_port/stackee_holdtap.c` | 独自キーの受け止めと、`MT()` で表せない HoldTap の状態機械 |
| `main/keymaps/default_keymap.c` | **生成物**。`keymaps[][5][10]` と HoldTap のキーごとの設定 |
| `main/qmk_port/stackee_keycodes.h` | **生成物**。独自キーコード (`STK_*`) |
| `via/stackee.json` | **生成物**。VIA / Remap の定義 |
| `main/stackee_tca8418.c` | TCA8418 の初期化と FIFO 読み出し。`firmware/kmk/code.py` と同じレジスタ設定 |
| `main/stackee_input.c` | 入力タスク (CPU1 / 最高優先度 / 1 ms) |
| `main/stackee_hid_out.c` | 送信タスク。キューから USB へ。NVS の書き戻しもここ |
| `main/stackee_report_queue.c` | 送信キュー (単一生産者・単一消費者のリングバッファ) |
| `main/stackee_nvs.c` | QMK の EEPROM を NVS のブロブ (`stackee` / `qmk_eep`、1 KB) に置く |
| `main/stackee_usb.c` | USB 記述子・1200bps タッチ・Raw HID の受信キュー |
| `main/stackee_console.c` | CDC の JSON 行コンソール |
| `main/stackee_lcd.c` | 転送ワーカー (段階 0)。段階 2 で ui がフレームバッファへ直に描けるようにした |
| `main/stackee_board.c` / `stackee_perf.c` | 段階 0 のまま (perf は `ui` / `ui_face` / `ui_bar` が増えた) |
| `main/stackee_assets.c` | FAT の読み取り (段階 0) + 素材の読み込みと zlib 展開 (段階 2) |
| `main/stackee_ui.c` | **段階 2**。ui タスク (CPU0 / 中優先度 / 5 ms)、素材の読み込み、console の画面コマンド |
| `main/stackee_draw.c` | **段階 2**。画素を置く実体 (顔 4bpp / アイコン 2bpp / BDF)。ESP-IDF に依存しない |
| `main/stackee_icons.c` | **段階 2**。`firmware/kmk/stackee_icons.py` の移植 (タイル番号・色・配置) |
| `main/stackee_faceanim.c` | **段階 2**。`firmware/kmk/stackee_face.py` の移植 (状態機械と差分の段取り) |
| `main/stackee_bdf.c` | **段階 2**。BDF から必要な 14 文字だけ展開 |
| `main/stackee_selftest.c` | **段階 2**。`ui.selftest` が描くバーの代表 6 状態 (表はここ 1 か所) |
| `main/stackee_crc32.c` | **段階 2**。zlib.crc32 と同じ CRC-32 |
| `main/stackee_settings.c` | **段階 3**。`/settings.toml` の読み (CircuitPython の supervisor と同じ範囲) |
| `main/stackee_jsonlite.c` | **段階 3**。サーバの応答を読む。**最上位のキーだけ**を見る字句解析 |
| `main/stackee_wifistore.c` | **段階 3**。Wi-Fi 登録簿 (`stackee_wifi_store.py` の移植) |
| `main/stackee_wifism.c` | **段階 3**。Wi-Fi 自動接続の状態機械 (`stackee_wifi.py` の移植) |
| `main/stackee_wifi.c` | **段階 3**。esp_wifi と NVS の橋渡し + net タスク + `wifi.*` コマンド |
| `main/stackee_talksm.c` | **段階 3**。会話の状態機械 (`stackee_talk.py` の移植) |
| `main/stackee_http.c` | **段階 3**。非同期 HTTPS ワーカー (`firmware/native-http` と同じ考え方) |
| `main/stackee_codec.c` | **段階 3**。ES7210 (`es7210.py`) と AW88298 (`stackee_speaker.py`) |
| `main/stackee_audio.c` | **段階 3**。audio タスク、半二重の I2S、一次回答、ヌル出力、`audio.*` / `talk.*` |
| `main/stackee_volume_core.c` | **段階 3**。音量の丸めと保存のタイミング (`stackee_volume.py` の移植) |
| `main/stackee_volume.c` | **段階 3**。NVS への読み書きと FAT からの移行 |
| `main/stackee_touch_core.c` | **段階 4**。タッチの判定 (回転・平滑・タップ・スクロール)。ESP-IDF に依存しない |
| `main/stackee_touch.c` | **段階 4**。FT6336 の読み出し・touch タスク (CPU0 / 5 ms)・`touch.*` |
| `main/stackee_camera.c` | **段階 4**。ALDO3・esp32-camera・JPEG 圧縮・`camera.*` |
| `main/stackee_fat.c` | **段階 4**。FAT への書き込み (生パーティションの diskio を自前で持つ) |
| `main/stackee_conhid.c` | **段階 4**。Raw HID の上のコンソール (`via_command_kb` を乗っ取る) |
| `main/stackee_uac.c` | **段階 4**。UAC マイク (full プロファイルのみ。dev では中身が消える) |
| `tools/console_hid.py` | **段階 4**。Mac 側のクライアント。dev (シリアル) と full (Raw HID) の両方 |
| `tools/fs_put.py` / `tools/install_assets.sh` | **段階 4**。素材を FAT へ送る |
| `tools/check_phase4.py` | **段階 4** の合否を実機で測る (読むだけ) |
| `tools/gen_keymap.py` | keymap.py から配列と VIA 定義を作る |
| `tools/import_faces.py` | 元絵 (`assets/src/faces/`) から顔の素材を作る (`--check` で一致だけ見る) |
| `tools/gen_font16.py` | 東雲 BDF (`assets/src/fonts/shinonome/`) から字幕フォントを作る |
| `tools/keycodes.md` | KMK と QMK の対応表 (読み物) |
| `tools/flash.py` | ota_0 だけを書き換える。退避・照合・復帰つき |
| `tools/check_phase1.py` | 段階 1 の合否を実機で測る (読むだけ) |
| `tools/check_phase2.py` | **段階 2** の合否を実機で測る (読むだけ) |
| `tools/check_phase3.py` | **段階 3** の合否を実機で測る (読むだけ・**必ずヌル出力にしてから**) |
| `tools/check_ble.py` | **段階 3**。BLE が Mac に繋がるかを bleak で測る (読むだけ・§15) |
| `tools/render_expected.py` | **段階 2** の期待値。同じ素材から同じ絵を Mac で組み立てて CRC32 を出す |
| `tools/check_phase0.py` | 段階 0 の合否 (起動時間など) |
| `tools/hid_desc_check.py` | HID 記述子を読んで構成を出す (実機不要) |
| `hostbuild/` | Mac 上で C を走らせる土台。打鍵列テストもここ |

---

## 3. ビルド

```
firmware/build.sh            # ビルド
firmware/build.sh clean      # build/ を消してから
firmware/build.sh size       # ビルドしてから内訳も出す
```

### ESP-IDF は upstream の v6.0.3 (2026-09-18 に更新)

`build.sh` が使う ESP-IDF は upstream espressif/esp-idf のタグ `v6.0.3`。
**取得物なのでこのリポジトリには入っていない。** `setup.sh` が取ってくる
(§0-0)。手で置くならこれと同じこと:

```
git clone --depth 1 --branch v6.0.3 --recursive --shallow-submodules \
  https://github.com/espressif/esp-idf.git esp-idf-v6.0.3
IDF_TOOLS_PATH=$PWD/.idf_tools-v6.0.3 \
  esp-idf-v6.0.3/install.sh esp32s3
```

浅い clone で困ったら `STACKEE_IDF_FULL_CLONE=1 ./setup.sh` (深さを切らない)。

`build.sh` は場所を上から順に見て、`export.sh` があるものを使う。

| 順 | 場所 | 何のため |
|---|---|---|
| 1 | `STACKEE_IDF_PATH` | この土台だけで使う置き場を名指しする |
| 2 | `IDF_PATH` | すでに用意してある環境をそのまま使う |
| 3 | `firmware/` の隣の `esp-idf-v6.0.3/` | 上の手順どおりに置いた場合 |
| 4 | 親リポジトリの `firmware/esp-idf-v6.0.3/` | 開発元の置き方 |
| 5 | `~/.local/share/stackee/esp-idf-v6.0.3/` | `setup.sh` の既定の置き場 |

ツールチェーンは `STACKEE_IDF_TOOLS_PATH` → `IDF_TOOLS_PATH` →
**実際に使うことにした IDF と同じ階層**の `.idf_tools-v6.0.3/`。
★ 3 番目は「既定の置き場」ではなく「決まった IDF」から数える。そうしないと
`STACKEE_IDF_PATH` だけを指定したときに、よその IDF と空のツールチェーンの
組み合わせになって `export.sh` が黙って落ちる (2026-09-21 に踏んで直した)。

**なぜ upstream に移したか**: 2026-09-18 まで使っていたのは
CircuitPython 側に同梱されていた Adafruit fork (v6.0.1 相当) で、ESP32-S3 の
ハードウェア MPI で署名検証が落ちる修正 (`c41dd724d` / `86f6192f1`) が
入っていなかった (§20)。v6.0.3 のツリーには両方入っている。

**ツールチェーンは別の場所に入れる**。コンパイラ自体は前の土台と同じ版
(`xtensa-esp-elf esp-15.2.0_20251204`) だが、gdb (16.3 → 17.1) /
openocd / esp-rom-elfs (20241011 → 20260528) が違う。

**TinyUSB は段階 1 で IDF Component Registry 版に切り替えた**
(`main/idf_component.yml` で `espressif/tinyusb == 0.21.0~1` に固定)。
別ツリーを直接参照するのをやめたので、この土台だけで像はビルドできる。
中身は実機で列挙が通った TinyUSB v0.21.0。
`managed_components/` は取得物なのでリポジトリに入れない。版は
`dependencies.lock` で固定されている。

ビルド時刻は像に埋めていないので、**同じソースからは毎回同じ sha256 が出る**。
ただし ESP-IDF はアプリ記述子に **ELF の sha256** (`app_elf_sha256`、像の
0x00B0 から 32 バイト) を埋める。ELF にはデバッグ情報としてソースの絶対パスが
入るので、**置き場所が変わると像の sha256 も変わる**。中身は 1 バイトも
変わっていないことを、この 32 バイトと末尾の像ハッシュ 32 バイト以外が
一致することで確かめられる。

出来上がるのは `build/stackee.bin`。ota_0 (2,048 KB) に収まっていることは
`build.sh` が確かめる。

### パーティション表は 2 つある

| ファイル | 用途 |
|---|---|
| `partitions.csv` | **実機に載っている表**。CircuitPython 16MB 版そのまま。焼かない |
| `partitions-build.csv` | **ビルド専用**。`uf2` の型だけ `app/factory` → `data/0x20` に読み替えたもの |

実機の表では `uf2` が `app, factory` なので、ESP-IDF は「アプリの置き場は
factory = 0x410000」と思い込み、ビルドの最後に誤った書き込み案内を出し、
サイズ検査も 256 KB を基準にしてしまう。ビルド専用の表はその型だけを
変えたもので、**オフセットもサイズも 1 バイトも変えていない**。
これで案内が `0x10000` を指すようになった。

書き込みは今までどおり `tools/flash.py` を使うこと。`idf.py flash` は使わない。

---

## 4. 実機に触らない確認

```
python3 firmware/tools/test_keyseq_host.py     # 打鍵列テスト・既定配列の移行 (47 件)
python3 firmware/tools/test_gen_keymap.py      # 配列生成 (22 件)
python3 firmware/tools/test_console_host.py    # コンソールの組み立て (26 件)
python3 firmware/tools/test_tools.py           # 道具・HID 記述子・ROM・nvs・USB 復帰 (67 件)
python3 firmware/tools/test_hid_dest_host.py   # 送信先の選び方 (7 件)
python3 firmware/tools/test_hid_report_map.py  # 記述子を esp_hid のパーサに通す (5 件)
python3 firmware/tools/hid_desc_check.py       # 記述子の構成を目で見る
python3 firmware/tools/test_faceanim_host.py   # 顔の状態機械 (20 件)
python3 firmware/tools/test_render_host.py     # 画面の描画と期待値 (27 件)
python3 firmware/tools/test_cfg_host.py        # 段階 3: 設定・登録簿・音量・素材 + 4 KB の JSON (48 件)
python3 firmware/tools/test_talk_host.py       # 段階 3: 会話の状態機械 + 字幕 + 切り捨て (73 件)
python3 firmware/tools/test_wifi_host.py       # 段階 3: Wi-Fi の状態機械 (21 件)
python3 firmware/tools/test_touch_host.py      # 段階 4: タッチ (25 件)
python3 firmware/tools/test_conhid_host.py     # 段階 4: Raw HID コンソール (18 件)
python3 firmware/tools/test_subtitle_host.py   # 字幕: フォント・帯・一次回答の行 (33 件)
python3 firmware/tools/test_ota_host.py        # アプリ内 OTA の中核 (40 件、§25)
python3 firmware/tools/gen_keymap.py --check   # 生成物が最新か
python3 firmware/tools/gen_font16.py --check   # 字幕フォントが最新か
```

全部で **479 件**。どれも実機に触らない。

★ 段階 4 の 2 本のうち `test_touch_host.py` は、**現行 CircuitPython 版の
`stackee_touch.py` をそのまま import して**同じ座標列を流し、出てくる
手を 1 つずつ突き合わせる (期待値を手で書かない)。`test_conhid_host.py` は
デバイス側の C・Mac 側の Python・ブラウザ側の JavaScript の**3 つが同じ枠を
作るか**を見る (node があればブラウザ側も一緒に)。

★ 段階 3 の 3 本 (`test_cfg_host` / `test_talk_host` / `test_wifi_host`) は
**ASan + UBSan つき**でビルドする。登録簿を JSON に直すところで入れ物の外へ
書いていた欠陥が、これを入れて初めて落ちた (snprintf の戻り値 = 「入れたかった
長さ」を足し込んでいた)。実機では同じ欠陥が黙って隣の領域を壊す。

`test_keyseq_host.py` が段階 1 の要。QMK の quantum + 橋渡し層を Mac 用に
ビルドし、**時刻つきの押下 / 解放の列**を流して、出てきた HID レポートの列を
確かめる。見ているのは:

* 単押し、レイヤー LT の長押しと短押し
* HT の Z=Shift (TAPPING_TERM 200 ms の両側 194 / 206 ms)
* LT の R/レイヤー 3 (TAPPING_TERM **300** ms の両側 294 / 306 ms) と、
  同じ 250 ms でも LT はタップ・MT はホールドになること
* `prefer_hold` のキーで他キー割り込み / `prefer_hold` なしのキーで割り込まない
* 同じキーコードで設定が違う 2 つのキー (レイヤー 0 の 36 と 37) の区別
* JIS の LANG1 / LANG2 が 0x90 / 0x91 で出ること
* 独自キーが HID に 1 バイトも漏れないこと。**ただし `MIC(kc)` だけは
  中のキーを送る** — 素のキーを押して離したのと 1 バイトも違わないこと
  (F13 のほか A / W / Space / → でも確かめる)
* 既定配列の**移行** — 旧い既定なら差し替え、ユーザーが変えていれば触らず、
  二度目は何もしないこと
* FIFO 溢れで押下中を全解放すること
* VIA で配列を書き換えたら実際に出る文字が変わり、NVS への書き戻しが
  1 回にまとまること
* `QK_BOOT` が ROM ダウンロードモードへ行くこと
* `key.inject` が使う空きスロット (4,0) が、既定では何も出さないこと

**ここで通っても実機で動く保証にはならない。** I2C も USB も入っていない。

---

## 5. 書き込み

### ★ 普段の更新は Raw HID の OTA を使う (2026-09-21〜)

```
node firmware/tools/ota.mjs --image firmware/build-full/stackee.bin
```

本体が動いたまま、使っていないほうの区画へ書いて切り替える。**ROM の
ダウンロードモードには入らない**ので、固まる経路が無い。書き込み中も
キーボードは使え、使えないのは再起動の約 1.2 秒だけ。実測 1.4 MB で
54〜58 秒。詳しくは **§25**。

下の `flash.py` は **ROM 経由なので復旧専用**にする。使うのは
「OTA で入れた像が起動しない」「コンソールが答えない」ときだけ。
★ **`ota.commit` を通したあとは `flash.py` の既定 (`0x10000` = ota_0) が
起動する区画とは限らない。** §25-7 を読むこと。

### 復旧 (ROM 経由、`tools/flash.py`)

**書き込み中は約 1 分キーボードが使えない。ユーザーの了承を得てから行う。**

```
# まず書かずに確かめる (退避と照合だけ)
python3 firmware/tools/flash.py --dry-run \
  --image firmware/build/stackee.bin \
  --expect-image <いま本体に載っているはずの像>.bin

# 本番 (--dry-run の直後に、退避を省いて 1 回の ROM 突入で書く)
python3 firmware/tools/flash.py --skip-backup \
  --image firmware/build/stackee.bin \
  --image-sha <build.sh が出した sha256> \
  --expect-image <いま本体に載っているはずの像>.bin
```

`--skip-backup` は「退避は直前の `--dry-run` で済ませた」前提で、ROM への
突入を 1 回だけにする。

★★ **1 回の ROM セッションで esptool は 1 回だけ。** 2 回目は
"No serial data received" で失敗し、そのまま ROM が固まる (2026-09-16 に
再現)。`flash.py` は 2 回目を**呼ぶ前に**止めるようにしてある。
退避と書き込みの両方が要るなら、

1. `--dry-run` (退避と照合だけ) → 通常起動へ戻る
2. アプリが起き上がってから 1200bps でもう一度突入 → `--skip-backup` で書く

と**セッションを分ける**。これが `--dry-run` → `--skip-backup` の 2 手に
なっている理由。

`--expect-image` は「いま実機に載っているはずの像」。違うものが載っていたら
書かずに止まる。それが正しい動き。

`bootloader` / `partition table` / `nvs` / `otadata` / `uf2` / `user_fs` には
触らない。= CIRCUITPY のファイルも BLE のボンドも残る。

---

## 6. 戻し方

### 6-1. ソフトから戻す (ふつうはこれ)

段階 1 の像には**脱出路が 3 つ**入っている。どれも行き先は同じ、USB-OTG の
ROM ダウンロードモード (`303a:0009`) で、`tools/flash.py` がそこをつかまえる。

| 入り口 | 使うとき |
|---|---|
| **1200 bps タッチ** | `flash.py` が自動で使う。CDC を 1200 bps で開いて DTR を落とすだけ |
| コンソール `bootloader` | 手で落としたいとき |
| `QK_BOOT` キー (レイヤー 5) | ホストから見えなくなったとき。Q 長押し → レイヤー 4 の `MO(5)` → レイヤー 5 の該当キー |

1200 bps タッチの手順は CircuitPython と同じにしてある
(`supervisor/shared/usb/usb_device.c` の `tud_cdc_line_state_cb` と
`ports/espressif/common-hal/microcontroller/__init__.c` の `RUNMODE_BOOTLOADER`:
`chip_usb_set_persist_flags(USBDC_BOOT_DFU)` + `RTC_CNTL_OPTION1_REG =
RTC_CNTL_FORCE_DOWNLOAD_BOOT` → 再起動)。

CircuitPython 版へ戻す:

```
python3 firmware/tools/flash.py --rollback --skip-backup \
  --image firmware/build/stackee.bin \
  --image-sha <書き込んだときと同じ sha256> \
  --expect-image <いま本体に載っているはずの像>.bin
```

`--rollback` は「書く像」と「載っているはずの像」を入れ替えるだけ。

### 6-1b. ROM ダウンロードモードの入り口は 2 つある

**書き込みのあと通常起動へ戻す方法が、入り口によって違う。**
`flash.py` は繋がっているデバイスの PID を見て自動で選ぶ (`FINAL_RESET`)。

| 入り口 | PID | 入り方 | esptool の `--after` |
|---|---|---|---|
| USB-OTG の ROM (CDC + DFU) | `303a:0009` | 1200 bps タッチ / コンソール `bootloader` / `QK_BOOT` / RST を約 2 秒長押し | **`hard-reset`** |
| USB-Serial/JTAG | `303a:1001` | USB-Serial/JTAG 側からダウンロードモードへ | **`watchdog-reset`** |

理由:

* **USB-OTG (`0009`)** には DTR/RTS で引けるストラップ線が無い。esptool の
  `hard-reset` は同じ接続のまま `FORCE_DOWNLOAD_BOOT`
  (`RTC_CNTL_OPTION1_REG` = `0x6000812C`) を消して watchdog リセットする。
  1200 bps タッチや `QK_BOOT` はこのビットで ROM に入るので、**消さないと
  次の起動もダウンロードモードになる**。だから `hard-reset` が要る。
* **USB-Serial/JTAG (`1001`)** では DTR/RTS が `EN` と `IO0` のストラップに
  繋がっている。ここで `hard-reset` を使うと、**DTR が立ったままなので
  `IO0` が低いまま再リセットされ、もう一度ダウンロードモードで起動する**
  (2026-09-16 に発生。像は正常だったのに 15 秒待っても `303a:811A` に
  ならず、`303a:1001` のままだった)。`watchdog-reset` はストラップを
  触らずウォッチドッグで落とすだけなので、これを使う。

手で `esptool` を叩くときも同じ。USB-Serial/JTAG から書いたら:

```
python -m esptool --chip esp32s3 -p <port> --before no-reset --after watchdog-reset \
  write-flash --flash-mode keep --flash-freq keep --flash-size keep \
  0x10000 firmware/build/stackee.bin
```

### 6-2. ソフトから戻せないとき (物理)

本体が固まって 1200 bps タッチも効かない場合:

1. **RST を、緑の LED が点くまで約 2 秒長押し**する。これで ROM の
   ダウンロードモードに入る。
2. そのうえで上と同じコマンドを打つ。

```
python3 firmware/tools/flash.py --rollback --skip-backup \
  --image firmware/build/stackee.bin \
  --image-sha <sha256> \
  --expect-image <いま本体に載っているはずの像>.bin
```

`flash.py` は既に ROM モードなら 1200 bps タッチを飛ばす
(`enter_rom()` の「既に ROM ダウンロードモード」)。

フラッシュに触らず ROM 経由で再起動だけしたいとき:

```
python3 firmware/tools/flash.py --reboot
```

---

## 7. 実機での確認

```
python3 firmware/tools/check_phase1.py            # 段階 1 の合否
python3 firmware/tools/check_phase2.py            # 段階 2 の合否
python3 firmware/tools/check_phase3.py            # 段階 3 の合否 (★ 先にヌル出力にする)
python3 firmware/tools/check_ble.py              # BLE が Mac に繋がるか (§15)
python3 firmware/tools/check_phase0.py --wait     # 起動時間 (段階 0 の合否)
python3 firmware/kmk/tools/stackee_console_client.py status
```

どれも読むだけで、書き込みも再起動もしない。
`hello` / `status` の `fw` は **`stackee-idf/3`**。前の段階の像 (`/0` / `/1`) と
見分けがつくようにしてある。

`check_phase1.py` が見るのは:

| 項目 | 合否 |
|---|---|
| TCA8418 | つながっている |
| 取りこぼし | FIFO 溢れ 0 / I2C 失敗 0 / 配線の無いスロット 0 |
| キー → レポート | 中央値 ≤ 2 ms、最大 ≤ 5 ms |
| 入力タスクの周期 | 1 ms 前後 |
| 送信キュー | 捨てた数 0 |

★ 遅延の数字は **しばらく普段どおり使ってから** 読む。時間を決めて
打ってもらうような検査はしない。`status` の `perf` は直近 256 標本の
中央値と、reset からの最大を覚えている。

### VIA / Remap で配列を変える

1. 本体を USB でつなぐ (dev プロファイルなので CDC も一緒に見える)。
2. VIA アプリ (https://usevia.app) を開く → 設定 (歯車) → `Show Design tab`
   と `Use V2 definitions` を有効にする。
3. `Design` タブで `firmware/via/stackee.json` を読み込む。
4. `Configure` タブに Stackee が出る。キーを押して割り当てを変える。
5. Remap (https://remap-keys.app) の場合は `+ KEYBOARD` →
   `Find your keyboard` でデバイスを選び、定義が見つからないと言われたら
   同じ JSON を読み込ませる。カタログ登録はしていない。

書き換えた配列は NVS のブロブに入る。**書き戻しは「静まってから 400 ms」に
1 回だけ**なので、1 キーずつフラッシュを叩くことはない (打鍵列テストで確認)。
再起動後も残る — ただしこれは**実機で確かめていない**。

独自キー (会話・音量・BLE 切替など) は VIA の `Custom` タブ (customKeycodes)
に出る。並び順が実装とずれると別のキーとして表示されるので、
`tools/test_gen_keymap.py` がそこを機械照合している。

★ **新しい独自キーは並びの "うしろ" に足す。** customKeycodes の並び順は
そのままキーコードの番号 (`0x7E00` から) になり、**VIA で変えた配列として
NVS に保存されている**。途中に足すとうしろが 1 つずつずれ、保存済みの配列の
意味が黙って変わる。置き場は `tools/gen_keymap.py` の
`TRAILING_CUSTOM_KEYS` (2026-09-21 の `MIC_F13`〜`MIC_F24`、そのあとに
足した `MIC_F1`〜`MIC_F12` がそれ。`STK_MT_0` = `0x7E07` は動かしていない)。

★ **既定配列を変えたら「移行」を足す。** 書き換えた本体は保存済みの配列で
動くので、**新しい既定は黙って無視される**。
`main/qmk_port/stackee_keymap_migrate.c` の表に 1 段足すこと (§10-1)。

### 配列を変えたら

`firmware/kmk/keymap.py` を直したら、生成し直す:

```
python3 firmware/tools/gen_keymap.py
python3 firmware/tools/test_keyseq_host.py
firmware/build.sh
```

**生成物 (`main/keymaps/default_keymap.c` / `main/qmk_port/stackee_keycodes.h` /
`via/stackee.json`) は手で編集しない。**

---

## 7a. 起動ログを読む

起動直後のログは、ホストが CDC を開くより前に流れてしまう。段階 1b で
「BLE が起動しない」を調べようとしたとき、原因を書いているはずの
`ESP_LOGE` が 1 行も読めなかった (2026-09-16)。以後どの段階でも同じことが
起きるので、**起動のいちばん最初から** 16 KB のリングバッファに溜めて、
あとから読めるようにしてある。

```
python3 firmware/kmk/tools/stackee_console_client.py log.tail
```

`{"cmd":"log.tail","bytes":600}` で末尾の長さを指定できる (1 枠に収まる
600 バイトが上限)。応答には `held` (いま持っている量)、`written` (起動から
書いた総量)、`dropped` (溢れて捨てた量) も入る。

`status` の `blex.err` / `blex.err_code` も同じ目的。BLE の立ち上げが
どこで失敗したかを、ログが読めなくても数字だけで分かるようにしてある
(`""` = 最後まで通った、`waiting_sync` = NimBLE の同期待ち)。

## 7b. BLE

| | |
|---|---|
| スタック | NimBLE + esp_hid (`esp_hid_device` 例と同じ構成) |
| 名前 | `stackee` (現行 `code.py` の `BLE_NAME` と同じ) |
| 外観 | キーボード (`ESP_HID_APPEARANCE_KEYBOARD`) |
| アドレス | **公開アドレス** (efuse の MAC)。CircuitPython と同じ決め方 |
| ペアリング | Just Works (`sm_io_cap = NO_IO` / `sm_mitm = 0` / `sm_sc = 0`)。パスキー入力は要らない |
| ボンド | NimBLE の `store/config` → nvs の名前空間 `nimble_bond` |
| 記述子 | **USB と同じ配列そのもの** (Report ID 1/2/3、Usage Maximum 0xFF) |
| 未接続時 | 1 秒ごとにアドバタイズし直す (現行 `BLEHID.ble_monitor` と同じ) |

### 記述子と NimBLE のレポート上限

BLE 側の HID サービスは、記述子から作られる**レポートの数**に上限がある
(`CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS`、既定 3)。超えると
`ble_svc_hid.c:725` が弾き、`esp_hidd_dev_init()` が `ESP_FAIL` を返して
**BLE が丸ごと立ち上がらない**（2026-09-16 の実機でこれを踏んだ）。

我々の記述子から esp_hid が作るレポートは **7 個**:

| Report ID | 種別 | protocol | 長さ |
|---|---|---|---|
| 1 キーボード | INPUT | report / boot | 8 |
| 1 キーボード | OUTPUT (LED) | report / boot | 1 |
| 2 マウス | INPUT | report / boot | 5 / 3 |
| 3 コンシューマ | INPUT | report | 2 |

boot protocol ぶんも別のレポートとして数えられるので、コレクション 3 つで
7 個になる。`CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS=8` にしてある。

記述子を増やすときに黙って踏まないよう、**IDF の
`esp_hid_parse_report_map()` そのもの**をホストビルドに取り込んで
数え直すテストがある:

```
python3 firmware/tools/test_hid_report_map.py
```

記述子のバイト列は `main/stackee_usb.c` の実体から抜き出すので、
「USB と BLE で同じ配列」という約束はテスト側でも崩れない。

### 現行 CircuitPython 版のボンドを引き継ぐ

ペアリングのパラメータは **CircuitPython と 1 行ずつ同じ**にしてある
(出所: `firmware/cp-uac/circuitpython/ports/espressif/common-hal/_bleio/Adapter.c`
の `common_hal_bleio_adapter_set_enabled()` と `_on_sync()`):

| | CircuitPython | こちら |
|---|---|---|
| ボンド保存 | `ble_store_config_init()` → nvs `nimble_bond` | 同じ |
| `sm_io_cap` | `BLE_SM_IO_CAP_NO_IO` | 同じ |
| `sm_bonding` | 1 | 同じ |
| `sm_mitm` | 0 | 同じ |
| `sm_sc` | **0** | 同じ |
| 鍵の配布 | `ENC | ID` を双方向 | 同じ |
| アドレス | `ble_hs_util_ensure_addr(false)` + `ble_hs_id_infer_auto(0, ...)` = 公開 | 同じ |
| nvs パーティション | 0x9000 / 20 KB | 同じ (書き換えない) |

同じ ESP-IDF の同じ NimBLE をビルドするので、ボンドの記録の形も同じ。
**鍵 (`our_sec` / `peer_sec` / `local_irk`) はそのまま効く見込み。**

残る不確かさは 1 つだけ: `cccd_sec`（相手が有効にした通知の記録）は
**属性ハンドルに依存する**。CircuitPython の HID サービス (adafruit_ble) と
esp_hid の GATT は構成が違うので、ハンドルがずれている可能性がある。
普通はこれを GATT の Service Changed が解決する
(`esp_hid` の NimBLE 実装は `ble_svc_gatt_init()` を呼んでいるので
Service Changed は存在する) が、効かなかった場合の逃げ道として、
**鍵を残したまま購読の記録だけを捨てる**コマンドを用意してある:

```
python3 firmware/kmk/tools/stackee_console_client.py ble.drop_cccd
```

これは Mac 側の操作を必要としない（ペアリングは残る）。

引き継げているかどうかは、実機の nvs を読めば書き込み前に分かる:

```
# 読むだけ。書き込みはしない
python -m esptool --chip esp32s3 -p <port> --before no-reset \
    --after watchdog-reset read-flash 0x9000 0x5000 nvs.bin

python3 firmware/tools/nvs_dump.py nvs.bin
```

`nimble_bond` に `peer_sec` / `our_sec` があれば鍵は残っている。
`cccd_sec` の有無で、上の逃げ道が要るかどうかの見当がつく。

## 8. 実機で確認済みのこと (2026-09-16)

段階 1 の像は実機で動いている。以下はすべて**実測**。

| 項目 | 結果 |
|---|---|
| USB キーボード | 認識され、打鍵がホストに届く |
| コンソール | `hello` / `status` / `reset` / `bootloader` / `log.tail` が応答 |
| 起動ログ | `log.tail` で CDC 接続前のログまで読める |
| 1200 bps タッチ → ROM → 復帰 | 通る (ソフトからの戻し道が生きている) |
| LCD | 起動表示。向きは MADCTL `0xA8` で正しい |
| FAT / 電池 | 目録を読める。残量が出る |
| **BLE** | 起動 0.7 秒でアドバタイズ、0.3 秒後に Mac が接続、暗号化 status=0 |
| **ボンドの引き継ぎ** | **再ペアリング不要**。CircuitPython 版のボンドがそのまま効いた |
| BLE 接続間隔 | 15 ms。60 秒安定 (起動直後の切断・再接続 1 回のみ) |
| 書き込み | ROM 突入 1 回 + esptool 1 回で成功 |

### 途中で踏んだもの (再発防止の仕掛け付き)

| 症状 | 原因 | 今どうしてあるか |
|---|---|---|
| 書き込み後、通常起動せず `303a:1001` のまま | USB-Serial/JTAG で `--after hard-reset` を使った。DTR が IO0 を下げたまま再リセットされる | `flash.py` が PID を見て `--after` を選ぶ。テスト 8 件 |
| ROM が固まる | 同じ ROM セッションで esptool を 2 回呼んだ | `flash.py` が 2 回目を**呼ぶ前に**止める |
| BLE が起動しない (`blex.err="esp_hidd_dev_init"`) | `CONFIG_BT_NIMBLE_SVC_HID_MAX_RPTS` の既定 3 に対し、記述子から作られるレポートが 7 個 | 8 に変更。`test_hid_report_map.py` が記述子から数え直して突き合わせる |
| 原因のログが読めない | 起動ログが CDC 接続前に流れる | 16 KB のリングバッファ + `log.tail` |
| `blex.err` が繋がっても `waiting_sync` | `esp_nimble_enable()` の後に代入していて、先に飛んできた `START_EVENT` の `""` を上書きしていた | 代入を前に移した |

## 9. まだ確かめていないこと

- **キー → HID 送出の遅延の実測**。目標は中央値 2 ms / 最大 5 ms
  (DESIGN.md §3)。`check_phase1.py` の `perf.input` で読む。
  人手を借りずに測りたいときは `key.inject` (下記)。
- **VIA / Remap** — 記述子はコレクション 1 つ・Report ID なしで素の VIA と
  同じ形だが、アプリから実際に配列を書き換えたことはまだない。
- **NVS に保存した配列が再起動をまたぐか** — ホストテストでは確認済み
  (書き戻しが 1 回にまとまることも)。実機では未確認。
- **BLE 越しの VIA** — 段階 1b の実験項目。BLE 側に Raw HID を出していない
  ので、いまは応答を捨てている。
- **マウスキー** — ホストビルドでは動いている (移動・加速・ボタン)。
  実機で USB / BLE に出たことはまだない。
- **`STK_HID_SWITCH` / `STK_BLE_REFRESH`** — 実機で押したことはない。
- **長時間の安定性** — 60 秒しか見ていない。

### 人手ゼロで打鍵を確かめる

```
python3 firmware/kmk/tools/stackee_console_client.py key.inject
# {"cmd":"key.inject","kc":"F24","hold_ms":30}
# {"cmd":"key.inject","kc":32264,"hold_ms":1500,"wait":false}   押し始めてすぐ返る
```

配線の無いスロット (ROW4 / COL0) にキーコードを一時的に置き、**TCA8418 の
イベントとして**押して離す。デバウンス → QMK → 送信キュー → hid_out と、
普段の打鍵とまったく同じ道を通る。終わったら元に戻す。

★ **`"wait":false` は押し始めてすぐ返る** (2026-09-21 に足した)。既定は
離し終わるまで待つので、その間コンソールのタスクが止まり **`ui.status` や
`lcd.crc` を読めない**。押している最中の画面を見たいとき (`STK_MIC_KEY` の
表情など) に使う。**遅延は返らない**ので、遅延を測るときは既定のまま。

★ 押している時間の上限は **3000 ms**。2026-09-21 まで 2 つの落とし穴があり、
`hold_ms` が 1000 を超えると黙って 30 ms になり、さらに「押下レポートが
出ないまま 500 ms」の諦め判定が**出ていても**効いていたので、
1500 ms を頼んでも実機では約 600 ms しか押していなかった。どちらも直した
(諦めるのはレポートが出ていないときだけ)。

応答:

| 項目 | 意味 |
|---|---|
| `press_ms` / `release_ms` | 注入から HID レポートがキューに載るまで |
| `dest` | そのとき実際に使った送信先 (`BLE` / `USB`) |
| `pushed` | 送信キューに積まれたレポート数 |
| `sent_usb` / `sent_ble` | 実際に出た数 |

既定のキーが F24 なのは、**ホスト側で何も起きないキー**だから。
検証のたびにエディタへ文字が入ったりしない。名前で指定できるのは
F13〜F24 と LANG1 / LANG2。ほかは数値 (`"kc":115`) で渡す。

---

## 10. 段階 2 (画面)

> **顔とステータスバーが、現行 CircuitPython 版と同じ絵・同じタイミングで出ること。**
> そして **顔を描いている最中も打鍵の遅延が悪くならないこと。**

確認は全部「本体の CRC32」と「Mac の期待値」の照合で行う。
**画面を目で見る必要はない。**

### 10-1. 何をどう描いているか

| | |
|---|---|
| 画面 | 240x320 (MADCTL `0xA8`)。背景は白。ただし字幕の帯 (y=250..319) は**いつでも黒** |
| 上段 | 高さ **28px** の黒帯。`[音量][NN%]` … `[Wi-Fi][BLE/USB][電池][NN%]` |
| 顔 | 240x240 を **上 29 px / 下 11 px 切り詰めた 240x200** を **x=0 / y=50** に (y=50..249)。空いた 40 px は字幕 3 行へ。29/11 は 32 コマ全部の余白の最小値で**1 画素も落ちない** (§23-8) |
| 顔の素材 | `faces.bin` (zlib / 4bpp / 32 枚の縦長シート)。**素材は 240x240 のまま。** 起動時に PSRAM へ 900 KB 展開し、その場で 240x200 (750 KB) へ詰め直す |
| 4bpp の並び | **画素 0 が上位ニブル**。パレットは `i*17` の等間隔グレー 16 段 |
| 差分 | `changes.bin` の bbox (32x32 組 x 4 バイト) を **1 周 16 行ずつ**。bbox は元の 240x240 の座標なので、描くときに 29 行ぶん上へ寄せて捨てた行を落とす |
| アイコン | `status_icons.bin` (zlib / 2bpp / 24x24 x 18 枚)。**画素 0 が最上位 2 ビット**、濃さ 0 は透明 |
| 数字 | `status_h24.bdf` (12x24) から `0123456789%-? ` の 14 文字だけ起動時に展開 |
| 展開器 | **ESP32-S3 の ROM にある tinfl** (`esp32s3.rom.ld` の `tinfl_decompress`)。像は 1 バイトも増えない |
| タスク | `ui` は CPU0 / 優先度 3 / 5 ms 周期。入力 (CPU1 / 最高) とは別の CPU |

顔の状態機械 (`awake` 2 秒 → `idle`、グループ 3000 ms、フレーム
聞き取り 250 / 考え中 700 / 発話 250 ms、打鍵後 1000 ms はまばたきを始めない、
撮影後 1500 ms は `camera`) は `firmware/kmk/stackee_face.py` から
**1 行ずつ移した**。段階 2 では会話もカメラも無いので、実際に出るのは
`awake` と `idle` だけ。

**表情の選び方** (`stackee_face_pick_state`。上から順に見て最初に当たったもの):

| 順 | 入力 | 表情 | どこから来るか |
|---|---|---|---|
| 1 | `speaking` | `speaking` | 返答の再生中 (audio) |
| 2 | `talk_recording` | `listening` | STK_TALK を押している (本体が録音中) |
| 3 | **`mic_held`** | **`listening`** | **`MIC(kc)` のキー (PC 側のプッシュトゥトーク) を押している (2026-09-21)** |
| 4 | `talk_busy` | `thinking` | 送信・返答待ち・受信 |
| 5 | `camera_active` (と撮影後 1500 ms) | `camera` | カメラ |
| 6 | 起動から 2 秒以内 | `awake` | — |
| 7 | それ以外 | `idle` | — |

★ **`mic_held` は本体の録音より下、考え中より上。** 本体が自分で録っている
ならそちらが勝ち、返答を待っている間に人が PC へ喋り始めたら耳の顔に戻る。
**新しい表情は作っていない** — `listening` (耳が動く 3 コマ、250 ms 送り) を
そのまま使う。印は入力タスクが立てる atomic な `bool` 1 つで、ui タスクが
毎周読むだけ。**打鍵の道には何も足していない。**

`ui.status` に `mic_held` が出る。

#### `MIC(kc)` — 任意のキーを包む

`LT(layer, kc)` / `MT(mod, kc)` と同じ発想。**中の基本キーコードを 8 bit
そのまま持つ**ので、どのキーでも包める。

```
MIC(kc) = 0x7F00 | (kc & 0xFF)        kc は 0x04..0xFF (修飾なしの基本キー)
MIC(KC_F13) = 0x7F68                  既定配列の右下 (レイヤー 0 / row 3 / col 9)
```

押下で中のキーを `register_code`、離しで `unregister_code` — **ホストから
見た振る舞いは素のキーとまったく同じ**。押している間だけ `mic_held` が立つ。

| 使い方 | どうするか |
|---|---|
| VIA / Remap の `Custom` タブ | **`MIC_F13`〜`MIC_F24`** と **`MIC_F1`〜`MIC_F12`** の 24 個が並ぶ (この順。うしろにしか足さない) |
| それ以外のキー | Remap の **「Any」** に `0x7F00 \| kc` を 16 進で入れる (例: `MIC(KC_A)` = `0x7F04`) |

★ **なぜ 2 通りあるのか。** VIA の `customKeycodes` は並び順がそのまま
キーコードの番号 (`QK_KB_0` = 0x7E00 から) になり、`QK_KB` は
**0x7E00..0x7E3F の 64 個しかない**。「MIC + 8 bit」の 256 個の連続領域は
そこに入らないので、領域は `QK_USER` (0x7E40..0x7FFF) の上半分に置き、
VIA からは 24 個の名前付きの入口を通す (本体が `MIC(kc)` に読み替える)。
入口の並びは **`MIC_F13`〜`MIC_F24`、そのうしろに `MIC_F1`〜`MIC_F12`**。
F1〜F12 はあとから足したので (2026-09-21)、先にあった F13〜F24 の番号
(0x7E08〜0x7E13) は動かしていない — **保存済みの配列がその番号で入っている**。

#### ★ 既定配列を変えても、保存済みの配列は変わらない (2026-09-21 に実機で踏んだ)

`MIC(KC_F13)` にした像を入れても**顔がまったく変わらなかった**。VIA の
読み出し (`0x04`) で見たら、保存済みの配列は
**`layer 0 / row 3 / col 9` = `0x0068` (素の `KC_F13`)** のままだった。

**VIA / Remap で配列を 1 度でも書き換えた本体は、以後 EEPROM (NVS) に
保存された配列で動く。** 像を新しくしても `main/keymaps/default_keymap.c`
は見に行かない。**新しい既定は黙って無視される。**

そこで「移行」を足した (`main/qmk_port/stackee_keymap_migrate.c`)。

* EEPROM の **キーボード用の 4 バイト** (`eeconfig_read_kb` /
  `eeconfig_update_kb`。VIA はここを触らない) に移行番号を持つ。
* 起動時 (`keyboard_init()` のあと) に、**未適用の移行を古いほうから順に**
  当てる。当て終わったら番号を進める。二度と当たらない。
* 移行は「**旧い既定のままなら差し替える。ユーザーが変えていたら触らない**」
  で書く。保存済みの配列はユーザーのものなので、知らない値を上書きしない。

| 移行 | 中身 |
|---|---|
| 1 (2026-09-21) | レイヤー 0 / row 3 / col 9 が `KC_F13` (旧い既定) か `0x7E08` (同じ日に一度だけ存在した `STK_MIC_KEY`) なら `MIC(KC_F13)` にする |

★ **既定配列を変えたら、ここに 1 段足すこと。** 足さないと、VIA を使った
ことのある本体にだけ新しい既定が届かない、という見つけにくい形で壊れる。

**実機での確認** (`15e9e69`、full、2026-09-21)。`key.inject` で
`MIC(KC_F13)` (`0x7F68`) を **1.5 秒押し**、100 ms ごとに `ui.status` と
`lcd.crc y=50 h=200` を読んだ。顔の CRC32 は `render_expected.py` の
期待値と突き合わせてある (下の「顔」は推測ではなく **CRC が合ったコマ**)。

```
押す前                                   state=idle  mic_held=False
t[ms]  state      grp frm cur  mic    crc32        顔 (CRC で特定)
109    listening  0   2   9    True    267893468   12 listening/a_02
329    listening  0   2   12   True   1899617587   10 listening/a_00
520    listening  0   0   10   True   1899617587   10 listening/a_00
710    listening  0   1   11   True   3856223340   11 listening/a_01
917    listening  0   2   12   True    267893468   12 listening/a_02
1132   listening  0   0   12   True   1899617587   10 listening/a_00
1499   listening  0   1   11   True   3856223340   11 listening/a_01
1684   idle       3   0   5    False  1128958265    5 idle/d_00
```

包むキーを変えても同じ (`key.inject` で 0.6 秒押し、`ui.status` を読む):

| 投げたもの | `mic_held` | 表情 |
|---|---|---|
| `MIC(KC_F14)` = `0x7F69` | **True** | `listening` |
| `MIC(KC_F24)` = `0x7F73` | **True** | `listening` |
| VIA の入口 `MIC_F14` = `0x7E09` | **True** | `listening` (本体が `MIC(F14)` に読み替えた) |
| 素の `F14` = `0x0069` | False | `idle` (包んでいないので顔は変わらない) |

聞き取り中の 3 コマ (`listening/a_00` `a_01` `a_02` = 顔 10/11/12) が
**250 ms ごとに順送り**で出て、離すと `idle` に戻っている。
`key.inject` は **本物の F13 を Mac へ送る**が、無害なキーなので
これだけは注入してよいことにしてある (ほかのキーは注入しない)。

★ `ui.status` と `lcd.crc` は別々の往復なので、顔が変わる 250 ms の境目を
またぐと `cur` と CRC のコマが 1 つずれて見えることがある (上の t=849)。
**画面そのものは常に完全な 1 コマ**で、CRC はどれも期待値と一致している。

### 10-2. 実機に触らない確認

```
python3 firmware/tools/test_render_host.py     # 描画と期待値 (27 件)
python3 firmware/tools/test_faceanim_host.py   # 状態機械の時刻と表情の選び方 (20 件)
python3 firmware/tools/render_expected.py      # 期待値の CRC を見る
```

`test_render_host.py` が段階 2 の要。**実機と同じ描画コードそのもの**
(`main/stackee_draw.c` ほか) を Mac 用にビルドして本物の素材を通し、
`tools/render_expected.py` が Python で別に組み立てた絵と CRC32 で
突き合わせる。見ているのは:

* 32 表情ぜんぶ。しかも顔 0 を全面で描いたあとは **`changes.bin` の差分だけ**で
  1 枚ずつ寄せて、全面で描いた絵と同じ CRC になること (= 差分表の読み方が正しい)
* ステータスバーの代表 6 状態 (`preview_status_bar.py` の 5 つ + 起動時の既定)
* タイル番号・色・文字・配置が `firmware/kmk/stackee_icons.py` と同じこと
  (電池 20/40/60/80、音量 0/33/66 の境目を総当たり)
* CRC-32 が `zlib.crc32` と同じ値になること
* 顔の切り詰め (上 29 行 / 下 11 行) を C と Python が同じようにすること。
  `faces.bin` そのものは 240x240 のまま 1 バイトも変わらないこと。
  **切り詰めで落ちる非背景画素が 32 コマ全部で 0** で、しかも 29/11 が
  余白の最小値そのもの (1 行でも増やすと欠ける) であること (§23-8)

**ここで通っても実機で動く保証にはならない。** FAT も PSRAM も ROM の
展開器も入っていない。実機でずれたら疑うのはその 3 つ。

### 10-3. 実機での確認 (読むだけ)

```
python3 firmware/tools/check_phase2.py
python3 firmware/tools/check_phase2.py --no-selftest   # 遅延と perf だけ
```

| 項目 | 合否 |
|---|---|
| 素材の展開 | `ui.assets` の CRC32 が Mac の `zlib.decompress` と一致 |
| 32 表情 | `ui.selftest` の 32 個の CRC32 (y=50 h=200) が `render_expected.py` と一致 |
| ステータスバー | 同 6 個 |
| 打鍵の遅延 (平常) | `key.inject` の中央値 ≤ 6 ms |
| 打鍵の遅延 (描画中) | **`ui.selftest` を回しながら** `key.inject`。中央値 ≤ 6 ms / 最大 ≤ 10 ms |
| 顔 1 コマの描画 | `perf.ui_face` |
| LCD 転送 | `perf.ui` |

★ `ui.selftest` は **1 周 1 手ずつ**進む (32 表情 + バー 6 状態 = 38 手)。
走っている間もコンソールが答えるので、`key.inject` を同時に回して
「描いている最中の遅延」を測れる。これが段階 2 の合否のひとつ。

不一致だったときは `check_phase2.py` がそのまま場所を絞る:
`face.set` / `bar.set` で画面を既知の状態に固定 → `lcd.crc` に `y` / `h` を
渡して**行の範囲を二分探索** → 最初に食い違った行の先頭 16 バイトを
`lcd.dump` で読み、期待値と並べて出す。

### 10-4. console のコマンド (段階 2 で増えた分)

| コマンド | 返すもの |
|---|---|
| `lcd.crc` | フレームバッファ全体 / 顔 (240x200 @ y=50) / 上段 (240x28) の CRC32 |
| `lcd.crc` `{"y":N,"h":M}` | 行の範囲を指定した CRC32 (二分探索に使う) |
| `lcd.dump` `{"y":N,"x":X,"n":16}` | その位置の生バイト (16 進、最大 128 バイト) |
| `face.set` `{"state":"thinking","group":0,"frame":1}` | 顔を固定して描き、CRC32 を返す。`{"i":7}` で顔番号を直に指定 |
| `face.auto` | 固定を解いて状態機械に戻す |
| `bar.set` `{"bat":55,"chg":true,"vol":50,"wifi":"up","link":"ble","ble":false}` | バーを固定して描き、CRC32 を返す。`{"i":3}` で代表 6 状態を指定 |
| `bar.auto` | 固定を解く |
| `ui.selftest` | 自己テストを始める / 進み具合を返す / 終わっていれば CRC32 の配列 |
| `ui.status` | いまの顔・見送った回数・描いた枚数・フォント |
| `ui.assets` | 展開した素材のバイト数と CRC32 (字幕フォントの長さ・CRC32・字数も) |
| `ui.subtitle` `{"text":"1 行目\n2 行目"}` | 字幕の帯を描き、帯 (240x70 @ y=250) の CRC32 と画素数 (いちばん長い行) と描画時間を返す。`text` は改行区切りで**最大 3 行**。`text` 無し / 空で帯を消す (§23) |

```
python3 firmware/kmk/tools/stackee_console_client.py status
# 任意のコマンドは check_phase2.py / ConsoleClient.request() から投げる
```

### 10-5. 段階 2 でまだ確かめていないこと

- **実機での見え方**。CRC は「期待した画素が並んでいるか」しか見ていない。
  パネルに出た絵が正しいかは、`0xA8` の向きが段階 0 で確認済みなことに乗っている。
- **上段の文字の置き方**が現行 CircuitPython 版 (`adafruit_display_text` の
  `Label`) と 1 画素まで同じか。期待値は `preview_status_bar.py` の
  `draw_text` (baseline = `y_mid - box_h//2 + FONT_ASCENT`) に合わせてある。
  現行と並べて見比べたことはない。
- **PSRAM の帯域が入力に与える影響**。顔 1 コマで 115 KB を PSRAM に書くので、
  `check_phase2.py` の「描画中の遅延」で見る。実測はまだ。
- **内蔵 8x8 へのフォールバック**。`status_h24.bdf` が読めないときの道。
  実機で踏んだことはない (縦 3 倍・横 1.5 倍で 12x24 に引き伸ばす)。

---

## 11. 段階 3 (音声と通信)

> **会話 1 往復が pi400 経由で成立すること。会話中の打鍵遅延が目標内であること。
> 音量が再起動後も残ること。**

### 11-0. ★ 音を鳴らさないための「ヌル出力」

ユーザーは会社でもこの本体を使う。**検証で音を鳴らしてはいけない。**

```
python3 firmware/kmk/tools/stackee_console_client.py  # から audio.null を投げる
# {"cmd":"audio.null","on":true}
```

これを立てている間、再生は

* **I2S の TX チャネルを作らない**
* **AW88298 に 1 バイトも書かない** (アンプは起こさない)

で、実時間ぶんだけ送出カウンタを進める。`audio.status` の
`played` / `play_ms` がその数字。**物理的に音が出る経路に入らない**ので、
音量が何%であっても鳴らない。

`tools/check_phase3.py` は **いちばん最初に必ずこれを立てる**
(`--allow-sound` を付けたときだけ外れる。普段は使わない)。

**既定はヌルではない。** 提出後の普段使いでは本当に鳴らすため。

### 11-1. settings.toml から読むキー

FAT の根っこ (`/settings.toml`) を起動時に 1 回だけ読む。
**書き込みはしない** (FAT は読み取り専用でマウントしている。`settings.set` は段階 4)。

| キー | 要否 | 意味 |
|---|---|---|
| `STACKEE_TALK_URL` | **必須** | 会話の相手。`https://pi400.…:8443/talk` のような形。無いと会話が使えない |
| `STACKEE_TALK_TOKEN` | 任意 | `Authorization: Bearer` に載せる。**HTTPS のときしか受け付けない** (平文に載せない) |
| `STACKEE_TALK_MIN_MS` | 任意 | 短押しを捨てる下限 [ms]。既定 **1000**。0 でこの条件を見ない (§11-4 の「誤って触れたときは何も起こさない」) |
| `STACKEE_TALK_VOICE_RMS` | 任意 | 「声がある」とみなす 20 ms 窓の RMS。既定 **1000**。0 でこの条件を見ない |
| `STACKEE_TALK_VOICE_WINDOWS` | 任意 | その窓がいくつ要るか。既定 **5** (= 100 ms)。0 でこの条件を見ない |

★ 値はログにも `status` にも出さない。`status` に出るのは
`talk_url` / `talk_token` の**真偽だけ**。

読まないキー: `STACKEE_HOST` / `STACKEE_PORT` (旧 VoiceLink。移植しない)、
`STACKEE_WIFI_SSID` / `_PASSWORD` / `_CHANNEL` (proto 2 で廃止済み)、
`CIRCUITPY_WIFI_*` (supervisor のもの。起動が 4.8 秒 × 4 回延びるので使わない)。

### 11-2. 保存先が現行と違うところ (2 つとも「初回に移す」)

| もの | 現行 CircuitPython | こちら | 移行 |
|---|---|---|---|
| 音量 | FAT `/stackee_volume.json` | **NVS** `stackee`/`volume` | 初回起動で FAT の値を NVS へ写す |
| Wi-Fi 登録簿 | FAT `/wifi_networks.json` | **NVS** `stackee`/`wifi_nets` (JSON のまま) | 同上 |

**なぜ NVS か。** `user_fs` は摩耗平準化の無い生の FAT で、CircuitPython が
`esp_partition` を直に読み書きしている。ESP-IDF の書き込み用マウント
(`esp_vfs_fat_spiflash_mount_rw`) は WL 層を挟むので、この領域には載せ替えられない。
**読むだけなら生マウントで読める**ので、初回に読んで NVS へ移す。
音量については DESIGN.md §8b がこの移行を求めている。登録簿も同じ形にした
(こちらは設計に明記が無いので、§12 の「設計への提案」に挙げてある)。

移行の結果は `status` の `volume_src` (`nvs` / `fat` / `default`) で分かる。

### 11-3. Wi-Fi 自動接続

`firmware/kmk/stackee_wifi.py` の状態機械をそのまま移した。
状態名も時間も同じ (ステータスバーの Wi-Fi アイコンがこの名前で引く):

```
boot(2s) → load → radio → scan_start → scan_wait → scan_read
         → connect → linkup → up(10s ごとに生存確認)
```

| 決まり | 値 | 理由 (現行と同じ) |
|---|---|---|
| 最初の探索 | 起動 2 秒後 | 起動を 1 ms も延ばさない |
| 走査 | 1ch ずつ。1..11ch は 300 ms、12ch 以上は 800 ms | パッシブ走査は滞在が 3 倍 |
| チャネルの順 | 登録簿の ch → 前回のチャネル → CircuitPython の走査順 | 自宅の AP を 1 周期目で拾う |
| 接続先 | 見えた中でいちばん強い登録済み AP。同点は登録順 | `wifi_store.pick` と同じ |
| 登録簿の ch より走査の ch | 走査が勝つ | 古い ch を渡すと FAST_SCAN が空振りする (実測 11.3 秒) |
| connect を撃つ条件 | 押されているキーが無く、最後の打鍵から 3 秒 | 打鍵の谷でだけ撃つ。30 秒粘って来なければ出直す |
| 録音・再生中 | 無線に触らない | 現行 console の `wifi.scan` と同じ制約 |
| 失敗 | 無線を落として 60 秒後 | 倍々にしない (家に着いた瞬間に繋がってほしい) |
| 切断 | 2 秒後にやり直す | |

★ `esp_wifi_connect()` は即戻るので、現行でいう「段階B (非ブロッキング)」
相当で動く。ブロックする経路は入っていない。
★ 認証情報をドライバの NVS に置かない (`WIFI_STORAGE_RAM`)。登録簿はこちらが
持っているので、二重に持たせると「消したはずの AP へ勝手に繋ぐ」が起きる。

### 11-4. 会話

`firmware/kmk/stackee_talk.py` の TalkLink をそのまま移した。

```
idle --(42 キー押下)--> recording --(離す)--> upload --> poll_wait ⇄ poll
     --> audio --> play_wait --> playing --> idle
```

| 決まり | 値 |
|---|---|
| 録音 | 16 kHz / mono / 16bit。最大 **30 秒** (960,000 B)。**短押し / 無音は捨てる** (下の「誤って触れたときは何も起こさない」) |
| 送信 | `POST /talk` (`Content-Type: audio/wav`、44 バイトの RIFF ヘッダ + PCM) |
| 受理 | `202 {"id":..,"status_url":"/jobs/<id>"}` |
| 応答待ち | `GET /jobs/<id>?wait=25` (**ロングポーリング**)。中継が最大 25 秒握る |
| ポーリング | 即返ってきたときだけ **1 秒**あけて投げ直す。25 秒握られたら待たずに次。390 秒で諦める |
| 返答 | `{"state":"done","reply":..,"audio_url":..,"sample_rate":16000,"channels":1,"sample_width":2}` |
| 返答 PCM | `GET /jobs/<id>/audio` (生の 16 kHz mono 16bit)。上限 **120 秒ぶん** (3,840,000 B)。超える応答は受信の途中で断って会話を失敗させる |
| 一次回答 | 録音が終わった直後に `ack_0N.pcmz` をランダム再生。**その間も通信は進む** |
| 最終回答 | **一次回答が鳴り終わってから**鳴らす (`play_wait`) |
| スピーカー待ち | 15 秒で諦める |
| 返答文 | `status` の `screen` に出る (現行と同じ意味) |

#### 誤って触れたときは何も起こさない (2026-09-21)

キーに指がかすっただけで**一次回答が声を出し、録音がサーバへ飛ぶ**のを止める。
録音を終えた時点で 2 つとも満たしたときだけ、従来の流れへ進む。

| 条件 | 既定 | settings のキー |
|---|---|---|
| 録音の長さ ≥ | **1000 ms** (録音の長さ = キーを押していた長さ) | `STACKEE_TALK_MIN_MS` |
| 20 ms の窓の RMS ≥ | **1000** … の窓が **5 個**以上 (= 100 ms) | `STACKEE_TALK_VOICE_RMS` / `STACKEE_TALK_VOICE_WINDOWS` |

満たさなければ**静かに idle へ戻る** — 一次回答を鳴らさない、送信しない、
「考え中」の顔にもならない (`listening` から直接 `idle`)。ログに
`[talk] 短すぎ/無音のため破棄 (len_ms=…, loud=…, rms_max=…, …)` を残し、
`talk.status` の `dropped_short` / `dropped_silent` が増える。
サーバ側の防御 (0.3 秒未満は 400、peak<150 は ignored) は変えていない。

★★ **「いちばん大きい窓」では判定できない。** 実機で測ると、環境音しか
無い部屋でも **窓 5 (マイクを開けてから 100〜120 ms) の RMS が毎回
3,257〜3,945** になる (6 回測って毎回 窓 5。窓の平均は 266〜276)。
マイクの立ち上がりの跳ねで、`research/stackee/record_onset_2026-09-21.md`
の「ES7210 の CSM 起動 約 96 ms」と時刻が合う。最大だけを見ると、この跳ね
1 つで必ず「声あり」になってしまう。だから**越えた窓の数**で見る —
跳ねは 1 窓しか無いので越えられず、声は 100 ms も続けば 5 窓ある。

★ 測るのは**録音バッファ全体**を 1 度なめる形 (1 サンプルあたり掛け算 1 回、
audio タスクの上)。将来プリロール (録音の冒頭の取りこぼしを埋める先読み) を
足しても、そのぶんを含めて数える。

★ `talk.inject` (決まった PCM を流す検査の道) は録音を通らないので、
この切り捨ての外にある。
| 計時 | `[talk-http-timing]` / `[talk-turn-timing]` をログに出す |

通信は**専用タスク** (`stackee_http`、CPU0 / 優先度 4 / スタック 10 KB) が持つ。
audio タスクは 1 周期 1 回状態を読むだけなので、DNS も TLS も待たない
(`firmware/native-http/README.md` と同じ考え方)。TLS は `esp_crt_bundle`
(標準 CA バンドル) + ホスト名検証。リダイレクトは追わない。POST の自動再送はしない。

★ 返答 PCM は通信側のバッファを**そのまま鳴らす** (3.84 MB をコピーしない)。
だから `audio` の完了時には `http_close()` を呼ばず、再生が終わってから閉じる。

★ **録音と返答は長さが別で、PSRAM の上でも同時に存在しない。**
録音の 960 KB は `record_alloc()` が取り、`POST /talk` が受理された時点で
返す。返答の 3.84 MB は `GET /jobs/<id>/audio` を始めるときに通信側
(`stackee_http.c`) が PSRAM へ取り、再生が終わって `http_close()` を
呼ぶまで生かす。だから録音中に返答の受け皿が壊れる経路は無い。
秒数は `stackee_talksm.h` の `STACKEE_TALK_RECORD_SECONDS` (30) と
`STACKEE_TALK_REPLY_SECONDS` (120) の 2 つだけで決まり、バイト数との
食い違いは同じヘッダの `_Static_assert` が止める。

**PSRAM の見積もり (8,388,608 B)**

| いつ | 何 | バイト |
|---|---|---|
| 起動から | 起動ログのリング | 16,384 |
| 起動から | LCD フレームバッファ (240x320x2) | 153,600 |
| 起動から | 顔シート (240x240 4bpp x 32) | 921,600 |
| 起動から | 一次回答 5 本 (manifest の `samples` x 2) | 487,084 |
| 会話中 | 録音 (44 + 16000x2x30) — **受理で返す** | 960,044 |
| 会話中 | 返答の受信 (16000x2x120 + 1) — **再生おわりで返す** | 3,840,001 |
| 通信中 | TLS (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`。IN 16 KB + OUT 2 KB + 手続き) | 約 40,000 |
| 常時 | Wi-Fi / lwIP (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`) | 約 100,000 |

いちばん混む瞬間は**返答を鳴らしている最中** (録音はもう返してある):
1,578,668 + 3,840,001 + 約 140,000 = **約 5.56 MB**。残り **約 2.8 MB**。
撮影が重なっても (カメラのフレーム 153,600 + JPEG の作業 ~200 KB)
2.4 MB 以上残る。

★ **起動時のぶんだけは実機で合っている。** full `5a589186…` を書き込んだ
直後 (2026-09-19、会話していない状態) の `status.psram_free` は
**6,743,164 B**。上の「起動から」4 行 (1,578,668 B) を 8,388,608 B から
引いた 6,809,940 B との差は 66,776 B で、Wi-Fi / lwIP の枠 (約 100,000 B) に
収まる。★ **会話中のぶんは未確認。** 連続した 3,840,001 B が取れるか
(断片化) は、実際に会話が起きるまで分からない。取れなかったときは会話だけが
「返答の受け皿を確保できません (メモリ不足)」で失敗し、`stackee_http.c` が
PSRAM の空き合計と最大の塊をログに残す (`log.tail` で読む)。

★ **返答待ちだけ長く待つ。** 1 要求ごとに TLS を張り直すので、Funnel 経由では
1 往復が 5〜10 秒かかる。1 秒おきに撃つと会話 1 回で握手が 12 回走る。
そこで返答待ちの `GET` にだけ `?wait=25` を付け、中継 (`dev/server/proxy.py`)
に最大 25 秒握ってもらう。この要求のときだけ HTTP のタイムアウトを 40 秒に
する (`stackee_http.c` が URL の `?wait=` を見て決める)。ほかの要求は 30 秒の
まま。390 秒で諦める上限は変えていない。
★ 中継が古いと `?wait=25` は 404 になる。**本体より先に pi400 の中継を
更新する** (`dev/server/README.md`)。

### 11-5. 半二重の音 (現行と同じ理由)

マイク (I2S RX) とスピーカー (I2S TX) は BCK (G34) と WS (G33) が同じ線なので、
同時には持てない。だから鳴らす間だけマイクを畳む。

| | ピン | 相手 |
|---|---|---|
| MCLK | G0 (★ 基板の緑 LED と共有) | ES7210 のみ。AW88298 には出さない |
| BCK / WS | G34 / G33 | 両方 |
| DIN | G14 | ES7210 → ESP32 |
| DOUT | G13 | ESP32 → AW88298 |

* ES7210 (0x40) は **スレーブ**。ESP32 がクロックマスタ。レジスタは
  `firmware/kmk/es7210.py` と同じ値、PGA は `code.py` と同じ 37.5 dB (code 14)。
* AW88298 (0x36) は `firmware/kmk/stackee_speaker.py` と同じ順・同じ値
  (M5Unified.cpp:461-478 の写し)。`reg 0x06` は 16 kHz で `0x14C3`。
* 音量はレジスタ `0x0C` で当てる (波形は触らない)。0% は `HMUTE` も立てる。
* **マイクは起動時には開かない。** 会話か `audio.selftest` のときだけ開いて、
  終わったら畳む。現行は USB マイク (UAC) のために起動から開きっぱなしだが、
  UAC は段階 4 なので、いまは開かない (起動が速く、緑 LED も点かない)。

#### なぜ I2S の口を常設にしたか

録音・再生のたびに `i2s_del_channel` していたのをやめ、**最初に音を使うときに
1 回だけ作って以後は消さない**。切り替えは `i2s_channel_enable` /
`i2s_channel_disable` だけ。

* 理由は ESP-IDF の issue #18640
  (https://github.com/espressif/esp-idf/issues/18640)。
  `i2s_del_channel` のあとも GDMA チャネルに状態が
  残り、次にそのチャネルを掴んだ暗号エンジン (AES/SHA) が壊れた出力を出す。
  HTTPS が「署名検証に失敗」で落ちていたのがこれで、いまはソフト AES で
  逃げている。会話のたびに作って消すのをやめれば、そもそも踏まない。
* BCK/WS は 1 組しか無いので、RX と TX は同じポートの **全二重の組**として作る
  (`i2s_new_channel(&cfg, &tx, &rx)`)。両方 16 kHz / 2 スロット x 16 bit なので
  ESP-IDF が自動で全二重に組み、**先に std へ入れた RX がクロックの主**、
  あとの TX が従になる。従は BCK/WS を主から受けるので、**鳴らしている間も
  RX を回したままにする** (入ってくる音は読まずに捨てる)。使い方は今までどおり
  半二重で、マイクとスピーカーを同時には使わない。
* 録音を始めるときは RX を `disable` → `enable` し直す。ESP-IDF が enable で
  受信待ちの行列を空にするので、前の再生中に溜まった音が録音の頭に混ざらない。
* MCLK (G0 = 緑 LED) は口を作った時点から出続けてしまうので、使わない間は
  **パッドの出力だけ** `gpio_output_disable()` で止める。点くのは今までどおり
  録音中だけ。

### 11-6. 音量

| | |
|---|---|
| 値 | 0..100。`STK_VOLUP` / `STK_VOLDN` で 5 きざみ。0 でミュート |
| 反映 | ステータスバー (すぐ) と AW88298 のレジスタ (audio タスクが 5 ms 以内に) |
| 保存 | 値が変わってから **2 秒**、最後の打鍵から 2 秒、かつ鳴っていないときに NVS へ 1 回 |
| 保存先 | NVS `stackee`/`volume`。初回だけ FAT の現行値を移す |

★ 音量キーは**入力タスク (CPU1・最高優先度)** で処理される。そこでは
I2C も NVS も触らない (値を書き換えるだけ)。レジスタと NVS は audio タスクの
仕事にしてあるので、打鍵の道にフラッシュ書き込みが混ざらない。

### 11-7. console のコマンド (段階 3 で増えた分)

| コマンド | 返すもの |
|---|---|
| `wifi.scan` | 全チャネルを 1 回走査して `nets` (強い順に 10 件)。**録音・再生中は `audio_busy`** |
| `wifi.list` | 登録簿。`{"ssid","channel","has_password"}` のみ (★ password は出さない) |
| `wifi.add` `{"ssid","password","channel"}` | 追加 / 上書き。`bad_ssid` / `bad_password` / `bad_channel` / `full` |
| `wifi.remove` `{"ssid"}` | 削除。`not_found` |
| `wifi.connect` | いますぐ探索し直す (ブロックしない)。`wifi_state` を返す |
| `wifi.status` | 状態名 / SSID / IP / 登録件数 / 探索回数 / 接続回数 / 失敗回数 / `up_ms` |
| `audio.null` `{"on":true}` | **ヌル出力の切り替え** |
| `audio.selftest` | 1 秒録音して `samples` / `rms` / `peak` / `ms` |
| `audio.play` `{"i":0}` | 一次回答を「再生」し始める (非同期)。`samples` と `expect_ms` を返す |
| `audio.status` | 半二重の状態 / ヌル出力 / 再生位置 / 送出サンプル数 / 所要 ms |
| `talk.inject` `{"ms":1000,"i":0}` | FAT の PCM を録音の代わりに送る。**非同期**。始めたら即返す |
| `talk.status` | 会話の状態名 / ポーリング回数 / 各段の所要 ms / 返答文 / 通信の様子 / 字幕 (`sub_src` = `inline`・`url`・`none`、`sub_pages` / `sub_page` / `sub_bytes` / `sub_dropped`) |

`status` に増えた欄: `wifi` / `wifi_state` / `ssid` / `ip` / `nets` /
`connect_ms` / `wifi_up_ms` / `volume_save_pending` / `volume_src` /
`talk` / `audio_null` / `audio_busy` / `talk_url` / `talk_token` / `screen`。

### 11-8. 実機に触らない確認 (126 件)

```
python3 firmware/tools/test_cfg_host.py    # 設定・登録簿・音量・JSON・URL・素材 (48 件)
python3 firmware/tools/test_talk_host.py   # 会話の状態機械 + 字幕 + 切り捨て (73 件)
python3 firmware/tools/test_wifi_host.py   # Wi-Fi の状態機械 (21 件)
```

いちばん大事なのは **`test_cfg_host.py` が期待値を手で書いていない**こと。
`firmware/kmk/stackee_wifi_store.py` と `stackee_console.py` (settings.toml の読み) を
**そのまま import** し、`stackee_speaker.py` の `volume_bits` はソースから
関数だけ取り出して、同じ入力を C 版に流して 1 文字ずつ比べている。
「現行と同じ答えを出すか」が直接の合否になる。

`test_talk_host.py` / `test_wifi_host.py` は、**本物の状態機械**
(`main/stackee_talksm.c` / `main/stackee_wifism.c`) を Mac 用にビルドして、
偽の時計・マイク・スピーカー・通信・無線をつないで台本を流す。だから
390 秒のタイムアウトも 60 秒の再試行も、実時間を待たずに確かめられる。

`test_talk_host.py` の録音の領域は **本当に malloc する**。11 通りの終わり方
(領域が取れない / マイクが失敗 / 短すぎる / 送信できない / HTTP 500 /
受理応答が壊れている / ignored / 390 秒のタイムアウト / 注入 / …) で
「確保した数 == 返した数」かつ「必ず idle に戻る」を見ている。

**ここで通っても実機で動く保証にはならない。** I2S も esp_wifi も TLS も
入っていない。実機でずれたらその 3 つを疑う。

### 11-9. 実機での確認 (読むだけ)

```
python3 firmware/tools/check_phase3.py
python3 firmware/tools/check_phase3.py --no-talk   # 通信を使わない
python3 firmware/tools/check_phase3.py --scan      # wifi.scan も回す
python3 firmware/tools/check_phase3.py --json
```

| 項目 | 合否 |
|---|---|
| ヌル出力 | **最初に `audio.null` が true になっている** |
| マイク | `audio.selftest` の `samples` が 12,800 以上 (= 1 秒ぶんの 8 割。DMA が回っている) |
| スピーカー | `audio.play` の送出サンプル数が全部で、所要 ms が実時間どおり |
| Wi-Fi | `wifi_state` = `up`、IP が付いている、起動から接続までの ms |
| 会話 | `talk.inject` が 1 往復して `turns` が増える。各段の ms と返答文 |
| 打鍵の遅延 (平常 / 会話中) | 中央値 ≤ 6 ms / 最大 ≤ 10 ms (段階 2 と同じ基準) |
| 音量キー | `STK_VOLUP` で +5、ステータスバーの CRC が一致、2 秒後に `volume_save_pending` が false |

音量が**再起動をまたぐか**は 2 手で見る (人手ゼロ):

```
python3 firmware/tools/check_phase3.py          # 最後に「--expect-volume N」と出る
python3 firmware/kmk/tools/stackee_console_client.py reset
python3 firmware/tools/check_phase3.py --only-volume --expect-volume N
```

### 11-10. 段階 3 でまだ確かめていないこと

* **実機での音の確認そのもの。** 鳴らしていないので「本当に音が出るか」は
  ユーザーの普段使いでしか分からない。確かめてあるのは
  「レジスタの値が現行と同じ」「送出のサンプル数と時間が合う」まで。
* **I2S の MONO スロットで ES7210 の 2ch 出力を正しく拾えるか。**
  ESP-IDF の `I2S_SLOT_MODE_MONO` + `I2S_STD_SLOT_LEFT` で左スロットだけを
  読む作りにしてあるが、実機で通していない。`audio.selftest` の
  `samples` が 16000 前後にならなければここを疑う。
* **Wi-Fi と BLE の共存。** `CONFIG_ESP_COEX_SW_COEXIST_ENABLE` は段階 0 から
  入れてあるが、実際に Wi-Fi を点けた状態で BLE の打鍵が落ちないかは未計測。
* **TLS のメモリ。** ハンドシェイク中のヒープ残量は実測していない
  (`status` の `heap_min` で見る)。
* **FAT にそのファイルがあるか。** 音量 (`/stackee_volume.json`) と Wi-Fi 登録簿
  (`/wifi_networks.json`) を初回に読んで NVS へ移すが、**実機の FAT に実際に
  あるかを確かめていない**。無ければ既定値 (音量 20%、登録 0 件) で立ち上がる。
  `status` の `volume_src` と `nets` で分かる。
* **像が 2 倍になった。** 563 KB → 1,272 KB (ota_0 の 60%)。増えたぶんは
  ほぼ Wi-Fi と mbedTLS。戻し道 (`flash.py`) の手順は変わらない。

---

## 12. 設計への提案 (段階 3 を作っていて気づいたこと)

DESIGN.md を書き換えてはいない。**判断はユーザーと Fable のもの**なので、
ここに並べるだけにしてある。

1. **Wi-Fi 登録簿の置き場を NVS と明記してほしい** (§2 の「保存領域」)。
   設計は「素材 (…Wi-Fi 登録簿) は user_fs の FAT を **読む**」と書いている
   が、`wifi.add` / `wifi.remove` は**書く**必要がある。user_fs は摩耗平準化の
   無い生 FAT で、ESP-IDF の書き込み用マウントは WL 層を挟むため載せ替え
   られない。音量と同じ「初回に FAT から NVS へ移す」にしてある。

2. **`status` の `wifi` と `wifi_state` は別物、と決めてほしい。**
   現行 CircuitPython 版は `wifi` に `up`/`on`/`off`、`wifi_state` に状態機械の
   名前を出している。こちらも同じにしたが、Web 操作盤がどちらを見るかは
   決まっていない。

3. **`audio.null` を「本番の像でも残す」かどうか。**
   いまは残してある (検証のたびに像を作り分けたくないため)。誤って
   立てたままにすると音が出なくなるので、`status` の `audio_null` に出し、
   再起動で必ず false に戻るようにしてある (NVS に保存しない)。

4. **マイクを起動時に開かない、を段階 4 で見直す必要がある。**
   段階 3 では会話のときだけ開く。段階 4 で UAC (USB マイク) を足すと、
   ホストがいつ録りに来るか分からないので開きっぱなしになる。そのとき
   「会話の録音」と「UAC の供給」が同じ I2S RX を取り合うので、
   現行 `stackee_halfduplex.py` の 9 段 (USB マイクを止める → 戻す) に
   相当する段取りが要る。段階 3 の `enter_mic` / `enter_off` はその形に
   なっている。

5. **返答 PCM をコピーしない設計を明記してほしい。**
   120 秒の返答は 3.84 MB ある。通信側のバッファをそのまま鳴らすので、
   「`audio` が終わっても再生が終わるまで `http_close()` を呼ばない」という
   約束が要る。ここを知らずに触ると、鳴っている最中に領域が消える。

6. **`talk.inject` の PCM の出どころ。** いまは一次回答 `ack_01.pcmz` の
   先頭 1 秒を流用している (`firmware/kmk/tools` に検証用の PCM/WAV が
   無かったため)。中身は「わかったのだ。少し待っていてほしいのだ。」の
   冒頭なので、サーバ側の音声認識はたいてい何かを返す。**決まった文を
   返させたい**なら、検証用の短い WAV を素材に足すのがよい。

7. **段階 3 で像が 2 倍になった** (563 KB → 1,271 KB、ota_0 の 60%)。
   段階 4 で UAC とカメラを足すと 2 MB に近づく可能性がある。
   `CONFIG_MBEDTLS_DYNAMIC_BUFFER` や証明書バンドルの絞り込みで削れるが、
   **動いている設定からの差分を増やす**ことになるので、必要になるまで
   触らないほうがよい。

---

## 13. 段階 3 で独立レビューが見つけたもの (2026-09-16、全部直してある)

段階 3 のコードを、書いた本人とは別の目で読み直した。見つかったもののうち
**実機で困るもの**を、直した内容とともに残す。同じ間違いを段階 4 でしないため。

| 何が起きるはずだったか | 原因 | どう直したか |
|---|---|---|
| **登録簿の保存で入れ物の外に書く** (ASan で再現) | `at += snprintf(...)` は「入れたかった長さ」を足す。`at` が `cap` を追い越すと `out + at` が外を指し、`cap - at` が巨大な `size_t` に化けて書き放題になる | 足し算をやめ、境界を見てから写す形に。入り切らなければ **0 を返して空にする**。呼び手 (`save_list` / 移行) は 0 を失敗として扱う。制御文字を SSID とパスワードから弾き、`STORE_MAX` を 2,560 に |
| **8,192 バイトぴったりの応答で終端が無い** | `if (got < buf_cap)` — 入れ物は `limit + 1` 取ってあるので `<=` が正しい | `<=` に。ちょうど上限の応答も文字列として安全に読める |
| **`audio.selftest` を鳴らしている最中に受けると、audio タスクが永久に止まる** | マイクを開くと I2S の TX が消え、再生が終われない。会話も Wi-Fi の再接続も道連れ | `audio.play` と同じ busy 判定を入れた。`audio.null` の切り替えも再生中は断る。送り先が消えていたら再生を失敗として畳む |
| **`talk.inject` が console タスクからコーデックと I2S を触る** | 一次回答の再生がその場で始まるため | `audio.play` と同じく**頼むだけ**にして、audio タスクが拾う形に |
| **アンプがフルボリュームで立ち上がりうる** | `0x0C` にフル (`0x0064`) を書いてから絞る作りで、絞る側の I2C が失敗すると黙ってフルのままだった (`stackee_speaker.py` は例外を投げる) | 最初から当てたい値を書く。音量 0 なら `HMUTE` を立てたまま起こす。書き込みの失敗を見て、駄目なら**鳴らさずに畳む** |
| `wifi.add` が 33 バイトの SSID を黙って 32 バイトに詰めて登録する | 登録簿の大きさの入れ物へ直に読んでいた | 大きめに読んでから検査する (`stackee_wifi_parse` と同じ守り) |
| `wifi.connect` が `wifi.scan` の無線の借用を勝手に取り上げる | `kick()` が `paused` を落としていた (`stackee_wifi.py` は触らない) | 触らないようにした |
| 会話中に無線を点けたまま最長 390 秒待ち続ける | 走査開始の `audio_busy` 待ちに上限が無かった | 30 秒で出直す (接続待ちと同じ上限) |
| 応答待ちの 390 秒が送信時間ぶん短い | 起点が「送り始めたとき」だった | 「受理されたとき」に (`stackee_talk.py` の `self.since` と同じ) |
| 鳴っている最中に 960 KB 確保してから断る | 半二重を取るのが確保のあとだった | 取ってから確保する (`stackee_talk.py` と同じ順) |
| 音量の保存に失敗すると 2 秒おきに叩き続ける | `changed_at` を置き直して代用していた | 5 秒の別の待ちにした (`stackee_volume.py` の `_save_failed_at` と同じ) |
| 鳴らしたあとマイクが戻らないまま終わる | やり直しが無かった | 3 回までやり直す (`stackee_halfduplex.py` の `RECOVERY_RETRIES` と同じ) |
| `wifi.status` が錠の外で状態機械を読む | — | 錠の中で読む |
| 通信の記録を state を公開した**あと**にログへ出す | 次の要求に上書きされうる | 公開の前に出す |

**見つからなかったもの** (確かめた上で): 会話の状態機械の領域の寿命
(11 通りの終わり方で確保と返却が一致)、`stackee_jsonlite` の読み取り
(40 万件の壊れた JSON を ASan で流して無事)、入力タスクを止める呼び出し、
錠の輪 (デッドロック)。

---

## 14. 実機で踏んだもの (2026-09-16、段階 3 の 1 回目の書き込み)

像 `70b43879…` を実機に入れて `check_phase3.py` を回した結果。

**通ったもの**: 起動、ヌル出力、打鍵の遅延 中央値 1.53 ms、マイク 15,872 サンプル
/ 1,005 ms（RMS 6,162 / 最大 32,768）、スピーカーのヌル出力 53,077 サンプル
3,320 ms、Wi-Fi 接続（起動から 12.1 秒、登録 2 件を FAT から NVS へ移行）、
`STK_VOLUP` で 15 → 20%、ステータスバーの CRC 一致、NVS への保存（元は `fat`）。

### 14-1. 会話が TLS で必ず失敗した

```
E esp-tls-mbedtls: mbedtls_ssl_setup returned -0x008D
E esp-tls: create_ssl_handle failed
```

`-0x008D` = `-141`。一次情報をたどると
`components/mbedtls/mbedtls/include/mbedtls/ssl.h` の
`MBEDTLS_ERR_SSL_ALLOC_FAILED` が `PSA_ERROR_INSUFFICIENT_MEMORY` で、
`tf-psa-crypto/include/psa/crypto_values.h` でその値が `-141`。
つまり **`mbedtls_ssl_setup()` がバッファを確保できなかった** —
内蔵 RAM の不足。`status` の `heap_free` は PSRAM 込みなので気づけなかった。

直した内容:

| | 前 | 後 | 根拠 |
|---|---|---|---|
| `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` | 未設定 | **`y`** | 現行 CircuitPython 版の `esp-idf-config/sdkconfig-psram.defaults` と同じ。`port/esp_mem.c` がこの設定のとき `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` を呼ぶ = SSL のバッファが PSRAM へ行く |
| `CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN` | 4096 | **2048** | 同上 (`sdkconfig.defaults`) |
| `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` | 16 | **4** | `sdkconfig-esp32s3.defaults`。静的 RX は **DMA が使える内蔵 RAM** から取る |
| `CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM` | 32 | **16** | 同上 |
| `CONFIG_ESP_WIFI_RX_BA_WIN` | 16 | **6** | 静的 RX の 2 倍を超えられない (`esp_wifi/src/wifi_init.c:56` が `#error` で止める)。6 は現行版と同じ値 |
| `CONFIG_ESP_WIFI_NVS_ENABLED` | `y` | **未設定** | 認証情報は登録簿で持っている (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`)。ドライバに二重に持たせない |

**見えるようにしたもの**: `status` に `heap_internal` / `heap_internal_min` /
`heap_internal_largest` / `heap_dma` を足し、起動おわりと通信のたびに
ログへも出す。次に同じことが起きたら `log.tail` だけで分かる。

### 14-2. Wi-Fi が up の間 BLE が繋がらなかった

`blex = {started:true, ready:true, adv:true, conn:0, disc:0}` のまま 160 秒。
`ble.refresh` も効かない。段階 1〜2 の像 (Wi-Fi 無し) では起動 1 秒で繋がる。

直した内容:

1. **`adv` を自分の旗で答えるのをやめた。** `stackee_ble.c` の
   `s_advertising` は `ble_gap_adv_start()` が成功したときに立てるだけで、
   そのあと NimBLE 側で止まっても下りない。**「出ているつもりで黙ったまま」**
   になる。`stackee_ble_tick()` は毎秒 `ble_gap_adv_active()` に聞き、
   止まっていれば撒き直す。`status` の `blex.adv` も NimBLE の申告を返す。
   撒いた回数 `adv_starts` / 失敗 `adv_fails` / **撒き直した回数
   `adv_revived`** を数えるので、起きていたかどうかが数字で残る。
2. **省電力を明示した。** `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)`。
   `esp_wifi.h` の既定と同じ値だが、既定に頼らない。`WIFI_PS_NONE` だと
   STA が電波を離さず、BLE のアドバタイズが時間を取れない。
3. **共存は BLE 優先に。** `esp_coex_preference_set(ESP_COEX_PREFER_BT)`。
   この機械の本業はキーボード。繋がらない BLE より遅い会話のほうがよい。
4. **内蔵 RAM を空けた** (14-1 の Wi-Fi バッファ)。BLE のコントローラも
   同じ内蔵 RAM を使う。
5. **切り分けの口を足した**: `wifi.off` で無線を止め、`wifi.on` で自動接続を
   再開する。`check_phase3.py --ble-without-wifi` が
   「Wi-Fi を切ると BLE が繋がるか」を人手なしで見る。

★ どれが効いたかは実機でしか分からない。`--ble-without-wifi` と
`blex.adv_revived` が、次の 1 回で切り分けられるようにしてある。

### 14-3. `wifi.list` の「パスワード漏れ」は**検査側の誤報**だった

`check_phase3.py` が `'password' in json.dumps(...)` と書いていて、
**`has_password` に当たっていた**。実機の応答は最初から
`{"ssid","channel","has_password"}` だけ。

それでも「漏れていない」を人の目に頼らないよう、外へ出す形を決めるところを
`stackee_wifi_public_json()` 1 か所に切り出し、`test_cfg_host.py` が
**現行 `stackee_wifi_store.public()` と 1 文字ずつ照合**し、かつ
「`"password"` という鍵が無いこと」「パスワードの文字列が現れないこと」を
直接確かめるようにした。検査側は `'"password"' in ...`（鍵として）と
「`password` という鍵があるか」の 2 通りで見る。

---

## 15. macOS の GATT キャッシュ (2026-09-16 に判明した BLE の真因)

### 15-1. 何が起きていたか

ボンドは CircuitPython 版から引き継げていて、**暗号化まで通る**。なのに

* Mac が自分から繋ぎ直さない (`blex.conn` が 0 のまま)
* `ble.refresh` も効かない
* Wi-Fi を切っても直らない (`check_phase3.py --ble-without-wifi` で確認)

Mac から `bleak` で明示的に繋ぐと**成功する**。そのとき列挙されたサービスが
`adaf0001`（Adafruit BLE のもの）・1813・180f・180a — つまり

> **macOS が CircuitPython 版の GATT の並びを覚えたままで、こちらの esp_hid の
> GATT を読み直していない。**

HID のホストは覚えている**古い属性ハンドル**に繋ごうとするので、
自動接続も打鍵も成立しない。段階 1〜2 で「接続・暗号化 OK」と記録したのも
同じ状態だった可能性が高く、**BLE 越しの打鍵が Mac に届いたことは
まだ一度も確かめられていない。**

★ 段階 1 の README にある「ボンドの引き継ぎ = 再ペアリング不要」は
**半分しか正しくなかった**。鍵は引き継げるが、GATT の並びは引き継げない。

### 15-2. 直し方 (仕様どおり)

**GATT Service Changed の indication** を送って「並びが変わった」と伝える。
相手はキャッシュを捨てて読み直す。

ただの `ble_svc_gatt_changed()` では足りない。あれは
`ble_gatts_chr_updated()` を呼ぶだけで (`nimble/host/services/gatt/src/
ble_svc_gatt.c`)、**相手が Service Changed を購読している記録 (CCCD)** が
無ければ何も送らない。相手が覚えているのは CircuitPython 版のハンドルなので、
こちらのハンドルでの購読記録は当然無い。そこで二段にしてある:

1. `ble_store_write_cccd()` で、その相手の **こちらのハンドル**に対する
   CCCD の記録を「indication 購読中」で書く。
   → **次に繋いだときは NimBLE の普通の道で送られる**
   (NimBLE は接続時に記録を RAM へ写すので、今回ぶんには効かない)
2. 今回ぶんは `ble_gatts_indicate_custom()` で**直接**送る。
   `nimble/host/src/ble_gattc.c` の実装は購読を見ないので必ず出る。

送るのは暗号化が済んだ時点 (`BLE_GAP_EVENT_ENC_CHANGE` の status 0)。
範囲は `0x0001`〜`0xFFFF` = 「全部読み直せ」。

数字は `status` の `blex` に出る:

| | |
|---|---|
| `svc_changed_handle` | Service Changed 特性のハンドル。**0 なら GATT に入っていない** |
| `svc_changed_sent` | 送った回数 |
| `svc_changed_acked` | 相手が受け取ったと返した回数 (`BLE_GAP_EVENT_NOTIFY_TX` の `BLE_HS_EDONE`) |
| `svc_changed_rc` | 直近の送信結果 (0 = 成功) |

Service Changed 特性そのものは `esp_hid` が入れている
(`components/esp_hid/src/nimble_hidd.c` の `nimble_hid_start_gatts()` が
`ble_svc_gatt_init()` を呼ぶ)。ハンドルは `ble_gatts_find_chr(0x1801, 0x2A05)`
で引く (`CONFIG_BT_NIMBLE_GATT_CACHING` が無効なので
`ble_svc_gatt_changed_handle()` は使えない)。

手で送る口も足してある:

```
python3 firmware/kmk/tools/stackee_console_client.py  # から
# {"cmd":"ble.svc_changed"}
```

### 15-3. 確かめ方 (人手ゼロ)

```
python3 firmware/tools/check_ble.py
python3 firmware/tools/check_ble.py --json
python3 firmware/tools/check_ble.py --no-connect
```

Mac 側の `bleak` を使う。見るのは 4 つ:

| 項目 | 合否 |
|---|---|
| スキャン | "stackee" が見える。広告に HID (0x1812) が入っている |
| GATT キャッシュ | 繋いで列挙したサービスに **`adaf` で始まるものが無い** (= 読み直された)。HID の 1812 は CoreBluetooth が隠すので見えなくてよい |
| 自動接続 | bleak を切ったあと、**Mac が自分から繋ぎ直す** (`status.ble`) |
| 打鍵 | `key.inject` で `sent_ble` が増える |

★ 最初に `hid.set {"dest":"BLE"}` で送信先を BLE に固定する。USB のままだと
`sent_ble` が増えず、この検査に意味が無いため。`hid.switch` はトグルなので
「必ず BLE に戻す」が書けない。だから名指しの `hid.set` を足してある。

### 15-4. それでも直らないときの最終手段 (★ ユーザーの操作が要る)

Service Changed を送っても macOS がキャッシュを捨てないことがある
(OS 側の判断で、ボンド済みの機器のキャッシュは強く保持される)。そのときは

> **Mac のシステム設定 → Bluetooth → stackee → 削除 → もう一度ペアリング**

しか無い。**これはユーザーの手を借りる唯一の項目**なので、提出時に明記する。
本体側は何もしなくてよい (ボンドを消したいときだけ `ble.clear_bonds`)。

---

## 16. ボンド済みの相手へ名指しで撒く (directed advertising)

### 16-1. 何が残っていたか

§15 の Service Changed で **GATT のキャッシュは入れ替わった**
(2026-09-16 の実機: bleak の列挙から `adaf*` が消え、
`svc_changed_sent 1 / acked 2 / rc 0`)。それでも

> **Mac が自分から繋ぎ直さない。** 切断後 92 秒待っても `conn` は
> bleak の 1 回のまま。スキャンには見えている (RSSI -48、広告に 0x1812)。

つまり「見えていないから繋がらない」のではなく、**Mac がこちらへ繋ぎに来る
気になっていない**。

### 16-2. 直し方

Bluetooth の仕様には、まさにこのための撒き方がある。

> **ADV_DIRECT_IND (directed advertising)** = 「このアドレスのあなたに
> 繋ぎに来てほしい」という**名指し**の広告。

段取りは ZMK や QMK の BLE ドライバと同じ:

| 段 | 撒き方 | 長さ | 誰に見えるか |
|---|---|---|---|
| 1 | `ADV_DIRECT_IND_HD` (3.75 ms 間隔) | **1.28 秒** (コントローラが自分で止める) | 名指しの相手だけ |
| 2 | `ADV_DIRECT_IND_LD` (30〜50 ms) | 30 秒 | 名指しの相手だけ |
| 3 | `ADV_IND` (今までの撒き方) | ずっと | 誰でも |

NimBLE では `ble_gap_adv_params` の `conn_mode = BLE_GAP_CONN_MODE_DIR` と
`high_duty_cycle` で選び、`ble_gap_adv_start()` に相手のアドレスを渡す
(`nimble/host/src/ble_gap.c` の `ble_gap_adv_type()` が
`ADV_DIRECT_IND_HD` / `_LD` に振り分ける)。相手は
`ble_store_util_bonded_peers()` で取る。

★ **1.28 秒切れは `BLE_GAP_EVENT_ADV_COMPLETE` (reason 0) で来る。**
`ble_gap.c:3287` が「slave role (HD directed advertising)」として
`ble_gap_adv_finished(0, 0, 0, 0)` を呼ぶ。接続失敗
(`BLE_ERR_DIR_ADV_TMO`) としては来ない。両方受けてあるが、普段通るのは
前者。イベントを取りこぼしても止まったままにならないよう、
`stackee_ble_tick()` に 2 秒の締め切りも置いてある。

★ **名指しで撒いている間は誰からも見つけられない** (その撒き方には広告
データを載せられない)。だから 31 秒で切り上げて `ADV_IND` に戻す。
戻さないと新しいホストとペアリングできなくなる。

★ やり直すのは **起動 / 切断 / `ble.refresh`** のときだけ。60 秒ごとに
繰り返したりはしない (そのたびに 31 秒見つけられなくなるため)。

### 16-3. 広告の中身を現行 CircuitPython 版と同じにした

一次情報 (`adafruit_ble/advertising/standard.py` の
`ProvideServicesAdvertisement.__init__` と `adafruit_ble/__init__.py` の
`BLERadio.start_advertising`) を読むと、現行版が撒いていたのは:

| | 中身 |
|---|---|
| 広告 | flags (`general_discovery | le_only` = 0x06) + 16bit サービス UUID `0x1812` |
| スキャン応答 | complete name + tx power |

**appearance も名前も広告には入っていない。** こちらは 5 つ全部を広告に
詰めていたので、同じ形に直した (名前と tx power はスキャン応答へ移動、
appearance は広告から落とす — GAP サービスの Appearance 特性が本来の出所で、
そちらは esp_hid が持っている)。

★ これは**合わせただけ**で、直ったという測定ではない。Mac が覚えている形
との差を 1 つ減らす、という意味しかない。

### 16-4. 見える数字

`status` の `blex` に増えた分:

| | |
|---|---|
| `adv_kind` | いまの撒き方 (`directed_high` / `directed_low` / `undirected` / `off`) |
| `adv_directed` | 名指しで撒いた回数 |
| `bond_peer` | 名指しの相手 (ボンド済み) が居るか |

```
python3 firmware/tools/check_ble.py
```

の「Mac が自分から繋ぎ直すか」は、**`blex.conn` が bleak の接続より増えたか**
で見る (`status.ble` だけだと bleak の接続を数え込む)。名指しの広告は
1.28 秒 + 30 秒で終わるので、待つのは 30 秒あれば足りる。

---

## 17. 段階 4 (周辺機能と本番構成)

### 17-1. タッチパッド → マウス

移植元は `firmware/kmk/stackee_touch.py`。判定そのものは
`main/stackee_touch_core.c` にあり、**ESP-IDF に一切依存していない**ので
Mac 上でそのまま走る (`tools/test_touch_host.py` が現行版と突き合わせる)。

```
FT6336 @0x38 (内部 I2C)
  ↓ 5 ms 周期で 16 バイト読む         ← touch タスク (CPU0 / 優先度 3)
stackee_touch_core   なぞり / タップ / スクロールの判定
  ↓
送信キュー (Report ID 2)            ← QMK のマウスキーと同じ経路
  ↓
hid_out タスク → USB か BLE
```

★ **符号は 2 つ別々にかかる**。移植元の実機でポインタがちょうど 180 度
ずれた原因がこれ。

| 何の補正か | どこで効くか |
|---|---|
| 画面の回転ぶん (rotation = 270) | `rotate_delta`。ポインタにもスクロールにも効く |
| パネル自体の向き (180 度) | `pointer_invert_x/y`。**ポインタを送る直前だけ**。スクロールには効かない |

実測で決まっている値 (動かさない):

| 値 | 意味 |
|---|---|
| `tap_min_time = 10 ms` | これ未満は FT6336 の幽霊タッチ。本物のタップは 11〜49 ms |
| `tap_time = 200 ms` | これを超えたら長押し (クリックにしない) |
| `tap_max_move = 30` | 開始点からの実移動距離。累積ではない (累積だと指の揺れで本物が落ちる) |
| `click_hold_ms = 120` | 押す → 120 ms → 離す。BLE で 5/5 届いた値 |
| `right_click_zone = 0.3` | 画面の右 30% でタップすると右クリック |

★ 向きの効き方 (実機と一致): rotation 270 なので画面 X = 生 y、さらに
`pointer_invert_x` で 239 - y。つまり **生 y = 220 は画面の左、生 y = 30 は右**。

人手ゼロで確かめる:

```
python3 tools/console_hid.py touch.status
python3 tools/console_hid.py touch.inject 'xy=[80,60,92,69,104,78]' step_ms=5
```

`touch.inject` は **I2C に 1 度も触らない**。時計を進めたことにして判定だけを
流すので、タップ判定の境目 (10 / 200 ms) を 1 ms 単位で狙える。
指も要らないし、Mac の画面も動かない (レポートは送信キューまで)。

### 17-2. カメラ

移植元は `firmware/kmk/stackee_camera.py`。ESP-IDF 側は
`espressif/esp32-camera == 2.1.7` (`main/idf_component.yml` で版固定)。

★ **2.1.7 でなければならない理由**が 2 つある。

1. `driver/sccb-ng.c` (新しい `i2c_master` ドライバ版) を持っていて、
   **既に開いている I2C ポートに相乗りできる** (`sccb_i2c_port` +
   `pin_sccb_sda = -1`)。内部 I2C には AXP2101・ES7210・AW88298・FT6336 が
   ぶら下がっているので、二重に開かせられない。これより古い版は legacy の
   `driver/i2c.h` を使い、こちらの `i2c_master` と同じポートを取り合って壊れる。
2. ESP-IDF 6.0 の依存 (`esp_driver_gpio` / `esp_driver_i2c` / `esp_driver_ledc`)
   を `CMakeLists.txt` で明示している。

撮る手順と、実測で決まっている値:

| 段 | 中身 | 実測 |
|---|---|---|
| power | ALDO3 (AXP2101 REG 0x90 bit2) を入れて待つ | **1,000 ms**。50 ms では素子が起きず、2 枚目以降が必ず失敗する |
| init | `esp_camera_init` + 向き (GC0308 0x14) を書く | 向きは**絶対値を 1 回書く**。読んで直すと 1 手ぶん古い値を掴む |
| warm | 捨て駒を撮る | **30 枚**。1 枚では色が決まらない。60 / 120 枚に増やしても、撮るたびのばらつき (±0.005) より小さい差しか無く時間だけ倍 |
| take | 本番の 1 枚 (RGB565) | — |
| jpeg | `frame2jpg` で software 圧縮 | GC0308 に JPEG エンコーダは無い |
| stop | `esp_camera_deinit` + **ALDO3 を切る** | 切らずにハードリセットすると GC0308 が自走したままブートループになる |

★ **XCLK は出さない** (`pin_xclk = -1`)。CoreS3 の XCLK は GPIO2 で、そこは
PORT.A の SDA = TCA8418 (キーマトリクス)。掴むとキーボードが死ぬ。
現行 CircuitPython 版も `external_clock_pin` を渡していない。

★★ **PSRAM DMA で開く。これが段階 4 でいちばん手こずったところ。**

既定 (PSRAM DMA 無効) では LCD_CAM の DMA が**内蔵 RAM の連続 30,720 バイト**を
要求する。実機はそれが取れず、必ず失敗した:

```
E cam_hal: cam_dma_config(524): DMA buffer 30720 Byte malloc failed,
           the current largest free block:4096 Byte
```

実機の内蔵 RAM (Wi-Fi と BLE が上がったあと、2026-09-16 実測):

| 数字 | 値 |
|---|---|
| 内蔵 RAM の空き合計 | 12,615 B |
| いちばん大きい空き塊 | 7,680 B |
| DMA に使える塊 | 4,827 B |

**空き合計ではなく「塊の大きさ」で決まる。** PSRAM DMA にすると DMA は
PSRAM のフレームバッファへ直接書き、内蔵に要るのは**記述子 40 個 = 480 B**
だけになる (`cam_hal.c` の `cam_dma_config` は `psram_mode` のとき内蔵の
`dma_buffer` をそもそも確保しない)。

`cam_set_psram_mode()` は esp32-camera の**私的ヘッダ**にあるが、シンボルは
外に出ているので宣言だけして使っている。開けなかったときは内蔵 DMA で
やり直す。そのときのために `CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=8192`
(= 内蔵の要求 7,680 B) にしてある。

あわせて内蔵 RAM を **約 20 KB 空けた**:

| 何 | 前 | 後 |
|---|---|---|
| 起動ログのリングバッファ | 内蔵 16 KB | **PSRAM 16 KB** + 内蔵の控え 2 KB |
| Raw HID コンソールの送信待ち | 内蔵 6 KB | 内蔵 3 KB |
| console の大きい応答バッファ | 1,600 B × 3 か所 | 1,600 B × 1 |

★ 起動ログを PSRAM に置いてよい理由: ここは esp_log の出口で、書式文字列は
フラッシュにある。つまり**キャッシュを止めた状態では元々呼べない**
(呼べば書式文字列の読み出しで落ちる)。PSRAM を触っても新しい危険は増えない。

★ 撮影中は **ui を止める** (DESIGN.md §3)。画面はそのまま残り、撮り終われば
次の周で描き直される。キー入力は止まらない (別タスク・別 CPU)。

★ 使う素子は GC0308 だけなので、他のドライバは `sdkconfig.defaults` で
外してある (像が 38 KB 小さくなった)。

人手ゼロで確かめる:

```
python3 tools/check_phase4.py --only camera --save-jpeg /tmp/shot.jpg
```

撮って `camera.dump` で取り出し、**Mac 側で JPEG として妥当か** (SOI / EOI /
SOF の寸法) を確かめる。ALDO3 が落ちていることも見る。

**未実装**: 撮った JPEG を Wi-Fi で送るところ。`stackee_http` のワーカーは
1 本しかなく会話が使っているのと、pi400 側の受け口の取り決めが無いため、
`STACKEE_CAMERA_PATH` (既定 `/image`) を読むところまでで止めてある。

### 17-3. Raw HID の上のコンソール

full プロファイルには CDC が無い。コンソールとログは VIA の Raw HID に
相乗りさせる。QMK の `via.c` が知らない command id を `via_command_kb()` に
回す仕組みを乗っ取っている (`main/stackee_conhid.c`)。

```
byte 0     command id (0xC0 送信 / 0xC1 受信 / 0xC2 情報)
byte 1     len   (payload の有効バイト数、0..29)
byte 2     flags (bit0 = まだ続きがある)
byte 3..31 payload (29 バイト)
```

★ **流れるバイト列は CDC のときとまったく同じ**。だからホスト側の切り分け器
(`stackee_console_client.py` の `FrameParser`、`docs/js/protocol.js` の
`Demux`) を 1 文字も変えずに使い回せる。

★ `via_command_kb()` が呼ばれるのは**入力タスク** (CPU1・最高優先度)。
ここでコマンドを処理すると `camera.capture` のような数秒かかるものが
キーボードを止める。だから受信を環状バッファに溜めるだけにして、
実際の処理はメインループ (`stackee_console_poll`) がやる。

★ **dev プロファイルでも有効**。CDC と Raw HID の両方へ同じバイト列を流す
ので、`console_hid.py` と操作盤は dev / full のどちらでも同じように動く。

★★ **ログは「ホストが読んでいるとき」しか溜めない。**
実機で踏んだ (2026-09-16): ログを無条件に溜めると、Mac が Raw HID を 1 度も
読んでいない間に環状バッファが起動ログで満杯になる。そのあと `hello` を
投げても、応答は「入り切らない」で捨てられ、**コンソールが永久に黙る**
(実測 `tx_pending 6143 / tx_dropped 299`)。直し方:

| 何を書くか | 扱い |
|---|---|
| 応答 (`stackee_conhid_write`) | **必ず入れる**。場所が無ければ**古いほう**を押し出す |
| ログ (`stackee_conhid_write_log`) | ホストが 3 秒以内に 0xC1 を撃っていなければ**捨てる**。溢れたら**新しいほう**を捨てる |

古いほうを押し出してよいのは、ホスト側の切り分け器が次の 0x1E で必ず枠を
取り直すから。逆に新しい応答を捨てると、いま投げたコマンドの返事が来ない。

★★ **Raw HID の応答は送信先 (BLE / USB) の選択に関わらず必ず USB へ出す。**
段階 1〜3 の `stackee_hid_out.c` はここを送信先で振り分けていて、既定の BLE の
ままでは VIA の応答もコンソールの応答も 1 バイトも返らなかった (段階 4 で発見・
修正)。USB が刺さっていなければ**捨てて先へ進む** — ここで止めると、後ろに
並んでいるキーのレポートまで BLE へ出られなくなる。

### 17-4. UAC マイク (full プロファイルのみ)

16 kHz / モノラル / 16 bit。移植元は `firmware/cp-uac` (CircuitPython 10.3.0 の
`usb_audio`) と、そこで確かめた `boot.py` の
`usb_audio.enable(sample_rate=16000, channel_count=1)`。

記述子は TinyUSB が持っている `TUD_AUDIO20_MIC_ONE_CH_DESCRIPTOR` をそのまま
使う (CircuitPython 版が手書きしていたものと同じ構成)。

★ **会話が最優先**。マイクとスピーカーで BCK / WS を共有している (半二重) ので
両方は持てない。`STK_TALK` で録音中・返答の再生中は、UAC には**無音**を送る。
口を絶やすとホストが「マイクが壊れた」扱いにするため、止めずに 0 を流す。
何サンプル無音にしたかは `usb.status` の `uac.silence` に出る。

```
python3 tools/check_phase4.py --only uac            # Mac から見えているか
python3 tools/check_phase4.py --only uac --record   # 1 秒録ってサンプル数
```

`--record` には `sox` か `ffmpeg` が要る。無ければ列挙だけ。

### 17-5. FAT への書き込み (`settings.set` / `fs.put`)

★ 普段は**読み取り専用**でマウントしてある。壊れ方を減らすためで、それは
変えていない。書くときだけ `main/stackee_fat.c` が
「外す → 書ける形で付け直す → 書く → 読み取り専用に戻す」をやる。

★ 書き方は CircuitPython 本体と同じ。あちらの
`ports/espressif/supervisor/internal_flash.c` は
`esp_partition_erase_range` + `esp_partition_write` で
**ウェアレベリング無しの生のパーティション**へ書いている。ESP-IDF の
`esp_vfs_fat_spiflash_mount_rw_wl` は WL のヘッダを前提にするので
**絶対に使わない** (使うと CIRCUITPY の中身ごと壊れる)。
そのため FATFS の diskio を自前で持っている (4 KB のセクタを
「読む → 差し替える → 消す → 書く」)。

`settings.set` は書き換えてよいキーだけを通す:

| キー | 書ける | 値を返す |
|---|---|---|
| `STACKEE_HOST` / `STACKEE_PORT` | ○ | ○ |
| `STACKEE_TALK_URL` | ○ | ○ |
| `STACKEE_TALK_TOKEN` | ○ | **×** (設定済みかだけ) |
| `STACKEE_WIFI_PASSWORD` ほかパスワード類 | × | **×** |
| `CIRCUITPY_WIFI_SSID` | **×** | ○ |

★ `CIRCUITPY_WIFI_SSID` を書かせないのは、CircuitPython に戻したときに
supervisor が起動中に `connect()` を 4 回呼び、AP 不在の場所で起動が 19 秒
延びるため (`wifi_autoconnect_design.md` §1.1 の実測)。

`fs.put` は base64 で 360 バイトずつ送り、`final` で 1 回だけ書く。
溜め場は PSRAM に 256 KB。いちばん大きい素材 (`ack_04.pcmz` = 76 KB) も入る。

### 17-6. Web 操作盤 (docs)

`docs/js/hid.js` を足して、Web Serial と WebHID の両方で話せるようにした。
接続欄の「接続方法」で `自動` / `USB シリアル (dev)` / `USB HID (full)` を選ぶ。
プロトコル層 (`protocol.js`) は 1 行も変えていない。

★ `docs/` は GitHub Pages で配る操作盤で、**ファームのコード (GPL) は
1 行も入れていない**。hid.js が知っているのは「32 バイトのレポートの形」だけ。
2026-09-21 に `firmware/` を同じリポジトリへ移したあとも、この境目は同じ
(`firmware/` だけが GPL-2.0-or-later)。

### 17-7. 段階 4 でまだ確かめていないこと

実機での書き込みと動作、full の列挙 (UAC + HID + Raw HID) は 2026-09-16 に
確かめた (RESULTS.md「段階 4 の最終結果」)。残りは人の手が要るもの。

- カメラの絵の中身 (JPEG として妥当かは見るが、「ちゃんと写っているか」は人の目)
- タッチパッドの操作感 (レポートが出ることは測れるが、使い心地は普段使いで)
- `fs.put` で素材を差し替えたあと、その素材で正しく描けるか

## 18. 撮影のあとのリセットでフラッシュが読めなくなる (2026-09-16、直してある)

### 18-1. 何が起きたか

段階 4b の dev 像 (`68973c4d…`) で `camera.capture` のあと、Raw HID の
`bootloader` → ROM (`303a:0009`) までは行けたが、`flash.py --reboot` の
watchdog-reset で戻すと本体が **`invalid header: 0xffffff1f`** を延々出す
ブートループに落ちた。USB は `303a:1001` (USB-Serial/JTAG) で 1〜2 秒ごとに
再列挙され、pyusb のバスリセットも esptool の接続も効かない。

ROM をダウンロードモードで止めて (DTR/RTS) esptool スタブからフラッシュを
読むと **JEDEC ID が `ffff01`、RDSR が busy 固定** = フラッシュ素子が
まったく応答していない。`0xAB` (deep power-down 解除) / `0xFF` / `0x66 0x99`
(リセット) を送っても変わらない。

### 18-2. 原因

カメラの PCLK = **G45**、VSYNC = **G46** は ESP32-S3 の**ストラッピングピン**
(G45 = VDD_SPI の電圧選択、High で 1.8V。G46 = ダウンロードモードの条件)。
撮影のあと ALDO3 を切っても線は High のまま残り (内部 I2C のプルアップ経由で
素子が寄生給電されていると見ている。`firmware/kmk/tools/stackee_serial.py`
の 2026-09-11 の記録と同じ現象)、次のリセットで ROM が VDD_SPI を 1.8V に
してしまい、3.3V のフラッシュが読めなくなる。電源を入れ直すまで直らない。

`bootloader` (FORCE_DOWNLOAD_BOOT) で ROM に入る 1 回目はフラッシュを読まない
ので通り、そこから戻る 2 回目で露見した。

### 18-3. 復旧 (ボタン無し、実機で確認)

```
python3 tools/pmic_cycle.py /dev/cu.usbmodemXXXX     # 1001 のポート
```

DTR/RTS で ROM をダウンロードモードに止め、esptool スタブの `write_reg` /
`read_reg` で G11 (SCL) / G12 (SDA) をビットバンギングして AXP2101 (0x34) の
REG 0x10 bit1 (Restart = 全レール OFF → ON) を書く。書いた瞬間に応答が
消え、**1.0 秒**で `303a:811a` のアプリが起動した。`--check` を付けると
読むだけ (REG 0x03 = 0x4A で I2C が通っていることを見る)。bit0 (Soft PWROFF)
は絶対に書かない (復帰にボタンが要る)。

`tools/flash.py` の `leave_rom()` は、CDC も 811A も 30 秒戻らず ROM が
`1001` に居るときに、これを自動で呼ぶ。

### 18-4. 予防 (ファーム)

`stackee_camera_pin_strap_safe()`: G45 / G46 を出力 Low にして
`gpio_hold_en()` で固定する。hold は RTC 領域なのでソフトリセット・
ウォッチドッグを跨いで残り、ROM がストラップを読む時点で Low になっている。
呼ぶ場所は 3 つ。

| どこ | いつ |
|---|---|
| `shutdown_camera()` (撮影の後片付け) | 毎回。ALDO3 を切ったあと |
| `stackee_usb_request_rom_download()` | `bootloader` / `QK_BOOT` / 1200 bps タッチの直前 |
| `stackee_usb_request_restart()` | `reset` / `QK_REBOOT` の直前 |

次に撮るときは `release_strap_pins()` で hold を外してから esp32-camera に
渡す。

### 18-5. 確かめ方と結果

```
python3 tools/check_camera_reset.py --via bootloader   # 撮影 → bootloader → flash.py で戻す
python3 tools/check_camera_reset.py --via reset        # 撮影 → reset
python3 tools/check_camera_reset.py --transport hid --via bootloader   # full
```

撮影 (JPEG 1826 B、ALDO3=0) → リセット → 復帰 → `status.up` が小さいこと →
15 秒見張って二度目の再起動が無いこと、を通す。

| 像 | 経路 | 結果 |
|---|---|---|
| dev `e214ede5…` | bootloader | ROM 3.0 s → 復帰 5.3 s。OK |
| dev `5e10751e…` | reset ×2 | 消滅 1.7〜2.5 s → 復帰 2.4〜3.9 s。OK |
| dev `5e10751e…` | bootloader | ROM 2.5 s → 復帰 4.7 s。OK |
| full `4a127621…` | bootloader / reset | ROM 2.5 s → 復帰 (811A) 1.2 s / 消滅 1.7 s → 復帰 2.2 s。OK |

★ macOS は本体が消えてから 2 秒ほどポート名を残す。復帰の判定は
「消えた → 戻った」の順で見ないと、古いポート名を復帰と誤認する
(check_camera_reset.py の 1 回目はこれで「二度目の再起動」と誤判定した。
`status.rst` (esp_reset_reason) を足して確かめた)。

### 18-6. ついでに足したもの

- `status.rst`: esp_reset_reason の数字 (1=電源投入 3=ソフト 4=パニック 5/6=WDT 9=ブラウンアウト)
- `status.i2c`: 内部 I2C の失敗回数と、バスリセット後のやり直しで通った回数。
  撮影 → reset → 撮影で AXP2101 への書き込みが 1 回失敗したので、
  `stackee_board.c` の 1 往復を「失敗したら `i2c_master_bus_reset()` して
  1 回やり直す」にした (その後の 4 回で fail 0)
- `log.tail` の `back`: 末尾から何バイト手前で終わるか。リング 16 KB を
  古い順に全部読める (`check_camera_reset.py --log`)
- **コンソールの 1 行上限を 512 → 1024 に**: `fs.put` の 1 枠 (base64 360 B =
  480 文字 + 枠) が 512 に入り切らず、本体が黙って捨てていた (応答が無いので
  ホストは 30 秒待ちになる。`status.drops` が 1 増える)。full 像で
  `install_assets.sh --only manifest.json` を回して発覚。`fs_put.py` の
  塊も 300 B に縮めた
- `flash.py` は full (CDC 無し) でも動く: `enter_rom()` は CDC が無く
  811A が居れば Raw HID の `bootloader` を送り、復帰は 811A の列挙で見る

## 19. 電源ボタン長押し OFF と、AXP2101 設定書き込みの事故 (2026-09-17)

### 19-1. 事故

「長押しで電源が切れない」に対し、AXP2101 REG 0x22 (PWROFF_EN) bit1
(PWRON > OFFLEVEL で電源断を許す) を console から 0x02 に書いた。直後の
`bootloader` (esp_restart) で本体の電源が落ち、USB から消えた。起動後の
REG 0x21 (PWROFF status) = 0x01 = 「PWRON がオフレベルの間 Low」が原因
として記録されていた。ボタンは誰も触っていない。復旧はソフトでは不可能で
(電源投入要因は EFUSE 固定、VBUS 入れ直しも効かない)、電源ボタンを押して
もらうしかなかった。0x22 は電源断を跨いで残るので、起動後に 0x00 へ戻した。

この個体の AXP2101 は REG 0x20〜0x28 が全部 0 を返し、データシート
(research/stackee/axp2101_datasheet_swcharge_v1.0.txt) の既定値と合わない。
**電源 IC の設定レジスタはソフトから書かない** (`axp.write` は診断用に
残すが、設定系には使わない)。

### 19-2. 長押し OFF を作るなら

REG 0x49 bit2 (POWERON Long PRESS IRQ、RW1C) を UI の 10 秒周期とは別に
100 ms 周期で読み、立っていたら REG 0x10 bit0 (Soft PWROFF) で切る。
電源 IC の「長押しで切る」機能には頼らない。検証にはユーザーの長押しと、
起動の押下が最低 1 回ずつ要る。**押してもらう回数を先に示して了承を得て
から**書き込む。

### 19-3. 電池残量 (同日、無線で実測)

1 分ごとの `電池 %` ログで、無線運用中に 96% → 59% (3.77 V) まで下がった。
残量の数値は更新されている。VBAT の ADC (REG 0x30 bit0) を有効にし、
`status.bat_mv` と 1 分ごとのログを残した。

## 20. 時間が経つと HTTPS の証明書検証が失敗する (2026-09-17)

### 20-1. 事象 (実測)

- ログ: `esp-x509-crt-bundle: PSA signature verification failed with error 0xffffff6b (-149)` →
  `Certificate matched but signature verification failed` → `mbedtls_ssl_handshake returned -0x3000`。
  -149 = PSA_ERROR_INVALID_SIGNATURE。
- 起動直後は通る。起動から約 1.4 時間後に初めて失敗し、以後 `talk.inject` 5 回とも失敗。
  `reset` で再起動すると直後の会話は成功。再起動後 1 時間以内に再現はしていない。
- サーバ (Tailscale Funnel 経由、Let's Encrypt) のチェーンは
  leaf(P-256) ← YE2(P-384) ← ISRG Root YE(P-384) ← **ISRG Root X2 を X1 がクロス署名 (RSA-4096/SHA-256)**。
  Mac 上で本体と同じ束 (cacrt_all.pem) を CAfile にした openssl verify は OK。
  オフィスの DNS は Funnel の公開 IP を返し、TCP/TLS は届いている。つまり束もサーバも正しく、
  本体の中の署名計算 (ハッシュ or RSA) が壊れている。
- 失敗時も内蔵 RAM 空き 38 KB / 最大の塊 19 KB で、成功時と同じ。
- Mac が USB マイク (UAC) を開いて I2S が動いている最中でも、起動直後なら成功する
  (ffmpeg で 90 秒録音しながら `talk.inject` → 証明書検証 4 回 OK)。同時実行だけでは再現しない。

### 20-2. 前例 (一次情報)

- **espressif/esp-idf#18640** (2026-05、未解決): ESP32-S3 / IDF v6.0.1 / PSRAM で、I2S の GDMA と
  共存するとハードウェア SHA (esp_crypto_shared_gdma 経由) が誤ったダイジェストを返し、
  x509 チェーン検証で `psa_verify_hash()` が **-149** を返す。`CONFIG_MBEDTLS_HARDWARE_SHA=n` で消える。
  I2S チャネル削除時に GDMA のレジスタが残る (`gdma_reset` は FIFO と FSM しか戻さない) という
  Espressif の回答とパッチあり。本機は会話のたびに I2S を作って消す (半二重、`close_channels()`)。
  https://github.com/espressif/esp-idf/issues/18640
- v6.0.3 (2026-09-02) で初めて入った暗号関連の修正: `c41dd724d` (S3 でハードウェア MPI の
  キャッシュ Rinv の大きさ違いで署名検証が失敗)、`86f6192f1` (RSA-4096 の `esp_mpi_exp_mod()` 入力検証)、
  `1c351b465` (PSA の SHA ドライバがエラー経路で状態を残す)、`a1f1d9072` / `0d8ff68f8`
  (クロス署名検証の不具合とリーク)。v6.0.1 にはどれも無い。
- 「時間が経つと失敗に転じる」という報告そのものは見つからなかった。

### 20-3. 対処 (最終、2026-09-18)

独立評価 (Opus、私の仮説を渡さず実測だけを渡した) の結論は「ハードウェア暗号を全部切るのは
効いてはいるが原因の特定を飛ばした過剰な対処。v6.0.3 の c41dd724d (ESP32-S3 でハードウェア
MPI のキャッシュ Rinv の大きさ不一致による署名検証失敗の修正) が事実に最も合う」。それに沿って
抜本対応にした。

| 手 | 内容 | 実測 |
|---|---|---|
| ESP-IDF **v6.0.3** (upstream) | `firmware/esp-idf-v6.0.3/` (gitignore) を `build.sh` が既定で使う (§3)。c41dd724d / 86f6192f1 / 1c351b465 / a1f1d9072 / 0d8ff68f8 を含むことをソースで確認 | dev/full とも警告 0 でビルド、ホストテスト 318 件 OK |
| ハードウェア SHA / MPI / AES を**すべて戻す** | sdkconfig.defaults を元に戻した | `crypto.selftest` ok (hw_sha=1 hw_mpi=1)、会話の POST 受理 **5.4 秒** (ソフト化前と同じ) |
| **I2S の常設化** | 会話ごとに `i2s_del_channel` しない (#18640 の引き金を消す)。RX を主、TX を従の全二重で 1 回だけ作り、以後 enable/disable だけ。詳細は §22 | マイク rms 815〜840 / 再生 53077 サンプル、再生をまたいでも録音可 |
| **ロングポーリング** | pi400 の中継が `GET /jobs/<id>?wait=25` で最大 25 秒保持。本体は `?wait=25` で待つ | 会話 1 回の GET が 12 回 → **3 回** |
| 自己診断と自動復旧は残す | TLS 接続失敗時だけ `stackee_cryptocheck` が走り、NG なら会話の合間に再起動。定期には走らない | — |

像: full `5fbd7828…` (firmware/cp-uac/cmp/phase5-full-5fbd7828.bin)。
★ **いま本体に載っているのはその後の full `5a589186…`** (1,370,848 B、
返答音声 120 秒、2026-09-19 に書き込み。退避は
`firmware/cp-uac/cmp/phase6-full-5a589186.bin`、書き込み前の像の退避は
`firmware/cp-uac/cmp/ota0-backup-20260919-022806.bin`)。くわしくは
RESULTS.md の「段階 6 相当: 返答音声を 120 秒にする」。

### 20-4. 2 つ目の症状: ハンドシェイクは通るのに本文で切られる (2026-09-18)

- 11:53 (起動から 1.5 時間): `Certificate validated` のあと本文送信中に
  `esp-tls-mbedtls: write error :-0x0050` / `Connection reset by peer`。11:57 の `talk.inject` は
  `status=400` + `ESP_ERR_HTTP_INCOMPLETE_DATA`。どちらも **pi400 のプロキシにはログが無い**
  (HTTP まで届いていない = pi400 の tailscaled の TLS 層で切られている)。
- 同じ時刻に Mac から Funnel の公開 IP へ 200 KB の POST (通常速度・25 kB/s・8 kB/s・TLS 1.2) を
  撃つと全部プロキシまで届く。pi400 自身から Funnel 経由で 26 秒かけた遅いアップロードも届く。
  つまり Funnel もプロキシも正常で、**本体が送る TLS 本文だけが相手に受け付けられていない**。
- 本体を再起動すると直後の POST は受理される (11:59)。
- 前例: **esp-idf#18640 の Issue 1**: `i2s_del_channel()` のあと同じ GDMA チャネルを
  `esp_crypto_shared_gdma` が引き継ぎ、**ハードウェア AES-GCM が壊れた暗号文を出す**。報告文そのままで
  「HTTPS POST: サーバは正しい TLS レコード枠を受け取るが中身が壊れている → HTTP 400」。
  回避策 `CONFIG_MBEDTLS_HARDWARE_AES=n`。本機は会話のたびに I2S を作って消している。
- 対処: **`CONFIG_MBEDTLS_HARDWARE_AES=n`** も入れた (像 `3e091d05…`)。これで SHA / MPI / AES の
  ハードウェア経路をすべて外し、#18640 が挙げる 3 つの壊れ方 (SHA 誤答・AES 誤答・Wi-Fi の CCMP) のうち
  mbedtls 側の 2 つを避ける。根本対処は Espressif のパッチ
  (`0001-fix-i2s-reset-gdma-when-deleting-i2s-channel.patch`、I2S 削除時に GDMA をリセット) を IDF に当てること。未実施。
- 代償 (実測、Funnel 経由の 1 要求あたり): ハードウェアあり 5.2〜5.9 秒 → SHA/MPI ソフト 7.5 秒 →
  AES もソフト **8.6〜10 秒**。会話は POST 1 回 + GET のポーリング (10 秒間隔) なので、返答までが
  数十秒延びる。次の改善候補は接続の使い回し (keep-alive、いまは要求ごとに TLS を張り直している)。
- 参考: Tailscale 側にも同じ時刻に `Drop: TCP{ingress→peerapi} no rules matched` /
  `connsInFlightByClient ... not handled` が出るが、これは Funnel ingress の既知の別件
  (tailscale#18181 / #19290 / #21114) で、Mac からの POST が通っている以上、今回の切れ方の原因ではない。

### 20-5. まだ分かっていないこと

- 昨日 (v6.0.1) の -149 が SHA と MPI のどちらだったかは、当時 `crypto.selftest` が無く確定していない。
  v6.0.3 + ハードウェア暗号ありで「起動から 1.4 時間後」の再発が無いことは、時間をかけて普段使いの
  中で確かめるしかない。再発すれば `cryptocheck` の行 (SHA か x509 か) がログに残り、本体は自分で再起動する。
- #18640 の GDMA パッチ (I2S 削除時のリセット) は v6.0.3 にも入っていない。本機は削除しなくしたので影響しない。

## 21. 返答が来ない: サーバ (ubook) の GPU が外れて音声認識が時間切れ (2026-09-18)

- 本体 → Funnel → pi400 のプロキシ → ubook の `stackee_server.py` までは通っている
  (POST 202、`?wait=25` のロングポーリング GET も 200)。ジョブは `{"state":"error","error":"whisper-cli timed out"}` で終わる。
- pi400 から ubook へ 1 秒の合成 WAV を直接 POST しても同じく `whisper-cli timed out` → **本体は無関係**。
- ubook の `dmesg`: `NVRM: Xid (PCI:0000:02:00): 154, GPU recovery action changed ... (Node Reboot Required)`、
  `nv_pci_remove` — GTX 1650 がドライバから外れている (2026-09-17 19 時ごろ JST)。`vulkaninfo` は
  `llvmpipe` (CPU) しか見えず、whisper.cpp の Vulkan 版が CPU で走って時間切れになる。
  ubook の journal では 2026-09-18 03:03 UTC 以降の全ジョブが `whisper-cli timed out`。
- 対処: **ubook の再起動** (GPU の復旧に Node Reboot が要ると NVRM 自身が言っている)。ubook は
  ユーザーの作業機 (Android エミュレータ等が動作中) なので、こちらからは再起動しない。

## 22. I2S を常設にしたときにマイクが無音になった件 (2026-09-18、直してある)

常設化の 1〜3 版目は `audio.selftest` が samples=15872 (定刻) なのに rms=0 だった。IDF v6.0.3 の
ドライバのソースを読ませた結果 (Opus)、原因は全二重ではなく **MCLK の出し方**:

- `gpio_output_enable()` / `gpio_output_disable()` は GPIO マトリクスの出力信号を素の GPIO に
  付け替える (esp_driver_gpio `gpio.c:228` → `gpio_hal_matrix_out_default`)。信号を繋いだ直後に
  `gpio_output_enable()` を呼ぶと MCLK が剥がれ、ES7210 は一度もクロックされない。RX は主なので
  BCK/WS/DMA は定刻で回り、samples だけ正常に見える。
- 正しい切り替え: ON は `esp_rom_gpio_connect_out_signal(G0, I2S0_MCLK_OUT_IDX, false, false)` だけ
  (出力イネーブルは内部で立つ)。OFF は `gpio_set_level(G0, 0)` → `SIG_GPIO_OUT_IDX` へ付け替え。
- 全二重の従 (TX) に同じ bclk/ws ピンを渡しても IDF は入力側の接続を足すだけで RX の出力は壊さない
  (`i2s_common.c` `i2s_gpio_check_and_set`)。`I2S_GPIO_UNUSED` は「変えない」の意味。
- 運用: 録音 = MCLK ON → ES7210 設定 → `enable(rx)`。再生 = `enable(rx)` (主のクロック源) →
  `enable(tx)`、RX の入力は捨てる。停止 = `disable(tx)` → `disable(rx)`。`i2s_del_channel` は呼ばない。
  MCLK を OFF にしている間 ES7210 が設定を保持するかは未検証 (録音のたびに設定し直している)。


## 23. 返答音声の字幕 (2026-09-20 / 3 行化 2026-09-21)

喋っている間、画面のいちばん下 (y=250..319 の 70 px) に **3 行**まで字幕を出す。
顔は y=50..249 なので**領域が重ならない**。顔のアニメーションと字幕は
互いを描き直さない。

★ **2026-09-21 に 1 行 → 3 行にした。** 空きを作るために顔を
**上 29 px / 下 11 px** 切り詰めて 240x200 で出している。**元絵と `faces.bin`
は変えていない** (切り詰めるのは起動時に PSRAM へ展開したあとだけ)。
29/11 は 32 コマ全部の余白の最小値で、**1 画素も落ちない**。詳しくは §23-8。

### 23-1. 同期はサーバが決める

サーバは返答を**文ごとに合成して連結**しているので、各文の音声が何 ms から
始まるかを正確に知っている。区切り (ページ) と開始時刻はサーバが計算して
本体へ渡し、本体は「再生位置 (ms) ≥ 開始時刻」の**最後の**ページを出すだけ。
本体側に推定ロジックは 1 行も無い。

| やりとり | 中身 |
|---|---|
| `GET /jobs/<id>` の `done` | `"subtitles"` … **本文そのもの** (`\t` `\n` を JSON で逃がした 1 文字列)。これがあれば別 GET をしない<br>`"subtitles_url"` … `"/jobs/<id>/subtitles"` (旧い本体のために残る)。既存の項目は不変 |
| `GET /jobs/<id>/subtitles` | `200 text/plain; charset=utf-8`。本文は 1 行 `<start_ms>\t<text>\n` |

* `start_ms` は 10 進整数、単調非減少、先頭行は 0。
* `text` はタブ・改行を含まない UTF-8。1 ページの幅 ≤ **15 桁** (全角 1、半角 0.5)。
  **1 ページ = 帯の 1 行**。行の積み方 (3 行で頁めくり) は本体側の話で、
  サーバの契約は 1 バイトも変わっていない (§23-8)。
* 行数 ≤ 48 (`STACKEE_TALK_SUB_PAGES`)、本文 ≤ 4096 B (`STACKEE_TALK_SUB_BYTES`)。
  どちらの経路でも**同じ本文** — サーバは done の JSON 全体を 8,192 B 以下に
  収めるため末尾ページを落とすことがあるが、`/subtitles` はそのとき
  「落としたあとの本文」を返す (`public/server/stackee_server.py` の `job_body`)。
* **旧サーバ互換**: どちらも無ければ字幕なしで従来どおり動く
  (HTTP の往復も増えない)。取得や解析に失敗しても会話は止めない。

**本体の選び方** (`talk.status` の `sub_src` に出る):

| `sub_src` | いつ | 歩き方 |
|---|---|---|
| `inline` | done に `"subtitles"` があり、1 ページ以上採れた | `poll → audio → play_wait → playing` (**別 GET なし**) |
| `url` | 本文が無い / 1 ページも採れない。`subtitles_url` から採れた | `poll → subs → audio → …` |
| `none` | どちらでも採れなかった | `poll → audio → …` (字幕なしで鳴る) |

★ **なぜ本文を JSON に混ぜるのか。** 別 GET は実機で**約 8 秒**かかる
(要求ごとに TLS を張り直すため。§23-6 の実測)。本文は 4 KB 以下なので、
done の応答に載せてもらえば喋り始めがその 8 秒ぶん早い。

★ そのぶん返答待ちの GET の受け皿を **8192 → `8192 + 2 x 4096` = 16,384 B**
にした (`STACKEE_TALK_POLL_LIMIT`)。いまのサーバは done の JSON 全体を
8,192 B 以下に抑えているので 8 KB でも足りるが、x2 は「サーバが
`ensure_ascii=True` に変えても `\uXXXX` で 2 倍までしか膨らまない」ぶんの
余裕として取ってある (受け皿が足りないと会話ごと失敗するので、ここは
サーバの上限に依存させない)。この受け皿は通信側が 1 往復ごとに **PSRAM** へ取って
閉じるときに返すので、常駐の使用量も像の大きさも増えない。
**逃がしを解くための 4 KB の中継ぎも持たない** — 1 行ぶん (128 B、スタック) ずつ
解いて行解析へ渡すので、内蔵 RAM の静的な使用量は 1 バイトも増えない。

### 23-2. フォント (東雲 16px)

`firmware/assets/font16.bin` — 半角 8x16 (158 字) + 全角 16x16 (6,879 字)、
合計 **7,037 字 / 236,770 B (231 KB)**、sha256
`01ba663af13538b285db6fc7461c4dc96d0b7febca10981755833ddff6d46b4e`。

* 素材は**東雲フォント** (`shnmk16.bdf` / `shnm8x16r.bdf`)。ライセンスは
  **Public Domain** (/efont/ — The Electronic Font Open Laboratory 2001)。
* 作るのは `tools/gen_font16.py`。BDF 本体はリポジトリに入れていない
  (未追跡の `research/stackee/fonts/shinonome/`)。置き場は `--wide` / `--narrow` で渡す。
  JIS X 0208 → Unicode の変換は表を手で書かず Python の codec (euc_jp) に任せる。
* FAT (`/stackee_assets/font16.bin`) に置き、起動時に **PSRAM へ丸ごと** 読む
  (顔 `faces.bin` と同じ流儀)。**像 (ota_0) は 231 KB 増えない**。
* 字形が無い字は **〓 (U+3013)** で代替する。font16.bin が無い・壊れている
  ときは帯だけ出して字は出さない (画面も会話も止まらない)。

```
python3 firmware/tools/gen_font16.py          # 作り直す
python3 firmware/tools/gen_font16.py --check  # 生成物が最新か
firmware/tools/install_assets.sh --only font16.bin
```

★ `tools/fs_put.py` の `MAX_BYTES` は 65,536 のままで、231 KB の font16.bin を
**送る前に断っていた** (2026-09-20 に判明)。本体側の受け皿 (`stackee_console.c` の
`FSPUT_MAX`) は 256 KB あるので、道具の側を 256 KB に揃えた。
実測: **転送 50 秒 / フラッシュ消去 255 回** (1 ファイルにつき FAT を 1 回だけ
読み書きで付け直す)。`faces.bin` (34 KB) より桁が 1 つ大きいので、
**作り直したときだけ**送ること。

### 23-3. 性能の約束と実測 (実機、full `b59358de…`、2026-09-20)

★ この節の数字は **1 行 (240x30) のときの実測**。3 行 (240x70) にしたあとの
実測は §23-8。約束も 2 ms → **6 ms** に置き直してある (根拠は §23-8)。

| 約束 | 実測 |
|---|---|
| 帯 1 回の**描画そのもの**は **≤ 2 ms** (3 行にしたあとは ≤ 6 ms。§23-8) | 空 **164 us** / 半角混在 442 us / 全角 15 桁 **631 us** / はみ出し 16 桁 714 us。240x30 = 7,200 画素 (14.4 KB) の塗りと、多くても 15 字の点打ちだけ |
| 再生中に **malloc しない** | ページは `stackee_talk_t` の中の固定配列 (48 x 64 B)。字形は font16.bin の中を指すポインタのまま使う |
| 字形を引くのは **O(log n)** | Unicode 昇順の表を二分探索。7,037 字で 13 回の比較 (`main/stackee_font16.c`) |
| 打鍵の道に描画が混ざらない | 帯を描くのは **ui タスクだけ**。会話の状態機械は文字列を置くだけ。打鍵の遅延 (字幕中) **中央値 1.543 ms / 最大 1.705 ms** (合否 6 / 10 ms) |

#### `perf.ui_sub` の最大値を合否にしてはいけない (2026-09-20 に測って分かったこと)

最初は `perf.ui_sub` の **max** を 2 ms と突き合わせていて、実機で
**中央値 613 us / 最大 2188 us** で NG になった。調べた結果、**描画は遅くない**。

`ui.subtitle` を処理するのは **main タスク (優先度 1)** — CPU0 でいちばん低い。
`esp_timer` の実時間で測っているので、途中で割り込まれたぶんがそのまま乗る:

| CPU0 のタスク | 優先度 |
|---|---|
| Wi-Fi | 23 |
| usbd / audio | 5 |
| http | 4 |
| **ui (普段はここが帯を描く)** | **3** |
| camera / net | 2 |
| **main (= console。`ui.subtitle` はここ)** / LCD の SPI ワーカー | **1** |

同じ絵を 40 回ずつ描いて条件を 1 つずつ外した実測:

| 条件 | 最小 | 中央値 | p90 | 最大 | >2 ms |
|---|---|---|---|---|---|
| 普段 (顔 自動 / Wi-Fi up) | 631 us | 821 us | 2151 us | **3014 us** | 4/40 |
| 顔を止める | 641 us | 836 us | 1273 us | 1837 us | 0/40 |
| 顔を止めて Wi-Fi off | 643 us | 798 us | 1021 us | 1465 us | 0/40 |

* **最小値はどの条件でも 631〜643 us で動かない** = 描画そのものの費用。
* 尾を伸ばしているのは顔のアニメーション (1 コマ = 240 行 = 115 KB の SPI 転送。
  ワーカーは優先度 1 で main と**同格**なのでラウンドロビンで時間を取り合う) と
  Wi-Fi (優先度 23 で素通しに割り込む)。呼ぶ間隔を 0/50/150/400 ms と変えても
  尾は消えないので、直前の転送待ちではない。
* 普段の道は **ui タスク (優先度 3)**。SPI ワーカーには割り込まれない。
  つまり `ui.subtitle` の実時間は**悲観側にずれた代理値**で、これを合否にすると
  描画ではなく「そのとき何に割り込まれたか」を測ることになる。
* 打鍵には 1 ミリ秒も響いていない (input は **CPU1 の最高優先度**で、画面の
  ことを何も知らない)。字幕中の実測は中央値 1.543 ms / 最大 1.705 ms。

そこで `check_phase2.py` は **1 ケース 6 回描いて最小値**(= 割り込まれなかった
標本) を 2 ms と突き合わせ、中央値と最大値は参考値として出す。
比較のため、同じ ui タスクが描くステータスバーは**中央値 1,812 us /
最大 75,118 us**、LCD 転送は最大 259,436 us。帯はこの中でいちばん軽い。

### 23-4. 実機に触らない確認

`tools/test_subtitle_host.py` (33 件) が、**本体の実体そのもの**
(`main/stackee_font16.c` / `stackee_draw.c` / `stackee_crc32.c`) を Mac 用に
ASan + UBSan つきでビルドし、本物の `assets/font16.bin` を通して描いた帯の
CRC32 を `tools/subtitle_expected.py` の期待値と突き合わせる
(1 行・2 行・3 行・3 行 15 桁・頁めくり直後・4 行目は捨てる・半角混在・
字形なし〓・空・半角カタカナの 13 通り)。行の積み方 (1 行目の位置が行数で動かない・
i 行目の上端 = `250 + 2 + i*22 + 3`・帯の上下の余りが 2 px ずつ) もここで見る。
`tools/test_talk_host.py` の `SubtitleTest` (15 件) が行解析・ページ選択・
上限・失敗時の挙動・旧サーバ互換を、`InlineSubtitleTest` (13 件) が
done の JSON に混ざってきた本文 (別 GET を飛ばす / 壊れていたら url へ落ちる /
どちらも無ければ字幕なしで鳴る / 受け皿が 16 KB になっていること) を見る。
`tools/test_cfg_host.py` の `JsonBigTest` (7 件) が、4 KB 級の JSON 文字列と
`\t` `\n` の逃がしを **返答文の 256 バイトの道とは別の道**で読めることを見る。
`AckLinesTest` (6 件) が、一次回答の行 (`assets/manifest.json` の
`acks[].lines`) を**本物のサーバを import して** 5 文すべてで突き合わせる
(§23-9)。

### 23-5. 実機での確認 (読むだけ)

`check_phase2.py` に 3 つ増えている。**書き込みも再起動もしない。**

* **字幕フォント** — `ui.assets` の `font16_len` / `font16_crc` が Mac の
  `assets/font16.bin` と同じか
* **字幕の帯** — `ui.subtitle` が返す帯 (3 行) の CRC32 が
  `subtitle_expected.py` と同じか
* **打鍵の遅延 (字幕中)** — `ui.subtitle` を挟みながら `key.inject` を撃ち、
  中央値 6 ms / 最大 10 ms を維持しているか

```
python3 firmware/tools/check_phase2.py --transport hid
python3 firmware/tools/subtitle_expected.py こんにちは   # 期待値を手で見る
```

実測 (full `b59358de…`、2026-09-20):

```
[OK] 字幕フォント     font16.bin 236770 B crc=853512126 (期待 236770 B / 853512126) / 半角 158 + 全角 6879 字
[OK] 字幕の帯       7/7 一致 (font16 True)
[OK] 字幕の桁数      15 桁 = 240 px
[OK] 帯の描画そのもの   最悪 714 us (はみ出し16桁) / 7 ケース x 6 回 (合否 2000 us)
[OK] 打鍵の遅延 (字幕中) 中央値 1.543 ms / 最大 1.705 ms / 7 回
```

### 23-6. 実機の会話では字幕が出なかった — 中継 (pi400) が `/subtitles` を通していない

2026-09-20、`audio.null` を立てて (無音) `talk.inject` で 1 往復したときの実測:

| | |
|---|---|
| 返答 | 「よかったです。また何かあれば、声をかけてください。」70,496 サンプル = **4,406 ms** |
| 通った状態 | `upload → poll_wait → poll → **subs** → audio → play_wait → playing → idle` |
| `talk.status` | `sub_pages=0` / `sub_page=-1` / `sub_bytes=0` / **`subs_failed=1`** |
| ログ | `[talk-subtitles] {"pages":0,"status":404,"got":1}` |
| 会話 | `errors=0`、`complete_ms=38524`。**最後まで鳴った** (字幕の失敗は会話を止めない) |
| 帯 | `ui.status` の `sub_paints` が 1 つも増えない (描いていない) |

原因は本体でもサーバでもなく **中継 (`dev/server/proxy.py`) の白名簿**。GET は
`/jobs/<32 桁 16 進>(/audio)?` しか通さないので、`/jobs/<id>/subtitles` は
上流 (ubook) へ届く前に中継が 404 を返していた。サーバ側は `done` の JSON に
`subtitles_url` を付けており (本体が `subs` 状態へ入っているのがその証拠)、
本体もその契約どおりに動いている。

* 直した (`dev/server/proxy.py` の白名簿に `/subtitles` を追加、`test_proxy.py` 13 件)。
  **配り直すまで字幕は出ない** — `dev/server/README.md` の手順で pi400 へ転送して
  `systemctl --user restart stackee-proxy.service`。
* **字幕の取得は 1 往復ぶんではなく約 8 秒かかる。** `subs` 状態が 16.3 秒 →
  24.3 秒 (200 ms 刻みのポーリングで観測)。本文は 4 KB 以下なのに遅いのは、
  `keep_alive_enable = false` で要求ごとに TLS を張り直すため (§20 と同じ)。
  そのぶん**喋り始めが約 8 秒遅くなる**。4 KB なら `done` の JSON に混ぜてしまう
  ほうが安い (ポーリングの応答は元から 8192 B まで読んでいる) — 契約の見直しは
  サーバ側と揃えて行うこと。
* 再生位置そのものは正確だった。`audio.status` の `pos` は 4.1 秒のあいだ
  ホストの実時間に対して**ずれが 5 ms 以内**で進む (差の 38〜40 ms は
  コンソールの往復ぶん)。ページ選択が使う時計は信用できる。

★ この 8 秒があるので、契約に **`done` の JSON に本文そのものを混ぜる**道を
足した (§23-1)。`inline` で通れば `subs` 状態を通らないので、中継の白名簿も
TLS の張り直しも関係なくなる。`subtitles_url` の道は旧サーバのために残してある。

### 23-7. `inline` の実測と、字幕が切り替わる位置 (実機、full `cea28e10…`、2026-09-20)

無音 (`audio.null`) で `talk.inject` を 2 往復。**書き込みも再起動もしていない。**

| | 1 往復目 | 2 往復目 |
|---|---|---|
| 返答 | 「了解です。また何かあれば、声をかけてください。」 | 「はい、待っています。準備ができたら声をかけてください。」 |
| 音声 | 70,154 サンプル = **4,384 ms** | 79,712 サンプル = **4,982 ms** |
| `sub_src` | **`inline`** | **`inline`** |
| ページ数 | 3 (`sub_bytes` 84 / `dropped` 0) | 4 |
| 状態 | `upload → poll_wait → poll → **audio** → play_wait → playing → idle` | 同じ |
| HTTP の往復 | **3 回** (POST / 状態 / `/audio`) | 3 回 |

**`subs` 状態を通っていない**のが眼目。ログも
`[talk-subtitles] {"src":"inline","pages":3,"bytes":84,"dropped":0}`、
`[talk-turn-timing] {...,"sub_pages":3,"sub_src":"inline"}`。

#### 別 GET の 8 秒が消えた

`reply_ready_ms → audio_ready_ms` (返答が出てから音声を受け取り終えるまで):

| | 内訳 | 合計 |
|---|---|---|
| 別 GET のころ (§23-6) | `subs` **8,034 ms** + `/audio` 9,848 ms | **17,838 ms** |
| `inline` 1 往復目 | `/audio` のみ 12,513 ms | **12,524 ms** |
| `inline` 2 往復目 | `/audio` のみ 11,892 ms | **11,907 ms** |

`/audio` (140〜160 KB) 自体が 9.8〜12.5 秒と回ごとにばらつくので、差を引き算で
語るより **「1 往復まるごと (約 8 秒) 無くなった」** と読むのが正しい
(HTTP の往復が 4 → 3)。

#### ページが切り替わった再生位置

再生中、`talk.status` (`sub_page`) と `audio.status` (`pos`) を続けて読み、
ページが変わった時点を前後の読みで挟んだ (幅は往復 2 回ぶん = 130〜166 ms)。
切り替わったところで帯の CRC32 (`lcd.crc y=290 h=30`) も取ってある。

| 往復 | ページ | 切り替わった位置 [ms] | 帯の CRC32 | 出ていた文字 (CRC から特定) |
|---|---|---|---|---|
| 1 | 0 → 1 | 1,302 〜 1,432 | 526477896 | 「また何かあれば、」 |
| 1 | 1 → 2 | 2,692 〜 2,827 | 660979874 | 「声をかけてください。」 |
| 2 | 0 → 1 | 695 〜 861 | 3230108024 | 「待っています。」 |
| 2 | 1 → 2 | 2,191 〜 2,331 | 3131895648 | 「準備ができたら声」 |
| 2 | 2 → 3 | 3,606 〜 3,756 | 1090296636 | 「をかけてください。」 |

★ **出ていた文字は推測ではない。** 帯の CRC32 が
`tools/subtitle_expected.py` で同じ `font16.bin` から描いた期待値と
**1 ビットも違わない**。`ui.status` の `sub_len` (21 / 24 / 27 / 30 B) も
その文字のバイト数と一致する。1 文を 15 桁で割るとき**均等に割る**
(17 字 → 8 + 9) というサーバの決まりも、ここに出ている。

★ 挟み幅 (130〜166 ms) は**こちらのポーリングの往復時間**であって本体の
遅れではない。本体側は audio タスクが 1 周ごとにページを選び、ui タスクが
5 ms 周期で帯を描くので、判定から描画までは 1 桁 ms のはず (HID 越しには測れない)。

★ **サーバの `start_ms` との差はまだ出していない。** ジョブ ID を本体も
`talk.status` も出さず、ubook への ssh が使えないため。ジョブは 5 分で消えるので、
測るなら「本体を回しながら同時に ubook 側で `job-timing` の ID を拾って
`/jobs/<id>/subtitles` を取る」必要がある。
なお、サーバの決まり (文の境目は PCM の長さから厳密、文の中は字数で比例配分、
文の間に 150 ms の無音) と実測の音声長から逆算すると、1 往復目の
「1 → 2」の予測は **2,672 〜 2,744 ms** で、観測した窓 (2,692 〜 2,827) と
重なる。**ただしこれは逆算であって照合ではない。**

### 23-8. 字幕を 3 行にした — 顔を上 29 / 下 11 px 切り詰める (2026-09-21)

ユーザーの決定: **「切り詰めは欠けない範囲だけにする。上 29 / 下 11。
字幕は 3 行」。**

★ **経緯**: 同じ日に一度 **上下 33 px 対称・字幕 4 行**で入れた
(コミット `f2c6688`、実機にも載せた)。設計時の見積もりが `thinking` の絵
だけを見たもので、`awake` の上 29 px と `camera` の下 11 px を数え落として
おり、**5 コマ 856 px が欠けた** (とくに `camera/a_00` は下 22 行 614 px)。
数えて分かったので**欠けない範囲に切り直した**のがこの 29 / 11。

#### 画面の割り付け

| | 元 (1 行) | 33/33 の版 (4 行) | **いま (3 行)** |
|---|---|---|---|
| ステータスバー | y=0..49 | y=0..49 | y=0..49 |
| 顔 | y=50..289 (240x240) | y=50..223 (240x174) | **y=50..249 (240x200)** |
| 字幕の帯 | y=290..319 (30 px / 1 行) | y=224..319 (96 px / 4 行) | **y=250..319 (70 px / 3 行)** |

1 行は **22 px** (上の余白 3 + 字形 16 + 下の余白 3)。3 行で 66 px、
余りの 4 px は帯の上下へ **2 px ずつ**。i 行目の字形の上端は
`250 + 2 + i*22 + 3`。左端は 0、1 行 15 桁、黒地に白文字、消すときは白
(どれも 1 行のときと同じ)。

#### 切り詰めで落ちた画素は **0** (実測、`assets/faces.bin` 全 32 コマ)

上 29 行 + 下 11 行に、地の色 (濃さ 15 = 白) でない画素は **1 つも無い**。

| | 上 | 下 |
|---|---|---|
| 落ちた非背景画素 (32 コマ合計) | **0 px** | **0 px** |
| 32 コマの余白の最小値 | **29 px** (`awake/a_00` `a_01`) | **11 px** (`camera/a_00`) |

**29 / 11 は最小値そのもの** — 上を 30 にすれば `awake` が、下を 12 にすれば
`camera` が欠ける。**1 画素も落とさずに取れる最大の切り詰めがこの値**で、
字幕に回せるのは 40 px = 3 行が限界 (4 行なら 88 px 要る)。

数え方はホストテストで固定してある
(`tools/test_render_host.py` の `test_the_trim_loses_nothing` と
`test_the_trim_is_as_tight_as_it_can_be`)。**素材を差し替えたらここが落ちる**
ので、そのとき数え直して切り直すこと。

参考: 33/33 で欠けていた分 (この版では全部残る)。

| コマ | 表情 | 33/33 で落ちていた |
|---|---|---|
| 0 / 1 | `awake/a_00` `a_01` | 上 70 px ずつ (y=29..32、右上の印のてっぺん) |
| 13 / 14 | `thinking/a_00` `a_01` | 下 51 px ずつ (y=207..209) |
| 31 | `camera/a_00` | 下 **614 px** (y=207..228、カメラの絵の下 22 行) |

#### 元絵と `faces.bin` は変えていない

切り詰めるのは**起動時に PSRAM へ展開したあと**だけ。

* `faces.bin` は 240x240x32 のまま。`stackee_assets_read_inflate` で
  921,600 B に展開して**丈を確かめてから**、その場で各コマの上 29 行・
  下 11 行を落として 240x200 に詰め直す (768,000 B)。余った 153,600 B は
  `heap_caps_realloc` で PSRAM へ返す。行は前へしか動かないので、
  入れ物は 1 つで足りる (`main/stackee_ui.c` の `trim_faces`)。
* `changes.bin` も変えていない。差分の bbox は元の 240x240 の座標なので、
  描くときに 29 行ぶん上へ寄せ、捨てた行にかかる部分を落とす
  (`paint_face_rect`)。
* `tools/render_expected.py` が**同じ切り詰め**をしてから期待値を組み立てる。
  `ui.assets` の `faces_len` / `faces_crc` も切り詰めたあとの値
  (素材そのものの値は `render_expected.py --json` の `assets.sheet_len` /
  `sheet_crc`)。

#### 行の積み方 (頁めくり)

サーバの行 (ページ) は 1 行ずつそのまま使う。**サーバ側は変えていない。**

* 再生位置がページ `i` の `start_ms` を越えたら、帯の `i % 3` 行目にその行を置く。
* `i % 3 == 0` のとき帯を空にしてから置く = **3 行が埋まった次のページで頁がめくれる**。
* 再生終了・失敗・中断で帯を消す (従来どおり)。

★ **本体は「いま何行出しているか」を覚えていない。** 表示すべき行の集合は
ページ番号だけで決まる (`stackee_talk_band`: `i - i%3` から `i` まで)。
だから `ui.subtitle` に同じ文字列を投げれば会話中とまったく同じ絵になり、
CRC32 の照合がそのまま使える。

API は `stackee_ui_set_subtitle(const char *utf8)` のまま。中身が
**改行区切りで最大 3 行**になっただけ (4 行目以降は捨てる)。
`ui.subtitle` の `text` にも `\n` (JSON の逃がし) で渡せる。

#### 内蔵 RAM

`ui.sub_want` / `ui.sub_shown` が 64 B → 192 B (3 行 x 63 B + 改行 2 + NUL)。
`.dram0.bss` は **74,920 → 75,176 B (+256 B)**、`.dram0.data` は変わらず。
帯のバッファは持たない (フレームバッファに直接描く)。

#### 帯の描画時間 — 約束を 2 ms → 6 ms に置き直した

**線形に伸ばした見積もりでは足りない。** フレームバッファが **PSRAM** にあり
(`stackee_lcd.c` は `MALLOC_CAP_SPIRAM` で取る)、**データキャッシュが 32 KB**
(`CONFIG_ESP32S3_DATA_CACHE_32KB`) だから:

* 1 行の帯 240x30 = **14.4 KB** はキャッシュに丸ごと載る。同じ帯を続けて
  描き直すと PSRAM まで行かないので 164 us で済んでいた。
* 3 行の帯 240x70 = **33.6 KB** は載らない。描き直すたびに全面が PSRAM との
  往復になる (書き込みは read-allocate なので、バスを通るのは約 67 KB)。

キャッシュに載るか載らないかの境目がちょうどこの辺りにある。実測
(40 回の**最小値** = 割り込まれなかった標本):

| 帯 | 面積 | 空 | 1 行 15 桁 | いちばん重い中身 |
|---|---|---|---|---|
| 1 行 240x30 | 14.4 KB | 164 us | 631 us | 714 us (16 桁) |
| **3 行 240x70** (`13ee4c9`) | 33.6 KB | **994 us** | **1,893 us** | **3,431 us** (3 行 x 15 桁) |
| 4 行 240x96 (`f2c6688`) | 46 KB | 3,185 us | 4,061 us | 6,188 us (4 行 x 15 桁) |

1 バイトあたりに直すと 11.4 → 30.6 → 69 ns。**面積には比例していない**
(14.4 KB は丸ごと載る / 33.6 KB は大半が載る / 46 KB は載らない)。

3 行の実測をもう少し細かく (`13ee4c9`、Wi-Fi up、40 回):

| 帯の中身 | 最小 | 中央値 | p90 | 最大 |
|---|---|---|---|---|
| 空 | 994 us | 1,324 | 1,870 | 2,383 |
| 1 行 15 桁 | 1,893 us | 2,297 | 2,780 | 2,861 |
| 3 行 (全角 25 字) | 2,010 us | 2,380 | 2,725 | 3,850 |
| **3 行 x 15 桁 (全角 45 字)** | **3,431 us** | 3,889 | 4,315 | 4,536 |

顔を自動 (アニメーション中) にしても最小値は動かない (空 1,029 us /
3 行 1,912 us)。**描画そのものの費用**で、割り込みではない。
合否は **6,000 us** (いちばん重い中身の 1.75 倍の余裕) に置き、
その中身 (3 行 x 15 桁) を検査のケースに足した (12 → 13 ケース)。

★ **これで困らない理由。** 帯を描くのは **ui タスク (優先度 3)** で、しかも
**ページが変わった時だけ** (1 往復で 3〜4 回)。打鍵は CPU1 の最高優先度の
入力タスクが扱うので、字幕を描いている最中でも平常時と変わらない。

★ 減らしたいなら道はある (どれも今回は**やっていない**):
「変わった行だけ描き直す」(本体が行の状態を持つことになり、`ui.subtitle`
一発で同じ絵を作れなくなる = CRC 照合が使えなくなる)、
「帯を内蔵 RAM に持って転送時だけ写す」(33.6 KB の常駐が増える)。

#### 実機での確認 (`13ee4c9`、full、2026-09-21)

```
[OK] 素材の展開       顔 768000 B crc=3105609344 (期待 3105609344、240x200 に切り詰めたあと)
[OK] 32 表情 (y=50 h=200)  32/32 一致 (802 ms)
[OK] ステータスバー     6/6 一致
[OK] 字幕の帯        13/13 一致 (font16 True)
[OK] 字幕の桁数       15 桁 = 240 px / 帯は 3 行 x 22 px
[OK] 帯の描画そのもの    最悪 3643 us (3行15桁) / 13 ケース x 6 回 (合否 6000 us)
[OK] 打鍵の遅延 (平常)   中央値 0.885 ms / 最大 1.811 ms / 12 回
[OK] 打鍵の遅延 (描画中)  中央値 1.091 ms / 最大 1.542 ms / 8 回
[OK] 打鍵の遅延 (字幕中)  中央値 0.809 ms / 最大 1.724 ms / 13 回
```

無音 (`audio.null`) の往復 4 回。帯の CRC32 はどれも
`subtitle_expected.py` の期待値と一致した (下の「本文」は推測ではなく
**CRC が合った文字列**)。**頁めくりも会話の中で 2 回出た。**

| 往復 | 頁 | 行 | 帯の CRC32 | 本文 | `sub_len` |
|---|---|---|---|---|---|
| 1 (7,968 ms) | 0 | 1 | 1990837405 | 「はい、」 | 9 B |
| | 1 | 2 | 2642012413 | +「了解です。」 | 25 B |
| | 2 | **3** | 1139261196 | +「今日は台風が近づく予報なので、」 | 71 B |
| | 3 | **1** | 2579804529 | **頁めくり** 「外出するときは気を」 | 27 B |
| | 4 | 2 | 2903512770 | +「つけてくださいね。」 | 55 B |
| 3 (4,747 ms) | 2 | **3** | 2572506729 | 「はい、」+「了解です。」+「また必要なときに」 | 50 B |
| | 3 | **1** | 4043736196 | **頁めくり** 「呼んでください。」 | 24 B |

`sub_len` の増え方 (9 → 25 → 71 で 3 行、次が 27 に落ちて 55) が
「積む → 3 行で空にして 1 行目から」とそのまま一致する。

加えて、6 ページぶんの帯の文字列をホストビルドの `stackee_talk_band` に
出させ、そのまま実機の `ui.subtitle` へ投げて **6/6 一致**:

```
頁 0 → 1 行 'ぺ0'                crc  572322984
頁 2 → 3 行 'ぺ0\nぺ1\nぺ2'       crc 4170352358
頁 3 → 1 行 'ぺ3'                crc 2034769597   ← 頁めくり
頁 5 → 3 行 'ぺ3\nぺ4\nぺ5'       crc 3174104647
```


### 23-9. 一次回答にも字幕を出す / 帯はいつでも黒 (2026-09-21)

ユーザーの決定 2 つ。**本体側だけ。サーバ・操作盤・顔の絵は触っていない。**

#### (a) 帯はいつでも黒

字幕が無いときも `y=250..319` は黒のまま、文字だけ消える。
「消すときは白」(画面の地の色に戻す) をやめた。**起動直後の 1 枚目から黒。**

* `main/stackee_draw.c` の `stackee_draw_subtitle` が空でも `STACKEE_SUB_BG`
  (0x000000) で塗る。
* `stackee_ui_start` が最初の 1 枚を出すときに `paint_subtitle("")` を挟む。
* Mac 側 (`render_expected.py` / `subtitle_expected.py`) とホストビルド
  (`hostbuild/render_main.c`) も同じにしてあるので、**全面の CRC32
  (`lcd.crc` の `all`) が帯まで含めて一致する**。顔とバーの CRC は領域が
  重ならないので 1 ビットも変わらない。

#### (b) 一次回答 (ack) の再生中も字幕を出す

一次回答 (`ack_01..05.pcmz`) は**サーバを通らない** — 録音を送っている間、
素材に入っている音声をその場で鳴らしている。だから返答のように
`<start_ms>\t<text>` が降ってこない。そこで **素材を作るときに行へ割って**
`assets/manifest.json` の `acks[].lines` に入れておく。

```
"acks":[{"file":"ack_01.pcmz","samples":53077,...,
         "text":"わかったのだ。少し待っていてほしいのだ。",
         "lines":["わかったのだ。","少し待っていてほしいのだ。"]}, ...]
```

| ファイル | 文 | 行 |
|---|---|---|
| `ack_01.pcmz` | わかったのだ。少し待っていてほしいのだ。 | 「わかったのだ。」 / 「少し待っていてほしいのだ。」 |
| `ack_02.pcmz` | 了解なのだ。ちょっと考えるのだ。 | 「了解なのだ。」 / 「ちょっと考えるのだ。」 |
| `ack_03.pcmz` | 聞こえたのだ。今から確認するのだ。 | 「聞こえたのだ。」 / 「今から確認するのだ。」 |
| `ack_04.pcmz` | 任せてほしいのだ。少し待っていてね。 | 「任せてほしいのだ。」 / 「少し待っていてね。」 |
| `ack_05.pcmz` | うん、考えてみるのだ。 | 「うん、」 / 「考えてみるのだ。」 |

**割り方はサーバの規則そのもの。** `tools/ack_lines.py` が
`public/server/stackee_server.py` の `page_width` / `opens_badly` /
`split_columns` / `clause_spans` / `subtitle_pages` の写しで、
`tools/import_faces.py` が manifest を書くときに `text` から引き直す。
写しなのでずれうる — `tools/test_subtitle_host.py` の `AckLinesTest` が
**本物のサーバを import して** 5 文すべてで突き合わせる (ずれたら落ちる)。
**サーバのコードは 1 行も変えていない。**

★ **行は全部 manifest に持たせ、切るのは本体の仕事。** 帯は 3 行なので、
本体 (`main/stackee_audio.c` の `ack_lines()`) は**先頭 3 行だけ**を使う。
いまの 5 文はどれも 2 行なので切られないが、文を差し替えて 4 行以上に
なったときは 4 行目以降が出ない (素材側では捨てない)。

**本体の歩き方**

* 起動時に `manifest.json` の `acks[].lines` を読み、改行で繋いだ 1 本の
  文字列にして **PSRAM** へ置く (内蔵 RAM は増やさない。増えたのは
  ポインタ 5 本ぶんだけ)。
* 一次回答を鳴らし始めるとき (`ops_ack_begin`) にその文字列を
  `stackee_ui_set_subtitle()` へ渡す。
* 鳴り終わったとき (`play_step` の終わり) に消す。**そのあと返答の字幕が
  来る流れは今までどおり** — 返答の帯は会話の状態機械が持っていて、
  一次回答のぶんには触らない (`play_sub` が立っているときだけ消す)。
* `audio.play i=N` (検査用) でも同じ字幕が出る。番号を指定できるので、
  帯の CRC32 を `subtitle_expected.py` の期待値と突き合わせられる。
* **`lines` が無い旧い manifest でも落ちない。** 一次回答の字幕が出ない
  だけで、音も会話も画面も止まらない。

`audio.status` に 3 つ増えた: `ack` (鳴らしている一次回答の番号。ちがえば
`-1`)、`ack_sub` (その字幕を帯に出しているか)、`ack_lines` (字幕を持って
いる一次回答の本数)。

#### 実機での確認 (`e6075c4`、full、2026-09-21)

**待機中の帯** — `lcd.crc y=250 h=70` = **2186780939**。
`subtitle_expected.py` が描いた「黒 240x70・文字なし」と一致。

**一次回答 5 本を名指しで** (`audio.play i=N` → 帯の CRC32):

| i | ファイル | 本体の CRC32 | 期待値 | 行 |
|---|---|---|---|---|
| 0 | `ack_01.pcmz` | 1220270829 | 1220270829 | 「わかったのだ。」/「少し待っていてほしいのだ。」 |
| 1 | `ack_02.pcmz` | 3384805615 | 3384805615 | 「了解なのだ。」/「ちょっと考えるのだ。」 |
| 2 | `ack_03.pcmz` | 2343289232 | 2343289232 | 「聞こえたのだ。」/「今から確認するのだ。」 |
| 3 | `ack_04.pcmz` | 584589839 | 584589839 | 「任せてほしいのだ。」/「少し待っていてね。」 |
| 4 | `ack_05.pcmz` | 3500596394 | 3500596394 | 「うん、」/「考えてみるのだ。」 |

**5/5 一致**。どれも鳴り終わったあと帯は黒 (2186780939) に戻る。

**無音 (`audio.null`) の往復 3 回** — 200 ms ごとに `audio.status` /
`talk.status` / `ui.status` / `lcd.crc y=250 h=70` を読んだ:

```
往復 1  返答「うん、出かけるときは傘を忘れずにね。」
  t=0.148  upload   ack=3  ack_sub=true   len=55  crc= 584589839  ← 一次回答 3 の行
  t=6.235  upload   ack=-1 ack_sub=false  len=0   crc=2186780939  ← 黒 (鳴り終わった)
  t=23.50  playing  page=0                len=9   crc=3132235215  ← 返答「うん、」
  t=24.29  playing  page=1                len=55  crc=2534176427  ← +「出かけるときは傘を忘れずにね。」
  終わったあと                                    crc=2186780939  ← 黒
```

3 往復とも同じ歩き方 (一次回答の行 → 黒 → 返答の頁 → 黒)。**CRC32 は
どれも `subtitle_expected.py` の期待値と一致**(上の「返答」の 2 つは、
サーバと同じ規則で割った行から描いたもの)。

★ **manifest.json は FAT に送り直す必要がある。** 像を書いても素材は
変わらない。

```
firmware/tools/install_assets.sh --only manifest.json --transport hid
```

#### `install_assets.sh` の 2 つの欠陥 (同じ日に踏んで直した)

**1 回目の `--only manifest.json` は古い manifest を送っていた。**
`ack_lines` が 0 本のままで気づいた。

* **出どころが逆だった。** 開発元 (非公開) では現行 CircuitPython 版の
  素材 `firmware/kmk/stackee_assets` を**先に**見ていたので、
  `tools/import_faces.py` が書いた `firmware/assets/manifest.json`
  (5,584 B、`lines` つき) ではなく、古いほう (5,012 B) が実機へ行っていた。
  → **`firmware/assets` を先に見る**ようにした。道具が作って検査した
  バイト列と、実機へ送るバイト列を同じにする。非公開側にしか無いものは
  そちらから拾う (いまはそういうものは無い。`font16.bin` は公開側にしか無い)。
* **`--only` が「それだけ」ではなかった。** 既定の一式 (目録・差分表・
  アイコン・フォント) を送ったうえで、名指しのものを**もう一度**送っていた。
  同じファイルを 2 回書いてフラッシュを余分に消す。
  → `--only` は名指しの 1 つだけを送る。

実測: 直したあと `--only manifest.json --transport hid` は
**5,584 B / 401 ms / フラッシュ消去 1 回ぶん**で終わる
(直す前は 5 ファイル + 重複で 12 回ぶん)。

## 24. 会話が「通信に失敗しました」で 7 秒で終わる — 内蔵 RAM の枯渇 (2026-09-20)

ユーザーが 42 キーで話しかけたら、顔が「調査中」にならずに会話が終わった。
実体は **POST が TLS を書けずに失敗**していた:

```
http: POST 開始 (内蔵RAM 空き 16375 B / 最大の塊 7936 B / PSRAM 5529212 B)
esp-aes: Failed to allocate memory
esp-tls-mbedtls: write error :-0x0084
http: POST .../talk 失敗 (ERROR, status=0, 7450ms)
audio: [talk] 会話エラー: 通信に失敗しました
```

ハードウェア AES は **DMA 可能な内蔵 RAM** を要る。そこが痩せると握手も書き込みも
できない。`/talk` は無線に出る前に落ちるので、サーバ側には何も残らない。

### 24-1. 測ったこと

**PSRAM は減っていない。** ログの 5,529,212 B は POST の最中の値で、録音の
受け皿 960 KB を抱えている。待機中に読むと **6,504,304 B** で、字幕を入れる前
(6,505,512 B) と実質同じ。font16.bin の 237 KB は起動時の 1 回だけ。

**漏れてもいない。** `status` の `heap_internal` を待機中に読みながら、
`ui.subtitle` x40 / `lcd.crc` x20 / `status` x40 / `ui.selftest` / 無音の
`talk.inject` / `audio.selftest` (マイクを開く) を順に回しても、
**16,375 B から 1 バイトも動かなかった** (最大の塊も 7,936 B のまま)。
`wifi.off` で戻るのも 196 B だけ。

**常駐は +3,640 B だった。** `build/stackee.map` を字幕の前 (`5a589186` =
コミット `8093c3e`) と後 (`cea28e10`) で比べた:

| 置き場 | 前 | 後 | 差 |
|---|---|---|---|
| `.bss.a` (`stackee_audio.c`。`stackee_talk_t` を丸ごと抱えている) | 1,592 B | 5,048 B | **+3,456 B** |
| `.bss.ui` (`stackee_ui.c`。帯の文字列 2 本 + font16 の索引) | 2,984 B | 3,168 B | +184 B |
| 内蔵 RAM の静的合計 | 107,320 B | 110,960 B | **+3,640 B** |

`+3,456 B` は字幕のページ表 (`pages[48]` = 48 x 68 B)。**静的に増えたのは
3.6 KB で、実機で足りなくなった 12.7 KB (29,127 → 16,375) の一部でしかない。**
残りの約 9 KB はこの起動中に消費されて戻っていないぶんで、字幕とは別の話
(会話 2 回のあとに起きている。TLS / lwIP まわりの疑い。**未解明**)。

### 24-2. 直したこと

**会話の状態機械 (`stackee_talk_t`、約 4.8 KB) を PSRAM へ移した。**
触るのは audio タスクと console タスクだけで、**割り込みからは触らない**ので
PSRAM でよい。PSRAM が取れない機体では内蔵 RAM に落ちる (動きは同じ)。

| | 内蔵 RAM の静的合計 | `.bss.a` |
|---|---|---|
| 字幕の前 (`5a589186`) | 107,320 B | 1,592 B |
| 字幕のあと (`cea28e10`、失敗した像) | 110,960 B | 5,048 B |
| **いま** | **106,160 B** | **248 B** |

失敗した像より **4,800 B 軽く**、字幕を入れる前より **1,160 B 軽い**。
字幕がもらっている内蔵 RAM は `.bss.ui` の **184 B** だけになった
(帯の文字列 2 本は ui タスクが 5 ms ごとに読み比べる道にあるので、
ここだけは内蔵に残してある。差し引きは負なので約束は満たしている)。

**1 往復ごとの内蔵 RAM をログに残すようにした。** `[talk-http-timing]` に
`internal_free` / `internal_largest` を足したので、「1 回の会話でどれだけ
戻ってこないか」を `log.tail` から直接読める。24-1 の残り 9 KB を追うのは次の手。

### 24-3. 直した像での実測 (full `4544180c…`、2026-09-20)

**会話 5 往復で内蔵 RAM は 1 バイトも減らなかった。** 減るのは**最初の 1 回だけ**。

| | 内蔵 RAM 空き | 最大の塊 | 最小 |
|---|---|---|---|
| 起動 31 秒後 | **33,939 B** | **25,600 B** | 23,436 B |
| 1 往復目のあと | 33,403 B | 17,408 B | 23,436 B |
| 2 往復目のあと (ユーザーの実会話。返答 14.2 秒 / 455,722 B / 8 ページ) | 33,403 B | 17,408 B | 23,436 B |
| 3〜5 往復目のあと | **33,403 B** | **17,408 B** | 23,436 B |
| `ui.subtitle` x20 + `lcd.crc` x20 のあと | 33,403 B | 17,408 B | 23,436 B |

* 1 往復目だけ **空きが 536 B**、**最大の塊が 8,192 B** 減る。空きの減りが小さいのに
  最大の塊が大きく減るのは、小さな確保が**いちばん大きい空き領域の中に居座って
  割った**から。TLS が要るのは「大きな連続領域」なので、効くのはこちら。
* 2 往復目以降はどちらも動かない。**漏れではなく初回確保**。
  返答が 14.2 秒 (455 KB) の大きな往復でも 1 バイトも増えない。
* `heap_internal_min` は起動時の 23,436 B のまま = **会話の最中でも 23.4 KB を
  下回っていない**。失敗していた像は 7,128 B まで落ちていた。
* 5 往復とも `errors` 0 / `subs_ok` 5 / `subs_failed` 0、`sub_src` は毎回
  **`inline`** (HTTP は 1 往復 3 回 x 5 = 15 回、`failures` 0)。
  ユーザーの実会話 (無音ではない) も `sub_src":"inline"` で 8 ページ出ている。

### 24-4. まだ分かっていないこと

* **失敗した像で余分に消えていた約 12 KB の出どころ。** 上のとおり「会話 1 往復
  あたり」ではない (5 往復で 0)。あの起動では check_phase2 と、こちらの
  `wifi.off` / `wifi.on` を含む長い探りを回していたので、そのどれかの疑いが
  あるが**再現していない** (直した像は起動 690 秒 / 会話 5 回で横ばい)。
  普段使いで `[talk-http-timing]` の `internal_free` を見張り、最大の塊が
  20 KB を切ったらその直前に何をしたかを控えること。
* **`status` だけが答えないことがある** (README の「気づき」と同じ症状)。
  会話の直後に 60 秒待っても返らず、`hello` / `talk.status` / `ui.status` /
  `audio.status` / `axp.read` / `bench` は 0.1 秒で返る、という状態が数分続いた
  あと自然に戻った。`bench` の `main_max_us` が **4,427,755 us** (4.4 秒) なので、
  コンソールを持つ main タスク (CPU0 の優先度 1) が長く止められている。
  `status` は集める項目がいちばん多いので最初に現れる。**原因は未調査。**

---

## 25. アプリ内 OTA — 操作盤 (Web ページ) からファームを書き換える (2026-09-21)

本体が**動いたまま** Raw HID で像を受け取り、使っていないほうの区画
(`ota_1`) へ書き、`otadata` を切り替えて再起動する。ROM のダウンロード
モードには**一切入らない**。方式の比較は
`research/stackee/web_flash_2026-09-20.md`、設計は `DESIGN.md §6c`。

今回入れたのは同報告書 §6 の**段階 0〜2**。段階 3 (ロールバック) と
段階 4 (Wi-Fi 入口) は**まだ入っていない**。

### 25-1. 何が変わるか

| | いまの `tools/flash.py` (ROM 経由) | アプリ内 OTA |
|---|---|---|
| 書く先 | `ota_0` (いま動いている区画) | **`ota_1`** → otadata 切替 |
| 書き込み中のキーボード | **死ぬ (約 1 分)** | **生きている** (消去のたびに数十 ms 引っかかる) |
| 失敗したら | **ROM に取り残される = 文鎮化の入口** | 古い像のまま起動。無傷 |
| 途中で切れたら | 致命的 | 無害。最初から送り直せばよい |
| 要るもの | Mac + esptool + pyusb | ブラウザ (Chrome/Edge) か Node |

`tools/flash.py` は**復旧専用**として残す。像が起動しなくなったときの
最後の手段はこちら。

### 25-2. 使い方

**人 (操作盤のページ)**

1. 操作盤を開いて「接続する」(full プロファイル = USB HID)
2. 「ファームウェア更新」の節で「いま載っている版を見る」
3. 手元の `.bin` を選ぶ (`build-full/stackee.bin`)。または
   `docs/firmware/manifest.json` を置いてあれば「配布版」から選べる
4. **いま載っている版 / これから書く版 / sha256** を見比べてから「書き込む」
5. 進捗が出る。終わったら新しい `app.info` が出る

「書き終わったら新しい版で起動するように切り替える」のチェックを外すと、
`ota_1` に書くところまでで止まる (起動する側は切り替えない)。

**AI / コマンドライン (同じ中核 JS を通る)**

```
cd firmware/tools && npm install        # 初回だけ (node-hid)
node firmware/tools/ota.mjs --info                       # いま載っている版
node firmware/tools/ota.mjs --image firmware/build-full/stackee.bin --no-commit
node firmware/tools/ota.mjs --commit                     # 書いてある next へ切り替える
node firmware/tools/ota.mjs --image firmware/build-full/stackee.bin   # 書いて切り替えまで
node firmware/tools/ota.mjs --abort                      # 止まった転送を畳む
```

★ **`--commit` は像を渡さずに単体で使える (2026-09-21 に直した)。**
それまでは `--image` が無いと使い方が出るだけで、`--no-commit` で書き終えた
あと**像を送り直さないと切り替えられなかった** (1.4 MB = 約 60 秒の無駄)。
いまは `app.info` で `next` の名札を確かめてから `ota.commit` を投げ、
再起動を待って「動いている像が `next` と同じか」まで見る。断るのは
`next` が無いとき・`next` がいま動いているのと同じ像のとき (`--force` で通る)・
本体が `notready` を返したとき (= まだ書いていない。`--image` を渡すこと)。

★ `tools/ota.mjs` は操作盤とまったく同じ `docs/js/ota.js` を import して
いる。**人が押すボタンと AI が叩く道具が同じ筋道を通る**ので、書き込みの
手順が食い違いようがない。`node-hid` はネイティブ拡張なので、依存は
`firmware/tools/package.json` に分けてあり、操作盤の配信物 (`docs/`) には
Node の依存を 1 つも混ぜていない。

**配布版を置く**

```
firmware/tools/release_image.sh                 # build-full/stackee.bin
firmware/tools/release_image.sh --profile dev
```

`docs/firmware/stackee-full.bin` と `manifest.json` を書く。
**コミットはしない** (どの像をいつ配るかは人が決める)。

★ **行き先は毎回入れ替わる。** `app.info` の `next` が次に書く区画。
`--info` で確かめてから書くこと (25-8)。

### 25-3. 命令

どれもコンソール (Raw HID の 0xC0/0xC1、`tools/console_hid.py` や操作盤から)。

| 命令 | 返すもの |
|---|---|
| `app.info` | `running` / `boot` / `next` の区画名・`esp_app_desc_t.version`・`esp_partition_get_sha256`、`ota_state`、いまの転送の状態 |
| `ota.begin {size, sha256}` | 受け入れ可否、書き込み先、1 枠の本文の大きさ、credit、無通信で諦めるまでの時間 |
| `ota.status` | 受け取った / 書いたバイト数、環状バッファの残り、断った枠の数、経過 |
| `ota.end` | 数えた sha256 と申告の一致、`esp_ota_end`、**書いた区画から読み直した名札**。★ 切り替えない |
| `ota.commit` | `esp_ota_set_boot_partition` → 応答を返してから 500 ms 後に再起動 |
| `ota.abort` | `esp_ota_abort`。最初からやり直せる |
| `app.boot_factory` | otadata を消して factory (`uf2` = CircuitPython の UF2 ブートローダ) を選ぶ。**戻れる道**。実機では未確認 |

像そのものは JSON では運ばない。**専用のレポート種別 0xC3** で生バイトを
流す (既存の 0xC0/0xC1/0xC2 とは衝突しない)。枠の中身と credit の約束は
`DESIGN.md §6c`。

### 25-4. ★ sha256 は 2 種類ある

ここを混ぜると一生合わない。

| | 何 | どこで出る | 何のため |
|---|---|---|---|
| **ファイル全体** | `shasum -a 256 stackee.bin` | `ota.begin` の引数、`ota.end` の `sha256` | 転送で 1 バイトも化けていないか |
| **像の名札** | 像の**末尾 32 バイト** (`hash_appended`) | `esptool image_info`、`esp_partition_get_sha256()`、`app.info` の running/boot/next、`ota.end` の `partition_sha256`、`manifest.json` の `image_sha256` | **どの区画に何が入っているか** |

名札は「末尾 32 バイトを除いた部分の SHA-256」なので、ファイル全体の値とは
必ず違う。実測 (`build-full/stackee.bin`, 1,391,824 B、版 `5639a53`):

```
ファイル全体 3c4707fb1dd8ad63175d59f0ee6b479aa9741d93c493d2c482e897c3a2ceaf46
像の名札     f6ba59615656085122c443f44c782c73fdd70dfa9e95d5c5097391eaf51e02e0
```

`esp_partition_get_sha256()` は返す前に中身を検証する
(`bootloader_common.c`) ので、`ota.end` の `partition_sha256` が期待と一致
したら「ディスクの `.bin` とフラッシュの像が同じで、かつ ESP-IDF の検査も
通った」と端から端まで言い切れる。

### 25-5. 速さと、書き込み中のキーボード

| | 値 |
|---|---|
| 1 枠の本文 | 27 バイト (32 - 5。位置を毎枠に書くため) |
| 理論上限 | 27 B x 1 kHz = **27 KB/s** → 1.4 MB で **51 秒** |
| **実測 (Node、2026-09-21)** | **23.3〜25.0 KB/s、1,391,824 B を 54〜58 秒**。送り直し 0 回 |
| credit | 「書き終えた位置 + 32 KB」まで先行して送ってよい |
| 本体が返す間隔 | 受け取った累積が 1 KB を跨ぐたびに 1 枚 (実測 1,406 枚) |
| **転送中に入力タスクが止まる最長 (実測)** | **約 10 ms** (4 KB 書き込み + 1 セクタ消去)。`perf.input_loop` の最大値は転送の前後で 1 us も動かなかった |
| **再起動 (実測)** | USB の面が無いのは **約 1.2 秒**、指示から `app.info` が答えるまで **約 4.0 秒** |

実測の詳しい表と、そこで見つけた不具合は `RESULTS.md` の
「アプリ内 OTA」の節。0xC3 の本文枠は **51,550 枚 = 理論最小とちょうど同数**
(1 枚も無駄打ちしていない)。

★ **1 枠ごとの ack にしない。** フラッシュの消去・書き込み中はキャッシュが
止まり、IRAM 非常駐の割り込み (TinyUSB を含む) が数十 ms 止まる。1 枚ごとに
返事を待つ作りだと毎回そこでタイムアウトする。

★ **本体は自分からは何も言わない。** 応答を返すのは 0xC3 を受けたときだけ。
credit で送れなくなったホストは、本文 0 バイトの 0xC3 (「状態だけ返せ」) を
撃って `written` の伸びを聞きに行く。

### 25-6. 戻れる道

* **切り替えるまでは無傷。** `esp_ota_set_boot_partition()` を呼ぶまで
  otadata は 1 バイトも変わらない。転送中に電源が切れても古い像で起動する。
* **`ota.end` と `ota.commit` は分けてある。** 「書けた」と「そっちで起動
  する」は別の決断。`--no-commit` で「書いたが切り替えていない」状態に
  できる。
* **起動できない像はブートローダが自分で見捨てる。** otadata が指す区画の
  像が `bootloader_load_image()` に通らなければ、ESP-IDF は factory →
  もう一方の ota スロット、の順に**自動で落ちていく**
  (`bootloader_utility.c` の `bootloader_utility_load_boot_image()`、
  前向き・後ろ向きの 2 つのループ)。両方のスロットに通る像が入っている
  限り、**壊れた像を書いて切り替えても次の起動で生きているほうに戻る**。
  ★ ただしこれは「読み込めない像」だけ。**読み込めるが動かない像**
  (起動してすぐ固まる、USB を出さない) は救われない。それを救うのが
  段階 3 のロールバックで、まだ入れていない。
* **`tools/flash.py` (ROM 経由) は復旧専用**として残す。
  ただし下の 25-7 の注意を読むこと。

### 25-7. ★ `boot` が `uf2` だった件と、`flash.py` への影響 (2026-09-21)

OTA を入れる前の実機は `app.info` がこう答えていた:

```
running  ota_0    ver=5639a53  sha256=f6ba5961…
boot     uf2      ver=null     sha256=null      ← これ
next     ota_1    ver=null     sha256=null
ota_state unknown
```

**`running` と `boot` が食い違っている。** 意味を追うとこうなる。

**1. otadata が空だと、ブートローダは factory (`uf2`) を選ぶ。**
この実機の otadata は全 `0xFF` (`firmware/cp-uac/BUILD_NOTES.md` の
区画表にも「この実機は全 0xFF」と書いてある)。ESP-IDF の
`bootloader_utility_get_selected_boot_partition()` は、otadata の両面が
無効で factory 区画があるとき `FACTORY_INDEX` を返す。
`app.info` の `boot` は `esp_ota_get_boot_partition()` の答え、つまり
**「ブートローダが選ぶ先」**なので `uf2` になる。

**2. ところが `uf2` 区画には読み込める像が入っていない。**
`esp_partition_get_sha256(uf2)` も `esp_ota_get_partition_description(uf2)` も
失敗している (上の `sha256: null` / `version: null`)。同じ呼び出しが
ota_0 / ota_1 では成功しているので、**中身のほうが無い**。
TinyUF2 は「IDF の 2 次ブートローダを改造したもの」＋「factory 区画に置く
UF2 アプリ (3 次ブートローダ)」の 2 つでできているが
(https://github.com/adafruit/tinyuf2 の `ports/espressif/README.md`)、
**この実機には後者が入っていない**。

**3. だから factory を選んだあと、ota_0 へ落ちてきていた。**
`bootloader_utility_load_boot_image(bs, FACTORY_INDEX)` は
「後ろ向きに factory まで試す → 駄目なら前向きに ota スロットを試す」
という作りなので、`uf2` の読み込みに失敗して **ota_0 を掴む**。
otadata は書き換わらない (`set_actual_ota_seq()` は otadata 区画が
**無い**ときにしか書かない) ので、毎回この遠回りをしていた。

**つまり `boot = uf2` は「UF2 ブートローダが動いていた」ではなく、
「空の区画を選んで失敗し、ota_0 へ落ちていた」という意味**だった。

#### これが「戻れる道」に効くところ

| 道 | OTA を入れる前 | `ota.commit` のあと |
|---|---|---|
| (a) 二度押しリセットで UF2 に入る | **もともと効かない。** TinyUF2 の 2 次ブートローダは `boot_index` を factory にするだけで、その factory が空なので結局 ota_0 が起動する | 同じ（変わらない） |
| (b) `tools/flash.py` (ota_0 へ書く) | 効く。otadata が空 → factory 失敗 → **ota_0** が起動するので、書いた像がそのまま動く | ★ **効かなくなる。** otadata が `ota_1` を指すので、ota_0 に何を書いても起動するのは ota_1。下の逃げ道を使うこと |
| (c) `app.boot_factory` (otadata を消す) | — | otadata が空に戻る → factory 失敗 → **ota_0** が起動する。**UF2 には入らない** (空だから)。名前に反して「ota_0 に戻す」命令として働く |
| (d) Raw HID の `bootloader` 命令 → ROM | 効く | **効く (変わらない)。最後の砦はこれ。** |

**`ota.commit` のあとに `flash.py` で復旧するときの手** (どれか 1 つ):

1. `node tools/ota.mjs --image <良い像>` — いちばん素直。ROM に入らない
2. `python3 tools/flash.py --offset 0x210000 …` — 起動している側 (ota_1) を
   直接書き換える
3. `console_hid.py app.boot_factory` で otadata を消してから `flash.py`
   (既定の ota_0 が起動するようになる)
4. ROM に入ってから `esptool erase-region 0xe000 0x2000` で otadata を消す

★ `flash.py` は `bootloader` / `partition table` / `nvs` / **`otadata`** /
`uf2` / `user_fs` に触らない作りで、書く先は `--offset` (既定 `0x10000`) だけ。
**otadata がどちらを指しているかを見ない**ので、この注意が要る。

#### `uf2` 区画についてまだ分かっていないこと

* 空 (全 `0xFF`) なのか、壊れた像が入っているのかは**区別できていない**。
  デバイスからは「読み込める像ではない」までしか分からない
  (フラッシュを読み出す命令を持っていない)。
* したがって **「二度押しリセットで UF2 に入れる」とは言えない**。
  この機体で UF2 を使いたいなら、まず `tinyuf2.bin` を `0x410000` へ
  書く必要がある。**未確認のまま「戻れる道」に数えないこと。**

### 25-8. 次の更新はもう一方のスロットへ行く

`esp_ota_get_next_update_partition()` は**いま動いていないほう**を返すので、
書き込むたびに行き先が入れ替わる。`app.info` の `next` がそれ。

```
起動が ota_0 → 次の OTA は ota_1 へ書く
起動が ota_1 → 次の OTA は ota_0 へ書く
```

これは意図した動きで、**片方が常に「前の版」として残る**。
上の「起動できない像はブートローダが自分で見捨てる」と組み合わさって、
壊れた像を書いてしまっても前の版に戻れる。

以後の更新は **`node tools/ota.mjs --image …` を標準**にする。
`tools/flash.py` は ROM 経由なので**復旧専用**（§5 と 25-7 を読むこと）。

### 25-9. 内蔵 RAM

**64 KB の環状バッファも 4 KB の作業バッファも `.bss` に置いていない。**
環状バッファは PSRAM、作業バッファは内蔵 RAM の**ヒープ**から取り、受信が
終わったら返す。`.bss` の増分は **192 B** (full、`.dram0.bss`
0x123e8 → 0x124a8 = 74,728 → 74,920 B) / **208 B** (dev)。`.data` は変化なし。

### 25-10. 版

`CMakeLists.txt` が `git describe --always --dirty --tags` を `PROJECT_VER`
に入れる。`app.info` の `version` と `hello` の `app` に出る。
**cmake の構成時にしか評価されない**ので、コミットしたあとに入れ直すには
`./build.sh clean` か `idf.py -B <dir> reconfigure` を挟むこと。
段階を表す文字列 (`stackee-idf/N`) は `hello` の `fw` が別に返す
(この段階で `stackee-idf/5` に上げた)。

### 25-11. 実機に触らない確認

```
python3 firmware/tools/test_ota_host.py    # 段階 5: アプリ内 OTA (40 件)
node --test 'test/**/*.mjs'                # 操作盤 (147 件。うち ota は 38 件)
```

Node 側は 2 本に分かれている。`test/ota.test.mjs` が**中核**
(`docs/js/ota.js`) を偽の転送層で回し、`test/ota_node.test.mjs` が
**`tools/ota.mjs` の転送層そのもの**を偽のデバイス (node-hid の口を真似た
もの) で回す。後者は AI が実機へ書き込むときに通る道と同じ経路で、
Report ID を足した 33 バイトの書き方・0xC0 の 29 バイト詰め・転送中に 0xC1 を
止めること・0xC3 の応答が credit に回ること・像が 1 バイトも欠けずに届くこと
まで見る。**node-hid も実機も要らない** (`ota.mjs` は `openDevice()` の中で
初めて node-hid を読むため)。

`test_ota_host.py` は `hostbuild/ota_main.c` で本体の中核 (`stackee_otacore.c`)
を **偽のフラッシュ** と自前の SHA-256 (`hostbuild/stub/sha256_stub.c`) を
差し込んで Mac 上で回す。見ているのは枠の分解・環状バッファ・credit の
数え方・ストリーミング SHA-256 (Python の `hashlib` と突き合わせ)・
断りどころ (二重 begin / 位置違い / magic 違い / sha 違い / 長さ違い /
溢れ / 無通信の自動 abort / フラッシュの失敗)・途中で切れてからの
やり直し。**さらに `docs/js/ota.js` が node で作った枠をそのまま
デバイス側に食わせて通す**ので、ホストと本体の枠が食い違ったら落ちる。

`CommitOnlyTest` (8 件) は `tools/ota.mjs` の `commitWritten()` を**偽の
link** で回す。像なしの `--commit` が `app.info` → `ota.commit` → `app.info`
の 3 回しか聞かない (= **像を 1 バイトも送らない**) ことと、断りどころ
(`next` が無い / `next` が running と同じ / `notready` / `app.info` が無い /
切り替えたのに別の像で起きた) を見る。実機も node-hid も要らない。

### 25-12. まだ確かめていないこと

実機で通ったこと (2026-09-21) は `RESULTS.md` の「アプリ内 OTA」の節。
残っているのは次のもの。

* **わざと壊した像を送ったときの実機の挙動。** ホストテストでは magic 違い・
  sha 違い・長さ違い・溢れ・無通信の自動 abort まで見ているが、実機へは
  正しい像しか送っていない。
* **転送中の打鍵。** `key.inject` は**本物のキーを Mac へ送ってしまう**ので
  使わなかった。`perf.input_loop` から「入力タスクは 1 ms 周期で回り続けた /
  35 ms を超える停止は起きなかった」までは言えるが、実際に文字が出るかは
  見ていない。
* **`app.boot_factory`。** 実機では撃っていない。しかも 25-7 のとおり
  `uf2` 区画が空なので、**UF2 には入らず ota_0 が起動する**。
  名前に反する動きなので、使う前に 25-7 を読むこと。
* **ブラウザ (WebHID) からの書き込み。** Node 経路とまったく同じ `ota.js`
  を通るが、実ブラウザでは走らせていない。
* **別の像への入れ替え。** 実機で流したのは毎回**同じ像**なので、
  「中身が入れ替わったこと」は `running.label` の変化でしか見ていない。
* **`uf2` 区画が空か壊れているか。** デバイスからは「読み込める像ではない」
  までしか分からない (フラッシュを読み出す命令が無い)。
* **段階 3 (ロールバック) と段階 4 (Wi-Fi 入口)。** 入れていない。
  25-6 の「ブートローダが自分で見捨てる」は**読み込めない像**にしか効かず、
  **読み込めるが動かない像**は救えない。
