# stackee

M5Stack CoreS3 を使った自作キーボード **stackee** の公開リポジトリ。

`docs/` に静的サイト **「stackee 操作盤」**、`server/` に Mac / Linux で動かすサーバー側コード、
`firmware/` に本体の C ファームウェア (ESP-IDF) を置きます。
音声受信サーバーの起動・API は [server/README.md](server/README.md) を参照してください。

- 公開先: https://takashi.company/stackee/
  (https://takashicompany.github.io/stackee/ はここへ転送されます)

---

## これは何か

USB ケーブルでつないだ stackee を、ブラウザから見て・設定するためのページです。
できることは 6 つ。

| 画面 | できること |
|---|---|
| 接続 | ブラウザのデバイス選択ダイアログから stackee を選んでつなぐ。**つなぎ方は USB シリアルと USB HID の 2 通り**あり、「自動」で選ばせることもできる |
| 状態 | 電池残量・充電中か・キー入力の送信先 (USB / BLE)・Wi-Fi の状態・ファームウェアの版・起動からの経過時間 (10 秒ごとに自動更新) |
| Wi-Fi ネットワーク | 接続先の Wi-Fi を**最大 8 件**まで登録・上書き・削除する。スキャンして選ぶこともできる |
| 音声サーバ | 音声サーバのアドレス (`STACKEE_HOST`) とポート (`STACKEE_PORT`) を書き込む |
| 再起動 | 設定を反映するための再起動。再接続まで自動で待つ |
| ログ | デバイスがシリアルに出している内容をそのまま表示 (一時停止・消去・キーイベント行の非表示) |

つなぎ方が 2 通りあるのは、デバイス側のファームウェアに 2 つのプロファイルがあるためです。
**どちらでも、このページからできることは同じ**です (流れるデータが同じで、運び方だけが違う)。

| ファームのプロファイル | つなぎ方 | 中身 |
|---|---|---|
| `dev` | USB シリアル (Web Serial) | USB CDC を持つ。これまでどおり |
| `full` | USB HID (WebHID) | CDC を持たず、コンソールが Raw HID (32 バイトのレポート) に載る |

> **登録しただけでは、まだ Wi-Fi につながりません。**
> 今のファームウェアは登録内容を保存するだけです。
> **自動接続は今後のファーム更新で有効になります。**

専用アプリのインストールも、Python も、ドライバも要りません。

---

## 対応ブラウザ

**デスクトップ版の Chrome / Edge / Opera だけです。**

このページはブラウザの USB の口を直に使います。使う API は 2 つ。

- Web Serial API — https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API
- WebHID API — https://developer.mozilla.org/en-US/docs/Web/API/WebHID_API

どちらも対応しているブラウザが限られています。

| ブラウザ | Web Serial | WebHID |
|---|---|---|
| Chrome / Edge / Opera (パソコン) | ○ | ○ |
| Firefox | △ 既定では無効。アドオンによる有効化が必要 | × 仕様に反対の立場で、対応の予定なし |
| Safari (Mac / iPhone / iPad) | × WebKit が仕様に反対の立場を取っており、対応の予定なし | × 同上 |
| Chrome (Android) | × Android 側に有線シリアルの仕組みが無い | × 同じく使えない |

対応していないブラウザで開いた場合は、その旨がページ上部に出ます。
文面は**選んでいる接続方法に合わせて**変わります。

### 接続方法の選び方

「接続」欄の **接続方法** で選びます。

| 選択 | 動き |
|---|---|
| 自動 (既定) | すでに許可済みのデバイスがあるほうを使う (同点なら USB シリアル)。どちらも未許可なら、使えるほうのうち USB シリアルを先に試す |
| USB シリアル (dev プロファイル) | Web Serial だけを使う |
| USB HID (full プロファイル) | WebHID だけを使う |

一度許可したデバイスは、次からダイアログなしで開き直します。
許可がまだ無いときだけ、ブラウザのデバイス選択ダイアログが出ます
(ダイアログはボタンを押したその場でしか出せないため、
許可済みかどうかはページ読み込み時と切断時に調べてあります)。

ページ自身が外へ通信しないことは CSP (`connect-src 'none'`) で縛っています。
Web Serial も WebHID も `connect-src` の対象外なので、この縛りは USB のやりとりを妨げません。
どちらを許すかは HTTP ヘッダの Permissions-Policy (`serial` / `hid`) が決めますが、
**既定の allowlist が self** なので、自分のオリジンで開くかぎり何も書かずに使えます
(GitHub Pages ではヘッダを書けないので、そもそも書けません)。

---

## デバイス側に必要なもの

stackee のファームウェアに **`stackee_console.py` (コンソールモジュール)** が
入っている必要があります。これが入っていないと、ログは見えますが
状態の取得も設定の書き込みもできません (「デバイスから応答がありません」と出ます)。

ファームウェアが古くて一部のコマンドに対応していない場合は、
その機能だけが「対応していません」という表示になり、他の機能は使えます。
たとえば Wi-Fi スキャンに対応していないファームでは、スキャンボタンが消えて
SSID の手入力だけになります。

### 複数 Wi-Fi に対応しているファームかどうか

接続すると最初に `hello` を送ります。**その応答の `features` に
`wifi.list` が入っているかどうか**で、「Wi-Fi ネットワーク」の欄を出すか決めます。

| デバイス | ページの見え方 |
|---|---|
| `proto: 2` / `features` に `wifi.list` あり | 一覧・追加・削除がすべて使える |
| `proto: 1` (`features` を返さない) | 「**旧ファーム: 複数 Wi-Fi 非対応**」とだけ出て、欄の中身は出ない |

★ **判断に使うのは版番号ではなく `features`** です。
版番号だけを見ると、`proto` は 2 でも Wi-Fi を切ってあるビルド
(`allow_wifi=False`) を取りこぼします。

---

## 使い方

1. stackee をパソコンに USB でつなぐ
2. https://takashi.company/stackee/ を Chrome か Edge で開く
3. 「接続する」を押して、出てきた一覧から **M5Stack Core S3** を選ぶ
4. Wi-Fi は「スキャンして選ぶ」→ パスワードを入れて「追加・更新」
   (最大 8 件。同じ SSID なら上書き)
5. 音声サーバは入力して「保存する」→「再起動する」

### ローカルで動かす

**`file://` では動きません。** Web Serial は secure context (https: か
`localhost`) でしか使えず、`file://` では `navigator.serial` がそもそも存在しません。

`docs/` を配信する簡易サーバを立てて `http://localhost:8000/` を開いてください。

```sh
cd <このリポジトリ>
python3 -m http.server 8000 --directory docs
```

ビルド手順はありません。`docs/index.html` と `docs/js/*.js` を直接編集すれば、
リロードするだけで反映されます。

---

## GitHub Pages への公開

公開リポジトリ `takashicompany/stackee` の **Settings → Pages** で、
**Source: Deploy from a branch**、**Branch: main**、**Folder: /docs** を選びます。
`main` へ push すると `docs/` の中身がそのまま配信されます。
GitHub Actions のワークフローは使いません。

URL は https://takashi.company/stackee/ です。
(アカウントに独自ドメインを設定しているためで、
https://takashicompany.github.io/stackee/ はこちらへ転送されます。)

`docs/.nojekyll` は Jekyll の処理を止めるためのものなので消さないでください。

`server/`、`firmware/`、テスト、README は `docs/` の外なので配信されません。
サーバーは Mac などのホストで別途実行する想定です。

参考: [GitHub 公式のブランチ配信手順](https://docs.github.com/en/pages/getting-started-with-github-pages/configuring-a-publishing-source-for-your-github-pages-site)

---

## プライバシー

- **操作盤はブラウザだけで動きます。** GitHub Pages が `docs/` の静的ファイルを
  配っているだけです。`server/` は Pages の配信対象に含めません。
- **入力した内容はどこにも送信されません。** Wi-Fi のパスワードは
  「ブラウザ → USB ケーブル → デバイス」の 1 経路しか通りません。
- **ブラウザにも保存しません。** `localStorage` / `sessionStorage` / Cookie を
  一切使いません。「追加・更新」を押したあとは入力欄も空にします。
- **パスワードは 1 度も画面に出ません。** 登録済み Wi-Fi の一覧
  (`wifi.list`) にパスワードは**入りません**。デバイスが返すのは
  `has_password` (真偽値) だけで、ページはそれを「設定済み / なし」と訳して出します。
  仮に値が入って返ってきても、ページの読み取り側 (`readNetworkList`) が捨てます。
- `settings.get` も同じで、デバイスは **設定済みのキーしか返しません** —
  まだ設定していない項目は空欄として表示され、エラーにはなりません。
- **外部への通信を、ページ自身の記述で禁じています。**
  `index.html` に次の CSP を書いてあります。

  ```
  default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:;
  font-src 'self'; connect-src 'none'; object-src 'none'; base-uri 'none'; form-action 'none'
  ```

  `connect-src 'none'` が止めるのは
  `fetch()` / `fetchLater()` / `XMLHttpRequest` / `WebSocket` / `EventSource` /
  `navigator.sendBeacon()` / `<a ping>` の 7 つです
  ([MDN: connect-src](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Content-Security-Policy/connect-src))。

  **Web Serial はこの一覧に入っていないので、この CSP に妨げられません。**
  シリアルを制御するのは CSP ではなく `Permissions-Policy` の `serial`
  ディレクティブで、その既定の allowlist は `self` です
  ([MDN: Permissions-Policy: serial](https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Permissions-Policy/serial))。
  つまり自分のオリジンでは何も書かなくても使えます。

  なお `<meta>` で書く CSP では `frame-ancestors` / `report-uri` / `sandbox` は
  無視されるため、他サイトからの iframe 埋め込みはこの記述では防げません。

- 外部の CDN・フォント・解析タグを 1 つも読み込みません。npm パッケージも
  使いません。リポジトリの中のファイルがすべてです。
- リポジトリに秘密は入っていません。デバイスの識別に使うのは
  USB の VID / PID (`303A` / `811A` = M5Stack CoreS3 共通の値) だけで、
  個体のシリアル番号は含みません。

---

## ファイル構成

```
.
├── docs/                     GitHub Pages の配信対象 (Branch: main / Folder: /docs)
│   ├── index.html            操作盤
│   ├── style.css             見た目
│   ├── js/
│   │   ├── protocol.js       プロトコル処理
│   │   ├── serial.js         Web Serial の制御 (dev プロファイル)
│   │   ├── hid.js            WebHID (Raw HID) の制御 (full プロファイル)
│   │   └── app.js            画面とイベント
│   └── .nojekyll
├── server/                   Mac / Linux 音声受信サーバーと常駐 Codex エージェント
├── firmware/                 本体の C ファームウェア (ESP-IDF)。**GPL-2.0-or-later**
│   ├── main/                 ファーム本体
│   ├── third_party/qmk/      QMK 0.34.4 のコピー (無改変)
│   ├── tools/                Mac 側の道具とホストテスト (実機に触らない)
│   ├── assets/               本体へ送る素材 (顔・アイコン・フォント・音声)
│   ├── build.sh              ビルド (ESP-IDF v6.0.3 は別途用意する)
│   └── README.md             使い方。設計は DESIGN.md、実測は RESULTS.md
├── test/
│   ├── protocol.test.mjs     docs/js/protocol.js の単体テスト
│   └── hid.test.mjs          docs/js/hid.js の単体テスト
├── package.json              Node 用の ES モジュール指定 (依存なし)
└── README.md
```

### プロトコル定数の置き場所

デバイスとやりとりする **合図はすべて `docs/js/protocol.js` の先頭にまとめて** あります
(`PROTOCOL` / `CMD` / `ERR` / `SETTING_KEYS`)。
ほかのファイルはそこから読むだけで、生の値を書いていません。

デバイス側のコンソールモジュール (`stackee_console.py`) と食い違いが出たら、
**まずこの 1 か所を直します。**

やりとりは 1 行の JSON です。ログや画面のステータスバーと同じ 1 本の
シリアルを共有するので、行頭に印字されない目印 `0x1E` を必ず付けます。

```
ページ  → デバイス   \x1e{"id":7,"cmd":"status"}\n
デバイス → ページ    \x1e{"id":7,"bat":83,"wifi":"up","ip":"192.168.1.42", ...}\n
```

USB HID でつないだ場合も、**この行そのものは 1 バイトも変わりません。**
32 バイトのレポートに 29 バイトずつ詰めて運ぶだけです
(先頭 3 バイトが運搬用の枠、残り 29 バイトが上の行の中身)。
だから `protocol.js` から上は接続方法を知りません。

### コマンド一覧 (proto 2)

| コマンド | 送るもの | 返るもの |
|---|---|---|
| `hello` | — | `{"proto":2,"fw":..,"cp":..,"board":..,"features":["wifi.list","wifi.add","wifi.remove","wifi.scan"]}` |
| `status` | — | 電池・送信先・BLE・Wi-Fi・稼働時間 |
| `settings.get` | — | `{"keys":{..},"secret":[..],"bytes":N}` (パスワードの値は返らない) |
| `settings.set` | `kv` (`STACKEE_HOST` / `STACKEE_PORT` **のみ**) | `{"ok":1,..}` |
| `wifi.list` | — | `{"networks":[{"ssid":..,"channel":10,"has_password":true},..],"n":N}` |
| `wifi.add` | `ssid` / `password` / `channel` | `{"ok":1,"n":N}` |
| `wifi.remove` | `ssid` | `{"ok":1,"n":N}` |
| `wifi.scan` | — | `{"ok":1,"nets":[{"ssid":..,"ch":..,"rssi":..}],..}` |
| `reset` | — | `{"ok":1,"in_ms":300}` |

**`wifi.add` の引数の約束。**

- `ssid` — 文字列。空でなく UTF-8 で 32 バイトまで。**同じ SSID があれば上書き**
- `password` — 文字列。**空文字は「暗号なしのネットワーク」** の意味。
  ページは空でも必ずこのキーを送ります (省くと「今の値を保つ」と区別できないため)
- `channel` — 1〜14 の整数か `null`。`null` は自動

**`settings.set` は Wi-Fi を受け付けません。** proto 2 のデバイスは
`STACKEE_WIFI_SSID` / `STACKEE_WIFI_PASSWORD` / `STACKEE_WIFI_CHANNEL` を
`denied:<KEY>` で拒否します。ページもこの 3 つを送りません。

**失敗コード** (`error` キー): `full` (8 件を超えた) / `bad_ssid` /
`bad_password` / `bad_channel` / `write_failed` / `not_found` (`wifi.remove`)。
ページはこれを日本語に訳して出します
(`protocol.js` の `errorText`)。

> **8 件の上限をページ側で弾かないのは意図的です。**
> ページが持っている一覧は古いことがあるので、数の判定はデバイス
> (`full`) だけが正しいと決めています。

### デバイス側のファイル

| ファイル | 中身 | 書くコマンド |
|---|---|---|
| `/wifi_networks.json` | 登録した Wi-Fi (最大 8 件)。SSID・チャネル・パスワード | `wifi.add` / `wifi.remove` |
| `/settings.toml` | `STACKEE_HOST` / `STACKEE_PORT` ほか | `settings.set` |

`/wifi_networks.json` の中身のうち、**パスワードだけは読み出す手段がありません。**
`wifi.list` は `has_password` (真偽値) しか返しません。

- 引数は入れ子にせず、要求の直下に置きます (`{"id":1,"cmd":"settings.set","kv":{...}}`)。
- **成功か失敗かは `error` キーの有無で決まります。** 成功時に `"ok":1` を返すのは
  一部のコマンドだけなので、`ok` の有無で判定してはいけません。
- 値に制御文字が混ざらないよう、非 ASCII は `\uXXXX` に逃がして送ります。
  とくに `0x03` (Ctrl-C) が生で通ると、デバイス側の USB が受信バッファごと
  中身を捨ててしまいます。
- 「再起動」はチップ全体の再起動 (`microcontroller.reset()`) だけです。
  同じ USB の VID / PID で戻ってくるので、ページはユーザ操作なしに開き直せます。

---

## 実測値 (2026-09-10 / 実機 M5Stack CoreS3 / CircuitPython 10.3.0 / BLE 接続中)

デバイス側の計測結果です。ページのタイムアウトや待ち時間はこの数字に合わせてあります。

| 操作 | 実測 | ページ側の設定 |
|---|---|---|
| `hello` | 往復 11〜15 ms | タイムアウト 3 秒 |
| `status` | 往復 13〜26 ms | タイムアウト 3 秒 / 自動更新 10 秒ごと |
| `settings.get` | 往復 22〜24 ms | タイムアウト 5 秒 |
| `settings.set` | **往復 243 ms** (デバイス内 224 ms) | **タイムアウト 10 秒** |
| `wifi.scan` (全 13 チャネル) | **5.09 秒** | タイムアウト 25 秒 |
| `wifi.list` | 未実測 (ファイルを読むだけ) | タイムアウト 5 秒 |
| `wifi.add` / `wifi.remove` | 未実測。約 250 ms 見込み (`settings.set` と同じ書き方) | **タイムアウト 10 秒** |

**`settings.set` に 10 秒とっている理由。** 書き込み自体は 224 ms
(フラッシュ書き + 読み戻し照合 + `os.rename`) ですが、その 224 ms のあいだ
キースキャンが止まるため、デバイスは**打鍵が 300 ms 途切れるのを待ってから**
書き込みます。待ちすぎないよう 3 秒で諦めて実行します。
つまり打ちながら保存すると最大 3.3 秒ほどかかります。
ページはその間「保存中…」と出し、他のボタンを押せなくします。

**`wifi.scan` が 5 秒かかる理由。** 13 チャネルを 1 つずつ走査し、
1〜11ch は 300 ms、12ch 以上はパッシブ走査なので 800 ms 置いてから結果を吸います
(短くすると 1 回の走査で 236〜420 ms もキースキャンが止まります)。
**この 5 秒のあいだもキーボードは止まりません** — 1 回のスキャンあたりの
最大停止は 7.57 ms でした。

**`wifi.add` / `wifi.remove` も 10 秒とっている理由。** `settings.set` と
同じフラッシュ書き込みなので、同じ「打鍵が 300 ms 途切れるのを待つ」
仕組みに乗ります。打ちながら登録すると最大 3.3 秒ほどかかります。

**★ `wifi.scan` / `settings.set` / `wifi.add` / `wifi.remove` の実行中、
デバイスは他のコマンドを読みません。**
ページはこの間、自動更新を止め、他のボタン (一覧の「削除」を含む) も
押せなくします。

### 再起動と再接続

`reset` は `{"ok":1,"in_ms":300}` を返してから 300 ms 後に落ちます。
そこから復帰までの実測 (応答を受け取った時刻を 0 秒とする):

| 出来事 | 実測 |
|---|---|
| ポートが消える | 1.69 s |
| 同じ名前で再び現れる | 5.23 s |
| `open()` が成功する | 5.25 s |
| `status` が返る | **8.51 s** |

ページはこの順番どおりに待ちます。

1. **まずポートが消えるのを待ちます。** 消える前に開くと、まだ生きている
   古いハンドルを掴んで「繋がったのに無反応」になります。
2. **次にポートが現れるのを待ちます** (`navigator.serial.getPorts()` を
   0.3 秒ごとに見る)。★ **現れる前に `open()` しにいきません。**
   macOS の USB 列挙が壊れて 20 分戻らなくなった記録があるためです。
3. 開けたあともデバイスは 3 秒ほど起動中で答えません。
   `hello` を 1.5 秒のタイムアウトで最大 6 回、0.5 秒間隔で叩き直します。

全体で最大 30 秒まで待ち、残り秒数を画面に出します。
30 秒で戻らなければ、USB を挿し直して「接続する」を押し直してもらいます。

### 接続時の自己確認

接続すると `hello` を送り、次の 3 つを行います。

1. `features` を見て「Wi-Fi ネットワーク」の欄を出すか決める
   (無ければ「旧ファーム: 複数 Wi-Fi 非対応」と出して欄を畳む)
2. デバイスの `proto` がページの想定 (2) **より新しければ**、接続欄に
   「このページが古い」と注意を出す。動作自体は止めません
3. `status` → `settings.get` → `wifi.list` の順に読む
   (`wifi.list` は対応しているときだけ)

---

## テスト

デバイスもブラウザも要りません。Node 18 以降で動きます (確認したのは v22.22.2)。

```sh
cd <このリポジトリ>
node --test test/                # Node 18 / 20
node --test 'test/**/*.mjs'      # Node 22 以降 (位置引数が glob になったため)
node --test                      # 引数なしでも自動で見つかる (どの版でも可)
```

見ているのは、デバイスとの間で崩れると気づきにくいところです。

- 要求行の枠 (`\x1e` … `\n`) と、行に制御文字が絶対に混ざらないこと
- ステータスバーの制御列が受信の切れ目で割れても、本文にもデータにも漏れないこと
- 要求 ID の突き合わせと、応答が来なかったときの打ち切り
- 設定値の検証 (ポート番号・チャネル・SSID の長さ・パスワードの長さ)
- `wifi.add` / `wifi.remove` の要求の形
  (チャネルが整数か `null` になること、空パスワードを省かないこと)
- `wifi.list` の読み取りで**パスワードが絶対に外に出ないこと**
- `features` に `wifi.list` が無いデバイス (proto 1) で機能を隠すこと
- `settings.set` に Wi-Fi のキーを混ぜないこと (`denied` を踏まないため)
- Raw HID の 32 バイトのレポートに本文を 29 バイトずつ詰める割り方と、その読み取り
  (壊れた `len` を弾くこと、割って繋ぎ直すと元のバイト列に戻ること、
  多バイト文字がレポートの切れ目で割れても化けないこと)
- どちらの接続方法を使うかの決め方 (`chooseTransport`)

---

## ライセンス

**`firmware/` だけが GPL-2.0-or-later** です。キーボードの処理に
QMK (GPL-2.0-or-later) のコードを `firmware/third_party/qmk/` へ取り込んで
一緒にビルドしているため、その派生物として同じ条件で配っています。
全文は [firmware/LICENSE](firmware/LICENSE)、取り込みの範囲は
[firmware/third_party/qmk/IMPORT.md](firmware/third_party/qmk/IMPORT.md)。

**それ以外 (`docs/` の操作盤、`server/`、`test/`) は従来どおり**
stackee プロジェクトのコードで、外部ライブラリは 1 つも同梱していません。

本体へ送る素材 (`firmware/assets/`) は、それぞれの出所の条件に従います
(Material Design Icons は Apache-2.0、/efont/ は BSD 系、東雲フォントは
Public Domain)。表記は [firmware/assets/README.md](firmware/assets/README.md)。
