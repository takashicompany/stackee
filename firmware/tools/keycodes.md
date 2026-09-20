# KMK と QMK の対応表

`firmware/kmk/keymap.py` (CircuitPython + KMK) を `main/keymaps/default_keymap.c`
(QMK) へ写すときの決めごと。変換するのは `tools/gen_keymap.py`、
確かめるのは `tools/test_gen_keymap.py` と `tools/test_keyseq_host.py`。

**キーコードの数値はこのファイルに書き写していない。** 生成スクリプトが
KMK 本体 (`firmware/kmk/.kmk_src/kmk_firmware`) と QMK の `keycodes.h` を
そのまま読んで作る。ここに書くのは「どういう規則で対応させたか」だけ。

## 1. 素のキー

| KMK | QMK | 備考 |
|---|---|---|
| `KeyboardKey(code=N)` | HID 使用番号 N の `KC_*` | 番号は両者で同じ |
| `ModifierKey` (単独) | `KC_LEFT_SHIFT` など | KMK のビット (0x02) を使用番号 (0xE1) に直す |
| `ModifiedKey{key, modifier}` | `LSFT(kc)` など | QMK は上位 5 ビットが修飾。右側は 0x10 が立つ |
| `KC.TRNS` | `KC_TRNS` | |
| `KC.NO` | `KC_NO` | |
| `KC.MO(n)` | `MO(n)` | `MO` 以外のレイヤーキーは keymap.py で使っていない |

JIS のキーは素直に写る。`KC.LANG1` = 144 = `KC_LANGUAGE_1` (0x90)、
`KC.LANG2` = 145 = `KC_LANGUAGE_2` (0x91)。USB の記述子側で Usage Maximum を
0xFF にしてあるので、この 2 つがホストに届く (DESIGN.md §2)。

## 2. HoldTap

KMK の `HoldTapKey` は 4 つの設定を持つ (kmk/modules/holdtap.py)。

| KMK | QMK | 実装 |
|---|---|---|
| `KC.HT(tap, hold)` | `MT(mod, tap)` | hold が修飾キーのとき |
| `KC.LT(layer, tap)` | `LT(layer, tap)` | hold が `MO(layer)` のとき |
| `prefer_hold=True` | `HOLD_ON_OTHER_KEY_PRESS` をそのキーだけ有効 | `get_hold_on_other_key_press()` |
| `tap_interrupted=True` | `PERMISSIVE_HOLD` をそのキーだけ有効 | `get_permissive_hold()` |
| `tap_time` | `TAPPING_TERM` | `get_tapping_term()` |

★ **既定値に注意。** `KC.HT(...)` は `prefer_hold=True` が既定
(HoldTapKey の引数既定)。`KC.LT(...)` は `lt_key()` が `prefer_hold=False` を
渡すので既定は False。つまり keymap.py のホームロー修飾 (Z=Shift / X=GUI /
.=Ctrl など) はすべて `HOLD_ON_OTHER_KEY_PRESS` 付きになる。

★ **表はキーコードではなく「位置 + キーコード」で引く。** レイヤー 0 の
キー 36 と 37 はどちらも `LT(1, LANG1)` だが、36 だけ `prefer_hold=True`。
キーコードだけで引くと片方の設定が黙って消える。VIA でキーを動かした
場合に備えて、位置が一致しなければキーコードだけで引き直す。

### tap_time の値

| | KMK の実際の値 | QMK に入れた値 |
|---|---|---|
| `KC.HT(...)` | 200 ms (keymap.py が `_HOLDTAP.tap_time = 200`) | **200 ms** |
| `KC.LT(...)` | 300 ms (Layers インスタンスは KMK 既定のまま) | **300 ms** |

LT が 300 ms なのは keymap.py のコメントにも書かれている
(「移植元 (keybotch) は HoldTap 側にだけ 200ms を設定していた」= `KC.LT` は
KMK 既定の 300ms のまま)。2026-09-16 に **KMK の実挙動に合わせる** と決めた
(DESIGN.md §5)。QMK 側は `TAPPING_TERM_PER_KEY` で HT と LT を別々に答える。
値を変える場所は `tools/gen_keymap.py` の `QMK_MT_TAPPING_TERM` /
`QMK_LT_TAPPING_TERM`。

### QMK 側で切った機能

| 機能 | 既定 | Stackee | 理由 |
|---|---|---|---|
| クイックタップ (`QUICK_TAP_TERM`) | `TAPPING_TERM` = 有効 | **0 = 無効** | タップ直後に同じ HoldTap キーを押し直すと長押しがタップ扱いのままになる。KMK の `repeat=` に相当するが keymap.py はどのキーにも指定していない |
| `NO_ACTION_ONESHOT` | 有効 | 無効化 | KMK に相当機能が無い |

## 3. タップ側が修飾つきの HoldTap

`KC.HT(KC.LSFT(KC.SCLN), KC.LSFT)` (レイヤー 1 のキー 21。離せば JIS の
「+」、押さえれば Shift) は QMK の `MT()` では表せない — `MT(mod, kc)` の
`kc` は 1 バイトしか入らないため。

そこで独自キーコード `STK_MT_0`, `STK_MT_1`, … に逃がし、
`main/qmk_port/stackee_holdtap.c` の小さな状態機械が

* 他のキーが押された (`prefer_hold` のとき) → hold 側を確定して修飾キーを押す
* `tapping_term` を過ぎた → hold 側を確定
* それより前に離した → tap 側を `tap_code16()` で送る

を行う。表 (`stackee_ext_mt[]`) は生成物。使う数が増えれば自動で増える。

## 3b. マウスキー

`MOUSEKEY_ENABLE` を有効にしてある（現行 CircuitPython 版も KMK の
MouseKeys を入れているため）。動作モードは QMK 既定の加速つき。

| KMK (kmk/modules/mouse_keys.py) | QMK | 値 |
|---|---|---|
| `KC.MS_UP` | `QK_MOUSE_CURSOR_UP` (`MS_UP`) | 0x00CD |
| `KC.MS_DOWN` / `KC.MS_DN` | `QK_MOUSE_CURSOR_DOWN` (`MS_DOWN`) | 0x00CE |
| `KC.MS_LEFT` / `KC.MS_LT` | `QK_MOUSE_CURSOR_LEFT` (`MS_LEFT`) | 0x00CF |
| `KC.MS_RIGHT` / `KC.MS_RT` | `QK_MOUSE_CURSOR_RIGHT` (`MS_RGHT`) | 0x00D0 |
| `KC.MB_LMB` | `QK_MOUSE_BUTTON_1` (`MS_BTN1`) | 0x00D1 |
| `KC.MB_RMB` | `QK_MOUSE_BUTTON_2` (`MS_BTN2`) | 0x00D2 |
| `KC.MB_MMB` | `QK_MOUSE_BUTTON_3` (`MS_BTN3`) | 0x00D3 |
| `KC.MB_BTN4` | `QK_MOUSE_BUTTON_4` (`MS_BTN4`) | 0x00D4 |
| `KC.MB_BTN5` | `QK_MOUSE_BUTTON_5` (`MS_BTN5`) | 0x00D5 |
| `KC.MW_UP` | `QK_MOUSE_WHEEL_UP` (`MS_WHLU`) | 0x00D9 |
| `KC.MW_DOWN` / `KC.MW_DN` | `QK_MOUSE_WHEEL_DOWN` (`MS_WHLD`) | 0x00DA |
| `KC.MW_LEFT` / `KC.MW_LT` | `QK_MOUSE_WHEEL_LEFT` (`MS_WHLL`) | 0x00DB |
| `KC.MW_RIGHT` / `KC.MW_RT` | `QK_MOUSE_WHEEL_RIGHT` (`MS_WHLR`) | 0x00DC |

KMK 側に無いもの（加速の `MS_ACL0`〜2、ボタン 6〜8）は keymap.py に書けない
ので、使いたければ **VIA から割り当てる**（QMK の標準キーコードなので VIA
の定義 JSON には何も足さなくてよい）。

現行の keymap.py はマウスキーを 1 つも使っていない。それでも表を置いてある
のは、使い始めたときに黙って落ちずそのまま出るようにするため。

## 4. 独自キー

VIA の `customKeycodes` は **`QK_KB_0` (0x7E00) から順に** 対応づく約束
(QMK の `SAFE_RANGE` = `QK_USER` = 0x7E40 ではない)。DESIGN.md §5 は
「SAFE_RANGE 以降」と書いているが、VIA / Remap に名前を出すには
`QK_KB_0` 側でなければならないので、そちらに合わせてある。
`process_record_kb()` が受け止めて **`false` を返す** (= HID には出さない)。

| 並び | C の名前 | KMK | 中身が入る段階 |
|---|---|---|---|
| 0 | `STK_TALK` | `KC.TALK` | 3 (音声会話) |
| 1 | `STK_VOLUP` | `KC.STK_VOLUP` | 3 (本体スピーカーの音量) |
| 2 | `STK_VOLDN` | `KC.STK_VOLDN` | 3 |
| 3 | `STK_HID_SWITCH` | `KC.HID_SWITCH` | 1b (BLE が入ってから) |
| 4 | `STK_BLE_REFRESH` | `KC.BLE_REFRESH` | 1b |
| 5 | `STK_CAMERA` | (未使用) | 4 |
| 6 | `STK_TOUCH_SCROLL` | `KC.TOUCH_SCROLL` | 4 (タッチパッド) |
| 7〜 | `STK_MT_n` | (上の §3) | 済 |

`KC.RESET` だけは独自キーにしていない。QMK の `QK_BOOT` をそのまま使い、
行き先を **ROM の USB ダウンロードモード** にしてある
(`main/qmk_port/qmk_port_stubs.c` の `bootloader_jump`)。CircuitPython 版の
`KC.RESET` は普通の再起動だったが、書き込みの脱出路を本体側にも持たせる
ほうが価値が高い。

## 5. 変換できないものが来たら

生成スクリプトは黙って飛ばさず、その場で止まる。

* 知らない形のキー (`Converter.convert` の最後)
* 左右が混ざった修飾キー
* `MO` 以外のレイヤーキー (`TG` / `TO` / `TT` / `LM` / `DF` / `FD`)
* `LT` のタップ側が 1 バイトに収まらない

いずれも keymap.py を書き換えたときに初めて出るもの。出たら
`gen_keymap.py` に対応を足すのが正しい (生成物を手で直さない)。
