#!/usr/bin/env python3
"""段階 0 の合否を実機で測る。**書き込んだ後に走らせる。**

見るのは 4 つ:
  1. 起動から CDC が応答するまでの時間 (合否: 3 秒以内)
  2. hello / status が現行と同じ枠で返るか
  3. status の perf (メインタスクの周期) とヒープ / PSRAM
  4. user_fs の目録 (size と faces) を読めているか

  python3 firmware/tools/check_phase0.py            # いま繋がっている本体を見る
  python3 firmware/tools/check_phase0.py --wait     # 再起動を待ってから測る
  python3 firmware/tools/check_phase0.py --json     # 機械可読

★ 書き込みも再起動も、REPL への侵入もしない。読むだけ。
★ 枠の実装は firmware/kmk/tools/stackee_console_client.py を借りる。2 つの
  実装がずれると片方でしか再現しない不具合になるため。
"""
import argparse
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

BOOT_BUDGET_S = 3.0


def collect(wait, timeout):
    """(結果の dict, 判定の list) を返す。"""
    result = {}
    if wait:
        # 再起動を待つ: ポートが現れる → 開ける → status が返る、の 3 点。
        client, timing = ccl.reconnect(timeout=timeout)
        result['reconnect'] = timing
        result['status'] = timing.pop('status')
    else:
        client = ccl.ConsoleClient()
        result['status'] = client.request('status', timeout=timeout)
    try:
        result['hello'] = client.request('hello', timeout=timeout)
        # 2 回目の status。up が進んでいることと RTT を見る。
        result['status2'] = client.request('status', timeout=timeout)
        result['port'] = client.port_name
        result['log'] = ''.join(client.log)[-2000:]
    finally:
        client.close()
    return result


def verdicts(result):
    out = []
    hello = result.get('hello') or {}
    status = result.get('status') or {}

    fw = hello.get('fw') or status.get('fw')
    # 段階が上がると数字が増える (stackee-idf/0 -> /1 -> ...)。段階 0 の
    # 合否を見るのに段階を固定する意味は無いので、名乗りの形だけを見る。
    out.append(('ファーム', bool(fw) and fw.startswith('stackee-idf/'),
                '%s (期待 stackee-idf/N)' % fw))

    if 'reconnect' in result:
        boot_s = result['reconnect'].get('status_s')
        out.append(('起動 → CDC 応答', boot_s is not None and boot_s <= BOOT_BUDGET_S,
                    '%s 秒 (合否 %.1f 秒以内)' % (boot_s, BOOT_BUDGET_S)))
    else:
        out.append(('起動 → CDC 応答', None, '--wait を付けないと測れない'))

    assets = status.get('assets') or {}
    out.append(('user_fs の目録', bool(assets.get('manifest')),
                'size=%s faces=%s%s' % (assets.get('size'), assets.get('faces'),
                                        ' error=%s' % assets['error']
                                        if assets.get('error') else '')))

    lcd = status.get('lcd') or {}
    out.append(('LCD', bool(lcd.get('ready')),
                '転送 %s 回 / %s 行 / ワーカー %s ms'
                % (lcd.get('transfers'), lcd.get('rows'), lcd.get('worker_ms'))))

    bat = status.get('bat')
    out.append(('電池 (AXP2101)', bat is not None, '%s%%' % bat))

    perf = (status.get('perf') or {}).get('main') or {}
    out.append(('メインタスクの周期', bool(perf),
                '中央値 %s us / 最大 %s us / 標本 %s'
                % (perf.get('med_us'), perf.get('max_us'), perf.get('n'))))

    out.append(('ヒープ / PSRAM', status.get('heap_free') is not None,
                'heap %s B (最小 %s) / PSRAM %s B'
                % (status.get('heap_free'), status.get('heap_min'),
                   status.get('psram_free'))))
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--wait', action='store_true',
                    help='ポートが現れるのを待ってから測る (再起動直後用)')
    ap.add_argument('--timeout', type=float, default=10.0)
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args(argv)

    started = time.monotonic()
    result = collect(args.wait, args.timeout)
    result['elapsed_s'] = round(time.monotonic() - started, 2)
    checks = verdicts(result)

    if args.json:
        result['checks'] = [{'name': n, 'ok': ok, 'detail': d} for n, ok, d in checks]
        print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    else:
        print('ポート: %s' % result.get('port'))
        for name, ok, detail in checks:
            mark = '？' if ok is None else ('OK' if ok else '★NG')
            print('%-4s %-18s %s' % (mark, name, detail))
        print()
        print('hello : %s' % json.dumps(result.get('hello'), ensure_ascii=False))
        print('status: %s' % json.dumps(result.get('status'), ensure_ascii=False))
        if result.get('log'):
            print('--- 枠の外 (ログ) ---')
            print(result['log'])
    return 0 if all(ok is not False for _, ok, _ in checks) else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ValueError, TimeoutError) as err:
        sys.exit('失敗: %s' % err)
