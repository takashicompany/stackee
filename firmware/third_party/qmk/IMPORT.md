# 取り込んだ QMK

| | |
|---|---|
| 出所 | https://github.com/qmk/qmk_firmware |
| 版 | **0.34.4** (0.xx.y 形式の最新の安定タグ、2026-09-16 時点) |
| コミット | `08c662f286ddfd12a985f57b584b02eca5af0ae6` (2026-09-05) |
| ライセンス | **GPL-2.0** (同梱の `LICENSE`) |
| ファイル数 | 86 |
| 取り込み方 | **コピー**。サブモジュールにしない (DESIGN.md §2) |

## 決めごと

**この下のファイルは 1 文字も変えない。** 差し替えが要るところは
`main/qmk_port/` で吸収する。改変していないことは

    diff -r third_party/qmk <展開した qmk_firmware 0.34.4 の同じパス>

で確かめられる (2026-09-16 時点で差分なしを確認)。

版を上げるときは:

1. 新しいタグを展開して、下の一覧と同じパスを上書きコピーする
2. `python3 tools/test_keyseq_host.py` を通す (打鍵列が変わっていないか)
3. `python3 tools/test_gen_keymap.py` を通す (keycodes.h の並びが変わると
   生成物が変わる)
4. `./build.sh` で像に収まるか見る
5. この表の版とコミットを書き換える

## 公開範囲

QMK 由来のコードは **GPL-2.0-or-later**。これを取り込んで一緒にビルドして
いるので、`firmware/` 全体が同じ条件になる。**2026-09-21 にソースを公開した**
(公開リポジトリ takashicompany/stackee の `firmware/`)。全文は
`firmware/LICENSE` (QMK 同梱の `LICENSE` と同じもの)。

QMK は「via.c を他ファームへ翻案すること」「非公開の無線ライブラリと
リンクした配布」を違反例に挙げている (docs.qmk.fm/license_violations)。
このファームは **via.c を翻案していない** (取り込んだ `quantum/via.c` を
そのまま使い、独自部分は `main/qmk_port/` に置いている)。
ESP-IDF の Wi-Fi / BLE がバイナリ提供なのは変わらないので、配布するのは
**ソースと、それを自分でビルドし直せる手順** (`firmware/README.md` §3)。

## 取り込んだファイル

### `platforms/`

timer / eeprom / suspend のヘッダと suspend.c。残りは main/qmk_port で自前

- `eeprom.h`
- `suspend.c`
- `suspend.h`
- `timer.h`

### `quantum/`

quantum の本体。キー処理・レイヤー・HoldTap・VIA・dynamic keymap

- `action.c`
- `action.h`
- `action_code.h`
- `action_layer.c`
- `action_layer.h`
- `action_tapping.c`
- `action_tapping.h`
- `action_util.c`
- `action_util.h`
- `bits.h`
- `bitwise.c`
- `bitwise.h`
- `command.h`
- `compiler_support.h`
- `debounce.h`
- `dynamic_keymap.c`
- `dynamic_keymap.h`
- `eeconfig.c`
- `eeconfig.h`
- `encoder.h`
- `keyboard.c`
- `keyboard.h`
- `keycode.h`
- `keycode_config.c`
- `keycode_config.h`
- `keycode_string.h`
- `keycodes.h`
- `keymap_common.c`
- `keymap_common.h`
- `keymap_introspection.c`
- `keymap_introspection.h`
- `led.c`
- `led.h`
- `matrix.h`
- `matrix_common.c`
- `modifiers.h`
- `mousekey.c`
- `mousekey.h`
- `programmable_button.h`
- `quantum.c`
- `quantum.h`
- `quantum_keycodes.h`
- `quantum_keycodes_legacy.h`
- `raw_hid.h`
- `ring_buffer.h`
- `sync_timer.c`
- `sync_timer.h`
- `util.h`
- `via.c`
- `via.h`

### `quantum/debounce/`

デバウンス 1 種類だけ (sym_defer_pk = 全キー共通で静まってから確定)

- `sym_defer_pk.c`

### `quantum/keymap_extras/`

quantum_keycodes.h が読む US 配列の別名表

- `keymap_us.h`

### `quantum/logging/`

dprintf / debug フラグ。出口は qmk_port/_print.h

- `debug.c`
- `debug.h`
- `print.c`
- `print.h`
- `sendchar.c`
- `sendchar.h`

### `quantum/nvm/`

eeconfig / dynamic keymap / via の保存の抽象

- `nvm_dynamic_keymap.h`
- `nvm_eeconfig.h`
- `nvm_via.h`

### `quantum/nvm/eeprom/`

その EEPROM 実装。qmk_port/qmk_port_eeprom.c が下を受ける

- `nvm_dynamic_keymap.c`
- `nvm_eeconfig.c`
- `nvm_eeprom_eeconfig_internal.h`
- `nvm_eeprom_via_internal.h`
- `nvm_via.c`

### `quantum/process_keycode/`

process_quantum が呼ぶ最小限

- `process_default_layer.c`
- `process_default_layer.h`
- `process_quantum.c`
- `process_quantum.h`

### `quantum/send_string/`

via.c のマクロ再生が使う

- `send_string.c`
- `send_string.h`
- `send_string_keycodes.h`

### `quantum/sequencer/`

quantum_keycodes.h が読むヘッダのみ

- `sequencer.h`

### `tmk_core/protocol/`

host_driver_t とレポートの型。host.c はそのまま使う

- `host.c`
- `host.h`
- `host_driver.h`
- `report.c`
- `report.h`
- `usb_device_state.c`
- `usb_device_state.h`
- `usb_types.h`

## 取り込まなかったもの (と、その代わり)

| QMK の場所 | なぜ要らない / 何で代えたか |
|---|---|
| `platforms/avr`, `platforms/chibios` | MCU 依存。ESP-IDF 版は `main/qmk_port/` |
| `platforms/gpio.h`, `atomic_util.h`, `progmem.h`, `wait.h`, `bootloader.h` | 中身が MCU 依存。`main/qmk_port/` に同名の自前版を置いた |
| `quantum/matrix.c` | GPIO を直接叩く実装。Stackee は I2C なので `matrix_common.c` の "CUSTOM MATRIX LITE" の口 (`matrix_init_custom` / `matrix_scan_custom`) だけを使う |
| `quantum/main.c` | MCU を起こして無限ループを回すためのもの。FreeRTOS のタスクがその役をする (`qmk_port/qmk_port_init.c`) |
| `lib/printf` | `_print.h` で自前の出口に差し替え |
| `quantum/rgblight`, `rgb_matrix`, `audio`, `encoder.c`, `pointing_device`, `split_common` ほか | Stackee に該当ハードが無い。ヘッダだけ要るものは `encoder.h` / `programmable_button.h` を置いてある |
| `quantum/keymap_extras` の keymap_us.h 以外 | `quantum_keycodes.h` が読むのは US だけ |
| `tmk_core/protocol/usb_descriptor.c` ほか | USB は TinyUSB で自前 (`main/stackee_usb.c`) |
