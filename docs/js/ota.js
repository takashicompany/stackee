// stackee 操作盤 — アプリ内 OTA (ファームウェアの書き換え) の中核
//
// **運び方を一切知らない。** 転送層は呼び手が渡す (下の `link`)。
// 同じこのファイルを 2 つの入口が読む:
//
//   ブラウザ  docs/js/hid.js  (WebHID)          … 人が押すボタン
//   Node      firmware/tools/ota.mjs (node-hid) … AI が書き込むときの経路
//
// ★ ここを 1 つにしてあるのが肝。枠の組み立て・分割・credit・sha256 の
//   照合・ota.* の順序が 1 か所にしか無いので、**AI が通した道が人の道**に
//   なる。食い違いようがない。
//
// 方式の選定は research/stackee/web_flash_2026-09-20.md §2(B) / §6。
// 本体側の実装は firmware/main/stackee_otacore.c。**定数はそちらと一字一句
// そろっている必要がある。** 食い違ったら、まずここと向こうの #define を見る。

/** 本体の 0xC3 (OTA のバイナリ枠) まわりの定数。stackee_otacore.h と対。 */
export const OTA = {
  /** レポートの先頭バイト。0xC0/0xC1/0xC2 (JSON コンソール) と衝突しない。 */
  CMD_DATA: 0xc3,
  REPORT_SIZE: 32,
  /** [0]=id [1]=len [2..4]=位置 (24bit LE) */
  HEADER_SIZE: 5,
  /** 1 枠に載る本文。29 ではなく 27 なのは位置を毎枠に書くから。 */
  MAX_PAYLOAD: 27,
  /** 「書き終えた位置 + これ」まで先行して送ってよい。 */
  CREDIT: 32 * 1024,
  /** 本体が応答を返す間隔 (受け取った累積がこれを跨ぐたび)。 */
  ACK_EVERY: 1024,
  /** 本体の環状バッファ。credit の 2 倍あるので溢れない。 */
  RING: 64 * 1024,
  /** 応答の byte15 の bit0。この枠は受け取られなかった。 */
  FLAG_REJECTED: 0x01,
  /** 本体が無通信で諦めるまで (ms)。 */
  IDLE_MS: 30000,
};

/** 本体の stackee_ota_state_t と同じ並び。 */
export const OTA_STATE = ['idle', 'receiving', 'done', 'failed'];

/** 本体の stackee_ota_err_t と同じ並び。 */
export const OTA_ERR = [
  'none', 'busy', 'arg', 'nomem', 'flash_begin', 'offset', 'full',
  'flash_write', 'flash_end', 'sha', 'size', 'idle', 'state', 'magic',
];

/** 人に見せる言い方。 */
export const OTA_ERR_TEXT = {
  busy: '別の転送が進行中です。いったん中止してからやり直してください。',
  arg: '大きさか sha256 の渡し方がおかしいです。',
  nomem: 'デバイス側で受信バッファを用意できませんでした。',
  flash_begin: 'デバイスが書き込み先を開けませんでした。',
  offset: '送る位置がずれました。やり直してください。',
  full: 'デバイスの受信バッファが溢れました。やり直してください。',
  flash_write: 'フラッシュへの書き込みが失敗しました。',
  flash_end: '書き込みの締めが失敗しました。',
  sha: '受け取った像の sha256 が合いません (途中で化けました)。',
  size: '受け取った長さが合いません。',
  idle: '30 秒以上とぎれたので、デバイスが転送を打ち切りました。',
  state: '転送が始まっていません。',
  magic: 'ESP32 のファームウェア像ではありません (先頭が 0xE9 ではない)。',
};

// ---------------------------------------------------------------------------
// 枠の組み立てと読み取り (純関数。DOM にも USB にも触らない)
// ---------------------------------------------------------------------------

/**
 * 本文 1 つぶんの 0xC3 レポートを作る。
 *
 * ★ 位置を毎枠に書く。本体は「期待する位置と違えば捨てる」だけでよく、
 *   ホストは「本体が言う accepted から送り直す」だけでよい。
 *
 * @param {number} offset 像の先頭からの位置 (0 〜 0xFFFFFF)
 * @param {Uint8Array} chunk 27 バイトまで
 * @returns {Uint8Array} 32 バイト
 */
export function buildDataReport(offset, chunk) {
  const body = chunk || new Uint8Array(0);
  if (body.length > OTA.MAX_PAYLOAD) {
    throw new RangeError(`1 枠は ${OTA.MAX_PAYLOAD} バイトまで (${body.length})`);
  }
  if (!Number.isInteger(offset) || offset < 0 || offset > 0xffffff) {
    throw new RangeError(`位置が 24 bit に収まりません: ${offset}`);
  }
  const rep = new Uint8Array(OTA.REPORT_SIZE);
  rep[0] = OTA.CMD_DATA;
  rep[1] = body.length;
  rep[2] = offset & 0xff;
  rep[3] = (offset >> 8) & 0xff;
  rep[4] = (offset >> 16) & 0xff;
  rep.set(body, OTA.HEADER_SIZE);
  return rep;
}

/** 本文 0 バイトの 0xC3 = 「状態だけ返せ」。詰まったときに撃つ。 */
export function buildStatusReport() {
  const rep = new Uint8Array(OTA.REPORT_SIZE);
  rep[0] = OTA.CMD_DATA;
  return rep;
}

/**
 * 像を 27 バイトずつの枠に割る。offset から末尾まで。
 * @returns {Uint8Array[]}
 */
export function encodeImage(image, from = 0) {
  const out = [];
  for (let off = from; off < image.length; off += OTA.MAX_PAYLOAD) {
    out.push(buildDataReport(off, image.subarray(off, Math.min(off + OTA.MAX_PAYLOAD, image.length))));
  }
  return out;
}

const u32 = (a, i) => (a[i] | (a[i + 1] << 8) | (a[i + 2] << 16) | (a[i + 3] << 24)) >>> 0;

/**
 * 本体からの 0xC3 応答を読む。壊れていたら null (例外にしない)。
 * @param {DataView|Uint8Array|ArrayBuffer} view
 */
export function parseStatusReport(view) {
  let a = null;
  if (view instanceof Uint8Array) a = view;
  else if (view instanceof ArrayBuffer) a = new Uint8Array(view);
  else if (view && typeof view.byteLength === 'number' && view.buffer) {
    a = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  }
  if (!a || a.length < 20) return null;
  if (a[0] !== OTA.CMD_DATA) return null;
  return {
    state: OTA_STATE[a[1]] || 'unknown',
    stateCode: a[1],
    err: OTA_ERR[a[2]] || 'unknown',
    errCode: a[2],
    accepted: u32(a, 3),
    written: u32(a, 7),
    size: u32(a, 11),
    rejected: (a[15] & OTA.FLAG_REJECTED) !== 0,
    free: u32(a, 16),
  };
}

/**
 * 「いまどこまで送ってよいか」= credit の窓の上端。
 *
 * ★ 1 枠ごとの ack にしないのがこの方式の要。フラッシュの消去中は
 *   USB が数十 ms 黙るので、1 枚ごとに返事を待つと毎回そこで止まる。
 *
 * @param {{written:number, size:number, credit?:number}} v
 * @returns {number} この位置まで (この値は含まない) 送ってよい
 */
export function sendLimit({ written, size, credit = OTA.CREDIT }) {
  const top = (written || 0) + credit;
  return top > size ? size : top;
}

/** SHA-256 を 16 進 64 文字で。ブラウザでも Node でも同じ口 (WebCrypto)。 */
export async function sha256Hex(bytes) {
  const src = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  // ★ subarray のまま渡すと実装によって全体を食うことがある。素の buffer にする。
  const buf = src.buffer.slice(src.byteOffset, src.byteOffset + src.byteLength);
  const digest = await globalThis.crypto.subtle.digest('SHA-256', buf);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

/**
 * 像の末尾に付いている SHA-256 (16 進 64 文字)。**これが像の名札。**
 *
 * ★ ここが混ざりやすいので念入りに。**sha256 は 2 種類ある。**
 *
 *   (a) ファイル全体の SHA-256 — `sha256Hex(image)` / `shasum -a 256 stackee.bin`
 *       転送で 1 バイトも化けていないかを見るための値。
 *       本体は受け取ったバイトを流しながら同じ計算をして `ota.end` で答える。
 *
 *   (b) 像に**埋め込まれている** SHA-256 = 末尾の 32 バイト
 *       (ESP-IDF の hash_appended)。`esptool image_info` が出す値でもあり、
 *       本体の `esp_partition_get_sha256()` が返す値でもある。
 *       **どの区画に何が入っているか**を名指しするのはこちら。
 *       中身は「末尾 32 バイトを除いた部分の SHA-256」なので (a) とは別物。
 *
 * app.info の running / boot / next の sha256 は (b)。
 * ota.begin に渡す sha256 と ota.end が返す sha256 は (a)。
 *
 * @param {Uint8Array} image
 * @returns {string|null}
 */
export function embeddedSha(image) {
  if (!image || image.length < 33) return null;
  return [...image.subarray(image.length - 32)]
    .map((b) => b.toString(16).padStart(2, '0')).join('');
}

/** 末尾の SHA-256 が本物か (= その像は hash_appended 付きか)。 */
export async function verifyEmbeddedSha(image) {
  const tail = embeddedSha(image);
  if (!tail) return false;
  return sameSha(tail, await sha256Hex(image.subarray(0, image.length - 32)));
}

/** 16 進の突き合わせ (大文字小文字と前後の空白を無視)。 */
export function sameSha(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string') return false;
  return a.trim().toLowerCase() === b.trim().toLowerCase();
}

/** 0xE9 で始まるか (ESP32 のアプリ像か)。 */
export function looksLikeEspImage(bytes) {
  return !!bytes && bytes.length > 0x20 && bytes[0] === 0xe9;
}

/** エラー名を日本語にする。 */
export function otaErrorText(err) {
  return OTA_ERR_TEXT[err] || `デバイスが "${err}" で断りました。`;
}

const defaultSleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------------------
// 転送
// ---------------------------------------------------------------------------

/**
 * 像を本体へ流し込む。`ota.begin` が済んでいる前提。
 *
 * link に要るのは 2 つだけ:
 *   sendOtaReport(bytes)  32 バイトを 1 枚送る (送るだけ。応答は待たない)
 *   onOtaStatus(cb)       0xC3 の応答が来たら cb(status)。外す関数を返す
 *
 * @param {object} link
 * @param {Uint8Array} image
 * @param {{credit?:number, onProgress?:Function, now?:Function, sleep?:Function,
 *          stallMs?:number, idleMs?:number, signal?:{aborted:boolean}}} [opts]
 * @returns {Promise<{accepted:number, written:number, resyncs:number, ms:number}>}
 */
export async function transferImage(link, image, opts = {}) {
  const size = image.length;
  const credit = opts.credit || OTA.CREDIT;
  const now = opts.now || (() => Date.now());
  const sleep = opts.sleep || defaultSleep;
  // 本体が「受け取った」を伸ばさないまま、これだけ経ったら送り直す。
  const stallMs = opts.stallMs || 1500;
  // 送る側の見切り。本体は IDLE_MS で勝手に abort する。
  const deadMs = opts.deadMs || 20000;
  const onProgress = opts.onProgress || (() => {});

  let last = { accepted: 0, written: 0, err: 'none', state: 'receiving', rejected: false };
  let sent = 0;
  let resyncs = 0;
  let acceptedAt = now();
  let lastAccepted = 0;
  const started = now();

  const off = link.onOtaStatus((st) => {
    if (!st) return;
    last = st;
    if (st.accepted !== lastAccepted) {
      lastAccepted = st.accepted;
      acceptedAt = now();
    }
  });

  try {
    for (;;) {
      if (opts.signal && opts.signal.aborted) {
        throw new Error('中止しました');
      }
      if (last.errCode) {
        const e = new Error(otaErrorText(last.err));
        e.code = last.err;
        throw e;
      }
      if (last.accepted >= size) break;

      // --- credit の窓ぶんだけ送る -------------------------------------
      const limit = sendLimit({ written: last.written, size, credit });
      while (sent < limit) {
        const end = Math.min(sent + OTA.MAX_PAYLOAD, size);
        await link.sendOtaReport(buildDataReport(sent, image.subarray(sent, end)));
        sent = end;
        onProgress({ sent, accepted: last.accepted, written: last.written, size });
      }

      // --- 便りを待つ ---------------------------------------------------
      // ★ 本体は**自分からは何も言わない**。応答を返すのは 0xC3 を受けた
      //   ときだけで、しかも 1 KB ごとに 1 枚。だから credit で送れなく
      //   なったら、こちらから「状態だけ返せ」(本文 0 バイト) を撃たないと
      //   `written` が伸びたことを永久に知れない = そこで止まる。
      //   撃つのは黙っているときだけ (毎周撃つと帯域を食う)。
      if (now() - acceptedAt >= (opts.quietMs || 120)) {
        await link.sendOtaReport(buildStatusReport());
      }
      await sleep(opts.pollMs || 8);
      if (last.accepted >= size) break;
      if (last.errCode) continue;

      if (now() - acceptedAt >= stallMs && sent > last.accepted) {
        // ★ それでも伸びない = 取りこぼした。本体が言う位置から送り直す。
        //   本体は位置の合わない枠を捨てるだけなので、重複しても害は無い。
        sent = last.accepted;
        acceptedAt = now();
        resyncs += 1;
        if (resyncs > (opts.maxResyncs || 32)) {
          throw new Error('送り直しが多すぎます (デバイスが受け取れていません)');
        }
      }
      if (now() - started > deadMs + (size / 1000) * 1000) {
        throw new Error('転送が終わりません');
      }
    }
  } finally {
    off();
  }
  return { accepted: last.accepted, written: last.written, resyncs, ms: now() - started };
}

// ---------------------------------------------------------------------------
// 一式 (app.info → ota.begin → 転送 → ota.end → (ota.commit))
// ---------------------------------------------------------------------------

/**
 * @param {object} link  request(cmd,args,opts) / sendOtaReport / onOtaStatus /
 *                       (任意) setPollPaused(bool) / waitAndReconnect(opts)
 * @param {Uint8Array} image
 * @param {{commit?:boolean, onStep?:Function, onProgress?:Function,
 *          force?:boolean, now?:Function, sleep?:Function}} [opts]
 */
export async function runOta(link, image, opts = {}) {
  const onStep = opts.onStep || (() => {});
  const sleep = opts.sleep || defaultSleep;
  const commit = opts.commit !== false;

  if (!looksLikeEspImage(image)) {
    throw new Error('ESP32 のファームウェア像ではありません (先頭が 0xE9 ではない)。');
  }
  // (a) ファイル全体 = 転送が化けていないかの照合に使う
  const want = await sha256Hex(image);
  // (b) 像に埋め込まれた名札 = どの区画に何が入っているかの照合に使う
  const wantImage = embeddedSha(image);
  if (!(await verifyEmbeddedSha(image))) {
    const e = new Error(
      'この .bin には末尾の SHA-256 が付いていません (途中で切れているか、'
      + 'hash_appended 無しでビルドされています)。');
    e.code = 'nohash';
    throw e;
  }
  const size = image.length;

  onStep('info', 'いま載っている版を読んでいます');
  const before = await link.request('app.info', null, { timeoutMs: 15000 });
  if (before && before.error) {
    const e = new Error('この像は app.info を持っていません (OTA 非対応の古いファーム)。');
    e.code = 'unsupported';
    throw e;
  }
  const running = (before && before.running) || {};
  if (sameSha(running.sha256, wantImage)) {
    onStep('same', 'すでに同じ像が動いています');
    if (!opts.force) {
      return { skipped: true, before, want, wantImage, size };
    }
  }

  onStep('begin', '書き込み先を開いています');
  let begun = await link.request('ota.begin', { size, sha256: want }, { timeoutMs: 15000 });
  if (begun && begun.error === 'busy') {
    // 前の転送が残っている。畳んでから 1 度だけやり直す。
    await link.request('ota.abort', null, { timeoutMs: 10000 });
    begun = await link.request('ota.begin', { size, sha256: want }, { timeoutMs: 15000 });
  }
  if (!begun || begun.error) {
    const e = new Error(otaErrorText((begun && begun.error) || 'arg'));
    e.code = (begun && begun.error) || 'begin';
    throw e;
  }

  if (link.setPollPaused) link.setPollPaused(true);
  let moved;
  try {
    onStep('send', '像を送っています');
    moved = await transferImage(link, image, {
      credit: begun.credit || OTA.CREDIT,
      onProgress: opts.onProgress,
      now: opts.now,
      sleep: opts.sleep,
      signal: opts.signal,
    });
  } catch (err) {
    if (link.setPollPaused) link.setPollPaused(false);
    try { await link.request('ota.abort', null, { timeoutMs: 10000 }); } catch (e) { /* 掃除 */ }
    throw err;
  }
  if (link.setPollPaused) link.setPollPaused(false);

  onStep('end', '書き終わりを確かめています');
  // ★ 残りを書き切るまで本体は返事をしない。消去を含むので長めに待つ。
  const ended = await link.request('ota.end', null, { timeoutMs: 30000 });
  if (!ended || !ended.ok) {
    const e = new Error(otaErrorText((ended && ended.err) || 'size'));
    e.code = (ended && ended.err) || 'end';
    e.detail = ended;
    throw e;
  }
  if (!sameSha(ended.sha256, want)) {
    const e = new Error('デバイスが計算した sha256 が、送った像と違います。');
    e.code = 'sha';
    e.detail = ended;
    throw e;
  }
  // ★ 区画から読み直した名札。ESP-IDF は返す前に中身を検証するので、
  //   ここが一致したら「フラッシュの上の像がディスクの .bin と同じ」と
  //   端から端まで言い切れる。
  if (!sameSha(ended.partition_sha256, wantImage)) {
    const e = new Error('書いた区画から読み直した sha256 が、送った像と違います。');
    e.code = 'partition_sha';
    e.detail = ended;
    throw e;
  }

  if (!commit) {
    onStep('done', '書き込みました (まだ切り替えていません)');
    return { committed: false, before, ended, want, wantImage, size, transfer: moved };
  }

  onStep('commit', '起動する側を切り替えています');
  const committed = await link.request('ota.commit', null, { timeoutMs: 10000 });
  if (!committed || !committed.ok) {
    const e = new Error('起動区画を切り替えられませんでした。');
    e.code = 'commit';
    e.detail = committed;
    throw e;
  }

  onStep('reboot', '再起動を待っています');
  if (link.waitAndReconnect) {
    const back = await link.waitAndReconnect({ timeoutMs: 40000 });
    if (!back) {
      const e = new Error('再起動したあと、デバイスを開き直せませんでした。');
      e.code = 'reconnect';
      throw e;
    }
  }
  // 起動直後はしばらく答えない。何度か聞き直す。
  let after = null;
  for (let i = 0; i < 20; i += 1) {
    try {
      after = await link.request('app.info', null, { timeoutMs: 5000 });
      if (after && after.running) break;
    } catch (e) { /* まだ起きていない */ }
    await sleep(1000);
  }
  const nowSha = after && after.running && after.running.sha256;
  if (!sameSha(nowSha, wantImage)) {
    const e = new Error('再起動しましたが、動いている像が書いたものと違います。');
    e.code = 'verify';
    e.detail = after;
    throw e;
  }
  onStep('done', '新しい版で動いています');
  return { committed: true, before, ended, after, want, wantImage, size, transfer: moved };
}

// ---------------------------------------------------------------------------
// 操作盤の画面 (ここだけ DOM に触る)
// ---------------------------------------------------------------------------
// ★ 上の中核はここを一切知らない。Node の tools/ota.mjs は上だけを import する。
//   app.js からは `attachOtaUi({ getLink, setBusy })` を 1 行呼ぶだけ。

const $ = (id) => document.getElementById(id);

function setText(id, text) {
  const el = $(id);
  if (el) el.textContent = text;
}

function show(el, text) {
  if (!el) return;
  if (text != null) el.textContent = text;
  el.hidden = false;
}

function hide(el) {
  if (el) el.hidden = true;
}

function fmtBytes(n) {
  if (!Number.isFinite(n)) return '—';
  return `${n.toLocaleString('ja-JP')} B (${(n / 1024).toFixed(1)} KB)`;
}

/**
 * 「ファームウェア更新」の節を動かす。
 *
 * @param {{getLink: () => object, setBusy?: (busy:boolean)=>void}} deps
 */
export function attachOtaUi(deps) {
  const getLink = deps.getLink;
  const setBusy = deps.setBusy || (() => {});
  /** @type {{name:string, bytes:Uint8Array, sha:string, image:string}|null} */
  let picked = null;
  let running = null;      // いま載っている像の名札 (埋め込みの sha256)
  let signal = null;

  function refreshButton() {
    const el = $('btn-ota-write');
    if (!el) return;
    const link = getLink();
    el.disabled = !picked || !link || !link.connected || !!signal;
  }

  async function showInfo() {
    const link = getLink();
    if (!link || !link.connected) return null;
    const info = await link.request('app.info', null, { timeoutMs: 20000 });
    const r = (info && info.running) || {};
    const n = (info && info.next) || {};
    running = r.sha256 || null;
    setText('ota-running', r.label || '—');
    setText('ota-running-ver', r.version || '—');
    setText('ota-running-sha', r.sha256 || '(読めません)');
    setText('ota-next', n.label
      ? `${n.label} (いま入っているもの: ${n.version || 'なし'})`
      : '—');
    const badge = $('ota-state');
    if (badge) {
      badge.textContent = info && info.state ? info.state : '—';
      badge.className = 'badge ' + (info && info.state === 'receiving' ? 'badge-wait' : 'badge-off');
    }
    showPicked();
    return info;
  }

  function showPicked() {
    if (!picked) {
      setText('ota-new-name', '—');
      setText('ota-new-size', '—');
      setText('ota-new-sha', '—');
      setText('ota-new-file-sha', '—');
    } else {
      setText('ota-new-name', picked.name);
      setText('ota-new-size', fmtBytes(picked.bytes.length));
      setText('ota-new-sha', picked.image
        + (running && sameSha(running, picked.image) ? ' ← いま動いているものと同じ' : ''));
      setText('ota-new-file-sha', picked.sha);
    }
    refreshButton();
  }

  async function take(name, bytes) {
    hide($('ota-error'));
    hide($('ota-done'));
    if (!looksLikeEspImage(bytes)) {
      picked = null;
      showPicked();
      show($('ota-error'), `${name} は ESP32 のファームウェア像ではありません (先頭が 0xE9 ではない)。`);
      return;
    }
    if (!(await verifyEmbeddedSha(bytes))) {
      picked = null;
      showPicked();
      show($('ota-error'),
           `${name} には末尾の SHA-256 が付いていません (途中で切れているか、`
           + 'hash_appended 無しでビルドされています)。');
      return;
    }
    picked = { name, bytes, sha: await sha256Hex(bytes), image: embeddedSha(bytes) };
    showPicked();
  }

  // --- 手元の .bin ---------------------------------------------------------
  const file = $('ota-file');
  if (file) {
    file.addEventListener('change', async () => {
      const f = file.files && file.files[0];
      if (!f) return;
      // ★ 読むだけ。どこにも送らない (このページに裏側の仕組みは無い)。
      await take(f.name, new Uint8Array(await f.arrayBuffer()));
    });
  }

  // --- 配布版 (docs/firmware/manifest.json があれば) ------------------------
  // ★ 取りに行くのは **このサイト自身に置いてあるファイル 1 つだけ**。
  //   選んだ .bin も入力した内容も、どこへも送らない。
  async function loadManifest() {
    const row = $('ota-dist-row');
    const sel = $('ota-dist');
    if (!row || !sel) return;
    let manifest = null;
    try {
      const res = await fetch('./firmware/manifest.json', { cache: 'no-store' });
      if (!res.ok) return;
      manifest = await res.json();
    } catch (e) {
      return;     // 置いていない = 手元のファイル選択だけで使う
    }
    const builds = Array.isArray(manifest) ? manifest
      : (Array.isArray(manifest && manifest.builds) ? manifest.builds : [manifest]);
    sel.textContent = '';
    for (const b of builds) {
      if (!b || !b.file) continue;
      const opt = document.createElement('option');
      opt.value = b.file;
      opt.dataset.sha = b.sha256 || '';
      opt.dataset.image = b.image_sha256 || '';
      opt.dataset.size = String(b.size || 0);
      opt.textContent = `${b.version || b.file}`
        + (b.profile ? ` (${b.profile})` : '')
        + (b.sha256 ? ` sha256 ${b.sha256.slice(0, 12)}…` : '');
      sel.appendChild(opt);
    }
    if (sel.options.length > 0) row.hidden = false;
  }

  const distBtn = $('btn-ota-dist');
  if (distBtn) {
    distBtn.addEventListener('click', async () => {
      const sel = $('ota-dist');
      const opt = sel && sel.selectedOptions[0];
      if (!opt) return;
      hide($('ota-error'));
      try {
        const res = await fetch('./firmware/' + opt.value, { cache: 'no-store' });
        if (!res.ok) throw new Error(`取得できません (${res.status})`);
        const bytes = new Uint8Array(await res.arrayBuffer());
        const sha = await sha256Hex(bytes);
        if (opt.dataset.sha && !sameSha(sha, opt.dataset.sha)) {
          throw new Error('目録の sha256 と取ってきた中身が合いません。');
        }
        if (opt.dataset.image && !sameSha(embeddedSha(bytes), opt.dataset.image)) {
          throw new Error('目録の image_sha256 と取ってきた像が合いません。');
        }
        await take(opt.textContent, bytes);
      } catch (e) {
        show($('ota-error'), '配布版を読めません: ' + (e && e.message));
      }
    });
  }

  // --- 書き込み -------------------------------------------------------------
  const writeBtn = $('btn-ota-write');
  if (writeBtn) {
    writeBtn.addEventListener('click', async () => {
      const link = getLink();
      if (!picked || !link || !link.connected) return;
      const commit = !!($('chk-ota-commit') && $('chk-ota-commit').checked);
      const same = running && sameSha(running, picked.image);
      const ask = same
        ? 'いま動いているものと同じ版です。それでも書き込みますか?'
        : `${picked.name} を書き込みます。`
          + (commit ? '\n書き終わったら新しい版で再起動します。' : '\n書くだけで、起動する側は切り替えません。')
          + '\n書いている間もキーボードは使えます (数十 ms ずつ引っかかります)。';
      if (!window.confirm(ask)) return;

      hide($('ota-error'));
      hide($('ota-done'));
      signal = { aborted: false };
      setBusy(true);
      refreshButton();
      show($('btn-ota-cancel'));
      const bar = $('ota-progress');
      if (bar) { bar.value = 0; bar.hidden = false; }
      try {
        const result = await runOta(link, picked.bytes, {
          commit,
          force: true,      // 同じ版かどうかは上で聞いた
          signal,
          onStep: (kind, text) => setText('ota-step', text),
          onProgress: (p) => {
            if (bar) bar.value = Math.floor((p.accepted / p.size) * 100);
            setText('ota-step',
              `${Math.floor((p.accepted / p.size) * 100)}% `
              + `(${p.accepted.toLocaleString('ja-JP')} / ${p.size.toLocaleString('ja-JP')} B)`);
          },
        });
        if (bar) bar.value = 100;
        const took = (result.transfer.ms / 1000).toFixed(1);
        show($('ota-done'), result.committed
          ? `書き込んで切り替えました (${took} 秒)。いまは ${result.wantImage.slice(0, 16)}… で動いています。`
          : `書き込みました (${took} 秒)。**まだ切り替えていません。**`);
        $('ota-done').className = 'notice notice-ok';
        await showInfo();
      } catch (e) {
        show($('ota-error'), '書き込めませんでした: ' + (e && e.message));
        setText('ota-step', '');
        try { await showInfo(); } catch (e2) { /* 繋がっていない */ }
      } finally {
        signal = null;
        setBusy(false);
        hide($('btn-ota-cancel'));
        refreshButton();
      }
    });
  }

  const cancelBtn = $('btn-ota-cancel');
  if (cancelBtn) {
    cancelBtn.addEventListener('click', () => {
      if (signal) signal.aborted = true;
      setText('ota-step', '中止しています…');
    });
  }

  const infoBtn = $('btn-ota-info');
  if (infoBtn) {
    infoBtn.addEventListener('click', async () => {
      hide($('ota-error'));
      try {
        await showInfo();
      } catch (e) {
        show($('ota-error'), 'いま載っている版を読めません: ' + (e && e.message));
      }
    });
  }

  void loadManifest();
  refreshButton();
  return { showInfo, refreshButton };
}
