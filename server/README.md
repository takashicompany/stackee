# stackee 音声サーバー (Mac)

同じ Wi-Fi の CoreS3 から音声を受け取り、ローカルの `whisper-cli` で文字起こし、
`codex exec` で返答生成、macOS の `say` で音声合成します。
返答も HTTP で S3 に返すため、マイク・スピーカーとも USB 音声転送は不要です。
返答のピーク音量はフルスケールの25%以下に抑えます。小さい音は増幅しません。

## 起動

Python 3.10 以降、`whisper-cli`、Whisper のモデルファイル、ログイン済みの Codex CLI、
macOS の `say` (Kyoko) が必要です。Python の外部パッケージは不要です。

公開リポジトリのルートから:

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
`--echo` は Codex を呼ばず、文字起こし結果をそのまま読み上げる疎通試験です。

Codex は一時ディレクトリで `exec --ephemeral --ignore-user-config --sandbox read-only`
として動き、最終メッセージを `-o` で受け取ります。個人の MCP 設定やプロジェクトの
作業指示は読み込みません。会話は直近4往復をサーバーのメモリに保持します。
モデルは Codex CLI の既定値、または `--codex-model` で指定したものです。

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

入力 WAV は非圧縮・16kHz・16bit・mono、0.3〜30秒。
受付時は `202` と JSON `{id, status_url}`、同じURLを `Location` ヘッダーにも返します。
`done` には `transcript`、`reply`、`audio_url`、`audio_bytes`、
`sample_rate`、`channels`、`sample_width` が入ります。1秒間隔のポーリングを想定します。

同時処理は1件で、処理中の追加送信は `409`。形式不正は `400`、サイズ超過は `413`、
MIME不一致は `415`。結果は最大8件・5分間、メモリだけに保持します。
音声の一時ファイルは処理終了時に削除します。文字起こし結果は Codex に送信されます。

## 後で外でも使う場合

送信先はURLで独立しているため、同じ API を持つ外部サーバーへ移せます。
現段階は信頼できる LAN 内の開発用 HTTP で、認証と TLS は未実装です。
外部公開する段階で HTTPS・認証と S3 の HTTPS 対応を追加します。
Mac 固有なのは `say` の音声合成部分です。別OSへ移す場合は置き換えが必要です。
GitHub Pages は `web/` の静的サイト専用で、このサーバーを実行する場所ではありません。

## テスト

```sh
python3 -m unittest discover -s server -p 'test_*.py'
```

音声形式、無音、Codexの最終出力、プロセスのタイムアウト、HTTP受付から音声取得、
処理の競合、失敗・期限切れを検証します。実際のCodexを呼ぶテストではありません。
