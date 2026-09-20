# firmware/assets — 本体へ送る素材

本体の FAT (`/stackee_assets/`) へ置くもの。送るのは
[`../tools/install_assets.sh`](../tools/install_assets.sh)。
ファームの像には含まれない (起動時にファイルとして読む)。

元データも `src/` に入れてある。**生成物だけでなく、そこから作り直せる
ものを置く**のが方針。ただし元データをここへ置けていないものが 2 つあり
(アイコンと一次回答の音声)、それは下の表に正直に書いてある。

## 一覧

| ファイル | 中身 | 元データ | 作者 / ライセンス | 作り直し方 |
|---|---|---|---|---|
| `faces.bin` | 顔 32 枚。240×240・4bpp の縦長シートを zlib 圧縮 | `src/faces/<状態>/*.png` (800×800、32 枚) | **takashicompany (ユーザー本人)。ライセンス未指定** | `python3 ../tools/import_faces.py` |
| `changes.bin` | 顔のフレーム間で変わる領域 (差分描画の下ごしらえ) | 同上 | 同上 | 同上 (一緒に出る) |
| `manifest.json` | 上の 2 つと音声の目録 (寸法・オフセット・SHA-256) | 同上 | — | 同上 (一緒に出る) |
| `font16.bin` | 字幕用の日本語 16px フォント (7,037 字) | `src/fonts/shinonome/shnmk16.bdf` と `shnm8x16r.bdf` | **東雲フォント / Public Domain** (`src/fonts/shinonome/LICENSE.utf8.txt`) | `python3 ../tools/gen_font16.py` |
| `status_icons.bin` | 上段のアイコン 18 枚。24×24・2bit を zlib 圧縮 | **このリポジトリには無い** (下の「元データが入っていないもの」) | Material Design Icons (Google) / **Apache-2.0** (`LICENSE-material-design-icons.txt`) | 親リポジトリの `firmware/kmk/tools/generate_status_assets.py --fetch` |
| `status_icons.json` | 上のタイル番号と寸法 | 同上 | — | 同上 (一緒に出る) |
| `status_h24.bdf` | 上段の数字。半角 12×24 の ASCII 95 文字 | **このリポジトリには無い** (/efont/ の `h24.bdf` を切り出したもの) | /efont/ Unicode Bitmap Fonts (`LICENSE-efont.txt`) | 同上 |
| `ack_01..05.pcmz` | 一次回答の音声 5 本。16 kHz / 16bit / mono PCM を zlib 圧縮 | **このリポジトリには無い** (合成の入力は日本語の文 5 つ) | VOICEVOX: ずんだもん (ノーマル、speaker 3)。VOICEVOX の利用規約に従う | 親リポジトリの `firmware/kmk/tools/generate_ack_assets.py --host <合成サーバ>` |

## 作り直しが合っているかを見る

どちらも**書かずに一致だけ**を見る。同じ元データからは毎回同じバイト列が出る。

```sh
python3 firmware/tools/import_faces.py --check     # faces.bin / changes.bin / manifest.json
python3 firmware/tools/gen_font16.py  --check      # font16.bin
```

`import_faces.py` は Pillow が要る (`python3 -m pip install --user pillow`)。
**どちらも実機に触らず、音も鳴らさない。**

## 元データが入っていないもの

- **アイコン** — Material Design Icons の SVG は上流のリポジトリから取る。
  `generate_status_assets.py --fetch` が `google/material-design-icons` から
  必要なものだけ落としてきて、`rsvg-convert` で 24×24 にラスタライズし、
  4 階調 (2bit) へ減らして 1 枚のシートにまとめる。
  SVG そのものはここには置いていない。
- **上段の数字のフォント** — /efont/ の `h24.bdf` (439 KB) から ASCII 95 文字
  だけを抜いたものが `status_h24.bdf`。元の BDF は配布元から取る。
- **一次回答の音声** — 合成は稼働中の音声サーバ (ubook) の VOICEVOX を
  そのまま使う。文面と合成設定と PCM の SHA-256 は `manifest.json` の
  `acks` に記録してある。**この道具は音を鳴らさない。**

この 3 つを作り直す道具は親リポジトリ (非公開) の `firmware/kmk/tools/` に
ある。生成物と、その出所・ライセンス・作り方はここに全部書いてあるので、
作り直さずにそのまま使う分にはこのリポジトリだけで足りる。

## ライセンスの表記

### 顔 (`src/faces/`, `faces.bin`, `changes.bin`)

作者は takashicompany (このリポジトリのユーザー本人)。Stack-chan の v2 の顔
として描いたもの。**ライセンスは指定していない。** 変換は
`../tools/import_faces.py` が、白背景に合成 → 状態ごとに倍率を固定して
中央基準で 240×240 へ縮小 → 16 階調 (4bpp) へ減色、の順で行う。

### Material Icons (アイコン)

Material Icons by Google —
https://github.com/google/material-design-icons

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

`src/fonts/shinonome/` に BDF そのものを入れてある。**Public Domain**
(日本の法律では著作権を放棄できないため、作者が権利を行使しないと宣言する
形での Public Domain。全文は同じ場所の `LICENSE.utf8.txt`)。
`font16.bin` はそこから `../tools/gen_font16.py` が作った生成物。

### 一次回答の音声 (`ack_*.pcmz`)

VOICEVOX: ずんだもん (ノーマル、speaker 3) で合成したもの。VOICEVOX と
キャラクターそれぞれの利用規約に従う。

---

**この `assets/` の中身は、上のディレクトリ (`firmware/` の
GPL-2.0-or-later) とは別のライセンスで、それぞれの出所の条件が効く。**
