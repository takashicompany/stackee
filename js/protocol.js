// stackee 操作盤 — プロトコル層
//
// このファイルは DOM に一切触らない。すべて純粋な関数とクラスなので、
// Node の `node --test public/test/` からそのまま単体テストできる。
//
// ===========================================================================
// ★★★ プロトコル定数はここ 1 か所にまとめる ★★★
//
// デバイス側の firmware/kmk/stackee_console.py と一字一句そろっている必要が
// ある。stackee_console.py は本ファイルと並行して書かれているため、細かな
// 調整が入りうる。食い違いが出たら「まずここを直す」こと。他のファイルは
// この定数を import しているだけで、生の "\x1e" や "status" を書いていない。
// ===========================================================================

/** 枠 (フレーミング) の定義。 */
export const PROTOCOL = {
  /** ホスト → デバイス。行頭に必ず付ける目印 (RS = Record Separator, 0x1E)。 */
  REQ_PREFIX: '\x1e',
  /** ホスト → デバイス。行末。 */
  REQ_TERM: '\n',
  /** デバイス → ホスト。行頭の目印。要求と同じ 0x1E。 */
  RES_PREFIX: '\x1e',
  /** デバイス → ホスト。行末。 */
  RES_TERM: '\n',
  /** プロトコル世代。hello の応答と突き合わせる。 */
  VERSION: 1,

  // JSON のキー名 (stackee_console.py の _handle() / _send() と同じ)
  KEY_ID: 'id',     // 要求 ID。デバイスはエコーするだけ
  KEY_CMD: 'cmd',   // コマンド名
  KEY_ERR: 'error', // 応答: これがあれば失敗。中身は下の ERR を参照
  KEY_OK: 'ok',     // 応答: 成功を明示するコマンドだけが 1 を返す
  KEY_END: 'end',   // 応答: 複数行応答の終端マーカー (今のファームは使わない)

  // ★ 引数は入れ子にせず、要求オブジェクトの直下に置く。
  //   stackee_console.py は req.get('kv') / req.get('ch') のように直下を見る。
  //   したがって引数に "id" / "cmd" というキーは使えない。
};

/** v1 のコマンド名。 */
export const CMD = {
  HELLO: 'hello',                 // 疎通確認 + 対応コマンド一覧
  STATUS: 'status',               // 電池・送信先・Wi-Fi・版・稼働時間
  SETTINGS_GET: 'settings.get',   // settings.toml の読み (パスワードの値は返らない)
  SETTINGS_SET: 'settings.set',   // settings.toml の書き (指定キーだけ差し替え)
  WIFI_SCAN: 'wifi.scan',         // Wi-Fi スキャン (複数行応答。非対応機あり)
  RESET: 'reset',                 // 再起動
};

/**
 * 失敗コード。デバイスは "error" キーに文字列を入れて返す。
 * 一部は "denied:STACKEE_HOST" のように ":" のあとに詳細が付く。
 */
export const ERR = {
  /** その機能がこのファームでは無効 (例: Wi-Fi を切ってある)。 */
  UNSUPPORTED: 'unsupported',
  /** 知らないコマンド。古いファーム。"unknown:<cmd>" で来る。 */
  UNKNOWN: 'unknown',
  /** 要求の JSON が読めなかった。 */
  BADJSON: 'badjson',
  /** settings.set に kv が無い / 空。 */
  NOKV: 'nokv',
  /** 書き換えを許していないキー。"denied:<KEY>" */
  DENIED: 'denied',
  /** 値が文字列でも null でもない。"notstr:<KEY>" */
  NOTSTR: 'notstr',
  /** 書き戻して読み直したら内容が違った。 */
  VERIFY: 'verify',
  /** ファイルの書き込み失敗。"write:<詳細>" */
  WRITE: 'write',
  /** ファイルの読み込み失敗。"read:<詳細>" */
  READ: 'read',
  /** 応答が来なかった (ページ側で作るコード。デバイスは送らない)。 */
  TIMEOUT: 'timeout',
  /** 接続が切れた (ページ側で作るコード)。 */
  DISCONNECTED: 'disconnected',
};

/** "denied:STACKEE_HOST" → "denied" のように、コードの頭だけを取る。 */
export function errorHead(code) {
  const s = String(code == null ? '' : code);
  const i = s.indexOf(':');
  return i < 0 ? s : s.slice(0, i);
}

/** settings.toml のキー名。firmware/kmk/settings.toml.example と同じ綴り。 */
export const SETTING_KEYS = {
  SSID: 'STACKEE_WIFI_SSID',
  PASSWORD: 'STACKEE_WIFI_PASSWORD',
  CHANNEL: 'STACKEE_WIFI_CHANNEL',
  HOST: 'STACKEE_HOST',
  PORT: 'STACKEE_PORT',
};

// reset に引数は無い。stackee_console.py は必ず microcontroller.reset() を行う
// (supervisor.reload() は BLE が死ぬ既知の不具合があるので実装されていない)。
// 応答 {"ok":1,"in_ms":300} を返してから 300 ms 後に落ちる。
// 同じ VID/PID で再列挙するので、ユーザ操作なしで開き直せる。

/** 1 応答の上限。これを超えたら壊れたとみなして捨てる。 */
export const MAX_FRAME_LENGTH = 8192;

// ---------------------------------------------------------------------------
// 要求の組み立て
// ---------------------------------------------------------------------------

/**
 * 文字列を純 ASCII の JSON にする。
 *
 * 非 ASCII (日本語 SSID など) を \uXXXX へ逃がす。こうしておくと
 * 送信行に 0x00-0x1F の生バイトが絶対に混ざらない = とくに \x03 (Ctrl-C) が
 * 混ざらないことが構造的に保証される。Ctrl-C は TinyUSB のコールバックで
 * 受信 FIFO ごと捨てられてしまうため、これは安全上の要請。
 */
function toAsciiJson(value) {
  return JSON.stringify(value).replace(/[\u007f-\uffff]/g, (ch) =>
    '\\u' + ch.charCodeAt(0).toString(16).padStart(4, '0'),
  );
}

/**
 * 要求を 1 行の文字列にする。
 *
 * 引数は入れ子にせず、要求オブジェクトの直下へ広げる
 * (stackee_console.py が req.get('kv') のように直下を見るため)。
 *
 * @param {number} id  0 以上の整数
 * @param {string} cmd CMD のいずれか
 * @param {object|null} args 省略可
 * @returns {string} "\x1e{...}\n"
 */
export function buildRequest(id, cmd, args) {
  if (!Number.isInteger(id) || id < 0) {
    throw new TypeError('要求 ID は 0 以上の整数であること: ' + String(id));
  }
  if (typeof cmd !== 'string' || cmd.length === 0) {
    throw new TypeError('コマンド名が空');
  }
  const obj = {};
  obj[PROTOCOL.KEY_ID] = id;
  obj[PROTOCOL.KEY_CMD] = cmd;
  if (args !== undefined && args !== null) {
    if (typeof args !== 'object' || Array.isArray(args)) {
      throw new TypeError('引数はオブジェクトであること');
    }
    for (const k of Object.keys(args)) {
      if (k === PROTOCOL.KEY_ID || k === PROTOCOL.KEY_CMD) {
        throw new TypeError('引数に予約キーは使えない: ' + k);
      }
      obj[k] = args[k];
    }
  }
  const json = toAsciiJson(obj);
  // 念のための保険。ここに引っかかるなら toAsciiJson が壊れている。
  if (/[\x00-\x1f]/.test(json)) {
    throw new Error('要求に制御文字が混ざった: ' + JSON.stringify(json));
  }
  return PROTOCOL.REQ_PREFIX + json + PROTOCOL.REQ_TERM;
}

// ---------------------------------------------------------------------------
// 受信ストリームの分解 (ログ / ステータスバー / 応答枠)
// ---------------------------------------------------------------------------

// CircuitPython のステータスバーは OSC タイトル列 (\x1b]0;...\x1b\\ か BEL 終端)
// で出る。行消去 (\x1b[2K) や桁移動 (\x1b[0G)、単独の ST (\x1b\\) も来る。
// firmware/kmk/tools/stackee_serial.py の _ESC_PATTERN と同じ考え方。
const ESC_COMPLETE = /\x1b\][^\x07\x1b]*(?:\x1b\\|\x07)|\x1b\[[0-9;?]*[A-Za-z]|\x1b\\/y;
// 受信の切れ目でエスケープ列が割れることがある。続きが届くまで本文にしない。
// (stackee_serial.py の _ESC_TAIL_RE と同じ。上流の circuitpython-repl-js も
//  同じバグを踏んで _getPartialTokenSuffix() という同じ対策に着地している)
const ESC_PARTIAL = /^\x1b(?:\][^\x07]*|\[[0-9;?]*)?$/;

/** OSC タイトル列の中身 "0;本文" から本文を取り出す。 */
function oscTitleBody(seq) {
  // seq は "\x1b]0;....\x1b\\" か "\x1b]0;....\x07"
  let body = seq.slice(2); // "\x1b]" を落とす
  body = body.replace(/(?:\x1b\\|\x07)$/, '');
  const m = /^(\d*);([\s\S]*)$/.exec(body);
  if (!m) return null;
  // タイトルは OSC 0 / 1 / 2。それ以外 (色設定など) はタイトルではない。
  if (m[1] !== '' && m[1] !== '0' && m[1] !== '1' && m[1] !== '2') return null;
  return m[2];
}

/**
 * シリアルから届いた文字列を「ログ本文」「ステータスバー」「応答枠」に分ける。
 *
 * 状態を持つのは (a) 途中で切れたエスケープ列 (b) 途中まで届いた応答枠 の 2 つ
 * だけ。どちらもインスタンスに溜めるので、チャンクの切れ目をまたいでよい。
 *
 * 判定は必ず「蓄積したバッファ」に対して行う。チャンク単位でエスケープを
 * 取り除くやり方では、境目で割れた列を取りこぼす。
 */
export class Demux {
  constructor() {
    /** @type {string} 未消費の末尾 (途中で切れたエスケープ列) */
    this._pending = '';
    /** @type {string|null} 受信中の応答枠。null なら枠の外 */
    this._frame = null;
  }

  /** 内部状態を捨てる。再接続時に呼ぶ。 */
  reset() {
    this._pending = '';
    this._frame = null;
  }

  /**
   * チャンクを 1 つ流し込む。
   * @param {string} chunk
   * @returns {{text: string, titles: string[], frames: object[], errors: object[]}}
   */
  push(chunk) {
    const buf = this._pending + (chunk || '');
    this._pending = '';
    let text = '';
    const titles = [];
    const frames = [];
    const errors = [];
    let i = 0;

    while (i < buf.length) {
      const ch = buf[i];

      if (ch === '\x1b') {
        ESC_COMPLETE.lastIndex = i;
        const m = ESC_COMPLETE.exec(buf);
        if (m) {
          if (m[0][1] === ']') {
            const title = oscTitleBody(m[0]);
            if (title !== null) titles.push(title);
          }
          i = ESC_COMPLETE.lastIndex;
          continue;
        }
        if (ESC_PARTIAL.test(buf.slice(i))) {
          // 続きの受信待ち。中身は本文として出さない。
          this._pending = buf.slice(i);
          break;
        }
        // 解釈できない ESC は 1 文字だけ捨てる。
        i += 1;
        continue;
      }

      if (this._frame === null) {
        if (ch === PROTOCOL.RES_PREFIX) {
          this._frame = '';
          i += 1;
          continue;
        }
        const j = nextSpecial(buf, i, false);
        text += buf.slice(i, j);
        i = j;
        continue;
      }

      // 応答枠の中
      if (ch === PROTOCOL.RES_TERM) {
        const raw = this._frame;
        this._frame = null;
        i += 1;
        const trimmed = raw.replace(/\r$/, '').trim();
        if (trimmed === '') continue;
        try {
          const obj = JSON.parse(trimmed);
          if (obj && typeof obj === 'object' && !Array.isArray(obj)) {
            frames.push(obj);
          } else {
            errors.push({ reason: 'notobject', raw: trimmed });
          }
        } catch (e) {
          errors.push({ reason: 'json', raw: trimmed, message: String(e && e.message) });
        }
        continue;
      }
      if (ch === PROTOCOL.RES_PREFIX) {
        // 枠の途中で次の枠が始まった = 前の枠は壊れている。作り直す。
        errors.push({ reason: 'resync', raw: this._frame });
        this._frame = '';
        i += 1;
        continue;
      }
      const j = nextSpecial(buf, i, true);
      this._frame += buf.slice(i, j);
      i = j;
      if (this._frame.length > MAX_FRAME_LENGTH) {
        errors.push({ reason: 'toolong', raw: this._frame.slice(0, 200) });
        this._frame = null;
      }
    }

    return { text, titles, frames, errors };
  }

  /**
   * 保留中の未完エスケープ列を諦めて本文として吐き出す。
   *
   * 「\x1b が 1 バイトだけ来て続きが来ない」場合にログが止まるのを防ぐ。
   * app 側から 250 ms ほどの間隔で呼ぶ (circuitpython-repl-js の
   * PARTIAL_TOKEN_TIMEOUT と同じ考え方)。
   * @returns {string}
   */
  flushPending() {
    const s = this._pending;
    this._pending = '';
    return s;
  }
}

/** buf[from] 以降で最初に現れる特別扱いの文字の位置。 */
function nextSpecial(buf, from, inFrame) {
  for (let k = from; k < buf.length; k += 1) {
    const c = buf[k];
    if (c === '\x1b' || c === PROTOCOL.RES_PREFIX) return k;
    if (inFrame && c === PROTOCOL.RES_TERM) return k;
  }
  return buf.length;
}

// ---------------------------------------------------------------------------
// 要求と応答の対応付け
// ---------------------------------------------------------------------------

/** 既定のタイムアウト (ms)。フラッシュ書き込みを伴うものは長めにする。 */
export const DEFAULT_TIMEOUT_MS = 3000;
export const TIMEOUT_MS = {
  [CMD.HELLO]: 3000,
  [CMD.STATUS]: 3000,
  [CMD.SETTINGS_GET]: 5000,
  // settings.set は実機で 224 ms (フラッシュ書き + 読み戻し照合 + os.rename)。
  // さらに打鍵中は無打鍵 300 ms を待ち、最大 3 秒で諦めて実行する。
  // 実測の往復は 243 ms だが、打鍵ガードの 3 秒を足して余裕を持たせる。
  [CMD.SETTINGS_SET]: 10000,
  // wifi.scan は全 13 チャネルを 1 応答で返す。実機で 5.09 秒。
  // (1〜11ch は settle 300 ms、12ch 以上はパッシブ走査なので 800 ms)
  [CMD.WIFI_SCAN]: 25000,
  [CMD.RESET]: 3000,
};

/**
 * 要求 ID の採番、応答との突き合わせ、タイムアウトを見る。
 *
 * 時刻は注入できる (`now`)。タイムアウトの判定は `sweep()` を呼んだときだけ
 * 起きるので、テストから偽の時計で完全に制御できる。ブラウザ側は
 * setInterval から sweep() を叩く。
 */
export class RequestTracker {
  /**
   * @param {{now?: () => number, firstId?: number}} [opts]
   */
  constructor(opts = {}) {
    this._now = opts.now || (() => Date.now());
    this._nextId = Number.isInteger(opts.firstId) ? opts.firstId : 1;
    /** @type {Map<number, object>} */
    this._pending = new Map();
  }

  /** 待っている要求の数。 */
  get pendingCount() {
    return this._pending.size;
  }

  /** 指定 ID を待っているか。 */
  isPending(id) {
    return this._pending.has(id);
  }

  /**
   * 要求を 1 つ作る。送信自体は呼び出し側 (serial.js) がやる。
   *
   * @param {string} cmd
   * @param {object|null} [args]
   * @param {{timeoutMs?: number, multi?: boolean}} [opts]
   *   multi=true なら {"end":1} が来るまで複数の応答を集める (wifi.scan)。
   * @returns {{id: number, line: string, promise: Promise<any>}}
   */
  create(cmd, args, opts = {}) {
    const id = this._nextId;
    this._nextId = (this._nextId + 1) % 100000 || 1;
    const line = buildRequest(id, cmd, args);
    const timeoutMs = opts.timeoutMs || TIMEOUT_MS[cmd] || DEFAULT_TIMEOUT_MS;
    const entry = {
      id,
      cmd,
      multi: !!opts.multi,
      parts: [],
      timeoutMs,
      deadline: this._now() + timeoutMs,
      resolve: null,
      reject: null,
    };
    const promise = new Promise((resolve, reject) => {
      entry.resolve = resolve;
      entry.reject = reject;
    });
    this._pending.set(id, entry);
    return { id, line, promise };
  }

  /**
   * 受け取った応答枠を 1 つ食わせる。
   * @param {object} frame
   * @returns {boolean} 対応する要求があったか (false なら迷子の応答)
   */
  onFrame(frame) {
    if (!frame || typeof frame !== 'object') return false;
    const id = frame[PROTOCOL.KEY_ID];
    if (!Number.isInteger(id)) return false;
    const entry = this._pending.get(id);
    if (!entry) return false;

    // 失敗は "error" キーの有無で決まる。成功時に "ok":1 を返すのは
    // reset / settings.set / wifi.scan だけで、hello / status / settings.get は
    // ok を持たない。だから ok の有無で成否を判定してはいけない。
    if (frame[PROTOCOL.KEY_ERR] != null) {
      this._pending.delete(id);
      entry.reject(makeProtocolError(frame, entry.cmd));
      return true;
    }

    if (entry.multi) {
      if (frame[PROTOCOL.KEY_END] === 1 || frame[PROTOCOL.KEY_END] === true) {
        this._pending.delete(id);
        entry.resolve(entry.parts);
        return true;
      }
      entry.parts.push(frame);
      // 途中の応答が来たらタイムアウトを延ばす (スキャンは数秒かかる)
      entry.deadline = this._now() + entry.timeoutMs;
      return true;
    }

    this._pending.delete(id);
    entry.resolve(frame);
    return true;
  }

  /**
   * 期限切れの要求を落とす。
   * @returns {number[]} 落とした要求の ID
   */
  sweep() {
    const t = this._now();
    const dead = [];
    for (const [id, entry] of this._pending) {
      if (entry.deadline <= t) dead.push(id);
    }
    for (const id of dead) {
      const entry = this._pending.get(id);
      this._pending.delete(id);
      const err = new Error(
        'デバイスから応答がありません (' + entry.cmd + ', ' + entry.timeoutMs + 'ms)',
      );
      err.code = ERR.TIMEOUT;
      err.cmd = entry.cmd;
      entry.reject(err);
    }
    return dead;
  }

  /**
   * 待っている要求を全部落とす。切断時と Ctrl-C 送信後に呼ぶ。
   * (Ctrl-C は TinyUSB が受信 FIFO ごと捨てるので、直前の要求は必ず消える)
   * @param {string} [reason]
   */
  abortAll(reason) {
    const ids = [...this._pending.keys()];
    for (const id of ids) {
      const entry = this._pending.get(id);
      this._pending.delete(id);
      const err = new Error(reason || '接続が切れました');
      err.code = ERR.DISCONNECTED;
      err.cmd = entry.cmd;
      entry.reject(err);
    }
    return ids;
  }
}

/** error 付きの応答を Error にする。 */
export function makeProtocolError(frame, cmd) {
  const code = String(frame[PROTOCOL.KEY_ERR]);
  const err = new Error(errorText(code, cmd));
  err.code = code;
  err.cmd = cmd;
  err.frame = frame;
  return err;
}

/** 失敗コードを日本語にする。"denied:KEY" のような詳細付きも受ける。 */
export function errorText(code, cmd) {
  const s = String(code == null ? '' : code);
  const detail = s.indexOf(':') < 0 ? '' : s.slice(s.indexOf(':') + 1);
  switch (errorHead(s)) {
    case ERR.UNSUPPORTED:
      return 'この機能はデバイス側で無効になっています'
        + (cmd ? ' (' + cmd + ')' : '');
    case ERR.UNKNOWN:
      return 'このファームウェアは ' + (detail || cmd || 'このコマンド') + ' に対応していません';
    case ERR.BADJSON:
      return '要求をデバイスが読めませんでした';
    case ERR.NOKV:
      return '書き換える項目がありません';
    case ERR.DENIED:
      return 'デバイスが書き換えを許していないキーです: ' + detail;
    case ERR.NOTSTR:
      return '値の型が正しくありません: ' + detail;
    case ERR.VERIFY:
      return '書き込んだ内容を読み直したら一致しませんでした。もう一度試してください';
    case ERR.WRITE:
      return 'settings.toml を書けませんでした: ' + detail;
    case ERR.READ:
      return 'settings.toml を読めませんでした: ' + detail;
    case ERR.TIMEOUT:
      return 'デバイスから応答がありません';
    case ERR.DISCONNECTED:
      return '接続が切れました';
    default:
      return 'デバイス側でエラーが起きました (' + s + ')';
  }
}

/**
 * 応答が「その機能は使えない」を意味するか。
 * 古いファームは "unknown:<cmd>"、機能を切ってあるファームは "unsupported"。
 * どちらもページ側では同じ扱い (その機能を隠す) でよい。
 */
export function isUnsupported(err) {
  if (!err) return false;
  const head = errorHead(err.code);
  return head === ERR.UNSUPPORTED || head === ERR.UNKNOWN;
}

// ---------------------------------------------------------------------------
// 設定値の検証
// ---------------------------------------------------------------------------

const utf8 = new TextEncoder();

/** UTF-8 でのバイト長。 */
export function byteLength(s) {
  return utf8.encode(String(s)).length;
}

/** SSID: 空でなく 32 バイト以下。 */
export function validateSsid(v) {
  const s = String(v == null ? '' : v);
  if (s.length === 0) return 'SSID を入力してください';
  if (byteLength(s) > 32) return 'SSID は 32 バイト以内です (現在 ' + byteLength(s) + ' バイト)';
  if (/[\x00-\x1f\x7f]/.test(s)) return 'SSID に制御文字は使えません';
  return null;
}

/** パスワード: 空 (= 変更しない/未設定) か、8〜63 文字。WPA2-PSK の規定。 */
export function validatePassword(v) {
  const s = String(v == null ? '' : v);
  if (s.length === 0) return null;
  if (s.length < 8) return 'パスワードは 8 文字以上です';
  if (s.length > 63) return 'パスワードは 63 文字以内です';
  if (/[\x00-\x1f\x7f]/.test(s)) return 'パスワードに制御文字は使えません';
  return null;
}

/** チャネル: 空 (自動) か 1〜14 の整数。 */
export function validateChannel(v) {
  const s = String(v == null ? '' : v).trim();
  if (s === '') return null;
  if (!/^\d+$/.test(s)) return 'チャネルは数字で入力してください';
  const n = Number(s);
  if (n < 1 || n > 14) return 'チャネルは 1〜14 です';
  return null;
}

/** ポート: 1〜65535 の整数。必須。 */
export function validatePort(v) {
  const s = String(v == null ? '' : v).trim();
  if (s === '') return 'ポート番号を入力してください';
  if (!/^\d+$/.test(s)) return 'ポート番号は数字で入力してください';
  const n = Number(s);
  if (n < 1 || n > 65535) return 'ポート番号は 1〜65535 です';
  return null;
}

/** 送信先ホスト: 空でなく、空白・制御文字を含まない。IP でもホスト名でもよい。 */
export function validateHost(v) {
  const s = String(v == null ? '' : v).trim();
  if (s === '') return '音声サーバの IP アドレスを入力してください';
  if (s.length > 255) return 'ホスト名が長すぎます';
  if (/[\s\x00-\x1f\x7f"\\]/.test(s)) return 'ホスト名に使えない文字が含まれています';
  return null;
}

/**
 * フォーム全体を検証する。
 * @param {{ssid?, password?, channel?, host?, port?}} v
 * @returns {{ok: boolean, errors: Object<string,string>}}
 */
export function validateSettings(v) {
  const errors = {};
  const put = (k, msg) => { if (msg) errors[k] = msg; };
  put('ssid', validateSsid(v.ssid));
  put('password', validatePassword(v.password));
  put('channel', validateChannel(v.channel));
  put('host', validateHost(v.host));
  put('port', validatePort(v.port));
  return { ok: Object.keys(errors).length === 0, errors };
}

/**
 * settings.set に渡す引数を作る。
 *
 * - 値は必ず文字列にする (settings.toml は全部クォート付き文字列で書く)。
 * - **null は「その行を消す」** の意味になる (stackee_console.py の
 *   rewrite_settings())。チャネル未指定と、パスワードの明示的な消去に使う。
 * - パスワードは、空なら **キーごと省く** = デバイス側は今の値を保つ。
 *   明示的に消したいときだけ clearPassword:true を渡す (null を送る)。
 *
 * @param {{ssid, password, channel, host, port, clearPassword?}} v
 * @returns {{kv: Object<string, string|null>}}
 */
export function buildSettingsArgs(v) {
  const kv = {};
  kv[SETTING_KEYS.SSID] = String(v.ssid == null ? '' : v.ssid);
  kv[SETTING_KEYS.HOST] = String(v.host == null ? '' : v.host).trim();
  kv[SETTING_KEYS.PORT] = String(v.port == null ? '' : v.port).trim();
  const ch = String(v.channel == null ? '' : v.channel).trim();
  kv[SETTING_KEYS.CHANNEL] = ch === '' ? null : ch;
  const pw = String(v.password == null ? '' : v.password);
  if (pw.length > 0) {
    kv[SETTING_KEYS.PASSWORD] = pw;
  } else if (v.clearPassword) {
    kv[SETTING_KEYS.PASSWORD] = null;
  }
  return { kv };
}

// ---------------------------------------------------------------------------
// 応答の読み取り (デバイス側の細かな表現ゆれを吸収する)
// ---------------------------------------------------------------------------

/**
 * settings.get の応答から表示用の値を取り出す。
 *
 * デバイスは {"keys": {...}, "secret": [...], "bytes": N} を返す。
 * settings.toml が無い場合は {"keys": {}, "missing": 1}。
 * 秘密キーの値は真偽値でしか入っていない (stackee_console.py の _settings_get)。
 * @param {object} frame
 */
export function readSettings(frame) {
  const f = frame || {};
  const answered = !!(f.keys && typeof f.keys === 'object' && !Array.isArray(f.keys));
  const keys = answered ? f.keys : {};
  // ★ デバイスは **設定済みのキーしか返さない**。実機の応答は
  //   {"keys":{"STACKEE_HOST":"192.168.0.106","STACKEE_PORT":"5555"}, ...} のように
  //   SSID やチャネルが丸ごと欠ける。欠けているのは「未設定」であって異常ではない。
  const get = (k) => {
    const val = keys[k];
    return val == null || typeof val === 'boolean' ? '' : String(val);
  };
  // secret はデバイスが「これは秘密扱いのキーだ」と宣言している名前の一覧で、
  // 「設定済み」の意味ではない。値そのものは keys 側に真偽値で入る
  // (古い/別の実装では "***" のような伏せ字文字列で来ることもある)。
  const secretKeys = Array.isArray(f.secret) ? f.secret.map(String) : [];
  let passwordSet = readPasswordState(keys[SETTING_KEYS.PASSWORD]);
  if (passwordSet === null && answered) {
    // 応答は返ってきたのにキーが無い = 未設定。「不明」にはしない。
    passwordSet = false;
  }
  return {
    ssid: get(SETTING_KEYS.SSID),
    channel: get(SETTING_KEYS.CHANNEL),
    host: get(SETTING_KEYS.HOST),
    port: get(SETTING_KEYS.PORT),
    passwordSet,
    secretKeys,
    /** settings.toml がそもそも無い。 */
    missing: f.missing === 1 || f.missing === true,
    /**
     * ★ 起動を 19 秒延ばす CIRCUITPY_WIFI_SSID が残っていないか。
     *   残っていたらページ側で警告する (wifi_autoconnect_design.md §1.1)。
     */
    legacyWifiKey: Object.prototype.hasOwnProperty.call(keys, 'CIRCUITPY_WIFI_SSID'),
  };
}

/**
 * パスワードが設定済みかどうかだけを読む。値そのものは絶対に扱わない。
 *
 * デバイス側は真偽値 (true/false) で返すのを想定しているが、設計書の
 * 「"***" を返す」案でも動くようにしてある。
 * @returns {boolean|null} null は「不明」
 */
export function readPasswordState(v) {
  if (v === true || v === 1) return true;
  if (v === false || v === 0) return false;
  if (typeof v === 'string') {
    if (v === '') return false;
    return true; // "***" などのマスク文字列
  }
  return null;
}

/**
 * hello の応答を読む。
 *
 * デバイスは {"proto":1,"fw":"stackee-console/1","cp":"10.3.0","board":"..."}
 * を返す。**対応コマンドの一覧は返さない** ので、使えるかどうかは
 * 実際に呼んで "unknown:<cmd>" / "unsupported" が返るかで判断する。
 * @returns {{version: number|null, firmware: string, circuitpython: string, board: string}}
 */
export function readHello(frame) {
  const f = frame || {};
  return {
    version: Number.isInteger(f.proto) ? f.proto : null,
    firmware: f.fw == null ? '' : String(f.fw),
    circuitpython: f.cp == null ? '' : String(f.cp),
    board: f.board == null ? '' : String(f.board),
  };
}

/**
 * wifi.scan の応答を一覧にする。
 *
 * 今のファームは全チャネル分を 1 応答 ({"nets":[...]}) で返す (実機 5.09 秒)。
 * 将来 1 行が CDC の送信 FIFO を超えて複数応答に割られても動くよう、
 * 応答 1 個でも配列でも受けられるようにしてある。
 *
 * ★ 応答には settle_ms / settle_passive_ms / chs / t / total_ms といった
 *   計測用の項目も入る。**知らない項目は黙って無視する** こと
 *   (デバイス側が計測値を足しても、ページを直さずに動き続けるように)。
 * ★ nets の各要素は {ssid, ch, rssi} だけ。暗号の有無は返らない。
 * @param {object|object[]} parts
 * @returns {{ssid: string, rssi: number|null, channel: number|null, secure: boolean|null}[]}
 */
export function readScanResults(parts) {
  const list = Array.isArray(parts) ? parts : [parts];
  const seen = new Map();
  for (const part of list) {
    if (!part || typeof part !== 'object') continue;
    const list = Array.isArray(part.nets) ? part.nets : [];
    for (const n of list) {
      const ssid = n && n.ssid != null ? String(n.ssid) : '';
      if (ssid === '') continue;
      const rssi = typeof n.rssi === 'number' ? n.rssi : null;
      const prev = seen.get(ssid);
      // 同じ SSID が複数チャネルに出たら電波の強い方を残す
      if (prev && prev.rssi != null && rssi != null && prev.rssi >= rssi) continue;
      seen.set(ssid, {
        ssid,
        rssi,
        channel: typeof n.ch === 'number' ? n.ch : null,
        secure: n.sec == null ? null : !!n.sec,
      });
    }
  }
  return [...seen.values()].sort((a, b) => (b.rssi ?? -999) - (a.rssi ?? -999));
}

/**
 * status の応答を画面に出せる形にする。
 *
 * デバイスが返すのは (stackee_console.py の _status):
 *   up (秒, 小数), fw, bat (0-100 か null), chg (真偽か null),
 *   hid ('USB'|'BLE'|'?'), ble (真偽か null),
 *   wifi ('off'|'on'|'up'|'?'), ip ('up' のときだけ),
 *   cmds / drops (コンソール自身の統計)
 */
export function readStatus(frame) {
  const f = frame || {};
  return {
    battery: typeof f.bat === 'number' ? f.bat + ' %' : null,
    charging: f.chg == null ? null : (f.chg ? '充電中' : '充電していない'),
    hid: f.hid === '?' || f.hid == null ? null : String(f.hid),
    ble: f.ble == null ? null : (f.ble ? '接続あり' : '接続なし'),
    wifi: formatWifi(f),
    firmware: f.fw == null ? '' : String(f.fw),
    uptime: f.up == null ? null : formatUptime(f.up),
  };
}

/** status の wifi / ip を 1 つの日本語にする。 */
export function formatWifi(frame) {
  const f = frame || {};
  switch (f.wifi) {
    case 'up': return '接続中' + (f.ip ? ' (' + f.ip + ')' : '');
    case 'on': return '有効 (未接続)';
    case 'off': return '無効';
    case '?': return '不明';
    default: return f.wifi == null ? null : String(f.wifi);
  }
}

/** 秒数を「1日 02:03:04」のように読める形にする。 */
export function formatUptime(seconds) {
  const s = Math.max(0, Math.floor(Number(seconds) || 0));
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  const pad = (n) => String(n).padStart(2, '0');
  const hms = pad(h) + ':' + pad(m) + ':' + pad(sec);
  return d > 0 ? d + '日 ' + hms : hms;
}
