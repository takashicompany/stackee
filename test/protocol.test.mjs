// protocol.js の単体テスト。
//
//   node --test test/            (Node 18 / 20)
//   node --test 'test/**/*.mjs'  (Node 22 以降。位置引数が glob になったため)
//   node --test                  (引数なし。どの版でも動く)
//
// ブラウザもデバイスも要らない。
// ここで守っているのは「デバイスとページの間で崩れると気づきにくい」ところ:
//   - 要求行の枠 (\x1e ... \n) と、行に制御文字が絶対に混ざらないこと
//   - ステータスバーの OSC 列が受信の切れ目で割れても壊れないこと
//   - 要求 ID の突き合わせとタイムアウト
//   - 設定値の検証
//
// 期待値は firmware/kmk/stackee_console.py の実装に合わせてある。
// あちらを直したらここも直すこと。

import test from 'node:test';
import assert from 'node:assert/strict';

import {
  CMD,
  Demux,
  ERR,
  LEGACY_WIFI_KEYS,
  MAX_WIFI_NETWORKS,
  PROTOCOL,
  RequestTracker,
  SETTING_KEYS,
  TIMEOUT_MS,
  buildRequest,
  buildSettingsArgs,
  buildWifiAddArgs,
  buildWifiRemoveArgs,
  byteLength,
  errorHead,
  errorText,
  formatUptime,
  formatWifi,
  helloSupports,
  isUnsupported,
  readHello,
  readNetworkCount,
  readNetworkList,
  readPasswordState,
  readScanResults,
  readSettings,
  readStatus,
  supportsMultiWifi,
  validateChannel,
  validatePassword,
  validatePort,
  validateSettings,
  validateSsid,
  validateWifiEntry,
} from '../web/js/protocol.js';

const RS = PROTOCOL.REQ_PREFIX;

// ===========================================================================
// 要求の組み立て
// ===========================================================================

test('要求行は \\x1e で始まり \\n で終わる', () => {
  assert.equal(buildRequest(7, CMD.STATUS), RS + '{"id":7,"cmd":"status"}\n');
});

test('引数は入れ子にせず要求の直下へ広げる', () => {
  // stackee_console.py は req.get(\'kv\') のように直下を見る
  const line = buildRequest(1, CMD.SETTINGS_SET, { kv: { STACKEE_PORT: '5555' } });
  assert.equal(line, RS + '{"id":1,"cmd":"settings.set","kv":{"STACKEE_PORT":"5555"}}\n');
});

test('引数なし / 空の引数は cmd だけの行になる', () => {
  assert.equal(buildRequest(2, CMD.HELLO), RS + '{"id":2,"cmd":"hello"}\n');
  assert.equal(buildRequest(2, CMD.HELLO, {}), RS + '{"id":2,"cmd":"hello"}\n');
  assert.equal(buildRequest(2, CMD.HELLO, null), RS + '{"id":2,"cmd":"hello"}\n');
});

test('引数に予約キー (id / cmd) は使えない', () => {
  assert.throws(() => buildRequest(1, CMD.STATUS, { id: 2 }), TypeError);
  assert.throws(() => buildRequest(1, CMD.STATUS, { cmd: 'x' }), TypeError);
});

test('非 ASCII は \\uXXXX に逃がして純 ASCII の行にする', () => {
  const line = buildRequest(3, CMD.WIFI_ADD, { ssid: 'わが家' });
  assert.match(line, /\\u308f\\u304c\\u5bb6/);
  // 行の本体が全部 ASCII の印字文字であること
  for (const ch of line.slice(1, -1)) {
    assert.ok(ch.charCodeAt(0) >= 0x20 && ch.charCodeAt(0) < 0x7f, ch);
  }
});

test('値に制御文字が入っても行に生の制御文字は出ない (Ctrl-C 混入の防止)', () => {
  // \x03 が生で通ると TinyUSB が受信 FIFO ごと捨てて KeyboardInterrupt を上げる
  const line = buildRequest(4, CMD.SETTINGS_SET, { kv: { X: 'a\x03b\nc' } });
  assert.equal(line.indexOf('\x03'), -1);
  assert.equal(line.indexOf('\n'), line.length - 1);
  assert.match(line, /\\u0003/);
});

test('要求行はデバイス側と同じ形の JSON として読み直せる', () => {
  const line = buildRequest(9, CMD.SETTINGS_SET, { kv: { [SETTING_KEYS.PORT]: '5555' } });
  const obj = JSON.parse(line.slice(1, -1));
  assert.equal(obj[PROTOCOL.KEY_ID], 9);
  assert.equal(obj[PROTOCOL.KEY_CMD], 'settings.set');
  assert.deepEqual(obj.kv, { STACKEE_PORT: '5555' });
});

test('不正な ID / コマンドは投げる', () => {
  assert.throws(() => buildRequest(-1, CMD.STATUS), TypeError);
  assert.throws(() => buildRequest(1.5, CMD.STATUS), TypeError);
  assert.throws(() => buildRequest(1, ''), TypeError);
  assert.throws(() => buildRequest(1, CMD.STATUS, [1, 2]), TypeError);
});

// ===========================================================================
// 受信ストリームの分解
// ===========================================================================

test('ログ本文と応答枠を分ける', () => {
  const d = new Demux();
  const out = d.push('起動しました\n' + RS + '{"id":1,"bat":83}\n' + 'つづき\n');
  assert.equal(out.text, '起動しました\nつづき\n');
  assert.deepEqual(out.frames, [{ id: 1, bat: 83 }]);
  assert.deepEqual(out.titles, []);
  assert.deepEqual(out.errors, []);
});

test('OSC タイトル列を本文から抜き出して titles に回す', () => {
  const d = new Demux();
  const out = d.push('a\x1b]0;\u{1F40D} | Wi-Fi: off | Done | 10.3.0\x1b\\b\n');
  assert.equal(out.text, 'ab\n');
  assert.deepEqual(out.titles, ['\u{1F40D} | Wi-Fi: off | Done | 10.3.0']);
});

test('BEL 終端の OSC も拾う', () => {
  const d = new Demux();
  const out = d.push('\x1b]0;title\x07rest');
  assert.deepEqual(out.titles, ['title']);
  assert.equal(out.text, 'rest');
});

test('CSI (行消去・桁移動・色) は本文から落とす', () => {
  const d = new Demux();
  const out = d.push('\x1b[2K\x1b[0Ghello\x1b[0m\n');
  assert.equal(out.text, 'hello\n');
  assert.deepEqual(out.titles, []);
});

test('OSC 列が受信の切れ目で割れても復元できる (1 文字ずつ流す)', () => {
  const d = new Demux();
  const stream = 'x\x1b]0;Wi-Fi: on | 10.3.0\x1b\\y\n';
  let text = '';
  const titles = [];
  for (const ch of stream) {
    const out = d.push(ch);
    text += out.text;
    titles.push(...out.titles);
  }
  assert.equal(text, 'xy\n');
  assert.deepEqual(titles, ['Wi-Fi: on | 10.3.0']);
});

test('割れた OSC の中身が本文に漏れない (2 チャンク)', () => {
  // 実機で観測されている壊れ方: <garbage>Wi-Fi: <garbage>Wi-Fi: off | Done
  const d = new Demux();
  const a = d.push('log1\n\x1b]0;Wi-Fi: ');
  assert.equal(a.text, 'log1\n');           // ← ここで "Wi-Fi: " を出してはいけない
  assert.deepEqual(a.titles, []);
  const b = d.push('off | Done\x1b\\log2\n');
  assert.equal(b.text, 'log2\n');
  assert.deepEqual(b.titles, ['Wi-Fi: off | Done']);
});

test('ESC が 1 バイトだけ来た場合も保留し、flushPending で諦められる', () => {
  const d = new Demux();
  const a = d.push('abc\x1b');
  assert.equal(a.text, 'abc');
  assert.equal(d.flushPending(), '\x1b');
  assert.equal(d.flushPending(), '');
});

test('ST の途中 (\\x1b] ... \\x1b) で切れても保留する', () => {
  const d = new Demux();
  const a = d.push('x\x1b]0;abc\x1b');
  assert.equal(a.text, 'x');
  assert.deepEqual(a.titles, []);
  const b = d.push('\\y');
  assert.deepEqual(b.titles, ['abc']);
  assert.equal(b.text, 'y');
});

test('応答枠が受信の切れ目で割れても 1 個にまとまる', () => {
  const d = new Demux();
  assert.deepEqual(d.push(RS + '{"id":5,"ok":1,').frames, []);
  assert.deepEqual(d.push('"bat":42}\n').frames, [{ id: 5, ok: 1, bat: 42 }]);
});

test('応答枠の途中に OSC が割り込んでも壊れない', () => {
  // stackee_console.py は 1 応答を 1 回の write() で出すが、C レベルの保証は無い
  const d = new Demux();
  const out = d.push(RS + '{"id":6,\x1b]0;status\x1b\\"ok":1}\n');
  assert.deepEqual(out.frames, [{ id: 6, ok: 1 }]);
  assert.deepEqual(out.titles, ['status']);
  assert.equal(out.text, '');
});

test('応答枠が 1 文字ずつ、OSC 混在で届いても復元できる', () => {
  const d = new Demux();
  const stream = 'boot\n' + RS + '{"id":2,\x1b]0;T1\x1b\\"up":12.5}\n' + 'after\n';
  let text = '';
  const frames = [];
  const titles = [];
  for (const ch of stream) {
    const out = d.push(ch);
    text += out.text;
    frames.push(...out.frames);
    titles.push(...out.titles);
  }
  assert.equal(text, 'boot\nafter\n');
  assert.deepEqual(frames, [{ id: 2, up: 12.5 }]);
  assert.deepEqual(titles, ['T1']);
});

test('壊れた JSON は errors に落ちて本文を汚さない', () => {
  const d = new Demux();
  const out = d.push(RS + '{"id":1,,}\n' + 'ok\n');
  assert.deepEqual(out.frames, []);
  assert.equal(out.errors.length, 1);
  assert.equal(out.errors[0].reason, 'json');
  assert.equal(out.text, 'ok\n');
});

test('枠の途中で次の枠が始まったら作り直す', () => {
  const d = new Demux();
  const out = d.push(RS + '{"id":1' + RS + '{"id":2,"ok":1}\n');
  assert.deepEqual(out.frames, [{ id: 2, ok: 1 }]);
  assert.equal(out.errors[0].reason, 'resync');
});

test('reset() で受信中の枠を捨てる', () => {
  const d = new Demux();
  d.push(RS + '{"id":1,');
  d.reset();
  const out = d.push('"ok":1}\n');
  assert.deepEqual(out.frames, []);
  assert.equal(out.text, '"ok":1}\n');
});

test('CRLF の \\r は枠の末尾から落とす', () => {
  const d = new Demux();
  assert.deepEqual(d.push(RS + '{"id":1,"ok":1}\r\n').frames, [{ id: 1, ok: 1 }]);
});

// ===========================================================================
// 要求と応答の対応付け・タイムアウト
// ===========================================================================

/** 手で進められる時計。 */
function fakeClock(start = 1000) {
  const c = { t: start };
  c.now = () => c.t;
  c.advance = (ms) => { c.t += ms; };
  return c;
}

test('応答は ID で対応付けられる (順番が入れ替わってもよい)', async () => {
  const tr = new RequestTracker({ now: fakeClock().now, firstId: 1 });
  const a = tr.create(CMD.STATUS);
  const b = tr.create(CMD.HELLO);
  assert.notEqual(a.id, b.id);

  assert.equal(tr.onFrame({ id: b.id, proto: 1, fw: 'stackee-console/1' }), true);
  assert.equal(tr.onFrame({ id: a.id, bat: 50 }), true);
  assert.deepEqual(await a.promise, { id: a.id, bat: 50 });
  assert.deepEqual(await b.promise, { id: b.id, proto: 1, fw: 'stackee-console/1' });
  assert.equal(tr.pendingCount, 0);
});

test('成功は "ok" の有無ではなく "error" の無さで決まる', async () => {
  // hello / status / settings.get は ok を返さない
  const tr = new RequestTracker({ now: fakeClock().now });
  const r = tr.create(CMD.SETTINGS_GET);
  tr.onFrame({ id: r.id, keys: {}, bytes: 0 });
  assert.deepEqual(await r.promise, { id: r.id, keys: {}, bytes: 0 });
});

test('知らない ID の応答は false を返す (迷子)', () => {
  const tr = new RequestTracker({ now: fakeClock().now });
  assert.equal(tr.onFrame({ id: 999, ok: 1 }), false);
  assert.equal(tr.onFrame({ ok: 1 }), false);
  assert.equal(tr.onFrame({ id: null, error: 'badjson' }), false);
  assert.equal(tr.onFrame(null), false);
});

test('error 付きの応答は Error になり、コードが読める', async () => {
  const tr = new RequestTracker({ now: fakeClock().now });
  const r = tr.create(CMD.WIFI_SCAN);
  tr.onFrame({ id: r.id, error: 'unsupported' });
  await assert.rejects(r.promise, (e) => {
    assert.equal(e.code, 'unsupported');
    assert.equal(e.cmd, CMD.WIFI_SCAN);
    assert.ok(isUnsupported(e));
    return true;
  });
});

test('"unknown:<cmd>" (古いファーム) も「未対応」として扱う', async () => {
  const tr = new RequestTracker({ now: fakeClock().now });
  const r = tr.create(CMD.WIFI_SCAN);
  tr.onFrame({ id: r.id, error: 'unknown:wifi.scan' });
  await assert.rejects(r.promise, (e) => {
    assert.equal(errorHead(e.code), ERR.UNKNOWN);
    assert.ok(isUnsupported(e));
    assert.match(e.message, /wifi\.scan/);
    return true;
  });
});

test('"denied:<KEY>" は詳細つきの日本語になる', async () => {
  const tr = new RequestTracker({ now: fakeClock().now });
  const r = tr.create(CMD.SETTINGS_SET);
  tr.onFrame({ id: r.id, error: 'denied:CIRCUITPY_WIFI_SSID' });
  await assert.rejects(r.promise, (e) => {
    assert.equal(errorHead(e.code), ERR.DENIED);
    assert.equal(isUnsupported(e), false);
    assert.match(e.message, /CIRCUITPY_WIFI_SSID/);
    return true;
  });
});

test('期限を過ぎた要求は sweep() で落ちる', async () => {
  const clock = fakeClock();
  const tr = new RequestTracker({ now: clock.now });
  const r = tr.create(CMD.STATUS, null, { timeoutMs: 3000 });

  clock.advance(2999);
  assert.deepEqual(tr.sweep(), []);
  assert.equal(tr.pendingCount, 1);

  clock.advance(1);
  assert.deepEqual(tr.sweep(), [r.id]);
  assert.equal(tr.pendingCount, 0);
  await assert.rejects(r.promise, (e) => {
    assert.equal(e.code, ERR.TIMEOUT);
    assert.equal(e.cmd, CMD.STATUS);
    return true;
  });
});

test('期限内に応答が来た要求は sweep() で落ちない', async () => {
  const clock = fakeClock();
  const tr = new RequestTracker({ now: clock.now });
  const r = tr.create(CMD.STATUS, null, { timeoutMs: 3000 });
  clock.advance(2000);
  tr.onFrame({ id: r.id, bat: 1 });
  clock.advance(5000);
  assert.deepEqual(tr.sweep(), []);
  await r.promise;
});

test('複数行応答 (将来用) は end:1 まで集め、途中の応答で期限が伸びる', async () => {
  const clock = fakeClock();
  const tr = new RequestTracker({ now: clock.now });
  const r = tr.create(CMD.WIFI_SCAN, null, { multi: true, timeoutMs: 5000 });
  tr.onFrame({ id: r.id, nets: [{ ssid: 'A', rssi: -40, ch: 6 }] });
  clock.advance(4000);
  assert.deepEqual(tr.sweep(), []);
  tr.onFrame({ id: r.id, nets: [{ ssid: 'B', rssi: -70, ch: 1 }] });
  clock.advance(4000);
  assert.deepEqual(tr.sweep(), []);
  tr.onFrame({ id: r.id, ok: 1, end: 1 });
  const parts = await r.promise;
  assert.equal(parts.length, 2);
  assert.deepEqual(readScanResults(parts).map((n) => n.ssid), ['A', 'B']);
});

test('複数行応答も無音が続けば落ちる', async () => {
  const clock = fakeClock();
  const tr = new RequestTracker({ now: clock.now });
  const r = tr.create(CMD.WIFI_SCAN, null, { multi: true, timeoutMs: 5000 });
  clock.advance(5000);
  assert.deepEqual(tr.sweep(), [r.id]);
  await assert.rejects(r.promise, (e) => e.code === ERR.TIMEOUT);
});

test('abortAll は待っている要求を全部落とす', async () => {
  const tr = new RequestTracker({ now: fakeClock().now });
  const a = tr.create(CMD.STATUS);
  const b = tr.create(CMD.HELLO);
  assert.deepEqual(tr.abortAll('切断').sort(), [a.id, b.id].sort());
  await assert.rejects(a.promise, (e) => e.code === ERR.DISCONNECTED);
  await assert.rejects(b.promise, (e) => e.code === ERR.DISCONNECTED);
  assert.equal(tr.pendingCount, 0);
});

// ===========================================================================
// 設定値の検証
// ===========================================================================

test('SSID: 空は不可、32 バイトまで', () => {
  assert.ok(validateSsid(''));
  assert.equal(validateSsid('home-ap'), null);
  assert.equal(validateSsid('a'.repeat(32)), null);
  assert.ok(validateSsid('a'.repeat(33)));
  // 日本語は文字数ではなくバイト数で数える (UTF-8 で 3 バイト/文字)
  assert.equal(byteLength('あ'), 3);
  assert.equal(validateSsid('あ'.repeat(10)), null);   // 30 バイト
  assert.ok(validateSsid('あ'.repeat(11)));            // 33 バイト
  assert.ok(validateSsid('a\x00b'));
});

test('パスワード: 空か 8〜63 文字 (WPA2-PSK の規定)', () => {
  assert.equal(validatePassword(''), null);
  assert.ok(validatePassword('1234567'));
  assert.equal(validatePassword('12345678'), null);
  assert.equal(validatePassword('x'.repeat(63)), null);
  assert.ok(validatePassword('x'.repeat(64)));
  assert.ok(validatePassword('12345\x0378'));
});

test('チャネル: 空か 1〜14', () => {
  assert.equal(validateChannel(''), null);
  assert.equal(validateChannel('  '), null);
  assert.equal(validateChannel('1'), null);
  assert.equal(validateChannel('14'), null);
  assert.ok(validateChannel('0'));
  assert.ok(validateChannel('15'));
  assert.ok(validateChannel('6.5'));
  assert.ok(validateChannel('six'));
  assert.ok(validateChannel('-1'));
});

test('ポート: 1〜65535 の整数、必須', () => {
  assert.ok(validatePort(''));
  assert.ok(validatePort('0'));
  assert.equal(validatePort('1'), null);
  assert.equal(validatePort('5555'), null);
  assert.equal(validatePort('65535'), null);
  assert.ok(validatePort('65536'));
  assert.ok(validatePort('55.5'));
  assert.ok(validatePort('abc'));
  assert.ok(validatePort('-1'));
});

test('validateSettings は音声サーバの 2 つだけを見る (Wi-Fi は含まない)', () => {
  const bad = validateSettings({ host: '', port: '0' });
  assert.equal(bad.ok, false);
  assert.deepEqual(Object.keys(bad.errors).sort(), ['host', 'port']);

  const good = validateSettings({ host: '192.168.1.10', port: '5555' });
  assert.equal(good.ok, true);
  assert.deepEqual(good.errors, {});
});

// ===========================================================================
// Wi-Fi の追加フォームの検証
// ===========================================================================

test('validateWifiEntry は問題のあるフィールド名を返す', () => {
  const bad = validateWifiEntry({ ssid: '', password: 'abc', channel: '99' });
  assert.equal(bad.ok, false);
  assert.deepEqual(Object.keys(bad.errors).sort(), ['channel', 'password', 'ssid']);
});

test('SSID は必須、UTF-8 で 32 バイトまで', () => {
  assert.equal(validateWifiEntry({ ssid: '', password: '', channel: '' }).ok, false);
  assert.equal(validateWifiEntry({ ssid: 'a'.repeat(32), password: '', channel: '' }).ok, true);
  assert.equal(validateWifiEntry({ ssid: 'a'.repeat(33), password: '', channel: '' }).ok, false);
  // 日本語は文字数ではなくバイト数 (UTF-8 で 3 バイト/文字)
  assert.equal(validateWifiEntry({ ssid: 'あ'.repeat(10) }).ok, true);   // 30 バイト
  assert.equal(validateWifiEntry({ ssid: 'あ'.repeat(11) }).ok, false);  // 33 バイト
});

test('パスワードは空 (暗号なし) か 8〜63 文字', () => {
  assert.equal(validateWifiEntry({ ssid: 'ap', password: '' }).ok, true);
  assert.equal(validateWifiEntry({ ssid: 'ap', password: '1234567' }).ok, false);
  assert.equal(validateWifiEntry({ ssid: 'ap', password: '12345678' }).ok, true);
  assert.equal(validateWifiEntry({ ssid: 'ap', password: 'x'.repeat(63) }).ok, true);
  assert.equal(validateWifiEntry({ ssid: 'ap', password: 'x'.repeat(64) }).ok, false);
});

test('チャネルは空 (自動) か 1〜14', () => {
  for (const ch of ['', '  ', '1', '14']) {
    assert.equal(validateWifiEntry({ ssid: 'ap', channel: ch }).ok, true, ch);
  }
  for (const ch of ['0', '15', '6.5', 'six', '-1']) {
    assert.equal(validateWifiEntry({ ssid: 'ap', channel: ch }).ok, false, ch);
  }
});

test('★ 8 件の上限はページでは弾かない (デバイスの "full" に任せる)', () => {
  // ページの手元の一覧は古いことがある。数の判定はデバイスだけが正しい。
  assert.equal(validateWifiEntry({ ssid: 'ap9', password: '' }).ok, true);
  assert.equal(MAX_WIFI_NETWORKS, 8);
  assert.match(errorText(ERR.FULL, CMD.WIFI_ADD), /8 件/);
});

// ===========================================================================
// wifi.add / wifi.remove の引数
// ===========================================================================

test('wifi.add は ssid / password / channel を要求の直下に置く', () => {
  const args = buildWifiAddArgs({ ssid: 'home-ap', password: 'secret12', channel: '10' });
  assert.deepEqual(args, { ssid: 'home-ap', password: 'secret12', channel: 10 });
  const line = buildRequest(1, CMD.WIFI_ADD, args);
  assert.equal(line, PROTOCOL.REQ_PREFIX
    + '{"id":1,"cmd":"wifi.add","ssid":"home-ap","password":"secret12","channel":10}\n');
});

test('チャネルは整数で送る。空欄は null (= 自動)', () => {
  assert.equal(buildWifiAddArgs({ ssid: 'a' }).channel, null);
  assert.equal(buildWifiAddArgs({ ssid: 'a', channel: '' }).channel, null);
  assert.equal(buildWifiAddArgs({ ssid: 'a', channel: '  ' }).channel, null);
  assert.equal(buildWifiAddArgs({ ssid: 'a', channel: null }).channel, null);
  assert.equal(buildWifiAddArgs({ ssid: 'a', channel: 6 }).channel, 6);
  assert.equal(buildWifiAddArgs({ ssid: 'a', channel: ' 13 ' }).channel, 13);
});

test('★ 空のパスワードも必ず送る (省くと「暗号なし」と区別できない)', () => {
  const args = buildWifiAddArgs({ ssid: 'open-ap', password: '' });
  assert.equal('password' in args, true);
  assert.equal(args.password, '');
});

test('wifi.add の値の型は ssid/password が文字列、channel が整数か null', () => {
  const args = buildWifiAddArgs({ ssid: 'ap', password: 'secret12', channel: 6 });
  assert.equal(typeof args.ssid, 'string');
  assert.equal(typeof args.password, 'string');
  assert.ok(args.channel === null || Number.isInteger(args.channel));
});

test('wifi.add の要求行は純 ASCII の 1 行になる (日本語 SSID / 制御文字)', () => {
  const line = buildRequest(2, CMD.WIFI_ADD,
    buildWifiAddArgs({ ssid: 'わが家', password: 'a\x03bcdefgh' }));
  assert.equal(line.indexOf('\x03'), -1);
  assert.equal(line.indexOf('\n'), line.length - 1);
  const obj = JSON.parse(line.slice(1, -1));
  assert.equal(obj.cmd, 'wifi.add');
  assert.equal(obj.ssid, 'わが家');
  assert.equal(obj.password, 'a\x03bcdefgh');
});

test('wifi.remove は ssid だけを送る', () => {
  assert.deepEqual(buildWifiRemoveArgs('home-ap'), { ssid: 'home-ap' });
  assert.deepEqual(buildWifiRemoveArgs(null), { ssid: '' });
  assert.equal(buildRequest(3, CMD.WIFI_REMOVE, buildWifiRemoveArgs('ap')),
    PROTOCOL.REQ_PREFIX + '{"id":3,"cmd":"wifi.remove","ssid":"ap"}\n');
});

test('★ wifi.add / wifi.remove のタイムアウトは 10 秒以上 (打鍵ガードのぶん)', () => {
  // デバイスは書き込み前に無打鍵 300 ms を待ち、最大 3 秒で諦めて実行する。
  assert.ok(TIMEOUT_MS[CMD.WIFI_ADD] >= 10000);
  assert.ok(TIMEOUT_MS[CMD.WIFI_REMOVE] >= 10000);
});

// ===========================================================================
// settings.set の引数 (音声サーバだけになった)
// ===========================================================================

test('settings.set が送るのは STACKEE_HOST / STACKEE_PORT の 2 つだけ', () => {
  // proto 2 のデバイスは STACKEE_WIFI_* を "denied:<KEY>" で拒否する
  const kv = buildSettingsArgs({ host: '192.168.1.10', port: '5555' }).kv;
  assert.deepEqual(Object.keys(kv).sort(), ['STACKEE_HOST', 'STACKEE_PORT']);
  for (const key of LEGACY_WIFI_KEYS) {
    assert.equal(key in kv, false, key);
  }
});

test('値は必ず文字列になる (デバイスは notstr で拒否する)', () => {
  const kv = buildSettingsArgs({ host: ' h ', port: 5555 }).kv;
  for (const [k, v] of Object.entries(kv)) {
    assert.equal(typeof v, 'string', k + ' = ' + typeof v);
  }
  assert.equal(kv[SETTING_KEYS.HOST], 'h');     // 前後の空白は落とす
  assert.equal(kv[SETTING_KEYS.PORT], '5555');
});

// ===========================================================================
// wifi.list の応答
// ===========================================================================

test('wifi.list を一覧として読む', () => {
  const list = readNetworkList({
    id: 1,
    networks: [
      { ssid: 'home-ap', channel: 10, has_password: true },
      { ssid: 'open-ap', channel: null, has_password: false },
    ],
    n: 2,
  });
  assert.equal(list.count, 2);
  assert.deepEqual(list.networks, [
    { ssid: 'home-ap', channel: 10, hasPassword: true },
    { ssid: 'open-ap', channel: null, hasPassword: false },
  ]);
});

test('★ wifi.list の応答にパスワードは入らない (入っていても持ち出さない)', () => {
  // デバイスは値を返さない設計。仮に返ってきても、読み取り結果に出さない。
  const list = readNetworkList({
    networks: [{ ssid: 'ap', channel: 6, has_password: true, password: 'leaked!!' }],
    n: 1,
  });
  assert.deepEqual(Object.keys(list.networks[0]).sort(), ['channel', 'hasPassword', 'ssid']);
  assert.equal(JSON.stringify(list).indexOf('leaked'), -1);
});

test('チャネル無しは null (画面では「自動」)', () => {
  const list = readNetworkList({ networks: [{ ssid: 'a' }, { ssid: 'b', channel: '6' }] });
  assert.equal(list.networks[0].channel, null);
  assert.equal(list.networks[1].channel, null);   // 文字列は整数ではないので null
});

test('壊れた要素・空の SSID は落とし、n が無ければ数える', () => {
  const list = readNetworkList({
    networks: [null, 'x', { ssid: '' }, { ssid: 'ok', has_password: 1 }],
  });
  assert.equal(list.networks.length, 1);
  assert.equal(list.networks[0].ssid, 'ok');
  assert.equal(list.networks[0].hasPassword, true);
  assert.equal(list.count, 1);
});

test('networks が無い / 応答が無くても落ちない', () => {
  assert.deepEqual(readNetworkList({ id: 1 }), { networks: [], count: 0 });
  assert.deepEqual(readNetworkList(null), { networks: [], count: 0 });
});

test('wifi.add / wifi.remove の応答から登録件数を読む', () => {
  assert.equal(readNetworkCount({ id: 1, ok: 1, n: 3 }), 3);
  assert.equal(readNetworkCount({ id: 1, ok: 1 }), null);
  assert.equal(readNetworkCount(null), null);
});

// ===========================================================================
// wifi.add / wifi.remove の失敗コード
// ===========================================================================

test('デバイスの失敗コードを読める日本語にする', () => {
  const cases = [
    [ERR.FULL, /8 件/],
    [ERR.BAD_SSID, /SSID/],
    [ERR.BAD_PASSWORD, /8〜63/],
    [ERR.BAD_CHANNEL, /1〜14/],
    [ERR.WRITE_FAILED, /wifi_networks\.json/],
    [ERR.NOT_FOUND, /登録されていません/],
  ];
  for (const [code, re] of cases) {
    assert.match(errorText(code, CMD.WIFI_ADD), re, code);
    // 「デバイス側でエラーが起きました (…)」のような素通しになっていないこと
    assert.equal(errorText(code, CMD.WIFI_ADD).indexOf(code), -1, code);
  }
});

test('失敗コードは Error になり、コードが読める', async () => {
  const tr = new RequestTracker({ now: () => 0 });
  const r = tr.create(CMD.WIFI_ADD);
  tr.onFrame({ id: r.id, error: ERR.FULL });
  await assert.rejects(r.promise, (e) => {
    assert.equal(e.code, ERR.FULL);
    assert.equal(e.cmd, CMD.WIFI_ADD);
    assert.equal(isUnsupported(e), false);   // 「未対応」ではない = 機能は隠さない
    return true;
  });
});

test('旧 Wi-Fi キーの denied は「wifi.add を使え」と案内する', () => {
  const msg = errorText('denied:STACKEE_WIFI_SSID', CMD.SETTINGS_SET);
  assert.match(msg, /STACKEE_WIFI_SSID/);
  assert.match(msg, /Wi-Fi ネットワーク/);
});

// ===========================================================================
// 応答の読み取り
// ===========================================================================

test('settings.get から音声サーバの 2 つを読む', () => {
  const s = readSettings({
    id: 1,
    keys: { STACKEE_HOST: '192.168.1.10', STACKEE_PORT: '5555' },
    secret: ['STACKEE_WIFI_PASSWORD'],
    bytes: 420,
  });
  assert.equal(s.host, '192.168.1.10');
  assert.equal(s.port, '5555');
  assert.equal(s.missing, false);
  assert.deepEqual(s.legacyWifiKeys, []);
  assert.deepEqual(s.bootSlowingKeys, []);
});

test('settings.toml が無い応答を missing として読む', () => {
  const s = readSettings({ id: 1, keys: {}, missing: 1 });
  assert.equal(s.missing, true);
  assert.equal(s.host, '');
  assert.equal(s.port, '');
});

test('設定済みのキーしか返らない実機の応答を、欠けたぶんを空欄として読む', () => {
  const s = readSettings({
    id: 1,
    keys: { STACKEE_HOST: '192.168.0.106' },
    secret: ['STACKEE_WIFI_PASSWORD', 'CIRCUITPY_WIFI_PASSWORD', 'CIRCUITPY_WEB_API_PASSWORD'],
    bytes: 1234,
  });
  assert.equal(s.host, '192.168.0.106');
  assert.equal(s.port, '');            // 欠けている = 空欄。エラーにしない
  assert.equal(s.missing, false);
  assert.deepEqual(s.secretKeys, [
    'STACKEE_WIFI_PASSWORD', 'CIRCUITPY_WIFI_PASSWORD', 'CIRCUITPY_WEB_API_PASSWORD',
  ]);
});

test('1 件だけだった頃の STACKEE_WIFI_* の残存を見つける (無視される旧設定)', () => {
  const s = readSettings({
    id: 1,
    keys: { STACKEE_WIFI_SSID: 'old-ap', STACKEE_WIFI_PASSWORD: true, STACKEE_HOST: 'h' },
  });
  assert.deepEqual(s.legacyWifiKeys, ['STACKEE_WIFI_SSID', 'STACKEE_WIFI_PASSWORD']);
  assert.deepEqual(s.bootSlowingKeys, []);
});

test('起動を遅くする CIRCUITPY_WIFI_SSID の残存を見つける', () => {
  const s = readSettings({ id: 1, keys: { CIRCUITPY_WIFI_SSID: 'old-ap' } });
  assert.deepEqual(s.bootSlowingKeys, ['CIRCUITPY_WIFI_SSID']);
});

test('真偽値で来た秘密キーを値として表示しない', () => {
  // keys の値が boolean なら、その中身は絶対に文字列化しない
  const s = readSettings({ id: 1, keys: { STACKEE_HOST: false } });
  assert.equal(s.host, '');
});

test('パスワードの有無は true/false/"***"/空文字/欠落のどれでも読める', () => {
  assert.equal(readPasswordState(true), true);
  assert.equal(readPasswordState(1), true);
  assert.equal(readPasswordState('***'), true);
  assert.equal(readPasswordState(false), false);
  assert.equal(readPasswordState(0), false);
  assert.equal(readPasswordState(''), false);
  assert.equal(readPasswordState(undefined), null);
});

test('hello を読む (proto 2 は対応コマンドの一覧を返す)', () => {
  const h = readHello({
    id: 1, proto: 2, fw: 'stackee-console/2', cp: '10.3.0', board: 'M5Stack CoreS3',
    features: ['wifi.list', 'wifi.add', 'wifi.remove', 'wifi.scan'],
  });
  assert.deepEqual(h, {
    version: 2,
    firmware: 'stackee-console/2',
    circuitpython: '10.3.0',
    board: 'M5Stack CoreS3',
    features: ['wifi.list', 'wifi.add', 'wifi.remove', 'wifi.scan'],
  });
  assert.deepEqual(readHello({}), {
    version: null, firmware: '', circuitpython: '', board: '', features: [],
  });
});

test('★ proto 1 (features を返さない) では複数 Wi-Fi の機能を隠す', () => {
  // 旧ファーム: 複数 Wi-Fi 非対応。ページはこの欄ごと出さない。
  const old = readHello({ id: 1, proto: 1, fw: 'stackee-console/1', cp: '10.3.0' });
  assert.deepEqual(old.features, []);
  assert.equal(supportsMultiWifi(old), false);
  assert.equal(helloSupports(old, CMD.WIFI_ADD), false);
  assert.equal(helloSupports(old, CMD.WIFI_SCAN), false);
  assert.equal(old.version, 1);
});

test('機能の有無は版番号ではなく features で決める', () => {
  // proto 2 と名乗っていても Wi-Fi を切ったビルドなら features に出ない
  const noWifi = readHello({ proto: PROTOCOL.VERSION_MULTI_WIFI, features: ['status'] });
  assert.equal(supportsMultiWifi(noWifi), false);

  // 逆に、版番号が分からなくても features にあれば使う
  const onlyFeatures = readHello({ features: ['wifi.list', 'wifi.add', 'wifi.remove'] });
  assert.equal(onlyFeatures.version, null);
  assert.equal(supportsMultiWifi(onlyFeatures), true);

  const now = readHello({ proto: 2, features: ['wifi.list', 'wifi.add', 'wifi.remove', 'wifi.scan'] });
  assert.equal(supportsMultiWifi(now), true);
  assert.equal(helloSupports(now, CMD.WIFI_REMOVE), true);
  assert.equal(helloSupports(now, 'wifi.forget'), false);
  assert.equal(helloSupports(null, CMD.WIFI_LIST), false);
});

test('ページが想定するプロトコル版は 2 (複数 Wi-Fi)', () => {
  assert.equal(PROTOCOL.VERSION, PROTOCOL.VERSION_MULTI_WIFI);
  assert.equal(PROTOCOL.VERSION, 2);
});

test('status を日本語の表示値にする', () => {
  const s = readStatus({
    id: 1, up: 3661.4, fw: 'stackee-console/1', bat: 83, chg: true,
    hid: 'BLE', ble: false, wifi: 'up', ip: '192.168.1.42',
  });
  assert.equal(s.battery, '83 %');
  assert.equal(s.charging, '充電中');
  assert.equal(s.hid, 'BLE');
  assert.equal(s.ble, '接続なし');
  assert.equal(s.wifi, '接続中 (192.168.1.42)');
  assert.equal(s.uptime, '01:01:01');
});

test('値が取れなかった項目は null (画面では —) になる', () => {
  const s = readStatus({ id: 1, bat: null, chg: null, hid: '?', ble: null, wifi: 'off' });
  assert.equal(s.battery, null);
  assert.equal(s.charging, null);
  assert.equal(s.hid, null);
  assert.equal(s.ble, null);
  assert.equal(s.uptime, null);
});

test('Wi-Fi の状態文字列を訳す', () => {
  assert.equal(formatWifi({ wifi: 'off' }), '無効');
  assert.equal(formatWifi({ wifi: 'on' }), '有効 (未接続)');
  assert.equal(formatWifi({ wifi: 'up', ip: '10.0.0.2' }), '接続中 (10.0.0.2)');
  assert.equal(formatWifi({ wifi: 'up' }), '接続中');
  assert.equal(formatWifi({ wifi: '?' }), '不明');
  assert.equal(formatWifi({}), null);
});

test('スキャン結果は 1 応答でも配列でも読める', () => {
  const one = readScanResults({
    id: 1, ok: 1, nets: [{ ssid: 'B', rssi: -40, ch: 6 }, { ssid: 'A', rssi: -70, ch: 1 }],
  });
  assert.deepEqual(one.map((n) => n.ssid), ['B', 'A']);
  assert.equal(one[0].channel, 6);
  assert.equal(one[0].secure, null);   // デバイスは暗号の有無を返さない
});

test('実機のスキャン応答を読む (知らない項目は無視する)', () => {
  // 実機の応答には settle_ms / settle_passive_ms / chs / t / total_ms が付く
  const nets = readScanResults({
    id: 1, ok: 1, naive: 0,
    chs: [6, 1, 11, 3, 9, 13, 2, 4, 8, 12, 5, 7, 10],
    settle_ms: 300,
    settle_passive_ms: 800,
    total_ms: 5090,
    t: { import_us: 7510, enable_us: 2320 },
    nets: [{ ssid: 'TP-Link_E640', ch: 10, rssi: -59 }],
  });
  assert.equal(nets.length, 1);
  assert.deepEqual(nets[0], { ssid: 'TP-Link_E640', rssi: -59, channel: 10, secure: null });
});

test('スキャン結果は SSID の重複を電波の強い方に畳んで並べ替える', () => {
  const nets = readScanResults([
    { nets: [{ ssid: 'A', rssi: -70, ch: 1 }, { ssid: 'B', rssi: -40, ch: 6 }] },
    { nets: [{ ssid: 'A', rssi: -50, ch: 11 }, { ssid: '', rssi: -30 }] },
  ]);
  assert.deepEqual(nets.map((n) => n.ssid), ['B', 'A']);
  assert.equal(nets[1].rssi, -50);
  assert.equal(nets[1].channel, 11);
});

test('nets が無い応答でも落ちない', () => {
  assert.deepEqual(readScanResults({ id: 1, ok: 1 }), []);
  assert.deepEqual(readScanResults(null), []);
  assert.deepEqual(readScanResults([]), []);
});

test('formatUptime', () => {
  assert.equal(formatUptime(0), '00:00:00');
  assert.equal(formatUptime(59.9), '00:00:59');
  assert.equal(formatUptime(3661), '01:01:01');
  assert.equal(formatUptime(90061), '1日 01:01:01');
});
