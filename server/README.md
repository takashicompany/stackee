# stackee 音声サーバー (Mac / Linux)

同じ Wi-Fi の CoreS3 から音声を受け取り、ローカルの `whisper-cli` で文字起こし、
会話エージェント (Codex または Claude) で返答生成、macOS の `say` または VOICEVOX ENGINE で音声合成します。
返答も HTTP で S3 に返すため、マイク・スピーカーとも USB 音声転送は不要です。
返答のピーク音量はフルスケールの25%以下に抑えます。小さい音は増幅しません。

## 起動

Python 3.10 以降、`whisper-cli`、Whisper のモデルファイル、ログイン済みの会話エージェント
(Codex CLI または Claude Code のどちらか、選んだ方)、音声合成エンジンが必要です。Mac は `say` (Kyoko)、Linux は VOICEVOX が既定です。
Python の外部パッケージは不要です。

サーバーはリポジトリを clone して起動します。単独ファイルのコピーでは起動しません。

```sh
mkdir -p ~/works
git clone git@github.com:takashicompany/stackee.git ~/works/stackee
cd ~/works/stackee
```

既に clone 済みなら `git pull --ff-only` で更新します。リポジトリのルートから:

```sh
python3 server/stackee_server.py --whisper-model /path/to/ggml-model.bin
```

開発用の親リポジトリから:

```sh
python3 public/server/stackee_server.py \
  --whisper-model firmware/kmk/tools/models/ggml-large-v3-turbo-q5_0.bin
```

既定では `0.0.0.0:8766` で待ち受けます。終了は Ctrl-C。
`--port`、`--host`、`--codex-model`、`--voice` で変更できます。
`--echo` はエージェントを呼ばず、文字起こし結果をそのまま読み上げる疎通試験です。
ブラウザの管理画面は `http://<サーバー>:8766/admin` です (例: `http://192.168.0.128:8766/admin`)。

### Linux / VOICEVOX

[VOICEVOX ENGINE](https://github.com/VOICEVOX/voicevox_engine) の Linux NVIDIA 版を
ダウンロード・展開し、GPU を有効にして起動します。CPU 版も同じ API で使えます。

```sh
./run --use_gpu --host 127.0.0.1 --port 50021
```

Docker と NVIDIA Container Toolkit が導入済みなら、公式イメージも使えます。

```sh
docker run --rm --gpus all -p 127.0.0.1:50021:50021 \
  voicevox/voicevox_engine:nvidia-latest
```

エンジンの起動後、別のターミナルで:

```sh
python3 server/stackee_server.py \
  --whisper-model /path/to/ggml-model.bin \
  --tts voicevox --voicevox-url http://127.0.0.1:50021 --speaker 3
```

`--whisper`、`--codex`、`--claude` で実行ファイルの絶対パスを指定できます。
`--speaker` は `/speakers` にあるスタイル ID で、既定の 3 は「ずんだもん（ノーマル）」です。
音声利用時は [VOICEVOX](https://voicevox.hiroshiba.jp/term/) と選択した音声の利用条件に従ってください。
クレジット例: `VOICEVOX:ずんだもん`。起動時に選択した音声モデルを初期化します。
`--voice` は `say` 用の設定です。Mac でも `--tts voicevox` を指定できます。

VOICEVOX へ 16kHz・モノラルを指定して合成し、返却 WAV の形式・長さを検証してから
音量制限を適用します。S3 に返す PCM と HTTP API は両方式で共通です。

#### ログイン後の自動起動

`stackee-voicevox.service` と `stackee-talk.service` は systemd のユーザーサービス例です。
リポジトリを `~/works/stackee`、VOICEVOX 0.25.2 を
`~/.local/share/stackee/voicevox-0.25.2` に置く構成になっています。
Whisper は `~/.local/share/stackee/whisper.cpp/build-vulkan/bin/whisper-cli`、
モデルは `~/.local/share/stackee/models/ggml-large-v3-turbo-q5_0.bin` を参照します。
配置が違う場合はサービスファイルを編集してください。
別のサービスが他のインターフェースで同じポートを使っている場合などは、
`~/.config/stackee-talk.env` に `STACKEE_BIND_HOST=192.168.1.10` のように
サーバーの LAN アドレスを指定し、会話サーバーを再起動できます。

Whisper の GPU 利用には [whisper.cpp](https://github.com/ggml-org/whisper.cpp) を
`-DGGML_VULKAN=ON` でビルドする方法があります。Ubuntu では `cmake`、C/C++ コンパイラ、
`libvulkan-dev`、`glslc`、`spirv-headers` が必要です。VOICEVOX の GPU 利用とは独立した設定です。

```sh
mkdir -p ~/.config/systemd/user
cp server/stackee-*.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now stackee-voicevox.service stackee-talk.service
```

起動直後は VOICEVOX の準備完了まで会話サーバーが再試行します。
状態・ログ・停止:

```sh
systemctl --user status stackee-voicevox stackee-talk
journalctl --user -u stackee-voicevox -u stackee-talk -n 50
systemctl --user disable --now stackee-talk stackee-voicevox
```

### 固定のエージェントと会話の継続

エージェントは clone したリポジトリの **`server/agent/`** を作業ディレクトリとして動きます。
音声変換用の一時フォルダとは別です。種別は `server/agent/agent.json` の `agent` で選びます。

- `codex`: `codex app-server --listen stdio://` を常駐させ、同じプロセス・同じ会話スレッドに
  発言を追加します。指示文は `server/agent/AGENTS.md` を開発者指示として渡します。
- `claude`: 1発話ごとに `claude -p` を起動し、保存した会話 ID を `--resume` で継続します。
  指示文は `server/agent/CLAUDE.md` で、Claude Code が作業ディレクトリから読み込みます。
  使える道具は Web 検索 (`WebSearch`) と Web 取得 (`WebFetch`) の2つだけで、
  ファイル操作・コマンド実行・MCP は使えません。権限モードは plan、
  設定ソースは project のみ (実行ユーザーの `~/.claude/CLAUDE.md` は読みません)。

**会話は種別ごとに別々です。** 切り替えて戻すと、それぞれ前の続きから再開します。

#### 既定値と実行時ファイル

管理画面から編集するファイルは git 管理外です。`git pull --ff-only` と衝突しません。

| 場所 | 役割 |
| --- | --- |
| `server/agent/defaults/AGENTS.md` / `CLAUDE.md` / `agent.json` | Git 追跡。出荷時の既定値 |
| `server/agent/AGENTS.md` / `CLAUDE.md` / `agent.json` | 実行時ファイル。Git 管理外 |
| `server/agent/.state/session.json` | 継続する会話の ID。Git 管理外 |
| `server/agent/defaults/keys.json` / `server/agent/keys.json` | 独自キー CSTM_0〜9 の設定 (既定は全キー未設定)。実行時ファイルは Git 管理外 |

サーバー起動時、実行時ファイルが無ければ `defaults/` からコピーします。あれば触りません。
初回配置や、誤って消した場合はサービスを再起動すれば復元されます。

`agent.json` の形式:

```json
{
  "agent": "codex",
  "codex": {"model": "gpt-6-astra", "effort": "low"},
  "claude": {"model": "sonnet", "effort": "low"}
}
```

思考量は Codex が `none` `minimal` `low` `medium` `high` `xhigh` `max` `ultra`、
Claude が `low` `medium` `high` `xhigh` `max`。モデル名は空でなければ自由に指定できます。
旧形式 `{"model": ..., "effort": ...}` は Codex 設定として読み込みます。
`session.json` も旧形式 `{"thread_id": ...}` を Codex の会話として読み込みます。

#### 管理画面

`http://<サーバー>:8766/admin` をブラウザで開きます。認証はありません。
LAN と Tailscale からしか届かない前提の設定画面です。外部の CDN やフォントは使いません。

- 現在の状態 (種別・稼働状況・会話 ID・モデル・思考量・処理中かどうか) を10秒ごとに表示します。
- エージェント種別、モデル、思考量を切り替えます。
- 指示文 (`AGENTS.md` / `CLAUDE.md`) をタブで切り替えて編集します。
- 各種別の会話 ID の横の**会話をリセット**は、その種別の会話だけを捨てます
  (もう一方の会話 ID は残ります)。押すと 2 段階の確認が同じ場所に出ます。
  リセット後、次の発話から記憶のない新しい会話が始まります。
  処理中の会話があれば終わるのを待ってから実行します。
  Codex 側の古いスレッド本体は `~/.codex` に残りますが、サーバーからは参照しなくなります。
- **保存**はファイルに書くだけで、動作中の会話には反映しません。
  **保存して反映**は保存後に会話プロセスを再起動し、新しい設定と指示文を読み込みます。
  反映は処理中の会話が終わるのを待ってから行います。モデル名などが誤っていればその場で失敗を表示し、
  バックエンドは停止したままになります (次の発話で再試行します)。

| メソッド・パス | 内容 |
| --- | --- |
| `GET /admin` | 管理画面の HTML |
| `GET /admin/api/state` | 設定・指示文・状態・選択肢 |
| `PUT /admin/api/config` | `agent.json` を保存 (反映はしない) |
| `PUT /admin/api/instructions/{codex,claude}` | 指示文を保存 (反映はしない) |
| `POST /admin/api/apply` | 保存済みの設定でバックエンドを再起動 |
| `POST /admin/api/conversation/{codex,claude}/reset` | その種別の会話だけを破棄 |
| `PUT /admin/api/keys` | 独自キーの設定 `keys.json` を保存 (**即反映**。次の押下から使う) |

書き込み系は `X-Stackee-Admin: 1` ヘッダーが必要で、`Origin` があれば `Host` と一致する場合だけ通します。
本文は 64KB まで。`/talk` と `/jobs` の API は従来どおりで、管理画面の追加による変更はありません。

#### 会話の保全

サーバー再起動時は保存した ID の会話を再開します。直近4往復の自前履歴は廃止し、
エージェント側の会話保存・コンテキスト圧縮に任せます。保存先が壊れたり会話が見つからない場合は
エラーにし、黙って記憶のない別の会話を作りません。旧方式の RAM 履歴は移行できません。
会話本体は実行ユーザーのエージェントの保存領域 (Codex は通常 `~/.codex`、
Claude Code は `~/.claude/projects/<作業ディレクトリ名>/`) に保持します。

コマンドラインからの上書きもできます。`--codex-model` は Codex のモデル設定を上書きし、
`--agent-dir` はエージェントの配置を変更します。`--codex` / `--claude` は実行ファイルのパスです。
同じエージェントフォルダを複数サーバーが同時に使うことはロックで防ぎます。
`/health` の `agent` で種別、`conversation` で cwd・PID・会話 ID・モデルを確認できます。

Codex は既存のログインと設定を使い、音声エージェントには読み取り専用サンドボックスと
非対話の承認ポリシーを指定します。Claude Code は Web 検索と Web 取得だけを許可し、
権限モードは plan なので承認待ちで止まりません。
エージェントには URL・ドメイン名・出典リンクを含めないよう指示します。
生成後にも URL と引用マーカーを除去し、Markdown リンクは表示名だけを残します。
除去は長さ調整の前に行い、音声合成・画面へ返す回答に適用します。
エージェント自身の履歴には生成した元の返答が保存されるため、返答指示でも URL を禁止します。
URL しかなく本文が残らない場合は、回答をまとめられなかった旨の短い文に置き換えます。

## S3 側

`settings.toml` の `STACKEE_TALK_URL` に接続先を指定して再起動します。
例 (IP アドレスはサーバーを動かす Mac に合わせる):

```toml
STACKEE_TALK_URL = "http://192.168.1.10:8766/talk"
```

中央の親指キー42 (`KC.TALK`) を押している間に録音し、離すと送信します。
0.3秒未満は捨て、最長30秒。処理・再生中の新しい押下は受け付けません。
USB マイクが有効なら録音中だけ停止し、録音終了後に再開します。
録音は1スキャンにつき15ms分ずつ取得し、HTTP転送も小分けで進めます。
接続確立だけは CircuitPython の制約で最大1秒待つため、その間はキー走査が止まります。

URL の Web 操作盤からの編集は今後追加します。現在は S3 の設定へ直接書き込みます。
既存の `STACKEE_HOST` / `STACKEE_PORT` (TCP音声・カメラ用) とは別設定です。

## API

| メソッド・パス | 内容 |
| --- | --- |
| `GET /health` | 稼働状態・処理中かどうか |
| `POST /talk` | `Content-Type: audio/wav` と `Content-Length` を付け、WAV 本体を送る |
| `POST /look` | `Content-Type: image/jpeg` と `Content-Length` を付け、カメラの JPEG 本体を送る (下記「写真を見せる」) |
| `GET /jobs/{id}` | `processing` / `done` / `ignored` / `error` と結果を取得 |
| `GET /jobs/{id}/audio` | 返答の生 PCM (16kHz、符号付き16bit LE、mono) を取得 |
| `GET /jobs/{id}/subtitles` | 返答の字幕 (開始ミリ秒とページ本文) を取得 |
| `GET /admin` ほか | 管理画面と設定 API (上記「管理画面」を参照) |
| `POST /key` | 独自キー CSTM_0〜9 の押下 (下記「独自キーと受け箱」) |
| `GET /inbox` | 受け箱。本体が取りに来る発話 (下記「独自キーと受け箱」) |
| `GET /inbox/{seq}/audio` | 受け箱の発話の生 PCM |
| `POST /say` | 受け箱に発話を積む。**このマシンからだけ** (`stackee-say` が使う) |

入力 WAV は非圧縮・16kHz・16bit・mono、0.3〜30秒。返答音声は最長120秒です。

返答は**文ごとに合成して連結**します。句点 (。．！？) と改行で文に分け、1文ずつ合成し、
文の間に150ミリ秒の無音を挟んでつなぎます。合計が120秒を超える手前で止め、以降の文は落とします
(文の途中では切りません)。1文目だけで超える場合は、その文を読点 (、) で区切って同じ手順で詰めます。
音量制限は連結後の全体に1回だけかけます。短縮したときは `reply-shortened` をログに出します。

まとめて合成しない理由は2つあります。VOICEVOX は長い文章 (実測で約500文字以上) の合成に
HTTP 500 で失敗すること、そして1回の長い合成で GPU の確保量が大きく膨らみ、
同じ GPU を使う whisper が動かなくなることです。それでも1文の合成が失敗した場合は、
その文を読点で分けて再試行します。一度も合成できなかった場合は、その失敗をそのままエラーとして返します。
受付時は `202` と JSON `{id, status_url}`、同じURLを `Location` ヘッダーにも返します。
`done` には `transcript`、`reply`、`audio_url`、`audio_bytes`、
`sample_rate`、`channels`、`sample_width` が入ります。字幕があるときは `subtitles`
(本文そのもの) と `subtitles_url` も入ります。1秒間隔のポーリングを想定します。

### 字幕

`GET /jobs/{id}/subtitles` は `text/plain; charset=utf-8` で、1行が
`<開始ミリ秒>\t<本文>` (改行区切り) です。開始ミリ秒は10進の整数で、先頭行は 0、
以降は単調非減少です。本文はタブと改行を含まない UTF-8 で、1行の表示幅は15桁まで
(全角1桁・半角0.5桁) です。行数は48行、本文は4096バイトまでで、超える分は末尾の行を
落とします (音声は落としません)。

区切りと時刻はサーバーが決めます。文ごとに合成しているため、各文の開始時刻は連結後の
PCM のバイト数から厳密に求まります (無音の150ミリ秒を含む)。文の中は読点と句点で節に
分け、15桁を超える節は、収まる最小のページ数で幅が均等になるように分けます
(例: 17桁の節は8桁と9桁の2ページ)。文の中の各ページの開始時刻は、文の長さを文字数で
按分した値です。行頭に句読点や閉じ括弧が来る分け方は避け、前のページに付けます。

字幕は `done` の応答にも `subtitles` として同じ本文が入ります (タブと改行は JSON の
エスケープになります)。本体は状態の取得だけで字幕を受け取れます。別途 GET すると
本体では接続のやり直しに数秒かかるためです。`GET /jobs/{id}/subtitles` は
デバッグ用と旧デバイス向けに残してあり、内容は `subtitles` と常に同一です。

`done` の応答全体は8192バイト以内に収めます。返答が長く収まらない場合は、
字幕の末尾の行を落として収めます (`/subtitles` も同じ内容になります)。
落としきっても収まらない場合は `subtitles` と `subtitles_url` の両方を外します。

処理中は `409`、`ignored` と `error` のジョブ、字幕が空のジョブは `404` です。
`subtitles` も `subtitles_url` も無ければ字幕なしで従来どおり動作します。

同時処理は1件 (`/look`・`/key` と共有) で、処理中の追加送信は `409`。形式不正は `400`、サイズ超過は `413`、
MIME不一致は `415`。結果は最大8件・5分間、メモリだけに保持します。
音声の一時ファイルは処理終了時に削除します。文字起こし結果はエージェントに送信されます。

### 写真を見せる (`POST /look`)

本体のカメラで撮った JPEG を送ると、エージェントが写真を見て話しかけ、その返答を
`/talk` と同じジョブで返します。受付以降 (`202` と `{id, status_url}`・`Location`、
`GET /jobs/{id}`・`?wait=`・`/audio`・`/subtitles`、`done` の項目) は `/talk` と完全に同じです。
違いは入力だけで、文字起こしをしないため `transcript` は空文字、`timings` に `stt_ms` がありません。

| 項目 | 規則 |
| --- | --- |
| 本文 | JPEG 1枚。先頭が SOI (`FF D8`)、末尾が EOI (`FF D9`)。違えば `400` |
| 大きさ | 512 KiB (524288 バイト) まで。超過・空は `413` (`image_too_large_or_empty`) |
| MIME | `image/jpeg` のみ。違えば `415` (`expected_image_jpeg`) |
| 同時処理 | `/talk` と共有の1件。会話の処理中に `/look` が来れば `409`、逆も同じ |
| `Origin` 付き | `403` (`/talk` と同じく本体専用) |

エージェントへの問いは固定文で、`stackee_server.py` の `LOOK_PROMPT` にあります:

> ユーザーが stackee のカメラで今撮った写真です。何が写っているかを見て、短く話しかけてください。

写真は音声会話と**同じ会話** (Codex のスレッド / Claude のセッション) に入ります。
後の音声会話で「さっきの写真」と言えば通じます。返答は `/talk` と同じ経路で
URL 除去・文ごとの合成・長さ調整・音量制限・字幕化をしてから返します。
写真はファイルに書かず、メモリから直接エージェントへ渡します。

- Codex: `turn/start` の `input` に文字と並べて `{"type": "image", "url": "data:image/jpeg;base64,..."}`
  を入れます (app-server の `UserInput` の `image`。`codex app-server generate-json-schema` の
  `TurnStartParams` で確認)。読み取り専用サンドボックスに画像を読ませる必要はありません。
- Claude: 写真のときだけ `claude -p` に `--input-format stream-json` を付け、標準入力へ
  `{"type":"user","message":{"role":"user","content":[文字, {"type":"image","source":{"type":"base64","media_type":"image/jpeg","data":...}}]}}`
  を1行で渡します。CLI の制約で出力も `--output-format stream-json --verbose` になり、
  最後の `result` 行を従来の `json` 出力と同じように読みます。道具は Web 検索と Web 取得のままで、
  `Read` は許可しません。`--resume` で同じ会話を続けます。

写真はエージェントの会話履歴 (Codex は `~/.codex`、Claude Code は `~/.claude/projects/`) に残ります。
`--echo` のときはエージェントを呼ばず「写真を受け取りました。<n>キロバイトです。」と答えます。

## 独自キーと受け箱 (`POST /key`・`stackee-say`・`GET /inbox`)

本体の独自キー CSTM_0〜CSTM_9 を押すと、本体はサーバーに「CSTM_n が押された」とだけ伝えます。
何をするかはキーごとに管理画面で決めます。

| 方式 | 動き |
| --- | --- |
| 未設定 | 何もしない。サーバーのログに記録するだけ (`200 {"state":"ignored","key":"CSTM_3"}`) |
| プロンプト | 設定した文を会話エージェントの**同じ会話**に送り、返答を音声と字幕で返す。`/look` と同じジョブ (`transcript` は空) |
| コマンド | このサーバーでコマンドラインを実行する。喋らせる・字幕を出すかはコマンドが `stackee-say` で決める |

### キーの設定

管理画面の「独自キー」で、キーごとに方式・プロンプト文またはコマンドライン・時間切れ秒 (コマンドだけ、既定 300、
1〜600) を入れて「キー設定を保存」を押します。保存はすぐ反映され、次の押下から使われます
(会話プロセスの再起動は不要)。中身は `server/agent/keys.json`:

```json
{"CSTM_0": {"mode": "command", "prompt": "", "command": "stackee-say \"$(date +%H時%M分)です\"", "timeout": 300},
 "CSTM_1": {"mode": "prompt", "prompt": "今日の東京の天気を短く教えて", "command": "", "timeout": 300}}
```

`mode` は `none` / `prompt` / `command`。書いていないキーは未設定です。方式を変えても反対側の文は残ります。
認証はありません (LAN と Tailscale の中だけの前提)。コマンドはサーバーの実行ユーザーの権限で動くので、
管理画面に届く人は誰でもこのマシンでコマンドを実行できます。

### `POST /key`

`Content-Type: application/json`、本文 `{"key":"CSTM_3"}` (256 バイトまで)。`Origin` 付きは `403`。

| 応答 | 場合 |
| --- | --- |
| `200 {"state":"ignored","key":"CSTM_3"}` | 未設定のキー (処理中でもこちら) |
| `202 {id, status_url, mode:"prompt"}` | プロンプト方式。以降は `/look` と同じジョブ |
| `202 {id, status_url, mode:"command"}` | コマンド方式 |
| `409 {"error":"busy"}` | `/talk`・`/look`・`/key` のジョブが処理中 (同時処理 1 件の枠を共有) |
| `400` / `413` / `415` | CSTM_0〜9 以外・JSON 不正 / 大きさ / MIME |
| `500 {"error":"keys_unreadable"}` | `keys.json` が壊れている |

コマンド方式のジョブ:

- `/bin/sh -c '<コマンドライン>'` で実行します。作業ディレクトリは `server/agent/`。
  環境変数 `STACKEE_KEY=CSTM_3`、`STACKEE_SAY_URL` (このサーバーの `/say`) を足し、
  `PATH` の先頭に `server/bin/` (`stackee-say` の場所) を足します。標準入力は空です。
- 実行中は `processing`。**コマンドが終わるまでジョブの枠を握ります** (その間の `/talk` などは `409`)。
- 正常終了 → `done` で `{"state":"done","reply":"","exit_code":0}`。`audio_url` はありません
  (本体は何も鳴らさずに終わります。`/audio` と `/subtitles` は `404`)。
- 終了コード≠0 → `error` で `"CSTM_3 失敗 (終了コード 2)"`、シグナルで終了 → `"CSTM_3 失敗 (シグナル 9)"`、
  時間切れ → `"CSTM_3 時間切れ"`。時間切れのときはコマンドのプロセスグループごと止めます。
- 標準出力・標準エラーは一時ファイルに受け、それぞれ末尾 4 KB をログの `key-command` 行に残します。
- コマンドが裏で子プロセスを残して終わるのは自由です (`sleep 60 && stackee-say 終わった &` など)。
  パイプではなくファイルに受けているので、残った子がジョブを引き止めることはありません。
  子はジョブの後でも `stackee-say` できます。

### `stackee-say`

`server/bin/stackee-say` は Python 標準ライブラリだけのスクリプトです。

```sh
stackee-say "ビルドが終わりました"
stackee-say --no-voice "字幕だけ出す"
make 2>&1 | tail -1 | stackee-say      # 引数が無ければ標準入力を読む
```

送り先は `STACKEE_SAY_URL` (既定 `http://127.0.0.1:8766/say`、`--url` でも指定可)。キーのコマンドには
サーバーが起動時に自分の待ち受けアドレスから組み立てた URL を渡します (`0.0.0.0` なら `127.0.0.1`、
`STACKEE_BIND_HOST` で LAN アドレスに絞っていればそのアドレス)。キーと無関係なプロセス (cron など) から
使う場合で、サーバーを LAN アドレスに絞っているときは `STACKEE_SAY_URL` を自分で指定してください。
積めたら終了コード 0 で何も出力しません。失敗は 1 (使い方の誤りは 2) で、理由を標準エラーに出します。
合成が終わるまで待ちます (音声ありなら数秒)。

### `POST /say`

`stackee-say` が使う API です。**このマシンからだけ**受け付け、それ以外は `403 {"error":"local_only"}`。
「このマシン」は送り元が 127.0.0.1 / ::1 のとき、または送り元がサーバー自身の待ち受けアドレスと同じとき
(LAN アドレスに絞って待ち受けているとき、同じマシンのプロセスはそのアドレスから届くため) です。
中継 (pi400) には通しません。`Origin` 付きは `403`。

本文 `{"text":"...", "voice": true|false}` (`Content-Type: application/json`、`voice` 省略時 true、
`text` は UTF-8 で 4 KB まで)。応答 `200 {"seq":n}`。

- `voice: true`: 会話の返答と同じ後処理をします。URL 除去 → 文ごとの合成 → 120 秒の長さ制限 →
  音量制限 → 字幕 (文の開始時刻は PCM から厳密に)。合成の失敗は `500`。
- `voice: false`: 音声なし、字幕だけの発話を積みます。URL 除去と字幕のページ分け (15 桁・句読点の扱い・
  48 行 / 4096 バイト) は同じで、時刻は 0 から **2.5 秒ごと**に 1 ページずつ進めます。
- URL しかないなど、話す中身が残らない文は `400 {"error":"nothing_to_say"}`。
- 合成どうし (`/say` と会話の返答) は 1 つずつ順番に行います (エンジンが GPU を whisper と共有しているため)。
  これはジョブの枠とは別の錠なので、コマンドの実行中 (枠を握っている間) にも `/say` は使えます。

### 受け箱 (`GET /inbox`)

発話 1 件は `/talk` の `done` と同じ形で、`seq` (サーバー起動ごとに 1 から増える整数) が付きます。

```json
{"state": "say", "seq": 3, "reply": "ビルドが終わりました。",
 "audio_url": "/inbox/3/audio", "audio_bytes": 51200,
 "sample_rate": 16000, "channels": 1, "sample_width": 2,
 "subtitles": "0\tビルドが終わりました。\n"}
```

音声なし (字幕だけ) の発話には `audio_url` と `audio_bytes` がありません。`subtitles_url` は付けません
(字幕は `subtitles` にだけ入ります)。応答全体は `/jobs` と同じく 8192 バイト以内に収め、
収まらなければ字幕の末尾の行を落とします。

| 要求 | 応答 |
| --- | --- |
| `GET /inbox` (query なし) | 即 `{"state":"empty","seq":<最新 seq、無ければ 0>}`。起動時・利用開始時に「今より後」を知るため (前からあった発話は返さない) |
| `GET /inbox?after=<n>&wait=<秒>` | `seq > n` の**最も古い**発話があれば即 `{"state":"say",...}`。無ければ最大 `wait` 秒 (0〜25、超えた値は 25) 待ち、待ち切れたら `{"state":"empty","seq":<最新>}` |
| `…&job=<id>` を付ける | そのジョブが `processing` でなくなった時点でも返し、`"job_state":"done"` / `"error"` (と `"error"` の文) を足す。ジョブが無い・期限切れは `"job_state":"not_found"`。発話があってジョブも終わっていれば発話を返し、`job_state` も付ける |
| `GET /inbox/<seq>/audio` | 生 PCM (16kHz / 16bit LE / mono)。無い・期限切れ・音声なしの発話は `404` |

`after` は必須 (query を付けるとき)、`after`・`wait`・`job` 以外の名前や重複・形式違いは `400`。
本体はコマンド方式のキーを押した後、この 1 本のロングポーリング (`after=<最後に再生した seq>&wait=25&job=<id>`)
で「コマンドが喋らせた発話」と「コマンドの終了」の両方を待てます。
保持は最大 16 件・5 分 (溢れたら古い順に捨てる)。メモリだけで、サーバーを再起動すると消えます。
`/inbox` はジョブの枠と無関係で、処理中でもいつでも使えます。今後、本体が暇なときに常に受け箱を見に来るようにすれば
(第 2 段)、キーと無関係にどのプロセスからでも `stackee-say` で stackee に喋らせられます。

ログには `key-press` (押下と結果)、`key-command` (終了コード・時間・出力の末尾)、`inbox-put` (seq・音声の有無・長さ)
が出ます。管理画面の「現在の状態」に直近 10 回の押下と受け箱の件数・最新 seq を表示します。

## 後で外でも使う場合

送信先はURLで独立しているため、同じ API を持つ外部サーバーへ移せます。
現段階は信頼できる LAN 内の開発用 HTTP で、認証と TLS は未実装です。
外部公開する段階で HTTPS・認証と S3 の HTTPS 対応を追加します。
Linux では `--tts voicevox` で音声合成を行います。
GitHub Pages は `docs/` の静的サイト専用で、このサーバーを実行する場所ではありません。

## ソースコードの更新・反映

ソースコードは GitHub 経由で受け渡します。親リポジトリで開発する場合は、
変更をコミットして `scripts/public-subtree.sh push` を実行します。
配置先ではソースを直接コピーせず、公開リポジトリを更新します。

```sh
cd ~/works/stackee
git pull --ff-only
python3 -m unittest discover -s server -p 'test_*.py'
# サービス定義を更新した場合
cp server/stackee-*.service ~/.config/systemd/user/
systemctl --user daemon-reload
# 処理中の会話が完了してから再起動
systemctl --user restart stackee-talk
git log -1 --oneline
systemctl --user is-active stackee-talk stackee-voicevox
```

`/health` の `busy` が false であることを確認してから再起動します。
ローカル変更がある場合は保全してから pull します。
接続アドレスなどのホスト固有設定は `~/.config/stackee-talk.env` に置きます。

## テスト

```sh
python3 -m unittest discover -s server -p 'test_*.py'
```

音声形式、無音、エージェントの最終出力、返答の長さ調整、会話のリセット、プロセスのタイムアウト、HTTP受付から音声取得、
処理の競合、失敗・期限切れ、`/look` の受付・形式検査・`/talk` との `409` 共有・偽エージェントへの写真の受け渡し、会話の継続・再開・二重起動防止に加えて、
Codex と Claude の会話が別々に保たれること、旧形式の設定・会話 ID の移行、
存在しない会話 ID での再開が失敗しても保存が壊れないこと、管理 API の検証・保護を確認します。
`test_stackee_keys.py` は独自キー (3 方式・`409` の共有・コマンドの成功/失敗/時間切れ/裏の子プロセス・環境変数)、
`stackee-say` → 受け箱 → `/inbox` (after / wait / job の組み合わせ・期限切れ・件数上限・音声)、
`/say` の `403` と検証、キー設定の保存 API を確かめます。合成は偽物で、音は鳴らしません。
通常のテストは偽のエージェントを使い、実際の Codex / Claude を呼びません。
実際の認証・モデルを使う検証は明示的に実行します。

```sh
python3 server/check_agent.py
```

本番とは別の試験用会話で、同じ PID での2往復、再起動後の記憶、指示変更の適用を検証します。

写真の受け渡しは `--image` で確かめます。`/look` と同じ固定文で写真を見せた後、
同じ会話で「さっきの写真」について文字だけで質問します。音声合成・再生はしません。

```sh
python3 server/check_agent.py --image test.jpg --agent codex --agent-dir /path/to/test-agent
python3 server/check_agent.py --image test.jpg --agent claude --agent-dir /path/to/test-agent
```

`--agent-dir` を省くと一時フォルダで試します。本番の `server/agent/` は指定できません。
