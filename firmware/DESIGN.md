# Stackee ネイティブファームウェア 設計書

作成: 2026-09-15（設計: Fable / 実装: Opus サブエージェント）
土台の選定根拠: research/stackee/c_firmware_base_2026-09-15.md

## 1. 目的と非目的

**目的**
- キー入力の遅延と揺らぎをなくす。Python の GC 停止（1 回 160 ms）、描画・通信によるメインループの停止を構造的に排除する。
- 配列変更を VIA / Remap で行えるようにする（USB 接続時）。BLE 越しの配列変更は実験項目。
- 現行の機能（BLE/USB キーボード、JIS、音声会話、顔、ステータスバー、Wi-Fi、コンソール、音量、電池、カメラ、タッチパッド）を失わない。

**非目的**
- QMK 本体（ビルドシステム、キーボード定義ツリー）を土台にすること。QMK は ESP32 非対応（docs.qmk.fm/compatible_microcontrollers、2026-09-15 確認）。
- 移行途中で CircuitPython 版の改良を続けること。以後 firmware/kmk は凍結し、バグ修正のみ。
- 公開リポジトリ（public/ subtree）への本ファームの公開。理由は §7。

## 2. 土台と構成

| 項目 | 決定 | 根拠 |
|---|---|---|
| フレームワーク | ESP-IDF **v6.0.3**（upstream espressif/esp-idf、`firmware/esp-idf-v6.0.3/`。2026-09-18 に cp-uac 同梱の Adafruit fork v6.0.1 相当から移行） | native-http / native-lcd の C 資産が使える。S3 のハードウェア MPI の署名検証の修正が v6.0.3 で入った (README §20) |
| プロジェクト置き場 | firmware/（本書と同じ階層。CMake の ESP-IDF プロジェクト） | firmware/kmk と並置し、両方をビルド・書き込みできる状態を保つ |
| キー処理 | QMK の quantum を「部品」として取り込む（quantum.c, keyboard.c, action*.c, action_tapping.c, dynamic_keymap.c, via.c, eeconfig.c 等 約 6,100 行）。橋渡し層は esp32-qmk-lucky65 の port/ を参考に自作（MIT） | 前例 https://github.com/chcbaram/esp32-qmk-lucky65 で ESP-IDF 上の動作と VIA が実証済み |
| QMK の版 | Phase 1 開始時点の最新の安定タグに固定し、firmware/third_party/qmk に**コピー**（サブモジュールにしない）。取り込んだファイル一覧と版を third_party/qmk/IMPORT.md に記録 | 更新は手作業になるため、差分が追える形にする |
| USB | TinyUSB（esp_tinyusb）。HID は 1 インターフェースに Report ID でキーボード・マウス・コンシューマを同居 | ESP32-S3 の IN エンドポイントは EP0 を含め 5 本まで（§4） |
| BLE | NimBLE + esp_hid（esp_hid_device 例の構成）。HID 記述子は USB と同一内容（LANG1/LANG2 = 0x90/0x91 を含む Usage Maximum） | 現行 code.py の JIS 対策（adafruit_ble の記述子上限 0x89 問題）を再発させない |
| 画面 | esp_lcd + 自前レンダラ。native-lcd の転送ワーカーを移植。顔は既存 faces.bin（4bpp、240×240、32 枚、差分表 changes.bin）をそのまま使う。LVGL は使わない | 既存素材と描画方式をそのまま使い、移植量を最小にする |
| 音声 | I2S 全二重は使わず現行どおり半二重。ES7210 録音、AW88298 再生。一次回答は既存 ack_0N.pcmz | 現行の stackee_halfduplex の設計を踏襲 |
| 通信 | esp_wifi + esp_http_client（TLS）。native-http の非同期ワーカー設計を踏襲 | 同上 |
| 保存領域 | パーティション表は CircuitPython の 16MB 版をそのまま使う（ota_0 = 0x10000 / 2048K、nvs、user_fs = FAT 11968K）。アプリは ota_0（≤ 2 MB）。VIA の配列と設定は NVS。素材（顔・アイコン・フォント・一次回答・Wi-Fi 登録簿）は user_fs の FAT を ESP-IDF の FATFS で読む | flash.py による CircuitPython への戻し道がそのまま有効。素材の転送手段が変わらない |
| 起動時間 | 目標 3 秒以内でキー入力可能（Wi-Fi・BLE 接続は後追い） | 現行より短くする。計測で確認 |

## 3. タスク構成と CPU 割り当て

| タスク | CPU | 優先度 | 役割 | 制約 |
|---|---|---|---|---|
| input | 1 | 最高 | TCA8418 の FIFO 読み出し → QMK keyboard_task（デバウンス・レイヤー・HoldTap）→ HID レポート生成 → 送信キューへ | このタスクは I2C（PORT.A）と送信キュー以外を触らない。ブロッキング呼び出し禁止。1 周 1 ms 目標 |
| hid_out | 1 | 高 | 送信キューから USB / BLE へ出す。USB 接続中は USB、それ以外は BLE（現行の HID_SWITCH で固定も可） | 送信失敗は捨てて次へ。input を待たせない |
| ui | 0 | 中 | 顔の状態機械と差分描画、ステータスバー、LCD 転送（native-lcd 相当） | フレームバッファは ui が所有。他タスクは状態値だけ渡す |
| audio | 0 | 高 | 録音（ES7210）、再生（AW88298）、一次回答 | I2S の DMA バッファ補充を止めない。会話中は ui の描画量を落とす |
| net | 0 | 低 | Wi-Fi 自動接続、HTTPS の送受信、状態通知 | 受信 PCM はストリームで audio へ渡す |
| console | 0 | 低 | JSON 行プロトコルの受付（§4 の通り道）、ログ出力 | 現行 stackee_console のコマンド名・応答形式を維持 |
| camera | 0 | 低 | 撮影と送信（現行 stackee_camera 相当） | 撮影中は ui を止めてよい |
| touch | 0 | 中 | FT6336 読み出し → マウスレポート → 送信キュー | 現行 stackee_touch の挙動を踏襲 |

BLE コントローラは sdkconfig 既定（CPU0）のまま。Wi-Fi/BLE 共存は ESP-IDF の coex 設定を使う。

**入力遅延の設計目標（計測で合否を決める）**
- キー押下（TCA8418 割り込み or ポーリング）から USB レポート送出まで: 中央値 ≤ 2 ms、最大 ≤ 5 ms（会話中・描画中を含む）
- 現行（CircuitPython）の実測: 中央値 8.5 ms、GC 停止時 160 ms、まばたき時 20 ms

## 4. USB 構成（IN エンドポイントの上限への対処）

ESP32-S3 は IN エンドポイントが EP0 を含めて 5 本まで（docs.espressif.com esp-usb usb_device）。

| プロファイル | 構成 | IN 本数 | 用途 |
|---|---|---|---|
| dev | HID(1) + Raw HID(1) + CDC(2) | 4 + EP0 | 開発中。ログとコンソールを CDC で読む |
| full | HID(1) + Raw HID(1) + UAC マイク(1) | 3 + EP0 | 本番。コンソールとログは Raw HID |

- Raw HID インターフェースは 1 本、トップレベルコレクションも **1 つだけ**（Usage Page 0xFF60 / Usage 0x61、32 バイト固定、Report ID なし）。VIA / Remap は Report ID 0 で送受信するため、コレクションを 2 つにして Report ID を付けると互換性を失う恐れがある（段階 0 の指摘、2026-09-16 決定）。
- コンソールは VIA プロトコルの中に相乗りさせる: QMK の via.c が未知の command id を `via_command_kb` に回す仕組みを使い、Stackee 独自の command id（0xC0〜、VIA 予約域 0x00〜0x0F と衝突しない）で JSON 行を 30 バイトずつ分割して運ぶ。dev プロファイルではコンソールは従来どおり CDC。
- キー用インターフェースのトップレベルコレクションは 3 つ（Report ID 1 = キーボード / 2 = マウス / 3 = コンシューマ）。**システムコントロール（電源・スリープ等）のコレクションは置かない**。現行の配列に該当キーが 1 つも無いため。QMK は `host_system_send()` 経由でシステム用レポートを作ることがあるが、送信キューの段で捨てて数だけ数える（`status` の `hidq.failed`）。必要になったら段階 4 でコレクションを 1 つ足す（IN エンドポイントは増えない）。
- 記述子は boot protocol を名乗らない（Report ID を使うため）。BIOS での利用は要件にない。
- プロファイルは sdkconfig の切替（ビルド時）。まず dev で全機能を作り、最後に full を検証する。
- Web 操作盤（docs）は full プロファイルでは Web Serial ではなく WebHID で話す必要がある。これは Phase 4 で対応し、それまでは dev プロファイル + 既存の Web Serial 版を使う。

## 5. キーマップと VIA / Remap

- マトリクス: TCA8418 の 5 行 × 10 列（50 スロット）をそのまま QMK の MATRIX_ROWS/COLS にする。43 キーの物理配置は keymap.py の ASSIGN / COORD_MAPPING から生成する（生成スクリプトを tools/ に置き、手書きしない）。
- レイヤー数: 現行 6（keymap.py の KEYMAP の段数）。VIA の dynamic keymap は 6 層ぶん確保。
- 既定配列: keymap.py から変換して keymaps/default_keymap.c を生成。HoldTap の対応表:
  - KMK HT(tap, hold) → QMK MT(hold, tap)、**TAPPING_TERM 200**（keymap.py が `_HOLDTAP.tap_time = 200` を設定している）
  - KMK LT(layer, key) → QMK LT(layer, key)、**TAPPING_TERM 300**（Layers インスタンスの tap_time は KMK 既定の 300 のまま。keymap.py のコメントのとおり、移植元は HoldTap 側にだけ 200 を設定していた）。QMK 側は `TAPPING_TERM_PER_KEY` で HT と LT を別々に答える（2026-09-16 決定: KMK の実挙動に合わせる）
  - prefer_hold=True → HOLD_ON_OTHER_KEY_PRESS を当該キーだけ有効（get_hold_on_other_key_press）。★ KMK の `KC.HT(...)` は **prefer_hold=True が既定**なので、ホームロー修飾はすべてこれが付く
  - tap_interrupted=False → PERMISSIVE_HOLD を当該キーで無効
  - **QUICK_TAP_TERM は 0（無効）**。QMK の既定は `QUICK_TAP_TERM = TAPPING_TERM` で有効になっており、「タップ直後に同じ HoldTap キーを押し直すと長押ししてもタップ扱い」という、KMK 側に無い癖が付く（KMK の `repeat=` に相当するが keymap.py はどのキーにも指定していない）
  - HoldTap の設定は**マトリクス位置 + キーコード**で引く。レイヤー 0 のキー 36 と 37 はどちらも LT(1, LANG1) だが prefer_hold が違うため、キーコードだけで引くと片方の設定が消える
  - **タップ側が修飾つきの HoldTap は QMK の MT() では表せない**（`MT(mod, kc)` の kc は 1 バイト）。該当するのは `KC.HT(KC.LSFT(KC.SCLN), KC.LSFT)`（レイヤー 1 のキー 21。離せば JIS の「+」、押さえれば Shift）。独自キーコード `STK_MT_n` に逃がし、qmk_port の小さな状態機械で「hold = 修飾 / tap = 16bit キーコード」を実装する
  - 対応の正しさは Phase 1 で「同じ打鍵列を入れて同じ出力になる」テストで確認する（tools/test_keyseq_host.py。TAPPING_TERM の両側を 1 ms 単位で狙う）
- **独自キーは HID に出さない。ただし `MIC(kc)` だけは出す（2026-09-21）。**
  `LT(layer, kc)` / `MT(mod, kc)` と同じ発想で、**中の基本キーコードを 8 bit
  そのまま持つ**包み（`MIC(kc) = 0x7F00 | (kc & 0xFF)`）。押している間だけ顔が
  `listening` になり、中のキーは `register_code` / `unregister_code` で普通の
  キーとまったく同じ道を通る。既定配列の右下は `MIC(KC_F13)`（ユーザーが PC 側の
  プッシュトゥトークに使っているキー）。打鍵列テストが「素のキーと 1 バイトも
  違わない」ことを見ている。
  ★ 領域は **`QK_USER`（0x7E40..0x7FFF）の上半分**。`QK_KB` は 0x7E00..0x7E3F の
  64 個しかなく、VIA の customKeycodes はその並び順で番号が決まるので、256 個の
  連続領域が入らない。VIA からは名前付きの入口 `MIC_F13`〜`MIC_F24`（12 個）を
  通し、本体が `MIC(kc)` に読み替える。ほかのキーは Remap の「Any」に 16 進で。
- **既定配列を変えたら「移行」を足す（2026-09-21）。** VIA / Remap で配列を
  1 度でも書き換えた本体は、以後 **EEPROM に保存された配列**で動く。像を新しく
  しても `default_keymap.c` は見に行かないので、**新しい既定は黙って無視される**
  （実機で踏んだ: 右下が `0x0068` のままで顔が変わらなかった）。
  `main/qmk_port/stackee_keymap_migrate.c` に移行の表を置き、EEPROM の
  キーボード用 4 バイト（`eeconfig_read_kb`。VIA は触らない）に番号を持って、
  起動時に未適用のものだけ当てる。移行は「**旧い既定のままなら差し替える。
  ユーザーが変えていたら触らない**」で書く — 保存済みの配列は本人のもの。
- **新しい独自キーは並びのうしろに足す。** VIA の customKeycodes の並び順が
  そのままキーコードの番号になり、その番号は **VIA で変えた配列として NVS に
  保存されている**。途中に足すとうしろが 1 つずつずれ、保存済みの配列の意味が
  黙って変わる。置き場は tools/gen_keymap.py の `TRAILING_CUSTOM_KEYS`
  （`STK_MT_0` = 0x7E07 は動かさない）。
- **既定配列の位置ごとの差し替えは、この木だけで完結させる**
  （tools/gen_keymap.py の `KEYMAP_OVERRIDES`）。移植元の keymap.py を触ると
  現行 CircuitPython 版の配列まで変わってしまう。ネイティブ版にしか無い
  独自キーは、位置とキーの結び付けをこちら側に置く。
- **マウスキーは有効**（`MOUSEKEY_ENABLE` / `MOUSE_ENABLE`、`quantum/mousekey.c` を取り込み）。現行 CircuitPython 版も KMK の MouseKeys を入れている（code.py が `keyboard.modules.append(MouseKeys())`）ので、同じように使える状態にしておく。動作モードは QMK 既定（加速つき）。キーコードは標準の `MS_UP` / `MS_DOWN` / `MS_LEFT` / `MS_RGHT` / `MS_BTN1`〜 / `MS_WHLU` / `MS_WHLD` / `MS_ACL0`〜2。レポートは既存の Report ID 2 のコレクションで送る（USB・BLE 共通。将来のタッチパッドと同じ送信キューを通る）。VIA 定義 JSON は標準キーコードなので変更不要（2026-09-16 決定）。keymap.py 側に `KC.MS_*` / `KC.MB_*` / `KC.MW_*` があれば生成時に対応する QMK キーコードへ変換する（対応表は tools/keycodes.md）。
- 独自キー（VIA の customKeycodes で表示）: STK_TALK、STK_VOLUP、STK_VOLDN、STK_HID_SWITCH、STK_BLE_REFRESH、STK_CAMERA、STK_TOUCH_SCROLL、および上記の STK_MT_n。**QK_KB_0（0x7E00）以降**に割り当て、process_record_kb で処理して false を返す（HID には出さない）。QMK の SAFE_RANGE（= QK_USER = 0x7E40）ではないのは、**VIA の customKeycodes が QK_KB_0 から順に対応づく約束**のため。並び順が実装とずれると VIA 上で別のキーとして表示されるので、生成時に機械照合する（2026-09-16 決定）。
- VIA 定義 JSON: firmware/via/stackee.json（layouts は keymap.py の KLE 定義から生成）。VID/PID は現行の USB 記述子と同じ値を使い、Remap のカタログ登録は行わない（定義 JSON の手動読み込みで使う）。
- 保存: QMK の eeconfig / dynamic_keymap を NVS 上のブロブに載せる（lucky65 の eeprom.c と同じ方式。書き込みは遅延して input タスクを止めない）。

## 6. 移行の段階と合否判定

各段階は Opus サブエージェントが実装し、Fable が設計との整合と計測結果を判定する。**実機への書き込みは各段階の最後に、ユーザーの了承を得てから行う**（書き込み中は約 1 分キーボードが使えない）。戻し道は flash.py --rollback（CircuitPython 版の ota_0 像）。

| 段階 | 内容 | 合否（すべて実測） |
|---|---|---|
| 0 | ESP-IDF プロジェクトの骨組み。ビルド、ota_0 への書き込み手順（flash.py の像指定を一般化）、起動ログ、LCD に起動表示、CDC コンソールで status 応答、FAT から manifest.json を読める、CircuitPython への戻しを実機で確認 | 書き込み → 起動 → 戻し が通る。起動から CDC 応答まで ≤ 3 秒 |
| 1 | キーボード: TCA8418、QMK quantum、USB HID（キーボード・マウス・コンシューマ）、BLE HID、JIS、HoldTap 対応表、独自キー、VIA（USB）、Remap 定義 JSON、NVS 保存 | 打鍵列テストが現行と一致。キー→USB 送出 中央値 ≤ 2 ms（本体内の自己計測）。Mac の VIA アプリと Remap で配列変更でき、再起動後も残る。BLE で打鍵できる |
| 1b | 実験: BLE 接続中の VIA / Remap / Vial 認識 | 結果を記録するだけ（合否なし） |
| 2 | 画面: LCD ワーカー、顔（既存素材・差分描画・状態機械）、ステータスバー（アイコン・h24 フォント・電池・音量・Wi-Fi・BLE） | 顔の 1 コマ描画中も段階 1 の遅延目標を維持。全 32 表情が出る |
| 3 | 音声と通信: Wi-Fi 自動接続（既存の登録簿）、HTTPS ワーカー、録音・送信・ポーリング・受信・再生の会話フロー、一次回答、音量キーと保存、電池表示 | 会話 1 往復が pi400 経由で成立。会話中の打鍵遅延が目標内。音量が再起動後も残る |
| 4 | 周辺: コンソールの全コマンド、Web 操作盤の WebHID 化、full プロファイル（UAC）、カメラ、タッチパッド、セルフテスト、ログ | 現行 stackee_console の FEATURES がすべて応答。UAC で Mac のマイクとして見える |

段階 1 の完了時点で「キーボードとして」は CircuitPython 版を置き換えられる。

**提出の方式（2026-09-16 決定）**: ユーザーは途中の確認に協力しない。段階 2〜4 は全部完成させてから一括で提出する。検証は人手を前提にしない:
- 画面: console の `lcd.crc`（フレームバッファ全体と顔領域の CRC32）を、Mac 側で同じ素材から描いた期待値と照合する。表情 32 枚・ステータスバーの各状態を順に出して照合する自己テストを console から起動できるようにする
- 音: 鳴らさない。録音は取り込んだ PCM の RMS とサンプル数、再生は I2S DMA の送出カウンタと「ヌル出力」モードで確認する
- 会話: console から既知の PCM を注入して pi400 → ubook の往復を自動で行い、返答 PCM の受信サイズと再生カウンタで判定する
- BLE: CircuitPython 版（NimBLE）が nvs に残したボンドを引き継ぎ、再ペアリングを要求しない。引き継げない場合は提出時に理由を書く
- 中間の像の書き込みは自動検証のために行うが、ユーザーへの報告は最終提出でまとめる

## 6a2. 誤って触れたときは何も起こさない（2026-09-21）

会話キーに指がかすっただけで、一次回答が**声を出し**、録音がサーバへ飛んでいた。
録音を終えた時点で 2 つとも満たしたときだけ先へ進める。

- **長さ**: 録音 ≥ `STACKEE_TALK_MIN_MS`（既定 1000 ms）。録音の長さ = キーを
  押していた長さなので、「1 秒押さなければ何も起きない」と同じ意味になる。
- **声**: 20 ms の窓の RMS が `STACKEE_TALK_VOICE_RMS`（既定 1000）以上の窓が
  `STACKEE_TALK_VOICE_WINDOWS`（既定 5 = 100 ms）以上。

満たさなければ**静かに idle へ戻る**（音も出さず、送らず、`thinking` の顔にも
しない）。数えた値はログと `talk.status` に出す。

**★ 「いちばん大きい窓」で判定してはいけない（実測して分かった）。** 環境音しか
無い部屋でも、マイクを開けた直後の窓 5（100〜120 ms）の RMS が毎回 3,257〜3,945
になる。窓の平均は 270 前後。ES7210 の立ち上がりの跳ねで、位置も大きさも毎回
同じ。最大だけを見ると、この跳ね 1 つで必ず「声あり」になる。**越えた窓の数**
で見れば、1 窓の跳ねは通らず、100 ms 続く声は通る。

**★ 判定は録音バッファ全体をなめる。** 別途「録音の冒頭が欠ける」調査が進んで
おり、将来プリロールを前に足す可能性がある。取り込みながら数えるのではなく
バッファを見る形にしておけば、足したぶんも自然に数に入る。

**★ 閾値は settings.toml で変えられる。** 部屋が変われば環境音も変わる。
既定は像に焼くが、**その既定は実測から決める**（`audio.selftest` が
`rms_max` / `rms_at` / `rms_mean` を返すのはそのため）。

## 6b. 返答音声の字幕（2026-09-20）

喋っている間、画面の下の 70 px（y=250..319）に **3 行**まで字幕を出す。顔は
y=50..249 なので**領域が重ならない**。顔の差分描画と字幕は互いを描き直さない。

★ **2026-09-21 に 1 行 → 3 行に広げた。** 空きを作るために、顔を
**上 29 px・下 11 px 切り詰めて 240x200** で出すようにした。**元絵
（`assets/src/faces` の PNG）と `faces.bin` は 1 バイトも変えていない** —
切り詰めるのは起動時に PSRAM へ展開したあとだけ（921,600 → 768,000 B）。
画面に置く位置（y=50）も変えていない。

★★ **29 / 11 は 32 コマ全部の余白の最小値そのもの。この値でだけ 1 画素も
落ちない。** 同じ日に一度 **上下 33 px 対称**で入れたが、`awake` の上 29 px と
`camera` の下 11 px を数え落としていて 5 コマ 856 px 欠けた。
**欠けない範囲に切り直す**のがユーザーの決定（経緯と実測は README §23-8）。

**同期はサーバが決める。** サーバは返答を文ごとに合成して連結しているので、
各文の音声の開始位置（ms）を正確に知っている。区切り（ページ）と開始時刻を
サーバが計算して渡し、本体は「再生位置（ms）≥ 開始時刻」の**最後の**ページを
出すだけ。本体側に推定ロジックを置かない（置くと、合成の都合が変わるたびに
本体を書き込み直すことになる）。

| 層 | 責務 | 置き場 |
|---|---|---|
| 契約 | `GET /jobs/<id>` の `done` に **`subtitles`（本文そのもの）** と `subtitles_url` が増える。本文は `<start_ms>\t<text>\n` の並び（行数 ≤ 48、本文 ≤ 4096 B） | README §23-1 |
| 状態機械 | `subtitles` があればそれを使い、**別 GET を飛ばす**。無ければ `subtitles_url` を `/audio` の前に 1 往復して取る（旧サーバ互換）。再生中は経過 ms からページを選び、**変わった時だけ** `ops->subtitle()` を呼ぶ。渡すのは「その頁の先頭からいまのページまで」を改行で繋いだもの（`stackee_talk_band`、頁は 3 行ごと） | `main/stackee_talksm.c` |
| 画面 | `stackee_ui_set_subtitle()` は文字列を置くだけ。帯を描くのは **ui タスクだけ** | `main/stackee_ui.c` |
| 描画 | 黒地・白文字の 240x70（1 行 22 px = 余白 3 + 字形 16 + 余白 3 の 3 行 + 上下に 2 px の余り）。**帯はいつでも黒**（字幕が無くても白に戻さない。起動直後から）。行の区切りは改行。ESP-IDF に依存しないので、ホストビルドで実機と同じ実体を走らせられる | `main/stackee_draw.c` |
| 一次回答 | `ack_01..05.pcmz` はサーバを通らないので、**行を素材に前計算して持たせる**（`manifest.json` の `acks[].lines`）。鳴らし始めに帯へ出し、鳴り終わりで消す | `main/stackee_audio.c` / `tools/ack_lines.py` |
| フォント | 東雲 16px（半角 8x16 + 全角 16x16、Public Domain）を FAT に置き、起動時に PSRAM へ丸ごと読む。字形は二分探索 | `main/stackee_font16.c` / `tools/gen_font16.py` |

**決めたこと（と、その理由）**

- **像を大きくしない。** フォント 231 KB は FAT（`user_fs`、11,968 KB）に置く。
  顔 `faces.bin` と同じ流儀。ota_0 は 2 MB しか無く、OTA の 2 面ぶんで 4 MB を
  使っている。
- **再生中に割り当てない。** ページは `stackee_talk_t` の中の固定配列
  （48 × 64 B）。字形は font16.bin の中を指すポインタのまま使う。
- **本文は done の JSON に載せる（2026-09-20 に変更）。** 別 GET は実機で
  **約 8 秒**かかった（要求ごとに TLS を張り直すため）。4 KB の本文のために
  喋り始めを 8 秒遅らせる価値は無い。返答待ちの受け皿を 8 KB → 16 KB
  （PSRAM、1 往復ごとに取って返す）に広げるだけで済む。逃がしを解く中継ぎは
  **1 行ぶん（128 B、スタック）**にして、内蔵 RAM の静的な使用量を増やさない。
  `subtitles_url` は旧い本体のために残し、本体も「本文が無ければ取りに行く」
  で旧サーバに合わせる。
- **行は積む。頁は 3 行ごと（2026-09-21）。** サーバの行（ページ）は 1 行ずつ
  そのまま使う。再生位置が行の開始時刻を越えるたびに帯へ**足す**（上から順）。
  3 行が埋まった状態で 4 行目が来たら帯を空にして 4 行目を 1 行目に置く。
  ★ **行の集合はページ番号だけで決まる**（ページ `i` は必ず `i % 3` 行目）。
  本体に「いま何行出しているか」という状態を持たせないので、`ui.subtitle`
  一発でまったく同じ絵を再現でき、CRC32 の照合がそのまま使える。
  サーバの契約（`<start_ms>\t<text>`）は 1 バイトも変えていない。
- **一次回答の行もサーバの規則で割る（2026-09-21）。** 一次回答は本体が
  素材の音声をその場で鳴らすのでサーバを通らないが、**行の切れ目の作法が
  返答と違う画面**になってはいけない。だから `tools/ack_lines.py` を
  `public/server/stackee_server.py` の写しにして、`tools/import_faces.py` が
  manifest を書くときに `text` から行を引く。写しである以上ずれうるので、
  ホストテストが**本物のサーバを import して** 5 文すべてで突き合わせる。
  **サーバのコードは変えない**（契約も変えない）。
  ★ 割るのは素材を作るとき。本体は割らない — 帯に入る 3 行だけ取って出す。
- **帯はいつでも黒（2026-09-21）。** 「字幕が無ければ白に戻す」をやめた。
  画面の下端が会話のたびに白と黒を往復するのをやめる、というユーザーの決定。
  Mac 側の参照描画とホストビルドも同じにしたので、**全面の CRC32 が帯まで
  含めて一致する**（顔とバーの CRC は領域が重ならないので変わらない）。
- **字幕の失敗は会話を止めない。** 取得も解析も「できなければ字幕なし」で
  先へ進む。`subtitles_url` が無い旧サーバでは HTTP の往復すら増えない。
- **合否は数字で。** 帯の絵は CRC32（`ui.subtitle` ↔ `tools/subtitle_expected.py`）、
  描画時間は `perf.ui_sub`（約束 ≤ 6 ms。1 行の 2 ms から線形には伸びない。
  理由は PSRAM のフレームバッファと 32 KB のデータキャッシュ。README §23-8）、
  打鍵の遅延は `key.inject` の中央値。目視での確認を合格条件にしない
  （§8 の作法どおり）。
- **顔の切り詰めも数字で。「欠けない」を数えて確かめる。**
  `ui.selftest` の 32 表情の CRC32 は「切り詰めたあとの y=50..249」で取り直し、
  `tools/render_expected.py` が**同じ切り詰め**をしてから期待値を組み立てる。
  落ちる非背景画素は `render_expected.py --json` の `face_lost` に出て、
  ホストテストが **0 であること**と **29/11 が余白の最小値であること**の
  両方を固定している。素材を差し替えたらここが落ちるので、そのとき数え直す。

## 6c. アプリ内 OTA — 操作盤からファームを書き換える（2026-09-21）

本体が**動いたまま** Raw HID で像を受け取り、使っていないほうの区画（`ota_1`）
へ書き、otadata を切り替えて再起動する。方式の比較は
`research/stackee/web_flash_2026-09-20.md`。ここは段階 0〜2（本体の受け皿と、
人と AI で 1 本になる中核 JS）まで。**段階 3（ロールバック）と段階 4
（Wi-Fi 入口）はまだ入れていない。**

### なぜ ROM を通らないか

いまの `tools/flash.py` は ROM のダウンロードモードへ落として書く。ROM に
入った瞬間、戻る道は 3 段構え（`write_reg` + watchdog / pyusb の USB バス
リセット / ROM から I2C で AXP2101 を再投入）に頼ることになるが、**ブラウザ
からは 1 段目しか撃てない**。2026-09-16 に 1 段目が 2 回空振りした実績がある
以上、日常の経路を ROM 経由にはしない。`flash.py` は復旧専用として残す。

アプリ内 OTA なら、
* 書いている間もキーボードは生きている（止まるのは再起動の約 3 秒だけ）
* 途中で切れても otadata は触っていないので、次も古い像で起動する
* 二重起動は `esp_ota_begin` のハンドルが 1 つしかないので構造的に防げる

### 層の分け方

| 層 | 責務 | 置き場 |
|---|---|---|
| 中核（本体） | 0xC3 の枠の分解・環状バッファ・credit・SHA-256 の照合・状態機械。**ESP-IDF に依存しない**のでホストビルドでそのまま回せる | `main/stackee_otacore.c` |
| 実機（本体） | `esp_ota_*` と PSA の SHA-256 を差し込む。バッファの確保。`app.info` / `ota.*` の受け答え。遅延再起動 | `main/stackee_ota.c` |
| 入口（本体） | Raw HID の 0xC3 を横取りして中核へ渡す。登録が無ければ何も変わらない | `main/stackee_conhid.c` の `stackee_conhid_set_raw_hook()` |
| 中核（ホスト） | 枠の組み立て・分割・credit・sha256 の照合・`ota.*` の順序。**運び方を知らない** | `docs/js/ota.js` |
| 転送（ホスト） | WebHID（人）/ node-hid（AI） | `docs/js/hid.js` / `tools/ota.mjs` |
| 画面 | ファイル選択・版と sha256 の並べ表示・進捗 | `docs/index.html` + `ota.js` の `attachOtaUi()` |

**中核 JS を 1 つにしてあるのが肝。** 人が押すボタンと AI が叩く
`node tools/ota.mjs` が同じ `ota.js` を通るので、書き込みの筋道が食い違い
ようがない（研究報告 §3-3 の (ii)）。

### 0xC3 の枠（JSON の 0xC0/0xC1/0xC2 とは別の種別）

```
ホスト → デバイス (32 B)
  byte 0      0xC3
  byte 1      len       本文の有効バイト数 0..27 (0 は「状態だけ返せ」)
  byte 2..4   offset    像の先頭からの位置 (24 bit little endian)
  byte 5..31  payload   27 バイト

デバイス → ホスト (32 B)
  byte 0      0xC3
  byte 1      state     0 idle / 1 receiving / 2 done / 3 failed
  byte 2      err
  byte 3..6   accepted  環状バッファに入れた累積 = 次に送るべき位置
  byte 7..10  written   esp_ota_write に渡し終えた累積 = credit の起点
  byte 11..14 size
  byte 15     flags     bit0 = この枠は受け取らなかった
  byte 16..19 free      環状バッファの空き
```

**位置を毎枠に書く。** 1 バイトの通し番号では credit の窓（32 KB ≒ 1,200 枠）
の中で一周してしまい、取りこぼしたときにどこから送り直すか決められない。
位置を持たせると、本体は「期待する位置と違えば捨てる」、ホストは「本体が言う
`accepted` から送り直す」だけでよい。代償は本文 29 → 27 バイト
（理論上限 29 → 27 KB/s、1.4 MB で 47 → 51 秒）。

**1 枠ごとの ack にしない。** フラッシュの消去・書き込み中はキャッシュが止まり、
IRAM 非常駐の割り込み（TinyUSB を含む）が数十 ms 止まる。1 枚ごとに返事を
待つ作りだと毎回そこでタイムアウトする。本体は「受け取った累積が 1 KB を
跨ぐたびに 1 枚だけ」返し、ホストは `written + 32 KB` まで先行して送る。

**本体は自分からは何も言わない。** 応答を返すのは 0xC3 を受けたときだけ。
だから credit で送れなくなったホストは、本文 0 バイトの 0xC3（「状態だけ
返せ」）を撃って `written` の伸びを聞きに行く。これを忘れると、そこで止まる。

### 誰がどのタスクで何をするか

| 仕事 | タスク | 理由 |
|---|---|---|
| 0xC3 を環状バッファに積む | **入力タスク**（CPU1・1 ms 周期） | `via_command_kb()` がここから呼ばれる。フラッシュに触ると打鍵が数十 ms 止まる |
| 環状バッファ → フラッシュ | **メインループ**（CPU0・優先度 1） | ここはコンソールしか見ていないので、数十 ms 止まってよい。画面も音も別タスク |
| `ota.begin` / `end` / `abort` | メインループ | 書き手と同じタスクなので、バッファの付け替えで競合しない |

環状バッファ（64 KB）は **PSRAM**。フラッシュへ渡す直前に **内蔵 RAM の
4 KB** へ写す — ESP-IDF の `esp_flash_write()` は元バッファが外部 RAM だと
**32 バイトずつ**しか書けず、その都度キャッシュを落とすので桁違いに遅くなる
（`esp_flash_api.c` の `temp_buf[8]`）。4 KB に揃えると、1 回の書き込みで
消えるのがフラッシュ 1 セクタちょうどになる。

`OTA_WITH_SEQUENTIAL_WRITES` を使う。`OTA_SIZE_UNKNOWN` だと 2 MB を最初に
全部消すので、その間ずっと USB が止まる。

### sha256 は 2 種類ある（混ぜると一生合わない）

| | 何 | どこで出る | 何のため |
|---|---|---|---|
| ファイル全体 | `shasum -a 256 stackee.bin` | `ota.begin` の引数、`ota.end` の `sha256` | 転送で 1 バイトも化けていないか |
| 像の名札 | 像の**末尾 32 バイト**（`hash_appended`） | `esptool image_info`、`esp_partition_get_sha256()`、`app.info` の running/boot/next、`ota.end` の `partition_sha256` | **どの区画に何が入っているか** |

名札のほうは「末尾 32 バイトを除いた部分の SHA-256」なので、ファイル全体の
値とは必ず違う。`esp_partition_get_sha256()` は返す前に中身を検証する
（`bootloader_common.c`）ので、`ota.end` の `partition_sha256` が期待と
一致したら「ディスクの .bin とフラッシュの像が同じで、かつ ESP-IDF の検査も
通った」と端から端まで言い切れる。

### 内蔵 RAM

**64 KB の環状バッファも 4 KB の作業バッファも `.bss` に置かない。**
環状バッファは PSRAM から最初の `ota.begin` で取り、そのあとは手放さない
（受信中に `free()` すると、入力タスクが書いている最中の領域を返すことに
なる）。作業バッファは内蔵 RAM のヒープから取り、受信が終わったら
メインループが返す。SHA-256 の途中経過と「走っている像の名札」の控えも
PSRAM の小さな構造体にまとめてある。

残る `.bss` の増分は **192 B**（full、`.dram0.bss` 0x123e8 → 0x124a8）。
中身は状態機械のカウンタと 2 本の 32 B の sha だけ。

### 戻れる道

* **切り替えるまでは無傷。** `esp_ota_set_boot_partition()` を呼ぶまで
  otadata は 1 バイトも変わらない。転送中に電源が切れても古い像で起動する。
* **`ota.end` と `ota.commit` を分けてある。** 「書けた」と「そっちで起動
  する」は別の決断。分けておけば「書いたが切り替えていない」状態で人が
  読み返せる。
* **`app.boot_factory`** は otadata を消して factory（`uf2`）を選ぶ。
  ROM を通らずに CircuitPython の UF2 ブートローダへ戻る道が 1 命令でできる
  （ESP-IDF の `esp_ota_set_boot_partition()` は factory を指されたとき
  「ota info 区画を初期化するだけ」）。**まだ実機では試していない。**
* **`tools/flash.py`（ROM 経由）は復旧専用として残す。**

### 版

`CMakeLists.txt` が `git describe --always --dirty --tags` を
`PROJECT_VER` に入れる（`CONFIG_APP_PROJECT_VER_FROM_CONFIG` は切った）。
`app.info` の `version` と `hello` の `app` に出る。cmake の構成時にしか
評価されないので、コミットしたあとに入れ直すには `./build.sh clean` か
`idf.py -B <dir> reconfigure` を挟むこと。段階を表す文字列
（`stackee-idf/N`）は `hello` の `fw` が別に返す。

### まだやっていないこと

* **段階 3: ロールバック**（`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`）。
  「何をもって valid とするか」を決めて、**わざと壊れた像で 1 回試す**まで
  入れない。試さないなら入れないほうがよい（研究報告 §6 の最大のリスク 2）。
* **段階 4: Wi-Fi 入口**（`ota.fetch <url>`）。受け皿は同じなので入口を
  1 本足すだけだが、内蔵ヒープを先に測る必要がある。
* **再開**（`esp_ota_resume`）。いまは途中で切れたら最初から送り直す。
  1.4 MB で 1〜2 分なので、まずは作らない。

## 7. ライセンスと公開範囲

- QMK 由来のコードは GPL-2.0-or-later。QMK は「via.c を他ファームへ翻案すること」「非公開の無線ライブラリとリンクした配布」を違反例に挙げている（docs.qmk.fm/license_violations）。ESP-IDF の Wi-Fi/BLE はバイナリ提供。
- **2026-09-21 決定（ユーザー）: ソースも像も公開する。** QMK を取り込んだ派生物なので GPL に合わせるのが素直、という判断。`firmware/` を公開リポジトリ takashicompany/stackee の `firmware/` へ移し、`firmware/LICENSE` に GPL-2.0 の全文を置いた。**`firmware/` だけが GPL で、同じリポジトリの `docs/`・`server/`・`test/` は従来どおり。**
- 公開にあたって、この土台から**実機や個人環境を名指しする値を外した**。書き込み先の ROM シリアル（= MAC）は環境変数 `STACKEE_ROM_SERIAL`、像の退避先は親リポジトリがあればそこ・無ければ `~/.local/share/stackee/backups`。現行 CircuitPython 版（`firmware/kmk`、非公開）に依る道具は「あれば使う、無ければ飛ばす」にした。
- 素材（顔は Stack-chan 由来、アイコンは Apache 2.0、フォントは efont）は現行どおり LICENSE を同梱。字幕用の `assets/font16.bin` は**東雲フォント**（/efont/、Public Domain）から生成したもの。BDF 本体はリポジトリに入れず、生成ツール（`tools/gen_font16.py`）と生成物だけを置く。

## 8. 開発の作法

- 各段階の作業は firmware/ 配下で完結させ、firmware/kmk は触らない。
- 自己計測を最初から入れる: 入力タスクの周期、キー→送出の遅延、各タスクの最大処理時間、ヒープ残量。console の `perf` コマンドで読める。合否判定はこの数字で行い、人手の打鍵確認を求めない。
- テスト: キー処理は Mac 上でホストビルド（QMK の quantum は依存が少ない）して打鍵列テストを回す。ESP-IDF 依存部は実機で自己テスト。
- 音を鳴らさない。一次回答・返答の再生を伴う検証は、再生先をヌルにした状態で行い、実音の確認はユーザーが普段使いで行う。
- 書き込み前に必ず flash.py の退避と照合を通す。戻し道が壊れた状態で次へ進まない。
- **脱出路はファーム側の必須機能**（2026-09-16 追加）: 1200 bps タッチで ROM ダウンロードモードへ入ること、コンソールの `reset` / `bootloader` コマンド。段階 0 の像はこれを欠いていたため、戻すのに物理ボタン（RST 長押し）が必要になった。段階 1 以降の像は、flash.py の enter_rom() が通ることを合格条件に含める。
- ROM への突入は 1 回の書き込みにつき 1 回にする（退避は事前の --dry-run で済ませ、本番は --skip-backup）。2 回目の突入で本体が応答しなくなる事象あり（2026-09-16）。

## 8b. 段階 0 の指摘への決定（2026-09-16）

| 指摘 | 決定 |
|---|---|
| Raw HID の Report ID が VIA と衝突しうる | §4 のとおりコレクション 1 つに戻し、コンソールは VIA の独自 command id に相乗り。段階 1 で修正 |
| 音量の保存先が現行（FAT のファイル）と設計（NVS）で違う | 段階 3 で「初回起動時に FAT の値を NVS へ移す」を入れる。設定を消さない |
| boot protocol を名乗っていない | そのままでよい |
| TinyUSB を cp-uac のツリー参照にしている | IDF Component Registry の espressif/esp_tinyusb と tinyusb を idf_component.yml で版固定して使う（cp-uac が消えても壊れない）。段階 1 で切替 |
| idf.py flash が factory(uf2) を指す | ビルド専用の CSV で uf2 行を data 型に読み替え、警告を消す。実機のパーティション表は焼かない。README の「idf.py flash 禁止」は維持 |

## 9. 未決事項（ユーザー判断が必要）

1. ~~full プロファイルで CDC を捨てて UAC を残す、で良いか~~ → **決定（2026-09-15 ユーザー）: USB マイクは使う。** full プロファイルは HID + Raw HID + UAC で確定。コンソール・ログ・Web 操作盤は Raw HID（Usage 0x62）に載せる。
2. Remap のカタログ登録をするか（VID/PID の重複回避が要る）。当面は定義 JSON の手動読み込みで運用する想定。
3. 段階 1 の書き込みのタイミング（キーボードとしての置き換えが起きる）。
