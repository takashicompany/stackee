#!/usr/bin/env python3
"""撮影のあとにリセットしてもブートループしないことを実機で確かめる。

  python3 firmware/tools/check_camera_reset.py [--transport auto|hid] [--via reset|bootloader]

手順:
  1. camera.capture (捨て駒 4 枚で短く) → camera.status で aldo3=0 を見る
  2. --via reset:      console `reset` → CDC (811A) が戻るまでの秒数
     --via bootloader: console `bootloader` → ROM (303A:0009) を見てから
                       flash.py の leave_rom() で戻す → CDC が戻るまでの秒数
  3. 戻ったら status で up (起動からの秒数) が小さいこと = 本当に再起動した

2026-09-16 の事故 (README §17): 撮影後の `bootloader` → watchdog-reset で
"invalid header: 0xffffff1f" のブートループ。G45/G46 の hold 固定で直したかの検査。
"""
import argparse, sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import console_hid          # noqa: E402
import flash                # noqa: E402
import stackee_serial as ss # noqa: E402


def alive():
    # dev は CDC のポート名、full (CDC 無し) は USB の列挙 (303A:811A) で見る。
    return flash.app_alive()


def wait_cdc(timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        p = alive()
        if p:
            return p, time.time() - t0
        time.sleep(0.1)
    return None, time.time() - t0


def watch_cdc(seconds):
    """CDC の出現/消滅の時系列を記録する (二度目の再起動を見逃さないため)。"""
    t0 = time.time()
    last = None
    events = []
    while time.time() - t0 < seconds:
        p = alive()
        state = 'up' if p else 'down'
        if state != last:
            events.append((time.time() - t0, state))
            last = state
        time.sleep(0.1)
    return events


def dump_log(c, limit=16000):
    """log.tail を back をずらしながら呼び、リング全体を古い順に返す。"""
    chunks = []
    back = 0
    while back < limit:
        d = c.request('log.tail', timeout=5, bytes=600, back=back)
        t = d.get('text', '')
        if not t:
            break
        chunks.append(t)
        back += len(t.encode('utf-8'))
    return ''.join(reversed(chunks))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--transport', default='auto')
    ap.add_argument('--via', default='bootloader', choices=['reset', 'bootloader'])
    ap.add_argument('--warmup', type=int, default=4)
    ap.add_argument('--watch', type=int, default=20, help='復帰後に CDC を見張る秒数')
    ap.add_argument('--log', action='store_true', help='復帰後に本体ログを全部出す')
    args = ap.parse_args()

    c = console_hid.open_client(transport=args.transport)
    shot = c.request('camera.capture', timeout=40, warmup=args.warmup)
    st = c.request('camera.status', timeout=5)
    print('撮影: jpeg=%s B ms=%s aldo3=%s state=%s' % (
        shot.get('jpeg_bytes'), shot.get('ms'), st.get('aldo3'), st.get('state')))
    if shot.get('jpeg_bytes', 0) <= 0:
        sys.exit('撮影できていないので検査にならない: %s' % shot)
    t0 = time.time()
    try:
        c.request(args.via, timeout=2)
    except Exception as err:
        print('%s 送信後の例外 (想定内): %s' % (args.via, err))
    try:
        c.close()
    except Exception:
        pass
    if args.via == 'bootloader':
        port, kind = flash.rom_device()
        print('ROM 出現: %s (%s) %.1fs' % (port, kind, time.time() - t0))
        if kind != 'usb-otg':
            print('!! 期待は usb-otg (303A:0009)。%s に居る' % kind)
        ok = flash.leave_rom()
        if not ok:
            sys.exit('!! ROM から戻れない')
    # ★ macOS は本体が消えてから 2 秒ほどポート名を残す。先に「消えた」を
    #   見届けてから「戻った」を待たないと、古いポートを復帰と誤認する。
    if args.via == 'reset':
        tg = time.time()
        while alive() and time.time() - tg < 15:
            time.sleep(0.1)
        if alive():
            sys.exit('!! %s を送っても 15 秒で CDC が消えない (再起動していない)' % args.via)
        print('CDC 消滅: %.1fs after %s' % (time.time() - t0, args.via))
    p, dt = wait_cdc(60)
    if not p:
        sys.exit('!! %s から 60 秒で CDC が戻らない' % args.via)
    print('CDC 復帰: %s (%.1fs after %s)' % (p, time.time() - t0, args.via))
    events = watch_cdc(args.watch)
    print('CDC の推移 (%ds 監視): %s' % (args.watch, ' '.join('%.1f:%s' % e for e in events)))
    if not alive():
        sys.exit('!! 監視の終わりに本体が USB に居ない')
    c = console_hid.open_client(transport=args.transport)
    s = c.request('status', timeout=5)
    print('status: fw=%s up=%.1fs rst=%s wifi=%s volume=%s' % (
        s.get('fw'), s.get('up', -1), s.get('rst'), s.get('wifi'), s.get('volume')))
    if args.log:
        print('---- 本体ログ ----')
        print(dump_log(c))
    if s.get('up', 1e9) > 120:
        sys.exit('!! 再起動していない (up が大きい)')
    downs = [e for e in events if e[1] == 'down']
    if downs:
        sys.exit('!! 復帰後に CDC がもう一度消えた (二度目の再起動) %s' % downs)
    print('OK: 撮影 → %s → 復帰 %.1fs、その後 %ds 安定' % (args.via, time.time() - t0, args.watch))


if __name__ == '__main__':
    main()
