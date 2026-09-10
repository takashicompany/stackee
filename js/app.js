// stackee 操作盤 — 画面の組み立てとイベント配線。
//
// プロトコルの知識はすべて protocol.js に、Web Serial の扱いは serial.js に置く。
// このファイルは「押されたら何を呼ぶか」「返ってきた値をどこに出すか」だけ。

import {
  CMD,
  PROTOCOL,
  SETTING_KEYS,
  isUnsupported,
  readHello,
  readScanResults,
  readSettings,
  readStatus,
  validateSettings,
  buildSettingsArgs,
} from './protocol.js';
import { StackeeSerial, unsupportedReason, USB_FILTER } from './serial.js';

const $ = (id) => document.getElementById(id);

// ---------------------------------------------------------------------------
// ログ表示
// ---------------------------------------------------------------------------

/** ログに残す最大行数。これを超えたら古い方から捨てる。 */
const LOG_MAX_LINES = 2000;

/**
 * KMK のキーイベント系のデバッグ行を見分ける当て推量。
 * デバイス側のデバッグ出力の書式が変わったらここを直す。
 */
const KMK_KEY_EVENT_RE =
  /\b(?:matrix|keyevent|key_event|hid_send|hid report|hidreport|process_key|resume\.|add_key|remove_key|keys_pressed|hid_pending|_send_hid|Keycode\()/i;

const logState = {
  lines: [],
  partial: '',
  paused: false,
  pendingWhilePaused: '',
  filterKmk: false,
};

function logAppend(text) {
  if (logState.paused) {
    // 一時停止中も取りこぼさない。上限を超えたぶんは頭から捨てる。
    logState.pendingWhilePaused += text;
    if (logState.pendingWhilePaused.length > 200000) {
      logState.pendingWhilePaused = logState.pendingWhilePaused.slice(-200000);
    }
    return;
  }
  ingest(text);
}

function ingest(text) {
  const buf = logState.partial + text.replace(/\r\n/g, '\n').replace(/\r/g, '\n');
  const parts = buf.split('\n');
  logState.partial = parts.pop();
  for (const line of parts) logState.lines.push(line);
  if (logState.lines.length > LOG_MAX_LINES) {
    logState.lines.splice(0, logState.lines.length - LOG_MAX_LINES);
  }
  scheduleRender();
}

let renderQueued = false;
function scheduleRender() {
  if (renderQueued) return;
  renderQueued = true;
  requestAnimationFrame(() => {
    renderQueued = false;
    renderLog();
  });
}

function renderLog() {
  const el = $('log');
  const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  const shown = logState.filterKmk
    ? logState.lines.filter((l) => !KMK_KEY_EVENT_RE.test(l))
    : logState.lines;
  el.textContent = shown.join('\n') + (logState.partial ? '\n' + logState.partial : '');
  $('log-count').textContent =
    logState.lines.length + ' 行' +
    (logState.filterKmk ? ' (表示 ' + shown.length + ' 行)' : '') +
    (logState.paused ? ' / 一時停止中' : '');
  if (atBottom) el.scrollTop = el.scrollHeight;
}

// ---------------------------------------------------------------------------
// 接続
// ---------------------------------------------------------------------------

const link = new StackeeSerial({
  onLog: logAppend,
  onTitle: (t) => { $('statusbar-text').textContent = t || '—'; },
  onState: onConnState,
  onFrameError: (e) => {
    // 壊れた枠はログに 1 行だけ残す。本文には混ぜない。
    logAppend('[操作盤] 応答の解釈に失敗 (' + e.reason + ')\n');
  },
  onStray: (f) => {
    logAppend('[操作盤] 対応する要求の無い応答: ' + JSON.stringify(f) + '\n');
  },
});

/**
 * このファームで使えないと分かったコマンド。
 * hello は対応コマンドの一覧を返さないので、実際に呼んで
 * "unknown:<cmd>" / "unsupported" が返ったものをここに覚える。
 */
const unsupportedCommands = new Set();

function onConnState(state, info) {
  const badge = $('conn-state');
  const detail = $('conn-detail');
  badge.className = 'badge ' + (state === 'connected' ? 'badge-on' : state === 'connecting' ? 'badge-wait' : 'badge-off');
  badge.textContent = state === 'connected' ? '接続中' : state === 'connecting' ? '接続しています…' : '未接続';
  detail.textContent = info || '';
  $('btn-connect').hidden = state === 'connected';
  $('btn-disconnect').hidden = state !== 'connected';
  $('btn-connect').disabled = state === 'connecting';
  for (const sec of document.querySelectorAll('.needs-conn')) {
    sec.classList.toggle('locked', state !== 'connected');
  }
  if (state === 'connected') {
    logAppend('[操作盤] 接続しました\n');
    void afterConnect();
  } else if (state === 'disconnected') {
    stopAutoStatus();
    unsupportedCommands.clear();
    $('btn-scan').hidden = false;
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * 起動直後のデバイスは 3 秒ほどコマンドに答えない
 * (実測: ポートが開けるのが 5.25 秒、status が返るのが 8.51 秒)。
 * だから hello は短いタイムアウトで何度か叩き直す。
 * @returns {Promise<object|null>} 応答。諦めたら null
 */
async function probeHello(attempts = 6, gapMs = 500) {
  for (let i = 0; i < attempts; i += 1) {
    if (!link.connected) return null;
    try {
      return await link.request(CMD.HELLO, null, { timeoutMs: 1500 });
    } catch (e) {
      if (i === attempts - 1) {
        logAppend('[操作盤] hello に応答がありません: ' + e.message + '\n');
        return null;
      }
      await sleep(gapMs);
    }
  }
  return null;
}

async function afterConnect() {
  hide($('proto-warning'));
  const frame = await probeHello();
  if (frame) {
    const hello = readHello(frame);
    if (hello.firmware) $('st-fwver').textContent = hello.firmware;
    $('st-cpver').textContent =
      (hello.circuitpython || '—') + (hello.board ? ' / ' + hello.board : '');
    // 自己確認: ページとファームのプロトコル版が違えば目に見える形で知らせる。
    if (hello.version !== PROTOCOL.VERSION) {
      show($('proto-warning'),
        'デバイスのプロトコル版は ' + (hello.version === null ? '不明' : hello.version)
        + '、このページは ' + PROTOCOL.VERSION + ' です。'
        + '表示されない項目や、保存できない設定があるかもしれません。'
        + 'ファームウェアかこのページのどちらかを更新してください。');
    }
  }
  startAutoStatus();
  await refreshStatus();
  await loadSettings();
}

$('btn-connect').addEventListener('click', async () => {
  // ★ requestPort() は transient activation が要るので、await を挟む前に呼ぶ。
  try {
    await link.connect();
  } catch (e) {
    if (e && e.name === 'NotFoundError') {
      $('conn-detail').textContent = 'デバイスが選ばれませんでした。';
      return;
    }
    $('conn-detail').textContent = '接続に失敗しました: ' + (e && e.message);
  }
});

$('btn-disconnect').addEventListener('click', async () => {
  await link.disconnect();
  logAppend('[操作盤] 切断しました\n');
});

// ---------------------------------------------------------------------------
// 状態
// ---------------------------------------------------------------------------

const AUTO_STATUS_MS = 10000;
let autoStatusTimer = null;

function startAutoStatus() {
  stopAutoStatus();
  if (!$('chk-autostatus').checked) return;
  autoStatusTimer = setInterval(() => { void refreshStatus(); }, AUTO_STATUS_MS);
}

function stopAutoStatus() {
  if (autoStatusTimer) clearInterval(autoStatusTimer);
  autoStatusTimer = null;
}

$('chk-autostatus').addEventListener('change', () => {
  if (link.connected) startAutoStatus(); else stopAutoStatus();
});
$('btn-status').addEventListener('click', () => { void refreshStatus(); });

let statusBusy = false;

/**
 * 時間のかかる仕事 (settings.set / wifi.scan) が動いている間は true。
 *
 * デバイスはこの間シリアルを読まない (stackee_console.py の after_matrix_scan は
 * 仕事が残っていると _next_line() を呼ばずに戻る)。要求は捨てられずに溜まるが、
 * ページ側のタイムアウトには間に合わない。だから自動更新を止め、
 * ほかのボタンも押せなくする。
 *   実測: settings.set = 224 ms + 打鍵ガード最大 3 秒 / wifi.scan = 5.09 秒
 */
let longJobBusy = false;

/** 長い仕事の間に押せなくするボタン。 */
const LONG_JOB_BUTTONS = ['btn-save', 'btn-load', 'btn-scan', 'btn-status', 'btn-reset'];

function setLongJobBusy(busy) {
  longJobBusy = busy;
  for (const id of LONG_JOB_BUTTONS) $(id).disabled = busy;
}

async function refreshStatus() {
  if (!link.connected || statusBusy || longJobBusy) return;
  statusBusy = true;
  try {
    const f = await link.request(CMD.STATUS);
    showStatus(f);
    hide($('status-error'));
    $('status-updated').textContent = '最終更新 ' + new Date().toLocaleTimeString('ja-JP');
  } catch (e) {
    show($('status-error'), '状態を取得できません: ' + e.message);
  } finally {
    statusBusy = false;
  }
}

function showStatus(frame) {
  const s = readStatus(frame);
  const setv = (id, v) => { $(id).textContent = v == null || v === '' ? '—' : String(v); };
  setv('st-bat', s.battery);
  setv('st-chg', s.charging);
  setv('st-hid', s.hid);
  setv('st-ble', s.ble);
  setv('st-wifi', s.wifi);
  setv('st-up', s.uptime);
  if (s.firmware) setv('st-fwver', s.firmware);
}

// ---------------------------------------------------------------------------
// Wi-Fi と音声サーバ
// ---------------------------------------------------------------------------

const FIELD_IDS = {
  ssid: 'in-ssid',
  password: 'in-pw',
  channel: 'in-ch',
  host: 'in-host',
  port: 'in-port',
};

function readForm() {
  return {
    ssid: $('in-ssid').value.trim(),
    password: $('in-pw').value,
    channel: $('in-ch').value.trim(),
    host: $('in-host').value.trim(),
    port: $('in-port').value.trim(),
    clearPassword: $('chk-pw-clear').checked,
  };
}

function showFieldErrors(errors) {
  for (const key of Object.keys(FIELD_IDS)) {
    const el = $('err-' + key);
    if (!el) continue;
    if (errors[key]) show(el, errors[key]); else hide(el);
  }
}

$('btn-pw-toggle').addEventListener('click', () => {
  const inp = $('in-pw');
  const showing = inp.type === 'text';
  inp.type = showing ? 'password' : 'text';
  $('btn-pw-toggle').textContent = showing ? '表示' : '隠す';
  $('btn-pw-toggle').setAttribute('aria-pressed', String(!showing));
});

$('btn-load').addEventListener('click', () => { void loadSettings(); });

function showPasswordState(state) {
  $('pw-state').textContent =
    state === true ? '設定済み' : state === false ? '未設定' : '不明';
}

async function loadSettings() {
  if (!link.connected) return;
  try {
    const cur = readSettings(await link.request(CMD.SETTINGS_GET));
    $('in-ssid').value = cur.ssid;
    $('in-ch').value = cur.channel;
    $('in-host').value = cur.host;
    $('in-port').value = cur.port;
    showPasswordState(cur.passwordSet);
    hide($('wifi-error'));
    const notes = [];
    if (cur.missing) {
      notes.push('デバイスに settings.toml がまだありません。保存すると作られます。');
    }
    if (cur.legacyWifiKey) {
      // wifi_autoconnect_design.md §1.1 の実測: AP 不在の場所で起動が 19 秒延びる
      notes.push('settings.toml に CIRCUITPY_WIFI_SSID が残っています。'
        + 'これがあると Wi-Fi が無い場所で起動が最大 19 秒遅くなります。'
        + '手で削除することをおすすめします (このページからは消せません)。');
    }
    if (notes.length) show($('wifi-note'), notes.join(' ')); else hide($('wifi-note'));
  } catch (e) {
    if (isUnsupported(e)) unsupportedCommands.add(CMD.SETTINGS_GET);
    show($('wifi-error'), isUnsupported(e) ? e.message : '設定を読み出せません: ' + e.message);
  }
}

$('btn-save').addEventListener('click', async () => {
  if (!link.connected) return;
  hide($('wifi-error'));
  hide($('wifi-ok'));
  const form = readForm();
  const { ok, errors } = validateSettings(form);
  showFieldErrors(errors);
  if (!ok) return;

  setLongJobBusy(true);
  $('save-state').textContent =
    '保存中… (デバイスが打鍵の切れ目を待つので、打っていると数秒かかります)';
  try {
    // ★ 保存の中身はログに出さない (パスワードが混ざる)。
    await link.request(CMD.SETTINGS_SET, buildSettingsArgs(form));
    // 書けたことをデバイスに読み直させて確かめる。
    const cur = readSettings(await link.request(CMD.SETTINGS_GET));
    showPasswordState(cur.passwordSet);
    const mismatch = [];
    if (cur.ssid !== form.ssid) mismatch.push(SETTING_KEYS.SSID);
    if (cur.host !== form.host) mismatch.push(SETTING_KEYS.HOST);
    if (cur.port !== form.port) mismatch.push(SETTING_KEYS.PORT);
    if (mismatch.length) {
      show($('wifi-error'), '保存したはずの値がデバイス側と一致しません: ' + mismatch.join(', '));
    } else {
      show($('wifi-ok'), '保存しました。反映するには再起動してください。');
    }
    // 入力欄からパスワードを消す。ページには一切残さない。
    $('in-pw').value = '';
    $('chk-pw-clear').checked = false;
    $('save-state').textContent = '';
  } catch (e) {
    $('save-state').textContent = '';
    show($('wifi-error'), isUnsupported(e) ? e.message : '保存に失敗しました: ' + e.message);
  } finally {
    setLongJobBusy(false);
  }
});

// --- Wi-Fi スキャン ---

$('btn-scan').addEventListener('click', async () => {
  if (!link.connected) return;
  const btn = $('btn-scan');
  setLongJobBusy(true);
  $('scan-result').hidden = false;
  $('scan-list').replaceChildren();
  $('scan-note').textContent = 'スキャン中… (13 チャネルを順に見るので 5 秒ほどかかります)';
  try {
    // デバイスは全チャネル分をまとめて 1 応答で返す。その間もキーボードは止まらない。
    const nets = readScanResults(await link.request(CMD.WIFI_SCAN));
    if (nets.length === 0) {
      $('scan-note').textContent = 'アクセスポイントが見つかりませんでした。SSID は手入力もできます。';
      return;
    }
    $('scan-note').textContent = nets.length + ' 件見つかりました。選ぶと SSID 欄に入ります。';
    const frag = document.createDocumentFragment();
    for (const n of nets) {
      const li = document.createElement('li');
      const b = document.createElement('button');
      b.type = 'button';
      const s = document.createElement('span');
      s.className = 'scan-ssid';
      s.textContent = n.ssid;
      const m = document.createElement('span');
      m.className = 'scan-meta';
      // デバイスは暗号の有無を返さないので、チャネルと電波の強さだけ出す。
      const bits = [];
      if (n.channel != null) bits.push('ch' + n.channel);
      if (n.rssi != null) bits.push(n.rssi + ' dBm');
      m.textContent = bits.join(' / ');
      b.append(s, m);
      b.addEventListener('click', () => {
        $('in-ssid').value = n.ssid;
        if (n.channel != null) $('in-ch').value = String(n.channel);
        hide($('err-ssid'));
      });
      li.append(b);
      frag.append(li);
    }
    $('scan-list').append(frag);
  } catch (e) {
    if (isUnsupported(e)) {
      unsupportedCommands.add(CMD.WIFI_SCAN);
      $('scan-note').textContent =
        'このデバイスでは Wi-Fi スキャンを使えません (' + e.message + ')。'
        + 'SSID を手で入力してください。';
      btn.hidden = true;
      return;
    }
    $('scan-note').textContent = 'スキャンに失敗しました: ' + e.message;
  } finally {
    setLongJobBusy(false);
  }
});

// ---------------------------------------------------------------------------
// 再起動
// ---------------------------------------------------------------------------

$('btn-reset').addEventListener('click', () => { void doReset(); });

async function doReset() {
  if (!link.connected) return;
  if (!window.confirm(
    'stackee を再起動します。\n'
    + 'キーボードとしても数秒間つながらなくなります。よろしいですか?',
  )) return;

  hide($('reset-error'));
  stopAutoStatus();
  setLongJobBusy(true);
  $('reset-state').textContent = '再起動を指示しました…';
  try {
    // デバイスは {"ok":1,"in_ms":300} を返してから 300 ms 後に落ちる。
    await link.request(CMD.RESET);
  } catch (e) {
    // 応答より先にリセットが走ることもある。タイムアウトは失敗扱いにしない。
    logAppend('[操作盤] reset の応答なし (' + e.message + ')。そのまま再接続を試みます。\n');
  }
  // 再起動でポートが落ちる。こちらから閉じておく。
  try { await link.disconnect(); } catch (e) { /* 無視 */ }

  // 実測: 1.7 秒でポートが消え、5.2 秒で現れ、8.5 秒で status が返る。
  // 消えるのを待ってから現れるのを待つ (現れる前に open() しに行かない)。
  const ok = await link.waitAndReconnect({
    timeoutMs: 30000,
    onTick: (remain, phase) => {
      const what = phase === 'vanish'
        ? 'デバイスが切れるのを待っています'
        : 'デバイスが出てくるのを待っています';
      $('reset-state').textContent = what + '… 残り ' + remain + ' 秒';
    },
  });
  setLongJobBusy(false);
  if (ok) {
    // 開けてもデバイスはまだ起動中のことがある。afterConnect() が
    // hello を短い間隔で叩き直すので、ここでは待つだけでよい。
    $('reset-state').textContent = '再接続しました。';
  } else {
    $('reset-state').textContent = '';
    show($('reset-error'),
      '30 秒待っても再接続できませんでした。USB を挿し直してから「接続する」を押してください。');
  }
}

// ---------------------------------------------------------------------------
// ログの操作
// ---------------------------------------------------------------------------

$('btn-log-pause').addEventListener('click', () => {
  logState.paused = !logState.paused;
  const b = $('btn-log-pause');
  b.textContent = logState.paused ? '再開' : '一時停止';
  b.setAttribute('aria-pressed', String(logState.paused));
  if (!logState.paused && logState.pendingWhilePaused) {
    const held = logState.pendingWhilePaused;
    logState.pendingWhilePaused = '';
    ingest(held);
  }
  renderLog();
});

$('btn-log-clear').addEventListener('click', () => {
  logState.lines = [];
  logState.partial = '';
  logState.pendingWhilePaused = '';
  renderLog();
});

$('chk-filter-kmk').addEventListener('change', (ev) => {
  logState.filterKmk = ev.target.checked;
  renderLog();
});

// ---------------------------------------------------------------------------
// 起動時
// ---------------------------------------------------------------------------

function show(el, text) {
  if (text != null) el.textContent = text;
  el.hidden = false;
}
function hide(el) { el.hidden = true; }

function boot() {
  const reason = unsupportedReason();
  if (reason) {
    show($('browser-warning'), reason);
    $('btn-connect').disabled = true;
  }
  for (const sec of document.querySelectorAll('.needs-conn')) sec.classList.add('locked');
  const hex = (n) => n.toString(16).toUpperCase().padStart(4, '0');
  $('conn-detail').textContent = '対象: VID ' + hex(USB_FILTER.usbVendorId)
    + ' / PID ' + hex(USB_FILTER.usbProductId) + ' (M5Stack CoreS3)';
  renderLog();
}

boot();
