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
  道具と MCP は無効、設定ソースは project のみ (実行ユーザーの `~/.claude/CLAUDE.md` は読みません)。

**会話は種別ごとに別々です。** 切り替えて戻すと、それぞれ前の続きから再開します。

#### 既定値と実行時ファイル

管理画面から編集するファイルは git 管理外です。`git pull --ff-only` と衝突しません。

| 場所 | 役割 |
| --- | --- |
| `server/agent/defaults/AGENTS.md` / `CLAUDE.md` / `agent.json` | Git 追跡。出荷時の既定値 |
| `server/agent/AGENTS.md` / `CLAUDE.md` / `agent.json` | 実行時ファイル。Git 管理外 |
| `server/agent/.state/session.json` | 継続する会話の ID。Git 管理外 |

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
非対話の承認ポリシーを指定します。Claude Code は道具を全部無効化し、権限モードは plan です。
エージェントには URL・ドメイン名・出典リンクを含めないよう指示します。
生成後にも URL と引用マーカーを除去し、Markdown リンクは表示名だけを残します。
除去は文字数制限の前に行い、音声合成・画面へ返す回答に適用します。
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
| `GET /jobs/{id}` | `processing` / `done` / `ignored` / `error` と結果を取得 |
| `GET /jobs/{id}/audio` | 返答の生 PCM (16kHz、符号付き16bit LE、mono) を取得 |
| `GET /admin` ほか | 管理画面と設定 API (上記「管理画面」を参照) |

入力 WAV は非圧縮・16kHz・16bit・mono、0.3〜30秒。
受付時は `202` と JSON `{id, status_url}`、同じURLを `Location` ヘッダーにも返します。
`done` には `transcript`、`reply`、`audio_url`、`audio_bytes`、
`sample_rate`、`channels`、`sample_width` が入ります。1秒間隔のポーリングを想定します。

同時処理は1件で、処理中の追加送信は `409`。形式不正は `400`、サイズ超過は `413`、
MIME不一致は `415`。結果は最大8件・5分間、メモリだけに保持します。
音声の一時ファイルは処理終了時に削除します。文字起こし結果はエージェントに送信されます。

## 後で外でも使う場合

送信先はURLで独立しているため、同じ API を持つ外部サーバーへ移せます。
現段階は信頼できる LAN 内の開発用 HTTP で、認証と TLS は未実装です。
外部公開する段階で HTTPS・認証と S3 の HTTPS 対応を追加します。
Linux では `--tts voicevox` で音声合成を行います。
GitHub Pages は `web/` の静的サイト専用で、このサーバーを実行する場所ではありません。

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

音声形式、無音、エージェントの最終出力、プロセスのタイムアウト、HTTP受付から音声取得、
処理の競合、失敗・期限切れ、会話の継続・再開・二重起動防止に加えて、
Codex と Claude の会話が別々に保たれること、旧形式の設定・会話 ID の移行、
存在しない会話 ID での再開が失敗しても保存が壊れないこと、管理 API の検証・保護を確認します。
通常のテストは偽のエージェントを使い、実際の Codex / Claude を呼びません。
実際の認証・モデルを使う検証は明示的に実行します。

```sh
python3 server/check_agent.py
```

本番とは別の試験用会話で、同じ PID での2往復、再起動後の記憶、指示変更の適用を検証します。
