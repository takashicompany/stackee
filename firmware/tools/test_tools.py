#!/usr/bin/env python3
"""ホスト側の道具の静的な単体テスト。実機には触らない。

  python3 -m pytest firmware/tools/test_tools.py
  python3 firmware/tools/test_tools.py        # pytest が無くても動く

見るのは「実機に触る前に判断を誤らない」ところだけ:
  * flash.py の引数解決 (--rollback で書く像と照合する像が入れ替わるか)
  * flash.py の sha256 照合 (合わなければ SystemExit で止まるか)
  * check_phase0.py の判定 (status の中身から OK / NG を正しく出すか)
  * hid_desc_check.py の解析 (記述子が設計どおりか)
"""
import hashlib
import os
import sys
import types
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import stackee_tree as tree             # noqa: E402

tree.add_kmk_tools(sys.path)

import hid_desc_check                    # noqa: E402


def _load_flash():
    """flash.py は import 時に pyserial 等を要求するので、無ければ飛ばす。"""
    try:
        import flash
        return flash
    except (Exception, SystemExit) as err:      # pragma: no cover - 環境依存
        raise unittest.SkipTest('flash.py を import できない: %s' % err)


class FlashArgsTest(unittest.TestCase):
    def setUp(self):
        self.flash = _load_flash()
        self.tmp = HERE / '_test_tmp'
        self.tmp.mkdir(exist_ok=True)
        self.new = self.tmp / 'new.bin'
        self.old = self.tmp / 'old.bin'
        self.new.write_bytes(b'new image')
        self.old.write_bytes(b'old image')
        self.new_sha = hashlib.sha256(b'new image').hexdigest()
        self.old_sha = hashlib.sha256(b'old image').hexdigest()

    def tearDown(self):
        for path in (self.new, self.old):
            if path.exists():
                path.unlink()
        if self.tmp.exists():
            self.tmp.rmdir()

    def parse(self, *argv):
        return self.flash.build_parser().parse_args(list(argv))

    def test_forward_writes_new_and_expects_old(self):
        args = self.parse('--image', str(self.new), '--image-sha', self.new_sha,
                          '--expect-image', str(self.old))
        target, target_sha, expect_sha, expect_size = self.flash.resolve(args)
        self.assertEqual(target, self.new)
        self.assertEqual(target_sha, self.new_sha)
        self.assertEqual(expect_sha, self.old_sha)
        self.assertEqual(expect_size, len(b'old image'))

    def test_rollback_swaps_the_two_images(self):
        args = self.parse('--rollback', '--image', str(self.new),
                          '--image-sha', self.new_sha,
                          '--expect-image', str(self.old))
        target, target_sha, expect_sha, expect_size = self.flash.resolve(args)
        self.assertEqual(target, self.old)          # 書くのは古い方
        self.assertEqual(target_sha, self.old_sha)
        self.assertEqual(expect_sha, self.new_sha)  # 載っているはずは新しい方
        self.assertEqual(expect_size, len(b'new image'))

    def test_wrong_sha_stops_before_touching_the_device(self):
        args = self.parse('--image', str(self.new), '--image-sha', 'de' * 32,
                          '--expect-image', str(self.old))
        with self.assertRaises(SystemExit):
            self.flash.resolve(args)

    def test_missing_image_stops(self):
        args = self.parse('--image', str(self.tmp / 'nope.bin'),
                          '--expect-image', str(self.old))
        with self.assertRaises(SystemExit):
            self.flash.resolve(args)

    def test_sha_may_be_given_directly_instead_of_a_file(self):
        args = self.parse('--image', str(self.new),
                          '--expect-image', str(self.old),
                          '--expect-sha', 'ab' * 32)
        _, _, expect_sha, _ = self.flash.resolve(args)
        self.assertEqual(expect_sha, 'ab' * 32)

    def test_default_offset_is_ota0(self):
        args = self.parse()
        self.assertEqual(args.offset, 0x10000)


class _FakePort:
    """serial.tools.list_ports.comports() の要素の代わり。"""

    def __init__(self, vid, pid, serial_number, device):
        self.vid = vid
        self.pid = pid
        self.serial_number = serial_number
        self.device = device


class RomDownloadModeTest(unittest.TestCase):
    """ROM ダウンロードモードの入り口は 2 つある (2026-09-16)。

      303a:0009  USB-OTG の ROM   … 1200bps タッチ / QK_BOOT / RST 長押し
      303a:1001  USB-Serial/JTAG  … JTAG 側から入ったとき

    どちらから入ったかで「戻し方」が変わる。USB-Serial/JTAG では DTR/RTS が
    EN と IO0 のストラップに繋がっているので、esptool の hard-reset を使うと
    IO0 が低いまま再リセットされ、**またダウンロードモードで起動する**。
    """

    # 名指しに使う仮のシリアル。実機の値は道具に書き残さない
    # (flash.ROM_SERIAL は既定 None で、環境変数 STACKEE_ROM_SERIAL で入れる)。
    SERIAL = '02:00:00:00:00:01'

    def setUp(self):
        self.flash = _load_flash()
        self._saved_serial = self.flash.ROM_SERIAL
        self.flash.ROM_SERIAL = self.SERIAL

    def tearDown(self):
        self.flash.ROM_SERIAL = self._saved_serial

    def otg(self, device='/dev/cu.otg'):
        return _FakePort(0x303A, 0x0009, self.flash.ROM_SERIAL, device)

    def jtag(self, serial_number=None, device='/dev/cu.jtag'):
        return _FakePort(0x303A, 0x1001, serial_number, device)

    def test_both_pids_are_recognised_as_rom(self):
        self.assertEqual(set(self.flash.ROM_PIDS), {0x0009, 0x1001})

    def test_usb_otg_ends_with_hard_reset(self):
        # USB-OTG には DTR/RTS で引けるストラップが無い。esptool が同じ接続の
        # まま FORCE_DOWNLOAD_BOOT を消して watchdog リセットしてくれる。
        self.assertEqual(self.flash.FINAL_RESET['usb-otg'], 'hard-reset')

    def test_usb_serial_jtag_ends_with_watchdog_reset(self):
        # ★ ここが 2026-09-16 に踏んだところ。hard-reset だとダウンロード
        #   モードに戻る。
        self.assertEqual(self.flash.FINAL_RESET['usb-jtag'], 'watchdog-reset')

    def test_picks_the_otg_rom_by_serial(self):
        found = self.flash.pick_rom([self.otg()])
        self.assertEqual(found.device, '/dev/cu.otg')
        self.assertEqual(self.flash.ROM_PIDS[found.pid], 'usb-otg')

    def test_picks_the_jtag_rom_even_without_a_serial_number(self):
        # USB-Serial/JTAG はシリアルを名乗らないことがある。
        found = self.flash.pick_rom([self.jtag()])
        self.assertEqual(found.device, '/dev/cu.jtag')
        self.assertEqual(self.flash.ROM_PIDS[found.pid], 'usb-jtag')

    def test_our_serial_wins_over_a_nameless_candidate(self):
        # 名無しの誰かと、この実機の ROM が両方見えたら、この実機を選ぶ。
        found = self.flash.pick_rom([self.jtag(), self.otg()])
        self.assertEqual(found.device, '/dev/cu.otg')

    def test_two_nameless_candidates_are_not_guessed(self):
        # どちらがこの実機か分からない。選ばない (= 書き込まない)。
        found = self.flash.pick_rom(
            [self.jtag(device='/dev/cu.a'), self.jtag(device='/dev/cu.b')])
        self.assertIsNone(found)

    def test_two_of_ours_is_an_error_not_a_guess(self):
        with self.assertRaises(RuntimeError):
            self.flash.pick_rom(
                [self.jtag(serial_number=self.flash.ROM_SERIAL), self.otg()])

    def test_ignores_the_running_firmware_port(self):
        # 通常起動中の 303a:811A は ROM ではない。
        running = _FakePort(0x303A, 0x811A, self.flash.ROM_SERIAL, '/dev/cu.run')
        self.assertIsNone(self.flash.pick_rom([running]))

    def test_esptool_is_refused_a_second_time_in_one_session(self):
        # ★ 2026-09-16: 同じ ROM セッションで 2 回目を呼ぶと
        #   "No serial data received" で失敗し、本体が ROM のまま固まった。
        #   呼ぶ前に止める。
        self.flash._esptool_calls = 1
        try:
            with self.assertRaises(RuntimeError) as caught:
                self.flash.esptool('read-flash')
            self.assertIn('2 回', str(caught.exception))
        finally:
            self.flash._esptool_calls = 0

    def test_without_a_named_serial_a_lone_candidate_is_accepted(self):
        # STACKEE_ROM_SERIAL を指定しない既定。候補が 1 つならそれを使う。
        self.flash.ROM_SERIAL = None
        found = self.flash.pick_rom([self.otg()])
        self.assertEqual(found.device, '/dev/cu.otg')

    def test_without_a_named_serial_two_candidates_are_not_guessed(self):
        # 2 台繋がっていたら選ばない。名指ししたいときだけ
        # STACKEE_ROM_SERIAL を入れる。
        self.flash.ROM_SERIAL = None
        self.assertIsNone(self.flash.pick_rom([self.otg(), self.jtag()]))

    def test_unknown_mode_falls_back_to_the_safe_reset(self):
        # 判定できないときはストラップを触らない側へ倒す。
        original = self.flash.rom_device

        def boom():
            raise RuntimeError('見つからない')

        self.flash.rom_device = boom
        try:
            self.assertEqual(self.flash.final_reset(), 'watchdog-reset')
        finally:
            self.flash.rom_device = original


class UsbBusResetTest(unittest.TestCase):
    """ROM が固まったときの最後の手段 (2026-09-16 に 2 回これで復帰した)。

    esptool の watchdog-reset が空振りして 303A:0009 に居座ったまま
    動かなくなることがある。Mac から pyusb で **USB のバスリセット**を
    かけると、そのたびに通常起動へ戻った。`dev.reset()` が
    "Entity not found" を投げても失敗ではない — リセットの直後に
    デバイスが再列挙されてハンドルが無効になっているだけなので、
    **例外を握りつぶして復帰を待つ**のが正しい。
    """

    def setUp(self):
        self.flash = _load_flash()

    def install_fake_usb(self, devices, reset_raises=None):
        calls = []

        class _Dev:
            def __init__(self, pid):
                self.pid = pid

            def reset(self_inner):
                calls.append(self_inner.pid)
                if reset_raises is not None:
                    raise reset_raises

        module = types.ModuleType('usb')
        core = types.ModuleType('usb.core')

        def find(find_all=False, idVendor=None, idProduct=None):
            return [_Dev(idProduct) for _ in range(devices.get(idProduct, 0))]

        core.find = find
        module.core = core
        sys.modules['usb'] = module
        sys.modules['usb.core'] = core
        self.addCleanup(sys.modules.pop, 'usb', None)
        self.addCleanup(sys.modules.pop, 'usb.core', None)
        return calls

    def test_resets_the_rom_device(self):
        calls = self.install_fake_usb({0x0009: 1})
        self.assertTrue(self.flash.usb_bus_reset())
        self.assertEqual(calls, [0x0009])

    def test_entity_not_found_is_not_a_failure(self):
        calls = self.install_fake_usb({0x0009: 1},
                                      reset_raises=RuntimeError('Entity not found'))
        self.assertTrue(self.flash.usb_bus_reset())
        self.assertEqual(calls, [0x0009])

    def test_nothing_to_reset(self):
        self.install_fake_usb({})
        self.assertFalse(self.flash.usb_bus_reset())

    def test_without_pyusb_it_just_says_so(self):
        sys.modules['usb'] = None       # import usb.core が ImportError になる
        self.addCleanup(sys.modules.pop, 'usb', None)
        self.assertFalse(self.flash.usb_bus_reset())

    def test_leave_rom_tries_the_bus_reset_last(self):
        # CDC が戻らないときだけ呼ぶ。戻っていれば触らない。
        source = open(os.path.join(HERE, 'flash.py')).read()
        body = source[source.index('def leave_rom'):source.index('def usb_bus_reset')]
        self.assertIn('usb_bus_reset()', body)
        # 「CDC が戻ったか」の確かめ (app_alive、旧名 find_port) が
        # バスリセットより先に来ていること。
        check = 'app_alive' if 'app_alive' in body else 'find_port'
        self.assertLess(body.index(check), body.index('usb_bus_reset()'))


class NvsDumpTest(unittest.TestCase):
    """nvs のダンプ解析。実機の nvs を読む前に、形式の読み取りを確かめる。

    ページは 4096 バイト、先頭 32 バイトがヘッダ、次の 32 バイトが
    エントリ状態のビットマップ (2 ビット x 126)、残りが 32 バイトの
    エントリ 126 個 (ESP-IDF プログラミングガイド Structure of a page)。
    """

    def setUp(self):
        import nvs_dump
        self.nvs = nvs_dump

    def page(self, entries):
        """(ns_index, type_id, key, value) の並びから 1 ページ作る。"""
        import struct
        header = struct.pack('<II', 0xFFFFFFFE, 1) + b'\xff' * 24
        bitmap = bytearray(b'\xff' * 32)
        body = bytearray(b'\xff' * (self.nvs.ENTRY_SIZE *
                                    self.nvs.ENTRIES_PER_PAGE))
        for index, (ns_index, type_id, key, value) in enumerate(entries):
            # 状態を "written" (0b10) にする。
            byte = index // 4
            shift = (index % 4) * 2
            bitmap[byte] = (bitmap[byte] & ~(0b11 << shift)) | (0b10 << shift)
            raw = bytearray(32)
            raw[0] = ns_index
            raw[1] = type_id
            raw[2] = 1
            raw[3] = 0xFF
            raw[8:8 + len(key)] = key.encode()
            raw[24:32] = struct.pack('<Q', value)
            body[index * 32:(index + 1) * 32] = raw
        return header + bytes(bitmap) + bytes(body)

    def test_namespaces_and_entries(self):
        image = self.page([
            (0, 0x01, 'stackee', 1),
            (0, 0x01, 'nimble_bond', 2),
            (1, 0x01, 'hid_dest', 1),
            (2, 0x01, 'peer_sec_1', 7),
            (2, 0x01, 'our_sec_1', 7),
            (2, 0x01, 'cccd_sec_1', 3),
        ])
        namespaces, entries = self.nvs.parse(image)
        self.assertEqual(namespaces, {1: 'stackee', 2: 'nimble_bond'})
        self.assertEqual(len(entries), 4)
        hid = [e for e in entries if e['key'] == 'hid_dest'][0]
        self.assertEqual(hid['value'], 1)

    def test_nimble_summary_counts_by_kind(self):
        image = self.page([
            (0, 0x01, 'nimble_bond', 1),
            (1, 0x01, 'peer_sec_1', 7),
            (1, 0x01, 'our_sec_1', 7),
            (1, 0x01, 'cccd_sec_1', 3),
            (1, 0x01, 'cccd_sec_2', 3),
            (1, 0x01, 'local_irk', 1),
        ])
        namespaces, entries = self.nvs.parse(image)
        summary = self.nvs.summarize_nimble(namespaces, entries)
        self.assertEqual(summary['counts'],
                         {'peer_sec': 1, 'our_sec': 1, 'cccd_sec': 2,
                          'local_irk': 1})

    def test_missing_nimble_bond_is_reported_not_crashed(self):
        image = self.page([(0, 0x01, 'stackee', 1), (1, 0x01, 'hid_dest', 0)])
        namespaces, entries = self.nvs.parse(image)
        self.assertIsNone(self.nvs.summarize_nimble(namespaces, entries))

    def test_erased_entries_are_skipped(self):
        # 状態ビットマップが "written" のものだけ読む。
        image = bytearray(self.page([(0, 0x01, 'stackee', 1),
                                     (1, 0x01, 'gone', 5)]))
        image[32] &= ~0b1100        # 2 つ目を "erased" (0b00) にする
        namespaces, entries = self.nvs.parse(bytes(image))
        self.assertEqual(namespaces, {1: 'stackee'})
        self.assertEqual(entries, [])


class HidDescriptorTest(unittest.TestCase):
    """記述子が設計 (DESIGN.md §4) どおりかを机の上で確かめる。"""

    @classmethod
    def setUpClass(cls):
        source = HERE.parent / 'main' / 'stackee_usb.c'
        arrays = hid_desc_check.extract_arrays(str(source))
        cls.keys = hid_desc_check.summarize('keys', arrays['s_hid_keys_report'])
        cls.raw = hid_desc_check.summarize('raw', arrays['s_hid_raw_report'])

    def test_keyboard_usage_max_covers_lang1_lang2(self):
        keyboard = [c for c in self.keys['collections'] if c['keyboard']]
        self.assertEqual(len(keyboard), 1)
        self.assertGreaterEqual(keyboard[0]['usage_max'], 0x91)

    def test_three_report_ids_on_one_interface(self):
        ids = [i for c in self.keys['collections'] for i in c['report_ids']]
        self.assertEqual(ids, [1, 2, 3])

    def test_keyboard_report_is_eight_bytes_plus_led(self):
        keyboard = [c for c in self.keys['collections'] if c['keyboard']][0]
        self.assertEqual(keyboard['reports']['input/id1'], 64)
        self.assertEqual(keyboard['reports']['output/id1'], 8)

    def test_raw_hid_has_exactly_one_collection_on_page_ff60(self):
        # ★ 段階 1 の決定 (DESIGN.md §8b): コレクションは 1 つだけ。
        #   2 つあると Report ID が必須になり、Report ID 0 を前提にしている
        #   VIA / Remap との互換性を失う恐れがある。
        pages = [c['usage_page'] for c in self.raw['collections']]
        usages = [c['usage'] for c in self.raw['collections']]
        self.assertEqual(pages, [0xFF60])
        self.assertEqual(usages, [0x61])

    def test_raw_hid_has_no_report_id_and_is_32_bytes(self):
        via = self.raw['collections'][0]
        self.assertEqual(via['report_ids'], [])
        self.assertEqual(via['reports']['input/idNone'], 32 * 8)
        self.assertEqual(via['reports']['output/idNone'], 32 * 8)


class CheckPhase0Test(unittest.TestCase):
    def setUp(self):
        try:
            import check_phase0
        except (Exception, SystemExit) as err:   # pragma: no cover - 環境依存
            # ★ SystemExit も拾う。check_phase0.py は移植元 (firmware/kmk/tools)
            #   が無いと import の途中で sys.exit() する。公開リポジトリだけの
            #   clone では普通に起きることなので、飛ばす。
            raise unittest.SkipTest('check_phase0.py を import できない: %s' % err)
        self.check = check_phase0

    def good_status(self):
        return {
            'fw': 'stackee-idf/0', 'up': 12.3, 'bat': 77,
            'assets': {'mounted': True, 'manifest': True, 'size': 240, 'faces': 32},
            'lcd': {'ready': True, 'transfers': 1, 'rows': 320, 'worker_ms': 41},
            'heap_free': 200000, 'heap_min': 180000, 'psram_free': 8000000,
            'perf': {'main': {'n': 1000, 'max_us': 2100, 'med_us': 1002}},
        }

    def verdict(self, result):
        return {name: ok for name, ok, _ in self.check.verdicts(result)}

    def test_all_good(self):
        result = {'hello': {'fw': 'stackee-idf/0'}, 'status': self.good_status(),
                  'reconnect': {'status_s': 2.4}}
        got = self.verdict(result)
        self.assertTrue(all(v for v in got.values()))

    def test_slow_boot_is_ng(self):
        result = {'hello': {'fw': 'stackee-idf/0'}, 'status': self.good_status(),
                  'reconnect': {'status_s': 4.1}}
        self.assertFalse(self.verdict(result)['起動 → CDC 応答'])

    def test_unreadable_manifest_is_ng(self):
        status = self.good_status()
        status['assets'] = {'mounted': True, 'manifest': False, 'size': -1,
                            'faces': -1, 'error': 'manifest.json が開けない'}
        result = {'hello': {'fw': 'stackee-idf/0'}, 'status': status}
        self.assertFalse(self.verdict(result)['user_fs の目録'])

    def test_circuitpython_still_running_is_ng(self):
        status = self.good_status()
        status['fw'] = 'stackee-console/2'
        result = {'hello': {'fw': 'stackee-console/2'}, 'status': status}
        self.assertFalse(self.verdict(result)['ファーム'])

    def test_without_wait_the_boot_time_is_unknown_not_failed(self):
        result = {'hello': {'fw': 'stackee-idf/0'}, 'status': self.good_status()}
        self.assertIsNone(self.verdict(result)['起動 → CDC 応答'])



# ---------------------------------------------------------------------------
# 段階 4
# ---------------------------------------------------------------------------
class JpegInfoTest(unittest.TestCase):
    """check_phase4.py の JPEG 検算。**ここが甘いと「撮れた」を誤報する。**"""

    @classmethod
    def setUpClass(cls):
        import check_phase4
        cls.mod = check_phase4

    def make(self, w, h, marker=0xC0, soi=True, eoi=True):
        out = bytearray()
        if soi:
            out += b'\xff\xd8'
        # APP0 (JFIF) を 1 つ挟む。実物と同じ並びにする。
        out += b'\xff\xe0' + (16).to_bytes(2, 'big') + b'JFIF\x00' + b'\x00' * 9
        out += bytes([0xFF, marker]) + (17).to_bytes(2, 'big') + b'\x08'
        out += h.to_bytes(2, 'big') + w.to_bytes(2, 'big') + b'\x03' + b'\x00' * 9
        out += b'\xff\xda' + (12).to_bytes(2, 'big') + b'\x00' * 10
        out += b'body'
        if eoi:
            out += b'\xff\xd9'
        return bytes(out)

    def test_good_qvga(self):
        got = self.mod.jpeg_info(self.make(320, 240))
        self.assertTrue(got['ok'], got)
        self.assertEqual((320, 240), (got['w'], got['h']))

    def test_progressive_sof2(self):
        got = self.mod.jpeg_info(self.make(160, 120, marker=0xC2))
        self.assertTrue(got['ok'])
        self.assertEqual((160, 120), (got['w'], got['h']))

    def test_missing_soi(self):
        self.assertFalse(self.mod.jpeg_info(self.make(320, 240, soi=False))['ok'])

    def test_missing_eoi(self):
        got = self.mod.jpeg_info(self.make(320, 240, eoi=False))
        self.assertFalse(got['ok'])
        self.assertEqual('EOI なし', got['why'])

    def test_truncated(self):
        self.assertFalse(self.mod.jpeg_info(b'\xff\xd8')['ok'])
        self.assertFalse(self.mod.jpeg_info(b'')['ok'])

    def test_not_a_jpeg(self):
        self.assertFalse(self.mod.jpeg_info(b'\x89PNG\r\n\x1a\n')['ok'])


class CheckPhase4Test(unittest.TestCase):
    """合否の付け方。**ここが甘いと壊れているのに OK と言う。**"""

    @classmethod
    def setUpClass(cls):
        import check_phase4
        cls.mod = check_phase4

    def args(self, **kw):
        base = {'json': False, 'record': False, 'no_camera': False,
                'warmup': 30, 'only': None}
        base.update(kw)
        return types.SimpleNamespace(**base)

    def good(self):
        return {
            'transport': 'serial', 'port': '/dev/cu.x',
            'hello': {'fw': 'stackee-idf/4', 'proto': 2, 'profile': 'dev',
                      'features': list(self.mod.LEGACY_FEATURES)},
            'touch_before': {'present': True, 'vendor': 0x11, 'reads': 100,
                             'read_fails': 0},
            'touch_drag': {'moves': 9, 'moved_x': -30, 'moved_y': -20,
                           'us': 300},
            'touch_tap': {'clicks': 1},
            'touch_after': {'reads': 140},
            'camera_capture': {'jpeg_bytes': 9000, 'camera_ms': 4500,
                               'camera_w': 320, 'camera_h': 240,
                               'camera_bytes': 153600, 'camera_warmup': 30,
                               'psram_dma': True, 'dma_largest': 4096,
                               'internal_free': 48000, 't': {}},
            'camera_jpeg_len': 9000,
            'camera_jpeg': {'ok': True, 'why': '', 'w': 320, 'h': 240},
            'camera_power': {'aldo3': 0},
            'settings_get': {'keys': {'STACKEE_HOST': 'a',
                                      'STACKEE_WIFI_PASSWORD': True},
                             'secret': ['STACKEE_WIFI_PASSWORD']},
            'settings_raw': {'bytes': 100,
                             'text': 'STACKEE_WIFI_PASSWORD = "***"'},
            'bench': {'n': 100, 'in_waiting_ns': 300, 'idle_poll_ns': 400,
                      'main_med_us': 1000},
            'log_burst': {'n': 20, 'us': 900, 'burst_bytes': 1200,
                          'burst_drops': 0},
            'lcd_status': {'ready': True, 'width': 240, 'height': 320,
                           'transfers': 5, 'rows_sent': 320},
            'lcd_full': {'ok': 1},
            'usb_status': {'profile': 'dev', 'cdc': True,
                           'conhid': {'proto': 1, 'tx_pending': 0,
                                      'tx_dropped': 0, 'tx_overrun': 0,
                                      'polls': 0}},
            'loop_stats': {'error': 'unsupported', 'note': 'perf を見ること'},
            'status_after': {'keys': {'tca': True, 'events': 5, 'iofail': 0},
                             'perf': {'input': {'med_us': 900, 'max_us': 1600,
                                                'n': 100}}},
        }

    def names(self, result):
        return dict((n, ok) for n, ok, _d in
                    self.mod.verdicts(result, self.args()))

    def test_all_good(self):
        got = self.names(self.good())
        self.assertNotIn(False, got.values(), got)

    def test_old_firmware_fails(self):
        result = self.good()
        result['hello']['fw'] = 'stackee-idf/3'
        self.assertFalse(self.names(result)['ファーム'])

    def test_missing_touch_fails(self):
        result = self.good()
        result['touch_before']['present'] = False
        self.assertFalse(self.names(result)['タッチ FT6336'])

    def test_touch_without_reports_fails(self):
        result = self.good()
        result['touch_drag'] = {'moves': 0, 'moved_x': 0, 'moved_y': 0}
        self.assertFalse(self.names(result)['タッチ なぞり → マウス'])

    def test_camera_power_left_on_fails(self):
        result = self.good()
        result['camera_power'] = {'aldo3': 1}
        self.assertFalse(self.names(result)['カメラ 撮影後の ALDO3'])

    def test_psram_dma_covers_a_tiny_free_block(self):
        """PSRAM DMA で開けていれば、内蔵の塊が小さくても合格。"""
        result = self.good()
        result['camera_capture']['dma_largest'] = 4096
        self.assertTrue(self.names(result)['カメラ 撮影前の内蔵 RAM'])

    def test_internal_dma_with_a_tiny_free_block_fails(self):
        """★ 実機で落ちた形。内蔵 DMA なのに塊が 4 KB しかない。"""
        result = self.good()
        result['camera_capture']['psram_dma'] = False
        result['camera_capture']['dma_largest'] = 4096
        self.assertFalse(self.names(result)['カメラ 撮影前の内蔵 RAM'])

    def test_internal_dma_with_enough_room_passes(self):
        result = self.good()
        result['camera_capture']['psram_dma'] = False
        result['camera_capture']['dma_largest'] = 12000
        self.assertTrue(self.names(result)['カメラ 撮影前の内蔵 RAM'])

    def test_raw_hid_backlog_fails(self):
        """★ 実機で踏んだ形。ログが溜まって応答が入らなくなる手前。"""
        result = self.good()
        result['usb_status']['conhid']['tx_pending'] = 6143
        self.assertFalse(
            self.names(result)['Raw HID の送信待ちが溜まっていない'])

    def test_jpeg_short_fails(self):
        result = self.good()
        result['camera_jpeg_len'] = 4000
        self.assertFalse(self.names(result)['カメラ 取り出した JPEG'])

    def test_missing_feature_fails(self):
        result = self.good()
        result['hello']['features'] = [f for f in self.mod.LEGACY_FEATURES
                                       if f != 'lcd.full']
        self.assertFalse(self.names(result)['コンソール 現行の FEATURES'])

    def test_secret_leak_fails(self):
        result = self.good()
        result['settings_get']['keys']['STACKEE_WIFI_PASSWORD'] = 'himitsu'
        self.assertFalse(self.names(result)['settings.get は値を伏せる'])

    def test_loop_answering_normally_fails(self):
        result = self.good()
        result['loop_stats'] = {'ok': 1}
        self.assertFalse(self.names(result)['コンソール loop.* は未対応と答える'])

    def test_full_profile_without_mac_device_fails(self):
        result = self.good()
        result['usb_status']['profile'] = 'full'
        result['usb_status']['cdc'] = False
        result['usb_status']['uac'] = {'streaming': False, 'opens': 0,
                                       'frames': 0, 'silence': 0,
                                       'underruns': 0}
        result['uac_device'] = None
        self.assertFalse(self.names(result)['UAC マイクを Mac が見ている'])

    def test_full_profile_with_mac_device_passes(self):
        result = self.good()
        result['usb_status']['profile'] = 'full'
        result['usb_status']['cdc'] = False
        result['usb_status']['uac'] = {'streaming': True, 'opens': 1,
                                       'frames': 16000, 'silence': 0,
                                       'underruns': 0}
        result['uac_device'] = 'M5Stack Core S3'
        got = self.names(result)
        self.assertTrue(got['UAC マイクを Mac が見ている'])
        self.assertIsNone(got['UAC 1 秒録音'])

    def test_dead_keyboard_fails(self):
        result = self.good()
        result['status_after']['keys']['tca'] = False
        self.assertFalse(self.names(result)['キーボードが生きている'])


class ConsoleHidFramingTest(unittest.TestCase):
    """console_hid.py の枠づくり (実機なしで見られる部分)。"""

    @classmethod
    def setUpClass(cls):
        import console_hid
        cls.ch = console_hid

    def test_split_and_join(self):
        data = bytes(range(0, 200))
        reports = self.ch.encode_tx_reports(data)
        self.assertEqual(7, len(reports))
        out = bytearray()
        for rep in reports:
            self.assertEqual(32, len(rep))
            self.assertEqual(self.ch.CMD_TX, rep[0])
            out += rep[3:3 + rep[1]]
        self.assertEqual(data, bytes(out))

    def test_empty(self):
        self.assertEqual([], self.ch.encode_tx_reports(b''))

    def test_exact_multiple(self):
        reports = self.ch.encode_tx_reports(b'x' * 58)
        self.assertEqual(2, len(reports))
        self.assertEqual(29, reports[0][1])
        self.assertEqual(29, reports[1][1])

    def test_decode_rejects_long_len(self):
        rep = bytearray(32)
        rep[0] = self.ch.CMD_RX
        rep[1] = 30
        with self.assertRaises(ValueError):
            self.ch.decode_rx(bytes(rep))

    def test_decode_more_flag(self):
        rep = bytearray(32)
        rep[0] = self.ch.CMD_RX
        rep[1] = 2
        rep[2] = self.ch.FLAG_MORE
        rep[3:5] = b'hi'
        rid, payload, more = self.ch.decode_rx(bytes(rep))
        self.assertEqual((self.ch.CMD_RX, b'hi', True), (rid, payload, more))

    def test_info(self):
        rep = bytearray(32)
        rep[0] = self.ch.CMD_INFO
        rep[1] = 1
        rep[3:7] = (1234).to_bytes(4, 'little')
        rep[7:11] = (7).to_bytes(4, 'little')
        self.assertEqual({'proto': 1, 'pending': 1234, 'dropped': 7},
                         self.ch.parse_info(bytes(rep)))

    def test_info_rejects_other_ids(self):
        rep = bytearray(32)
        rep[0] = self.ch.CMD_RX
        with self.assertRaises(ValueError):
            self.ch.parse_info(bytes(rep))

if __name__ == '__main__':
    unittest.main(verbosity=2)
