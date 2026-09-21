#!/usr/bin/env node
// stackee のファームウェアを **Raw HID 経由で** 書き換える (Node 版)。
//
//   node firmware/tools/ota.mjs --image firmware/build-full/stackee.bin
//   node firmware/tools/ota.mjs --image ... --no-commit   # 書くだけ。切り替えない
//   node firmware/tools/ota.mjs --commit                  # 書いてある next に切り替える
//   node firmware/tools/ota.mjs --info                    # いま載っている版を見るだけ
//   node firmware/tools/ota.mjs --abort                   # 途中で止まった転送を畳む
//
// ★ 中核 (枠の組み立て・credit・sha256 の照合・ota.* の順序) は
//   **操作盤のページとまったく同じ** docs/js/ota.js を import している。
//   ここにあるのは「node-hid で運ぶ」ぶんだけ。人が押すボタンと AI が叩く
//   この道具が食い違いようがない、というのがこの形の目的
//   (research/stackee/web_flash_2026-09-20.md §3-3)。
//
// 要るもの: node-hid。firmware/tools/package.json に入れてある。
//   cd firmware/tools && npm install
// ★ 操作盤の配信物 (docs/) には Node の依存を一切混ぜていない。
//   node-hid はネイティブ拡張なので、ブラウザ側とは置き場を分けてある。
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { Demux, RequestTracker, PROTOCOL } from '../../docs/js/protocol.js';
import {
  OTA, parseStatusReport, runOta, sha256Hex, looksLikeEspImage,
  embeddedSha, verifyEmbeddedSha,
} from '../../docs/js/ota.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));

// docs/js/hid.js の USB_FILTER と同じ面。
const USB_VID = 0x303a;
const USB_PID = 0x811a;
const USAGE_PAGE = 0xff60;
const USAGE = 0x61;

// stackee_conhid.h。
const CMD_TX = 0xc0;
const CMD_RX = 0xc1;
const REPORT_SIZE = 32;
const HEADER_SIZE = 3;
const MAX_PAYLOAD = REPORT_SIZE - HEADER_SIZE;   // 29

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---------------------------------------------------------------------------
// node-hid の転送層 (docs/js/hid.js と同じ口を持つ)
// ---------------------------------------------------------------------------
class NodeHidLink {
  constructor(device, opts = {}) {
    this.device = device;
    this.tracker = new RequestTracker();
    this._demux = new Demux();
    this._decoder = new TextDecoder('utf-8');
    this._otaListeners = new Set();
    this._pollPaused = false;
    this._closing = false;
    this._txChain = Promise.resolve();
    this.verbose = !!opts.verbose;
    this.logs = [];

    this._attach(device);
    this._sweep = setInterval(() => this.tracker.sweep(), 200);
    this._pollLoop = this._poll();
  }

  /**
   * デバイスに聞き耳を付ける。
   *
   * ★ **'error' を必ず付けること。** node-hid の HID は EventEmitter なので、
   *   聞き手のいない 'error' は Node の既定で**プロセスごと落とす**。
   *   再起動を待っている間に device が消えると読みが失敗して 'error' が
   *   飛ぶので、付け忘れると「commit のあと必ず落ちる」ことになる
   *   (2026-09-21 に実機で踏んだ。waitAndReconnect が開き直した device に
   *   'data' しか付けていなかった)。
   */
  _attach(device) {
    this.device = device;
    device.on('data', (buf) => this._onReport(buf));
    device.on('error', (err) => {
      // 再起動の前後で読み書きが失敗するのは想定どおり。黙って畳む。
      if (!this._closing && this.verbose) process.stderr.write(`HID: ${err}\n`);
    });
  }

  _onReport(buf) {
    const a = new Uint8Array(buf);
    if (a.length === 0) return;
    if (a[0] === OTA.CMD_DATA) {
      const st = parseStatusReport(a);
      for (const cb of this._otaListeners) {
        try { cb(st); } catch (e) { /* 聞き手の都合で受信を止めない */ }
      }
      return;
    }
    if (a[0] !== CMD_RX) return;          // 0xC0 / 0xC2 の ack は使わない
    const len = Math.min(a[1], MAX_PAYLOAD);
    if (len === 0) return;
    const out = this._demux.push(
      this._decoder.decode(a.slice(HEADER_SIZE, HEADER_SIZE + len), { stream: true }));
    if (out.text) {
      this.logs.push(out.text);
      if (this.verbose) process.stderr.write(out.text);
    }
    for (const f of out.frames) this.tracker.onFrame(f);
  }

  /** hidapi の約束: 先頭に Report ID (このデバイスは 0) を足して書く。 */
  _write(report) {
    const buf = Buffer.alloc(REPORT_SIZE + 1);
    buf[0] = 0x00;
    Buffer.from(report).copy(buf, 1);
    this.device.write(buf);
  }

  _serial(fn) {
    const next = this._txChain.then(fn, fn);
    this._txChain = next.then(() => {}, () => {});
    return next;
  }

  async _poll() {
    while (!this._closing) {
      if (!this._pollPaused) {
        const rep = Buffer.alloc(REPORT_SIZE);
        rep[0] = CMD_RX;
        try {
          await this._serial(() => this._write(rep));
        } catch (e) {
          // ★ 再起動の前後では必ずここへ来る (デバイスが消えている)。
          //   騒がない。開き直すのは waitAndReconnect の仕事。
          if (!this._closing && this.verbose) {
            process.stderr.write(`受信が途切れました: ${e}\n`);
          }
          return;
        }
      }
      await sleep(this.tracker.pendingCount > 0 ? 2 : 15);
    }
  }

  async request(cmd, args, opts) {
    const { line, promise } = this.tracker.create(cmd, args, opts);
    // ★ **ここで先に聞き手を付けておく。** 下の書き込みが失敗すると
    //   request() はその例外で終わるが、tracker に積んだ promise は
    //   宙に浮いたまま残り、あとで sweep() が reject する。聞き手が
    //   いない reject は Node 22 の既定でプロセスごと落とす
    //   (2026-09-21、再起動を待っている最中に実機で踏んだ)。
    promise.catch(() => {});
    const bytes = new TextEncoder().encode(line);
    try {
      await this._serial(() => {
        for (let off = 0; off < bytes.length; off += MAX_PAYLOAD) {
          const chunk = bytes.subarray(off, Math.min(off + MAX_PAYLOAD, bytes.length));
          const rep = new Uint8Array(REPORT_SIZE);
          rep[0] = CMD_TX;
          rep[1] = chunk.length;
          rep.set(chunk, HEADER_SIZE);
          this._write(rep);
        }
      });
    } catch (e) {
      // 書けなかった = このコマンドは届いていない。待たせない。
      this.tracker.abortAll('送信に失敗しました');
      throw e;
    }
    return promise;
  }

  sendOtaReport(report) {
    return this._serial(() => this._write(report));
  }

  onOtaStatus(cb) {
    this._otaListeners.add(cb);
    return () => this._otaListeners.delete(cb);
  }

  setPollPaused(paused) {
    this._pollPaused = !!paused;
  }

  /** 再起動のあと、同じ面が戻ってくるのを待って開き直す。 */
  async waitAndReconnect(opts = {}) {
    const timeoutMs = opts.timeoutMs || 40000;
    const deadline = Date.now() + timeoutMs;
    this.close();
    await sleep(1500);
    while (Date.now() < deadline) {
      let probe = null;
      try {
        probe = await openDevice();
      } catch (e) { /* まだ現れていない */ }
      if (probe) {
        // ★ 開けただけでは足りない。列挙の途中を掴むと、開けたのに
        //   読み書きが失敗する時間帯がある。1 往復できて初めて「戻った」。
        this._closing = false;
        this._demux.reset();
        this._txChain = Promise.resolve();
        this._attach(probe.device);
        this._sweep = setInterval(() => this.tracker.sweep(), 200);
        this._pollLoop = this._poll();
        try {
          await this.request('hello', null, { timeoutMs: 2000 });
          return true;
        } catch (e) {
          this.close();       // まだ答えない。畳んで次の周回に賭ける
        }
      }
      await sleep(400);
    }
    return false;
  }

  close() {
    this._closing = true;
    if (this._sweep) clearInterval(this._sweep);
    this._sweep = null;
    const device = this.device;
    if (device) {
      // ★ 'data' は外すが、**'error' は外さずに「何もしない聞き手」へ
      //   差し替える**。閉じたあとにも遅れて飛んでくることがあり、
      //   聞き手が 1 人もいない EventEmitter の 'error' は
      //   プロセスごと落とすため。
      try { device.removeAllListeners('data'); } catch (e) { /* 無視 */ }
      try {
        device.removeAllListeners('error');
        device.on('error', () => {});
      } catch (e) { /* 無視 */ }
      try { device.close(); } catch (e) { /* 無視 */ }
    }
  }
}

// ---------------------------------------------------------------------------
async function openDevice() {
  let HID;
  try {
    HID = await import('node-hid');
  } catch (e) {
    throw new Error(
      'node-hid がありません。firmware/tools で `npm install` してください。\n' +
      `  (${e && e.message})`);
  }
  const hid = HID.default || HID;
  const found = hid.devices().filter(
    (d) => d.vendorId === USB_VID && d.productId === USB_PID &&
           d.usagePage === USAGE_PAGE && d.usage === USAGE);
  if (found.length === 0) {
    throw new Error(
      `stackee の Raw HID が見えません ` +
      `(VID 0x${USB_VID.toString(16)} / PID 0x${USB_PID.toString(16)} / ` +
      `usagePage 0x${USAGE_PAGE.toString(16)} / usage 0x${USAGE.toString(16)})。` +
      'USB に刺さっているか、像が full プロファイルかを確かめてください。');
  }
  const device = new hid.HID(found[0].path);
  // ★ _attach するまでの隙間でも 'error' で落ちないように、先に 1 つ置く。
  device.on('error', () => {});
  return { device, info: found[0] };
}

function parseArgs(argv) {
  const out = { commit: true, verbose: false };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--image' || a === '-i') out.image = argv[++i];
    else if (a === '--no-commit') out.commit = false;
    // ★ --image 無しの --commit は「書いてある next へ切り替えるだけ」。
    //   --no-commit で書き終えたあと、転送し直さずに切り替えられる。
    else if (a === '--commit') { out.commit = true; out.commitOnly = true; }
    else if (a === '--force') out.force = true;
    else if (a === '--info') out.info = true;
    else if (a === '--abort') out.abort = true;
    else if (a === '--status') out.status = true;
    else if (a === '--verbose' || a === '-v') out.verbose = true;
    else if (a === '--help' || a === '-h') out.help = true;
    else if (!out.image && !a.startsWith('-')) out.image = a;
  }
  return out;
}

const USAGE_TEXT = `使い方:
  node tools/ota.mjs --image <stackee.bin> [--no-commit] [--force] [-v]
  node tools/ota.mjs --commit    書いてある next へ切り替えて再起動する
  node tools/ota.mjs --info      いま載っている版と sha256 を見る
  node tools/ota.mjs --status    進行中の転送の様子
  node tools/ota.mjs --abort     途中で止まった転送を畳む

  --no-commit  もう片方の区画に書くところまでで止める (切り替えない)
  --commit     像を渡さずに単体で使うと「書き終えてある next に切り替える」
  --force      同じ sha256 が動いていても書く / 切り替える
`;

// ---------------------------------------------------------------------------
// --image 無しの --commit — 書き終えてある next へ切り替えるだけ
// ---------------------------------------------------------------------------
// ★ `--no-commit` で書いたあと「やっぱり切り替える」に使う。像をもう一度
//   送らない (1.4 MB = 約 60 秒を捨てない)。
//
// 断るところ:
//   ・app.info が無い (OTA 非対応の古い像)
//   ・next が無い / 名札が読めない
//   ・next が running と同じ名札 (切り替えても何も変わらない) … --force で通す
//   ・本体が `notready` を返す (ota.end が通っていない = まだ何も書いていない、
//     か、書いたあとに再起動して転送の状態が消えている)
//
// link は runOta と同じ口 (request / 任意の waitAndReconnect)。テストから
// 偽の link を渡せるように、ここは node-hid を一切知らない。
function sameSha(a, b) {
  return typeof a === 'string' && typeof b === 'string' && a.length === 64
    && a.toLowerCase() === b.toLowerCase();
}

export async function commitWritten(link, opts = {}) {
  const onStep = opts.onStep || (() => {});
  const sleep = opts.sleep || ((ms) => new Promise((r) => setTimeout(r, ms)));

  onStep('info', '書いてある版を読んでいます');
  const before = await link.request('app.info', null, { timeoutMs: 15000 });
  if (!before || before.error) {
    const e = new Error('この像は app.info を持っていません (OTA 非対応の古いファーム)。');
    e.code = 'unsupported';
    throw e;
  }
  const next = before.next || null;
  const running = before.running || {};
  if (!next || !next.sha256) {
    const e = new Error('切り替え先 (next) の名札を読めません。まだ何も書いていない可能性があります。');
    e.code = 'nonext';
    e.detail = before;
    throw e;
  }
  if (sameSha(next.sha256, running.sha256) && !opts.force) {
    const e = new Error(
      'next にはいま動いているのと同じ像が入っています (切り替えても変わりません)。'
      + ' --force で通せます。');
    e.code = 'same';
    e.detail = before;
    throw e;
  }
  const wantImage = next.sha256;

  onStep('commit', '起動する側を切り替えています');
  const committed = await link.request('ota.commit', null, { timeoutMs: 10000 });
  if (!committed || !committed.ok) {
    const why = committed && committed.error;
    const e = new Error(why === 'notready'
      ? '本体が notready を返しました (ota.end が通っていません)。'
        + ' --image を渡して書き直してください。'
      : '起動区画を切り替えられませんでした。');
    e.code = why === 'notready' ? 'notready' : 'commit';
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
  let after = null;
  for (let i = 0; i < 20; i += 1) {
    try {
      after = await link.request('app.info', null, { timeoutMs: 5000 });
      if (after && after.running) break;
    } catch (e) { /* まだ起きていない */ }
    await sleep(1000);
  }
  if (!sameSha(after && after.running && after.running.sha256, wantImage)) {
    const e = new Error('再起動しましたが、動いている像が切り替え先と違います。');
    e.code = 'verify';
    e.detail = after;
    throw e;
  }
  onStep('done', '新しい版で動いています');
  return { committed: true, before, after, wantImage };
}

function fmtPart(name, p) {
  if (!p) return `  ${name}: (なし)`;
  return `  ${name}: ${p.label} ver=${p.version || '?'} sha256=${p.sha256 || '(なし)'}`;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help) {
    process.stdout.write(USAGE_TEXT);
    return 0;
  }

  const { device, info } = await openDevice();
  process.stderr.write(`# ${info.product || 'stackee'} ${info.path}\n`);
  const link = new NodeHidLink(device, { verbose: args.verbose });

  try {
    if (args.abort) {
      const r = await link.request('ota.abort', null, { timeoutMs: 10000 });
      process.stdout.write(JSON.stringify(r) + '\n');
      return 0;
    }
    if (args.status) {
      const r = await link.request('ota.status', null, { timeoutMs: 10000 });
      process.stdout.write(JSON.stringify(r, null, 2) + '\n');
      return 0;
    }
    if (args.commitOnly && !args.image) {
      const r = await commitWritten(link, {
        force: args.force,
        onStep: (kind, text) => process.stderr.write(`# [${kind}] ${text}\n`),
      });
      process.stderr.write('# 切り替えて再起動しました。新しい版で動いています。\n');
      process.stderr.write(fmtPart('running', r.after.running) + '\n');
      process.stderr.write(fmtPart('next', r.after.next) + '\n');
      process.stdout.write(JSON.stringify({
        ok: true, committed: true, image_sha256: r.wantImage,
        before: r.before, after: r.after,
      }) + '\n');
      return 0;
    }
    if (args.info || !args.image) {
      const r = await link.request('app.info', null, { timeoutMs: 20000 });
      process.stdout.write(JSON.stringify(r, null, 2) + '\n');
      if (!args.image && !args.info) {
        process.stderr.write('\n' + USAGE_TEXT);
      }
      return 0;
    }

    const file = path.resolve(args.image);
    const image = new Uint8Array(fs.readFileSync(file));
    if (!looksLikeEspImage(image)) {
      throw new Error(`${file} は ESP32 のアプリ像ではありません (先頭が 0xE9 ではない)`);
    }
    if (!(await verifyEmbeddedSha(image))) {
      throw new Error(
        `${file} には末尾の SHA-256 が付いていません ` +
        '(途中で切れているか、hash_appended 無しでビルドされています)');
    }
    // ★ sha256 は 2 種類ある (docs/js/ota.js の embeddedSha を読むこと)。
    const want = await sha256Hex(image);        // ファイル全体 = 転送の照合
    const wantImage = embeddedSha(image);       // 埋め込み = 像の名札
    process.stderr.write(`# 像: ${file}\n`);
    process.stderr.write(`#     ${image.length} B\n`);
    process.stderr.write(`#     sha256       ${want} (ファイル全体)\n`);
    process.stderr.write(`#     image_sha256 ${wantImage} (esptool image_info と同じ)\n`);

    const started = Date.now();
    let lastShown = 0;
    const result = await runOta(link, image, {
      commit: args.commit,
      force: args.force,
      onStep: (kind, text) => process.stderr.write(`# [${kind}] ${text}\n`),
      onProgress: (p) => {
        const pct = Math.floor((p.accepted / p.size) * 100);
        if (pct !== lastShown) {
          lastShown = pct;
          const kbs = p.accepted / Math.max(1, (Date.now() - started) / 1000) / 1024;
          process.stderr.write(
            `\r#   ${pct}%  ${p.accepted}/${p.size} B  ${kbs.toFixed(1)} KB/s   `);
        }
      },
    });
    process.stderr.write('\n');

    if (result.skipped) {
      process.stderr.write('# すでに同じ像が動いています (--force で上書きできます)\n');
      process.stdout.write(JSON.stringify(
        { skipped: true, sha256: result.want, image_sha256: result.wantImage }) + '\n');
      return 0;
    }
    const secs = (result.transfer.ms / 1000).toFixed(1);
    const kbs = (result.size / 1024 / Math.max(0.001, result.transfer.ms / 1000)).toFixed(1);
    process.stderr.write(`# 転送 ${secs} 秒 (${kbs} KB/s、送り直し ${result.transfer.resyncs} 回)\n`);
    process.stderr.write(`# 本体が数えた sha256    : ${result.ended.sha256}\n`);
    process.stderr.write(`# 区画から読み直した名札 : ${result.ended.partition_sha256}\n`);
    if (result.committed) {
      process.stderr.write('# 切り替えて再起動しました。新しい版で動いています。\n');
      process.stderr.write(fmtPart('running', result.after.running) + '\n');
    } else {
      process.stderr.write('# 書き込みました。**まだ切り替えていません** '
                          + '(切り替えるには --commit)。\n');
    }
    process.stdout.write(JSON.stringify({
      ok: true, committed: !!result.committed, sha256: result.want,
      image_sha256: result.wantImage, size: result.size, ms: result.transfer.ms, resyncs: result.transfer.resyncs,
      end: result.ended, after: result.after || null,
    }) + '\n');
    return 0;
  } finally {
    link.close();
  }
}

// ★ 直に起動されたときだけ走る (テストから import しても勝手に動かない)。
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().then((code) => process.exit(code), (err) => {
    process.stderr.write(`\n${err && err.stack ? err.stack : err}\n`);
    process.exit(1);
  });
}

export { NodeHidLink, openDevice, HERE, PROTOCOL };
