#!/usr/bin/env python3
"""段階 2 (画面) の合否を実機で測る。**読むだけ。書き込みも再起動もしない。**

人手はいらない。画面を目で見る必要も無い。見るのは 7 つ:

  1. 展開した素材が Mac と同じバイト列か (ui.assets の CRC32 と zlib の CRC32)
  2. 32 表情ぜんぶが正しく描けるか (ui.selftest の CRC32 と render_expected.py)
     ★ 本体は顔 0 を全面で描いたあと changes.bin の差分だけで 1 枚ずつ寄せる。
       全面で描いた期待値と一致すれば、差分表の読み方も合っている。
  3. ステータスバーの代表 6 状態が正しく描けるか (同上)
  4. 自己テストを回している **最中に** 打鍵の遅延が悪化しないか (key.inject)
  5. 顔 1 コマの描画時間と LCD 転送時間 (status の perf.ui_face / perf.ui)
  6. 字幕の帯 (y=250..319 の 3 行) が正しく描けるか
     (`ui.subtitle` の CRC32 と subtitle_expected.py)
     ★ 1 行・3 行・頁めくり直後・空・字形なし〓・半角混在を含む
     ★ 描画時間は 1 ケース 6 回描いて**最小値**で見る (理由は run_subtitles)
  7. **字幕を描いている最中に** 打鍵の遅延が悪化しないか (key.inject)

  python3 firmware/tools/check_phase2.py
  python3 firmware/tools/check_phase2.py --json
  python3 firmware/tools/check_phase2.py --no-selftest   # 4 と 5 だけ

不一致だったときは、そのまま「どこが違うか」を絞りに行く:
`face.set` と `bar.set` で画面を既知の状態に固定し、`lcd.crc` に y / h を
渡して行の範囲を二分探索し、最初に食い違った行の先頭 16 バイトを
`lcd.dump` で読んで期待値と並べて出す。
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
import render_expected as rex           # noqa: E402
import subtitle_expected as sex         # noqa: E402
import stackee_console_client as ccl    # noqa: E402
import console_hid                       # noqa: E402

# 段階 1 の実測 (README §9)。画面を足しても悪くしない、が段階 2 の合否。
LATENCY_MEDIAN_MS = 6.0
LATENCY_MAX_MS = 10.0
# 字幕の帯 1 回の「描画そのもの」の約束 (README §23-3 / §23-8)。
# ★ 帯は 1 行 240x30 から **3 行 240x70** になった (顔を 240x200 に切り詰めて
#   空いた 40 px を回した)。塗るのは 7,200 -> 16,800 画素 (14.4 -> 33.6 KB)。
#   線形に伸ばした見積もりでは足りない。フレームバッファが PSRAM にあり、
#   **データキャッシュが 32 KB** (CONFIG_ESP32S3_DATA_CACHE_32KB) なので、
#   14.4 KB の帯はキャッシュに丸ごと載っていたが 33.6 KB は載らず、
#   描き直すたびに PSRAM まで往復する (4 行 96 px の版で空 3.2 ms を実測)。
#   6 ms は「いちばん重い中身 (3 行 x 15 桁 = 全角 45 字) の実測に余裕を
#   持たせた値」。実測は README §23-8 と RESULTS.md。
#   ★ 帯を描くのは ui タスク (優先度 3) で、しかもページが変わった時だけ。
#     打鍵は CPU1 の最高優先度なので 1 ミリ秒も響かない (下の「字幕中」で実測)。
SUB_PAINT_MAX_US = 6000
# 1 ケースあたり何回描いて最小値を取るか。割り込まれなかった 1 発が要る。
SUB_BURST = 6
INJECT_KEY = 'F24'          # ホスト側で何も起きないキー


# ---------------------------------------------------------------------------
# 実機と話す
# ---------------------------------------------------------------------------
def inject(client, timeout=5.0):
    """key.inject を 1 回。押下までの遅れ [ms] を返す (失敗なら None)。"""
    try:
        reply = client.request('key.inject', timeout=timeout,
                               kc=INJECT_KEY, hold_ms=20)
    except Exception:
        return None
    if not reply.get('ok'):
        return None
    return float(reply.get('press_ms', 0))


def run_selftest(client, budget=60.0, verbose=True):
    """ui.selftest を走らせ、走っている間ずっと key.inject を回す。"""
    started = time.time()
    reply = client.request('ui.selftest', timeout=10.0, restart=True)
    latencies = []
    steps = []
    while reply.get('state') == 'running':
        if time.time() - started > budget:
            raise TimeoutError('ui.selftest が %.0f 秒で終わらない' % budget)
        ms = inject(client)
        if ms is not None:
            latencies.append(ms)
        reply = client.request('ui.selftest', timeout=10.0)
        steps.append(reply.get('step'))
        if verbose:
            sys.stderr.write('\r  ui.selftest %s/%s ...'
                             % (reply.get('step'), reply.get('n')))
            sys.stderr.flush()
    if verbose:
        sys.stderr.write('\r' + ' ' * 40 + '\r')
    return reply, latencies


def run_subtitles(client, timeout=15.0, burst=SUB_BURST):
    """`ui.subtitle` を 1 ケース burst 回ずつ投げ、帯の CRC と描画時間を集める。

    ★ 顔と同じ方式。帯 (y=290..319) だけの CRC32 を本体から取り、
      Mac 側が同じ font16.bin から描いた期待値と突き合わせる。

    ★★ **描画そのものの時間は「その山の最小値」で見る。**
      `ui.subtitle` が返す `us` は esp_timer の実時間で、途中で他のタスクに
      割り込まれたぶんも入っている。しかもこの命令を処理するのは
      **main タスク (優先度 1)** — CPU0 でいちばん低い。LCD の SPI ワーカー
      (優先度 1) とはラウンドロビンで取り合い、Wi-Fi (23) / USB (5) /
      audio (5) / http (4) / ui (3) には素通しで割り込まれる。
      普段、帯を描くのは **ui タスク (優先度 3)** なので、SPI ワーカーには
      割り込まれない。つまり `us` の中央値や最大値は**悲観側にずれた代理値**で、
      これを合否にすると割り込みの多寡を測ることになる。
      割り込まれなかった標本 = 最小値が「描画そのもの」。実測は README §23-3。
    """
    rows = []
    latencies = []
    for case in sex.expected()['cases']:
        got = []
        reply = {}
        for _ in range(burst):
            reply = client.request('ui.subtitle', timeout=timeout,
                                   text=case['text'])
            if isinstance(reply.get('us'), int):
                got.append(reply['us'])
        ms = inject(client)
        if ms is not None:
            latencies.append(ms)
        got = got or [None]
        rows.append({'name': case['name'], 'want': case['crc'],
                     'want_px': case['px'], 'got': reply.get('crc'),
                     'px': reply.get('px'),
                     'draw_us': min(x for x in got if x is not None) if got[0] is not None else None,
                     'med_us': statistics.median(got) if got[0] is not None else None,
                     'wall_max_us': max(got) if got[0] is not None else None,
                     'n': len(got),
                     'font16': reply.get('font16'), 'error': reply.get('error')})
    client.request('ui.subtitle', timeout=timeout, text='')     # 帯を消して戻す
    return rows, latencies


def baseline_latency(client, count=12):
    out = []
    for _ in range(count):
        ms = inject(client)
        if ms is not None:
            out.append(ms)
    return out


# ---------------------------------------------------------------------------
# 不一致を絞る
# ---------------------------------------------------------------------------
def locate_mismatch(client, face, bar):
    """画面を (face, bar) に固定して、最初に食い違う行を二分探索する。"""
    fb = rex.Renderer().framebuffer(face=face, bar=bar)
    want = bytes(fb.buf)
    stride = rex.STRIDE
    client.request('bar.set', timeout=10.0, i=bar)
    client.request('face.set', timeout=10.0, i=face)

    def device_crc(y, h):
        reply = client.request('lcd.crc', timeout=10.0, y=y, h=h)
        return reply.get('crc')

    import zlib

    def expect_crc(y, h):
        return zlib.crc32(want[y * stride:(y + h) * stride])

    if device_crc(0, rex.HEIGHT) == expect_crc(0, rex.HEIGHT):
        return None
    low, high = 0, rex.HEIGHT          # [low, high) に不一致がある
    while high - low > 1:
        mid = (low + high) // 2
        if device_crc(low, mid - low) != expect_crc(low, mid - low):
            high = mid
        else:
            low = mid
    row = low
    dump = client.request('lcd.dump', timeout=10.0, y=row, x=0, n=16)
    return {
        'face': face, 'bar': bar, 'row': row,
        'device': dump.get('hex'),
        'expected': want[row * stride:row * stride + 16].hex().upper(),
    }


# ---------------------------------------------------------------------------
def collect(args):
    expected = rex.Renderer().expected()
    client = console_hid.open_client(transport=TRANSPORT)
    result = {'expected': expected}
    try:
        result['port'] = client.port_name
        result['hello'] = client.request('hello', timeout=args.timeout)
        result['assets'] = client.request('ui.assets', timeout=args.timeout)
        result['ui_before'] = client.request('ui.status', timeout=args.timeout)
        result['baseline'] = baseline_latency(client)
        if not args.no_selftest:
            reply, latencies = run_selftest(client, budget=args.budget,
                                            verbose=not args.json)
            result['selftest'] = reply
            result['busy_latency'] = latencies
            bad_faces = [i for i, crc in enumerate(reply.get('faces') or [])
                         if crc != expected['faces'][i]]
            bad_bars = [i for i, crc in enumerate(reply.get('bars') or [])
                        if crc != expected['bars'][i]]
            result['bad_faces'] = bad_faces
            result['bad_bars'] = bad_bars
            if bad_faces or bad_bars:
                face = bad_faces[0] if bad_faces else 0
                bar = bad_bars[0] if bad_bars else len(rex.BAR_SCENARIOS) - 1
                try:
                    result['mismatch'] = locate_mismatch(client, face, bar)
                except Exception as err:
                    result['mismatch_error'] = '%s' % err
                client.request('face.auto', timeout=args.timeout)
                client.request('bar.auto', timeout=args.timeout)
        if not args.no_subtitle:
            try:
                rows, latencies = run_subtitles(client, timeout=args.timeout)
                result['subtitles'] = rows
                result['subtitle_latency'] = latencies
            except Exception as err:
                result['subtitle_error'] = '%s' % err
        result['status'] = client.request('status', timeout=args.timeout)
        result['ui_after'] = client.request('ui.status', timeout=args.timeout)
        result['log'] = ''.join(client.log)[-2000:]
    finally:
        client.close()
    return result


def verdicts(result):
    out = []
    expected = result['expected']
    hello = result.get('hello') or {}
    status = result.get('status') or {}
    perf = status.get('perf') or {}

    # ★ 版の文字列は段階 1 のまま ("stackee-idf/1")。段階 2 の像かどうかは
    #   features に画面まわりのコマンドが載っているかで見分ける。
    features = hello.get('features') or []
    need = ('lcd.crc', 'face.set', 'bar.set', 'ui.selftest')
    out.append(('ファーム', all(f in features for f in need),
                '%s / 画面のコマンド %s'
                % (hello.get('fw'),
                   'あり' if all(f in features for f in need) else 'ない')))

    assets = result.get('assets') or {}
    want = expected['assets']
    if not assets.get('ok'):
        out.append(('素材', False, 'ui.assets が答えない (画面が立っていない)'))
    else:
        same = all(assets.get(k) == want[k] for k in
                   ('faces_len', 'faces_crc', 'changes_len', 'changes_crc',
                    'icons_len', 'icons_crc'))
        out.append(('素材の展開', same,
                    '顔 %s B crc=%s (期待 %s、240x%d に切り詰めたあと) / '
                    '差分 %s B / アイコン %s B / フォント %s (%s 字)'
                    % (assets.get('faces_len'), assets.get('faces_crc'),
                       want['faces_crc'], expected['face_rows'],
                       assets.get('changes_len'),
                       assets.get('icons_len'), assets.get('font'),
                       assets.get('glyphs'))))
        # 字幕用のフォントは FAT に置いた assets/font16.bin そのもの
        # (圧縮していないので、読み込めたバイト列の CRC が Mac のものと同じ)。
        want16 = sex.expected()['font16']
        ok16 = (assets.get('font16') is True and
                assets.get('font16_len') == want16['bytes'] and
                assets.get('font16_crc') == want16['crc'] and
                assets.get('font16_narrow') == want16['narrow'] and
                assets.get('font16_wide') == want16['wide'])
        out.append(('字幕フォント', ok16,
                    'font16.bin %s B crc=%s (期待 %s B / %s) / 半角 %s + 全角 %s 字'
                    % (assets.get('font16_len'), assets.get('font16_crc'),
                       want16['bytes'], want16['crc'],
                       assets.get('font16_narrow'), assets.get('font16_wide'))))

    selftest = result.get('selftest')
    if selftest is None:
        out.append(('32 表情', None, '--no-selftest で飛ばした'))
        out.append(('ステータスバー', None, '--no-selftest で飛ばした'))
    elif selftest.get('state') != 'done':
        out.append(('32 表情', False, 'ui.selftest が終わらなかった: %r' % selftest))
    else:
        bad_faces = result.get('bad_faces') or []
        bad_bars = result.get('bad_bars') or []
        out.append(('32 表情 (y=%d h=%d)' % (expected['face_y'],
                                               expected['face_rows']),
                    not bad_faces,
                    '%d/%d 一致%s (%s ms)'
                    % (32 - len(bad_faces), 32,
                       '' if not bad_faces else ' / 違うのは %r' % bad_faces,
                       selftest.get('ms'))))
        out.append(('ステータスバー', not bad_bars,
                    '%d/%d 一致%s'
                    % (len(expected['bars']) - len(bad_bars), len(expected['bars']),
                       '' if not bad_bars else ' / 違うのは %r' % bad_bars)))

    base = result.get('baseline') or []
    busy = result.get('busy_latency') or []
    if not base:
        out.append(('打鍵の遅延 (平常)', None, 'key.inject が返らない'))
    else:
        med = statistics.median(base)
        out.append(('打鍵の遅延 (平常)', med <= LATENCY_MEDIAN_MS,
                    '中央値 %.3f ms / 最大 %.3f ms / %d 回 (合否 中央値 %.1f ms)'
                    % (med, max(base), len(base), LATENCY_MEDIAN_MS)))
    if busy:
        med = statistics.median(busy)
        worst = max(busy)
        out.append(('打鍵の遅延 (描画中)', med <= LATENCY_MEDIAN_MS and worst <= LATENCY_MAX_MS,
                    '中央値 %.3f ms / 最大 %.3f ms / %d 回 (ui.selftest を回しながら)'
                    % (med, worst, len(busy))))
    elif selftest is not None:
        out.append(('打鍵の遅延 (描画中)', None, '標本が取れなかった'))

    face = perf.get('ui_face') or {}
    out.append(('顔 1 コマの描画', bool(face),
                '中央値 %s us / 最大 %s us / %s 枚'
                % (face.get('med_us'), face.get('max_us'), face.get('n'))))
    lcd = perf.get('ui') or {}
    out.append(('LCD 転送', bool(lcd),
                '中央値 %s us / 最大 %s us / %s 回'
                % (lcd.get('med_us'), lcd.get('max_us'), lcd.get('n'))))
    bar = perf.get('ui_bar') or {}
    if bar:
        out.append(('バーの描き直し', True,
                    '中央値 %s us / 最大 %s us / %s 回'
                    % (bar.get('med_us'), bar.get('max_us'), bar.get('n'))))

    # ---- 字幕 -------------------------------------------------------------
    rows = result.get('subtitles')
    if rows is None:
        out.append(('字幕の帯', None,
                    result.get('subtitle_error') or '--no-subtitle で飛ばした'))
    else:
        bad = [r['name'] for r in rows if r['got'] != r['want']]
        out.append(('字幕の帯', not bad,
                    '%d/%d 一致%s (font16 %s)'
                    % (len(rows) - len(bad), len(rows),
                       '' if not bad else ' / 違うのは %r' % bad,
                       rows[0].get('font16') if rows else '?')))
        bad_px = [r['name'] for r in rows if r['px'] != r['want_px']]
        out.append(('字幕の桁数', not bad_px,
                    '15 桁 = %d px / 帯は %d 行 x %d px%s'
                    % (sex.SUB_WIDTH, sex.SUB_LINES, sex.SUB_LINE_H,
                       '' if not bad_px else ' / 違うのは %r' % bad_px)))
    if rows:
        # 約束は「帯 1 回の**描画そのもの**が 2 ms 以下」(README §23-3)。
        # 合否はケースごとの最小値 (= 割り込まれなかった標本) のいちばん悪い方。
        draws = [r for r in rows if r.get('draw_us') is not None]
        if draws:
            worst = max(r['draw_us'] for r in draws)
            worst_name = [r['name'] for r in draws if r['draw_us'] == worst][0]
            out.append(('帯の描画そのもの', worst <= SUB_PAINT_MAX_US,
                        '最悪 %d us (%s) / %d ケース x %d 回 (合否 %d us)'
                        % (worst, worst_name, len(draws), draws[0]['n'],
                           SUB_PAINT_MAX_US)))
            # 実時間 (割り込み込み) は参考値。main タスクは CPU0 でいちばん
            # 低い優先度なので、ここは悲観側にずれる (README §23-3)。
            out.append(('帯の実時間 (参考)', None,
                        '中央値 %d us / 最大 %d us … 割り込み込み。'
                        'ui タスク (優先度 3) で描く普段の道より悲観側'
                        % (statistics.median([r['med_us'] for r in draws]),
                           max(r['wall_max_us'] for r in draws))))
    subs = perf.get('ui_sub') or {}
    if subs:
        out.append(('perf.ui_sub (参考)', None,
                    '中央値 %s us / 最大 %s us / %s 回 (起動からの累計)'
                    % (subs.get('med_us'), subs.get('max_us'), subs.get('n'))))
    sub_lat = result.get('subtitle_latency') or []
    if sub_lat:
        med = statistics.median(sub_lat)
        worst = max(sub_lat)
        out.append(('打鍵の遅延 (字幕中)',
                    med <= LATENCY_MEDIAN_MS and worst <= LATENCY_MAX_MS,
                    '中央値 %.3f ms / 最大 %.3f ms / %d 回 (ui.subtitle を挟みながら)'
                    % (med, worst, len(sub_lat))))

    ui = result.get('ui_after') or {}
    if ui.get('ok'):
        out.append(('画面の状態', not ui.get('frozen') and not ui.get('bar_forced'),
                    '顔 %s / 見送り %s 回 / 描いた %s コマ / フォント %s'
                    % (ui.get('current'), ui.get('skipped'), ui.get('frames'),
                       ui.get('font'))))

    mismatch = result.get('mismatch')
    if mismatch:
        out.append(('不一致の場所', False,
                    '顔 %s / バー %s / 最初に違う行 y=%s\n'
                    '           本体   %s\n'
                    '           期待値 %s'
                    % (mismatch['face'], mismatch['bar'], mismatch['row'],
                       mismatch['device'], mismatch['expected'])))
    elif result.get('mismatch_error'):
        out.append(('不一致の場所', None, result['mismatch_error']))

    return out


TRANSPORT = 'auto'


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--timeout', type=float, default=15.0)
    parser.add_argument('--budget', type=float, default=60.0,
                        help='ui.selftest を待つ上限 [秒]')
    parser.add_argument('--no-selftest', action='store_true',
                        help='遅延と perf だけ読む (画面を触らない)')
    parser.add_argument('--no-subtitle', action='store_true',
                        help='字幕の帯を飛ばす (旧い像を測るとき)')
    parser.add_argument('--json', action='store_true')
    parser.add_argument('--transport', default='auto', choices=['auto', 'serial', 'hid'],
                        help='full プロファイル (CDC 無し) は hid')
    args = parser.parse_args()
    global TRANSPORT
    TRANSPORT = args.transport

    result = collect(args)
    checks = verdicts(result)
    if args.json:
        result.pop('expected', None)
        print(json.dumps({'result': result,
                          'verdicts': [{'name': n, 'ok': o, 'detail': d}
                                       for n, o, d in checks]},
                         ensure_ascii=False, indent=2))
    else:
        print('ポート: %s' % result.get('port'))
        for name, ok, detail in checks:
            mark = '--' if ok is None else ('OK' if ok else 'NG')
            print('[%s] %-20s %s' % (mark, name, detail))
    return 0 if all(ok is not False for _n, ok, _d in checks) else 1


if __name__ == '__main__':
    sys.exit(main())
