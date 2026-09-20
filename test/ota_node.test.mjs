// firmware/tools/ota.mjs の転送層 (NodeHidLink) を偽のデバイスで回す。
// **実機にも node-hid にも触らない。**
//
// ota.test.mjs が見ているのは中核 (docs/js/ota.js) だけで、運び方は偽物だった。
// こちらは **AI が実機へ書き込むときに実際に通る道** をそのまま動かす:
//
//   runOta → NodeHidLink.request()      → 0xC0 に 29 バイトずつ詰めて書く
//          → NodeHidLink._poll()        → 0xC1 で取り出して Demux → RequestTracker
//          → NodeHidLink.sendOtaReport()→ 0xC3 を直列に流す
//          → NodeHidLink.onOtaStatus()  → 0xC3 の応答を credit に回す
//
// 偽のデバイスは firmware/main/stackee_conhid.c + stackee_otacore.c と同じ
// 約束でふるまう (32 バイト固定・本文 29 / 27・位置が合わない枠は捨てる・
// 受け取った累積が 1 KB を跨ぐたびに 1 枚だけ返す)。
//
// ★ node-hid は要らない。ota.mjs は openDevice() の中で初めて import する
//   ので、転送層のクラスだけなら素の Node で読める。
//
//   node --test 'test/**/*.mjs'
import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { EventEmitter } from 'node:events';

import { NodeHidLink } from '../firmware/tools/ota.mjs';
import { OTA, embeddedSha, sha256Hex, runOta } from '../docs/js/ota.js';

const REPORT_SIZE = 32;
const CMD_TX = 0xc0;
const CMD_RX = 0xc1;

/** 末尾 32 バイトに本物の SHA-256 を付けた、作り物の像。 */
function makeImage(size) {
  const img = new Uint8Array(size);
  img[0] = 0xe9;
  for (let i = 1; i < size; i += 1) img[i] = (i * 37 + (i >> 9)) & 0xff;
  img.set(createHash('sha256').update(img.subarray(0, size - 32)).digest(), size - 32);
  return img;
}

/**
 * node-hid の HID と同じ口 (on / write / close) を持つ偽のデバイス。
 * 中身は stackee_conhid.c + stackee_otacore.c の約束どおり。
 */
class FakeHid extends EventEmitter {
  constructor() {
    super();
    this.closed = false;
    this.writes = 0;
    /** ホスト → デバイスの JSON 行を組み立てる (0x1E … \n) */
    this._line = '';
    this._inFrame = false;
    /** デバイス → ホストの送信待ち (conhid の環状バッファに当たる) */
    this._tx = [];
    // --- OTA の状態 (stackee_otacore.c と同じ) ---
    this.ota = {
      state: 0, err: 0, size: 0, accepted: 0, written: 0, ackedAt: 0,
      received: [], wantSha: null, begins: 0, aborts: 0, rejects: 0,
    };
    this.running = { label: 'ota_0', version: 'before', sha256: 'ee'.repeat(32) };
    this.next = { label: 'ota_1', version: null, sha256: null };
    /** 書き込みが受け取りに遅れる量 (実機のフラッシュ書き込みと同じ向き) */
    this.writeLag = 8192;
  }

  // ★ node-hid の HID は EventEmitter。**聞き手のいない 'error' は
  //   プロセスごと落とす**ので、そこまで真似る。
  close() { this.closed = true; }

  _emit(report) {
    // 実機と同じで、返事は次のターンに来る。
    setImmediate(() => {
      if (this.closed) return;
      this.emit('data', Buffer.from(report));
    });
  }

  write(buf) {
    this.writes += 1;
    // ★ hidapi の約束: 先頭に Report ID の 0 を足した 33 バイト。
    assert.equal(buf.length, REPORT_SIZE + 1, 'レポートは Report ID + 32 バイト');
    assert.equal(buf[0], 0x00, '先頭は Report ID の 0');
    const rep = new Uint8Array(buf.subarray(1));
    if (rep[0] === CMD_TX) return this._onTx(rep);
    if (rep[0] === CMD_RX) return this._onRx(rep);
    if (rep[0] === OTA.CMD_DATA) return this._onOta(rep);
    return buf.length;
  }

  // --- 0xC0: ホストからの本文 -----------------------------------------------
  _onTx(rep) {
    const len = rep[1];
    assert.ok(len <= 29, '0xC0 の本文は 29 バイトまで');
    for (let i = 0; i < len; i += 1) {
      const ch = String.fromCharCode(rep[3 + i]);
      if (ch === '\x1e') { this._inFrame = true; this._line = ''; continue; }
      if (!this._inFrame) continue;
      if (ch === '\n') { this._inFrame = false; this._handleLine(this._line); continue; }
      this._line += ch;
    }
    const ack = new Uint8Array(REPORT_SIZE);
    ack[0] = CMD_TX; ack[1] = len;
    this._emit(ack);
    return REPORT_SIZE + 1;
  }

  // --- 0xC1: 取り出し -------------------------------------------------------
  _onRx() {
    const rep = new Uint8Array(REPORT_SIZE);
    rep[0] = CMD_RX;
    let n = 0;
    while (n < 29 && this._tx.length > 0) { rep[3 + n] = this._tx.shift(); n += 1; }
    rep[1] = n;
    rep[2] = this._tx.length > 0 ? 0x01 : 0x00;
    this._emit(rep);
    return REPORT_SIZE + 1;
  }

  _reply(obj) {
    for (const b of Buffer.from('\x1e' + JSON.stringify(obj) + '\n', 'utf8')) {
      this._tx.push(b);
    }
  }

  _handleLine(line) {
    const req = JSON.parse(line);
    const id = req.id;
    const o = this.ota;
    switch (req.cmd) {
      case 'app.info':
        return this._reply({
          id, ok: 1, proto: 1, running: this.running,
          boot: this.running, next: this.next,
          ota_state: 'valid', state: ['idle', 'receiving', 'done', 'failed'][o.state],
          err: 'none', accepted: o.accepted, written: o.written, size: o.size,
        });
      case 'ota.begin':
        if (o.state === 1) return this._reply({ id, error: 'busy' });
        o.begins += 1;
        Object.assign(o, {
          state: 1, err: 0, size: req.size, accepted: 0, written: 0, ackedAt: 0,
          received: [], wantSha: req.sha256,
        });
        return this._reply({
          id, ok: 1, target: 'ota_1', size: req.size, chunk: OTA.MAX_PAYLOAD,
          credit: OTA.CREDIT, ring: OTA.RING, cmd: OTA.CMD_DATA, idle_ms: OTA.IDLE_MS,
        });
      case 'ota.end': {
        const body = Uint8Array.from(o.received);
        const got = createHash('sha256').update(body).digest('hex');
        if (o.received.length !== o.size) {
          o.state = 3; o.err = 10;
          return this._reply({ id, ok: 0, state: 'failed', err: 'size',
                               written: o.received.length, size: o.size, sha256: null });
        }
        o.state = 2;
        this.next = { label: 'ota_1', version: 'after',
                      sha256: embeddedSha(body) };
        return this._reply({
          id, ok: 1, state: 'done', err: 'none', written: o.size, size: o.size,
          sha256: got, want_sha256: o.wantSha, partition: 'ota_1',
          partition_sha256: this.next.sha256, valid: 1, committed: 0,
        });
      }
      case 'ota.commit':
        if (o.state !== 2) return this._reply({ id, error: 'notready' });
        return this._reply({ id, ok: 1, boot: 'ota_1', in_ms: 500 });
      case 'ota.abort':
        o.aborts += 1;
        Object.assign(o, { state: 0, err: 0, accepted: 0, written: 0, size: 0 });
        return this._reply({ id, ok: 1, state: 'idle' });
      case 'ota.status':
        return this._reply({ id, ok: 1,
          state: ['idle', 'receiving', 'done', 'failed'][o.state], err: 'none',
          size: o.size, accepted: o.accepted, written: o.written });
      default:
        return this._reply({ id, error: 'unsupported' });
    }
  }

  // --- 0xC3: 像 -------------------------------------------------------------
  _otaReply(rejected) {
    const o = this.ota;
    const a = new Uint8Array(REPORT_SIZE);
    a[0] = OTA.CMD_DATA; a[1] = o.state; a[2] = o.err;
    const put = (i, v) => {
      a[i] = v & 0xff; a[i + 1] = (v >> 8) & 0xff;
      a[i + 2] = (v >> 16) & 0xff; a[i + 3] = (v >>> 24) & 0xff;
    };
    put(3, o.accepted); put(7, o.written); put(11, o.size);
    a[15] = rejected ? OTA.FLAG_REJECTED : 0;
    put(16, OTA.RING - (o.accepted - o.written));
    return a;
  }

  _onOta(rep) {
    const o = this.ota;
    const len = rep[1];
    const offset = rep[2] | (rep[3] << 8) | (rep[4] << 16);
    assert.ok(len <= OTA.MAX_PAYLOAD, '0xC3 の本文は 27 バイトまで');
    if (len === 0) {
      // 「状態だけ返せ」。待っている間に本体の書き込みは追いつく。
      o.written = o.accepted;
      this._emit(this._otaReply(false));
      return REPORT_SIZE + 1;
    }
    if (o.state !== 1 || offset !== o.accepted) {
      o.rejects += 1;
      this._emit(this._otaReply(true));
      return REPORT_SIZE + 1;
    }
    for (let i = 0; i < len; i += 1) o.received.push(rep[OTA.HEADER_SIZE + i]);
    o.accepted += len;
    o.written = Math.max(0, o.accepted - this.writeLag);
    if (o.accepted >= o.size) o.written = o.accepted;
    if (o.accepted >= o.size || o.accepted - o.ackedAt >= OTA.ACK_EVERY) {
      o.ackedAt = o.accepted;
      this._emit(this._otaReply(false));
    }
    return REPORT_SIZE + 1;
  }
}

// ---------------------------------------------------------------------------
test('Node の転送層: app.info が 0xC0/0xC1 を通って返る', async () => {
  const dev = new FakeHid();
  const link = new NodeHidLink(dev);
  try {
    const info = await link.request('app.info', null, { timeoutMs: 5000 });
    assert.equal(info.ok, 1);
    assert.equal(info.running.label, 'ota_0');
    assert.equal(info.next.label, 'ota_1');
  } finally {
    link.close();
  }
});

test('Node の転送層: 応答が無ければ諦める (無限に待たない)', async () => {
  const dev = new FakeHid();
  dev._handleLine = () => {};        // 何も返さないデバイス
  const link = new NodeHidLink(dev);
  try {
    await assert.rejects(
      link.request('app.info', null, { timeoutMs: 300 }), /応答がありません/);
  } finally {
    link.close();
  }
});

test('Node の転送層: runOta 一式 (--no-commit) が偽のデバイスで通る', async () => {
  const img = makeImage(40 * 1024);
  const dev = new FakeHid();
  const link = new NodeHidLink(dev);
  const steps = [];
  const progress = [];
  try {
    const result = await runOta(link, img, {
      commit: false,
      onStep: (kind) => steps.push(kind),
      onProgress: (p) => progress.push(p.accepted),
    });
    assert.equal(result.committed, false);
    assert.equal(result.want, await sha256Hex(img));
    assert.equal(result.wantImage, embeddedSha(img));
    assert.deepEqual(steps, ['info', 'begin', 'send', 'end', 'done']);
    // ★ 像が 1 バイトも欠けず・重複せず・並べ替わらずに届いている
    assert.equal(dev.ota.received.length, img.length);
    assert.deepEqual(Uint8Array.from(dev.ota.received), img);
    // 本体が返した名札が、こちらの知っている名札と一致している
    assert.equal(result.ended.partition_sha256, embeddedSha(img));
    assert.equal(result.ended.valid, 1);
    assert.equal(dev.ota.begins, 1);
    assert.equal(dev.ota.aborts, 0);
    assert.equal(dev.ota.rejects, 0);
    assert.ok(progress.length > 0 && progress[progress.length - 1] <= img.length);
  } finally {
    link.close();
  }
});

test('Node の転送層: 転送中は 0xC1 の取り出しを止める', async () => {
  const img = makeImage(8 * 1024);
  const dev = new FakeHid();
  const link = new NodeHidLink(dev);
  let pollsDuringSend = 0;
  const realOnRx = dev._onRx.bind(dev);
  dev._onRx = (...a) => {
    if (link._pollPaused) pollsDuringSend += 1;
    return realOnRx(...a);
  };
  try {
    await runOta(link, img, { commit: false });
    assert.equal(pollsDuringSend, 0, '止めたはずの 0xC1 が飛んでいる');
    assert.deepEqual(Uint8Array.from(dev.ota.received), img);
  } finally {
    link.close();
  }
});

test('Node の転送層: 転送で失敗したら ota.abort を撃ってから投げる', async () => {
  const img = makeImage(16 * 1024);
  const dev = new FakeHid();
  const link = new NodeHidLink(dev);
  const realOnOta = dev._onOta.bind(dev);
  let seen = 0;
  dev._onOta = (rep) => {
    seen += 1;
    if (seen > 40 && rep[1] > 0) return REPORT_SIZE + 1;   // 黙る
    return realOnOta(rep);
  };
  try {
    await assert.rejects(runOta(link, img, { commit: false, maxResyncs: 2 }));
    assert.equal(dev.ota.aborts, 1, 'ota.abort を撃っていない');
  } finally {
    link.close();
  }
});

test("Node の転送層: デバイスの 'error' で落ちない", async () => {
  const dev = new FakeHid();
  const link = new NodeHidLink(dev);
  try {
    // ★ 聞き手が付いていなければ、この 1 行でプロセスごと落ちる。
    //   再起動を待っている間に必ず飛んでくるので、ここは譲れない。
    assert.doesNotThrow(() => dev.emit('error', new Error('hid_read_timeout')));
    const info = await link.request('app.info', null, { timeoutMs: 5000 });
    assert.equal(info.ok, 1, "'error' のあとも話せること");
  } finally {
    link.close();
  }
  // 閉じたあとに遅れて飛んでくる 'error' でも落ちない。
  assert.doesNotThrow(() => dev.emit('error', new Error('late')));
});

test("Node の転送層: close() は 'data' だけ外し、'error' は残す", () => {
  const dev = new FakeHid();
  const link = new NodeHidLink(dev);
  assert.ok(dev.listenerCount('error') > 0);
  assert.ok(dev.listenerCount('data') > 0);
  link.close();
  // ★ 'error' の聞き手を 0 にしてはいけない。閉じたあとに遅れて飛んでくる
  //   'error' でプロセスごと落ちる。
  assert.ok(dev.listenerCount('error') > 0, "close() が 'error' の聞き手を消した");
  assert.equal(dev.listenerCount('data'), 0);
  assert.equal(dev.closed, true);
});

test('Node の転送層: 書けなかった要求は宙に浮かせない', async () => {
  const dev = new FakeHid();
  const link = new NodeHidLink(dev);
  dev.write = () => { throw new Error('IOHIDDeviceSetReport failed'); };
  try {
    // ★ ここで投げるだけでなく、tracker に積んだ promise にも聞き手が
    //   付いていること。付いていないと、200 ms 後の sweep() の reject が
    //   「unhandled rejection」になってプロセスごと落ちる。
    await assert.rejects(link.request('app.info', null, { timeoutMs: 300 }));
    assert.equal(link.tracker.pendingCount, 0, '要求が宙に浮いている');
    // sweep が回るだけの時間を置いて、落ちないことを確かめる。
    await new Promise((r) => setTimeout(r, 500));
  } finally {
    link.close();
  }
});
