// stackee 操作盤 — WebHID (Raw HID) のトランスポート
//
// serial.js と同じ口を持つ。役目も同じ 3 つだけ。
//   1. デバイスを開く / 閉じる / 再起動後に開き直す
//   2. 受信バイトを文字列にして protocol.js の Demux に流し、
//      「ログ本文」「ステータスバー (OSC タイトル列)」「応答枠」に振り分ける
//   3. 行を書く
//
// ★ 違うのは「運び方」だけ。
//   dev プロファイルのファームは USB CDC (シリアル) を持つので serial.js で話す。
//   full プロファイルのファームは CDC を持たず、コンソールを Raw HID に載せる。
//   どちらも **流れるバイト列は同じ** なので、Demux から上は一切変わらない。
//
// プロトコル (JSON の中身) は一切知らない。定数はすべて protocol.js から取る。

import { PROTOCOL, Demux, RequestTracker } from './protocol.js';

/**
 * Raw HID インターフェースの指定。
 *
 * VID / PID は CDC と同じ (同じ USB デバイスの別インターフェース)。
 * usagePage 0xFF60 / usage 0x61 はベンダ定義の領域で、キーボードや
 * マウスの usage と衝突しない = ブラウザが触ってよい面。
 * 個体のシリアル番号は書かない (ここは公開リポジトリ)。
 */
export const USB_FILTER = {
  vendorId: 0x303a,
  productId: 0x811a,
  usagePage: 0xff60,
  usage: 0x61,
};

/** レポートの大きさ (入出力とも固定)。Report ID は無い。 */
export const REPORT_SIZE = 32;
/** 先頭 3 バイトが枠 (command id / len / flags)。 */
export const HEADER_SIZE = 3;
/** 1 レポートに載せられる本文の最大バイト数。 */
export const MAX_PAYLOAD = REPORT_SIZE - HEADER_SIZE; // 29

/**
 * レポートの先頭バイト (command id)。
 *
 * ★ デバイス側の実装と一字一句そろっている必要がある。
 *   食い違ったら「まずここを直す」こと。
 */
export const HID_CMD = {
  /** ホスト → デバイス: 本文を送る。デバイスは受け取ったバイト数を返す。 */
  TX: 0xc0,
  /** ホスト → デバイス: 受信の取り出し。デバイスは溜まっている本文を返す。 */
  RX: 0xc1,
  /** ホスト → デバイス: 情報の問い合わせ。 */
  INFO: 0xc2,
};

/** flags のビット。bit0 = デバイス側にまだ続きがある。 */
export const FLAG_MORE = 0x01;

/** 対応している HID コンソールの版 (0xC2 の応答の byte1)。 */
export const HID_PROTO_VERSION = 1;

/** 応答待ちがあるときの取り出し間隔 (ms)。 */
const POLL_FAST_MS = 4;
/** 何も待っていないときの取り出し間隔 (ms)。 */
const POLL_IDLE_MS = 25;
/** 1 レポート送ってから応答が返るまでの待ち上限 (ms)。 */
const REPORT_TIMEOUT_MS = 1000;

/** 未完エスケープ列を諦めるまでの時間 (ms)。serial.js と同じ。 */
const PARTIAL_FLUSH_MS = 250;

/** タイムアウト掃除の間隔 (ms)。 */
const SWEEP_MS = 200;

/** 送信が一部しか受け取られなかったときの待ち直し (ms)。 */
const TX_RETRY_MS = 4;

/** 1 件の送信を諦めるまでの時間 (ms)。 */
const TX_DEADLINE_MS = 3000;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------------------
// レポートの組み立てと読み取り (DOM に触らない純関数)
// ---------------------------------------------------------------------------

/**
 * 送るバイト列を 0xC0 レポートの並びに割る。
 *
 * 1 レポートに 29 バイトずつ。端数はそのまま (残りは 0 埋め)。
 * 空入力なら空配列 = 送るものが無い。
 *
 * @param {Uint8Array} bytes
 * @returns {Uint8Array[]} 各 32 バイト
 */
export function encodeTxReports(bytes) {
  const src = bytes || new Uint8Array(0);
  const out = [];
  for (let off = 0; off < src.length; off += MAX_PAYLOAD) {
    const chunk = src.subarray(off, Math.min(off + MAX_PAYLOAD, src.length));
    const rep = new Uint8Array(REPORT_SIZE);
    rep[0] = HID_CMD.TX;
    rep[1] = chunk.length;
    rep[2] = 0; // ホスト → デバイスでは flags を使わない
    rep.set(chunk, HEADER_SIZE);
    out.push(rep);
  }
  return out;
}

/**
 * 受け取ったレポートを読む。
 *
 * ★ 壊れた入力は null を返す (例外にしない)。受信は止められないので、
 *   1 枚おかしくても次を読み続けられる形にしておく。
 *   - 3 バイトに満たない
 *   - len が 29 を超える (payload をはみ出す)
 *   - len が実際の長さより大きい
 *
 * @param {DataView|{buffer:ArrayBuffer,byteOffset:number,byteLength:number}} view
 * @returns {{id:number, len:number, flags:number, more:boolean, payload:Uint8Array}|null}
 */
export function decodeRxReport(view) {
  if (!view || typeof view.byteLength !== 'number') return null;
  if (view.byteLength < HEADER_SIZE) return null;
  const all = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  const len = all[1];
  if (len > MAX_PAYLOAD) return null;
  if (HEADER_SIZE + len > all.length) return null;
  return {
    id: all[0],
    len,
    flags: all[2],
    more: (all[2] & FLAG_MORE) !== 0,
    // ★ subarray ではなく slice。元の ArrayBuffer は次の受信で使い回される。
    payload: all.slice(HEADER_SIZE, HEADER_SIZE + len),
  };
}

/** 受信の取り出し (0xC1) レポート。payload は空。 */
export function buildPollReport() {
  const rep = new Uint8Array(REPORT_SIZE);
  rep[0] = HID_CMD.RX;
  return rep;
}

/** 情報の問い合わせ (0xC2) レポート。payload は空。 */
export function buildInfoReport() {
  const rep = new Uint8Array(REPORT_SIZE);
  rep[0] = HID_CMD.INFO;
  return rep;
}

/**
 * 情報 (0xC2) の応答を読む。
 *
 *   byte1    プロトコル版
 *   byte3..6 送信待ちバイト数 (little endian uint32)
 *   byte7..10 取りこぼしたバイト数 (little endian uint32)
 *
 * @param {DataView} view
 * @returns {{proto:number, pending:number, dropped:number}|null}
 */
export function parseInfoReport(view) {
  if (!view || typeof view.byteLength !== 'number') return null;
  if (view.byteLength < 11) return null;
  const all = new Uint8Array(view.buffer, view.byteOffset, view.byteLength);
  if (all[0] !== HID_CMD.INFO) return null;
  const u32 = (i) => (all[i] | (all[i + 1] << 8) | (all[i + 2] << 16) | (all[i + 3] << 24)) >>> 0;
  return { proto: all[1], pending: u32(3), dropped: u32(7) };
}

// ---------------------------------------------------------------------------
// 対応の判定
// ---------------------------------------------------------------------------

/**
 * ブラウザが WebHID を持っているか。
 *
 * ★ Web Serial と同じで、非セキュアなオリジンでは例外ではなく
 *   navigator.hid が undefined になる (IDL が [SecureContext])。
 */
export function isSupported() {
  return typeof navigator !== 'undefined' && 'hid' in navigator;
}

/** secure context か (https: か localhost)。 */
export function isSecureContextOk() {
  return typeof window !== 'undefined' && window.isSecureContext === true;
}

/** 対応していない理由を日本語で返す。対応していれば null。 */
export function unsupportedReason() {
  if (isSupported()) return null;
  if (!isSecureContextOk()) {
    return 'このページが安全な文脈 (https:// か http://localhost/) で開かれていないため、'
      + 'WebHID が使えません。file:// で開いた場合はこうなります。'
      + 'ローカルで試すときは公開用フォルダで `python3 -m http.server` を動かし、'
      + 'http://localhost:8000/ を開いてください。';
  }
  const ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '';
  if (/Firefox\//.test(ua)) {
    return 'Firefox は WebHID に対応していません (仕様に反対の立場)。'
      + 'Chrome か Edge (デスクトップ版) で開いてください。';
  }
  if (/Safari\//.test(ua) && !/Chrome\//.test(ua)) {
    return 'Safari は WebHID に対応していません (WebKit は仕様に反対の立場)。'
      + 'Chrome か Edge (デスクトップ版) で開いてください。';
  }
  if (/Android/.test(ua)) {
    return 'Android の Chrome は WebHID に対応していません。'
      + 'パソコンの Chrome か Edge で開いてください。';
  }
  return 'このブラウザは WebHID に対応していません。'
    + 'デスクトップ版の Chrome / Edge / Opera で開いてください。';
}

// ---------------------------------------------------------------------------
// どちらの経路で繋ぐか
// ---------------------------------------------------------------------------

/**
 * 接続方法を決める。
 *
 * ★ 画面 (app.js) ではなくここに置いてあるのは、DOM に触らない純関数として
 *   Node から単体テストするため。app.js はこれを import して re-export する。
 *
 * 規則:
 *   - 'serial' / 'hid' を指名されたら、それが使えるときだけそれを返す。
 *   - 'auto' は **すでに許可済みの口があるほうを優先** する (serial が先)。
 *     許可済みが無ければ、使えるほうのうち serial を優先する。
 *   - 決められないときは transport: null と、その理由を返す。
 *
 * @param {{mode?: string, serialSupported?: boolean, hidSupported?: boolean,
 *          hasSerialPort?: boolean, hasHidDevice?: boolean}} opts
 * @returns {{transport: 'serial'|'hid'|null, reason: string|null}}
 */
export function chooseTransport(opts = {}) {
  const mode = opts.mode || 'auto';
  const serialOk = !!opts.serialSupported;
  const hidOk = !!opts.hidSupported;
  // 許可済みでも、そのブラウザで使えなければ意味がない。
  const serialReady = serialOk && !!opts.hasSerialPort;
  const hidReady = hidOk && !!opts.hasHidDevice;

  if (mode === 'serial') {
    return serialOk
      ? { transport: 'serial', reason: null }
      : { transport: null, reason: 'この接続方法 (USB シリアル) はこのブラウザでは使えません。' };
  }
  if (mode === 'hid') {
    return hidOk
      ? { transport: 'hid', reason: null }
      : { transport: null, reason: 'この接続方法 (USB HID) はこのブラウザでは使えません。' };
  }
  // 'auto' (知らない値もここに落とす)
  if (serialReady) return { transport: 'serial', reason: null };
  if (hidReady) return { transport: 'hid', reason: null };
  if (serialOk) return { transport: 'serial', reason: null };
  if (hidOk) return { transport: 'hid', reason: null };
  return {
    transport: null,
    reason: 'このブラウザは USB シリアルにも USB HID にも対応していません。',
  };
}

/** HIDDevice がコンソール用の面 (usagePage 0xFF60 / usage 0x61) を持っているか。 */
function hasConsoleCollection(device) {
  const cols = (device && device.collections) || [];
  if (cols.length === 0) return true; // 情報を返さない実装は弾かない
  for (const c of cols) {
    if (c.usagePage === USB_FILTER.usagePage && c.usage === USB_FILTER.usage) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 接続 1 本ぶん
// ---------------------------------------------------------------------------

/**
 * デバイスとの接続 1 本ぶん (Raw HID 版)。
 *
 * イベントはコンストラクタに渡すコールバックで受け取る。serial.js と同じ。
 *   onLog(text)          ログ本文 (エスケープ列は除去済み)
 *   onTitle(text)        ステータスバーの中身
 *   onState(state, info) 'disconnected' | 'connecting' | 'connected'
 *   onFrameError(err)    壊れた応答枠 (デバッグ用)
 *   onStray(frame)       対応する要求の無い応答枠
 */
export class StackeeHid {
  constructor(handlers = {}) {
    this.on = {
      onLog: handlers.onLog || (() => {}),
      onTitle: handlers.onTitle || (() => {}),
      onState: handlers.onState || (() => {}),
      onFrameError: handlers.onFrameError || (() => {}),
      onStray: handlers.onStray || (() => {}),
    };
    this.tracker = new RequestTracker();
    this._demux = new Demux();
    this._device = null;
    this._decoder = null;
    this._state = 'disconnected';
    this._closing = false;
    this._timers = [];
    this._pollLoop = null;
    /** 送信の直列化。sendReport を 2 本同時に走らせない。 */
    this._txChain = Promise.resolve();
    /** 送った順に並ぶ応答待ち。 */
    this._waiters = [];
    this._onInput = (ev) => this._handleInputReport(ev);

    if (isSupported()) {
      // 抜かれたら状態を戻す。
      navigator.hid.addEventListener('disconnect', (ev) => {
        if (this._device && ev.device === this._device) {
          this._teardown('デバイスが外されました');
        }
      });
    }
  }

  get state() {
    return this._state;
  }

  get connected() {
    return this._state === 'connected';
  }

  _setState(state, info) {
    if (this._state === state) return;
    this._state = state;
    this.on.onState(state, info);
  }

  /**
   * ユーザ操作から呼ぶ。選択ダイアログを出して開く。
   *
   * ★ requestDevice() は transient activation が要る。await を挟んだ後や
   *   setTimeout の中からは呼べない。クリックハンドラの中で最初に呼ぶこと。
   */
  async connect() {
    if (!isSupported()) throw new Error(unsupportedReason());
    if (this._state !== 'disconnected') return;
    const devices = await navigator.hid.requestDevice({ filters: [USB_FILTER] });
    const device = (devices || []).find(hasConsoleCollection) || (devices || [])[0];
    if (!device) {
      const err = new Error('デバイスが選ばれませんでした。');
      err.name = 'NotFoundError';
      throw err;
    }
    await this._openDevice(device);
  }

  /**
   * 許可済みのデバイスのうち、stackee のコンソール面に一致するものを列挙する。
   * @returns {Promise<Array>}
   */
  async listDevices() {
    if (!isSupported()) return [];
    let devices = [];
    try { devices = await navigator.hid.getDevices(); } catch (e) { return []; }
    return (devices || []).filter((d) => {
      if (d.vendorId !== USB_FILTER.vendorId) return false;
      if (d.productId !== USB_FILTER.productId) return false;
      return hasConsoleCollection(d);
    });
  }

  /** stackee の HID 面が今 OS から見えているか。 */
  async hasDevice() {
    return (await this.listDevices()).length > 0;
  }

  /**
   * すでに許可済みのデバイスがあれば、ユーザ操作なしで開き直す。
   * @returns {boolean} 開けたか
   */
  async reconnect() {
    if (!isSupported()) return false;
    if (this._state !== 'disconnected') return false;
    for (const d of await this.listDevices()) {
      try {
        await this._openDevice(d);
        return true;
      } catch (e) {
        // 再起動直後は見えても open が失敗する時間帯がある。次の周回に賭ける。
      }
    }
    return false;
  }

  async _openDevice(device) {
    this._setState('connecting');
    try {
      if (!device.opened) await device.open();
    } catch (e) {
      this._setState('disconnected', String(e && e.message));
      throw e;
    }
    this._device = device;
    this._closing = false;
    this._demux.reset();
    this._decoder = new TextDecoder('utf-8');
    this._waiters = [];
    this._txChain = Promise.resolve();
    device.addEventListener('inputreport', this._onInput);
    this._startTimers();
    this._pollLoop = this._poll();
    this._setState('connected');
  }

  _startTimers() {
    this._stopTimers();
    this._timers.push(setInterval(() => {
      for (const id of this.tracker.sweep()) void id;
    }, SWEEP_MS));
    this._timers.push(setInterval(() => {
      const held = this._demux.flushPending();
      if (held) this.on.onLog(held);
    }, PARTIAL_FLUSH_MS));
  }

  _stopTimers() {
    for (const t of this._timers) clearInterval(t);
    this._timers = [];
  }

  // --- 受信 -----------------------------------------------------------------

  _handleInputReport(ev) {
    // Report ID を持たないデバイスなので 0 で来る。それ以外は他人の面。
    if (ev.reportId !== 0) return;
    const rep = decodeRxReport(ev.data);
    if (!rep) {
      this.on.onFrameError({ reason: 'hidreport', raw: '' });
      return;
    }
    if (rep.id === HID_CMD.RX && rep.len > 0) {
      // ★ CDC に流れるのと同じバイト列。扱いは serial.js とまったく同じ。
      this._dispatch(this._decoder.decode(rep.payload, { stream: true }));
    }
    this._settle(rep, ev.data);
  }

  _dispatch(chunk) {
    const out = this._demux.push(chunk);
    if (out.text) this.on.onLog(out.text);
    for (const t of out.titles) this.on.onTitle(t);
    for (const f of out.frames) {
      if (!this.tracker.onFrame(f)) this.on.onStray(f);
    }
    for (const e of out.errors) this.on.onFrameError(e);
  }

  /** 届いたレポートを、対応する応答待ちに渡す。 */
  _settle(rep, view) {
    const i = this._waiters.findIndex((w) => w.id === rep.id);
    if (i < 0) return; // 待っていないレポート (取りこぼしの後など)
    // 間に挟まっている待ちは応答が来なかったもの。まとめて諦める。
    const skipped = this._waiters.splice(0, i + 1);
    const hit = skipped.pop();
    for (const w of skipped) w.done(null, null);
    hit.done(rep, view);
  }

  // --- 送信 -----------------------------------------------------------------

  /**
   * レポートを 1 枚送り、対応する応答を待つ。
   *
   * ★ 応答が来なくても reject しない (null を返す)。デバイスは
   *   フラッシュ書き込みなどで数秒だまることがあり、そこで接続を
   *   切ってしまうと使い物にならない。送信自体の失敗だけを投げる。
   *
   * @returns {Promise<{rep: object|null, view: DataView|null}>}
   */
  _exchange(report, timeoutMs = REPORT_TIMEOUT_MS) {
    const run = async () => {
      const device = this._device;
      if (!device || this._closing) throw new Error('デバイスが開いていません');
      let waiter = null;
      const answer = new Promise((resolve) => {
        waiter = {
          id: report[0],
          timer: setTimeout(() => {
            const i = this._waiters.indexOf(waiter);
            if (i >= 0) this._waiters.splice(i, 1);
            resolve({ rep: null, view: null });
          }, timeoutMs),
          done: (rep, view) => {
            clearTimeout(waiter.timer);
            resolve({ rep, view });
          },
        };
      });
      // ★ 送る直前に並べる。並び順 = 送った順にしておかないと突き合わせが狂う。
      this._waiters.push(waiter);
      try {
        await device.sendReport(0, report);
      } catch (e) {
        const i = this._waiters.indexOf(waiter);
        if (i >= 0) this._waiters.splice(i, 1);
        clearTimeout(waiter.timer);
        throw e;
      }
      return answer;
    };
    // 直列化。前の送信が失敗しても鎖は続ける。
    const next = this._txChain.then(run, run);
    this._txChain = next.then(() => {}, () => {});
    return next;
  }

  /** 受信の取り出しを回し続ける。 */
  async _poll() {
    while (this._device && !this._closing) {
      let rep = null;
      try {
        rep = (await this._exchange(buildPollReport())).rep;
      } catch (e) {
        if (!this._closing) this._teardown('受信が途切れました: ' + String(e && e.message));
        return;
      }
      if (this._closing) return;
      // 何か取れた / まだ続きがあるなら、間を空けずに次を撃つ。
      if (rep && (rep.more || rep.len > 0)) continue;
      await sleep(this.tracker.pendingCount > 0 ? POLL_FAST_MS : POLL_IDLE_MS);
    }
  }

  /**
   * バイト列を送る。29 バイトずつに割って 0xC0 で流す。
   *
   * デバイスが一部しか受け取らなかったら (byte1 < 送った長さ)、
   * 残りだけを詰め直して送り直す。
   */
  async _writeBytes(bytes) {
    const reports = encodeTxReports(bytes);
    const deadline = Date.now() + TX_DEADLINE_MS;
    let i = 0;
    while (i < reports.length) {
      const rep = reports[i];
      const len = rep[1];
      const { rep: ack } = await this._exchange(rep);
      // 応答が無いときは受け取られたものとして進む (要求側のタイムアウトに任せる)。
      const took = ack && ack.id === HID_CMD.TX ? Math.min(ack.len, len) : len;
      if (took >= len) {
        i += 1;
        continue;
      }
      if (Date.now() > deadline) {
        throw new Error('デバイスが送信を受け取りません (バッファが詰まっています)');
      }
      reports[i] = encodeTxReports(rep.slice(HEADER_SIZE + took, HEADER_SIZE + len))[0];
      await sleep(TX_RETRY_MS);
    }
  }

  async _writeRaw(str) {
    if (!this._device) throw new Error('デバイスが開いていません');
    await this._writeBytes(new TextEncoder().encode(str));
  }

  /** 生の文字列を送る (改行を含めること)。緊急用。 */
  async writeRaw(str) {
    return this._writeRaw(str);
  }

  /**
   * コマンドを 1 つ送って応答を待つ。serial.js とまったく同じ意味論。
   * @param {string} cmd
   * @param {object|null} [args]
   * @param {{timeoutMs?: number, multi?: boolean}} [opts]
   */
  async request(cmd, args, opts) {
    if (!this.connected) {
      const err = new Error('接続していません');
      err.code = 'disconnected';
      throw err;
    }
    const { line, promise } = this.tracker.create(cmd, args, opts);
    try {
      await this._writeRaw(line);
    } catch (e) {
      this.tracker.abortAll('送信に失敗しました');
      throw e;
    }
    return promise;
  }

  /**
   * デバイス側のコンソールの様子を問い合わせる (0xC2)。
   * @returns {Promise<{proto:number, pending:number, dropped:number}|null>}
   */
  async info() {
    if (!this._device) return null;
    const { view } = await this._exchange(buildInfoReport());
    return view ? parseInfoReport(view) : null;
  }

  // --- 後始末 ---------------------------------------------------------------

  /** 明示的に閉じる。 */
  async disconnect() {
    this._closing = true;
    this.tracker.abortAll('切断しました');
    this._stopTimers();
    this._abortWaiters();
    try { if (this._pollLoop) await this._pollLoop; } catch (e) { /* 無視 */ }
    this._pollLoop = null;
    const device = this._device;
    this._device = null;
    if (device) {
      try { device.removeEventListener('inputreport', this._onInput); } catch (e) { /* 無視 */ }
      try { await device.close(); } catch (e) { /* 無視 */ }
    }
    this._setState('disconnected');
  }

  /** 想定外の切断。close() の完了は待たない。 */
  _teardown(reason) {
    if (this._state === 'disconnected') return;
    this._closing = true;
    this.tracker.abortAll(reason);
    this._stopTimers();
    this._abortWaiters();
    const device = this._device;
    this._device = null;
    if (device) {
      try { device.removeEventListener('inputreport', this._onInput); } catch (e) { /* 無視 */ }
      try { device.close(); } catch (e) { /* 無視 */ }
    }
    this._setState('disconnected', reason);
  }

  _abortWaiters() {
    const waiters = this._waiters;
    this._waiters = [];
    for (const w of waiters) w.done(null, null);
  }

  /**
   * 再起動のあと、開き直せるまで粘る。段取りは serial.js と同じ。
   *
   *   1. **まず消えるのを待つ。** 消える前に開くと、まだ生きている古い
   *      ハンドルを掴んでしまい「繋がったのに無反応」になる。
   *   2. **次に現れるのを待つ。** 現れる前に open() しにいかない。
   *   3. 現れてから open() する。失敗したら次の周回で試す。
   *
   * 開けたあと、デバイスはまだ数秒間コマンドに答えない。
   * コマンドの再試行は呼び出し側 (app.js) が行う。
   *
   * @param {{timeoutMs?: number, intervalMs?: number,
   *          onTick?: (remainSec:number, phase:string)=>void}} opts
   * @returns {Promise<boolean>}
   */
  async waitAndReconnect(opts = {}) {
    const timeoutMs = opts.timeoutMs || 30000;
    const intervalMs = opts.intervalMs || 300;
    const onTick = opts.onTick || (() => {});
    const deadline = Date.now() + timeoutMs;
    const remain = () => Math.max(0, Math.ceil((deadline - Date.now()) / 1000));

    // --- 1. デバイスが消えるのを待つ ---
    // 消えないまま 8 秒たったら、そもそも落ちていない可能性が高い。
    const vanishDeadline = Math.min(deadline, Date.now() + 8000);
    while (Date.now() < vanishDeadline) {
      onTick(remain(), 'vanish');
      if (!(await this.hasDevice())) break;
      await sleep(intervalMs);
    }

    // --- 2. デバイスが現れるのを待ってから開く ---
    while (Date.now() < deadline) {
      onTick(remain(), 'appear');
      if (await this.hasDevice()) {
        try {
          if (await this.reconnect()) return true;
        } catch (e) { /* 次の周回に賭ける */ }
      }
      await sleep(intervalMs);
    }
    onTick(0, 'timeout');
    return false;
  }
}

export { PROTOCOL };
