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
  PROTOCOL,
  RequestTracker,
  SETTING_KEYS,
  buildRequest,
  buildSettingsArgs,
  byteLength,
  errorHead,
  formatUptime,
  formatWifi,
  isUnsupported,
  readHello,
  readPasswordState,
  readScanResults,
  readSettings,
  readStatus,
  validateChannel,
  validatePassword,
  validatePort,
  validateSettings,
  validateSsid,
} from '../js/protocol.js';

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
  const line = buildRequest(3, CMD.SETTINGS_SET, { kv: { [SETTING_KEYS.SSID]: 'わが家' } });
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

test('validateSettings は問題のあるフィールド名を返す', () => {
  const bad = validateSettings({ ssid: '', password: 'abc', channel: '99', host: '', port: '0' });
  assert.equal(bad.ok, false);
  assert.deepEqual(Object.keys(bad.errors).sort(), ['channel', 'host', 'password', 'port', 'ssid']);

  const good = validateSettings({
    ssid: 'home-ap', password: '', channel: '', host: '192.168.1.10', port: '5555',
  });
  assert.equal(good.ok, true);
  assert.deepEqual(good.errors, {});
});

// ===========================================================================
// settings.set の引数
// ===========================================================================

test('空のパスワードはキーごと省く (今の値を保つ)', () => {
  const a = buildSettingsArgs({ ssid: 'ap', password: '', host: 'h', port: '1' });
  assert.equal(SETTING_KEYS.PASSWORD in a.kv, false);
  assert.equal(a.kv[SETTING_KEYS.SSID], 'ap');
});

test('パスワードの明示的な消去は null (= 行を消す) を送る', () => {
  const c = buildSettingsArgs({ ssid: 'ap', password: '', host: 'h', port: '1', clearPassword: true });
  assert.equal(c.kv[SETTING_KEYS.PASSWORD], null);
});

test('チャネル未指定は null (= 行を消す)、指定ありは文字列', () => {
  assert.equal(buildSettingsArgs({ ssid: 'a', host: 'h', port: '1' }).kv[SETTING_KEYS.CHANNEL], null);
  assert.equal(buildSettingsArgs({ ssid: 'a', channel: '  ', host: 'h', port: '1' }).kv[SETTING_KEYS.CHANNEL], null);
  assert.equal(buildSettingsArgs({ ssid: 'a', channel: 6, host: 'h', port: '1' }).kv[SETTING_KEYS.CHANNEL], '6');
});

test('値は必ず文字列か null になる (デバイスは notstr で拒否する)', () => {
  const kv = buildSettingsArgs({ ssid: 'ap', password: 'secret12', channel: 6, host: 'h', port: 5555 }).kv;
  for (const [k, v] of Object.entries(kv)) {
    assert.ok(v === null || typeof v === 'string', k + ' = ' + typeof v);
  }
  assert.equal(kv[SETTING_KEYS.PORT], '5555');
  assert.equal(kv[SETTING_KEYS.PASSWORD], 'secret12');
});

test('書き換え対象は ALLOWED_KEYS の 5 つだけ (CIRCUITPY_* を送らない)', () => {
  const kv = buildSettingsArgs({ ssid: 'a', password: 'secret12', channel: '6', host: 'h', port: '1' }).kv;
  assert.deepEqual(Object.keys(kv).sort(), [
    'STACKEE_HOST', 'STACKEE_PORT', 'STACKEE_WIFI_CHANNEL',
    'STACKEE_WIFI_PASSWORD', 'STACKEE_WIFI_SSID',
  ]);
});

test('settings.set の引数はそのまま 1 行にできる', () => {
  const args = buildSettingsArgs({ ssid: 'わが家', password: 'secret12', host: 'h', port: '1' });
  const line = buildRequest(1, CMD.SETTINGS_SET, args);
  assert.equal(line.indexOf('\x03'), -1);
  assert.equal(JSON.parse(line.slice(1, -1)).kv[SETTING_KEYS.SSID], 'わが家');
});

// ===========================================================================
// 応答の読み取り
// ===========================================================================

test('settings.get は keys の中を読み、パスワードは有無だけを扱う', () => {
  const s = readSettings({
    id: 1,
    keys: {
      STACKEE_WIFI_SSID: 'home-ap',
      STACKEE_WIFI_CHANNEL: '6',
      STACKEE_HOST: '192.168.1.10',
      STACKEE_PORT: '5555',
      STACKEE_WIFI_PASSWORD: true,
    },
    secret: ['STACKEE_WIFI_PASSWORD'],
    bytes: 420,
  });
  assert.equal(s.ssid, 'home-ap');
  assert.equal(s.channel, '6');
  assert.equal(s.host, '192.168.1.10');
  assert.equal(s.port, '5555');
  assert.equal(s.passwordSet, true);
  assert.equal(s.missing, false);
  assert.equal(s.legacyWifiKey, false);
});

test('settings.toml が無い応答を missing として読む', () => {
  const s = readSettings({ id: 1, keys: {}, missing: 1 });
  assert.equal(s.missing, true);
  assert.equal(s.ssid, '');
  assert.equal(s.passwordSet, false);   // 応答は来ている = 未設定 (不明ではない)
});

test('設定済みのキーしか返らない実機の応答を、欠けたぶんを空欄として読む', () => {
  // 実機の応答 (2026-09-10): SSID もチャネルもパスワードも入っていない
  const s = readSettings({
    id: 1,
    keys: { STACKEE_HOST: '192.168.0.106', STACKEE_PORT: '5555' },
    secret: ['STACKEE_WIFI_PASSWORD', 'CIRCUITPY_WIFI_PASSWORD', 'CIRCUITPY_WEB_API_PASSWORD'],
    bytes: 1234,
  });
  assert.equal(s.host, '192.168.0.106');
  assert.equal(s.port, '5555');
  assert.equal(s.ssid, '');            // 欠けている = 空欄。エラーにしない
  assert.equal(s.channel, '');
  assert.equal(s.passwordSet, false);  // 欠けている = 未設定
  assert.equal(s.missing, false);
  assert.deepEqual(s.secretKeys, [
    'STACKEE_WIFI_PASSWORD', 'CIRCUITPY_WIFI_PASSWORD', 'CIRCUITPY_WEB_API_PASSWORD',
  ]);
});

test('秘密キーは真偽値でも伏せ字文字列でも「設定済み」になる', () => {
  const withBool = readSettings({ keys: { STACKEE_WIFI_PASSWORD: true } });
  assert.equal(withBool.passwordSet, true);
  const withMask = readSettings({ keys: { STACKEE_WIFI_PASSWORD: '***' } });
  assert.equal(withMask.passwordSet, true);
  const withFalse = readSettings({ keys: { STACKEE_WIFI_PASSWORD: false } });
  assert.equal(withFalse.passwordSet, false);
});

test('応答そのものが無ければ「不明」のまま (未設定と区別する)', () => {
  assert.equal(readSettings(null).passwordSet, null);
  assert.equal(readSettings({ id: 1 }).passwordSet, null);
});

test('起動を遅くする CIRCUITPY_WIFI_SSID の残存を見つける', () => {
  const s = readSettings({ id: 1, keys: { CIRCUITPY_WIFI_SSID: 'old-ap' } });
  assert.equal(s.legacyWifiKey, true);
});

test('真偽値で来た秘密キーを値として表示しない', () => {
  // keys の値が boolean なら、その中身は絶対に文字列化しない
  const s = readSettings({ id: 1, keys: { STACKEE_WIFI_SSID: false } });
  assert.equal(s.ssid, '');
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

test('hello を読む (対応コマンドの一覧は返らない)', () => {
  const h = readHello({ id: 1, proto: 1, fw: 'stackee-console/1', cp: '10.3.0', board: 'M5Stack CoreS3' });
  assert.deepEqual(h, {
    version: 1, firmware: 'stackee-console/1', circuitpython: '10.3.0', board: 'M5Stack CoreS3',
  });
  assert.deepEqual(readHello({}), { version: null, firmware: '', circuitpython: '', board: '' });
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
