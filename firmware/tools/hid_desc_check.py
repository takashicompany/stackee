#!/usr/bin/env python3
"""main/stackee_usb.c の HID レポート記述子を読んで、構成を目で確かめる。

実機に書き込む前に、記述子が設計どおりかを机の上で確かめるためのもの。
見たいのは主に 2 点:

  * キーボードの Usage Maximum が 0x91 以上か
    (JIS の LANG1=0x90 かな / LANG2=0x91 英数 が入るか。現行 code.py が
     adafruit_ble の上限 0x89 を避けて自前記述子を使っているのと同じ理由)
  * Report ID の割り当てと、Raw HID のトップレベルコレクション 2 つ

  python3 firmware/tools/hid_desc_check.py           # 一覧を出す
  python3 firmware/tools/hid_desc_check.py --json    # 機械可読
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.normpath(os.path.join(HERE, '..', 'main', 'stackee_usb.c'))

ARRAYS = ('s_hid_keys_report', 's_hid_raw_report')

# HID 1.11 の短いアイテム。(bTag<<4)|bType → 名前。
ITEM_NAMES = {
    0x80: 'Input', 0x90: 'Output', 0xB0: 'Feature',
    0xA0: 'Collection', 0xC0: 'End Collection',
    0x04: 'Usage Page', 0x14: 'Logical Minimum', 0x24: 'Logical Maximum',
    0x34: 'Physical Minimum', 0x44: 'Physical Maximum', 0x54: 'Unit Exponent',
    0x64: 'Unit', 0x74: 'Report Size', 0x84: 'Report ID', 0x94: 'Report Count',
    0xA4: 'Push', 0xB4: 'Pop',
    0x08: 'Usage', 0x18: 'Usage Minimum', 0x28: 'Usage Maximum',
}


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def extract_arrays(path):
    """C のソースから記述子の配列を取り出す。

    プリプロセッサのマクロ (STACKEE_REPORT_ID_*) も同じファイルの
    #define から解決する。コメントは先に落とす。
    """
    with open(path, encoding='utf-8') as source:
        raw = source.read()
    defines = {}
    for name, value in re.findall(r'#define\s+(STACKEE_(?:REPORT_ID|RAW_HID_SIZE)\w*)\s+(\S+)', raw):
        defines[name] = value
    header = os.path.normpath(os.path.join(os.path.dirname(path), 'stackee_usb.h'))
    if os.path.exists(header):
        with open(header, encoding='utf-8') as source:
            head = source.read()
        for name, value in re.findall(r'#define\s+(STACKEE_(?:REPORT_ID|RAW_HID_SIZE)\w*)\s+(\S+)', head):
            defines.setdefault(name, value)

    text = strip_comments(raw)
    out = {}
    for name in ARRAYS:
        match = re.search(r'\b%s\[\]\s*=\s*\{(.*?)\n\};' % name, text, flags=re.S)
        if not match:
            raise SystemExit('%s が見つからない: %s' % (name, path))
        body = []
        for token in match.group(1).split(','):
            token = token.strip()
            if not token:
                continue
            if token in defines:
                token = defines[token]
            body.append(int(token, 0))
        out[name] = body
    return out


def parse(items):
    """短いアイテムだけ (長いアイテム 0xFE は使っていない) を順に読む。"""
    out = []
    i = 0
    while i < len(items):
        prefix = items[i]
        size = prefix & 0x03
        size = 4 if size == 3 else size
        tag = prefix & 0xFC
        data = items[i + 1:i + 1 + size]
        value = 0
        for shift, byte in enumerate(data):
            value |= byte << (8 * shift)
        out.append({'tag': tag, 'name': ITEM_NAMES.get(tag, '0x%02X' % tag),
                    'value': value, 'size': size})
        i += 1 + size
    return out


def summarize(name, items):
    """トップレベルコレクションごとに、Report ID と報告の大きさをまとめる。

    報告の大きさは Input/Output アイテムに出会うたびに、そのときの
    Report Size x Report Count を足して数える (HID の決まりどおり)。
    """
    parsed = parse(items)
    collections = []
    depth = 0
    usage_page = None
    pending_usage = None
    current = None
    size = count = 0
    report_id = None
    for item in parsed:
        tag = item['name']
        if tag == 'Usage Page':
            usage_page = item['value']
        elif tag == 'Usage' and depth == 0:
            pending_usage = item['value']
        elif tag == 'Collection':
            if depth == 0:
                current = {'usage_page': usage_page, 'usage': pending_usage,
                           'keyboard': usage_page == 0x01 and pending_usage == 0x06,
                           'report_ids': [], 'usage_max': None, 'reports': {}}
                collections.append(current)
                report_id = None
            depth += 1
        elif tag == 'End Collection':
            depth -= 1
        elif current is not None:
            if tag == 'Report ID':
                report_id = item['value']
                current['report_ids'].append(report_id)
            elif tag == 'Usage Maximum':
                prev = current['usage_max']
                if prev is None or item['value'] > prev:
                    current['usage_max'] = item['value']
            elif tag == 'Report Size':
                size = item['value']
            elif tag == 'Report Count':
                count = item['value']
            elif tag in ('Input', 'Output', 'Feature'):
                key = '%s/id%s' % (tag.lower(), report_id)
                current['reports'][key] = current['reports'].get(key, 0) + size * count
    return {'name': name, 'bytes': len(items), 'items': len(parsed),
            'collections': collections}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--source', default=SOURCE, help='読むソース (既定: main/stackee_usb.c)')
    ap.add_argument('--json', action='store_true', help='JSON で出す')
    args = ap.parse_args(argv)

    arrays = extract_arrays(args.source)
    reports = [summarize(name, arrays[name]) for name in ARRAYS]
    if args.json:
        print(json.dumps(reports, ensure_ascii=False, indent=2))
        return 0

    for report in reports:
        print('=== %s (%d バイト / %d アイテム)'
              % (report['name'], report['bytes'], report['items']))
        for col in report['collections']:
            print('  コレクション: Usage Page 0x%04X / Usage 0x%02X%s'
                  % (col['usage_page'], col['usage'] or 0,
                     '  ← キーボード' if col['keyboard'] else ''))
            print('    Report ID : %s' % (col['report_ids'] or 'なし'))
            if col['keyboard']:
                usage_max = col['usage_max']
                ok = usage_max is not None and usage_max >= 0x91
                print('    Usage Max : 0x%02X %s' % (
                    usage_max or 0,
                    '(JIS の LANG1 0x90 / LANG2 0x91 が入る)' if ok else
                    '★ 0x91 未満。JIS の LANG1/LANG2 が入らない'))
            for key in sorted(col['reports']):
                bits = col['reports'][key]
                print('    %-12s %d bit = %d バイト' % (key, bits, bits // 8))
    return 0


if __name__ == '__main__':
    sys.exit(main())
