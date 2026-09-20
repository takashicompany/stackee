#!/usr/bin/env python3
"""段階 3 (音声と通信) の合否を実機で測る。**読むだけ。書き込みも再起動もしない。**

★★ **いちばん最初に必ず `audio.null 1` を送る。**
   これを立てると I2S にも AW88298 にも一切触らず、DMA 相当のカウンタだけが
   実時間で進む = **音は 1 ミリ秒も鳴らない**。会社で使っている本体なので、
   検証で音を出さないことがこのスクリプトのいちばん大事な仕事。
   (--allow-sound を付けたときだけ外れる。普段は使わない。)

見るのは 5 つ:

  1. マイク  `audio.selftest` … 1 秒録って サンプル数 / RMS / 最大値。
     無音でよい。**サンプル数が 16000 前後なら I2S の DMA が回っている。**
  2. スピーカー `audio.play` … 一次回答 ack_01 を「ヌル出力で」鳴らし、
     サンプル数と所要 ms を見る。実時間どおりなら送出の段取りは動いている。
  3. Wi-Fi   `status` … 起動から接続までの秒数・状態名・IP・登録件数。
  4. 会話   `talk.inject` … FAT の PCM を録音の代わりに送り、pi400 → ubook の
     往復を 1 回。**非同期**なので、その間ずっと `key.inject` を撃って
     「会話中の打鍵遅延」を同時に測る。
  5. 音量   `key.inject` で STK_VOLUP を押し、+5 されること、ステータスバーの
     CRC に反映されること、2 秒後に NVS へ保存されることを見る。

  python3 firmware/tools/check_phase3.py
  python3 firmware/tools/check_phase3.py --json
  python3 firmware/tools/check_phase3.py --no-talk      # 通信を使わない
  python3 firmware/tools/check_phase3.py --scan         # wifi.scan も回す
  python3 firmware/tools/check_phase3.py --expect-volume 25
        再起動のあとに回して「音量が残っているか」を見る (下の手順)

音量が再起動をまたぐかの確かめ方 (人手ゼロ・2 手):
  1. `check_phase3.py` を回す。最後に「再起動後は --expect-volume N」と出る。
  2. Fable が本体を再起動する (`stackee_console_client.py reset`)。
  3. `check_phase3.py --expect-volume N --only-volume` を回す。
"""
import argparse
import json
import os
import statistics
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import stackee_tree as tree             # noqa: E402

if not tree.add_kmk_tools(sys.path):
    sys.exit('現行 CircuitPython 版の道具 (firmware/kmk/tools) が要る '
             '(枠の実装をそこから借りている)')
import stackee_console_client as ccl    # noqa: E402
import console_hid                       # noqa: E402

# 独自キーは HID に出ないので、キーコードを数値で渡す (QK_KB_0 = 0x7E00)。
STK_TALK = 0x7E00
STK_VOLUP = 0x7E01
STK_VOLDN = 0x7E02
INJECT_KEY = 'F24'          # ホスト側で何も起きないキー

# 段階 2 の実測 (RESULTS.md)。音と通信を足しても悪くしない、が段階 3 の合否。
LATENCY_MEDIAN_MS = 6.0
LATENCY_MAX_MS = 10.0
# 会話 1 往復の上限 [秒]。pi400 の実測は 7〜11 秒 (native-http/README.md)。
TALK_BUDGET_S = 180.0
VOLUME_SAVE_WAIT_S = 3.0


# ---------------------------------------------------------------------------
# 小道具
# ---------------------------------------------------------------------------
def inject(client, kc=INJECT_KEY, hold_ms=20, timeout=5.0):
    """key.inject を 1 回。押下までの遅れ [ms] を返す (失敗なら None)。"""
    try:
        reply = client.request('key.inject', timeout=timeout, kc=kc,
                               hold_ms=hold_ms)
    except Exception:
        return None
    if not reply.get('ok'):
        return None
    return float(reply.get('press_ms', 0))


def latencies(client, count=12):
    out = []
    for _ in range(count):
        ms = inject(client)
        if ms is not None:
            out.append(ms)
    return out


def summarize(values):
    if not values:
        return None
    return {'n': len(values),
            'median_ms': round(statistics.median(values), 3),
            'max_ms': round(max(values), 3)}


# ---------------------------------------------------------------------------
# 1. マイク
# ---------------------------------------------------------------------------
def check_mic(client, result):
    result['audio_selftest'] = client.request('audio.selftest', timeout=10.0)


# ---------------------------------------------------------------------------
# 2. スピーカー (ヌル出力)
# ---------------------------------------------------------------------------
def check_speaker(client, result, verbose=True):
    start = client.request('audio.play', timeout=5.0, i=0)
    result['audio_play'] = start
    if not start.get('ok'):
        return
    began = time.time()
    status = {}
    while time.time() - began < 30.0:
        status = client.request('audio.status', timeout=5.0)
        if not status.get('playing'):
            break
        if verbose:
            sys.stderr.write('\r  audio.play %s/%s ...'
                             % (status.get('pos'), status.get('samples')))
            sys.stderr.flush()
        time.sleep(0.2)
    if verbose:
        sys.stderr.write('\r' + ' ' * 40 + '\r')
    result['audio_after'] = status


# ---------------------------------------------------------------------------
# 3. Wi-Fi
# ---------------------------------------------------------------------------
def check_wifi(client, result, do_scan=False):
    result['wifi_list'] = client.request('wifi.list', timeout=10.0)
    if do_scan:
        # ★ 録音・再生中は本体が断る (現行と同じ制約)。ここは何も鳴っていない。
        result['wifi_scan'] = client.request('wifi.scan', timeout=20.0)


# ---------------------------------------------------------------------------
# 4. 会話 (非同期。打鍵の遅延を同時に測る)
# ---------------------------------------------------------------------------
def check_talk(client, result, budget=TALK_BUDGET_S, verbose=True):
    start = client.request('talk.inject', timeout=10.0, ms=1000, i=0)
    result['talk_start'] = start
    if not start.get('ok'):
        return
    began = time.time()
    typing = []
    status = {}
    while time.time() - began < budget:
        status = client.request('talk.status', timeout=10.0)
        if status.get('state') == 'idle':
            break
        ms = inject(client)
        if ms is not None:
            typing.append(ms)
        if verbose:
            sys.stderr.write('\r  talk.inject %-10s %.0fs 打鍵 %d 回 ...'
                             % (status.get('state'), time.time() - began,
                                len(typing)))
            sys.stderr.flush()
        time.sleep(0.2)
    if verbose:
        sys.stderr.write('\r' + ' ' * 60 + '\r')
    result['talk_end'] = status
    result['talk_elapsed_s'] = round(time.time() - began, 1)
    result['talk_typing'] = summarize(typing)


# ---------------------------------------------------------------------------
# 5. 音量
# ---------------------------------------------------------------------------
def bar_params(status):
    """status の中身から bar.set に渡す引数を作る (いま出ているはずの絵)。"""
    return {
        'bat': status.get('bat') if status.get('bat') is not None else -1,
        'chg': bool(status.get('chg')),
        'vol': status.get('volume'),
        'wifi': status.get('wifi_state', 'off'),
        'link': 'usb' if status.get('hid') == 'USB' else 'ble',
        'ble': bool(status.get('ble')),
    }


def check_volume(client, result, verbose=True):
    before = client.request('status', timeout=10.0)
    start = before.get('volume')
    result['volume_before'] = start
    if not isinstance(start, int):
        result['volume_error'] = 'status に volume が無い'
        return
    # 100% だと上げられないので、そのときは下げてから測る。
    up = start < 100
    kc = STK_VOLUP if up else STK_VOLDN
    want = (start + 5) if up else (start - 5)
    result['volume_key'] = 'STK_VOLUP' if up else 'STK_VOLDN'
    inject(client, kc=kc, hold_ms=30)
    time.sleep(0.4)
    after = client.request('status', timeout=10.0)
    result['volume_after'] = after.get('volume')
    result['volume_want'] = want

    # ステータスバーに反映されたか。
    #   いま出ている絵 == 「新しい音量で描いた絵」
    #   いま出ている絵 != 「元の音量で描いた絵」
    # ★ 2 つ見るのは、電池の % が検査の途中で動いたときに「どちらとも
    #   違う」= 一致しない理由が音量ではない、と分かるようにするため。
    live = client.request('lcd.crc', timeout=10.0)
    params = bar_params(after)
    forced = client.request('bar.set', timeout=10.0, **params)
    old = dict(params, vol=start)
    forced_old = client.request('bar.set', timeout=10.0, **old)
    client.request('bar.auto', timeout=10.0)
    result['bar_live_crc'] = live.get('bar')
    result['bar_expected_crc'] = forced.get('bar')
    result['bar_old_crc'] = forced_old.get('bar')

    # 2 秒静かにしてから保存されるか。★ ここで打鍵を撃たない。
    if verbose:
        sys.stderr.write('  音量の保存を待つ (%.0f 秒、打鍵しない) ...\n'
                         % VOLUME_SAVE_WAIT_S)
    time.sleep(VOLUME_SAVE_WAIT_S)
    saved = client.request('status', timeout=10.0)
    result['volume_save_pending'] = saved.get('volume_save_pending')
    result['volume_src'] = saved.get('volume_src')
    result['volume_saved'] = saved.get('volume')


# ---------------------------------------------------------------------------
# 切り分け: Wi-Fi を切ると BLE が繋がるか
# ---------------------------------------------------------------------------
def check_ble_without_wifi(client, result, wait_s=60.0, verbose=True):
    """`wifi.off` → BLE が繋がるかを待つ → `wifi.on` で戻す。

    2026-09-16 の実機で「Wi-Fi が up の間 BLE が繋がらない」を踏んだので、
    人手なしで切り分けられるようにしてある。Mac 側の操作は要らない
    (ボンド済みなので、アドバタイズが届けば Mac から繋ぎに来る)。
    """
    before = client.request('status', timeout=10.0)
    result['ble_before_off'] = before.get('blex')
    result['wifi_off'] = client.request('wifi.off', timeout=10.0)
    began = time.time()
    got = before
    while time.time() - began < wait_s:
        time.sleep(2.0)
        got = client.request('status', timeout=10.0)
        if got.get('ble'):
            break
        if verbose:
            sys.stderr.write('\r  wifi.off のあと BLE を待つ %.0fs ...'
                             % (time.time() - began))
            sys.stderr.flush()
    if verbose:
        sys.stderr.write('\r' + ' ' * 50 + '\r')
    result['ble_after_off'] = got.get('blex')
    result['ble_connected_without_wifi'] = bool(got.get('ble'))
    result['ble_wait_s'] = round(time.time() - began, 1)
    result['wifi_on'] = client.request('wifi.on', timeout=10.0)


def collect(args):
    result = {}
    client = console_hid.open_client(port=args.port, transport=args.transport)
    result['port'] = client.port_name
    try:
        result['hello'] = client.request('hello', timeout=args.timeout)
        # ★★ 何より先に。ここから先、音は鳴らない。
        if args.allow_sound:
            result['null'] = {'skipped': True}
        else:
            result['null'] = client.request('audio.null', timeout=args.timeout,
                                            on=True)
        result['status_before'] = client.request('status', timeout=args.timeout)
        result['baseline'] = summarize(latencies(client))

        if not args.only_volume:
            check_mic(client, result)
            check_speaker(client, result, verbose=not args.json)
            check_wifi(client, result, do_scan=args.scan)
            if not args.no_talk:
                check_talk(client, result, budget=args.budget,
                           verbose=not args.json)
        check_volume(client, result, verbose=not args.json)
        if args.ble_without_wifi:
            check_ble_without_wifi(client, result, verbose=not args.json)
        result['status_after'] = client.request('status', timeout=args.timeout)
    finally:
        client.close()
    return result


# ---------------------------------------------------------------------------
def _fw_at_least(fw, want):
    """'stackee-idf/4' のような版が、求める段階以上か。

    ★ 段階が上がっても段階 3 の合否は測りたい (音と通信が壊れていないか)。
    """
    if not isinstance(fw, str) or not fw.startswith('stackee-idf/'):
        return False
    try:
        return int(fw.split('/', 1)[1]) >= want
    except ValueError:
        return False


def verdicts(result, args):
    out = []
    hello = result.get('hello') or {}
    out.append(('ファーム', _fw_at_least(hello.get('fw'), 3),
                '%s / proto %s' % (hello.get('fw'), hello.get('proto'))))

    null = result.get('null') or {}
    if args.allow_sound:
        out.append(('ヌル出力', None, '--allow-sound のため立てていない (音が出る)'))
    else:
        out.append(('ヌル出力', null.get('null') is True,
                    'audio.null = %s (これが true の間は音が出ない)'
                    % null.get('null')))

    base = result.get('baseline')
    if base:
        out.append(('打鍵の遅延 (平常)',
                    base['median_ms'] <= LATENCY_MEDIAN_MS and
                    base['max_ms'] <= LATENCY_MAX_MS,
                    '中央値 %.2f ms / 最大 %.2f ms / %d 回'
                    % (base['median_ms'], base['max_ms'], base['n'])))

    mic = result.get('audio_selftest')
    if mic is not None:
        samples = mic.get('samples') or 0
        ms = mic.get('ms') or 0
        # 1 秒ぶん (16000) の 8 割は取れていること = DMA が回っている証拠。
        out.append(('マイク (audio.selftest)',
                    bool(mic.get('ok')) and samples >= 12800,
                    '%s サンプル / %s ms / RMS %s / 最大 %s%s'
                    % (samples, ms, mic.get('rms'), mic.get('peak'),
                       (' / %s' % mic['error']) if mic.get('error') else '')))

    play = result.get('audio_play')
    after = result.get('audio_after') or {}
    if play is not None:
        want = play.get('expect_ms') or 0
        got = after.get('play_ms') or 0
        ok = (bool(play.get('ok')) and not after.get('playing') and
              after.get('played') == play.get('samples') and
              abs(got - want) <= max(200, want // 10) and
              not after.get('failed'))
        out.append(('スピーカー (ヌル出力)', ok,
                    '%s サンプルを %s ms で送出 (期待 %s ms) / failed=%s'
                    % (after.get('played'), got, want, after.get('failed'))))

    status = result.get('status_after') or result.get('status_before') or {}
    wifi_ok = status.get('wifi_state') == 'up'
    out.append(('Wi-Fi', wifi_ok,
                '状態 %s / SSID %s / IP %s / 起動から %s ms / 登録 %s 件'
                % (status.get('wifi_state'), status.get('ssid') or '-',
                   status.get('ip') or '-', status.get('wifi_up_ms'),
                   status.get('nets'))))

    wl = result.get('wifi_list')
    if wl is not None:
        names = [n.get('ssid') for n in (wl.get('networks') or [])]
        # ★ `'password' in ...` と書かないこと。**`has_password` に当たる**
        #   (2026-09-16: この検査そのものの欠陥で「漏れている」と誤報した)。
        #   見るのは「`password` という鍵があるか」。`"has_password"` の中には
        #   `"password"` (前に引用符) は現れないので、これで区別できる。
        raw = json.dumps(wl, ensure_ascii=False)
        leaked = ('"password"' in raw or
                  any('password' in (n or {}) for n in (wl.get('networks') or [])))
        out.append(('wifi.list', not leaked,
                    '%s 件 %s%s' % (wl.get('n'), names,
                                    ' ★ password の鍵が入っている' if leaked else
                                    ' (has_password だけ。password の鍵は無い)')))
    scan = result.get('wifi_scan')
    if scan is not None:
        out.append(('wifi.scan', bool(scan.get('ok')),
                    '%s 件 / %s ms' % (scan.get('n'), scan.get('total_ms'))))

    start = result.get('talk_start')
    end = result.get('talk_end') or {}
    if start is not None:
        http = end.get('http') or {}
        ok = (bool(start.get('ok')) and end.get('state') == 'idle' and
              not end.get('error') and (end.get('turns') or 0) > 0)
        out.append(('会話 1 往復', ok,
                    '受理 %s ms / 返答 %s ms / PCM %s ms / 完了 %s ms / '
                    'ポーリング %s 回 / 返答 %s B%s'
                    % (end.get('accepted_ms'), end.get('reply_ready_ms'),
                       end.get('audio_ready_ms'), end.get('complete_ms'),
                       end.get('polls'), http.get('last_bytes'),
                       (' / エラー %s' % end['error']) if end.get('error') else '')))
        out.append(('返答文', bool(end.get('reply')),
                    repr(end.get('reply'))))
        typing = result.get('talk_typing')
        if typing:
            out.append(('打鍵の遅延 (会話中)',
                        typing['median_ms'] <= LATENCY_MEDIAN_MS and
                        typing['max_ms'] <= LATENCY_MAX_MS,
                        '中央値 %.2f ms / 最大 %.2f ms / %d 回'
                        % (typing['median_ms'], typing['max_ms'], typing['n'])))

    if result.get('volume_error'):
        out.append(('音量キー', False, result['volume_error']))
    elif 'volume_after' in result:
        out.append(('音量キー (%s)' % result.get('volume_key'),
                    result.get('volume_after') == result.get('volume_want'),
                    '%s%% → %s%% (期待 %s%%)'
                    % (result.get('volume_before'), result.get('volume_after'),
                       result.get('volume_want'))))
        live = result.get('bar_live_crc')
        out.append(('ステータスバーへの反映',
                    (live is not None and live == result.get('bar_expected_crc')
                     and live != result.get('bar_old_crc')),
                    'いまの絵 %s / 新しい音量で描いた絵 %s / 元の音量で描いた絵 %s'
                    % (live, result.get('bar_expected_crc'),
                       result.get('bar_old_crc'))))
        out.append(('NVS への保存', result.get('volume_save_pending') is False,
                    '%.0f 秒後に save_pending=%s (元は %s)'
                    % (VOLUME_SAVE_WAIT_S, result.get('volume_save_pending'),
                       result.get('volume_src'))))

    # ★ TLS の入出力バッファが取れるかを決めるのは **内蔵 RAM**。
    #   heap_free は PSRAM 込みなので、これだけを見ていると
    #   mbedtls_ssl_setup の ALLOC_FAILED を見逃す (2026-09-16)。
    internal = status.get('heap_internal')
    if internal is not None:
        # 握手には 16 KB + 2 KB + 作業領域が要る。PSRAM へ逃がしてあるので
        # 内蔵はもっと少なくて済むが、余裕は見ておきたい。
        out.append(('内蔵 RAM', internal >= 40000,
                    '空き %s B / 最小 %s B / 最大の塊 %s B / DMA %s B '
                    '(PSRAM %s B)'
                    % (internal, status.get('heap_internal_min'),
                       status.get('heap_internal_largest'),
                       status.get('heap_dma'), status.get('psram_free'))))

    blex = (result.get('status_after') or {}).get('blex') or {}
    if blex:
        out.append(('BLE', bool((result.get('status_after') or {}).get('ble')),
                    'adv %s / 接続 %s 回 / 切断 %s 回 / 撒いた %s 回 / '
                    '失敗 %s 回 / 撒き直し %s 回'
                    % (blex.get('adv'), blex.get('conn'), blex.get('disc'),
                       blex.get('adv_starts'), blex.get('adv_fails'),
                       blex.get('adv_revived'))))

    if 'ble_connected_without_wifi' in result:
        out.append(('Wi-Fi を切ると BLE が繋がるか',
                    result['ble_connected_without_wifi'],
                    '%s (%.0f 秒待った)。切る前 %s / 切ったあと %s'
                    % ('繋がった' if result['ble_connected_without_wifi']
                       else '繋がらない', result.get('ble_wait_s', 0),
                       result.get('ble_before_off'), result.get('ble_after_off'))))

    if args.expect_volume is not None:
        got = (result.get('status_before') or {}).get('volume')
        out.append(('再起動をまたいだ音量', got == args.expect_volume,
                    '%s%% (期待 %s%%)' % (got, args.expect_volume)))

    return out


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port')
    parser.add_argument('--timeout', type=float, default=15.0)
    parser.add_argument('--budget', type=float, default=TALK_BUDGET_S,
                        help='会話 1 往復を待つ上限 [秒]')
    parser.add_argument('--no-talk', action='store_true',
                        help='通信を使わない (マイク・スピーカー・音量だけ)')
    parser.add_argument('--only-volume', action='store_true',
                        help='音量だけ見る (再起動後の確認に使う)')
    parser.add_argument('--scan', action='store_true', help='wifi.scan も回す')
    parser.add_argument('--ble-without-wifi', action='store_true',
                        help='wifi.off で無線を止めて BLE が繋がるかを見る '
                             '(見終わったら wifi.on で戻す)')
    parser.add_argument('--allow-sound', action='store_true',
                        help='★ ヌル出力を立てない = 実際に音が鳴る。普段は使わない')
    parser.add_argument('--expect-volume', type=int,
                        help='起動時の音量がこの値なら合格 (再起動後の確認)')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--transport', default='auto', choices=['auto', 'serial', 'hid'],
                        help='full プロファイル (CDC 無し) は hid')
    args = parser.parse_args()

    result = collect(args)
    checks = verdicts(result, args)
    if args.json:
        print(json.dumps({'result': result,
                          'verdicts': [{'name': n, 'ok': o, 'detail': d}
                                       for n, o, d in checks]},
                         ensure_ascii=False, indent=2))
    else:
        print('ポート: %s' % result.get('port'))
        for name, ok, detail in checks:
            mark = '--' if ok is None else ('OK' if ok else 'NG')
            print('[%s] %-22s %s' % (mark, name, detail))
        saved = result.get('volume_saved')
        if saved is not None:
            print('\n再起動をまたぐかを確かめるには、本体を reset してから:\n'
                  '  python3 firmware/tools/check_phase3.py '
                  '--only-volume --expect-volume %s' % saved)
    return 0 if all(ok is not False for _n, ok, _d in checks) else 1


if __name__ == '__main__':
    sys.exit(main())
