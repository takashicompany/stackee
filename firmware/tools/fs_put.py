#!/usr/bin/env python3
"""ファイルを 1 つ、console 経由で本体の FAT に置く (段階 4)。

ネイティブ版には CircuitPython の USB ドライブ (/Volumes/CIRCUITPY) が無い。
素材を差し替える道はこれだけなので、tools/install_assets.sh はこれを呼ぶ。

    python3 firmware/tools/fs_put.py --dest stackee_assets/icons.bin path
    python3 firmware/tools/fs_put.py --transport hid --dest settings.toml f

★ 1 ファイルにつき FAT を 1 回だけ読み書き可能で付け直す
  (main/stackee_fat.c)。フラッシュを消すのはそのときだけ。
★ 途中で切ると、そのファイルは書かれない (`<名前>.part` に書いてから
  照合して rename するため)。既にあるファイルが壊れることはない。
★ 顔 (900 KB) のような大きいものは時間がかかる。1 枠あたり 768 バイト、
  シリアルなら 1 秒あたり 100 KB ほど。
"""
import argparse
import base64
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import console_hid          # noqa: E402

# 1 回に載せるバイト数。base64 で約 1.37 倍になるので、768 バイトなら
# 1,024 文字。console の 1 行の上限 (512) を超えないよう控えめにする。
CHUNK = 300     # base64 で 400 文字。本体の 1 行上限 (CONSOLE_LINE_MAX 1024) に余裕を残す
# デバイス側 (stackee_console.c の FSPUT_MAX)。
MAX_BYTES = 256 * 1024     # stackee_console.c の FSPUT_MAX と同じ (PSRAM に受ける)


def put(client, dest, data, timeout=30.0, verbose=True):
    if len(data) > MAX_BYTES:
        raise ValueError('大きすぎる (%d > %d バイト)。デバイス側の FSPUT_MAX '
                         'を上げるか、分けて置くこと' % (len(data), MAX_BYTES))
    off = 0
    started = time.monotonic()
    while off < len(data):
        chunk = data[off:off + CHUNK]
        final = (off + len(chunk)) >= len(data)
        kw = {'off': off, 'b64': base64.b64encode(chunk).decode('ascii')}
        if off == 0:
            kw['path'] = dest
        if final:
            kw['final'] = True
        reply = client.request('fs.put', timeout=timeout, **kw)
        if reply.get('error'):
            raise RuntimeError('fs.put: %s' % reply['error'])
        off += len(chunk)
        if verbose:
            sys.stderr.write('\r    %d / %d バイト' % (off, len(data)))
            sys.stderr.flush()
        if final:
            if verbose:
                sys.stderr.write('\n')
            reply['_elapsed_s'] = round(time.monotonic() - started, 2)
            return reply
    return None


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('path', help='置きたいファイル')
    parser.add_argument('--dest', required=True,
                        help='FAT の中の置き場 (先頭の / は付けない)')
    parser.add_argument('--transport', default='auto',
                        choices=('auto', 'serial', 'hid'))
    parser.add_argument('--port')
    parser.add_argument('--timeout', type=float, default=30.0)
    parser.add_argument('--quiet', action='store_true',
                        help='進み具合を出さない (秘密のファイルのとき)')
    args = parser.parse_args()

    data = open(args.path, 'rb').read()
    client = console_hid.open_client(port=args.port, transport=args.transport)
    try:
        reply = put(client, args.dest, data, timeout=args.timeout,
                    verbose=not args.quiet)
    finally:
        client.close()
    print('%s に %s バイト (%s ms / フラッシュ消去 %s 回)'
          % (reply.get('path'), reply.get('bytes'), reply.get('ms'),
             reply.get('erases')))
    return 0


if __name__ == '__main__':
    sys.exit(main())
