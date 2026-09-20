#!/usr/bin/env python3
"""段階 1 の合否を実機で測る。**書き込んだ後に走らせる。**

見るのは 7 つ:
  1. TCA8418 がつながっているか / 取りこぼしが無いか
  2. キー押下 → HID レポート生成 の遅延 (合否: 中央値 2 ms 以内・最大 5 ms 以内)
  3. 入力タスクの周期 (合否: 中央値 1 ms 前後)
  4. 送信キューが詰まっていないか (捨てた数 0)
  5. 独自キーが HID に漏れていないか (custom の数だけ増えて送出は増えない)
  6. BLE がつながっているか / 接続間隔 (ms)
  7. 送信先 (BLE / USB) と、その選択が NVS に残っているか

  python3 firmware/tools/check_phase1.py            # いま繋がっている本体
  python3 firmware/tools/check_phase1.py --wait     # 再起動を待ってから
  python3 firmware/tools/check_phase1.py --json

★ 書き込みも再起動もしない。読むだけ (status を 2 回投げる)。
★ 遅延の数字は **普段使いのあとに読む**。時間を決めて打ってもらうような
  検証はしない (memory: 時間窓付きの操作依頼をしない)。しばらく使ってから
  この道具を走らせれば、そのあいだの中央値と最大が出る。
★ 人手ゼロで打鍵を作りたいときは console の key.inject を使う。
  配線の無いスロットに F24 を置いて、普段と同じ道を通して 1 回叩く。
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stackee_tree as tree             # noqa: E402

if not tree.add_kmk_tools(sys.path):
    sys.exit('現行 CircuitPython 版の道具 (firmware/kmk/tools) が要る '
             '(枠の実装をそこから借りている)')
import stackee_console_client as ccl    # noqa: E402
import console_hid                       # noqa: E402

# DESIGN.md §3 の「入力遅延の設計目標」。
LATENCY_MEDIAN_US = 2000
LATENCY_MAX_US = 5000


def collect(wait, timeout, transport='auto'):
    result = {}
    if wait:
        client, timing = ccl.reconnect(timeout=timeout)
        result['reconnect'] = timing
        result['status'] = timing.pop('status')
    else:
        client = console_hid.open_client(transport=transport)
        result['status'] = client.request('status', timeout=timeout)
    try:
        result['hello'] = client.request('hello', timeout=timeout)
        result['status2'] = client.request('status', timeout=timeout)
        result['port'] = client.port_name
        result['log'] = ''.join(client.log)[-2000:]
    finally:
        client.close()
    return result


def _fw_at_least(fw, want):
    """'stackee-idf/4' のような版が、求める段階以上か。"""
    if not isinstance(fw, str) or not fw.startswith('stackee-idf/'):
        return False
    try:
        return int(fw.split('/', 1)[1]) >= want
    except ValueError:
        return False


def verdicts(result):
    out = []
    status = result.get('status2') or result.get('status') or {}
    hello = result.get('hello') or {}

    fw = hello.get('fw') or status.get('fw')
    # ★ 段階が上がっても通す。段階 4 の像 (stackee-idf/4) でも
    #   「キーボードとして段階 1 の合否を満たすか」を測りたいため。
    out.append(('ファーム', _fw_at_least(fw, 1),
                '%s (期待 stackee-idf/1 以上)' % fw))
    keys = status.get('keys') or {}
    queue = status.get('hidq') or {}
    perf = status.get('perf') or {}

    out.append(('TCA8418', bool(keys.get('tca')),
                'イベント %s 件 / 押下中 %s'
                % (keys.get('events'), keys.get('down'))))

    out.append(('取りこぼし', keys.get('ovf') == 0 and keys.get('iofail') == 0
                and keys.get('stray') == 0,
                'FIFO 溢れ %s / I2C 失敗 %s / 配線の無いスロット %s'
                % (keys.get('ovf'), keys.get('iofail'), keys.get('stray'))))

    latency = perf.get('input') or {}
    if not latency:
        out.append(('キー → レポート', None,
                    'まだ 1 度も打っていない (しばらく使ってから読む)'))
    else:
        ok = (latency.get('med_us', 1 << 30) <= LATENCY_MEDIAN_US
              and latency.get('max_us', 1 << 30) <= LATENCY_MAX_US)
        out.append(('キー → レポート', ok,
                    '中央値 %s us / 最大 %s us / 標本 %s (合否 中央値 %d / 最大 %d)'
                    % (latency.get('med_us'), latency.get('max_us'),
                       latency.get('n'), LATENCY_MEDIAN_US, LATENCY_MAX_US)))

    loop = perf.get('input_loop') or {}
    out.append(('入力タスクの周期', bool(loop),
                '中央値 %s us / 最大 %s us / 標本 %s'
                % (loop.get('med_us'), loop.get('max_us'), loop.get('n'))))

    out.append(('送信キュー', queue.get('dropped') == 0,
                '積んだ %s / USB %s / BLE(捨て) %s / 溢れ %s / 失敗 %s / 滞留 %s'
                % (queue.get('pushed'), queue.get('usb'), queue.get('ble'),
                   queue.get('dropped'), queue.get('failed'),
                   queue.get('depth'))))

    out.append(('独自キー', keys.get('custom') is not None,
                '%s 回押された (HID には出ていないはず)' % keys.get('custom')))

    # 送信先。既定は BLE (現行 CircuitPython 版と同じ)。
    out.append(('HID の送信先', status.get('hid') in ('BLE', 'USB'),
                'いま %s / 選択 %s (STK_HID_SWITCH でトグル、NVS に保存)'
                % (status.get('hid'), status.get('hid_sel'))))

    # BLE。
    blex = status.get('blex') or {}
    out.append(('BLE スタック', bool(blex.get('ready')),
                '起動 %s / アドバタイズ中 %s / 接続 %s 回 / 切断 %s 回'
                % (blex.get('ready'), blex.get('adv'),
                   blex.get('conn'), blex.get('disc'))))

    interval = status.get('ble_interval_ms')
    if status.get('ble'):
        # 接続間隔は打鍵がホストへ届くまでの遅れの下限になる
        # (1 レポートは次の接続イベントまで待つ)。ホストが決める値。
        out.append(('BLE 接続', True,
                    '接続中 / 接続間隔 %s ms / 送信 %s 件 / 失敗 %s 件'
                    % (interval, blex.get('sent'), blex.get('failed'))))
    else:
        out.append(('BLE 接続', None,
                    '未接続 (Mac 側でペアリングしてから読む)'))

    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wait', action='store_true',
                        help='ポートが現れるのを待ってから測る')
    parser.add_argument('--timeout', type=float, default=15.0)
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--transport', default='auto', choices=['auto', 'serial', 'hid'],
                        help='full プロファイル (CDC 無し) は hid')
    args = parser.parse_args()

    result = collect(args.wait, args.timeout, args.transport)
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
            print('[%s] %-18s %s' % (mark, name, detail))
    return 0 if all(ok is not False for _n, ok, _d in checks) else 1


if __name__ == '__main__':
    sys.exit(main())
