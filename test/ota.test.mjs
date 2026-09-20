// docs/js/ota.js — アプリ内 OTA の中核を Node で回す。**実機には触らない。**
//
// 偽の転送層 (FakeDevice) が firmware/main/stackee_otacore.c と同じ約束で
// ふるまう: 位置の合わない枠は捨てる / 受け取った累積が 1 KB を跨ぐたびに
// 1 枚だけ応答を返す / 環状バッファに空きが無ければ捨てる。
//
// 見ているのは 5 つ:
//   ・枠の組み立てと読み取りが、C 側と同じ形か (バイト位置まで)
//   ・credit — 「書き終えた + 32 KB」より先へ送っていないか
//   ・応答を取りこぼしても最後まで通るか (送り直しで自力で戻れるか)
//   ・本体が黙ったら諦めるか (無限に待たない)
//   ・進捗が単調で、最後に size に届くか
//
//   node --test 'test/**/*.mjs'
import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';

import {
  OTA, OTA_STATE, OTA_ERR,
  buildDataReport, buildStatusReport, encodeImage, parseStatusReport,
  sendLimit, sha256Hex, sameSha, looksLikeEspImage, otaErrorText,
  embeddedSha, verifyEmbeddedSha, transferImage, runOta,
} from '../docs/js/ota.js';

// ---------------------------------------------------------------------------
// 道具
// ---------------------------------------------------------------------------
/**
 * 作り物の像。**末尾 32 バイトに本物の SHA-256 を付ける** (hash_appended)。
 * 実機の .bin と同じ形にしておかないと、名札 (埋め込み sha256) の照合が
 * 試せない。
 */
function makeImage(size) {
  const img = new Uint8Array(size);
  img[0] = 0xe9;            // ESP のアプリ像の印
  for (let i = 1; i < size; i += 1) img[i] = (i * 31 + (i >> 8)) & 0xff;
  const tail = createHash('sha256').update(img.subarray(0, size - 32)).digest();
  img.set(tail, size - 32);
  return img;
}

/** 進む偽の時計。sleep() を呼ぶと時刻が進む。 */
function makeClock() {
  let t = 1000;
  return {
    now: () => t,
    sleep: async (ms) => { t += ms; },
    advance: (ms) => { t += ms; },
  };
}

/**
 * firmware/main/stackee_otacore.c のふるまいを写した偽デバイス。
 */
class FakeDevice {
  constructor(opts = {}) {
    this.size = opts.size || 0;
    this.ring = opts.ring || OTA.RING;
    this.ackEvery = opts.ackEvery || OTA.ACK_EVERY;
    this.accepted = 0;
    this.written = 0;
    this.state = 1;           // receiving
    this.err = 0;
    this.ackedAt = 0;
    this.frames = 0;
    this.rejects = 0;
    this.received = [];
    this.replies = [];        // 実際にホストへ渡した応答
    /** テストが仕掛ける細工 */
    this.dropEveryNthReply = opts.dropEveryNthReply || 0;
    this.dropFrameAt = opts.dropFrameAt === undefined ? -1 : opts.dropFrameAt;
    this.deaf = false;        // true なら何も受け取らない
    this.replyCount = 0;
    /** 書き込みは「受け取ったぶんを少し遅れて追いかける」ことにする。 */
    this.writeLag = opts.writeLag === undefined ? 4096 : opts.writeLag;
  }

  _reply(rejected) {
    const a = new Uint8Array(OTA.REPORT_SIZE);
    a[0] = OTA.CMD_DATA;
    a[1] = this.state;
    a[2] = this.err;
    const put = (i, v) => {
      a[i] = v & 0xff; a[i + 1] = (v >> 8) & 0xff;
      a[i + 2] = (v >> 16) & 0xff; a[i + 3] = (v >>> 24) & 0xff;
    };
    put(3, this.accepted);
    put(7, this.written);
    put(11, this.size);
    a[15] = rejected ? OTA.FLAG_REJECTED : 0;
    put(16, this.ring - (this.accepted - this.written));
    return a;
  }

  /** ホストが 1 枚送ってきた。応答を返すなら Uint8Array、返さないなら null。 */
  feed(report) {
    assert.equal(report.length, OTA.REPORT_SIZE);
    assert.equal(report[0], OTA.CMD_DATA);
    const len = report[1];
    const offset = report[2] | (report[3] << 8) | (report[4] << 16);
    if (len === 0) {
      // 「状態だけ返せ」。ホストが待っている間に本体の書き込みは追いつく。
      this.written = this.accepted;
      return this._reply(false);
    }
    this.frames += 1;
    if (this.deaf) return null;
    if (this.frames === this.dropFrameAt) return null;   // 届かなかったことにする
    if (offset !== this.accepted) {
      this.rejects += 1;
      return this._reply(true);
    }
    if (len > this.ring - (this.accepted - this.written)) {
      return this._reply(true);
    }
    for (let i = 0; i < len; i += 1) this.received.push(report[OTA.HEADER_SIZE + i]);
    this.accepted += len;
    // 書き込みは遅れて追いかける (実機のフラッシュ書き込みと同じ向き)
    this.written = Math.max(0, Math.min(this.accepted, this.accepted - this.writeLag));
    if (this.accepted >= this.size) this.written = this.accepted;
    if (this.accepted >= this.size || this.accepted - this.ackedAt >= this.ackEvery) {
      this.ackedAt = this.accepted;
      return this._reply(false);
    }
    return null;
  }
}

/** FakeDevice を ota.js の link の形にくるむ。 */
function makeLink(device, opts = {}) {
  const listeners = new Set();
  let maxSentAhead = 0;
  const link = {
    sent: 0,
    reports: 0,
    async sendOtaReport(report) {
      link.reports += 1;
      const offset = report[2] | (report[3] << 8) | (report[4] << 16);
      const ahead = offset + report[1] - device.written;
      if (ahead > maxSentAhead) maxSentAhead = ahead;
      const reply = device.feed(report);
      if (!reply) return;
      device.replyCount += 1;
      if (opts.dropEveryNthReply && device.replyCount % opts.dropEveryNthReply === 0) {
        return;   // ★ 応答を落とす。ホストは送り直しで立ち直れること
      }
      const st = parseStatusReport(reply);
      for (const cb of listeners) cb(st);
    },
    onOtaStatus(cb) { listeners.add(cb); return () => listeners.delete(cb); },
    get maxSentAhead() { return maxSentAhead; },
  };
  return link;
}

// ---------------------------------------------------------------------------
test('0xC3 の枠は C 側 (stackee_otacore.h) と同じ並び', () => {
  const rep = buildDataReport(0x123456, Uint8Array.from([1, 2, 3]));
  assert.equal(rep.length, 32);
  assert.equal(rep[0], 0xc3);
  assert.equal(rep[1], 3);
  assert.equal(rep[2], 0x56);
  assert.equal(rep[3], 0x34);
  assert.equal(rep[4], 0x12);
  assert.deepEqual([...rep.slice(5, 8)], [1, 2, 3]);
  assert.deepEqual([...rep.slice(8)], new Array(24).fill(0));
});

test('本文は 27 バイトまで、位置は 24 bit まで', () => {
  assert.throws(() => buildDataReport(0, new Uint8Array(28)), RangeError);
  assert.throws(() => buildDataReport(0x1000000, new Uint8Array(1)), RangeError);
  assert.throws(() => buildDataReport(-1, new Uint8Array(1)), RangeError);
  assert.doesNotThrow(() => buildDataReport(0xffffff, new Uint8Array(27)));
});

test('本文 0 バイトの枠は「状態だけ返せ」', () => {
  const rep = buildStatusReport();
  assert.equal(rep[0], 0xc3);
  assert.equal(rep[1], 0);
  assert.equal(rep.length, 32);
});

test('像を割ると全バイトが 1 度ずつ・順番どおりに出る', () => {
  const img = makeImage(1000);
  const reports = encodeImage(img);
  assert.equal(reports.length, Math.ceil(1000 / 27));
  const out = [];
  let expect = 0;
  for (const r of reports) {
    const offset = r[2] | (r[3] << 8) | (r[4] << 16);
    assert.equal(offset, expect);
    expect += r[1];
    for (let i = 0; i < r[1]; i += 1) out.push(r[5 + i]);
  }
  assert.equal(expect, 1000);
  assert.deepEqual(out, [...img]);
});

test('応答の読み取り (壊れた入力は null)', () => {
  const dev = new FakeDevice({ size: 100 });
  dev.accepted = 0x010203;
  dev.written = 0x04;
  const st = parseStatusReport(dev._reply(true));
  assert.equal(st.state, 'receiving');
  assert.equal(st.err, 'none');
  assert.equal(st.accepted, 0x010203);
  assert.equal(st.written, 4);
  assert.equal(st.size, 100);
  assert.equal(st.rejected, true);
  assert.equal(parseStatusReport(new Uint8Array(4)), null);
  assert.equal(parseStatusReport(new Uint8Array(32)), null);   // id が 0xC3 でない
  assert.equal(parseStatusReport(null), null);
});

test('状態名とエラー名は C 側と同じ並び', () => {
  assert.deepEqual(OTA_STATE, ['idle', 'receiving', 'done', 'failed']);
  assert.equal(OTA_ERR[0], 'none');
  assert.equal(OTA_ERR[1], 'busy');
  assert.equal(OTA_ERR[9], 'sha');
  assert.equal(OTA_ERR[13], 'magic');
  assert.match(otaErrorText('magic'), /0xE9/);
  assert.match(otaErrorText('しらない'), /しらない/);
});

test('credit の窓は「書き終えた + 32 KB」で、像の末尾で止まる', () => {
  assert.equal(sendLimit({ written: 0, size: 1000000 }), 32768);
  assert.equal(sendLimit({ written: 100000, size: 1000000 }), 132768);
  assert.equal(sendLimit({ written: 999000, size: 1000000 }), 1000000);
  assert.equal(sendLimit({ written: 0, size: 100 }), 100);
  assert.equal(sendLimit({ written: 0, size: 1000000, credit: 4096 }), 4096);
});

test('sha256 は Node の hashlib と一致する', async () => {
  const img = makeImage(5000);
  const want = createHash('sha256').update(img).digest('hex');
  assert.equal(await sha256Hex(img), want);
  // subarray を渡しても全体を食わない
  const part = img.subarray(100, 200);
  assert.equal(await sha256Hex(part),
               createHash('sha256').update(img.slice(100, 200)).digest('hex'));
});

test('sha の突き合わせは大文字小文字と空白を無視する', () => {
  assert.equal(sameSha('ABcd', ' abCD '), true);
  assert.equal(sameSha('ab', 'ac'), false);
  assert.equal(sameSha(null, 'ab'), false);
  assert.equal(sameSha('ab', undefined), false);
});

test('ESP の像かどうかは先頭 1 バイトで分かる', () => {
  assert.equal(looksLikeEspImage(makeImage(1000)), true);
  const bad = makeImage(1000);
  bad[0] = 0x7f;
  assert.equal(looksLikeEspImage(bad), false);
  assert.equal(looksLikeEspImage(new Uint8Array(4)), false);
  assert.equal(looksLikeEspImage(null), false);
});

// ---------------------------------------------------------------------------
// 転送
// ---------------------------------------------------------------------------
test('素直な転送: 全バイトが 1 度ずつ届く', async () => {
  const img = makeImage(60000);
  const dev = new FakeDevice({ size: img.length });
  const link = makeLink(dev);
  const clock = makeClock();
  const seen = [];
  const r = await transferImage(link, img, {
    now: clock.now, sleep: clock.sleep,
    onProgress: (p) => seen.push(p.sent),
  });
  assert.equal(r.accepted, img.length);
  assert.equal(r.resyncs, 0);
  assert.deepEqual(dev.received, [...img]);
  // 進捗は単調で、最後は像の大きさ
  for (let i = 1; i < seen.length; i += 1) assert.ok(seen[i] > seen[i - 1]);
  assert.equal(seen[seen.length - 1], img.length);
});

test('credit — 「書き終えた + 32 KB」より先へは送らない', async () => {
  const img = makeImage(300000);
  const dev = new FakeDevice({ size: img.length, writeLag: 20000 });
  const link = makeLink(dev);
  const clock = makeClock();
  await transferImage(link, img, { now: clock.now, sleep: clock.sleep });
  assert.ok(link.maxSentAhead <= OTA.CREDIT,
            `先行 ${link.maxSentAhead} B が credit ${OTA.CREDIT} B を超えた`);
  // credit をきちんと使い切っている (単なる 1 枠ずつになっていない)
  assert.ok(link.maxSentAhead > 20000, `先行 ${link.maxSentAhead} B しか出ていない`);
  assert.deepEqual(dev.received, [...img]);
});

test('credit を小さくすると、そのぶんしか先行しない', async () => {
  const img = makeImage(80000);
  const dev = new FakeDevice({ size: img.length, writeLag: 8192 });
  const link = makeLink(dev);
  const clock = makeClock();
  await transferImage(link, img, { credit: 8192, now: clock.now, sleep: clock.sleep });
  assert.ok(link.maxSentAhead <= 8192 + OTA.MAX_PAYLOAD);
  assert.deepEqual(dev.received, [...img]);
});

test('応答を 3 枚に 1 枚落としても最後まで通る', async () => {
  const img = makeImage(120000);
  const dev = new FakeDevice({ size: img.length });
  const link = makeLink(dev, { dropEveryNthReply: 3 });
  const clock = makeClock();
  const r = await transferImage(link, img, { now: clock.now, sleep: clock.sleep });
  assert.equal(r.accepted, img.length);
  assert.deepEqual(dev.received, [...img]);
});

test('枠が 1 枚落ちても、本体が言う位置から送り直して通る', async () => {
  const img = makeImage(40000);
  const dev = new FakeDevice({ size: img.length, dropFrameAt: 50 });
  const link = makeLink(dev);
  const clock = makeClock();
  const r = await transferImage(link, img, { now: clock.now, sleep: clock.sleep });
  assert.ok(r.resyncs >= 1, '送り直しが起きていない');
  assert.ok(dev.rejects > 0, '本体が位置違いを断っていない');
  assert.equal(r.accepted, img.length);
  // ★ 送り直しても像は 1 度ぶんしか入っていない (重複が混ざらない)
  assert.deepEqual(dev.received, [...img]);
});

test('本体が黙ったら諦める (無限に待たない)', async () => {
  const img = makeImage(40000);
  const dev = new FakeDevice({ size: img.length });
  const link = makeLink(dev);
  const clock = makeClock();
  dev.deaf = true;
  await assert.rejects(
    transferImage(link, img, { now: clock.now, sleep: clock.sleep, maxResyncs: 3 }),
    /送り直しが多すぎ/);
});

test('本体がエラーを返したら、その場で止める', async () => {
  const img = makeImage(40000);
  const dev = new FakeDevice({ size: img.length });
  const link = makeLink(dev);
  const clock = makeClock();
  let n = 0;
  const origin = dev.feed.bind(dev);
  dev.feed = (rep) => {
    n += 1;
    if (n === 200) { dev.state = 3; dev.err = 7; return dev._reply(true); }
    return origin(rep);
  };
  await assert.rejects(
    transferImage(link, img, { now: clock.now, sleep: clock.sleep }),
    /フラッシュへの書き込み/);
});

test('signal.aborted を立てると止まる', async () => {
  const img = makeImage(200000);
  const dev = new FakeDevice({ size: img.length });
  const link = makeLink(dev);
  const clock = makeClock();
  const signal = { aborted: false };
  const orig = link.sendOtaReport;
  link.sendOtaReport = async (rep) => {
    if (link.reports > 100) signal.aborted = true;
    return orig.call(link, rep);
  };
  await assert.rejects(
    transferImage(link, img, { now: clock.now, sleep: clock.sleep, signal }),
    /中止/);
});

// ---------------------------------------------------------------------------
// 一式 (runOta)
// ---------------------------------------------------------------------------
function makeFullLink(img, opts = {}) {
  const dev = new FakeDevice({ size: img.length });
  const link = makeLink(dev);
  const calls = [];
  let sha = null;                       // ota.begin で申告されたファイル全体の sha
  const imageSha = embeddedSha(img);    // 像の名札 (末尾 32 バイト)
  let committed = false;
  let runningSha = opts.runningSha || 'ff'.repeat(32);
  link.request = async (cmd, args) => {
    calls.push([cmd, args]);
    if (cmd === 'app.info') {
      return {
        ok: 1,
        running: { label: 'ota_0', version: 'v1', sha256: runningSha },
        boot: { label: 'ota_0' },
        next: { label: 'ota_1', sha256: committed ? imageSha : null },
        ota_state: 'valid',
      };
    }
    if (cmd === 'ota.begin') {
      if (opts.busyOnce && calls.filter((c) => c[0] === 'ota.begin').length === 1) {
        return { error: 'busy' };
      }
      sha = args.sha256;
      dev.size = args.size;
      dev.accepted = 0; dev.written = 0; dev.received = []; dev.state = 1; dev.err = 0;
      dev.ackedAt = 0;
      return { ok: 1, target: 'ota_1', size: args.size, chunk: 27, credit: OTA.CREDIT };
    }
    if (cmd === 'ota.abort') { dev.state = 0; return { ok: 1 }; }
    if (cmd === 'ota.end') {
      const got = createHash('sha256').update(Uint8Array.from(dev.received)).digest('hex');
      if (opts.badEndSha) {
        return { ok: 1, sha256: '00'.repeat(32), want_sha256: sha, valid: 0,
                 partition: 'ota_1', partition_sha256: imageSha };
      }
      return {
        ok: 1, state: 'done', err: 'none', written: dev.received.length,
        size: dev.size, sha256: got, want_sha256: sha,
        partition: 'ota_1',
        partition_sha256: opts.badPartitionSha ? '11'.repeat(32) : imageSha,
        valid: 1, committed: 0,
      };
    }
    if (cmd === 'ota.commit') {
      committed = true;
      runningSha = imageSha;     // 再起動後は名札が入れ替わる
      return { ok: 1, boot: 'ota_1', in_ms: 500 };
    }
    return { error: 'unsupported' };
  };
  link.waitAndReconnect = async () => true;
  link.setPollPaused = () => {};
  link.calls = calls;
  link.device = dev;
  return link;
}

test('runOta: app.info → begin → 転送 → end → commit → 読み直し', async () => {
  const img = makeImage(50000);
  const link = makeFullLink(img);
  const clock = makeClock();
  const steps = [];
  const r = await runOta(link, img, {
    onStep: (k) => steps.push(k), now: clock.now, sleep: clock.sleep,
  });
  assert.equal(r.committed, true);
  assert.equal(r.want, await sha256Hex(img));
  assert.equal(r.wantImage, embeddedSha(img));
  assert.deepEqual(link.calls.map((c) => c[0]),
                   ['app.info', 'ota.begin', 'ota.end', 'ota.commit', 'app.info']);
  assert.deepEqual(steps, ['info', 'begin', 'send', 'end', 'commit', 'reboot', 'done']);
  assert.deepEqual(link.device.received, [...img]);
});

test('runOta: --no-commit なら切り替えない', async () => {
  const img = makeImage(30000);
  const link = makeFullLink(img);
  const clock = makeClock();
  const r = await runOta(link, img, { commit: false, now: clock.now, sleep: clock.sleep });
  assert.equal(r.committed, false);
  assert.ok(!link.calls.some((c) => c[0] === 'ota.commit'));
});

test('runOta: 同じ像なら何もしない', async () => {
  const img = makeImage(30000);
  // ★ 突き合わせるのは **名札** (埋め込みの sha256)。ファイル全体の sha では
  //   本体の app.info が返す値と噛み合わない。
  const link = makeFullLink(img, { runningSha: embeddedSha(img) });
  const clock = makeClock();
  const r = await runOta(link, img, { now: clock.now, sleep: clock.sleep });
  assert.equal(r.skipped, true);
  assert.deepEqual(link.calls.map((c) => c[0]), ['app.info']);
});

test('runOta: 二重起動 (busy) は abort してから 1 度だけやり直す', async () => {
  const img = makeImage(30000);
  const link = makeFullLink(img, { busyOnce: true });
  const clock = makeClock();
  const r = await runOta(link, img, { commit: false, now: clock.now, sleep: clock.sleep });
  assert.equal(r.committed, false);
  assert.deepEqual(link.calls.map((c) => c[0]),
                   ['app.info', 'ota.begin', 'ota.abort', 'ota.begin', 'ota.end']);
});

test('runOta: ESP の像でなければ 1 枚も送らない', async () => {
  const img = makeImage(30000);
  img[0] = 0x00;
  const link = makeFullLink(img);
  await assert.rejects(runOta(link, img), /0xE9/);
  assert.equal(link.reports, 0);
  assert.equal(link.calls.length, 0);
});

test('runOta: 本体の sha が合わなければ止める', async () => {
  const img = makeImage(30000);
  const link = makeFullLink(img, { badEndSha: true });
  const clock = makeClock();
  await assert.rejects(
    runOta(link, img, { now: clock.now, sleep: clock.sleep }),
    /sha256 が、送った像と違います/);
  assert.ok(!link.calls.some((c) => c[0] === 'ota.commit'));
});

test('runOta: 区画から読み直した sha が合わなければ止める', async () => {
  const img = makeImage(30000);
  const link = makeFullLink(img, { badPartitionSha: true });
  const clock = makeClock();
  await assert.rejects(
    runOta(link, img, { now: clock.now, sleep: clock.sleep }),
    /読み直した sha256/);
  assert.ok(!link.calls.some((c) => c[0] === 'ota.commit'));
});

test('runOta: app.info を持たない古い像は、その場で断る', async () => {
  const img = makeImage(30000);
  const link = makeFullLink(img);
  link.request = async (cmd) => {
    if (cmd === 'app.info') return { error: 'unsupported' };
    throw new Error('ここへは来ないはず: ' + cmd);
  };
  await assert.rejects(runOta(link, img), /OTA 非対応/);
});

test('runOta: 転送で失敗したら ota.abort を撃ってから投げる', async () => {
  const img = makeImage(50000);
  const link = makeFullLink(img);
  const clock = makeClock();
  const orig = link.sendOtaReport;
  link.sendOtaReport = async (rep) => {
    if (link.reports > 50) { link.device.deaf = true; }
    return orig.call(link, rep);
  };
  // ★ maxResyncs が transferImage まで届いていること。渡し忘れると既定の
  //   32 回まで粘り、「転送が終わりません」(時間切れ) のほうで落ちる。
  await assert.rejects(
    runOta(link, img, { now: clock.now, sleep: clock.sleep, maxResyncs: 2 }),
    /送り直しが多すぎ/);
  assert.ok(link.calls.some((c) => c[0] === 'ota.abort'), 'ota.abort を撃っていない');
});

// ---------------------------------------------------------------------------
// sha256 は 2 種類ある
// ---------------------------------------------------------------------------
test('像の名札は末尾 32 バイト (esptool image_info と同じ値)', async () => {
  const img = makeImage(5000);
  const tail = createHash('sha256').update(img.subarray(0, img.length - 32)).digest('hex');
  assert.equal(embeddedSha(img), tail);
  // ファイル全体の sha とは別物
  assert.notEqual(embeddedSha(img), await sha256Hex(img));
  assert.equal(embeddedSha(new Uint8Array(10)), null);
  assert.equal(embeddedSha(null), null);
});

test('末尾の sha が中身と合わない像は受け取らない', async () => {
  const img = makeImage(5000);
  assert.equal(await verifyEmbeddedSha(img), true);
  const broken = img.slice();
  broken[100] ^= 0xff;                    // 中身だけ 1 バイト変える
  assert.equal(await verifyEmbeddedSha(broken), false);
  const link = makeFullLink(broken);
  await assert.rejects(runOta(link, broken), /末尾の SHA-256/);
  assert.equal(link.reports, 0);
  assert.equal(link.calls.length, 0);
});

test('runOta: ota.begin に渡すのはファイル全体の sha256', async () => {
  const img = makeImage(20000);
  const link = makeFullLink(img);
  const clock = makeClock();
  await runOta(link, img, { commit: false, now: clock.now, sleep: clock.sleep });
  const begin = link.calls.find((c) => c[0] === 'ota.begin');
  assert.equal(begin[1].sha256, await sha256Hex(img));
  assert.equal(begin[1].size, img.length);
});
