// stackee 操作盤 — Web Serial のトランスポート
//
// 役目は 3 つだけ。
//   1. ポートを開く / 閉じる / 再起動後に開き直す
//   2. 受信バイトを文字列にして protocol.js の Demux に流し、
//      「ログ本文」「ステータスバー (OSC タイトル列)」「応答枠」に振り分ける
//   3. 行を書く
//
// プロトコルの中身は一切知らない。定数はすべて protocol.js から取る。

import { PROTOCOL, Demux, RequestTracker } from './protocol.js';

/**
 * M5Stack CoreS3 (CircuitPython) の USB VID / PID。
 * 出所: circuitpython/ports/espressif/boards/m5stack_cores3/mpconfigboard.mk
 *   USB_VID = 0x303A / USB_PID = 0x811A
 * 個体のシリアル番号は書かない (ここは公開リポジトリ)。
 */
export const USB_FILTER = { usbVendorId: 0x303a, usbProductId: 0x811a };

/** CDC のボーレート。ネイティブ USB なので実際には何でもよいが揃えておく。 */
export const BAUD_RATE = 115200;

/** 未完エスケープ列を諦めるまでの時間 (ms)。repl.js の PARTIAL_TOKEN_TIMEOUT 相当。 */
const PARTIAL_FLUSH_MS = 250;

/** タイムアウト掃除の間隔 (ms)。 */
const SWEEP_MS = 200;

/**
 * ブラウザが Web Serial を持っているか。
 *
 * ★ 非セキュアなオリジンでは例外が飛ぶのではなく navigator.serial が
 *   undefined になる (IDL が [SecureContext])。try/catch では検出できない。
 */
export function isSupported() {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
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
      + 'Web Serial が使えません。file:// で開いた場合はこうなります。'
      + 'ローカルで試すときは公開用フォルダで `python3 -m http.server` を動かし、'
      + 'http://localhost:8000/ を開いてください。';
  }
  const ua = (typeof navigator !== 'undefined' && navigator.userAgent) || '';
  if (/Firefox\//.test(ua)) {
    return 'Firefox は既定では Web Serial を使えません (アドオンによる有効化が必要)。'
      + 'Chrome か Edge (デスクトップ版) で開いてください。';
  }
  if (/Safari\//.test(ua) && !/Chrome\//.test(ua)) {
    return 'Safari は Web Serial に対応していません (WebKit は仕様に反対の立場)。'
      + 'Chrome か Edge (デスクトップ版) で開いてください。';
  }
  if (/Android/.test(ua)) {
    return 'Android の Chrome は有線 USB シリアルに対応していません。'
      + 'パソコンの Chrome か Edge で開いてください。';
  }
  return 'このブラウザは Web Serial に対応していません。'
    + 'デスクトップ版の Chrome / Edge / Opera で開いてください。';
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * デバイスとの接続 1 本ぶん。
 *
 * イベントはコンストラクタに渡すコールバックで受け取る。
 *   onLog(text)          ログ本文 (エスケープ列は除去済み)
 *   onTitle(text)        ステータスバーの中身
 *   onState(state, info) 'disconnected' | 'connecting' | 'connected'
 *   onFrameError(err)    壊れた応答枠 (デバッグ用)
 *   onStray(frame)       対応する要求の無い応答枠
 */
export class StackeeSerial {
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
    this._port = null;
    this._reader = null;
    this._writer = null;
    this._readLoop = null;
    this._state = 'disconnected';
    this._closing = false;
    this._timers = [];

    if (isSupported()) {
      // 抜かれたら状態を戻す。読みループの reject と両方飛ぶので再入ガードが要る。
      navigator.serial.addEventListener('disconnect', (ev) => {
        if (this._port && ev.target === this._port) this._teardown('デバイスが外されました');
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
   * ★ requestPort() は transient activation が要る。await を挟んだ後や
   *   setTimeout の中からは呼べない。クリックハンドラの中で最初に呼ぶこと。
   */
  async connect() {
    if (!isSupported()) throw new Error(unsupportedReason());
    if (this._state !== 'disconnected') return;
    const port = await navigator.serial.requestPort({ filters: [USB_FILTER] });
    await this._openPort(port);
  }

  /**
   * 許可済みのポートのうち、stackee の VID/PID に一致するものを列挙する。
   *
   * ★ 再列挙をまたぐと SerialPort オブジェクト自体が別物になる
   *   (Chromium は列挙ごとの UnguessableToken でポートを識別する) ので、
   *   古いハンドルは使わず必ず getPorts() から取り直す。
   * @returns {Promise<SerialPort[]>}
   */
  async listPorts() {
    if (!isSupported()) return [];
    const ports = await navigator.serial.getPorts();
    return ports.filter((p) => {
      let info = {};
      try { info = p.getInfo() || {}; } catch (e) { return true; } // 古い実装
      if (info.usbVendorId != null && info.usbVendorId !== USB_FILTER.usbVendorId) return false;
      if (info.usbProductId != null && info.usbProductId !== USB_FILTER.usbProductId) return false;
      return true;
    });
  }

  /** stackee のポートが今 OS から見えているか。 */
  async hasPort() {
    return (await this.listPorts()).length > 0;
  }

  /**
   * すでに許可済みのポートがあれば、ユーザ操作なしで開き直す。
   *
   * 同じ VID/PID で再列挙するリセット (microcontroller.reset) なら
   * getPorts() に残っているので、これで自動再接続できる。
   * @returns {boolean} 開けたか
   */
  async reconnect() {
    if (!isSupported()) return false;
    if (this._state !== 'disconnected') return false;
    for (const p of await this.listPorts()) {
      try {
        await this._openPort(p);
        return true;
      } catch (e) {
        // 再起動直後はポートが見えても open が失敗する時間帯がある。次の周回に賭ける。
      }
    }
    return false;
  }

  async _openPort(port) {
    this._setState('connecting');
    try {
      await port.open({ baudRate: BAUD_RATE });
    } catch (e) {
      this._setState('disconnected', String(e && e.message));
      throw e;
    }
    this._port = port;
    this._closing = false;
    this._demux.reset();
    this._writer = port.writable.getWriter();
    this._startTimers();
    this._readLoop = this._read();
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

  async _read() {
    const decoder = new TextDecoder('utf-8');
    while (this._port && this._port.readable && !this._closing) {
      const reader = this._port.readable.getReader();
      this._reader = reader;
      try {
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          if (!value || value.length === 0) continue;
          this._dispatch(decoder.decode(value, { stream: true }));
        }
      } catch (e) {
        if (!this._closing) {
          this._teardown('受信が途切れました: ' + String(e && e.message));
          return;
        }
      } finally {
        try { reader.releaseLock(); } catch (e) { /* 既に解放済み */ }
        this._reader = null;
      }
      if (this._closing) break;
    }
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

  /**
   * コマンドを 1 つ送って応答を待つ。
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

  async _writeRaw(str) {
    if (!this._writer) throw new Error('ポートが開いていません');
    await this._writer.write(new TextEncoder().encode(str));
  }

  /** 生の文字列を送る (改行を含めること)。緊急用。 */
  async writeRaw(str) {
    return this._writeRaw(str);
  }

  /** 明示的に閉じる。 */
  async disconnect() {
    this._closing = true;
    this.tracker.abortAll('切断しました');
    this._stopTimers();
    try { if (this._reader) await this._reader.cancel(); } catch (e) { /* 無視 */ }
    try { if (this._readLoop) await this._readLoop; } catch (e) { /* 無視 */ }
    try { if (this._writer) this._writer.releaseLock(); } catch (e) { /* 無視 */ }
    this._writer = null;
    try { if (this._port) await this._port.close(); } catch (e) { /* 無視 */ }
    this._port = null;
    this._setState('disconnected');
  }

  /** 想定外の切断。close() の完了は待たない。 */
  _teardown(reason) {
    if (this._state === 'disconnected') return;
    this._closing = true;
    this.tracker.abortAll(reason);
    this._stopTimers();
    try { if (this._writer) this._writer.releaseLock(); } catch (e) { /* 無視 */ }
    this._writer = null;
    const port = this._port;
    this._port = null;
    if (port) { try { port.close(); } catch (e) { /* 無視 */ } }
    this._setState('disconnected', reason);
  }

  /**
   * 再起動のあと、開き直せるまで粘る。
   *
   * 実機の実測 (ack を 0 秒とする):
   *   1.69 s  ポートが消える
   *   5.23 s  同じ名前で再び現れる
   *   5.25 s  open() が成功する (errno 6 は 0 回)
   *   8.51 s  status が返る (このときデバイスの up は 1.1 秒)
   *
   * だから順番を守る。
   *   1. **まず消えるのを待つ。** 消える前に開くと、まだ生きている古い
   *      ハンドルを掴んでしまい「繋がったのに無反応」になる。
   *   2. **次に現れるのを待つ。** ★ ポートが現れる前に open() しにいかない。
   *      macOS の USB 列挙が壊れて 20 分戻らなくなった記録がある。
   *   3. 現れてから open() する。失敗したら次の周回で試す。
   *
   * 開けたあと、デバイスはまだ 3 秒ほど起動中でコマンドに答えない。
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

    // --- 1. ポートが消えるのを待つ (実測 1.69 秒) ---
    // 消えないまま 8 秒たったら、そもそも落ちていない可能性が高い。
    // その場合は「現れるのを待つ」段階へ進めて、そのまま開きに行く。
    const vanishDeadline = Math.min(deadline, Date.now() + 8000);
    while (Date.now() < vanishDeadline) {
      onTick(remain(), 'vanish');
      if (!(await this.hasPort())) break;
      await sleep(intervalMs);
    }

    // --- 2. ポートが現れるのを待ってから開く (実測 5.23 秒) ---
    while (Date.now() < deadline) {
      onTick(remain(), 'appear');
      if (await this.hasPort()) {
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
