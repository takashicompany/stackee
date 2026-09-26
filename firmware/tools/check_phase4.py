#!/usr/bin/env python3
"""段階 4 (周辺機能と本番構成) の合否を実機で測る。**読むだけ。書き込みも再起動もしない。**

見るのは 4 つ。DESIGN.md §6 の段階 4 の行と同じ順。

  1. タッチパッド → マウス
     `touch.status` で FT6336 が居るか・読めているか。
     `touch.inject` に擬似の座標列を流して、**マウスレポートが出る**ことを数える。
     (実機の指は要らない。ホストに何も届かないので Mac の画面も動かない)

  2. カメラ
     `camera.capture` で撮り、JPEG のバイト数と所要 ms を見る。
     `camera.dump` で Mac に取り出し、**本当に JPEG として妥当か** (SOI / EOI /
     寸法) をここで確かめる。撮り終えたら ALDO3 が落ちていることも見る。
     ★ 撮影に約 4〜5 秒かかる (捨て駒 30 枚)。その間 console は答えない。

  3. コンソールの残り
     現行 CircuitPython 版 (firmware/kmk/stackee_console.py) の FEATURES が
     全部そろっているか。settings.get / settings.raw に**パスワードの値が
     1 文字も出ていない**か。bench / log.burst / lcd.status / lcd.full が
     数字を返すか。loop.* は「未対応」と答えるか。

  4. 本番 USB 構成 (full プロファイル)
     `usb.status` の profile。full なら Mac 側で
     `system_profiler SPAudioDataType` に "Stackee Mic" の入力が
     現れるか。sox か ffmpeg があれば 1 秒録ってサンプル数も見る
     (--record を付けたときだけ。**既定では録らない**)。

  python3 firmware/tools/check_phase4.py
  python3 firmware/tools/check_phase4.py --json
  python3 firmware/tools/check_phase4.py --transport hid   # full プロファイル
  python3 firmware/tools/check_phase4.py --no-camera       # 撮らない (速い)
  python3 firmware/tools/check_phase4.py --record          # UAC で 1 秒録る
  python3 firmware/tools/check_phase4.py --only look       # 撮って AI に見せる (鳴らさない)

  5. 画像を AI に見せる (--only look を書いたときだけ。**既定では走らない**)
     `camera.look play=0` で 撮影 → POST /look → 返答待ち → 返答 PCM の受信
     まで進め、**鳴らす直前で止める**。`camera.look_status` で job id /
     返答文の長さ / 受け取った PCM のバイト数 / 各段の ms / エラーを読む。
     撮影中は key.inject を回して打鍵の遅延も見る。先に `audio.null` も立てる。
     ★ サーバ (STACKEE_TALK_URL の /look) に 1 件の仕事を投げる。

  6. stackee 独自キー CSTM_n (--only cstm を書いたときだけ。**既定では走らない**)
     `key.cstm n=<--cstm-key> play=0` でキー押下と同じ流れ (GET /inbox →
     POST /key → prompt なら返答待ち → PCM / command なら受け箱 → 発話の PCM)
     を起こし、**鳴らす直前で止める**。`key.cstm_status` で mode / job id /
     発話の数・各発話の seq / 字幕長 / audio_bytes / 最終状態 / 各段の ms を読む。
     返答待ちの間は key.inject を回して打鍵の遅延も見る。先に `audio.null` を
     立て、終わったら**必ず元に戻す**。
     ★ サーバ側にそのキーの試験用の設定が要る (未設定なら ignored で NG)。

  python3 firmware/tools/check_phase4.py --only cstm --cstm-key 9
  python3 firmware/tools/check_phase4.py --only cstm --cstm-key 9 --cstm-expect command

★ 音は鳴らさない。ここが触るのは**マイク側だけ**で、スピーカーには 1 度も
  触らない (camera も touch も音とは無関係)。
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
IDF = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import stackee_tree as tree             # noqa: E402

tree.add_kmk_tools(sys.path)

import console_hid          # noqa: E402

FW_MIN = 4
# 現行 CircuitPython 版の FEATURES のうち、C 版でも同じ名前で答えるもの。
# loop.* は C 版に当たるものが無いので別扱い (「未対応」と答えれば合格)。
LEGACY_FEATURES = [
    'hello', 'status',
    'settings.get', 'settings.raw', 'settings.set',
    'wifi.scan', 'wifi.list', 'wifi.add', 'wifi.remove', 'wifi.connect',
    'camera.capture', 'camera.power', 'camera.dump',
    'bench', 'log.burst', 'lcd.status', 'lcd.full', 'reset',
]
LEGACY_LOOP = ['loop.stats', 'loop.detail', 'loop.gc',
               'loop.keytest', 'loop.keyresult']

# 撮影は捨て駒 30 枚で約 4〜5 秒。余裕を見る。
CAMERA_TIMEOUT_S = 40.0
# 画像を見せる: 返答待ちの上限 (390 秒) + 撮影 + 受信の余裕。
LOOK_TIMEOUT_S = 450.0
# コマンドの時間切れは最大 600 秒 (本体側の上限は 660 秒)。それより少し長く待つ。
CSTM_TIMEOUT_S = 700.0
INJECT_KEY = 'F24'          # ホスト側で何も起きないキー (check_phase2 と同じ)
# 打鍵の遅延の合否 (DESIGN.md §3。撮影中も同じ基準)。
KEY_MED_MS = 2.0
KEY_MAX_MS = 5.0
# ★ 内蔵 DMA でやり直すときに要る連続領域 [byte]。
#   sdkconfig の CONFIG_CAMERA_DMA_BUFFER_SIZE_MAX = 8192 のとき、
#   QVGA RGB565 (1 行 640 B) では 半バッファ 3,840 × 2 = 7,680。
#   PSRAM DMA で開けていればここは要らない (記述子 480 B だけ)。
CAMERA_FALLBACK_DMA_BYTES = 7680
# 秘密の値がそのまま出ていないかを見るための目印。settings.toml の中身は
# 読まない (読めばこちらが漏らすことになる) ので、「伏せ字があるか」と
# 「真偽になっているか」だけを見る。
MASK = '"***"'


# ---------------------------------------------------------------------------
# JPEG の検算 (Mac 側でやる。デバイスの CPU を 1 サイクルも使わない)
# ---------------------------------------------------------------------------
def jpeg_info(data):
    """JPEG として妥当かを見る。{'ok':bool,'why':str,'w':int,'h':int}。"""
    if len(data) < 4:
        return {'ok': False, 'why': 'short', 'w': 0, 'h': 0}
    if data[0:2] != b'\xff\xd8':
        return {'ok': False, 'why': 'SOI なし', 'w': 0, 'h': 0}
    if data[-2:] != b'\xff\xd9':
        return {'ok': False, 'why': 'EOI なし', 'w': 0, 'h': 0}
    # SOF0 / SOF1 / SOF2 を探して寸法を読む。
    at = 2
    while at + 4 <= len(data):
        if data[at] != 0xFF:
            at += 1
            continue
        marker = data[at + 1]
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7 or marker == 0x01:
            at += 2
            continue
        if at + 4 > len(data):
            break
        seglen = int.from_bytes(data[at + 2:at + 4], 'big')
        if marker in (0xC0, 0xC1, 0xC2):
            if at + 9 > len(data):
                break
            h = int.from_bytes(data[at + 5:at + 7], 'big')
            w = int.from_bytes(data[at + 7:at + 9], 'big')
            return {'ok': True, 'why': '', 'w': w, 'h': h}
        if marker == 0xDA:      # 画像データの始まり。ここから先は走査しない
            break
        at += 2 + seglen
    return {'ok': False, 'why': 'SOF なし', 'w': 0, 'h': 0}


# ---------------------------------------------------------------------------
# 1. タッチパッド
# ---------------------------------------------------------------------------
def check_touch(client, result, timeout):
    result['touch_before'] = client.request('touch.status', timeout=timeout)
    # 生座標で右下へ 12 点なぞる。移動が出るだけの距離を動かす。
    xy = []
    for i in range(12):
        xy += [80 + i * 12, 60 + i * 9]
    result['touch_drag'] = client.request('touch.inject', timeout=timeout,
                                          xy=xy, step_ms=5, release=False)
    # 短いタップ (10..200 ms かつ移動 30 以下) でクリックが出る。
    result['touch_tap'] = client.request('touch.inject', timeout=timeout,
                                         xy=[200, 30, 200, 30], step_ms=40,
                                         release=True)
    result['touch_after'] = client.request('touch.status', timeout=timeout)


# ---------------------------------------------------------------------------
# 2. カメラ
# ---------------------------------------------------------------------------
def fetch_jpeg(client, total, timeout, verbose=True):
    """camera.dump を繰り返して JPEG を全部取り出す。"""
    out = bytearray()
    off = 0
    while off < total:
        reply = client.request('camera.dump', timeout=timeout, off=off, n=1024)
        if reply.get('error'):
            raise RuntimeError('camera.dump: %s' % reply['error'])
        chunk = base64.b64decode(reply['b64'])
        if not chunk:
            break
        out += chunk
        off += len(chunk)
        if verbose:
            sys.stderr.write('\r  取り出し %d / %d バイト' % (off, total))
            sys.stderr.flush()
    if verbose:
        sys.stderr.write('\n')
    return bytes(out)


def check_camera(client, result, warmup, verbose=True, out_path=None):
    if verbose:
        print('  撮影中 (捨て駒 %d 枚。4〜5 秒かかる)...' % warmup)
    shot = client.request('camera.capture', timeout=CAMERA_TIMEOUT_S,
                          warmup=warmup)
    result['camera_capture'] = shot
    if shot.get('error'):
        return
    total = shot.get('jpeg_bytes', 0)
    if not total:
        return
    data = fetch_jpeg(client, total, 10.0, verbose=verbose)
    result['camera_jpeg_len'] = len(data)
    result['camera_jpeg'] = jpeg_info(data)
    if out_path:
        open(out_path, 'wb').write(data)
        result['camera_jpeg_path'] = out_path
    # ★ 撮り終えたら ALDO3 が落ちていること。落ちていないまま
    #   ハードリセットすると GC0308 が自走してブートループになる。
    result['camera_power'] = client.request('camera.status', timeout=5.0)


# ---------------------------------------------------------------------------
# 5. 画像を AI に見せる (POST /look)。★ 鳴らさない (play=0 + audio.null)
# ---------------------------------------------------------------------------
def inject_ms(client, timeout=5.0):
    """key.inject を 1 回。押下までの遅れ [ms] (失敗なら None)。"""
    try:
        reply = client.request('key.inject', timeout=timeout,
                               kc=INJECT_KEY, hold_ms=20)
    except Exception:
        return None
    if not reply.get('ok'):
        return None
    return float(reply.get('press_ms', 0))


def check_look(client, result, warmup, verbose=True):
    # ★ 二重の守り。play=0 で再生の直前に止まるが、ヌル出力も立てておく。
    #   ★★ 終わったら元に戻す。立てたままだと、そのあと人が話しかけても
    #   AI の返答が鳴らない (字幕だけ出る)。2026-09-26 に実際に踏んだ。
    was_null = client.request('audio.status', timeout=5.0).get('null') is True
    result['look_null'] = client.request('audio.null', timeout=5.0, on=True)
    try:
        _check_look_body(client, result, warmup, verbose)
    finally:
        if not was_null:
            result['look_null_restored'] = client.request(
                'audio.null', timeout=5.0, on=False)


def _check_look_body(client, result, warmup, verbose):
    before = client.request('camera.look_status', timeout=5.0)
    result['look_before'] = before
    seq0 = before.get('seq', 0)
    done0 = (before.get('talk') or {}).get('looks_done', 0) or 0
    start = client.request('camera.look', timeout=5.0, play=0, warmup=warmup)
    result['look_start'] = start
    if start.get('error'):
        return
    if verbose:
        print('  撮影 → /look → 返答待ち (鳴らさない)...')
    lat = []
    last = {}
    t0 = time.time()
    while time.time() - t0 < LOOK_TIMEOUT_S:
        last = client.request('camera.look_status', timeout=5.0)
        phase = last.get('phase')
        talk = last.get('talk') or {}
        mine = last.get('seq', 0) > seq0
        if mine and phase == 'capturing':
            ms = inject_ms(client)          # 撮影中の打鍵
            if ms is not None:
                lat.append(ms)
            continue
        if mine and phase in ('ignored', 'error'):
            break
        if mine and phase == 'submitted' and talk.get('state') == 'idle' and (
                (talk.get('looks_done', 0) or 0) > done0 or talk.get('error')):
            break
        time.sleep(1.0)
    result['look_status'] = last
    result['look_elapsed_s'] = round(time.time() - t0, 1)
    result['look_key_ms'] = lat
    result['look_camera'] = client.request('camera.status', timeout=5.0)


# ---------------------------------------------------------------------------
# 6. stackee 独自キー CSTM_n (POST /key)。★ 鳴らさない (play=0 + audio.null)
# ---------------------------------------------------------------------------
def check_cstm(client, result, key, verbose=True):
    # ★★ audio.null は終わったら**必ず元に戻す** (check_look と同じ。立てた
    #   ままだと、そのあと人が話しかけても返答が鳴らない。2026-09-26 に踏んだ)。
    was_null = client.request('audio.status', timeout=5.0).get('null') is True
    result['cstm_null'] = client.request('audio.null', timeout=5.0, on=True)
    try:
        _check_cstm_body(client, result, key, verbose)
    finally:
        if not was_null:
            result['cstm_null_restored'] = client.request(
                'audio.null', timeout=5.0, on=False)


def _check_cstm_body(client, result, key, verbose):
    before = client.request('key.cstm_status', timeout=5.0)
    result['cstm_before'] = before
    seq0 = before.get('seq', 0) or 0
    start = client.request('key.cstm', timeout=5.0, n=key, play=0)
    result['cstm_start'] = start
    if start.get('error'):
        return
    if verbose:
        print('  CSTM_%d → /key → (返答 / 受け箱) 待ち (鳴らさない)...' % key)
    lat = []
    last = {}
    t0 = time.time()
    while time.time() - t0 < CSTM_TIMEOUT_S:
        last = client.request('key.cstm_status', timeout=5.0)
        if (last.get('seq', 0) or 0) > seq0 and not last.get('active'):
            break
        ms = inject_ms(client)              # 待っている間の打鍵
        if ms is not None:
            lat.append(ms)
        time.sleep(1.0)
    result['cstm_status'] = last
    result['cstm_elapsed_s'] = round(time.time() - t0, 1)
    result['cstm_key_ms'] = lat


def cstm_verdicts(result, args, out):
    start = result.get('cstm_start')
    if start is None:
        return
    key = args.cstm_key
    if start.get('error'):
        out.append(('CSTM key.cstm', False, '%s' % start.get('error')))
        return
    st = result.get('cstm_status') or {}
    mine = (st.get('seq', 0) or 0) > ((result.get('cstm_before') or {}).get('seq', 0) or 0)
    final = st.get('final')
    mode = st.get('mode')
    says = st.get('say') or []
    detail = ('final=%s mode=%s job=%s says=%s job_state=%s 各段 %s / %s 秒 / '
              'error="%s"' % (final, mode, st.get('job_id'), st.get('says'),
                              st.get('job_state'),
                              json.dumps(st.get('t', {}), ensure_ascii=False),
                              result.get('cstm_elapsed_s'), st.get('error')))
    if final == 'ignored':
        out.append(('CSTM_%d サーバーの応答' % key, False,
                    'ignored (サーバー側に CSTM_%d の試験用の設定が要る) %s'
                    % (key, detail)))
        return
    ok = (mine and not st.get('active') and final == 'done' and
          not st.get('error') and st.get('play') == 0 and st.get('n') == key)
    if args.cstm_expect != 'any':
        ok = ok and mode == args.cstm_expect
    if mode == 'prompt':
        ok = ok and (st.get('reply_len', 0) or 0) > 0 and \
            (st.get('audio_bytes', 0) or 0) > 0
    out.append(('CSTM_%d /key → 最後まで' % key, ok, detail))
    for i, say in enumerate(says):
        out.append(('CSTM_%d 発話 %d' % (key, i + 1),
                    (not say.get('audio')) or
                    (say.get('got', 0) or 0) > 0,
                    'seq=%s 字幕 %s B / %s 頁 audio=%s 申告 %s B / 受信 %s B '
                    'played=%s (%s ms)'
                    % (say.get('seq'), say.get('sub'), say.get('pages'),
                       say.get('audio'), say.get('audio_bytes'), say.get('got'),
                       say.get('played'), say.get('at'))))
    played = [s for s in says if s.get('played')]
    out.append(('CSTM_%d 鳴らしていない' % key,
                st.get('play') == 0 and st.get('null') is True and not played,
                'play=%s null=%s 鳴らした発話 %d 件'
                % (st.get('play'), st.get('null'), len(played))))
    restored = result.get('cstm_null_restored')
    if restored is not None:
        out.append(('CSTM_%d audio.null を戻した' % key,
                     restored.get('null') is False, 'null=%s' % restored.get('null')))
    lat = result.get('cstm_key_ms') or []
    if lat:
        import statistics
        med = statistics.median(lat)
        worst = max(lat)
        out.append(('CSTM_%d 待ち中の打鍵' % key, med <= KEY_MED_MS and
                    worst <= KEY_MAX_MS,
                    '中央値 %.3f ms / 最大 %.3f ms / %d 回 (合否 ≤%.0f / ≤%.0f ms)'
                    % (med, worst, len(lat), KEY_MED_MS, KEY_MAX_MS)))


# ---------------------------------------------------------------------------
# 3. コンソールの残り
# ---------------------------------------------------------------------------
def check_console(client, result, timeout):
    result['settings_get'] = client.request('settings.get', timeout=timeout)
    result['settings_raw'] = client.request('settings.raw', timeout=timeout)
    result['bench'] = client.request('bench', timeout=timeout, n=100)
    result['log_burst'] = client.request('log.burst', timeout=timeout, n=20)
    result['lcd_status'] = client.request('lcd.status', timeout=timeout)
    result['lcd_full'] = client.request('lcd.full', timeout=timeout)
    result['usb_status'] = client.request('usb.status', timeout=timeout)
    result['loop_stats'] = client.request('loop.stats', timeout=timeout)


# ---------------------------------------------------------------------------
# 4. UAC (Mac 側から見る)
# ---------------------------------------------------------------------------
def mac_audio_inputs():
    """system_profiler から入力デバイスの名前を拾う。"""
    if sys.platform != 'darwin':
        return None
    try:
        raw = subprocess.run(
            ['system_profiler', '-json', 'SPAudioDataType'],
            capture_output=True, text=True, timeout=30)
    except Exception as err:
        return {'error': str(err)}
    if raw.returncode != 0:
        return {'error': raw.stderr.strip()[:200]}
    try:
        doc = json.loads(raw.stdout)
    except ValueError as err:
        return {'error': 'json: %s' % err}
    names = []
    def walk(node):
        if isinstance(node, dict):
            name = node.get('_name')
            if name and node.get('coreaudio_device_input'):
                names.append(name)
            for value in node.values():
                walk(value)
        elif isinstance(node, list):
            for item in node:
                walk(item)
    walk(doc)
    return {'inputs': names}


def record_one_second(device_name):
    """sox か ffmpeg があれば 1 秒録ってサンプル数を返す。無ければ None。"""
    if shutil.which('sox'):
        out = subprocess.run(
            ['sox', '-t', 'coreaudio', device_name, '-t', 'raw', '-b', '16',
             '-e', 'signed', '-c', '1', '-r', '16000', '-', 'trim', '0', '1'],
            capture_output=True, timeout=20)
        if out.returncode == 0:
            return {'tool': 'sox', 'bytes': len(out.stdout),
                    'samples': len(out.stdout) // 2}
        return {'tool': 'sox', 'error': out.stderr.decode()[:200]}
    if shutil.which('ffmpeg'):
        out = subprocess.run(
            ['ffmpeg', '-hide_banner', '-loglevel', 'error',
             '-f', 'avfoundation', '-i', ':%s' % device_name,
             '-t', '1', '-ac', '1', '-ar', '16000',
             '-f', 's16le', '-'],
            capture_output=True, timeout=30)
        if out.returncode == 0:
            return {'tool': 'ffmpeg', 'bytes': len(out.stdout),
                    'samples': len(out.stdout) // 2}
        return {'tool': 'ffmpeg', 'error': out.stderr.decode()[:200]}
    return None


def check_uac(client, result, do_record):
    result['audio_inputs'] = mac_audio_inputs()
    usb = result.get('usb_status') or {}
    if usb.get('profile') != 'full':
        return
    inputs = (result['audio_inputs'] or {}).get('inputs') or []
    # ★ 実機の名前は "Stackee Mic" (製造元 M5Stack)。2026-09-16 実測。
    hit = [n for n in inputs if 'Stackee' in n or 'M5Stack' in n or 'Core S3' in n]
    result['uac_device'] = hit[0] if hit else None
    if do_record and hit:
        result['uac_record'] = record_one_second(hit[0])


# ---------------------------------------------------------------------------
def collect(args):
    result = {}
    client = console_hid.open_client(port=args.port, transport=args.transport)
    result['transport'] = client.transport
    result['port'] = client.port_name
    try:
        result['hello'] = client.request('hello', timeout=args.timeout)
        result['status'] = client.request('status', timeout=args.timeout)
        if not args.only:
            wanted = ('touch', 'camera', 'console', 'uac')
        else:
            wanted = tuple(args.only)
        if 'touch' in wanted:
            check_touch(client, result, args.timeout)
        if 'console' in wanted:
            check_console(client, result, args.timeout)
        if 'camera' in wanted and not args.no_camera:
            check_camera(client, result, args.warmup, verbose=not args.json,
                         out_path=args.save_jpeg)
        if 'uac' in wanted:
            check_uac(client, result, args.record)
        # ★ 画像はサーバに仕事を投げるので、書いたときだけ (既定の組に入れない)。
        if args.only and 'look' in args.only:
            check_look(client, result, args.warmup, verbose=not args.json)
        # ★ CSTM もサーバに仕事を投げるので、書いたときだけ。
        if args.only and 'cstm' in args.only:
            check_cstm(client, result, args.cstm_key, verbose=not args.json)
        result['status_after'] = client.request('status', timeout=args.timeout)
    finally:
        client.close()
    return result


def fw_at_least(fw, want):
    if not isinstance(fw, str) or not fw.startswith('stackee-idf/'):
        return False
    try:
        return int(fw.split('/', 1)[1]) >= want
    except ValueError:
        return False


def verdicts(result, args):
    out = []
    hello = result.get('hello') or {}
    out.append(('ファーム', fw_at_least(hello.get('fw'), FW_MIN),
                '%s (期待 stackee-idf/%d 以上) / proto %s / profile %s'
                % (hello.get('fw'), FW_MIN, hello.get('proto'),
                   hello.get('profile'))))
    out.append(('通り道', True, '%s (%s)' % (result.get('transport'),
                                             result.get('port'))))

    # ---- 1. タッチ ------------------------------------------------------
    before = result.get('touch_before')
    if before is None:
        out.append(('タッチ', None, '見ていない'))
    else:
        present = bool(before.get('present'))
        out.append(('タッチ FT6336', present,
                    'present=%s vendor=0x%02X reads=%s read_fails=%s'
                    % (present, before.get('vendor', 0) or 0,
                       before.get('reads'), before.get('read_fails'))))
        drag = result.get('touch_drag') or {}
        moved = abs(drag.get('moved_x', 0)) + abs(drag.get('moved_y', 0))
        out.append(('タッチ なぞり → マウス', drag.get('moves', 0) > 0 and moved > 0,
                    'レポート %s 件 / 移動量 |dx|+|dy| = %s (%s us)'
                    % (drag.get('moves'), moved, drag.get('us'))))
        tap = result.get('touch_tap') or {}
        out.append(('タッチ タップ → クリック', tap.get('clicks', 0) > 0,
                    'ボタンのレポート %s 件' % tap.get('clicks')))
        after = result.get('touch_after') or {}
        # 実機のタッチ読み出しが回っていること (注入とは別の道)。
        grew = (after.get('reads', 0) > before.get('reads', 0)) if present else None
        out.append(('タッチ 読み出しが回っている', grew,
                    'reads %s -> %s' % (before.get('reads'), after.get('reads'))))

    # ---- 2. カメラ ------------------------------------------------------
    shot = result.get('camera_capture')
    if shot is None:
        out.append(('カメラ', None, '見ていない (--no-camera)'))
    elif shot.get('error'):
        out.append(('カメラ 撮影', False,
                    '%s (%s)' % (shot.get('error'), shot.get('camera_err'))))
    else:
        out.append(('カメラ 撮影', shot.get('jpeg_bytes', 0) > 0,
                    'JPEG %s バイト / %s ms (%dx%d, 生 %s バイト, 捨て駒 %s) '
                    '内訳 %s'
                    % (shot.get('jpeg_bytes'), shot.get('camera_ms'),
                       shot.get('camera_w', 0), shot.get('camera_h', 0),
                       shot.get('camera_bytes'), shot.get('camera_warmup'),
                       json.dumps(shot.get('t', {}), ensure_ascii=False))))
        info = result.get('camera_jpeg') or {}
        got_len = result.get('camera_jpeg_len', 0)
        want_len = shot.get('jpeg_bytes', 0)
        ok = (info.get('ok') and got_len == want_len and
              info.get('w') == shot.get('camera_w') and
              info.get('h') == shot.get('camera_h'))
        out.append(('カメラ 取り出した JPEG', bool(ok),
                    '%d / %d バイト / %s / %dx%d'
                    % (got_len, want_len, info.get('why') or 'SOI+EOI OK',
                       info.get('w', 0), info.get('h', 0))))
        power = result.get('camera_power') or {}
        out.append(('カメラ 撮影後の ALDO3', power.get('aldo3') == 0,
                    'aldo3=%s (0 でないとハードリセットが危ない)'
                    % power.get('aldo3')))
        # ★ 段階 4 の 1 回目はここで落ちた。内蔵 RAM の**連続した**空きが
        #   足りず、DMA バッファを取れなかった (空き合計ではなく塊の大きさ)。
        psram = shot.get('psram_dma')
        largest = shot.get('dma_largest', 0)
        ok = bool(psram) or largest >= CAMERA_FALLBACK_DMA_BYTES
        out.append(('カメラ 撮影前の内蔵 RAM', ok,
                    'PSRAM DMA %s / DMA の最大の塊 %s B '
                    '(内蔵 DMA でやり直すなら %s B 要る) / 内蔵の空き %s B'
                    % ('あり' if psram else 'なし', largest,
                       CAMERA_FALLBACK_DMA_BYTES, shot.get('internal_free'))))

    # ---- 5. 画像を AI に見せる -------------------------------------------
    look = result.get('look_status')
    if result.get('look_start') is not None:
        start = result.get('look_start') or {}
        if start.get('error'):
            out.append(('画像 camera.look', False, '%s' % start.get('error')))
        else:
            talk = (look or {}).get('talk') or {}
            done0 = ((result.get('look_before') or {}).get('talk') or {}).get(
                'looks_done', 0) or 0
            ok = ((look or {}).get('phase') == 'submitted' and
                  talk.get('state') == 'idle' and
                  (talk.get('looks_done', 0) or 0) > done0 and
                  not talk.get('error') and talk.get('look') == 1 and
                  talk.get('play') == 0 and
                  (talk.get('reply_len', 0) or 0) > 0 and
                  (talk.get('audio_bytes', 0) or 0) > 0)
            out.append(('画像 撮影 → /look → 返答 PCM', ok,
                        'phase=%s job=%s reply_len=%s audio_bytes=%s '
                        '(%s ms) 撮影 %s ms / 渡す %s ms / 各段 %s / %s 秒 / '
                        'error="%s" look_err="%s"'
                        % ((look or {}).get('phase'), talk.get('job_id'),
                           talk.get('reply_len'), talk.get('audio_bytes'),
                           talk.get('audio_ms'), (look or {}).get('capture_ms'),
                           (look or {}).get('submit_ms'),
                           json.dumps(talk.get('t', {}), ensure_ascii=False),
                           result.get('look_elapsed_s'), talk.get('error'),
                           (look or {}).get('look_err'))))
            out.append(('画像 鳴らしていない', talk.get('play') == 0 and
                        talk.get('null') is True and
                        (talk.get('t') or {}).get('play_setup', 0) == 0,
                        'play=%s null=%s play_setup=%s'
                        % (talk.get('play'), talk.get('null'),
                           (talk.get('t') or {}).get('play_setup'))))
            cam = result.get('look_camera') or {}
            out.append(('画像 撮影後の ALDO3', cam.get('aldo3') == 0,
                        'aldo3=%s' % cam.get('aldo3')))
            lat = result.get('look_key_ms') or []
            if lat:
                import statistics
                med = statistics.median(lat)
                worst = max(lat)
                out.append(('画像 撮影中の打鍵', med <= KEY_MED_MS and
                            worst <= KEY_MAX_MS,
                            '中央値 %.3f ms / 最大 %.3f ms / %d 回 '
                            '(合否 ≤%.0f / ≤%.0f ms)'
                            % (med, worst, len(lat), KEY_MED_MS, KEY_MAX_MS)))
            else:
                out.append(('画像 撮影中の打鍵', None,
                            '撮影中に key.inject を撃てなかった'))

    # ---- 6. stackee 独自キー CSTM_n ---------------------------------------
    cstm_verdicts(result, args, out)

    # ---- 3. コンソール --------------------------------------------------
    features = hello.get('features') or []
    missing = [f for f in LEGACY_FEATURES if f not in features]
    out.append(('コンソール 現行の FEATURES', not missing,
                '%d / %d 個そろっている%s'
                % (len(LEGACY_FEATURES) - len(missing), len(LEGACY_FEATURES),
                   ('。足りない: ' + ', '.join(missing)) if missing else '')))
    loop = result.get('loop_stats')
    if loop is not None:
        out.append(('コンソール loop.* は未対応と答える',
                    loop.get('error') == 'unsupported',
                    '%s / %s' % (loop.get('error'), loop.get('note'))))

    sget = result.get('settings_get')
    if sget is not None:
        keys = sget.get('keys') or {}
        secret = sget.get('secret') or []
        leaked = [k for k in secret if isinstance(keys.get(k), str)]
        out.append(('settings.get は値を伏せる', not leaked,
                    '%d 個のキー / 伏せる対象 %d 個%s'
                    % (len(keys), len(secret),
                       ('。★ 漏れ: ' + ', '.join(leaked)) if leaked else '')))
    sraw = result.get('settings_raw')
    if sraw is not None:
        text = sraw.get('text') or ''
        has_secret_line = any(k in text for k in (sget or {}).get('secret', []))
        ok = (not has_secret_line) or (MASK in text)
        out.append(('settings.raw は伏せ字になっている', ok,
                    '%d バイト / 伏せ字 %s'
                    % (sraw.get('bytes', 0), 'あり' if MASK in text else 'なし')))

    bench = result.get('bench')
    if bench is not None:
        out.append(('bench', bench.get('n') == 100,
                    'in_waiting %s ns / idle_poll %s ns / main 中央値 %s us'
                    % (bench.get('in_waiting_ns'), bench.get('idle_poll_ns'),
                       bench.get('main_med_us'))))
    burst = result.get('log_burst')
    if burst is not None:
        out.append(('log.burst', burst.get('burst_bytes', 0) > 0,
                    '%s 行 / %s us / 書けた %s バイト / 捨てた %s'
                    % (burst.get('n'), burst.get('us'),
                       burst.get('burst_bytes'), burst.get('burst_drops'))))
    lcd = result.get('lcd_status')
    if lcd is not None:
        out.append(('lcd.status / lcd.full',
                    bool(lcd.get('ready')) and
                    (result.get('lcd_full') or {}).get('ok') == 1,
                    'ready=%s %sx%s 転送 %s 回 / 行 %s'
                    % (lcd.get('ready'), lcd.get('width'), lcd.get('height'),
                       lcd.get('transfers'), lcd.get('rows_sent'))))

    # ---- 4. USB プロファイルと UAC --------------------------------------
    usb = result.get('usb_status')
    if usb is not None:
        conhid = usb.get('conhid') or {}
        out.append(('USB プロファイル', usb.get('profile') in ('dev', 'full'),
                    '%s (CDC %s) / Raw HID コンソール proto %s, 送信待ち %s, '
                    'ログ捨て %s, 押し出し %s, 0xC1 %s 回'
                    % (usb.get('profile'), 'あり' if usb.get('cdc') else 'なし',
                       conhid.get('proto'), conhid.get('tx_pending'),
                       conhid.get('tx_dropped'), conhid.get('tx_overrun'),
                       conhid.get('polls'))))
        # ★ ホストが読んでいないあいだにログを溜め込んでいないこと。
        #   溜めると応答が入らなくなり、コンソールが永久に黙る (実機で踏んだ)。
        pending = conhid.get('tx_pending', 0)
        out.append(('Raw HID の送信待ちが溜まっていない', pending < 3072,
                    '%s バイト (上限 3072。シリアルで見ているときは '
                    '0 に近いはず)' % pending))
        if usb.get('profile') == 'full':
            uac = usb.get('uac') or {}
            device = result.get('uac_device')
            out.append(('UAC マイクを Mac が見ている', bool(device),
                        '%s' % (device or '見えない。'
                                'system_profiler SPAudioDataType に無い')))
            out.append(('UAC の受け渡し', uac.get('opens', 0) >= 0,
                        'streaming=%s opens=%s frames=%s silence=%s '
                        'underruns=%s'
                        % (uac.get('streaming'), uac.get('opens'),
                           uac.get('frames'), uac.get('silence'),
                           uac.get('underruns'))))
            rec = result.get('uac_record')
            if rec is None:
                out.append(('UAC 1 秒録音', None,
                            '録っていない (--record を付けると録る / '
                            'sox か ffmpeg が要る)'))
            elif rec.get('error'):
                out.append(('UAC 1 秒録音', False,
                            '%s: %s' % (rec.get('tool'), rec.get('error'))))
            else:
                # 16 kHz なら 1 秒で 16,000 サンプル前後。
                ok = 12000 <= rec.get('samples', 0) <= 20000
                out.append(('UAC 1 秒録音', ok,
                            '%s で %s サンプル (期待 16000 前後)'
                            % (rec.get('tool'), rec.get('samples'))))
        else:
            out.append(('UAC マイク', None,
                        'dev プロファイルには入っていない。'
                        './build.sh --profile full で作った像で見ること'))

    # ---- 打鍵が止まっていないこと ---------------------------------------
    after = result.get('status_after') or {}
    perf = (after.get('perf') or {}).get('input') or {}
    if perf:
        out.append(('打鍵 (参考)', True,
                    'キー → 送出 中央値 %s us / 最大 %s us / 標本 %s'
                    % (perf.get('med_us'), perf.get('max_us'), perf.get('n'))))
    keys = after.get('keys') or {}
    out.append(('キーボードが生きている', keys.get('tca') is True,
                'TCA8418 %s / イベント %s / I2C 失敗 %s'
                % ('あり' if keys.get('tca') else 'なし',
                   keys.get('events'), keys.get('iofail'))))
    return out


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', help='シリアルのポート (dev のとき)')
    parser.add_argument('--transport', default='auto',
                        choices=('auto', 'serial', 'hid'))
    parser.add_argument('--timeout', type=float, default=15.0)
    parser.add_argument('--warmup', type=int, default=30,
                        help='カメラの捨て駒の枚数 (減らすと速いが色が決まらない)')
    parser.add_argument('--no-camera', action='store_true',
                        help='撮らない (4〜5 秒節約)')
    parser.add_argument('--save-jpeg', help='取り出した JPEG の置き場')
    parser.add_argument('--record', action='store_true',
                        help='UAC で 1 秒録る (sox か ffmpeg が要る)')
    parser.add_argument('--only', nargs='*',
                        choices=('touch', 'camera', 'console', 'uac', 'look',
                                 'cstm'),
                        help='一部だけ見る')
    parser.add_argument('--cstm-key', type=int, choices=range(10),
                        help='--only cstm で押す CSTM_n の n (サーバ側に試験用の'
                             '設定が要る)')
    parser.add_argument('--cstm-expect', default='any',
                        choices=('any', 'prompt', 'command'),
                        help='--only cstm でサーバが返すはずの mode')
    parser.add_argument('--json', action='store_true')
    args = parser.parse_args()
    if args.only and 'cstm' in args.only and args.cstm_key is None:
        parser.error('--only cstm には --cstm-key N (0..9) が要る')

    result = collect(args)
    checks = verdicts(result, args)
    if args.json:
        print(json.dumps({'result': result,
                          'verdicts': [{'name': n, 'ok': o, 'detail': d}
                                       for n, o, d in checks]},
                         ensure_ascii=False, indent=2))
    else:
        print('通り道: %s (%s)' % (result.get('transport'), result.get('port')))
        for name, ok, detail in checks:
            mark = '--' if ok is None else ('OK' if ok else 'NG')
            print('[%s] %-28s %s' % (mark, name, detail))
        if result.get('camera_jpeg_path'):
            print('\n取り出した JPEG: %s' % result['camera_jpeg_path'])
    return 0 if all(ok is not False for _n, ok, _d in checks) else 1


if __name__ == '__main__':
    sys.exit(main())
