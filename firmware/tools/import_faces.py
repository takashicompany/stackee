"""元絵 (800x800 の PNG 32 枚) から faces.bin / changes.bin / manifest.json を作る。

**実機に触らない。音も鳴らさない。** 一次回答の音声 (manifest の `acks`) は
いま出力先にある manifest から読み直して持ち越すだけで、作り直さない。

  python3 firmware/tools/import_faces.py            # 既定の元絵から作り直す
  python3 firmware/tools/import_faces.py --check    # 書かずに一致だけ見る
  python3 firmware/tools/import_faces.py <別の元絵> --output <別の置き場>

元絵は `firmware/assets/src/faces/<状態>/*.png`。作者はユーザー本人
(takashicompany)。Stack-chan の v2 の顔として作ったもので、ライセンスは
未指定 (くわしくは firmware/assets/README.md)。

★ 同じ元絵からは毎回同じバイト列が出る。`--check` はそれを確かめる。
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path
import zlib
from PIL import Image, ImageChops

SIZE = 240
CASES = ('awake', 'idle', 'listening', 'thinking', 'speaking', 'camera')

FW = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = FW / 'assets/src/faces'
DEFAULT_OUTPUT = FW / 'assets'

OUTPUTS = ('faces.bin', 'changes.bin', 'manifest.json')


def case_dir(source, case):
    """元絵の置き場。stack-chan のリポジトリをそのまま渡してもよい。"""
    nested = source / 'assets/v2' / case
    return nested if nested.is_dir() else source / case


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', type=Path, nargs='?', default=DEFAULT_SOURCE,
                        help='元絵の置き場 (既定: %s)'
                             % DEFAULT_SOURCE.relative_to(FW.parent))
    parser.add_argument('--output', type=Path, default=DEFAULT_OUTPUT,
                        help='出力先 (既定: %s)'
                             % DEFAULT_OUTPUT.relative_to(FW.parent))
    parser.add_argument('--check', action='store_true',
                        help='書かずに、いまある生成物と同じになるかだけ見る')
    args = parser.parse_args()
    real_output = args.output
    if args.check:
        import tempfile
        args.output = Path(tempfile.mkdtemp())
        for name in OUTPUTS:
            if (real_output / name).is_file():
                (args.output / name).write_bytes((real_output / name).read_bytes())
    args.output.mkdir(parents=True, exist_ok=True)
    previous = args.output / 'manifest.json'
    acks = json.loads(previous.read_text()).get('acks', []) if previous.exists() else []
    manifest = {'v': 1, 'size': SIZE, 'cases': {}, 'faces': [], 'acks': acks}
    sheet = bytearray()
    frames = []
    for case in CASES:
        groups = {}
        paths = sorted(case_dir(args.source, case).glob('*.png'))
        # Keep one scale per state so mouth/eye animation never changes size.
        # Fit props (camera, books, sleep marks) as well as the central face.
        scale = None
        for path in paths:
            with Image.open(path) as source:
                source = source.convert('RGBA')
                white = Image.new('RGBA', source.size, 'white')
                white.alpha_composite(source)
                bounds = white.convert('L').point(lambda p: 255 if p < 240 else 0).getbbox()
                w, h = source.size
                extent = max(w/2-bounds[0], bounds[2]-w/2, h/2-bounds[1], bounds[3]-h/2)
                fit = min(1.5 * SIZE / max(w, h), (SIZE-8) / (2*extent))
                scale = fit if scale is None else min(scale, fit)
        for path in paths:
            with Image.open(path) as im:
                im = im.convert('RGBA')
                white = Image.new('RGBA', im.size, 'white')
                white.alpha_composite(im)
                # Same centered 1.5x face scale as KawaiiRenderer, adapted to a
                # square panel that leaves the Stackee status labels visible.
                resized = white.convert('L').resize((round(im.width*scale), round(im.height*scale)), Image.Resampling.LANCZOS)
                im = Image.new('L', (SIZE, SIZE), 255)
                im.paste(resized, ((SIZE-resized.width)//2, (SIZE-resized.height)//2))
                pixels = bytes((p * 15 + 127) // 255 for p in im.tobytes())
            frame = len(manifest['faces'])
            frames.append(Image.frombytes('L', (SIZE, SIZE), pixels))
            manifest['faces'].append({'name': case + '/' + path.stem,
                                      'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
            groups.setdefault(path.stem.split('_')[0], []).append(frame)
            sheet.extend((pixels[i] << 4) | pixels[i+1] for i in range(0, len(pixels), 2))
        if not groups:
            raise ValueError('Missing face case: ' + case)
        manifest['cases'][case] = list(groups.values())
    (args.output / 'faces.bin').write_bytes(zlib.compress(sheet, 9))
    changes = bytearray()
    for before in frames:
        for after in frames:
            bounds = ImageChops.difference(before, after).getbbox()
            changes.extend(bounds or (0, 0, 0, 0))
    (args.output / 'changes.bin').write_bytes(zlib.compress(changes, 9))
    (args.output / 'manifest.json').write_text(json.dumps(manifest, separators=(',', ':')) + '\n')
    print('顔 %d 枚 / 一次回答 %d 本 (音声は作り直していない)'
          % (len(manifest['faces']), len(manifest['acks'])))
    if not args.check:
        print('できたバイト数:',
              sum(p.stat().st_size for p in args.output.iterdir() if p.is_file()))
        return 0
    bad = []
    for name in OUTPUTS:
        made = (args.output / name).read_bytes()
        have = (real_output / name)
        if not have.is_file() or have.read_bytes() != made:
            bad.append(name)
        else:
            print('  一致: %s (%d バイト)' % (name, len(made)))
    if bad:
        print('!! 生成物が元絵と合わない: %s' % ', '.join(bad), file=sys.stderr)
        return 1
    print('生成物は最新')
    return 0


if __name__ == '__main__':
    sys.exit(main() or 0)
