// hid.js (WebHID トランスポート) の単体テスト。
//
//   node --test test/            (Node 18 / 20)
//   node --test 'test/**/*.mjs'  (Node 22 以降。位置引数が glob になったため)
//   node --test                  (引数なし。どの版でも動く)
//
// ブラウザもデバイスも要らない。ここで試すのは DOM に触らない純関数だけ。
// 守っているのは「デバイスとページの間で崩れると気づきにくい」ところ:
//   - 32 バイト固定のレポートに本文を 29 バイトずつ詰める割り方
//   - 受け取ったレポートの読み取り (len / flags / 壊れた入力)
//   - 割って送って繋ぎ直すと元のバイト列に戻ること
//   - 要求 1 件が HID を通って Demux まで無事に届くこと
//   - どちらの経路 (serial / hid) で繋ぐかの決め方
//
// 期待値はデバイス側の Raw HID コンソールの実装に合わせてある。
// あちらを直したらここも直すこと。

import test from 'node:test';
import assert from 'node:assert/strict';

import {
  FLAG_MORE,
  HEADER_SIZE,
  HID_CMD,
  MAX_PAYLOAD,
  REPORT_SIZE,
  buildInfoReport,
  buildPollReport,
  chooseTransport,
  decodeRxReport,
  encodeTxReports,
  parseInfoReport,
} from '../docs/js/hid.js';
import { Demux, PROTOCOL, buildRequest } from '../docs/js/protocol.js';

/** 0..n-1 の数を並べたバイト列。 */
const seq = (n) => Uint8Array.from({ length: n }, (_, i) => i & 0xff);

/** デバイス → ホストのレポートを組み立てる (テスト用の偽デバイス)。 */
function makeRxReport(payload, opts = {}) {
  const rep = new Uint8Array(REPORT_SIZE);
  rep[0] = opts.id == null ? HID_CMD.RX : opts.id;
  rep[1] = opts.len == null ? payload.length : opts.len;
  rep[2] = opts.more ? FLAG_MORE : 0;
  rep.set(payload, HEADER_SIZE);
  return new DataView(rep.buffer);
}

// ===========================================================================
// 送信レポートの割り方
// ===========================================================================

test('枠の大きさは 32 バイト固定・本文は 29 バイトまで', () => {
  assert.equal(REPORT_SIZE, 32);
  assert.equal(HEADER_SIZE, 3);
  assert.equal(MAX_PAYLOAD, 29);
});

test('29 バイトちょうどは 1 枚に収まる', () => {
  const reports = encodeTxReports(seq(29));
  assert.equal(reports.length, 1);
  assert.equal(reports[0].length, REPORT_SIZE);
  assert.equal(reports[0][0], HID_CMD.TX);
  assert.equal(reports[0][1], 29);
  assert.equal(reports[0][2], 0);
  assert.deepEqual(reports[0].slice(HEADER_SIZE), seq(29));
});

test('30 バイトは 29 + 1 に割れる', () => {
  const reports = encodeTxReports(seq(30));
  assert.equal(reports.length, 2);
  assert.equal(reports[0][1], 29);
  assert.equal(reports[1][1], 1);
  assert.equal(reports[1][HEADER_SIZE], 29);
  // 端数の後ろは 0 埋め。前の中身が残らないこと。
  assert.deepEqual(reports[1].slice(HEADER_SIZE + 1), new Uint8Array(MAX_PAYLOAD - 1));
});

test('どの枚数でもレポートは必ず 32 バイト', () => {
  for (const n of [1, 28, 29, 58, 59, 100]) {
    for (const rep of encodeTxReports(seq(n))) {
      assert.equal(rep.length, REPORT_SIZE, n + ' バイトのとき');
      assert.equal(rep[0], HID_CMD.TX);
      assert.ok(rep[1] <= MAX_PAYLOAD);
    }
  }
});

test('87 バイト (29 の 3 倍) はちょうど 3 枚', () => {
  const reports = encodeTxReports(seq(87));
  assert.equal(reports.length, 3);
  assert.deepEqual(reports.map((r) => r[1]), [29, 29, 29]);
});

test('空入力は 1 枚も作らない (送るものが無い)', () => {
  assert.deepEqual(encodeTxReports(new Uint8Array(0)), []);
  assert.deepEqual(encodeTxReports(null), []);
  assert.deepEqual(encodeTxReports(undefined), []);
});

// ===========================================================================
// 受信レポートの読み取り
// ===========================================================================

test('len / flags / payload を取り出す', () => {
  const rep = decodeRxReport(makeRxReport(seq(5)));
  assert.equal(rep.id, HID_CMD.RX);
  assert.equal(rep.len, 5);
  assert.equal(rep.flags, 0);
  assert.equal(rep.more, false);
  assert.deepEqual(rep.payload, seq(5));
});

test('flags の bit0 が「まだ続きがある」', () => {
  const rep = decodeRxReport(makeRxReport(seq(29), { more: true }));
  assert.equal(rep.flags & FLAG_MORE, FLAG_MORE);
  assert.equal(rep.more, true);
  assert.equal(rep.len, 29);
  assert.deepEqual(rep.payload, seq(29));
});

test('len が 0 のときは payload が空 (0 埋めを拾わない)', () => {
  const rep = decodeRxReport(makeRxReport(new Uint8Array(0)));
  assert.equal(rep.len, 0);
  assert.equal(rep.payload.length, 0);
});

test('len が 29 を超える壊れたレポートは弾く', () => {
  assert.equal(decodeRxReport(makeRxReport(seq(29), { len: 30 })), null);
  assert.equal(decodeRxReport(makeRxReport(seq(29), { len: 255 })), null);
});

test('短すぎるレポートと出鱈目な入力は弾く', () => {
  assert.equal(decodeRxReport(new DataView(new Uint8Array(2).buffer)), null);
  assert.equal(decodeRxReport(null), null);
  assert.equal(decodeRxReport(undefined), null);
  assert.equal(decodeRxReport({}), null);
  // 枠は読めても本文がはみ出す場合 (32 バイト未満のレポート)
  const short = new Uint8Array([HID_CMD.RX, 10, 0, 1, 2]);
  assert.equal(decodeRxReport(new DataView(short.buffer)), null);
});

test('payload は写し。元のレポートを使い回されても壊れない', () => {
  const src = new Uint8Array(REPORT_SIZE);
  src[0] = HID_CMD.RX;
  src[1] = 3;
  src.set([1, 2, 3], HEADER_SIZE);
  const rep = decodeRxReport(new DataView(src.buffer));
  src.fill(0xff); // ブラウザは同じ ArrayBuffer を次の受信に使い回しうる
  assert.deepEqual(rep.payload, Uint8Array.from([1, 2, 3]));
});

// ===========================================================================
// 割って繋ぎ直す
// ===========================================================================

test('割って繋ぎ直すと元のバイト列に戻る', () => {
  for (const n of [0, 1, 28, 29, 30, 58, 59, 200]) {
    const src = seq(n);
    const joined = [];
    for (const tx of encodeTxReports(src)) {
      // デバイスが受け取った本文を、そのまま受信レポートに載せて返すとみなす。
      const rep = decodeRxReport(makeRxReport(tx.slice(HEADER_SIZE, HEADER_SIZE + tx[1])));
      joined.push(...rep.payload);
    }
    assert.deepEqual(Uint8Array.from(joined), src, n + ' バイトのとき');
  }
});

// ===========================================================================
// 取り出しと情報の問い合わせ
// ===========================================================================

test('取り出し (0xC1) のレポートは中身が空', () => {
  const rep = buildPollReport();
  assert.equal(rep.length, REPORT_SIZE);
  assert.equal(rep[0], HID_CMD.RX);
  assert.deepEqual(rep.slice(1), new Uint8Array(REPORT_SIZE - 1));
});

test('情報 (0xC2) のレポートは中身が空', () => {
  const rep = buildInfoReport();
  assert.equal(rep.length, REPORT_SIZE);
  assert.equal(rep[0], HID_CMD.INFO);
  assert.deepEqual(rep.slice(1), new Uint8Array(REPORT_SIZE - 1));
});

test('情報の応答から版・送信待ち・取りこぼしを読む', () => {
  const rep = new Uint8Array(REPORT_SIZE);
  rep[0] = HID_CMD.INFO;
  rep[1] = 1;
  rep[2] = 0;
  rep.set([0x78, 0x56, 0x34, 0x12], 3); // little endian
  rep.set([0x01, 0x00, 0x00, 0x00], 7);
  assert.deepEqual(parseInfoReport(new DataView(rep.buffer)), {
    proto: 1,
    pending: 0x12345678,
    dropped: 1,
  });
});

test('取りこぼしが 32 bit いっぱいでも符号なしで読む', () => {
  const rep = new Uint8Array(REPORT_SIZE);
  rep[0] = HID_CMD.INFO;
  rep[1] = 1;
  rep.set([0xff, 0xff, 0xff, 0xff], 7);
  assert.equal(parseInfoReport(new DataView(rep.buffer)).dropped, 4294967295);
});

test('情報でないレポートや短い入力は弾く', () => {
  const rep = new Uint8Array(REPORT_SIZE);
  rep[0] = HID_CMD.RX;
  assert.equal(parseInfoReport(new DataView(rep.buffer)), null);
  assert.equal(parseInfoReport(new DataView(new Uint8Array(4).buffer)), null);
  assert.equal(parseInfoReport(null), null);
});

// ===========================================================================
// 要求 1 件を HID に通す (経路まるごと)
// ===========================================================================

test('要求 1 件を割って送り、繋ぎ直して Demux に流すと元の枠が取れる', () => {
  const line = buildRequest(1, 'status');
  assert.equal(line, PROTOCOL.REQ_PREFIX + '{"id":1,"cmd":"status"}' + PROTOCOL.REQ_TERM);

  const reports = encodeTxReports(new TextEncoder().encode(line));
  assert.equal(reports.length, 1); // 25 バイトなので 1 枚

  const demux = new Demux();
  const decoder = new TextDecoder('utf-8');
  const out = { text: '', frames: [], errors: [] };
  for (const tx of reports) {
    const rep = decodeRxReport(makeRxReport(tx.slice(HEADER_SIZE, HEADER_SIZE + tx[1])));
    const got = demux.push(decoder.decode(rep.payload, { stream: true }));
    out.text += got.text;
    out.frames.push(...got.frames);
    out.errors.push(...got.errors);
  }
  assert.deepEqual(out.frames, [{ id: 1, cmd: 'status' }]);
  assert.equal(out.text, '');
  assert.deepEqual(out.errors, []);
});

test('29 バイトの切れ目で割れた応答とログが混ざっても元に戻る', () => {
  // 実際のデバイスはログと応答枠を 1 本の流れに混ぜて返す。
  const frame = PROTOCOL.RES_PREFIX + '{"id":7,"bat":83,"wifi":"up","ip":"192.168.1.42"}'
    + PROTOCOL.RES_TERM;
  const stream = 'boot ok\n' + frame + 'matrix scan\n';
  const bytes = new TextEncoder().encode(stream);
  assert.ok(bytes.length > MAX_PAYLOAD * 2, '2 枚以上に割れること');

  const demux = new Demux();
  const decoder = new TextDecoder('utf-8');
  let text = '';
  const frames = [];
  const chunks = encodeTxReports(bytes); // 割り方は送受で同じ
  chunks.forEach((tx, i) => {
    const rep = decodeRxReport(makeRxReport(
      tx.slice(HEADER_SIZE, HEADER_SIZE + tx[1]),
      { more: i < chunks.length - 1 },
    ));
    assert.equal(rep.more, i < chunks.length - 1);
    const got = demux.push(decoder.decode(rep.payload, { stream: true }));
    text += got.text;
    frames.push(...got.frames);
  });
  assert.deepEqual(frames, [{ id: 7, bat: 83, wifi: 'up', ip: '192.168.1.42' }]);
  assert.equal(text, 'boot ok\nmatrix scan\n');
});

test('多バイト文字がレポートの切れ目で割れても化けない', () => {
  const stream = 'あ'.repeat(40) + '\n';
  const bytes = new TextEncoder().encode(stream); // 3 バイト × 40 + 1
  const demux = new Demux();
  const decoder = new TextDecoder('utf-8');
  let text = '';
  for (const tx of encodeTxReports(bytes)) {
    const rep = decodeRxReport(makeRxReport(tx.slice(HEADER_SIZE, HEADER_SIZE + tx[1])));
    text += demux.push(decoder.decode(rep.payload, { stream: true })).text;
  }
  assert.equal(text, stream);
});

// ===========================================================================
// どちらの経路で繋ぐか
// ===========================================================================

const ALL_OK = { serialSupported: true, hidSupported: true };

test('serial を指名: 使えればそれ、駄目なら選ばない', () => {
  assert.deepEqual(
    chooseTransport({ mode: 'serial', ...ALL_OK }),
    { transport: 'serial', reason: null },
  );
  // 許可済みが HID 側にしか無くても、指名は覆らない。
  assert.equal(
    chooseTransport({ mode: 'serial', ...ALL_OK, hasHidDevice: true }).transport,
    'serial',
  );
  const ng = chooseTransport({ mode: 'serial', serialSupported: false, hidSupported: true });
  assert.equal(ng.transport, null);
  assert.match(ng.reason, /USB シリアル/);
});

test('hid を指名: 使えればそれ、駄目なら選ばない', () => {
  assert.deepEqual(
    chooseTransport({ mode: 'hid', ...ALL_OK }),
    { transport: 'hid', reason: null },
  );
  assert.equal(
    chooseTransport({ mode: 'hid', ...ALL_OK, hasSerialPort: true }).transport,
    'hid',
  );
  const ng = chooseTransport({ mode: 'hid', serialSupported: true, hidSupported: false });
  assert.equal(ng.transport, null);
  assert.match(ng.reason, /USB HID/);
});

test('自動: 許可済みの口があるほうを選ぶ (serial が先)', () => {
  assert.equal(
    chooseTransport({ mode: 'auto', ...ALL_OK, hasSerialPort: true }).transport,
    'serial',
  );
  assert.equal(
    chooseTransport({ mode: 'auto', ...ALL_OK, hasHidDevice: true }).transport,
    'hid',
  );
  assert.equal(
    chooseTransport({ mode: 'auto', ...ALL_OK, hasSerialPort: true, hasHidDevice: true }).transport,
    'serial',
  );
});

test('自動: 許可済みが無ければ使えるほう (serial が先)', () => {
  assert.equal(chooseTransport({ mode: 'auto', ...ALL_OK }).transport, 'serial');
  assert.equal(
    chooseTransport({ mode: 'auto', serialSupported: false, hidSupported: true }).transport,
    'hid',
  );
  assert.equal(
    chooseTransport({ mode: 'auto', serialSupported: true, hidSupported: false }).transport,
    'serial',
  );
});

test('自動: 使えない経路の許可済みは数えない', () => {
  // Web Serial を持たないブラウザに serial の許可が残っていることは無いが、
  // 呼び出し側の取り違えでそうなっても HID に落ちること。
  const got = chooseTransport({
    mode: 'auto', serialSupported: false, hidSupported: true, hasSerialPort: true,
  });
  assert.equal(got.transport, 'hid');
});

test('自動: 両方駄目なら選ばない', () => {
  const ng = chooseTransport({ mode: 'auto', serialSupported: false, hidSupported: false });
  assert.equal(ng.transport, null);
  assert.match(ng.reason, /USB シリアル/);
  assert.match(ng.reason, /USB HID/);
});

test('知らない指定と引数なしは自動と同じ扱い', () => {
  assert.equal(chooseTransport({ mode: 'usb', ...ALL_OK }).transport, 'serial');
  assert.equal(chooseTransport({ ...ALL_OK }).transport, 'serial');
  assert.equal(chooseTransport().transport, null);
  assert.equal(chooseTransport({}).transport, null);
});
