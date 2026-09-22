#!/usr/bin/env python3
"""案11の素材を検査。実機・移植元KMK・ESP-IDFは不要。"""
import json
from pathlib import Path
import unittest
import zlib

ASSETS = Path(__file__).resolve().parents[1] / 'assets'
SIZE = 240
FRAME_BYTES = SIZE * SIZE // 2


class MicrophoneAssetsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ASSETS / 'manifest.json').read_text())
        cls.sheet = zlib.decompress((ASSETS / 'faces.bin').read_bytes())
        cls.changes = zlib.decompress((ASSETS / 'changes.bin').read_bytes())
        cls.frames = []
        for f in range(35):
            packed = cls.sheet[f * FRAME_BYTES:(f + 1) * FRAME_BYTES]
            cls.frames.append(bytes(v for b in packed for v in (b >> 4, b & 15)))

    def test_pc_mic_frames_are_separate_from_ai_listening(self):
        self.assertEqual(self.manifest['cases']['listening'], [[10, 11, 12]])
        self.assertEqual(self.manifest['cases']['microphone'], [[32, 33, 34]])
        self.assertEqual(len(self.sheet), 35 * FRAME_BYTES)
        self.assertEqual(len(self.changes), 35 * 35 * 4)
        self.assertEqual(len(set(self.frames[32:])), 3)

    def test_face_and_gripping_hand_do_not_move(self):
        # マイクのヘッドと3本線の領域だけが変わる。
        # 左側の顔、下側の腕と握りを含む画素は完全一致する。
        for target in self.frames[33:]:
            different = [(i % SIZE, i // SIZE)
                         for i, (a, b) in enumerate(zip(self.frames[32], target))
                         if a != b]
            self.assertTrue(different)
            self.assertTrue(all(166 <= x < 236 and 71 <= y < 141
                                for x, y in different))

    def test_all_three_frames_fit_between_status_and_subtitles(self):
        for frame in self.frames[32:]:
            self.assertTrue(all(v == 15 for v in frame[:29 * SIZE]))
            self.assertTrue(all(v == 15 for v in frame[229 * SIZE:]))

    def test_diff_boxes_reconstruct_every_mic_entry_and_exit(self):
        # 各表情からマイクへの遷移、各マイクから全表情への遷移。
        pairs = {(a, b) for a in range(35) for b in range(32, 35)}
        pairs |= {(b, a) for a, b in pairs.copy()}
        for before, after in pairs:
            offset = (before * 35 + after) * 4
            x, y, right, bottom = self.changes[offset:offset + 4]
            reconstructed = bytearray(self.frames[before])
            for row in range(y, bottom):
                start, end = row * SIZE + x, row * SIZE + right
                reconstructed[start:end] = self.frames[after][start:end]
            self.assertEqual(reconstructed, self.frames[after], (before, after))


if __name__ == '__main__':
    unittest.main(verbosity=2)
