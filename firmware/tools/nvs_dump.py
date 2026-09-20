#!/usr/bin/env python3
"""nvs パーティションの生イメージを読んで、中身の一覧を出す。実機に触らない。

    # 実機から吸い出す (これは人がやる。読むだけで、書き込みはしない)
    python -m esptool --chip esp32s3 -p <port> --before no-reset --after watchdog-reset \\
        read-flash 0x9000 0x5000 nvs.bin

    # 中身を見る
    python3 firmware/tools/nvs_dump.py nvs.bin
    python3 firmware/tools/nvs_dump.py nvs.bin --json
    python3 firmware/tools/nvs_dump.py nvs.bin --ns nimble_bond

何のためにあるか
----------------
段階 1b で BLE を入れるにあたり、**現行 CircuitPython 版が Mac と作った
ボンドをそのまま引き継げるか**を、実機を触らずに判断したい。

CircuitPython (ports/espressif/common-hal/_bleio/Adapter.c) も、こちらの
ファームも、NimBLE の `store/config` を使う。その保存先は nvs の
名前空間 **"nimble_bond"** で、キーは

    our_sec    こちらの鍵 (LTK など)
    peer_sec   相手の鍵
    cccd_sec   相手がどの通知を有効にしたか (**属性ハンドル依存**)
    p_dev_rec  ペア済み機器の一覧
    local_irk  こちらの IRK

(出所: components/bt/host/nimble/nimble/nimble/host/store/config/src/
 ble_store_nvs.c の NIMBLE_NVS_* 定義)

このうち our_sec / peer_sec / local_irk / p_dev_rec は GATT の中身に依存
しないので、同じ NimBLE・同じ nvs なら**そのまま効く**はず。
cccd_sec だけは属性ハンドルに依存するので、GATT の構成が変わると
ずれる可能性がある。ダンプを見れば、何が何件入っているかが分かる。

NVS の形式
----------
一次情報: ESP-IDF プログラミングガイド「Non-volatile storage library」の
Structure of a page / Entry。ページは 4096 バイト、先頭 32 バイトがページ
ヘッダ、次の 32 バイトがエントリ状態のビットマップ (2 ビット x 126)、
残りが 32 バイトのエントリ 126 個。
"""
import argparse
import binascii
import json
import struct
import sys

PAGE_SIZE = 4096
ENTRY_SIZE = 32
ENTRIES_PER_PAGE = 126

PAGE_STATE = {
    0xFFFFFFFF: 'uninitialized',
    0xFFFFFFFE: 'active',
    0xFFFFFFFC: 'full',
    0xFFFFFFF8: 'freeing',
    0xFFFFFFF0: 'corrupt',
}

ENTRY_STATE = {0b11: 'empty', 0b10: 'written', 0b00: 'erased', 0b01: 'illegal'}

TYPES = {
    0x01: 'u8', 0x11: 'i8', 0x02: 'u16', 0x12: 'i16',
    0x04: 'u32', 0x14: 'i32', 0x08: 'u64', 0x18: 'i64',
    0x21: 'str', 0x41: 'blob_data', 0x42: 'blob', 0x48: 'blob_idx',
}

FIXED_SIZES = {'u8': 1, 'i8': 1, 'u16': 2, 'i16': 2,
               'u32': 4, 'i32': 4, 'u64': 8, 'i64': 8}


def entry_states(bitmap):
    out = []
    for index in range(ENTRIES_PER_PAGE):
        byte = bitmap[index // 4]
        bits = (byte >> ((index % 4) * 2)) & 0b11
        out.append(ENTRY_STATE.get(bits, 'illegal'))
    return out


def parse_page(page):
    """1 ページぶんの (state, seq, エントリのリスト) を返す。"""
    state, seq = struct.unpack('<II', page[0:8])
    bitmap = page[32:64]
    states = entry_states(bitmap)
    body = page[64:]

    entries = []
    index = 0
    while index < ENTRIES_PER_PAGE:
        if states[index] != 'written':
            index += 1
            continue
        raw = body[index * ENTRY_SIZE:(index + 1) * ENTRY_SIZE]
        if len(raw) < ENTRY_SIZE:
            break
        ns_index, type_id, span, chunk_index = raw[0], raw[1], raw[2], raw[3]
        key = raw[8:24].split(b'\x00')[0].decode('utf-8', 'replace')
        kind = TYPES.get(type_id)
        entry = {
            'ns_index': ns_index,
            'type': kind if kind else '0x%02X' % type_id,
            'key': key,
            'span': span,
            'chunk_index': chunk_index,
        }
        if kind in FIXED_SIZES:
            entry['value'] = int.from_bytes(raw[24:24 + FIXED_SIZES[kind]],
                                            'little')
        elif kind in ('str', 'blob_data', 'blob'):
            size = struct.unpack('<H', raw[24:26])[0]
            entry['size'] = size
            data = body[(index + 1) * ENTRY_SIZE:
                        (index + 1) * ENTRY_SIZE + size]
            entry['data'] = data
        elif kind == 'blob_idx':
            entry['size'] = struct.unpack('<I', raw[24:28])[0]
            entry['chunk_count'] = raw[28]
        entries.append(entry)
        index += max(1, span)
    return PAGE_STATE.get(state, '0x%08X' % state), seq, entries


def parse(image):
    """(名前空間の表, エントリのリスト) を返す。"""
    namespaces = {}
    entries = []
    for offset in range(0, len(image), PAGE_SIZE):
        page = image[offset:offset + PAGE_SIZE]
        if len(page) < PAGE_SIZE:
            break
        state, seq, found = parse_page(page)
        if state in ('uninitialized', 'corrupt'):
            continue
        for entry in found:
            entry['page'] = offset // PAGE_SIZE
            entry['page_state'] = state
            entry['page_seq'] = seq
            if entry['ns_index'] == 0 and entry['type'] == 'u8':
                # 名前空間そのものの登録 (ns_index 0 の u8 = 番号)
                namespaces[entry['value']] = entry['key']
            else:
                entries.append(entry)
    return namespaces, entries


# ---------------------------------------------------------------------------
# NimBLE のボンド
# ---------------------------------------------------------------------------
NIMBLE_KEYS = {
    'our_sec': 'こちらの鍵 (LTK ほか)。GATT の中身に依存しない',
    'peer_sec': '相手の鍵。GATT の中身に依存しない',
    'cccd_sec': '相手が有効にした通知。★ 属性ハンドルに依存する',
    'csfc_sec': 'クライアント対応機能',
    'p_dev_rec': 'ペア済み機器の一覧',
    'local_irk': 'こちらの IRK',
    'ead_sec': '暗号化アドバタイズ用',
    'rpa_rec': 'ランダムアドレスの記録',
}


def summarize_nimble(namespaces, entries):
    """nimble_bond の中身を人間向けにまとめる。"""
    index = None
    for number, name in namespaces.items():
        if name == 'nimble_bond':
            index = number
    if index is None:
        return None
    rows = [e for e in entries if e['ns_index'] == index]
    counts = {}
    for entry in rows:
        # キーは "peer_sec_1" のように連番が付く。頭だけで数える。
        base = entry['key']
        for known in NIMBLE_KEYS:
            if base.startswith(known):
                base = known
                break
        counts[base] = counts.get(base, 0) + 1
    return {'ns_index': index, 'entries': len(rows), 'counts': counts,
            'keys': sorted(e['key'] for e in rows)}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('image', help='nvs パーティションの生イメージ')
    parser.add_argument('--ns', help='この名前空間だけ出す')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--hex', action='store_true',
                        help='blob / str の中身も 16 進で出す')
    args = parser.parse_args()

    with open(args.image, 'rb') as handle:
        image = handle.read()
    if len(image) % PAGE_SIZE:
        print('!! 長さが %d の倍数でない (%d バイト)。読み出しが途中かも'
              % (PAGE_SIZE, len(image)), file=sys.stderr)

    namespaces, entries = parse(image)
    nimble = summarize_nimble(namespaces, entries)

    if args.json:
        out = {
            'namespaces': namespaces,
            'entries': [{k: (binascii.hexlify(v).decode() if isinstance(v, bytes) else v)
                         for k, v in e.items()} for e in entries],
            'nimble_bond': nimble,
        }
        print(json.dumps(out, ensure_ascii=False, indent=2))
        return 0

    print('=== 名前空間 (%d 個) ===' % len(namespaces))
    for number in sorted(namespaces):
        rows = [e for e in entries if e['ns_index'] == number]
        print('  %2d  %-20s  エントリ %d 個' % (number, namespaces[number], len(rows)))

    print()
    print('=== エントリ ===')
    for entry in entries:
        name = namespaces.get(entry['ns_index'], '?')
        if args.ns and name != args.ns:
            continue
        line = '  [%s] %-20s %-10s' % (name, entry['key'], entry['type'])
        if 'value' in entry:
            line += ' = %d' % entry['value']
        if 'size' in entry:
            line += ' size=%d' % entry['size']
        print(line)
        if args.hex and isinstance(entry.get('data'), bytes):
            print('      ' + binascii.hexlify(entry['data']).decode())

    print()
    if nimble is None:
        print('=== NimBLE のボンド ===')
        print('  "nimble_bond" が無い。')
        print('  → CircuitPython 版でまだ一度もペアリングしていないか、')
        print('     ボンドを消した状態。引き継ぐものが無い。')
    else:
        print('=== NimBLE のボンド (nimble_bond) ===')
        for key in sorted(nimble['counts']):
            print('  %-12s %d 件   %s'
                  % (key, nimble['counts'][key], NIMBLE_KEYS.get(key, '')))
        print()
        print('  読み方:')
        print('   * peer_sec / our_sec が 1 件以上 → 鍵は残っている。')
        print('     こちらの NimBLE は同じ store/config・同じ名前空間を使うので、')
        print('     そのまま読める見込み。再ペアリングは要らない。')
        print('   * cccd_sec があると、相手が覚えている「通知の購読」が')
        print('     **古い属性ハンドル**を指している可能性がある')
        print('     (CircuitPython の HID サービスと esp_hid の GATT は構成が違う)。')
        print('     その場合は cccd_sec だけ消せば、鍵を残したまま')
        print('     購読をやり直させられる。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
