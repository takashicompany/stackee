#!/usr/bin/env python3
"""ROM 突入 1 回・esptool 呼び出し 1 回で ota_0 に像を書き、同じ接続内で戻す。

  python3 firmware/tools/write_image.py <sha256> [像のパス (既定 build/stackee.bin)]

sha256 が合わない像は書かない (取り違え防止)。dev は 1200 bps タッチ、
full は Raw HID の bootloader で ROM に入る (flash.py の enter_rom)。
"""
import sys, hashlib, os
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash

if len(sys.argv) < 2:
    sys.exit(__doc__)
want = sys.argv[1]
new = (Path(sys.argv[2]).resolve() if len(sys.argv) > 2
       else flash.DEFAULT_IMAGE)
got = hashlib.sha256(new.read_bytes()).hexdigest()
if got != want:
    sys.exit('sha256 が合わない: %s\n  期待 %s\n  実物 %s' % (new, want, got))
flash.enter_rom()
flash.esptool('write-flash', '--flash-mode', 'keep', '--flash-freq', 'keep',
              '--flash-size', 'keep', hex(flash.OTA0), str(new), after='final')
sys.exit(0 if flash.leave_rom() else 3)
