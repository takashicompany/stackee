# firmware/assets — 本体へ送る素材

本体の FAT (`/stackee_assets/`) へ置くもの。送るのは
[`../tools/install_assets.sh`](../tools/install_assets.sh)。
ファームの像には含まれない (起動時にファイルとして読む)。

| ファイル | 中身 | 出所とライセンス |
|---|---|---|
| `faces.bin` | 顔 32 枚。4bpp の縦長シートを zlib 圧縮 | Stack-chan の v2 の顔を 200px 四方・16 階調へ変換したもの |
| `changes.bin` | 顔のフレーム間で変わる領域 (差分描画の下ごしらえ) | 同上 |
| `manifest.json` | 上の 2 つと音声の目録 (寸法・オフセット・SHA-256) | — |
| `status_icons.bin` | 上段のアイコン 18 枚。24×24・2bit を zlib 圧縮 | Material Design Icons (Google) / **Apache-2.0** → `LICENSE-material-design-icons.txt` |
| `status_icons.json` | 上のタイル番号と寸法 | — |
| `status_h24.bdf` | 上段の数字。半角 12×24 の ASCII 95 文字 | /efont/ Unicode Bitmap Fonts の `h24.bdf` → `LICENSE-efont.txt` |
| `font16.bin` | 字幕用の日本語 16px フォント (7,037 字) | **東雲 (Shinonome) フォント / Public Domain**。`../tools/gen_font16.py` が BDF から作る |
| `ack_01..05.pcmz` | 一次回答の音声 5 本。16 kHz / 16bit / mono PCM を zlib 圧縮 | VOICEVOX: ずんだもん (ノーマル、speaker 3) で合成 |

## ライセンスの表記

### Material Icons (アイコン)

Material Icons by Google —
https://github.com/google/material-design-icons
(コミット `40a7a292a79d9394157e1ea24f83d52d5e17c556`)

Licensed under the Apache License, Version 2.0 (the "License"); you may not use
these files except in compliance with the License. You may obtain a copy of the
License at http://www.apache.org/licenses/LICENSE-2.0 (全文:
`LICENSE-material-design-icons.txt`). Unless required by applicable law or
agreed to in writing, software distributed under the License is distributed on
an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
or implied.

変更点: `src/*/*/materialicons/24px.svg` を 24×24 にラスタライズし、4 階調
(2bit) へ減色して 1 枚のシートにまとめた。上流のリポジトリには NOTICE ファイルが
無いため、同梱するのは LICENSE 全文とこの表記。

### /efont/ Unicode Bitmap Fonts (上段の数字)

`status_h24.bdf` は /efont/ Unicode Bitmap Fonts の `h24.bdf` から ASCII 部分
だけを抜き出したもの。http://openlab.ring.gr.jp/efont/ 。BDF 内の `COPYRIGHT`
プロパティも元のまま残してある。全文は `LICENSE-efont.txt`。

### 東雲 (Shinonome) フォント (字幕)

`font16.bin` は東雲フォント (`shnmk16.bdf` / `shnm8x16r.bdf`) から
`../tools/gen_font16.py` が作った生成物。**Public Domain**。
BDF 本体はこのリポジトリに入れていないので、作り直すときは配布元から取ってきて
`--wide` / `--narrow` で場所を渡す。

### 顔と一次回答

顔は Stack-chan の素材を変換したもの、一次回答は VOICEVOX で合成したもの。
どちらも元の配布条件に従う。**このディレクトリの素材は、上のディレクトリ
(`firmware/` の GPL-2.0-or-later) とは別のライセンスで、それぞれの出所の
条件が効く。**
