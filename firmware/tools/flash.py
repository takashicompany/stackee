#!/usr/bin/env python3
"""ota_0 (0x10000) だけを書き換える。1 回の起動で最後まで面倒を見る。

firmware/native-lcd/flash.py と手順はまったく同じで、像だけを引数で選べる
ようにしたもの。退避・照合・ROM 再突入・書き込み・復帰待ちの順番と、
「照合が合わなければ 1 バイトも書かない」「finally で必ず通常起動へ戻す」は
そのまま。

  # いま CircuitPython が載っている本体へ、新しいファームを書く
  python3 firmware/tools/flash.py \
      --image firmware/build/stackee.bin --image-sha <新しい像の sha256> \
      --expect-image <いま載っている像>.bin

  # 戻す (CircuitPython 版へ)
  python3 firmware/tools/flash.py --rollback \
      --image firmware/build/stackee.bin --image-sha <同じ sha256> \
      --expect-image <いま載っている像>.bin

  # 書かずに退避と照合だけ
  python3 firmware/tools/flash.py ... --dry-run

  # 固まった本体を ROM 経由で再起動するだけ (フラッシュには触らない)
  python3 firmware/tools/flash.py --reboot

★★ **1 回の ROM セッションで esptool は 1 回だけ呼ぶ。**
  2 回目は "No serial data received" で失敗し、そのまま ROM が固まる
  (2026-09-16 に再現)。退避と書き込みの両方が要るなら、
    1. --dry-run (退避と照合だけ) → 通常起動へ戻る
    2. アプリが起き上がってから 1200bps で**もう一度**突入 → --skip-backup で書く
  と **セッションを分ける**。だから既定の手順は
  「--dry-run してから --skip-backup」の 2 手になっている。

★ ROM ダウンロードモードの入り口は 2 つあり、**戻し方が違う**。
  303a:0009 (USB-OTG)       … --after hard-reset
  303a:1001 (USB-Serial/JTAG) … --after watchdog-reset
  どちらかは繋がっているデバイスの PID で自動判定する (ROM_PIDS / FINAL_RESET
  のコメントに理由)。取り違えると、書き込みは成功しているのに本体が
  ダウンロードモードで起動し続ける (2026-09-16 に発生)。

★ なぜスクリプトにするか
  README の手順を手で分割して実行すると、途中で止めたときにデバイスが
  ROM ダウンロードモードに取り残される = **キーボードが死ぬ**。

bootloader / partition table / nvs / otadata / uf2 / user_fs には触らない。
= CIRCUITPY のファイルも BLE のボンドも残る (再ペアリング不要)。
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import stackee_tree as tree             # noqa: E402

# 現行 CircuitPython 版の道具 (非公開の firmware/kmk) があればそれを使う。
# 無ければ、この道具が要る 3 つ (VID / ポート探し / カメラの電源断) だけを
# 自前で持つ。どちらでも同じ動きになる。
tree.add_kmk_tools(sys.path)
try:
    import stackee_serial as ss         # noqa: E402
except ImportError:
    class _Serial:
        """firmware/kmk が無いときの最小の代わり。"""

        VENDOR_ID = 0x303A              # Espressif / M5Stack CoreS3 共通の VID
        PRODUCT_ID = 0x811A             # 本体 (アプリ) の PID

        @staticmethod
        def find_port():
            """本体の CDC (dev プロファイル) のポート名。無ければ None。"""
            try:
                from serial.tools import list_ports
            except ImportError:
                return None
            for port in list_ports.comports():
                if (port.vid == _Serial.VENDOR_ID
                        and port.pid == _Serial.PRODUCT_ID):
                    return port.device
            return None

        @staticmethod
        def _camera_power_off_best_effort():
            """カメラを開いたままリセットするとフラッシュが読めなくなる
            (README §18)。CircuitPython 版の道具が無いここでは何もしない —
            ネイティブ版はリセット前に自分で ALDO3 を落とす。"""
            return False

    ss = _Serial()

OTA0 = 0x10000

# ROM ダウンロードモードには **入り口が 2 つ** ある。どちらから入ったかで、
# 書き込みのあと通常起動へ戻す方法が変わる。
#
#   303a:0009  USB-OTG の ROM (CDC + DFU)
#       1200bps タッチ、コンソールの bootloader、QK_BOOT、物理 RST 長押しで入る。
#       戻すのは --after hard-reset。USB-OTG には DTR/RTS で引ける
#       ストラップ線が無いので、esptool は同じ接続のまま
#       FORCE_DOWNLOAD_BOOT を消して watchdog リセットする。
#
#   303a:1001  USB-Serial/JTAG のダウンロードモード
#       USB-Serial/JTAG 側から入ったとき。**ここで --after hard-reset を使うと
#       ダウンロードモードに戻ってしまう。** USB-Serial/JTAG では DTR/RTS が
#       EN と IO0 のストラップに繋がっていて、esptool の hard-reset は
#       それを叩く。DTR が立ったままだと IO0 が低いまま再リセットされ、
#       もう一度ダウンロードモードで起動する (2026-09-16 に発生)。
#       戻すのは --after watchdog-reset。ストラップを触らず、
#       ウォッチドッグで再起動させるだけ。
ROM_PIDS = {
    0x0009: 'usb-otg',
    0x1001: 'usb-jtag',
}
ROM_DFU_PID = 0x0009                    # 1200bps タッチの行き先
ROM_JTAG_PID = 0x1001                   # USB-Serial/JTAG のダウンロードモード

# ROM セッションの最後の操作に渡す --after。
FINAL_RESET = {
    'usb-otg': 'hard-reset',
    'usb-jtag': 'watchdog-reset',
}

# 書き込む相手の ROM CDC のシリアル (= MAC)。**値は持たない。**
# 手元に CoreS3 が 2 台以上あるときだけ STACKEE_ROM_SERIAL で名指しする。
# 指定が無ければ「候補がちょうど 1 つ」のときだけ受け入れる (下の pick_rom)。
ROM_SERIAL = os.environ.get('STACKEE_ROM_SERIAL') or None
CMP = tree.backups_dir()

# 既定の像。--image / --expect-image で上書きできる。
DEFAULT_IMAGE = tree.DEFAULT_IMAGE
DEFAULT_EXPECT = CMP / 'native-lcd-polling-dc19c2f2.bin'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def pick_rom(ports):
    """候補から 1 つ選ぶ。STACKEE_ROM_SERIAL があればそれを優先する。

    list_ports の要素をそのまま受けるので、テストから呼べる。
    """
    ports = [p for p in ports
             if p.vid == ss.VENDOR_ID and p.pid in ROM_PIDS]
    # ★ STACKEE_ROM_SERIAL が無ければシリアルでは選り分けない。
    #   (シリアル = MAC なので、この道具に値を書き残さない)
    mine = ([p for p in ports if p.serial_number == ROM_SERIAL]
            if ROM_SERIAL else [])
    if len(mine) == 1:
        return mine[0]
    if mine:
        raise RuntimeError('同じシリアルの ROM CDC が %d 個見えている' % len(mine))
    # ★ USB-Serial/JTAG (303a:1001) はシリアルを名乗らないことがある。
    #   その場合だけ「候補がちょうど 1 つ」を条件に受け入れる。
    if len(ports) == 1:
        return ports[0]
    return None


def rom_device():
    """(ポート名, 入り口の種類) を返す。

    stub flasher を走らせるたびに再列挙されてポート名が変わるので、
    esptool を呼ぶ直前に必ず取り直す。
    """
    from serial.tools import list_ports
    for _ in range(60):
        found = pick_rom(list_ports.comports())
        if found is not None:
            return found.device, ROM_PIDS[found.pid]
        time.sleep(0.5)
    raise RuntimeError("対象 CoreS3 の ROM CDC を一意に特定できない")


def rom_port():
    return rom_device()[0]


def final_reset():
    """いま繋がっている ROM の種類に合った --after を返す。"""
    try:
        return FINAL_RESET[rom_device()[1]]
    except Exception:
        # 分からないときは安全側 (ストラップを触らない) に倒す。
        return 'watchdog-reset'


# このセッションで esptool を何回呼んだか。2 回目は失敗するので数えて止める。
_esptool_calls = 0


def esptool(*args, after=None):
    """esptool を 1 回呼ぶ。

    ★ **同じ ROM セッションで 2 回呼んではいけない。** 2 回目は
      "No serial data received" で失敗し、本体が ROM のまま固まる
      (2026-09-16 に再現)。ここで数えて、2 回目は呼ぶ前に止める。

    ★ ROM セッションの**最後の**操作には after を渡す。何を渡すかは
      入り口によって違う (上の ROM_PIDS のコメント)。after=None なら
      その場で判定する。別プロセスで ROM に繋ぎ直すのは失敗しやすいので、
      同じ接続のまま戻すのが原則 (繋ぎ直しは 2026-09-16 に失敗し、
      電源ボタンでの復旧が要った)。
    """
    global _esptool_calls
    _esptool_calls += 1
    if _esptool_calls > 1:
        raise RuntimeError(
            '同じ ROM セッションで esptool を 2 回呼ぼうとしている。\n'
            '  2 回目は "No serial data received" で失敗し、本体が ROM のまま\n'
            '  固まる (2026-09-16 に再現)。退避と書き込みを両方やりたいときは\n'
            '  --dry-run と --skip-backup でセッションを分けること。')
    port, kind = rom_device()
    if after is None:
        after = 'no-reset'
    elif after == 'final':
        after = FINAL_RESET[kind]
        print('ROM の入り口は %s → 戻しは --after %s' % (kind, after), flush=True)
    cmd = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '-p', port,
           '--before', 'no-reset', '--after', after, '--baud', '921600'] + list(args)
    print('$ ' + ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def enter_rom():
    import usb.core

    def present():
        """どちらの入り口でも「もう ROM にいる」とみなす。"""
        for pid in ROM_PIDS:
            dev = usb.core.find(idVendor=ss.VENDOR_ID, idProduct=pid)
            if dev is not None:
                return ROM_PIDS[pid]
        return None

    here = present()
    if here:
        print('既に ROM ダウンロードモード (%s)' % here, flush=True)
        return
    import serial
    port = ss.find_port()
    if not port and usb_app_present():
        # ★ full プロファイル (CDC 無し)。Raw HID のコンソールに `bootloader` を
        #   送る。応答は返らない (300 ms 後に落ちる) ので待たない。
        print('CDC が無い (full) → Raw HID の bootloader で ROM へ', flush=True)
        try:
            import console_hid
            client = console_hid.open_client(transport='hid')
            try:
                client.request('bootloader', timeout=1.5)
            except Exception:
                pass
            try:
                client.close()
            except Exception:
                pass
        except Exception as err:
            sys.exit('Raw HID で bootloader を送れない: %s' % err)
    elif not port:
        sys.exit('本体 (303A:811A) が USB に見えない。ケーブルを確認。')
    else:
        print('1200bps タッチ → ROM ダウンロードモードへ (%s)' % port, flush=True)
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, 1200, 0.2
        s.dtr = True
        s.open()
        time.sleep(0.3)
        s.dtr = False
        time.sleep(0.3)
        s.close()
    for _ in range(20):
        time.sleep(1)
        if present():
            print('ROM ダウンロードモード (303a:0009) を確認', flush=True)
            return
    sys.exit('ROM DFU が現れない。USB 最下層も応答していない → 物理リセットが要る\n'
             '  RST を緑の LED が点くまで約 2 秒長押しするか、USB-Serial/JTAG の\n'
             '  ダウンロードモード (303a:1001) へ入れてから、もう一度実行する。')


def usb_app_present():
    """本体のアプリ (303A:811A) が USB に居るか。full プロファイルは CDC が
    無いので、ポート名ではなくこれで「戻った」を判定する。"""
    try:
        import usb.core
        return usb.core.find(idVendor=ss.VENDOR_ID, idProduct=0x811A) is not None
    except Exception:
        return False


def app_alive():
    """dev なら CDC のポート名、full なら USB の列挙で本体の復帰を見る。"""
    port = ss.find_port()
    if port:
        return port
    return '303A:811A (CDC 無し = full プロファイル)' if usb_app_present() else None


def leave_rom():
    """FORCE_DOWNLOAD_BOOT を消して watchdog-reset。フラッシュには書かない。"""
    from esptool.targets.esp32s3 import ESP32S3ROM
    print('通常起動へ戻す (watchdog-reset)', flush=True)
    # ★ 実機では esptool がポートを閉じた時点で本体が自分で再起動することが
    #   ある (2026-09-14 の dry-run で確認)。その場合 ROM CDC はもう無いので、
    #   ここで失敗しても異常扱いにせず、本体の復帰だけを待つ。
    if app_alive():
        print('本体は既に通常起動へ戻っている', flush=True)
    else:
        try:
            port, kind = rom_device()
            rom = ESP32S3ROM(port)
            try:
                rom.connect(mode='no-reset', attempts=3)
                if kind == 'usb-otg':
                    # FORCE_DOWNLOAD_BOOT (RTC_CNTL_OPTION1_REG) を消す。
                    # 1200bps タッチや QK_BOOT はこのビットで ROM に入るので、
                    # 消さないと次の起動もダウンロードモードになる。
                    rom.write_reg(0x6000812C, 0, 1)
                # ★ USB-Serial/JTAG で入った場合はこのビットが立っていない。
                #   立っていないものを消しても害は無いが、こちらで本体を
                #   ダウンロードモードに留めているのは DTR/RTS のストラップ
                #   なので、触らずにウォッチドッグで落とすだけにする。
                rom.watchdog_reset()
            finally:
                rom._port.close()
        except Exception as err:
            print('ROM 側の戻し処理は空振り (%s)。復帰を待つ' % err, flush=True)
    for i in range(150):
        time.sleep(0.1)
        if app_alive():
            print('★ 本体の CDC 復帰 (%.1fs): %s' % ((i + 1) / 10, app_alive()),
                  flush=True)
            return True
    # ★ 最後の手段: Mac から USB のバスリセットをかける。
    #   2026-09-16 に 2 回、esptool の watchdog-reset が空振りして ROM
    #   (303A:0009) に居座ったまま動かなくなった。pyusb で dev.reset() を
    #   撃つと、そのたびに通常起動へ戻った。"Entity not found" が出ても
    #   **失敗ではない** — リセットの直後にデバイスが再列挙されるので、
    #   ハンドルが無効になって出ているだけ。
    if usb_bus_reset():
        for i in range(150):
            time.sleep(0.1)
            if app_alive():
                print('★ USB バスリセットで復帰 (%.1fs): %s'
                      % ((i + 1) / 10, app_alive()), flush=True)
                return True
    # ★ 最後の最後: USB-Serial/JTAG (303A:1001) で ROM がブートループしている
    #   = フラッシュが読めない ("invalid header: 0xffffff1f")。2026-09-16 に
    #   カメラを開いたあとの `bootloader` → watchdog-reset で起きた (README §17)。
    #   ROM から I2C を叩いて AXP2101 に電源再投入させると戻る (tools/pmic_cycle.py)。
    if pmic_rescue():
        for i in range(300):
            time.sleep(0.1)
            if app_alive():
                print('★ 電源再投入で復帰 (%.1fs): %s'
                      % ((i + 1) / 10, app_alive()), flush=True)
                return True
    print('!! 30 秒待っても CDC が戻らない', file=sys.stderr)
    return False


def pmic_rescue():
    """303A:1001 に居る本体を AXP2101 の電源再投入で立て直す。効いたら True。"""
    from serial.tools import list_ports
    found = pick_rom(list_ports.comports())
    if found is None or ROM_PIDS.get(found.pid) != 'usb-jtag':
        return False
    try:
        import pmic_cycle
    except ImportError as err:
        print('pmic_cycle が読めない: %s' % err, file=sys.stderr)
        return False
    print('USB-Serial/JTAG の ROM が残っている。電源再投入 (AXP2101) を試す',
          flush=True)
    try:
        if not pmic_cycle.enter_download_via_jtag(found.device):
            print('ROM をダウンロードモードで止められない', file=sys.stderr)
            return False
        return pmic_cycle.power_cycle(found.device)
    except Exception as err:
        print('電源再投入に失敗: %s' % err, file=sys.stderr)
        return False


def usb_bus_reset():
    """ROM CDC (303A:0009) に USB のバスリセットをかける。効いたら True。

    esptool を通さない道なので、ROM のシリアルが固まっていても届く。
    Mac では権限は要らない (ROM は kext に握られていない)。
    """
    try:
        import usb.core
    except ImportError:
        print('pyusb が無いのでバスリセットは試さない '
              '(pip install pyusb)', file=sys.stderr)
        return False
    found = False
    for pid in ROM_PIDS:
        try:
            devices = list(usb.core.find(find_all=True, idVendor=ss.VENDOR_ID,
                                         idProduct=pid))
        except Exception as err:
            print('pyusb で探せない: %s' % err, file=sys.stderr)
            return False
        for dev in devices:
            found = True
            print('USB バスリセット %04x:%04x' % (ss.VENDOR_ID, pid), flush=True)
            try:
                dev.reset()
            except Exception as err:
                # "Entity not found" は再列挙が始まった合図。異常ではない。
                print('  reset(): %s (再列挙を待つ)' % err, flush=True)
    if not found:
        print('ROM デバイスが見当たらない (既に居ない可能性)', flush=True)
    return found


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--image', type=Path, default=DEFAULT_IMAGE,
                    help='書き込む像 (既定: %s)' % DEFAULT_IMAGE)
    ap.add_argument('--image-sha', default=None,
                    help='--image の sha256。合わなければ何もしない')
    ap.add_argument('--expect-image', type=Path, default=DEFAULT_EXPECT,
                    help='いま本体に載っているはずの像 (既定: %s)' % DEFAULT_EXPECT.name)
    ap.add_argument('--expect-sha', default=None,
                    help='--expect-image の sha256 の代わりに直接指定する')
    ap.add_argument('--rollback', action='store_true',
                    help='--expect-image を書き戻す (いま載っているのは --image)')
    ap.add_argument('--dry-run', action='store_true',
                    help='退避と照合だけ。1 バイトも書かない')
    ap.add_argument('--reboot', action='store_true',
                    help='フラッシュに触らず ROM 経由で再起動だけする')
    ap.add_argument('--offset', type=lambda x: int(x, 0), default=OTA0,
                    help='書き込み先 (既定: 0x10000 = ota_0)')
    ap.add_argument('--skip-backup', action='store_true',
                    help='退避を省き、1 回の ROM 突入で書き込む。直前の --dry-run で '
                         '退避と照合を済ませてあるときだけ使う (ROM 突入を 2 回に'
                         'すると 2 回目で本体が応答しなくなることがある: 2026-09-16)')
    return ap


def resolve(args):
    """(書き込む像, その sha256, 今載っているはずの sha256, 退避する長さ) を決める。

    --rollback は「書く像」と「今載っている像」を入れ替えるだけ。
    照合の sha256 が 1 つでも欠けていたら、書かずに止める。
    """
    if args.rollback:
        target, other = args.expect_image, args.image
        target_sha, other_sha = args.expect_sha, args.image_sha
    else:
        target, other = args.image, args.expect_image
        target_sha, other_sha = args.image_sha, args.expect_sha

    if not target.exists():
        raise SystemExit('像が無い: %s' % target)
    if not other.exists():
        raise SystemExit('照合用の像が無い: %s' % other)

    got = sha256(target)
    if target_sha is not None and got != target_sha:
        raise SystemExit('像の sha256 が違う: %s\n  期待 %s\n  実際 %s'
                         % (target, target_sha, got))
    expect_sha = other_sha if other_sha is not None else sha256(other)
    return target, got, expect_sha, other.stat().st_size


def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.reboot:
        # VM が固まっていても 1200bps タッチは USB の下層で受けるので ROM へ入れる。
        # 読み書きは一切せず、watchdog-reset で通常起動へ戻す。
        enter_rom()
        return 0 if leave_rom() else 3

    target, target_sha, expect_sha, expect_size = resolve(args)
    size = target.stat().st_size
    print('書き込む像: %s (%d B, sha256 %s)' % (target.name, size, target_sha), flush=True)
    print('本体に載っているはず: sha256 %s (先頭 %d B を照合)'
          % (expect_sha, expect_size), flush=True)

    ss._camera_power_off_best_effort()
    enter_rom()

    status = 1
    try:
        if args.skip_backup and not args.dry_run:
            print('--skip-backup: 退避は直前の --dry-run 済みとして、このまま書き込む', flush=True)
            esptool('write-flash', '--flash-mode', 'keep', '--flash-freq', 'keep',
                    '--flash-size', 'keep', hex(args.offset), str(target),
                    after='final')
            status = 0
            return status
        CMP.mkdir(parents=True, exist_ok=True)
        backup = CMP / ('ota0-backup-%s.bin' % time.strftime('%Y%m%d-%H%M%S'))
        esptool('read-flash', hex(args.offset), str(expect_size), str(backup),
                after='final' if args.dry_run else 'no-reset')
        got = sha256(backup)
        print('退避: %s (%d B)\n  sha256 %s'
              % (backup, backup.stat().st_size, got), flush=True)
        if got != expect_sha:
            print('!! 実機の ota_0 が想定と違う。**書き込まずに中止する**\n'
                  '   期待 %s\n   実際 %s' % (expect_sha, got), file=sys.stderr)
            return 2
        print('実機の ota_0 は想定どおり', flush=True)
        if args.dry_run:
            print('--dry-run のため書き込まない', flush=True)
            return 0
        # read-flash を閉じると本体が勝手に再起動することがある。その復帰を
        # 済ませてから、書き込み用に ROM を開き直す。
        if not leave_rom():
            raise RuntimeError('退避後の通常起動を確認できないため書き込まない')
        ss._camera_power_off_best_effort()
        enter_rom()
        esptool('write-flash', '--flash-mode', 'keep', '--flash-freq', 'keep',
                '--flash-size', 'keep', hex(args.offset), str(target),
                after='final')
        status = 0
    finally:
        if not leave_rom():
            status = 3
    return status


if __name__ == '__main__':
    sys.exit(main())
