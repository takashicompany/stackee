#!/usr/bin/env python3
"""firmware/kmk/keymap.py から QMK の既定配列と VIA 定義 JSON を作る。

    python3 firmware/tools/gen_keymap.py          # 生成して書き出す
    python3 firmware/tools/gen_keymap.py --check   # 差分があれば異常終了

出すもの:

  firmware/main/keymaps/default_keymap.c
      QMK の keymaps[][MATRIX_ROWS][MATRIX_COLS] と、KMK の HoldTap 設定を
      キーごとに答える 3 つのコールバック
      (get_tapping_term / get_permissive_hold / get_hold_on_other_key_press)。
  firmware/via/stackee.json
      VIA / Remap に読み込ませる定義。matrix は 5x10、layouts は
      hardware/layout.kle.json (keymap.py の PHYS_ROWS と同じ物理配置)。

**この 2 つは手で編集しない。** 直すのは keymap.py かこのスクリプト。

--- なぜ本物の KMK を読むのか -------------------------------------------------

キーコードの数値 (KC.Q = 20、KC.LANG1 = 144 など) をこのスクリプトに書き写すと、
書き写した時点で 2 つ目の真実ができてしまう。KMK 本体
(firmware/kmk/.kmk_src/kmk_firmware) を CPython からそのまま import して
KC を評価すれば、写し間違いという壊れ方が最初から存在しない。

CircuitPython 専用モジュール (supervisor / usb_cdc / board ...) は
import できるだけの偽物を差し込む。キーコードの表そのものは本物を読む。

--- 独自キー -----------------------------------------------------------------

KC.TALK / KC.STK_VOLUP などは stackee_*.py が make_key で生やすキーで、
KMK 本体には無い。ここでは code.py と同じ名前で登録し直してから keymap.py を
読む (登録順・名前が code.py とずれると別のキーになる)。対応表は
tools/keycodes.md。
"""

import argparse
import json
import os
import re
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import stackee_tree as tree             # noqa: E402

REPO = str(tree.OUTER)
# 現行 CircuitPython 版 (非公開)。無い clone では配列を作り直せない
# (生成物 main/keymaps/default_keymap.c と via/stackee.json は入っている)。
KMK_DIR = str(tree.KMK) if tree.KMK else os.path.join(REPO, 'firmware', 'kmk')
KMK_SRC = os.path.join(KMK_DIR, '.kmk_src', 'kmk_firmware')
KEYCODES_H = os.path.join(IDF, 'third_party', 'qmk', 'quantum', 'keycodes.h')
KLE_PATH = os.path.join(REPO, 'hardware', 'layout.kle.json')

OUT_KEYMAP = os.path.join(IDF, 'main', 'keymaps', 'default_keymap.c')
OUT_VIA = os.path.join(IDF, 'via', 'stackee.json')

N_ROWS = 5
N_COLS = 10

# VIA の customKeycodes は 0x7E00 (QMK の QK_KB_0) から順に並ぶ約束。
# QMK の SAFE_RANGE (= QK_USER = 0x7E40) ではないので注意 (tools/keycodes.md)。
QK_KB_0 = 0x7E00

# KMK の修飾キーのビット (kmk/keys.py maybe_make_mod_key) -> QMK の MOD_*。
# 左は同値、右は QMK 側が「右フラグ 0x10 + 左の値」で表す。
KMK_MOD_TO_QMK = {
    0x01: ('MOD_LCTL', 0x01, 'LCTL'),
    0x02: ('MOD_LSFT', 0x02, 'LSFT'),
    0x04: ('MOD_LALT', 0x04, 'LALT'),
    0x08: ('MOD_LGUI', 0x08, 'LGUI'),
    0x10: ('MOD_RCTL', 0x11, 'RCTL'),
    0x20: ('MOD_RSFT', 0x12, 'RSFT'),
    0x40: ('MOD_RALT', 0x14, 'RALT'),
    0x80: ('MOD_RGUI', 0x18, 'RGUI'),
}

# --------------------------------------------------------------------------
# 独自キー (VIA の customKeycodes に並ぶ順 = QK_KB_0 からの並び)
# --------------------------------------------------------------------------
# name       … C の識別子 / VIA の customKeycodes[].name
# kmk        … keymap.py で使われている KMK 側の名前 (None = まだ使っていない)
# title      … VIA の説明文
# short      … VIA のキーキャップに出る短い名前
CUSTOM_KEYS = (
    ('STK_TALK',          'TALK',          '押して録音 / 離して送信 (音声会話)', 'Talk'),
    ('STK_VOLUP',         'STK_VOLUP',     '本体スピーカーの音量 +5%',           'Vol+'),
    ('STK_VOLDN',         'STK_VOLDN',     '本体スピーカーの音量 -5%',           'Vol-'),
    ('STK_HID_SWITCH',    'HID_SWITCH',    'BLE と USB の切り替え',              'HIDsw'),
    ('STK_BLE_REFRESH',   'BLE_REFRESH',   'BLE のアドバタイズを撒き直す',       'BLEad'),
    ('STK_CAMERA',        None,            'カメラで 1 枚撮る (段階 4)',         'Cam'),
    ('STK_TOUCH_SCROLL',  'TOUCH_SCROLL',  '押している間だけタッチをスクロールに', 'Scrl'),
)
N_FIXED_CUSTOM = len(CUSTOM_KEYS)

# KMK 側で「本体が自前で処理する」キーのうち、QMK に同じものがあるもの。
KMK_BUILTIN_TO_QMK = {
    'RESET': ('QK_BOOT', 0x7C00),
}

# マウスキー。KMK の MouseKeys モジュール (kmk/modules/mouse_keys.py) が
# 生やす名前を、QMK の標準キーコードへ写す。
#
# ★ keymap.py がマウスキーを 1 つも使っていなくても、ここに表があること自体に
#   意味がある: 使い始めたときに黙って落ちず、そのまま出るようになる。
#   KMK 側に無いもの (MS_ACL0-2、BTN6-8、MW_LEFT/RIGHT の QMK 名) は
#   QMK の名前で直接 keymap.py に書けないので、VIA から割り当てる。
KMK_MOUSE_TO_QMK = {
    'MS_UP':    ('QK_MOUSE_CURSOR_UP', 0x00CD),
    'MS_DOWN':  ('QK_MOUSE_CURSOR_DOWN', 0x00CE),
    'MS_DN':    ('QK_MOUSE_CURSOR_DOWN', 0x00CE),
    'MS_LEFT':  ('QK_MOUSE_CURSOR_LEFT', 0x00CF),
    'MS_LT':    ('QK_MOUSE_CURSOR_LEFT', 0x00CF),
    'MS_RIGHT': ('QK_MOUSE_CURSOR_RIGHT', 0x00D0),
    'MS_RT':    ('QK_MOUSE_CURSOR_RIGHT', 0x00D0),
    'MB_LMB':   ('QK_MOUSE_BUTTON_1', 0x00D1),
    'MB_RMB':   ('QK_MOUSE_BUTTON_2', 0x00D2),
    'MB_MMB':   ('QK_MOUSE_BUTTON_3', 0x00D3),
    'MB_BTN4':  ('QK_MOUSE_BUTTON_4', 0x00D4),
    'MB_BTN5':  ('QK_MOUSE_BUTTON_5', 0x00D5),
    'MW_UP':    ('QK_MOUSE_WHEEL_UP', 0x00D9),
    'MW_DOWN':  ('QK_MOUSE_WHEEL_DOWN', 0x00DA),
    'MW_DN':    ('QK_MOUSE_WHEEL_DOWN', 0x00DA),
    'MW_LEFT':  ('QK_MOUSE_WHEEL_LEFT', 0x00DB),
    'MW_LT':    ('QK_MOUSE_WHEEL_LEFT', 0x00DB),
    'MW_RIGHT': ('QK_MOUSE_WHEEL_RIGHT', 0x00DC),
    'MW_RT':    ('QK_MOUSE_WHEEL_RIGHT', 0x00DC),
}

# KMK の HoldTap のモジュール既定 tap_time (kmk/modules/holdtap.py: HoldTap.tap_time
# = 300、code.py 側で HT 用インスタンスだけ 200 に設定)。keymap.py が
# tap_time を指定しないキーはこの値になる。
KMK_HT_TAP_TIME = 200      # keymap.py: _HOLDTAP.tap_time = 200
KMK_LT_TAP_TIME = 300      # Layers インスタンスは既定のまま

# QMK 側で使う TAPPING_TERM。DESIGN.md §5 (2026-09-16 更新)。
# KMK の実挙動に合わせる: HT は 200 ms、LT は 300 ms。
# (LT が 300 なのは Layers インスタンスの tap_time が KMK 既定のままだから。
#  keymap.py のコメント「移植元は HoldTap 側にだけ 200ms を設定していた」)
QMK_MT_TAPPING_TERM = 200
QMK_LT_TAPPING_TERM = 300


# --------------------------------------------------------------------------
# CircuitPython の偽物を差し込んで本物の KMK を読む
# --------------------------------------------------------------------------
def install_circuitpython_stubs():
    stubs = {
        'supervisor': {'ticks_ms': lambda: 0},
        'usb_cdc': {'console': None},
        'usb_hid': {},
        'micropython': {'const': (lambda x: x)},
        'microcontroller': {},
        'board': {},
        'busio': {},
        'digitalio': {},
        'keypad': {},
        'storage': {},
        # ★ KMK の scheduler は CircuitPython の _asyncio を使う。
        #   マウスキーのモジュールがそれを import するので、形だけ用意する
        #   (このスクリプトはキーを登録するだけで、タスクは走らせない)。
        '_asyncio': {'Task': type('Task', (), {}),
                     'TaskQueue': type('TaskQueue', (), {})},
    }
    for name, attrs in stubs.items():
        if name in sys.modules:
            continue
        mod = types.ModuleType(name)
        for key, value in attrs.items():
            setattr(mod, key, value)
        sys.modules[name] = mod


_LOADED = None


def load_kmk():
    """本物の KMK と keymap.py を読み込んで返す。

    ★ 1 プロセスに 1 回だけ。make_key() を 2 回呼ぶと同じ名前で **別の**
      Key オブジェクトができ、既に読み込んだ keymap.py が持っている
      オブジェクトと同一性で照合できなくなる (KMK のキーは名前ではなく
      オブジェクトの同一性で見分けるため)。
    """
    global _LOADED
    if _LOADED is not None:
        return _LOADED
    install_circuitpython_stubs()
    if KMK_SRC not in sys.path:
        sys.path.insert(0, KMK_SRC)
    from kmk.keys import KC, make_key

    # code.py が入れている MouseKeys モジュール。import ではなく
    # インスタンス化した時点で KC.MS_* / KC.MB_* / KC.MW_* が生える。
    from kmk.modules.mouse_keys import MouseKeys

    MouseKeys()

    # code.py が stackee_*.py の import で生やしているキーをここでも生やす。
    for name, kmk_name, _title, _short in CUSTOM_KEYS:
        if kmk_name is None:
            continue
        make_key(names=(kmk_name,))

    if KMK_DIR not in sys.path:
        sys.path.insert(0, KMK_DIR)
    import keymap as keymap_module

    _LOADED = (KC, keymap_module)
    return _LOADED


# --------------------------------------------------------------------------
# QMK の keycodes.h から「HID の使用番号 -> KC_ 名」を読む
# --------------------------------------------------------------------------
_ENUM_RE = re.compile(r'^\s*(KC_[A-Z0-9_]+)\s*=\s*0x([0-9A-Fa-f]{4})\s*,')


def load_qmk_basic_names(path=KEYCODES_H):
    """third_party/qmk の keycodes.h から 0x00-0xFF の KC_ 名を拾う。

    表をこのスクリプトへ書き写さないのは KMK 側と同じ理由。取り込んだ QMK の
    版が変わればこの表も自動で追従する。
    """
    names = {}
    with open(path, encoding='utf-8') as handle:
        for line in handle:
            match = _ENUM_RE.match(line)
            if not match:
                continue
            value = int(match.group(2), 16)
            if value <= 0xFF and value not in names:
                names[value] = match.group(1)
    if not names:
        raise SystemExit('keycodes.h から KC_ 名を読めない: %s' % path)
    return names


# --------------------------------------------------------------------------
# KMK のキー -> QMK のキーコード
# --------------------------------------------------------------------------
class Converted:
    """1 キーぶんの変換結果。"""

    def __init__(self, code, expr, kind='plain', tapping_term=None,
                 hold_on_other_key_press=False, permissive_hold=False):
        self.code = code                # QMK の 16bit キーコード
        self.expr = expr                # C に書く式
        self.kind = kind                # 'plain' / 'mt' / 'lt' / 'ext_mt'
        self.tapping_term = tapping_term
        self.hold_on_other_key_press = hold_on_other_key_press
        self.permissive_hold = permissive_hold

    def __repr__(self):
        return 'Converted(0x%04X, %r)' % (self.code, self.expr)


class Converter:
    def __init__(self, KC, basic_names):
        self.KC = KC
        self.basic = basic_names
        # 素の Key (TRNS / NO / 独自キー) は同一性でしか見分けられない。
        self.by_identity = {}
        for name in ('TRNS', 'NO'):
            self.by_identity[id(KC[name])] = name
        for name in KMK_BUILTIN_TO_QMK:
            self.by_identity[id(KC[name])] = name
        # マウスキーも素の Key / MouseKey なので同一性で引く。
        self.mouse_by_identity = {}
        for name, entry in KMK_MOUSE_TO_QMK.items():
            try:
                self.mouse_by_identity[id(KC[name])] = entry
            except Exception:
                pass    # MouseKeys が入っていない環境では無いだけ
        self.custom_by_identity = {}
        for index, (c_name, kmk_name, _t, _s) in enumerate(CUSTOM_KEYS):
            if kmk_name is None:
                continue
            self.custom_by_identity[id(KC[kmk_name])] = (c_name, QK_KB_0 + index)
        # tap が 1 バイトに収まらない HoldTap 用の追加キーコード。
        self.ext_mods = []              # [(mods_expr, mods_value, tap_expr, tap_code, prefer_hold)]

    # ---- 部品 ------------------------------------------------------------
    def basic_name(self, code):
        if code not in self.basic:
            raise SystemExit('QMK に HID 使用番号 0x%02X の名前が無い' % code)
        return self.basic[code]

    def mods_of(self, modifier_key):
        """KMK の ModifierKey -> [(QMK の MOD_ 名, 値, ラッパ名)]。"""
        bits = modifier_key.code
        out = []
        for bit in sorted(KMK_MOD_TO_QMK):
            if bits & bit:
                out.append(KMK_MOD_TO_QMK[bit])
        if not out:
            raise SystemExit('修飾キーのビットが空: %r' % modifier_key)
        left = sum(1 for _n, value, _w in out if not value & 0x10)
        if left != len(out) and left != 0:
            raise SystemExit('左右混在の修飾キーには対応していない: %r' % modifier_key)
        return out

    # ---- 本体 ------------------------------------------------------------
    def convert(self, key):
        from kmk.keys import KeyboardKey, ModifiedKey, ModifierKey
        from kmk.modules.holdtap import HoldTapKey
        from kmk.modules.layers import LayerKey

        # 素の Key (TRNS / NO / RESET / 独自キー)
        ident = id(key)
        if ident in self.by_identity:
            name = self.by_identity[ident]
            if name == 'TRNS':
                return Converted(0x0001, 'KC_TRNS')
            if name == 'NO':
                return Converted(0x0000, 'KC_NO')
            expr, code = KMK_BUILTIN_TO_QMK[name]
            return Converted(code, expr)
        if ident in self.custom_by_identity:
            expr, code = self.custom_by_identity[ident]
            return Converted(code, expr)
        if ident in self.mouse_by_identity:
            expr, code = self.mouse_by_identity[ident]
            return Converted(code, expr)

        if isinstance(key, HoldTapKey):
            return self.convert_holdtap(key)
        if isinstance(key, LayerKey):
            return self.convert_layer(key)
        if isinstance(key, ModifiedKey):
            return self.convert_modified(key)
        if isinstance(key, ModifierKey):
            return Converted(key.code and self.mod_only(key) or 0, self.mod_only_expr(key))
        if isinstance(key, KeyboardKey):
            name = self.basic_name(key.code)
            return Converted(key.code, name)

        raise SystemExit('変換できないキー: %r (%s)' % (key, type(key).__name__))

    def mod_only(self, key):
        mods = self.mods_of(key)
        code = 0
        for _name, value, _wrap in mods:
            code |= value
        # 単独の修飾キーは HID の使用番号 (0xE0..0xE7) で表す。
        base = {0x01: 0xE0, 0x02: 0xE1, 0x04: 0xE2, 0x08: 0xE3,
                0x11: 0xE4, 0x12: 0xE5, 0x14: 0xE6, 0x18: 0xE7}
        if code not in base:
            raise SystemExit('単独で押す複合修飾キーには対応していない: %r' % key)
        return base[code]

    def mod_only_expr(self, key):
        return self.basic_name(self.mod_only(key))

    def convert_modified(self, key):
        inner = self.convert(key.key)
        if inner.code > 0xFF:
            raise SystemExit('修飾キーの中に修飾キーは入れられない: %r' % key)
        code = inner.code
        expr = inner.expr
        for _mod_name, value, wrap in reversed(self.mods_of(key.modifier)):
            code |= value << 8
            expr = '%s(%s)' % (wrap, expr)
        return Converted(code, expr)

    def convert_layer(self, key):
        handler = getattr(key, '_on_press', None)
        handler_name = getattr(getattr(handler, '__func__', handler), '__name__', '')
        if handler_name != '_mo_pressed':
            raise SystemExit('MO 以外のレイヤーキーには対応していない: %r (%s)'
                             % (key, handler_name))
        if key.layer > 0x1F:
            raise SystemExit('レイヤー番号が大きすぎる: %r' % key)
        return Converted(0x5220 | key.layer, 'MO(%d)' % key.layer)

    def convert_holdtap(self, key):
        from kmk.modules.layers import LayerKey

        tap = self.convert(key.tap)
        hold_on_other = bool(key.prefer_hold)
        # KMK は tap_interrupted=True のとき「割り込んだキーが離れた時点で
        # hold にする」= QMK の PERMISSIVE_HOLD。keymap.py は全キー False。
        permissive = bool(key.tap_interrupted)

        if isinstance(key.hold, LayerKey):
            layer = self.convert_layer(key.hold)
            layer_num = layer.code & 0x1F
            if tap.code > 0xFF:
                raise SystemExit('LT の tap が 1 バイトに収まらない: %r' % key)
            if layer_num > 0xF:
                raise SystemExit('LT のレイヤー番号が大きすぎる: %r' % key)
            term = key.tap_time if key.tap_time is not None else KMK_LT_TAP_TIME
            return Converted(0x4000 | (layer_num << 8) | tap.code,
                             'LT(%d, %s)' % (layer_num, tap.expr),
                             kind='lt',
                             tapping_term=QMK_LT_TAPPING_TERM if term == KMK_LT_TAP_TIME else term,
                             hold_on_other_key_press=hold_on_other,
                             permissive_hold=permissive)

        mods = self.mods_of(key.hold)
        mod_value = 0
        mod_expr_parts = []
        for name, value, _wrap in mods:
            mod_value |= value
            mod_expr_parts.append(name)
        mod_expr = ' | '.join(mod_expr_parts)
        term = key.tap_time if key.tap_time is not None else KMK_HT_TAP_TIME
        term = QMK_MT_TAPPING_TERM if term == KMK_HT_TAP_TIME else term

        if tap.code <= 0xFF:
            return Converted(0x2000 | ((mod_value & 0x1F) << 8) | tap.code,
                             'MT(%s, %s)' % (mod_expr, tap.expr),
                             kind='mt',
                             tapping_term=term,
                             hold_on_other_key_press=hold_on_other,
                             permissive_hold=permissive)

        # ★ tap が修飾つき (例: KC.HT(KC.LSFT(KC.SCLN), KC.LSFT)) の場合、
        #   QMK の MT() は tap を 1 バイトしか持てないので表現できない。
        #   独自キーコードに逃がし、qmk_port/stackee_holdtap.c の小さな
        #   状態機械で「hold = 修飾 / tap = 16bit キーコード」を実装する。
        index = len(self.ext_mods)
        self.ext_mods.append((mod_expr, mod_value, tap.expr, tap.code, hold_on_other, term))
        name = 'STK_MT_%d' % index
        return Converted(QK_KB_0 + N_FIXED_CUSTOM + index, name,
                         kind='ext_mt',
                         tapping_term=term,
                         hold_on_other_key_press=hold_on_other,
                         permissive_hold=permissive)


# --------------------------------------------------------------------------
# 生成
# --------------------------------------------------------------------------
def build(keymap_module, converter):
    """[layer][row][col] の Converted と、キーごとの HoldTap 設定を返す。

    HoldTap の設定は **マトリクス位置 + キーコード** で覚える。同じキーコード
    でも設定が違うことがあるため (レイヤー 0 のキー 36 と 37 はどちらも
    LT(1, LANG1) だが prefer_hold が違う)。キーコードだけで表を引くと
    片方の設定が黙って消える。
    """
    assign = keymap_module.ASSIGN
    order = keymap_module.KEY_ORDER
    grid = []
    holdtap = []
    seen = set()
    for layer in keymap_module.KEYMAP:
        rows = [[Converted(0x0000, 'KC_NO') for _ in range(N_COLS)]
                for _ in range(N_ROWS)]
        for position, key in enumerate(layer):
            row, col = assign[order[position]]
            conv = converter.convert(key)
            rows[row][col] = conv
            if conv.kind not in ('mt', 'lt', 'ext_mt'):
                continue
            entry = (row, col, conv.code)
            if entry in seen:
                continue
            seen.add(entry)
            conv.row = row
            conv.col = col
            holdtap.append(conv)
        grid.append(rows)
    return grid, holdtap


def c_matrix(grid):
    out = []
    names = ('BASE', '記号 (JIS)', '記号 2 / 修飾', 'ナビ', 'F キー', 'システム')
    for layer_index, rows in enumerate(grid):
        label = names[layer_index] if layer_index < len(names) else ''
        out.append('    // ---- レイヤー %d%s ----' % (layer_index, (': ' + label) if label else ''))
        out.append('    [%d] = {' % layer_index)
        for row_index, row in enumerate(rows):
            cells = ', '.join('%-22s' % cell.expr for cell in row)
            out.append('        /* row %d */ { %s },' % (row_index, cells.rstrip()))
        out.append('    },')
    return '\n'.join(out)


def c_holdtap_table(holdtap):
    lines = []
    for conv in holdtap:
        lines.append('    { %d, %d, %-26s, %3d, %-5s, %-5s },'
                     % (conv.row, conv.col, conv.expr, conv.tapping_term,
                        'true' if conv.hold_on_other_key_press else 'false',
                        'true' if conv.permissive_hold else 'false'))
    return '\n'.join(lines)


def c_ext_mt_table(ext_mods):
    lines = []
    for mod_expr, _mod_value, tap_expr, _tap_code, prefer_hold, term in ext_mods:
        lines.append('    { %-24s, %-22s, %3d, %-5s },'
                     % (mod_expr, tap_expr, term,
                        'true' if prefer_hold else 'false'))
    return '\n'.join(lines)


HEADER = '''// ★ 生成物。手で編集しない。
//
//     python3 firmware/tools/gen_keymap.py
//
// 出所は firmware/kmk/keymap.py (KEYMAP / ASSIGN / COORD_MAPPING)。
// KMK と QMK の対応表は firmware/tools/keycodes.md。
//
// 配線があるのは 5x10 = 50 スロットのうち 43 個。残り 7 個は KC_NO で埋めて
// ある (そこからイベントが来ることは無い。来たら配線異常)。
#include "quantum.h"

#include "stackee_keycodes.h"

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
%(matrix)s
};

// --------------------------------------------------------------------------
// HoldTap のキーごとの設定 (KMK の prefer_hold / tap_interrupted / tap_time)
// --------------------------------------------------------------------------
// ★ 位置 (row/col) とキーコードの両方を持つ。同じキーコードでも設定が違う
//   ことがあるため: レイヤー 0 のキー 36 と 37 はどちらも LT(1, LANG1) だが、
//   36 だけ prefer_hold=True。キーコードだけで引くと片方が黙って消える。
typedef struct {
    uint8_t  row;
    uint8_t  col;
    uint16_t keycode;
    uint16_t tapping_term;
    bool     hold_on_other_key_press;   // KMK の prefer_hold
    bool     permissive_hold;           // KMK の tap_interrupted
} stackee_holdtap_t;

static const stackee_holdtap_t s_holdtap[] = {
%(holdtap)s
};

#define STACKEE_HOLDTAP_COUNT (sizeof(s_holdtap) / sizeof(s_holdtap[0]))

// 引き方は 2 段:
//   1. 位置とキーコードが両方一致するもの (= 既定配列のまま使っている)
//   2. キーコードだけ一致するもの (= VIA でキーを別の場所へ動かした場合)
// どちらも無ければ QMK の既定 (TAPPING_TERM / false / false)。
static const stackee_holdtap_t *find_holdtap(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *by_keycode = NULL;
    for (size_t i = 0; i < STACKEE_HOLDTAP_COUNT; i++) {
        if (s_holdtap[i].keycode != keycode) {
            continue;
        }
        if (record != NULL && s_holdtap[i].row == record->event.key.row &&
            s_holdtap[i].col == record->event.key.col) {
            return &s_holdtap[i];
        }
        if (by_keycode == NULL) {
            by_keycode = &s_holdtap[i];
        }
    }
    return by_keycode;
}

uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *entry = find_holdtap(keycode, record);
    return entry ? entry->tapping_term : TAPPING_TERM;
}

bool get_hold_on_other_key_press(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *entry = find_holdtap(keycode, record);
    return entry ? entry->hold_on_other_key_press : false;
}

bool get_permissive_hold(uint16_t keycode, keyrecord_t *record) {
    const stackee_holdtap_t *entry = find_holdtap(keycode, record);
    return entry ? entry->permissive_hold : false;
}

// --------------------------------------------------------------------------
// 修飾つきタップの HoldTap (QMK の MT() では表せないもの)
// --------------------------------------------------------------------------
// 例: KMK の KC.HT(KC.LSFT(KC.SCLN), KC.LSFT) — 離せば「+」(JIS)、押さえれば
// Shift。QMK の MT() はタップ側を 1 バイトしか持てないので、独自キーコードに
// 逃がして qmk_port/stackee_holdtap.c が処理する。
const stackee_ext_mt_t stackee_ext_mt[] = {
%(ext_mt)s
};

const uint8_t stackee_ext_mt_count = %(ext_mt_count)d;
'''


def render_keymap_c(grid, holdtap, ext_mods):
    return HEADER % {
        'matrix': c_matrix(grid),
        'holdtap': c_holdtap_table(holdtap),
        'ext_mt': c_ext_mt_table(ext_mods),
        'ext_mt_count': len(ext_mods),
    }


def render_keycodes_h(ext_mods):
    lines = [
        '// ★ 生成物。手で編集しない (tools/gen_keymap.py)。',
        '//',
        '// Stackee の独自キーコード。VIA の customKeycodes は QK_KB_0 (0x7E00) から',
        '// 順に並ぶ約束なので、この並び順と via/stackee.json の並び順は一致させる',
        '// こと (tools/test_gen_keymap.py が確かめる)。',
        '#pragma once',
        '',
        '#include <stdbool.h>',
        '#include <stdint.h>',
        '',
        '#include "quantum.h"',
        '',
        'enum stackee_keycodes {',
    ]
    for index, (name, _kmk, _title, _short) in enumerate(CUSTOM_KEYS):
        lines.append('    %-20s = QK_KB_0 + %d,' % (name, index))
    for index in range(len(ext_mods)):
        lines.append('    %-20s = QK_KB_0 + %d,'
                     % ('STK_MT_%d' % index, N_FIXED_CUSTOM + index))
    lines += [
        '};',
        '',
        '#define STACKEE_KEYCODE_FIRST QK_KB_0',
        '#define STACKEE_KEYCODE_LAST  (QK_KB_0 + %d)' % (N_FIXED_CUSTOM + len(ext_mods) - 1),
        '#define STK_MT_BASE           (QK_KB_0 + %d)' % N_FIXED_CUSTOM,
        '',
        '// 修飾つきタップの HoldTap (default_keymap.c が中身を持つ)。',
        'typedef struct {',
        '    uint8_t  mods;          // hold で押さえる修飾キー (MOD_*)',
        '    uint16_t tap;           // tap で送るキーコード (16bit)',
        '    uint16_t tapping_term;',
        '    bool     hold_on_other_key_press;',
        '} stackee_ext_mt_t;',
        '',
        'extern const stackee_ext_mt_t stackee_ext_mt[];',
        'extern const uint8_t stackee_ext_mt_count;',
        '',
    ]
    return '\n'.join(lines)


# --------------------------------------------------------------------------
# VIA 定義 JSON
# --------------------------------------------------------------------------
def load_kle(path=KLE_PATH):
    with open(path, encoding='utf-8') as handle:
        return json.load(handle)


def render_via(keymap_module, ext_mods, kle=None):
    """KLE の凡例 (キー番号) を VIA の "row,col" に差し替える。"""
    kle = kle if kle is not None else load_kle()
    assign = keymap_module.ASSIGN
    rows = []
    for kle_row in kle:
        out_row = []
        for item in kle_row:
            if isinstance(item, dict):
                out_row.append(item)
                continue
            number = int(str(item).split('\n')[0].strip())
            row, col = assign[number]
            out_row.append('%d,%d' % (row, col))
        rows.append(out_row)

    custom = []
    for name, _kmk, title, short in CUSTOM_KEYS:
        custom.append({'name': name, 'title': title, 'shortName': short})
    for index, (mod_expr, _mv, tap_expr, _tc, _ph, _term) in enumerate(ext_mods):
        custom.append({
            'name': 'STK_MT_%d' % index,
            'title': '押さえて %s / 離して %s' % (mod_expr, tap_expr),
            'shortName': 'MT%d' % index,
        })

    return {
        'name': 'Stackee',
        'vendorId': '0x%04X' % 0x303A,
        'productId': '0x%04X' % 0x811A,
        'matrix': {'rows': N_ROWS, 'cols': N_COLS},
        'layouts': {'keymap': rows},
        'customKeycodes': custom,
    }


# --------------------------------------------------------------------------
def generate():
    """(default_keymap.c, stackee_keycodes.h, via の dict) を返す。"""
    KC, keymap_module = load_kmk()
    converter = Converter(KC, load_qmk_basic_names())
    grid, holdtap = build(keymap_module, converter)
    return (render_keymap_c(grid, holdtap, converter.ext_mods),
            render_keycodes_h(converter.ext_mods),
            render_via(keymap_module, converter.ext_mods),
            grid, converter)


def write_if_needed(path, text, check):
    old = None
    if os.path.exists(path):
        with open(path, encoding='utf-8') as handle:
            old = handle.read()
    if old == text:
        return False
    if check:
        raise SystemExit('生成物が古い: %s' % path)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as handle:
        handle.write(text)
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='書き出さず、生成物が最新かどうかだけ見る')
    args = parser.parse_args()

    keymap_c, keycodes_h, via, _grid, _conv = generate()
    via_text = json.dumps(via, ensure_ascii=False, indent=2) + '\n'

    changed = []
    for path, text in ((OUT_KEYMAP, keymap_c),
                       (os.path.join(IDF, 'main', 'qmk_port', 'stackee_keycodes.h'),
                        keycodes_h),
                       (OUT_VIA, via_text)):
        if write_if_needed(path, text, args.check):
            changed.append(path)

    if args.check:
        print('生成物は最新')
    else:
        for path in changed:
            print('書き出した: %s' % os.path.relpath(path, REPO))
        if not changed:
            print('変更なし')
    return 0


if __name__ == '__main__':
    sys.exit(main())
