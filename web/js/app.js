// stackee 操作盤 — 画面の組み立てとイベント配線。
//
// プロトコルの知識はすべて protocol.js に、Web Serial の扱いは serial.js に置く。
// このファイルは「押されたら何を呼ぶか」「返ってきた値をどこに出すか」だけ。

import {
  CMD,
  MAX_WIFI_NETWORKS,
  PROTOCOL,
  SETTING_KEYS,
  WIFI_NETWORKS_PATH,
  buildSettingsArgs,
  buildWifiAddArgs,
  buildWifiRemoveArgs,
  isUnsupported,
  readHello,
  readNetworkCount,
  readNetworkList,
  readScanResults,
  readSettings,
  readStatus,
  supportsMultiWifi,
  validateSettings,
  validateWifiEntry,
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
    // 次に繋ぐのが別のファームかもしれないので、機能の判定はやり直す。
    multiWifiOk = false;
    knownSsids = new Set();
    $('wifi-multi').hidden = false;
    hide($('wifi-unsupported'));
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
  const hello = readHello(frame);
  if (frame) {
    if (hello.firmware) $('st-fwver').textContent = hello.firmware;
    $('st-cpver').textContent =
      (hello.circuitpython || '—') + (hello.board ? ' / ' + hello.board : '');
    // 自己確認: デバイスがページより新しいプロトコルなら、知らせるだけ知らせる。
    // (古い側は「複数 Wi-Fi 非対応」として Wi-Fi 欄で個別に案内する)
    if (hello.version !== null && hello.version > PROTOCOL.VERSION) {
      show($('proto-warning'),
        'デバイスのプロトコル版は ' + hello.version
        + '、このページは ' + PROTOCOL.VERSION + ' です。'
        + 'このページが古いので、表示されない項目があるかもしれません。');
    }
  }
  // hello に答えなかった場合は「非対応」と決めつけない (null を渡す)。
  applyWifiFeature(frame ? hello : null);
  startAutoStatus();
  await refreshStatus();
  await loadSettings();
  await refreshNetworks();
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
 *   wifi.add / wifi.remove も同じフラッシュ書き込みなので同じ扱いにする。
 */
let longJobBusy = false;

/** 長い仕事の間に押せなくするボタン。 */
const LONG_JOB_BUTTONS = [
  'btn-save', 'btn-load', 'btn-scan', 'btn-status', 'btn-reset',
  'btn-net-reload', 'btn-net-add',
];

function setLongJobBusy(busy) {
  longJobBusy = busy;
  for (const id of LONG_JOB_BUTTONS) {
    const el = $(id);
    if (el) el.disabled = busy;
  }
  // 一覧の「削除」は動的に作るので、まとめて拾う。
  for (const b of document.querySelectorAll('.btn-net-del')) b.disabled = busy;
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
// Wi-Fi ネットワーク (最大 8 件)
// ---------------------------------------------------------------------------

/** デバイスが複数 Wi-Fi (wifi.list / wifi.add / wifi.remove) を持っているか。 */
let multiWifiOk = false;

/**
 * 直近の wifi.list に入っていた SSID。
 * 「追加」なのか「上書き」なのかの言い分けにだけ使う。
 */
let knownSsids = new Set();

/** Wi-Fi の入力欄 (エラー表示の対応付け)。 */
const WIFI_FIELDS = ['ssid', 'password', 'channel'];
/** 音声サーバの入力欄。 */
const SERVER_FIELDS = ['host', 'port'];

function showFieldErrors(fields, errors) {
  for (const key of fields) {
    const el = $('err-' + key);
    if (!el) continue;
    if (errors[key]) show(el, errors[key]); else hide(el);
  }
}

/**
 * hello の features を見て、複数 Wi-Fi の画面を出すかどうかを決める。
 *
 * ★ 版番号ではなく features で判断する (protocol.js の supportsMultiWifi)。
 *   持っていないファームには、機能そのものを見せない。
 */
function applyWifiFeature(hello) {
  multiWifiOk = supportsMultiWifi(hello);
  $('wifi-multi').hidden = !multiWifiOk;
  if (multiWifiOk) {
    hide($('wifi-unsupported'));
    return;
  }
  knownSsids = new Set();
  if (hello == null) {
    // hello 自体に応答が無かった。非対応と決めつけない。
    show($('wifi-unsupported'),
      'デバイスが hello に答えないので、複数 Wi-Fi に対応しているか分かりません。'
      + '「接続する」を押し直すか、USB を挿し直してください。');
    return;
  }
  show($('wifi-unsupported'),
    '旧ファーム: 複数 Wi-Fi 非対応 — このデバイス (プロトコル '
    + (hello.version == null ? '不明' : hello.version)
    + ') は Wi-Fi を複数登録する機能を持っていません。'
    + 'ファームウェアを更新すると、この欄から最大 ' + MAX_WIFI_NETWORKS
    + ' 件まで登録できるようになります。');
}

$('btn-net-reload').addEventListener('click', () => { void refreshNetworks(); });

async function refreshNetworks() {
  if (!link.connected || !multiWifiOk || longJobBusy) return;
  try {
    renderNetworks(readNetworkList(await link.request(CMD.WIFI_LIST)));
    hide($('net-error'));
  } catch (e) {
    if (isUnsupported(e)) {
      // features には出ていたのに実際は使えなかった。機能ごと引っ込める。
      unsupportedCommands.add(CMD.WIFI_LIST);
      applyWifiFeature({ version: null, features: [] });
      return;
    }
    show($('net-error'), '登録済みの Wi-Fi を読み出せません: ' + e.message);
  }
}

/**
 * 一覧を表に描く。
 * ★ パスワードそのものは受け取っていないし、描かない。出すのは有無だけ。
 */
function renderNetworks(list) {
  knownSsids = new Set(list.networks.map((n) => n.ssid));
  $('net-count').textContent =
    '登録 ' + list.count + ' / ' + MAX_WIFI_NETWORKS + ' 件';
  $('net-table').hidden = list.networks.length === 0;
  $('net-empty').hidden = list.networks.length > 0;

  const rows = document.createDocumentFragment();
  for (const n of list.networks) {
    const tr = document.createElement('tr');
    const ssid = document.createElement('td');
    ssid.className = 'net-ssid';
    ssid.textContent = n.ssid;
    const ch = document.createElement('td');
    ch.className = 'net-ch';
    ch.textContent = n.channel == null ? '自動' : 'ch' + n.channel;
    const pw = document.createElement('td');
    pw.textContent = n.hasPassword ? '設定済み' : 'なし';
    const act = document.createElement('td');
    act.className = 'net-act';
    const del = document.createElement('button');
    del.type = 'button';
    del.className = 'btn-net-del';
    del.textContent = '削除';
    del.addEventListener('click', () => { void removeNetwork(n.ssid); });
    act.append(del);
    tr.append(ssid, ch, pw, act);
    rows.append(tr);
  }
  $('net-rows').replaceChildren(rows);
  // 今作ったボタンにも、今の「取り込み中」状態を反映させる。
  setLongJobBusy(longJobBusy);
}

$('btn-net-add').addEventListener('click', async () => {
  if (!link.connected || !multiWifiOk) return;
  hide($('net-error'));
  hide($('net-ok'));
  const form = {
    ssid: $('in-ssid').value.trim(),
    password: $('in-pw').value,
    channel: $('in-ch').value.trim(),
  };
  const { ok, errors } = validateWifiEntry(form);
  showFieldErrors(WIFI_FIELDS, errors);
  if (!ok) return;

  // 「上書きしました」と言えるかは、送る前の一覧で決める。
  const overwrite = knownSsids.has(form.ssid);
  setLongJobBusy(true);
  $('net-state').textContent =
    '書き込み中… (デバイスが打鍵の切れ目を待つので、打っていると数秒かかります)';
  try {
    // ★ 送る中身はログに出さない (パスワードが混ざる)。
    const n = readNetworkCount(await link.request(CMD.WIFI_ADD, buildWifiAddArgs(form)));
    const count = n == null ? '' : ' (登録 ' + n + ' / ' + MAX_WIFI_NETWORKS + ' 件)';
    show($('net-ok'),
      (overwrite ? '上書きしました' : '追加しました') + '「' + form.ssid + '」' + count
      + '。自動接続は今後のファーム更新で有効になります。');
    // 入力欄からパスワードを消す。ページには一切残さない。
    $('in-pw').value = '';
    $('in-ssid').value = '';
    $('in-ch').value = '';
    showFieldErrors(WIFI_FIELDS, {});
  } catch (e) {
    show($('net-error'), '登録できませんでした: ' + e.message);
  } finally {
    $('net-state').textContent = '';
    setLongJobBusy(false);
  }
  await refreshNetworks();
});

async function removeNetwork(ssid) {
  if (!link.connected || !multiWifiOk || longJobBusy) return;
  if (!window.confirm('「' + ssid + '」の登録を消します。よろしいですか?')) return;
  hide($('net-error'));
  hide($('net-ok'));
  setLongJobBusy(true);
  $('net-state').textContent = '削除中…';
  try {
    await link.request(CMD.WIFI_REMOVE, buildWifiRemoveArgs(ssid));
    show($('net-ok'), '「' + ssid + '」を削除しました。');
  } catch (e) {
    show($('net-error'), '削除できませんでした: ' + e.message);
  } finally {
    $('net-state').textContent = '';
    setLongJobBusy(false);
  }
  await refreshNetworks();
}

$('btn-pw-toggle').addEventListener('click', () => {
  const inp = $('in-pw');
  const showing = inp.type === 'text';
  inp.type = showing ? 'password' : 'text';
  $('btn-pw-toggle').textContent = showing ? '表示' : '隠す';
  $('btn-pw-toggle').setAttribute('aria-pressed', String(!showing));
});

// ---------------------------------------------------------------------------
// 音声サーバ (STACKEE_HOST / STACKEE_PORT のみ)
// ---------------------------------------------------------------------------

$('btn-load').addEventListener('click', () => { void loadSettings(); });

async function loadSettings() {
  if (!link.connected) return;
  try {
    const cur = readSettings(await link.request(CMD.SETTINGS_GET));
    $('in-host').value = cur.host;
    $('in-port').value = cur.port;
    hide($('srv-error'));
    const notes = [];
    if (cur.missing) {
      notes.push('デバイスに settings.toml がまだありません。保存すると作られます。');
    }
    if (cur.legacyWifiKeys.length) {
      notes.push('settings.toml に ' + cur.legacyWifiKeys.join(' / ')
        + ' が残っています。これは 1 件だけ Wi-Fi を書いていた頃の設定で、'
        + '今のファームは読みません。書き換えもできません (消すのは手作業です)。');
    }
    if (cur.bootSlowingKeys.length) {
      // wifi_autoconnect_design.md §1.1 の実測: AP 不在の場所で起動が 19 秒延びる
      notes.push('settings.toml に ' + cur.bootSlowingKeys.join(' / ')
        + ' が残っています。これがあると Wi-Fi が無い場所で起動が最大 19 秒遅くなります。'
        + '手で削除することをおすすめします (このページからは消せません)。');
    }
    if (notes.length) show($('srv-note'), notes.join(' ')); else hide($('srv-note'));
  } catch (e) {
    if (isUnsupported(e)) unsupportedCommands.add(CMD.SETTINGS_GET);
    show($('srv-error'), isUnsupported(e) ? e.message : '設定を読み出せません: ' + e.message);
  }
}

$('btn-save').addEventListener('click', async () => {
  if (!link.connected) return;
  hide($('srv-error'));
  hide($('srv-ok'));
  const form = { host: $('in-host').value.trim(), port: $('in-port').value.trim() };
  const { ok, errors } = validateSettings(form);
  showFieldErrors(SERVER_FIELDS, errors);
  if (!ok) return;

  setLongJobBusy(true);
  $('save-state').textContent =
    '保存中… (デバイスが打鍵の切れ目を待つので、打っていると数秒かかります)';
  try {
    await link.request(CMD.SETTINGS_SET, buildSettingsArgs(form));
    // 書けたことをデバイスに読み直させて確かめる。
    const cur = readSettings(await link.request(CMD.SETTINGS_GET));
    const mismatch = [];
    if (cur.host !== form.host) mismatch.push(SETTING_KEYS.HOST);
    if (cur.port !== form.port) mismatch.push(SETTING_KEYS.PORT);
    if (mismatch.length) {
      show($('srv-error'), '保存したはずの値がデバイス側と一致しません: ' + mismatch.join(', '));
    } else {
      show($('srv-ok'), '保存しました。反映するには再起動してください。');
    }
    $('save-state').textContent = '';
  } catch (e) {
    $('save-state').textContent = '';
    show($('srv-error'), isUnsupported(e) ? e.message : '保存に失敗しました: ' + e.message);
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
    $('scan-note').textContent =
      nets.length + ' 件見つかりました (電波の強い順)。選ぶと SSID とチャネルが入ります。';
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
  // ★ 生の値は protocol.js の 1 か所だけに置く (README「プロトコル定数の置き場所」)。
  $('wifi-path').textContent = WIFI_NETWORKS_PATH;
  const hex = (n) => n.toString(16).toUpperCase().padStart(4, '0');
  $('conn-detail').textContent = '対象: VID ' + hex(USB_FILTER.usbVendorId)
    + ' / PID ' + hex(USB_FILTER.usbProductId) + ' (M5Stack CoreS3)';
  renderLog();
}

boot();
