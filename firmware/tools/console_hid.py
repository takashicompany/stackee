#!/usr/bin/env python3
"""stackee の常駐コンソールを **Raw HID** で話す Mac 側クライアント (段階 4)。

full プロファイル (DESIGN.md §4) には CDC が無い。ESP32-S3 の IN
エンドポイントが EP0 込みで 5 本しかなく、UAC マイクに 1 本使うため。
そこでコンソールとログを VIA の Raw HID に相乗りさせてある
(firmware/main/stackee_conhid.c)。こちらはその Mac 側。

★ 枠の中身は CDC のときと**まったく同じバイト列**。だから
  firmware/kmk/tools/stackee_console_client.py の FrameParser を
  そのまま使い回せる。違うのは「バイトの運び方」だけ。

  32 バイトのレポート:
    byte 0     command id (0xC0 送信 / 0xC1 受信 / 0xC2 情報)
    byte 1     len   (payload の有効バイト数、0..29)
    byte 2     flags (bit0 = まだ続きがある)
    byte 3..31 payload

使い方 (単体で):
    python3 firmware/tools/console_hid.py hello
    python3 firmware/tools/console_hid.py status
    python3 firmware/tools/console_hid.py --transport serial status
    python3 firmware/tools/console_hid.py watch 10

他のスクリプトから:
    import console_hid
    client = console_hid.open_client()      # 見つかったほうで開く
    print(client.request('hello'))

★ 実機には**読むことしかしない**。書き込みも再起動もここからは撃たない
  (reset / bootloader はコマンドとして送れるが、このファイルは自分から
  撃たない)。
"""
import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import stackee_tree as _tree            # noqa: E402

# シリアル経路 (dev プロファイル) の枠は現行 CircuitPython 版の道具を借りる。
# 非公開側にしか無いので、無ければ Raw HID 経路だけを使う。
_tree.add_kmk_tools(sys.path)

# ---------------------------------------------------------------------------
# 枠 (純粋な関数。テストはここを直接叩く)
# ---------------------------------------------------------------------------
REPORT_SIZE = 32
HEADER_SIZE = 3
MAX_PAYLOAD = REPORT_SIZE - HEADER_SIZE     # 29

CMD_TX = 0xC0
CMD_RX = 0xC1
CMD_INFO = 0xC2

FLAG_MORE = 0x01

# stackee_conhid.h の STACKEE_CONHID_PROTO。
CONHID_PROTO = 1

USB_VID = 0x303A
USB_PID = 0x811A
USAGE_PAGE = 0xFF60
USAGE = 0x61


def encode_tx_reports(data):
    """バイト列を 0xC0 のレポート列 (各 32 バイト) に割る。"""
    if isinstance(data, str):
        data = data.encode('utf-8')
    out = []
    for off in range(0, len(data), MAX_PAYLOAD):
        chunk = data[off:off + MAX_PAYLOAD]
        rep = bytearray(REPORT_SIZE)
        rep[0] = CMD_TX
        rep[1] = len(chunk)
        rep[2] = 0
        rep[HEADER_SIZE:HEADER_SIZE + len(chunk)] = chunk
        out.append(bytes(rep))
    return out


def poll_report():
    """0xC1 (受信ポーリング) の 32 バイト。"""
    rep = bytearray(REPORT_SIZE)
    rep[0] = CMD_RX
    return bytes(rep)


def info_report():
    """0xC2 (情報) の 32 バイト。"""
    rep = bytearray(REPORT_SIZE)
    rep[0] = CMD_INFO
    return bytes(rep)


def decode_rx(report):
    """受信レポートを (id, payload, more) に分ける。壊れていれば ValueError。"""
    if report is None or len(report) < HEADER_SIZE:
        raise ValueError('レポートが短い: %r' % (report,))
    rid = report[0]
    length = report[1]
    flags = report[2]
    if length > MAX_PAYLOAD:
        raise ValueError('len が大きすぎる: %d' % length)
    if len(report) < HEADER_SIZE + length:
        raise ValueError('payload が足りない')
    return rid, bytes(report[HEADER_SIZE:HEADER_SIZE + length]), \
        bool(flags & FLAG_MORE)


def parse_info(report):
    """0xC2 の応答から {proto, pending, dropped} を作る。"""
    rid, _payload, _more = decode_rx(report)
    if rid != CMD_INFO:
        raise ValueError('0xC2 の応答ではない: 0x%02X' % rid)
    proto = report[1]
    pending = int.from_bytes(report[3:7], 'little')
    dropped = int.from_bytes(report[7:11], 'little')
    return {'proto': proto, 'pending': pending, 'dropped': dropped}


# ---------------------------------------------------------------------------
# hidapi の入り口
# ---------------------------------------------------------------------------
# ★ ここは 2 つの別々の問題を同時に片付けている。**どちらも実機で踏んだ。**
#
# (1) ライブラリが見つからない (macOS / Apple Silicon)
#     PyPI の `hid` (apmorton/pyhidapi) は ctypes で
#     `LoadLibrary('libhidapi.dylib')` を**名前だけ**で呼ぶ。Homebrew は
#     /opt/homebrew/lib に置くが、そこは framework Python の dyld 既定の
#     探索先に入っていないので import そのものが ImportError で落ちる
#     ("Unable to load any of the following libraries:...")。
#     DYLD_LIBRARY_PATH はプロセス開始前にしか効かないので後から直せない。
#     → **import する瞬間だけ LoadLibrary を差し替えて**、こちらで見つけた
#       絶対パスを渡す。
#
# (2) `hid` という名前のモジュールが 2 種類ある
#     ・PyPI `hid` (pyhidapi)      … hid.Device(path=...) / .read(n, ms) /
#                                    .nonblocking = 1  ★ いま入っているのはこちら
#     ・PyPI `hidapi` (cython-hidapi) … hid.device() / .open_path() /
#                                    .set_nonblocking()
#     API がまるで違うので、どちらでも動く薄い覆いを被せる。
import ctypes
import ctypes.util
import glob as _glob

# 探す先。上から順に試す。
_DYLIB_HINTS = (
    '/opt/homebrew/lib/libhidapi.dylib',        # Homebrew (Apple Silicon)
    '/usr/local/lib/libhidapi.dylib',           # Homebrew (Intel)
    '/opt/homebrew/lib/libhidapi.0.dylib',
    '/usr/local/lib/libhidapi.0.dylib',
    '/usr/lib/libhidapi.dylib',
)


def find_hidapi_library():
    """libhidapi の絶対パス。見つからなければ None。"""
    for path in _DYLIB_HINTS:
        if os.path.exists(path):
            return path
    found = ctypes.util.find_library('hidapi')
    if found and os.path.isabs(found):
        return found
    # Homebrew の Cellar を直接探す (lib/ への symlink が無い構成の保険)。
    for pattern in ('/opt/homebrew/Cellar/hidapi/*/lib/libhidapi.dylib',
                    '/usr/local/Cellar/hidapi/*/lib/libhidapi.dylib'):
        hits = sorted(_glob.glob(pattern))
        if hits:
            return hits[-1]
    return None


_HID = None
_HID_ERROR = None


def _import_hid():
    """`hid` を返す。読めなければ None (理由は _HID_ERROR)。"""
    global _HID, _HID_ERROR
    if _HID is not None or _HID_ERROR is not None:
        return _HID
    lib = find_hidapi_library()
    real = ctypes.cdll.LoadLibrary

    def patched(name):
        # pyhidapi は名前だけで呼ぶ。見つけた絶対パスに読み替える。
        if lib and not os.path.isabs(name) and 'hidapi' in name:
            try:
                return real(lib)
            except OSError:
                pass
        return real(name)

    ctypes.cdll.LoadLibrary = patched
    try:
        import hid
        _HID = hid
    except Exception as err:      # ImportError / OSError
        _HID_ERROR = '%s' % err
    finally:
        ctypes.cdll.LoadLibrary = real
    if _HID is None:
        try:
            import hidapi as _alt
            _HID = _alt
        except Exception:
            pass
    return _HID


def hid_available():
    return _import_hid() is not None


def hid_problem():
    """使えない理由を日本語で返す。使えるなら None。"""
    if hid_available():
        return None
    lib = find_hidapi_library()
    if lib is None:
        return ('libhidapi が見つからない。macOS なら `brew install hidapi`、'
                'そのあと `python3 -m pip install --user hid`')
    return ('hid パッケージが読めない (libhidapi は %s にある): %s。'
            '`python3 -m pip install --user hid` を試すこと' % (lib, _HID_ERROR))


# ---- 2 種類の API を 1 つに見せる覆い ---------------------------------------
class _Backend:
    """open / write / read / close だけの薄い覆い。

    どちらの実装でも「report は先頭に Report ID の 0 を足した 33 バイトを
    書き、読みは 32 バイトぶん」になるように揃える。
    """

    def __init__(self, hid, path):
        self.kind = 'pyhidapi' if hasattr(hid, 'Device') and not hasattr(hid, 'device') \
            else 'cython'
        if self.kind == 'pyhidapi':
            self.dev = hid.Device(path=path)
            self.dev.nonblocking = 1
        else:
            self.dev = hid.device()
            self.dev.open_path(path)
            self.dev.set_nonblocking(1)

    def write(self, data):
        if self.kind == 'pyhidapi':
            return self.dev.write(bytes(data))
        return self.dev.write(list(bytes(data)))

    def read(self, size, timeout_ms):
        data = self.dev.read(size, timeout_ms)
        if not data:
            return None
        return bytes(data)

    def close(self):
        try:
            self.dev.close()
        except Exception:
            pass


def list_devices():
    """stackee の Raw HID インターフェースの一覧 (辞書の list)。"""
    hid = _import_hid()
    if hid is None:
        return []
    out = []
    for info in hid.enumerate(USB_VID, USB_PID):
        if isinstance(info, dict):
            page, usage, path = (info.get('usage_page'), info.get('usage'),
                                 info.get('path'))
            product = info.get('product_string')
        else:                       # hidapi パッケージは属性で返す
            page, usage = info.usage_page, info.usage
            path, product = info.path, info.product_string
        if page == USAGE_PAGE and usage == USAGE:
            out.append({'path': path, 'usage_page': page, 'usage': usage,
                        'product_string': product})
    return out


# ---------------------------------------------------------------------------
# クライアント
# ---------------------------------------------------------------------------
DEFAULT_TIMEOUT = 5.0
# 応答待ちのときの受信ポーリング間隔 [秒]。デバイスは 1 ms 周期で
# レポートを返せるので、詰めても本体は止まらない。
POLL_BUSY_S = 0.002
POLL_IDLE_S = 0.02


class HidConsoleClient:
    """firmware/kmk/tools/stackee_console_client.py の ConsoleClient と同じ口。"""

    def __init__(self, path=None, show_log=False):
        hid = _import_hid()
        if hid is None:
            raise RuntimeError(hid_problem())
        devices = list_devices()
        if path is None:
            if not devices:
                raise RuntimeError(
                    'Raw HID なし (0x%04X:0x%04X の usage_page 0x%04X / '
                    'usage 0x%02X が見えない)。デバイスが USB に刺さっているか、'
                    '像が段階 4 以降か確かめること'
                    % (USB_VID, USB_PID, USAGE_PAGE, USAGE))
            path = devices[0]['path']
        self.path = path
        self.port_name = path.decode() if isinstance(path, bytes) else str(path)
        self.transport = 'hid'
        self.dev = _Backend(hid, path)

        # 受信バイト列の切り分けは CDC 版と同じものを使う。
        import stackee_console_client as ccl
        self.parser = ccl.FrameParser()
        self._encode_request = ccl.encode_request
        self.pending = []
        self.log = []
        self.show_log = show_log
        self._next_id = 1

    # ---- 下まわり ---------------------------------------------------------
    def _write(self, report):
        # Report ID なしのデバイスなので、先頭に 0 を足して送る
        # (hidapi の約束: 最初のバイトは Report ID)。
        self.dev.write(b'\x00' + report)

    def _read(self, timeout_s=0.05):
        deadline = time.monotonic() + timeout_s
        while True:
            data = self.dev.read(REPORT_SIZE, 5)
            if data:
                return data
            if time.monotonic() >= deadline:
                return None

    def _exchange(self, report, timeout_s=1.0):
        """1 枚送って 1 枚受け取る。応答が来なければ None。"""
        self._write(report)
        return self._read(timeout_s)

    def _pump(self, rounds=1):
        """溜まっているものを吸い出して切り分ける。"""
        for _ in range(rounds):
            reply = self._exchange(poll_report(), 0.5)
            if reply is None:
                return False
            try:
                rid, payload, more = decode_rx(reply)
            except ValueError:
                return False
            if rid != CMD_RX:
                continue
            if payload:
                frames, log_text = self.parser.feed(payload)
                self.pending.extend(frames)
                if log_text:
                    self.log.append(log_text)
                    if self.show_log:
                        sys.stderr.write(log_text)
                        sys.stderr.flush()
            if not more:
                return bool(payload)
        return True

    # ---- 口 ---------------------------------------------------------------
    def close(self):
        try:
            self.dev.close()
        except Exception:
            pass

    def info(self):
        reply = self._exchange(info_report(), 1.0)
        if reply is None:
            raise TimeoutError('0xC2 に応答なし')
        return parse_info(reply)

    def _drain_backlog(self, budget=512):
        """溜まっているものを先に吸い出す。

        ★ これをやらないと、古いファーム (ログを無条件に溜める版) では
          環状バッファが起動ログで満杯のまま応答が入らず、
          **hello が永久に返らない**。新しいファームはログを
          「読んでいるとき」しか溜めないが、こちらでも先に空けておく。
        """
        for _ in range(budget):
            reply = self._exchange(poll_report(), 0.3)
            if reply is None:
                return
            try:
                rid, payload, more = decode_rx(reply)
            except ValueError:
                return
            if payload:
                frames, log_text = self.parser.feed(payload)
                self.pending.extend(frames)
                if log_text:
                    self.log.append(log_text)
                    if self.show_log:
                        sys.stderr.write(log_text)
                        sys.stderr.flush()
            if not more:
                return

    def request(self, cmd, timeout=DEFAULT_TIMEOUT, **kw):
        self._drain_backlog()
        rid = self._next_id
        self._next_id += 1
        line = self._encode_request(rid, cmd, **kw)
        for report in encode_tx_reports(line):
            ack = self._exchange(report, 1.0)
            if ack is None:
                raise TimeoutError('0xC0 に応答なし (cmd=%s)' % cmd)
            # ★ デバイスが一部しか受け取らなかったら、その場では諦める。
            #   溢れているのは異常なので、静かに続けると枠が壊れる。
            if ack[0] == CMD_TX and ack[1] != report[1]:
                raise IOError('デバイスが %d/%d バイトしか受け取れない'
                              % (ack[1], report[1]))
        started = time.monotonic()
        deadline = started + timeout
        while time.monotonic() < deadline:
            got = self._pump(rounds=8)
            for i, frame in enumerate(self.pending):
                if frame.get('id') == rid:
                    del self.pending[i]
                    frame['_rtt_ms'] = round(
                        (time.monotonic() - started) * 1000, 2)
                    return frame
            time.sleep(POLL_BUSY_S if got else POLL_IDLE_S)
        raise TimeoutError('応答なし: cmd=%s id=%d (%.1f 秒)'
                           % (cmd, rid, timeout))

    def drain(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if not self._pump(rounds=8):
                time.sleep(POLL_IDLE_S)


# ---------------------------------------------------------------------------
# dev / full どちらでも開く
# ---------------------------------------------------------------------------
def open_client(port=None, transport='auto', show_log=False):
    """シリアル (dev) か Raw HID (full) のうち、使えるほうで開く。

    transport は 'auto' / 'serial' / 'hid'。'auto' はシリアルを先に試す
    (dev プロファイルのほうが応答が速く、ログもそのまま流れてくるため)。

    返るものは ConsoleClient か HidConsoleClient。どちらも
    `request(cmd, timeout=..., **kw)` / `drain(s)` / `close()` /
    `port_name` / `transport` を持つ。
    """
    errors = []
    if transport in ('auto', 'serial'):
        try:
            import stackee_console_client as ccl
            client = ccl.ConsoleClient(port=port, show_log=show_log)
            client.transport = 'serial'
            return client
        except Exception as err:      # pyserial 無し / ポート無し
            errors.append('serial: %s' % err)
            if transport == 'serial':
                raise
    if transport in ('auto', 'hid'):
        try:
            return HidConsoleClient(show_log=show_log)
        except Exception as err:
            errors.append('hid: %s' % err)
            if transport == 'hid':
                raise
    raise RuntimeError('コンソールに繋がらない (' + ' / '.join(errors) + ')')


# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('cmd', help='hello / status / watch / 任意のコマンド名')
    parser.add_argument('rest', nargs='*')
    parser.add_argument('--transport', default='auto',
                        choices=('auto', 'serial', 'hid'))
    parser.add_argument('--port', help='シリアルのポート (dev のとき)')
    parser.add_argument('--timeout', type=float, default=DEFAULT_TIMEOUT)
    parser.add_argument('--log', action='store_true', help='ログも流す')
    args = parser.parse_args()

    if args.cmd == 'devices':
        for info in list_devices():
            print('%s  usage_page=0x%04X usage=0x%02X  %s'
                  % (info.get('path'), info.get('usage_page', 0),
                     info.get('usage', 0), info.get('product_string')))
        return 0

    client = open_client(port=args.port, transport=args.transport,
                         show_log=args.log)
    try:
        print('# transport=%s port=%s' % (client.transport, client.port_name),
              file=sys.stderr)
        if args.cmd == 'watch':
            client.drain(float(args.rest[0]) if args.rest else 5.0)
            sys.stdout.write(''.join(client.log))
            return 0
        if args.cmd == 'info':
            print(json.dumps(client.info(), ensure_ascii=False))
            return 0
        kw = {}
        for item in args.rest:
            if '=' not in item:
                continue
            key, value = item.split('=', 1)
            try:
                kw[key] = json.loads(value)
            except ValueError:
                kw[key] = value
        reply = client.request(args.cmd, timeout=args.timeout, **kw)
        print(json.dumps(reply, ensure_ascii=False, indent=2, sort_keys=True))
    finally:
        client.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
