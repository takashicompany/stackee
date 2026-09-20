#!/usr/bin/env python3
"""BLE が Mac に繋がるかを人手ゼロで確かめる。**本体には書き込まない。**

## なぜこれが要るか (2026-09-16 の実機)

ボンドは CircuitPython 版から引き継げていて、暗号化まで通る。なのに
**Mac が自動接続しない・打鍵も届かない**。Mac から bleak で明示的に繋ぐと
成功し、そのとき列挙されるサービスが `adaf0001`（Adafruit BLE のもの）だった
— つまり **macOS が CircuitPython 版の GATT の並びを覚えたまま**で、
こちらの esp_hid の GATT を読み直していない。HID のホストは覚えている古い
属性ハンドルに繋ごうとするので、繋がらない。

直し方は仕様どおり **GATT Service Changed の indication**。本体は暗号化が
済んだ時点で送る。ここでは「Mac 側のキャッシュが本当に入れ替わったか」を
bleak で確かめる。

## 見るもの

  1. スキャンで "stackee" が見える (RSSI と広告しているサービス)
  2. 繋いで列挙したサービスに **`adaf` で始まるものが無い**
     (= キャッシュが入れ替わった証拠。HID の 1812 は CoreBluetooth が
       隠すので、見えなくてよい)
  3. bleak を切ったあと、**Mac が自分から繋ぎ直す** (本体の status.ble)
  4. `key.inject` で `sent_ble` が増える (打鍵が BLE に出ている)

  python3 firmware/tools/check_ble.py
  python3 firmware/tools/check_ble.py --json
  python3 firmware/tools/check_ble.py --no-connect   # 1 と 3 と 4 だけ

★ Mac 側の操作は要らない。bleak は入っている前提 (pip install bleak)。
"""
import argparse
import asyncio
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stackee_tree as tree             # noqa: E402

if not tree.add_kmk_tools(sys.path):
    sys.exit('現行 CircuitPython 版の道具 (firmware/kmk/tools) が要る '
             '(枠の実装をそこから借りている)')
import stackee_console_client as ccl    # noqa: E402

BLE_NAME = 'stackee'
# CircuitPython (adafruit_ble) が使っていたサービスの頭。これが見えたら
# macOS はまだ古い GATT を覚えている。
STALE_PREFIX = 'adaf'
HID_SERVICE = '00001812'
INJECT_KEY = 'F24'


def status(client):
    return client.request('status', timeout=15.0)


async def scan(seconds):
    from bleak import BleakScanner
    found = {}
    def seen(device, adv):
        name = adv.local_name or device.name or ''
        if BLE_NAME.lower() not in name.lower():
            return
        found[device.address] = {
            'address': device.address,
            'name': name,
            'rssi': adv.rssi,
            'services': sorted(adv.service_uuids or []),
        }
    scanner = BleakScanner(detection_callback=seen)
    await scanner.start()
    began = time.time()
    while time.time() - began < seconds and not found:
        await asyncio.sleep(0.5)
    # 1 つ見つかっても、RSSI が落ち着くまで少しだけ待つ。
    await asyncio.sleep(0.5)
    await scanner.stop()
    return list(found.values())


async def connect_and_list(address, timeout=25.0):
    from bleak import BleakClient
    out = {'address': address}
    async with BleakClient(address, timeout=timeout) as client:
        out['connected'] = client.is_connected
        services = []
        for service in client.services:
            services.append({'uuid': str(service.uuid),
                             'description': service.description})
        out['services'] = services
        # 少し保持してから切る (macOS がキャッシュを書き換える間)。
        await asyncio.sleep(2.0)
    return out


def wait_for_auto_connect(client, seconds, base_conn=0, verbose=True):
    """切断のあと、Mac が**自分から**繋ぎ直すのを待つ。

    ★ `status.ble` だけでなく `blex.conn` が増えたかも見る。bleak が繋いだ
      ぶんを数え込まないため (base_conn がその基準)。
    """
    began = time.time()
    last = {}
    while time.time() - began < seconds:
        last = status(client)
        blex = last.get('blex') or {}
        if last.get('ble') and (blex.get('conn') or 0) > base_conn:
            break
        if verbose:
            sys.stderr.write('\r  Mac からの自動接続を待つ %.0fs ...'
                             % (time.time() - began))
            sys.stderr.flush()
        time.sleep(2.0)
    if verbose:
        sys.stderr.write('\r' + ' ' * 50 + '\r')
    blex = last.get('blex') or {}
    ok = bool(last.get('ble')) and (blex.get('conn') or 0) > base_conn
    return ok, round(time.time() - began, 1), last


def collect(args):
    result = {}
    client = ccl.ConsoleClient(port=args.port)
    result['port'] = client.port_name
    try:
        # ★ 打鍵の行き先を BLE に固定する。USB のままだと sent_ble が
        #   増えないので、この検査そのものが意味を持たない。
        if args.set_ble:
            result['hid_set'] = client.request('hid.set', timeout=10.0,
                                               dest='BLE')
        before = status(client)
        result['status_before'] = before
        result['blex_before'] = before.get('blex')

        result['scan'] = asyncio.run(scan(args.scan_seconds))
        if result['scan'] and not args.no_connect:
            address = result['scan'][0]['address']
            try:
                result['gatt'] = asyncio.run(connect_and_list(address))
            except Exception as err:
                result['gatt_error'] = '%s: %s' % (type(err).__name__, err)
            # bleak が握っている間に本体側の数字を読む余地は無いので、
            # 切れたあとに読む。
            result['status_after_gatt'] = status(client)

        # bleak が繋いだぶんを基準にする。これより増えたら「Mac が自分から」。
        seen_blex = (result.get('status_after_gatt') or
                     result.get('status_before') or {}).get('blex') or {}
        base_conn = seen_blex.get('conn') or 0
        result['base_conn'] = base_conn
        ok, waited, last = wait_for_auto_connect(client, args.wait,
                                                 base_conn=base_conn,
                                                 verbose=not args.json)
        result['auto_connected'] = ok
        result['auto_connect_s'] = waited
        result['status_after'] = last
        result['blex_after'] = last.get('blex')

        if ok:
            reply = client.request('key.inject', timeout=10.0, kc=INJECT_KEY,
                                   hold_ms=20)
            result['inject'] = reply
    finally:
        client.close()
    return result


def verdicts(result):
    out = []
    before = result.get('status_before') or {}
    out.append(('送信先', (result.get('status_after') or before).get('hid_sel') == 'BLE',
                'hid_sel=%s / hid=%s'
                % ((result.get('status_after') or before).get('hid_sel'),
                   (result.get('status_after') or before).get('hid'))))

    seen = result.get('scan') or []
    out.append(('スキャンで見えるか', bool(seen),
                '; '.join('%s %s RSSI %s %s'
                          % (s['name'], s['address'], s['rssi'], s['services'])
                          for s in seen) or '見つからない'))
    if seen:
        advertised = ' '.join(' '.join(s['services']).lower() for s in seen)
        out.append(('HID を広告しているか', HID_SERVICE in advertised,
                    '広告のサービス一覧に %s があるか' % HID_SERVICE))

    gatt = result.get('gatt')
    if gatt is not None:
        uuids = [s['uuid'].lower() for s in gatt.get('services', [])]
        stale = [u for u in uuids if u.startswith(STALE_PREFIX)]
        out.append(('GATT キャッシュ', not stale,
                    '%d 個: %s%s'
                    % (len(uuids), uuids,
                       ' ★ 古い並びが残っている (%s)' % stale if stale else
                       ' (adaf* は無い = 読み直された)')))
    elif result.get('gatt_error'):
        out.append(('GATT キャッシュ', None, result['gatt_error']))

    blex = result.get('blex_after') or {}
    if blex:
        out.append(('Service Changed',
                    (blex.get('svc_changed_sent') or 0) > 0,
                    'handle %s / 送った %s 回 / 受け取られた %s 回 / rc %s'
                    % (blex.get('svc_changed_handle'),
                       blex.get('svc_changed_sent'),
                       blex.get('svc_changed_acked'),
                       blex.get('svc_changed_rc'))))

    if blex:
        out.append(('アドバタイズの撒き方', blex.get('adv_kind') != 'off',
                    'いま %s / 名指しで撒いた %s 回 / 名指しの相手 %s'
                    % (blex.get('adv_kind'), blex.get('adv_directed'),
                       'あり' if blex.get('bond_peer') else 'なし')))

    out.append(('Mac が自分から繋ぎ直すか', result.get('auto_connected'),
                '%s (%.0f 秒待った)。接続 %s 回 (基準 %s) / 切断 %s 回'
                % ('繋がった' if result.get('auto_connected') else '繋がらない',
                   result.get('auto_connect_s', 0),
                   blex.get('conn'), result.get('base_conn'), blex.get('disc'))))

    inject = result.get('inject')
    if inject is not None:
        out.append(('BLE へ打鍵が出るか',
                    bool(inject.get('ok')) and (inject.get('sent_ble') or 0) > 0,
                    'dest %s / 積んだ %s / BLE へ %s / USB へ %s / 押下 %s ms'
                    % (inject.get('dest'), inject.get('pushed'),
                       inject.get('sent_ble'), inject.get('sent_usb'),
                       inject.get('press_ms'))))
    return out


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port')
    parser.add_argument('--scan-seconds', type=float, default=15.0)
    parser.add_argument('--wait', type=float, default=30.0,
                        help='Mac の自動接続を待つ上限 [秒]。名指しの広告は '
                             '1.28 秒 + 30 秒なので、ここは 30 秒で足りる')
    parser.add_argument('--no-connect', action='store_true',
                        help='bleak で繋がない (スキャンと自動接続だけ見る)')
    parser.add_argument('--no-set-ble', dest='set_ble', action='store_false',
                        help='送信先を BLE に固定しない')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()

    result = collect(args)
    checks = verdicts(result)
    if args.json:
        print(json.dumps({'result': result,
                          'verdicts': [{'name': n, 'ok': o, 'detail': d}
                                       for n, o, d in checks]},
                         ensure_ascii=False, indent=2))
    else:
        print('ポート: %s' % result.get('port'))
        for name, ok, detail in checks:
            mark = '--' if ok is None else ('OK' if ok else 'NG')
            print('[%s] %-24s %s' % (mark, name, detail))
        if not result.get('auto_connected'):
            print('\n★ ここまでやっても Mac が繋ぎ直さない場合、残る手は\n'
                  '  **Mac 側で Bluetooth のペアリングを削除して入れ直す**\n'
                  '  (システム設定 → Bluetooth → stackee → 削除)。\n'
                  '  これはユーザーの操作が要る。')
    return 0 if all(ok is not False for _n, ok, _d in checks) else 1


if __name__ == '__main__':
    sys.exit(main())
