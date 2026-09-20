# 実機検証の記録（提出用）

すべて Fable が実機で実測したもの。人手の操作を伴う確認は含まない。

## 段階 0（骨組み） 2026-09-16

| 項目 | 結果 |
|---|---|
| 書き込み → 起動 | 像 1f65e314…（275 KB）。USB 復帰 0.3 秒 |
| コンソール | hello / status に応答。現行ツール stackee_console_client.py がそのまま使えた |
| FAT の素材 | manifest.json: size=240 faces=32 を読めた |
| 電池（AXP2101） | 100% |
| LCD | 全 320 行を 42 ms で転送。向き・文字は正しい（ユーザー目視 1 回のみ、以後は CRC 照合へ） |
| メインタスク周期 | 中央値 1000 us / 最大 1057 us / 標本 63,599 |
| 失敗 | 像に 1200 bps タッチが無く、ソフトから ROM へ戻れなかった → 段階 1 で必須化 |

## 段階 1（キーボード） 2026-09-16

| 項目 | 結果 |
|---|---|
| 最終像 | c9d78a8c…（551 KB、fw "stackee-idf/1"） |
| USB HID | Mac が「M5Stack Core S3」のキーボードと Raw HID（0xFF60/0x61）を認識 |
| BLE HID | 起動 0.7 秒でアドバタイズ、0.3 秒後に Mac が接続、暗号化 status=0。**CircuitPython 版のボンドを引き継ぎ、再ペアリング不要**。接続間隔 15 ms。60 秒以上安定。**ただし後日判明: macOS が CircuitPython 版の GATT 構成をキャッシュしたままで、HID としては動いていなかった可能性が高い（段階 3 参照）。BLE での打鍵が Mac に届いたことは未確認** |
| キー → 送出の遅延（BLE、key.inject 20 回） | デバウンス有効時 中央値 5.99 ms / 最大 6.52 ms → **デバウンス無効で 中央値 1.52 ms / 最大 1.55 ms**（目標 ≤ 2 ms / ≤ 5 ms を達成）。送信 40 / 失敗 0 |
| 入力タスク周期 | 中央値 398 us |
| 脱出路 | 1200 bps タッチ → ROM（303A:0009）→ 退避・照合 → hard-reset → 復帰 0.1 秒、を実機で確認 |
| 起動ログ | log.tail で起動直後からのログを読める（BLE の初期化失敗の特定に使用） |
| 参考（CircuitPython 版） | キー → 送出 中央値 8.5 ms、GC 停止 160 ms、まばたき時 20 ms |

### 途中で踏んだ問題と対処
- USB-Serial/JTAG 経由の書き込み後、esptool の RTS リセットでダウンロードモードに戻る → watchdog-reset を使う（flash.py に反映）
- 1 回の ROM セッションで esptool を 2 回呼ぶと 2 回目が応答なし → 1 セッション 1 回に固定（flash.py が 2 回目を拒否）
- NimBLE の HID サービスはレポート数上限が既定 3。記述子が 7 個ぶん要求して init 失敗 → 上限 8 に設定
- ESP32 用の Classic BT メモリ解放を S3 で呼んでいた → nimble_port_init() だけに統一

### 未確認（人手が要るため保留）
- VIA / Remap アプリからの実操作（定義 JSON の読み込み、配列変更）
- BLE 越しの VIA
- 長時間（数時間以上）の安定性

## 段階 2（画面） 2026-09-16

| 項目 | 結果 |
|---|---|
| 像 | 6e668d8b…（563 KB） |
| 素材の展開 | 顔 921,600 B の CRC が Mac 側の期待値と一致。差分表・アイコン・フォント（14 字）読み込み OK |
| 32 表情 | **32/32 が期待値 CRC と一致**（800 ms で全表情） |
| ステータスバー | 代表 6 状態が **6/6 一致** |
| 打鍵の遅延（平常） | 中央値 1.12 ms / 最大 1.61 ms |
| 打鍵の遅延（描画中、ui.selftest 実行中） | 中央値 0.67 ms / 最大 1.63 ms（描画は入力を止めない） |
| 顔 1 コマの描画 | 中央値 8.8 ms / 最大 40.7 ms（ui タスク内。入力には影響しない） |
| LCD 転送 | 中央値 1.9 ms / 最大 209 ms（全面転送時） |
| 問題 | 書き込み後の hard-reset が効かず ROM に残った → Mac からの USB バスリセット（pyusb）で復帰。flash.py の復帰手順に組み込む |

## 段階 3（音声と通信） 2026-09-16 途中経過（像 70b43879…）

| 項目 | 結果 |
|---|---|
| マイク（audio.selftest） | 15,872 サンプル / 1005 ms（RMS 6162）。I2S 録音は動作 |
| スピーカー（ヌル出力） | 53,077 サンプルを 3,320 ms で送出（期待 3,317 ms）。音は出していない |
| Wi-Fi | 起動 8.6〜12 秒で接続、登録簿 2 件を FAT から NVS へ移行 |
| 音量キー | STK_VOLUP で 5% 上がり、ステータスバー CRC が新音量の期待値と一致、2 秒後に NVS 保存、再起動後も残る（FAT の 15% を初回に引き継ぎ） |
| 打鍵の遅延 | 平常 中央値 1.5 ms / 会話中 0.75 ms |
| **NG** 会話 1 往復 | TLS 接続で失敗（mbedtls_ssl_setup -0x008D）。内蔵 RAM 不足の疑い → 修正中 |
| **NG** wifi.list | パスワードが応答に含まれていた（CircuitPython 版は真偽のみ）→ 修正中 |
| **NG** BLE | Wi-Fi 接続中は Mac に繋がらない（アドバタイズ中のまま接続 0 回）。coex / Wi-Fi 省電力設定の疑い → 修正中 |
| 注意 | 検証で音量が 15 → 25 に上がった。提出前に 15 へ戻す |

### 段階 3 追記（像 500b49d6…）
| 項目 | 結果 |
|---|---|
| 会話 1 往復（pi400 → ubook） | **成功**。受理 5.9 秒 / 返答 17.7 秒 / PCM 112 KB / 返答文を受信。会話中の打鍵遅延 中央値 0.74 ms / 最大 1.67 ms |
| TLS | 内蔵 RAM 不足（mbedtls -0x008D = 確保失敗）→ SSL バッファを PSRAM へ、Wi-Fi の静的バッファを削減。内蔵 RAM 空き 52 KB（最小 38 KB） |
| wifi.list | 検査側の誤報。応答は has_password のみで問題なし |
| BLE | **Mac が自動接続しない真因**: Mac から bleak で接続すると成功し、列挙されるサービスに `adaf0001`（Adafruit BLE = CircuitPython 版の GATT）が残っている。macOS が旧 GATT をキャッシュしており、HID の属性を再探索していない。Wi-Fi/coex は無関係（wifi.off でも変わらず）。対処: 接続時に GATT Service Changed を送って再探索させる（実装中） |
| Mac 側でのキー受信 | pynput の監視は Input Monitoring 権限が無く受信できず。**Mac 上でキーが届いたことの自動確認は権限の都合でできない**（USB は TinyUSB の送信成功、BLE は notification の送信成功までを確認） |

### 段階 3 追記 2（像 07257c25… / e524f949…）
| 項目 | 結果 |
|---|---|
| GATT Service Changed | 送信・受領を確認。Mac の GATT キャッシュから旧 Adafruit サービスが消えた（bleak で確認） |
| 相手指定の広告（directed adv） | 起動・切断後に送出（adv_directed 2）。Mac は反応せず |
| 切り分け | 段階 1 の像（以前は 0.3 秒で自動接続）に戻しても Mac は接続しない → **Mac 側の状態が変わった**（HID の関連付けが旧 GATT のまま、または再接続の打ち切り）。本体側でこれ以上できることは無い |
| Mac 側からの操作 | bleak（CoreBluetooth）で接続は可能だが HID としては取り付かない。blueutil --unpair は LE のペアリングを消せない。本体のボンドを消して再接続すると Mac は「Peer removed pairing information」で拒否（自動再ペアリングしない） |
| 結論 | **BLE を使うには、Mac のシステム設定 → Bluetooth で stackee を削除し、再ペアリングする操作が 1 回必要**（ユーザー操作。提出時に明記） |
| 復元 | nvs を検証前の退避像に戻した。ボンド・音量 15%・送信先 BLE・Wi-Fi 登録簿が検証前の状態 |

## 段階 4（周辺機能と本番構成） 2026-09-16（実機の結果は末尾「段階 4 の最終結果」）

Opus サブエージェントが実装したところまで。**実機への書き込みも検証も
まだ行っていない**（この段階の作業では実機に書かない決まり）。
下の表は Fable が実機で埋める。

### 実機に触らずに確かめたこと（ここまでは済んでいる）

| 項目 | 結果 |
|---|---|
| ホストテスト | **315 件すべて OK**（段階 3 までの 219 件 + タッチ 25 件 + Raw HID コンソール 18 件 + 設定の読み書き 13 件 + 段階 4 の道具 40 件） |
| タッチの判定 | 現行 `stackee_touch.py` を import して同じ座標列を流し、**出てくる手が 1 つずつ一致**（乱数 20 本のなぞりを含む） |
| Raw HID の枠 | デバイス側の C・Mac 側の Python・ブラウザ側の JavaScript の **3 つが同じ形**を作る |
| settings の読み書き | 現行 `mask_settings` / `rewrite_settings` と**1 文字も違わない** |
| ビルド（dev） | `1,339,392` B / `68973c4d9604055e7231b04c2fcd4fd9186e9e2893be774f6cfaebc536c3bd92`（ota_0 の 63%） |
| ビルド（full） | `1,340,848` B / `706eb7c8f97cf4db48e31bf6a702f0d3427339d512b088cbe5ed4b7ce704801f`（ota_0 の 63%） |
| 操作盤のテスト | `node --test`（`public/test/`）109 件すべて OK（既存 81 + WebHID 28） |

### 実機で確認する項目（Fable が埋める）

`python3 firmware/tools/check_phase4.py` を回す。

| # | 項目 | 見る数字 | 合格の条件 | 結果 |
|---|---|---|---|---|
| 1 | 像の書き込み（dev） | `hello.fw` | `stackee-idf/4` | OK `stackee-idf/4`（dev `5e10751e…`） |
| 2 | 脱出路（dev） | `flash.py --dry-run` | 1200 bps タッチ → ROM → 復帰が通る | OK 1200 bps タッチ → ROM → hard-reset で復帰（書き込みのたびに通している） |
| 3 | 段階 1〜3 が壊れていない | `check_phase1/2/3.py` | すべて OK（打鍵遅延・表情 32 枚・会話 1 往復） | OK 段階 1: キー→送出 中央値 0.54 ms / 最大 0.79 ms。段階 2: 32 表情・バー 6 状態が一致、打鍵 1.14 ms（描画中 0.74 ms）。段階 3: 会話 1 往復 25.2 s で返答文あり、打鍵（会話中）1.60 ms（**full 像で計測**） |
| 4 | タッチ FT6336 | `touch.status` の `present` / `vendor` / `reads` | `present=true`、`reads` が増え続ける | OK present=true vendor=0x11 reads 26804→26825（full） |
| 5 | タッチ なぞり → マウス | `touch.inject` の `moves` / `moved_x,y` | レポートが 1 件以上出る | OK レポート 11 件 / 移動量 924 |
| 6 | タッチ タップ → クリック | `touch.inject` の `clicks` | 2 件（押す + 離す） | OK ボタンのレポート 1 件 |
| 7 | タッチ 実指（普段使い） | — | ポインタの向きが正しい・右 30% で右クリック | — 普段使い（人の目） |
| 8 | カメラ 撮影 | `camera.capture` の `jpeg_bytes` / `camera_ms` | JPEG が出る。所要 4〜6 秒 | OK JPEG 1826 B / 4682 ms（電源 1001 + 初期化 404 + 捨て駒 3053 + 撮影 99 + JPEG 120） |
| 9 | カメラ JPEG の妥当性 | `camera.dump` → Mac で検算 | SOI/EOI あり・320x240・長さ一致 | OK 1826/1826 B、SOI+EOI、320x240 |
| 10 | カメラ 撮影後の ALDO3 | `camera.status` の `aldo3` | **0**（1 のままだとハードリセットが危ない） | OK aldo3=0 |
| 11 | カメラ 撮影中の打鍵 | `status` の `perf.input` | 撮影中も中央値 ≤ 2 ms / 最大 ≤ 5 ms | OK 撮影中も入力タスク中央値 0.46 ms（status.perf.input_loop） |
| 12 | カメラ 絵の中身 | `--save-jpeg` した画像 | 上下左右が正しい・色が破綻していない（人の目） | — 人の目 |
| 13 | コンソール 全コマンド | `hello.features` | 現行 FEATURES が全部そろう。`loop.*` は `unsupported` | OK 18/18、loop.* は unsupported |
| 14 | settings.get / raw | 応答 | パスワードとトークンの値が**1 文字も出ない** | OK 4 キーとも伏せ字、password/token の値は 0 文字 |
| 15 | settings.set | `settings.set` → `reset` → `settings.get` | 書いた値が残る。FAT の他のファイルが壊れていない | OK（final full 像、HID 経由）`kv={STACKEE_PORT:5555}` → ok changed=1 消去 8 回 312 ms → reset → 4 キーとも同じ値、会話 1 往復も成立 |
| 16 | fs.put | `install_assets.sh --only manifest.json` | 書けて、再起動後に `status.assets` が読める | OK（final full 像、HID 経由）5012 B / 395 ms / 消去 79 回 → reset → manifest=true faces=32、32 表情の CRC も一致。**1 つ前の像では 1 行上限 512 で黙って捨てられていた（直した）** |
| 17 | bench / log.burst / lcd.status / lcd.full | 応答 | 数字が返る。log.burst 500 行で本体が止まらない | OK bench main 中央値 996 us / log.burst 20 行 捨て 0 / lcd.status ready |
| 18 | Raw HID コンソール（dev） | `console_hid.py --transport hid status` | CDC と同じ応答が返る | OK hello/status/log.tail が CDC と同じ応答 |
| 19 | 像の書き込み（full） | `usb.status.profile` | `full` / `cdc=false` | OK `profile=full` / `cdc=false`（full `4a127621…`） |
| 20 | full の列挙 | Mac のシステム情報 | HID・Raw HID・UAC が同時に見える | OK 303A:811A がインターフェース 4 本（HID / Raw HID / UAC 制御 / UAC ストリーム）で列挙 |
| 21 | UAC マイク | `system_profiler SPAudioDataType` | "M5Stack Core S3" の入力が現れる | OK "Stackee Mic"（製造元 M5Stack、USB、16000 Hz、1 ch）。Mac の既定の入力になった |
| 22 | UAC 1 秒録音 | `check_phase4.py --record` | 16,000 サンプル前後 | OK ffmpeg で 2 秒 = 32000 サンプル、RMS 76 / 最大 295（環境音が入っている）。本体側 frames 38000 / opens 2 / underruns 1 |
| 23 | UAC 中の会話 | `talk.inject` + `usb.status.uac` | 会話が成立し、UAC は `silence` が増えるだけ | OK 会話 1 往復成立（受理 5.9 s / 返答 17.6 s / 完了 25.2 s、返答文「うん、了解なのだ。」）。UAC は silence が増えるだけ |
| 24 | full の脱出路 | `console_hid.py bootloader` | ROM に入れる（**1200 bps タッチは使えない**） | OK `console_hid.py --transport hid bootloader` → ROM 0009 が 2.5 s で出現 → `flash.py` の復帰で 811A が 1.2 s。**撮影のあとでも通る**（README §18） |
| 25 | VIA（full） | VIA アプリ | コンソールが相乗りしていても配列を変えられる | — VIA アプリでの実操作は未確認（Raw HID の応答が USB へ出ることは段階 4b で確認） |
| 26 | 操作盤（WebHID） | ブラウザ | full プロファイルで接続・状態表示・設定書き込みができる | — ブラウザでの実操作は未確認（hid.js のホストテストのみ） |
| 27 | 長時間 | 数時間の普段使い | キーが止まらない・ヒープが減り続けない | — 数時間の普段使いは未実施 |

### 実装中に見つけた段階 1〜3 の欠陥（直してある）

| 何 | どうなっていたか | 直し方 |
|---|---|---|
| **Raw HID の応答が送信先に引きずられていた** | `stackee_hid_out.c` が Raw HID (VIA とコンソール) のレポートも「いま選ばれている送信先」で振り分けていた。既定は BLE なので、**BLE のままでは VIA の応答もコンソールの応答も 1 バイトも返らない**。段階 1〜3 では VIA の実操作が未確認だったので露見していなかった。full プロファイルではコンソールがこの道しか無いので、そのままなら手も足も出なくなる | Raw HID は送信先の選択に関わらず**必ず USB へ出す**。USB が刺さっていなければ**捨てて先へ進む**（ここで止めると後ろに並んだキーのレポートまで BLE へ出られなくなる） |

### 書き込む前に読むこと

- **full プロファイルには 1200 bps タッチの脱出路が無い**（CDC が無いため）。
  戻る道はコンソールの `bootloader` / `QK_BOOT` キー / 物理の RST 長押しの 3 つだけ。
  **先に dev で `bootloader` コマンドが効くことを確かめてから** full を書くこと。
- カメラを開いたら、**リセットの前に必ず ALDO3 を切る**
  （`camera.power state=off`）。自走したままリセットするとブートループになる。
- `fs.put` と `settings.set` はフラッシュを消す。試すのは必要な回数だけ。
- 音は鳴らさない。段階 4 が触るのはマイク側だけで、スピーカーには 1 度も触らない。

---

## 段階 4 の 1 回目の実機（dev 像 `0d6710a7…`） 2026-09-16

Fable が `check_phase4.py` で測った。**OK だったもの**: タッチ（存在・なぞり・
タップ）、コンソール互換 18/18、settings の伏せ字、bench、log.burst、lcd。

### NG 1: カメラが内蔵 RAM 不足で開けない — **直した**

```
I cam_hal: Allocating 153600 Byte frame buffer in PSRAM
E cam_hal: cam_dma_config(524): DMA buffer 30720 Byte malloc failed,
           the current largest free block:4096 Byte
E camera:  Camera config failed with error 0xffffffff
```

実機の内蔵 RAM（Raw HID の `status` で読んだ。Wi-Fi と BLE が上がったあと）:

| 数字 | 値 |
|---|---|
| 内蔵 RAM の空き合計 | **12,615 B** |
| いちばん大きい空き塊 | 7,680 B |
| DMA に使える塊 | **4,827 B** |

**空き合計ではなく塊の大きさで決まる。** 直し方（効く順）:

| # | 何をしたか | 効き目 |
|---|---|---|
| 1 | **PSRAM DMA モードで開く**（`cam_set_psram_mode(true)`）。DMA が PSRAM のフレームバッファへ直接書くので、内蔵に要るのは**記述子 40 個 = 480 B** だけ | 30,720 B → **480 B** |
| 2 | 開けなかったときの内蔵 DMA でのやり直し用に `CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX=8192` | 30,720 B → 7,680 B |
| 3 | 起動ログのリングバッファを **PSRAM** へ（内蔵の控えは 2 KB だけ残す） | 内蔵 −14,336 B |
| 4 | Raw HID の送信待ちを 6 KB → 3 KB | 内蔵 −3,072 B |
| 5 | console の大きい応答バッファを 3 か所 → 1 か所 | 内蔵 −3,200 B |
| 6 | 使わない素子のドライバを外す（GC0308 だけ残す） | 像 −38 KB |

3〜5 で内蔵 RAM が **約 20 KB** 戻る見込み（実測は次の書き込みで）。
起動の要所（BLE のあと / Wi-Fi のあと / 音と通信のあと）と撮影の直前に
「最大の塊」をログに出すようにした。`check_phase4.py` に
**「カメラ 撮影前の内蔵 RAM」**の行を足した（PSRAM DMA で開けたか、
内蔵 DMA なら塊が 7,680 B 以上あるか）。

**タスクのスタック（大きい順・参考）**

| タスク | スタック | CPU | 置き場 |
|---|---|---|---|
| stackee_http | 10,240 | 0 | 内蔵（TLS。フラッシュ I/O 無し） |
| main（console） | 8,192 | 0 | **内蔵必須**（FAT を書く） |
| stackee_audio | 6,144 | 0 | **内蔵必須**（音量を NVS へ書く） |
| input | 6,144 | 1 | **内蔵必須**（最優先・遅延に効く） |
| stackee_net | 6,144 | 0 | **内蔵必須**（Wi-Fi 登録簿を NVS へ書く） |
| camera | 4,096 | 0 | 内蔵（PSRAM へ移せる候補） |
| hid_out | 4,096 | 1 | **内蔵必須**（VIA の配列を NVS へ書く） |
| stackee_lcd | 4,096 | 0 | 内蔵（SPI DMA） |
| stackee_ui | 4,096 | 0 | 内蔵（PSRAM へ移せる候補） |
| usbd | 4,096 | 0 | 内蔵（USB 割り込み） |
| touch | 3,072 | 0 | 内蔵（PSRAM へ移せる候補） |

★ **スタックを PSRAM へ移すのは今回やっていない。** `ui` / `touch` /
`camera` の 3 本（合計 11 KB）は移せる見込みだが、フラッシュ書き込み中は
キャッシュが止まるので「その間に走りうるタスク」を PSRAM スタックに
置くのは危ない。1〜5 で足りるかを実機で見てから判断したい。

### NG 2: Raw HID コンソールを Mac から開けない — **直した。実機で確認した**

2 つの別々の問題が重なっていた。

| 何 | 中身 | 直し方 |
|---|---|---|
| ライブラリが見つからない | PyPI の `hid` は `LoadLibrary('libhidapi.dylib')` を**名前だけ**で呼ぶ。Homebrew の `/opt/homebrew/lib` は framework Python の dyld の探索先に無い。`DYLD_LIBRARY_PATH` はプロセス開始前にしか効かない | **import する瞬間だけ `LoadLibrary` を差し替えて**、こちらで見つけた絶対パスを渡す |
| API が 2 種類ある | `hid`（pyhidapi: `Device(path=)` / `.nonblocking`）と `hidapi`（cython: `device()` / `open_path()` / `set_nonblocking()`）で API がまるで違う | どちらでも動く薄い覆い（`_Backend`）を被せる |

さらに**本体側にも欠陥があった**（実機の数字で判明）:
ログを無条件に環状バッファへ溜めるので、Mac が Raw HID を 1 度も読んで
いない間に**起動ログで満杯**になり（`tx_pending 6143 / tx_dropped 299`）、
そのあとの `hello` の応答が「入り切らない」で捨てられて**永久に黙った**。
→ 応答は必ず入れる（古いほうを押し出す）／ログはホストが 3 秒以内に
0xC1 を撃っていなければ捨てる、に直した。Mac 側も送る前に溜まりを
吸い出すようにした（古い像でも通るように）。

**実機で確認（dev 像 `0d6710a7…`、読み取りのみ）**

| コマンド | 結果 |
|---|---|
| `console_hid.py devices` | `usage_page=0xFF60 usage=0x61 / M5Stack Core S3` が 1 個見える |
| `--transport hid hello` | 返る（`fw=stackee-idf/4`、往復 93 ms） |
| `--transport hid status` | 返る（内蔵 RAM の数字はこれで読んだ） |
| `--transport hid log.tail` | 返る（カメラの失敗ログをこれで読んだ） |
| `--transport hid usb.status` | 返る（`reports_in 509 / reports_out 509 / rx_dropped 0`） |

★ `bootloader` は**送っていない**（ROM へ入れるのは Fable）。

### 直したあとの像

| profile | bytes | sha256 |
|---|---|---|
| dev | 1,339,392 | `68973c4d9604055e7231b04c2fcd4fd9186e9e2893be774f6cfaebc536c3bd92` |
| full | 1,340,848 | `706eb7c8f97cf4db48e31bf6a702f0d3427339d512b088cbe5ed4b7ce704801f` |

★ `sdkconfig` を作り直してある（`sdkconfig.defaults` にカメラの設定を
足したため）。段階 3 までの設定（PSRAM・mbedtls の外部確保・Wi-Fi の
静的バッファ 4・FREERTOS_HZ 1000・NimBLE の CCCD 8・16MB・
`partitions-build.csv`）はすべて元のまま入っていることを確認した。

## 段階 4 の最終結果 2026-09-16

### 像

| profile | bytes | sha256 | 退避 |
|---|---|---|---|
| dev | 1,341,456 | `d74dd8b80e86b2dce269c7510cbdd5582f6c4ac815c8affc61d3aa8260ebb7bd` | `firmware/cp-uac/cmp/phase4c-dev-d74dd8b8.bin` |
| full | 1,342,912 | `31f480e9c2aaf07dba0ce8f59bcd07985dff0537fd6deca42a89ac07e1d6bbfb` | `firmware/cp-uac/cmp/phase4c-full-31f480e9.bin` |

1 つ前の像（dev `5e10751e…` / full `4a127621…`）との差はコンソールの
1 行上限 512 → 1024（`fs.put` が入り切らず黙って捨てていた。README §18-6）だけ。
段階 1〜4 の検査は full `4a127621…` で通したあと、最終像でも段階 2・3・
脱出路・`fs.put`・`settings.set` を通した（下）。

### 段階 4b（dev `68973c4d…`）で見つけて直したもの

| 事象 | 直し |
|---|---|
| **撮影のあとの `bootloader` → 復帰でフラッシュが読めなくなるブートループ**（`invalid header: 0xffffff1f`、JEDEC ID ffff01）。G45/G46（PCLK/VSYNC）がストラッピングピンで、撮影後 High のまま残る | G45/G46 を出力 Low + hold で固定してからリセットする。撮影 → bootloader / reset → 復帰を dev 4 回・full 2 回通した。復旧の道具 `tools/pmic_cycle.py`（ROM から AXP2101 に電源再投入）も作って実証。README §18 |
| 撮影 → reset → 撮影で AXP2101 への I2C が 1 回失敗 | 失敗したらバスをリセットして 1 回やり直す。`status.i2c` に回数（その後 fail 0） |
| **`fs.put` が通らない**（full 像で `install_assets.sh --only manifest.json` が 30 秒待ちで失敗、`status.drops` +1） | コンソールの 1 行上限 512 に base64 360 B の枠が入り切らず黙って捨てていた。上限を 1024 に、塊を 300 B に |
| `check_phase3` のバー CRC が 1 回不一致 | 再測定で一致（`lcd.crc` と `status` の間で電池 % が動いた疑い。検査側は両方の期待値を出すので原因を切り分けられる） |

### full 像で通した自動検査

| 検査 | 結果 |
|---|---|
| check_phase1（HID 経由） | 全項目 OK。キー→送出 中央値 539 us / 最大 786 us |
| check_phase2（HID 経由） | 全項目 OK。32 表情 32/32、バー 6/6、打鍵 1.135 ms / 描画中 0.742 ms |
| check_phase3（HID 経由、会話あり） | 会話・マイク・ヌル出力・Wi-Fi・音量キー・NVS・バー OK。**NG 2 件**: 内蔵 RAM 空き 39.2 KB（検査の閾値 40 KB、最小 23.4 KB、最大の塊 26.6 KB。会話の TLS も撮影も通っているので機能は満たす）、BLE 接続 0 回（Mac 側の再ペアリング待ち） |
| check_phase4（HID 経由、録音あり） | 全項目 OK（タッチ・カメラ・コンソール・設定・UAC の列挙と 1 秒録音 16000 サンプル） |
| check_camera_reset（HID 経由） | bootloader / reset とも OK |
| 最終像 `31f480e9…` で再実施 | fs.put・settings.set・reset 後の残り方、check_phase2（32/32・6/6・打鍵 0.76 ms）、check_phase3 ×2（会話 2 往復とも返答文あり、打鍵 0.80〜1.54 ms）、撮影 → bootloader → 復帰 4.4 s。NG は上と同じ 2 件（内蔵 RAM の閾値・BLE 未接続） |

### 未確認（自動では確かめられないもの）

| 項目 | 理由 |
|---|---|
| BLE で Mac にキーが届く | macOS が CircuitPython 版の GATT をキャッシュしており、**システム設定 → Bluetooth で stackee を削除して再ペアリング**する操作が要る（段階 3 の結論）。Mac 側の受信は Input Monitoring 権限が無く自動確認できない |
| USB でも「Mac にキーが届いた」 | 同じく権限の都合。TinyUSB の送信成功（失敗 0）までを確認 |
| VIA / Remap の実操作、Web 操作盤（WebHID）の実操作 | アプリ・ブラウザの操作が要る。Raw HID の往復はコンソールで確認済み |
| カメラの絵の中身、タッチの操作感、長時間 | 人の目・普段使い |

### 本体の状態（提出時）

full 像 `4a127621…`。音量 15%（NVS）、送信先 BLE、ヌル出力 off、Wi-Fi 登録 2 件、ボンド 1 件。

## 抜本対応（2026-09-17〜18）: HTTPS の署名検証失敗と、返答までの遅さ

| 項目 | 結果 |
|---|---|
| 事象 | 起動から約 1.4 時間で証明書検証が `PSA_ERROR_INVALID_SIGNATURE (-149)`、翌日は本文送信中の切断。再起動で直る。サーバ・束・DNS は正常（Mac から同経路で通る） |
| 前例 | esp-idf#18640（ESP32-S3 / v6.0.1、I2S の GDMA と HW 暗号の衝突）と、v6.0.3 の c41dd724d（S3 の HW MPI 署名検証失敗の修正） |
| 対処 | ESP-IDF v6.0.3 へ更新（HW SHA/MPI/AES はすべて有効のまま）、I2S を常設化（削除しない）、返答待ちをロングポーリングに、TLS 失敗時の自己診断 + 自動再起動 |
| 実測（full `5fbd7828…`） | `crypto.selftest` ok / マイク rms 832 / 会話の POST 受理 5.4 秒 / GET 3 回（ソフト暗号時は 8.6〜11 秒・12 回） / 段階 1・2・4 の自動検査 OK |
| 未確認 | 1.4 時間後の再発が無いこと（時間が要る）。返答の到達はサーバ（ubook）の GPU 脱落で音声認識が時間切れのため未確認（README §21） |

## 段階 6 相当: 返答音声を 120 秒にする（2026-09-19）

返答音声の受け皿を 30 秒（960,000 B）から **120 秒（3,840,000 B）** に広げた
（コミット `8093c3e`）。録音の上限は 30 秒のまま。受け皿は録音バッファではなく
通信側の受信バッファで、`GET /jobs/<id>/audio` を始めるときに PSRAM から取り、
再生が終わってから返す（README §11-4）。

### 像

| profile | bytes | sha256 | 退避 |
|---|---|---|---|
| dev（未書き込み） | 1,369,408 | `7fe8cefbe9152a0fd4452308a7effa3a4d9f0700a67444b60007a4599cd9fedf` | `firmware/cp-uac/cmp/phase6-dev-7fe8cefb.bin` |
| **full（本体に入っている）** | 1,370,848 | `5a589186abfad5aa286590243bc53ec2b7fd977329cf73265ddbddd328b1f560` | `firmware/cp-uac/cmp/phase6-full-5a589186.bin` |

書き込み前に載っていた像（full `5fbd7828…`）の退避は
`firmware/cp-uac/cmp/ota0-backup-20260919-022806.bin`（sha 一致を確認済み）。

### 書き込み（2026-09-19 02:28 頃 JST）

| 段 | 内容 |
|---|---|
| 手順 | `flash.py --dry-run`（退避と照合）→ 通常起動へ復帰 → `flash.py --skip-backup`。README §5 のとおりセッションを 2 つに分けた |
| ROM への突入 | **Raw HID の `bootloader` 命令**（full には CDC が無いので 1200 bps タッチは使えない） |
| 書き込み | esptool が "Hash of data verified" |
| 復帰 | watchdog-reset で **0.1 秒** |

### 書き込み後の確認（実機、2026-09-19 02:30 頃 JST）

| 項目 | 結果 |
|---|---|
| `hello` | 応答。profile **full** / proto 2 |
| `status` の `psram_free` | **6,743,164 B**（会話していない状態）。静的見積もりの「起動時に 1,578,668 B 使用」なら 8,388,608 − 1,578,668 = 6,809,940 B なので、**差は 66,776 B**。見積もりで「Wi-Fi / lwIP に約 100,000 B」と置いた枠の中に収まっており、積み上げは合っている |
| check_phase1 | 全項目 OK。キー → 送出 中央値 **506 us** / 最大 **832 us**、BLE 接続中 |
| check_phase2 | 全項目 OK。32 表情 **32/32**、バー **6/6**、打鍵 **1.561 ms** |
| 実機に触らない検査 | ホストテスト **323 件 OK**、`gen_keymap --check` OK、dev / full とも新しい警告なし（既存の `tud_init` 非推奨 1 件のみ） |

### 未検証

| 項目 | なぜ |
|---|---|
| **返答受信時の 3,840,001 B の連続確保** | 会話を実際に行わないと確かめられず、音が出るので今回は実施していない。次に普段使いで会話が起きたときの `log.tail` で見る。確保に失敗していれば `stackee_http.c` が **PSRAM の空き合計と最大の塊**をログに残し、会話だけが「返答の受け皿を確保できません（メモリ不足）」で失敗する（キーボードには影響しない） |
| 120 秒の返答を最後まで鳴らすこと | 同上。ホスト側では 50 秒・120 秒の PCM を最後まで鳴らし切ることを確認済み（`test_talk_host.py`） |
| 3.84 MB の受信そのものの所要時間 | `/audio` の GET には `?wait=` が付かないので HTTP のタイムアウトは 30 秒のままだが、これはソケット単位の待ちであって総転送時間の上限ではない、という読み。実測はしていない |

### 気づき（記録のみ、原因は未調査）

書き込み前の旧像（full `5fbd7828…`、起動から約 43 分、操作盤の Web Serial / HID 接続を
使ったあと）で、**`hello` と `log.tail` は応答するのに `status` だけが 20 秒待っても
応答しなかった**。新しい像では起動直後から応答する。`status` は他のコマンドより
集める項目が多い（perf・heap・Wi-Fi・音・USB…）ので、どれかの読み出しで
待たされた可能性があるが、**原因は調べていない**。再現したら `log.tail` を先に取る。

## 返答音声の字幕（2026-09-20）

### 像

| | |
|---|---|
| プロファイル | **full**（本体に載せるのはこちら） |
| バイト数 | **1,374,176**（ota_0 の 65%。字幕ぶんの増分は dev / full とも **+3,328 B**） |
| sha256 | `b59358de4afecead8cff88e91e6f2af3c00867bd6f1fbb0777c433f8504a556f` |
| 直前の像の退避 | `firmware/cp-uac/cmp/ota0-backup-20260920-192852.bin`（書き込み前に dry-run で `5a589186…` を退避・照合してから `--skip-backup`） |
| 素材 | `firmware/assets/font16.bin` **236,770 B**（東雲 16px、半角 158 + 全角 6,879 = 7,037 字、sha256 `01ba663a…`）を FAT の `/stackee_assets/` へ |

### 素材の転送でつまずいたところ（直した）

`tools/fs_put.py` の `MAX_BYTES` が **65,536** のままで、231 KB の `font16.bin` を
**送る前に**断っていた。本体側の受け皿（`stackee_console.c` の `FSPUT_MAX`）は
**256 KB** あるので、道具の側を 256 KB に揃えた。転送は **50 秒 / フラッシュ消去 255 回**。

### 書き込み後の確認（実機、Raw HID、読むだけ）

| 項目 | 結果 |
|---|---|
| `hello` | 応答。profile **full** / `stackee-idf/4` |
| check_phase2 | 全項目 OK。32 表情 **32/32**、バー **6/6**、**字幕フォント OK**（236,770 B / crc 853512126 / 158 + 6,879 字）、**字幕の帯 7/7 一致**、桁数 15 桁 = 240 px |
| 打鍵の遅延 | 平常 中央値 **1.530 ms** / 最大 1.698 ms、描画中 0.796 / 1.644 ms、**字幕中 中央値 1.543 ms / 最大 1.705 ms**（合否 6 / 10 ms） |
| 帯の描画そのもの | 最悪 **714 us**（はみ出し 16 桁）。空 164 us / 全角 15 桁 631 us（合否 2,000 us） |
| 実機に触らない検査 | ホストテスト **358 件 OK**、`gen_font16 --check` OK、dev / full とも警告なし |

### `perf.ui_sub` の最大値は合否に使えない（測って分かった）

最初は `perf.ui_sub` の **max** を 2 ms と突き合わせていて、**中央値 613 us /
最大 2,188 us** で NG になった。40 回ずつ条件を外して測ると、**最小値は
631〜643 us で動かない**（= 描画そのもの）。尾を伸ばしているのは顔の 1 コマ
（240 行 = 115 KB の SPI 転送）と Wi-Fi で、`ui.subtitle` を処理する main タスクが
**CPU0 でいちばん低い優先度（1）** だから乗ってくる。普段、帯を描くのは
**ui タスク（優先度 3）** で SPI ワーカーには割り込まれない。
詳しくは README §23-3。`check_phase2.py` は **1 ケース 6 回の最小値**を合否にし、
中央値・最大値は参考値として出すように直した。

### 実機の会話（無音、`audio.null` + `talk.inject`）

| 項目 | 結果 |
|---|---|
| 返答 | 「よかったです。また何かあれば、声をかけてください。」70,496 サンプル = **4,406 ms** |
| 状態 | `upload → poll_wait → poll → **subs** → audio → play_wait → playing → idle`、`complete_ms` **38,524**、`errors` 0 |
| 字幕 | **出なかった**。`sub_pages=0` / `subs_failed=1`、ログに `[talk-subtitles] {"pages":0,"status":404,"got":1}` |
| 原因 | 中継（`dev/server/proxy.py`）の GET 白名簿が `/jobs/<id>(/audio)?` だけで、`/subtitles` を上流へ通していなかった。**直したが、pi400 へ配り直すまで字幕は出ない** |
| 再生位置 | `audio.status` の `pos` は 4.1 秒のあいだホストの実時間に対して**ずれ 5 ms 以内**。ページ選択が使う時計は正しい |
| 字幕取得の費用 | `subs` 状態が **約 8 秒**（16.3 → 24.3 秒）。本文 4 KB でも要求ごとに TLS を張り直すため。そのぶん喋り始めが遅れる |

### 未検証

| 項目 | なぜ |
|---|---|
| **サーバの `start_ms` と本体が切り替えた経過 ms の差** | 中継が `/subtitles` を通していないので、本体に字幕が 1 行も届いていない。中継を配り直したあと、同じ手順（無音の `talk.inject` + 200 ms ごとの `talk.status` / `audio.status`）で測る |
| 実際に帯が見えるか（目視） | 自動では確かめられない。CRC は 7/7 一致しているので画素は合っている |
| 字幕を出しながらの長い返答（120 秒・48 ページ） | 会話 1 往復しか行っていない |

## 字幕の本文を done の JSON に載せる（2026-09-20、書き込み前）

### なぜ

実機で測ったら、`subtitles_url` を別に GET するのに **約 8 秒**かかっていた
（`subs` 状態が 16.3 → 24.3 秒）。本文は 4 KB 以下なのに遅いのは、要求ごとに
TLS を張り直すため（§20 と同じ）。そのぶん**喋り始めが 8 秒遅れる**。
4 KB のために 8 秒払う価値は無いので、契約に「`done` の JSON に本文そのものを
混ぜる」道を足した。`subtitles_url` は旧い本体のために残す。

### 本体側の変更

| | |
|---|---|
| 選び方 | `subtitles` があればそれを使い **別 GET を飛ばす** → 無ければ `subtitles_url` を GET（旧サーバ互換）→ どちらも無ければ字幕なし |
| 出どころ | `talk.status` の **`sub_src`**（`inline` / `url` / `none`）と `[talk-subtitles]` / `[talk-turn-timing]` のログに出る |
| 返答待ちの受け皿 | 8,192 → **16,384 B**（`8192 + 2 x 4096`）。通信側が 1 往復ごとに **PSRAM** へ取って閉じるときに返すので、常駐の使用量は増えない |
| 逃がしの中継ぎ | **持たない**。1 行ぶん（128 B、audio タスクのスタック）ずつ解いて行解析へ渡す。**内蔵 RAM の静的な使用量は 1 バイトも増えない** |
| 壊れていたら | 1 ページも採れなければ `subtitles_url` へ落ちる。それも無ければ字幕なしで鳴る（会話は止めない） |

### ビルド（実機には触っていない）

| プロファイル | 前 | 後 | 増分 | sha256 |
|---|---|---|---|---|
| dev | 1,372,736 B | **1,373,392 B** | +656 B | `518b66c8ce82f7e76214059f813ef15c0b164040bb07f58aeb663ea8c9f30f4a` |
| full | 1,374,176 B | **1,374,848 B** | +672 B | `cea28e10ccb1e46591b6e4b56918a5bb695951a064d000aae925c524a5b7baa3` |

どちらも警告 0。ota_0 の 65%。

### 実機に触らない確認

ホストテスト **378 件 OK**（前 358 件）。増えたのは 2 本:

* `test_talk_host.py` の `InlineSubtitleTest`（13 件）— 本文があれば `subs` を
  通らない / 無ければ従来どおり GET / どちらも無ければ字幕なしで鳴る /
  壊れていたら url へ落ちる / `\t` `\n` `\"` `\\` `\/` が戻る /
  4 KB の本文を丸ごと読む / 48 ページの上限 / 長すぎる行は改行まで捨てる /
  返答待ちの受け皿が 16,384 B になっている
* `test_cfg_host.py` の `JsonBigTest`（7 件）— `stackee_jsonlite` が 4 KB 級の
  値と `\t` `\n` を正しく扱うこと。**返答文の 256 バイトの道（`stackee_json_str`）
  とは別の道**であることを、同じ本文を両方に通して確かめている

### 書き込み後の確認（実機、Raw HID、読むだけ、2026-09-20）

像 **full `cea28e10…`**（退避 `firmware/cp-uac/cmp/ota0-backup-20260920-200911.bin`、
照合用 `phase7-full-b59358de.bin`、新像の控え `phase8-full-cea28e10.bin`）。
pi400 の中継は `/subtitles` を通す版、ubook は字幕同梱版（`ab06dee`）が稼働中。

check_phase2（hid）は全項目 OK。字幕の帯 **7/7**、帯の描画そのもの 最悪 **893 us**、
打鍵の遅延 平常 **1.570 ms** / 字幕中 中央値 **1.449 ms** / 最大 **1.675 ms**。

無音（`audio.null`）で `talk.inject` を 2 往復:

| | 1 往復目 | 2 往復目 |
|---|---|---|
| 返答 | 「了解です。また何かあれば、声をかけてください。」 | 「はい、待っています。準備ができたら声をかけてください。」 |
| 音声 | 70,154 サンプル = **4,384 ms** | 79,712 サンプル = **4,982 ms** |
| `sub_src` | **`inline`** | **`inline`** |
| ページ数 | 3（`sub_bytes` 84 / `dropped` 0） | 4 |
| 状態 | `upload → poll_wait → poll → audio → play_wait → playing → idle`（**`subs` を通らない**） | 同じ |
| HTTP の往復 | **3 回**（別 GET のころは 4 回） | 3 回 |
| `reply_ready → audio_ready` | **12,524 ms**（`/audio` 12,513 ms だけ） | **11,907 ms**（`/audio` 11,892 ms だけ） |

別 GET のころは同じ区間が **17,838 ms**（`subs` 8,034 + `/audio` 9,848）だった。
`/audio` 自体が 9.8〜12.5 秒とばらつくので、差を引き算で語るより
**「1 往復まるごと（約 8 秒）無くなった」** と読むのが正しい。

#### ページが切り替わった再生位置

| 往復 | ページ | 切り替わった位置 [ms] | 帯の CRC32 | 出ていた文字（CRC から特定） |
|---|---|---|---|---|
| 1 | 0 → 1 | 1,302 〜 1,432 | 526477896 | 「また何かあれば、」 |
| 1 | 1 → 2 | 2,692 〜 2,827 | 660979874 | 「声をかけてください。」 |
| 2 | 0 → 1 | 695 〜 861 | 3230108024 | 「待っています。」 |
| 2 | 1 → 2 | 2,191 〜 2,331 | 3131895648 | 「準備ができたら声」 |
| 2 | 2 → 3 | 3,606 〜 3,756 | 1090296636 | 「をかけてください。」 |

**出ていた文字は推測ではない。** 帯の CRC32 が `tools/subtitle_expected.py` で
同じ `font16.bin` から描いた期待値と 1 ビットも違わず、`ui.status` の
`sub_len`（21 / 24 / 27 / 30 B）もその文字のバイト数と一致する。
1 文を 15 桁で割るときに**均等に割る**（17 字 → 8 + 9）というサーバの決まりも
ここに出ている。挟み幅（130〜166 ms）は**こちらのポーリングの往復時間**であって
本体の遅れではない。

### 未検証

| 項目 | なぜ |
|---|---|
| **サーバの `start_ms` と本体が切り替えた経過 ms の差** | ジョブ ID を本体も `talk.status` も出さず、ubook への ssh が使えない。ジョブは 5 分で消えるので、測るには「本体を回しながら同時に ubook 側で `job-timing` の ID を拾って `/jobs/<id>/subtitles` を取る」必要がある。逆算（文の境目は PCM の長さから厳密・文の中は字数で比例・文間 150 ms の無音）では 1 往復目の「1 → 2」の予測 2,672〜2,744 ms が観測窓 2,692〜2,827 と重なるが、**これは逆算であって照合ではない** |
| 帯が実際に見えるか（目視） | 自動では確かめられない。画素は CRC で一致している |
| 長い返答（120 秒・48 ページ） | 2 往復とも 5 秒・3〜4 ページ。FAT の一次回答 PCM で誘える返答はこの長さまで |
| 8,192 B ぎりぎりの done JSON | サーバ側の上限いっぱいの応答は出ていない（実測 84 B の本文） |

## 会話が失敗した（内蔵 RAM の枯渇、2026-09-20）

ユーザーが 42 キーで話しかけたら会話が 7 秒で終わった。実体は
`esp-aes: Failed to allocate memory` → `write error :-0x0084` で、
**POST が TLS を書けなかった**。ハードウェア AES は DMA 可能な内蔵 RAM を要る。

### 実機で測った数字（Raw HID、読むだけ）

| | 字幕の前（`5a589186`、起動 554 s、会話前） | 失敗した像（`cea28e10`、起動 2,192 s、会話 2 回のあと） |
|---|---|---|
| `heap_internal` | **29,127 B** | **16,375 B** |
| `heap_internal_largest` | **20,480 B** | **7,936 B** |
| `heap_internal_min` | 23,436 B | 7,128 B |
| `heap_dma` | 21,339 B | 8,587 B |
| `psram_free` | 6,505,512 B | **6,504,304 B** |

* **PSRAM は減っていない。** ログの 5,529,212 B は POST 中の値で、録音の受け皿
  960 KB を抱えているだけ。待機中はほぼ同じ。font16.bin の 237 KB は起動時 1 回。
* **漏れていない。** 待機中に `ui.subtitle` x40 / `lcd.crc` x20 / `status` x40 /
  `ui.selftest` / 無音の `talk.inject` / `audio.selftest` を順に回しても
  `heap_internal` は **16,375 B から 1 バイトも動かない**。`wifi.off` で戻るのも 196 B。

### 常駐の増分（`build/stackee.map` の比較）

| 置き場 | `8093c3e`（= `5a589186`） | `cea28e10` | 差 |
|---|---|---|---|
| `.bss.a`（`stackee_audio.c`。`stackee_talk_t` を丸ごと） | 1,592 B | 5,048 B | **+3,456 B** |
| `.bss.ui`（`stackee_ui.c`） | 2,984 B | 3,168 B | +184 B |
| 内蔵 RAM の静的合計 | 107,320 B | 110,960 B | **+3,640 B** |

字幕が増やした常駐は **3,640 B**。実機で足りなくなった **12,752 B** の一部でしかなく、
残り約 9 KB は**この起動中に消費されて戻っていない**（成功した会話 2 回のあと）。
出どころは未解明で、字幕とは別の話。

### 直したこと

`stackee_talk_t`（約 4.8 KB）を **PSRAM** へ移した（audio タスクと console
タスクしか触らず、割り込みからは触らないため）。PSRAM が取れない機体では
内蔵 RAM に落ちる。

| | 内蔵 RAM の静的合計 | `.bss.a` |
|---|---|---|
| 字幕の前 | 107,320 B | 1,592 B |
| 失敗した像 | 110,960 B | 5,048 B |
| **いま** | **106,160 B** | **248 B** |

**失敗した像より 4,800 B 軽く、字幕を入れる前より 1,160 B 軽い。**
あわせて `[talk-http-timing]` に `internal_free` / `internal_largest` を足し、
1 往復ごとの戻り具合を `log.tail` から読めるようにした。

### ビルド

| プロファイル | バイト数 | sha256 |
|---|---|---|
| dev | 1,373,536 | `c0732a8aa3e5330b99919b3f11c51bdfc678c8d66193b9c0dffe02be334aef4e` |
| full | **1,374,960** | `4544180ccde805c6f9c6bcaf5682b11fdd21e5d9b47b732e3a55bb82e14b0dfe` |

どちらも警告 0。ホストテスト **378 件 OK**。
（2026-09-21 に `firmware/` を公開リポジトリへ移した際、書き込み先の名指しを環境変数に移した分の 2 件が増えて **380 件**になった。）

### 直した像での実測（full `4544180c…`、実機、Raw HID、2026-09-20）

書き込み後の起動 31 秒: `heap_internal` **33,939** / `largest` **25,600** /
`min` 23,436 / `heap_dma` 26,151 / `psram_free` 6,500,644。
失敗していた像（16,375 / 7,936）から**空きが +17.6 KB、最大の塊が +17.7 KB**。

無音（`audio.null`）の `talk.inject` を 4 往復＋ユーザーの実会話 1 往復:

| | 内蔵 RAM 空き | 最大の塊 | 最小 |
|---|---|---|---|
| 起動 31 秒後 | **33,939 B** | **25,600 B** | 23,436 B |
| 1 往復目のあと | 33,403 B | 17,408 B | 23,436 B |
| 2 往復目（ユーザーの実会話。返答 14.2 秒 / 455,722 B / 8 ページ） | 33,403 B | 17,408 B | 23,436 B |
| 3〜5 往復目 | **33,403 B** | **17,408 B** | 23,436 B |
| `ui.subtitle` x20 + `lcd.crc` x20 | 33,403 B | 17,408 B | 23,436 B |

**漏れではなく初回確保。** 減るのは 1 往復目だけで、空き −536 B・最大の塊 −8,192 B。
空きの減りが小さいのに最大の塊が大きく減るのは、小さな確保がいちばん大きい
空き領域の中に居座って割ったため。2 往復目以降はどちらも動かず、返答 14.2 秒
（455 KB）の大きな往復でも増えない。`heap_internal_min` は起動時の 23,436 B の
まま＝**会話の最中でも 23.4 KB を下回っていない**（失敗した像は 7,128 B）。

会話は 5 往復とも成功。`errors` 0 / `subs_ok` 5 / `subs_failed` 0、
`sub_src` は毎回 **`inline`**、HTTP は 1 往復 3 回 × 5 = 15 回で `failures` 0。
ユーザーの実会話（無音ではない）も `inline` で 8 ページ出ている。

### 未検証

| 項目 | なぜ |
|---|---|
| **失敗した像で余分に消えていた約 12 KB の出どころ** | 「会話 1 往復あたり」ではない（5 往復で 0）。あの起動では check_phase2 と `wifi.off` / `wifi.on` を含む長い探りを回していたのでそのどれかの疑いがあるが、**再現していない**（直した像は起動 690 秒 / 会話 5 回で横ばい）。普段使いで `[talk-http-timing]` の `internal_free` を見張る |
| **`status` だけが答えないことがある** | 会話の直後に 60 秒待っても返らず、`hello` / `talk.status` / `ui.status` / `audio.status` / `axp.read` / `bench` は 0.1 秒で返る状態が数分続いたあと自然に戻った。`bench` の `main_max_us` が **4,427,755 us**（4.4 秒）で、コンソールを持つ main タスク（CPU0 の優先度 1）が長く止められている。README の「気づき」と同じ症状。**原因は未調査** |
