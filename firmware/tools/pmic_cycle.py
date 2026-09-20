#!/usr/bin/env python3
"""ROM ダウンロードモード (303A:1001 / 0009) から AXP2101 に電源再投入を命じる。

用途: カメラを開いたあとの再起動などでブートループ ("invalid header: 0xffffff1f"
が延々流れ、フラッシュが読めない) に落ちたとき、ボタンを押さずに CoreS3 全体の
電源を切って入れ直す。ESP32-S3 の ROM (esptool スタブ) に GPIO レジスタを
叩かせて I2C をビットバンギングし、AXP2101 REG 0x10 bit1 (Restart) を書く。

  python3 firmware/tools/pmic_cycle.py [/dev/cu.usbmodemXXXX] [--check]

--check は読むだけ (REG 0x03 / 0x10 / 0x90) で電源は切らない。
bit0 (Soft PWROFF) は絶対に書かない (復帰にボタンが要る)。
"""
import sys, time, glob
from esptool.targets.esp32s3 import ESP32S3ROM

GPIO = 0x60004000
OUT_W1TS, OUT_W1TC = GPIO + 0x08, GPIO + 0x0C
ENA_W1TS, ENA_W1TC = GPIO + 0x24, GPIO + 0x28
IN_REG = GPIO + 0x3C
IOMUX = 0x60009000
SCL, SDA = 11, 12            # CoreS3 内部 I2C
AXP = 0x34


class BitBang:
    def __init__(self, esp):
        self.esp = esp
        for pin in (SCL, SDA):
            # MCU_SEL=1 (GPIO), FUN_IE, FUN_WPU, DRV=1
            esp.write_reg(IOMUX + 4 + 4 * pin, 0x1700)
            esp.write_reg(GPIO + 0x554 + 4 * pin, 0x100)   # 出力信号 = GPIO_OUT
            esp.write_reg(OUT_W1TC, 1 << pin)               # 出力値は常に 0
            esp.write_reg(ENA_W1TC, 1 << pin)               # 開放 (プルアップで High)

    def low(self, pin):  self.esp.write_reg(ENA_W1TS, 1 << pin)
    def rel(self, pin):  self.esp.write_reg(ENA_W1TC, 1 << pin)
    def read(self, pin): return (self.esp.read_reg(IN_REG) >> pin) & 1

    def start(self):
        self.rel(SDA); self.rel(SCL); self.low(SDA); self.low(SCL)
    def stop(self):
        self.low(SDA); self.rel(SCL); self.rel(SDA)
    def wbit(self, b):
        (self.rel if b else self.low)(SDA); self.rel(SCL); self.low(SCL)
    def rbit(self):
        self.rel(SDA); self.rel(SCL); b = self.read(SDA); self.low(SCL); return b
    def wbyte(self, v):
        for i in range(7, -1, -1): self.wbit((v >> i) & 1)
        return self.rbit() == 0        # ACK
    def rbyte(self, ack):
        v = 0
        for _ in range(8): v = (v << 1) | self.rbit()
        self.wbit(0 if ack else 1)
        return v

    def read_reg8(self, dev, reg):
        self.start()
        a1 = self.wbyte(dev << 1); a2 = self.wbyte(reg)
        self.start()
        a3 = self.wbyte((dev << 1) | 1)
        v = self.rbyte(False); self.stop()
        if not (a1 and a2 and a3):
            raise RuntimeError('I2C NACK (%d%d%d) dev=0x%02x reg=0x%02x' % (a1, a2, a3, dev, reg))
        return v

    def write_reg8(self, dev, reg, val):
        self.start()
        a1 = self.wbyte(dev << 1); a2 = self.wbyte(reg); a3 = self.wbyte(val)
        self.stop()
        return a1 and a2 and a3


def enter_download_via_jtag(port, timeout=20.0):
    """ブートループ中の USB-Serial/JTAG (303A:1001) に DTR/RTS のリセットを
    撃ってダウンロードモードで止める。ROM は 1〜2 秒ごとに再列挙される
    ので、ポートが開けた瞬間に撃つ。止まれば "waiting for download" が出る。
    """
    import serial
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = serial.Serial(port, 115200, timeout=0.2)
            # esptool の UsbJtagSerialReset と同じ順
            s.rts = False; s.dtr = False; time.sleep(0.1)
            s.dtr = True;  s.rts = False; time.sleep(0.1)
            s.rts = True;  s.dtr = False; time.sleep(0.1)
            s.rts = True;  s.dtr = True;  time.sleep(0.1)
            s.rts = False; s.dtr = False
            time.sleep(0.5)
            buf = s.read(4096)
            s.close()
            if b'waiting for download' in buf:
                print('ROM をダウンロードモードで止めた (%.1fs)' % (time.time() - t0),
                      flush=True)
                return True
            time.sleep(0.3)
        except Exception:
            time.sleep(0.3)
    return False


def read_pmic(port):
    esp = ESP32S3ROM(port)
    esp.connect('no-reset', attempts=5)
    esp = esp.run_stub()
    bb = BitBang(esp)
    cid = bb.read_reg8(AXP, 0x03)
    r10 = bb.read_reg8(AXP, 0x10)
    r90 = bb.read_reg8(AXP, 0x90)
    print('AXP2101 REG03=0x%02X (0x4A 期待) REG10=0x%02X REG90=0x%02X' % (cid, r10, r90), flush=True)
    if cid != 0x4A:
        raise RuntimeError('チップ ID が合わない。I2C が通っていない')
    return esp, bb, r10


def power_cycle(port, wait=60.0):
    """ダウンロードモードで止まっている ROM 経由で AXP2101 に Restart を書く。
    書いた瞬間に本体の電源が落ちるので応答は無い。ポートの消滅を見届けて返る。
    """
    esp, bb, r10 = read_pmic(port)
    if r10 & 0x01:
        raise RuntimeError('REG0x10 bit0 (PWROFF) が立っている。中止')
    print('REG 0x10 bit1 (Restart) を書く。応答は返らないのが正常', flush=True)
    try:
        bb.write_reg8(AXP, 0x10, (r10 & ~0x01) | 0x02)
    except Exception as err:
        print('書き込み直後の例外 (想定内): %s' % err, flush=True)
    try:
        esp._port.close()
    except Exception:
        pass
    t0 = time.time()
    while time.time() - t0 < wait:
        if port not in glob.glob('/dev/cu.usbmodem*'):
            print('★ 電源が落ちた (%.1fs)。再起動を待つ' % (time.time() - t0), flush=True)
            return True
        time.sleep(0.2)
    print('ポートが消えない = Restart が効いていない', flush=True)
    return False


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    check = '--check' in sys.argv
    port = args[0] if args else None
    if not port:
        ports = sorted(glob.glob('/dev/cu.usbmodem*'))
        if len(ports) != 1:
            sys.exit('ポートを指定して (候補: %s)' % ports)
        port = ports[0]
    if check:
        read_pmic(port)
        return
    if not power_cycle(port):
        sys.exit(1)


if __name__ == '__main__':
    main()
