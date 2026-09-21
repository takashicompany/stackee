#!/usr/bin/env node
// 操作盤のページを **本物のブラウザで** 人と同じ手順で操作して書き込む。
//
//   node firmware/tools/ota_browser.mjs --image firmware/build-full/stackee.bin
//   node firmware/tools/ota_browser.mjs --image ... --page https://takashi.company/stackee/
//   node firmware/tools/ota_browser.mjs --image ... --no-commit
//
// ★ **なぜこれがあるのか。** `tools/ota.mjs` は操作盤と同じ中核 JS
//   (docs/js/ota.js) を import しているが、**ページそのものは通らない**。
//   ボタンの有効・無効、ファイル選択、確認ダイアログ、進捗の出し方、
//   再起動後の表示 — 人が見るものは 1 つも確かめていなかった。
//   ここはページを本物の Chrome で開いて、**人と同じ順番でクリックする**。
//
// ★ **ヘッドレス。ウィンドウを出さない・フォーカスを奪わない。**
//   ユーザーの作業の邪魔をしないのが約束。
//
// --- 機器選択ダイアログをどう越えるか ---------------------------------------
//
// WebHID の選択ダイアログ (navigator.hid.requestDevice) は**自動化できない**
// (research/stackee/web_flash_2026-09-20.md §3)。越え方は 2 つある。
//
//   1. **使い捨てのプロファイルに許可を書いておく** ← ここが採る道
//      Chrome は「一度許可した機器」を プロファイルの `Preferences` に
//      残す (`hid_chooser_data`)。**シリアル番号を持つ機器だけ**永続する。
//      stackee は持っている (`44B16F3EC808`)。だから起動前にその形で
//      書いておけば、ページは `navigator.hid.getDevices()` で拾える。
//      **root も管理ポリシーも要らない。使い捨てなので後に何も残らない。**
//
//   2. 管理ポリシー `WebHidAllowDevicesForUrls`
//      macOS では `/Library/Managed Preferences/<bundle id>.plist` に置く。
//      **root が要る。**`defaults write <bundle id>` (ユーザー領域) は
//      **効かない** — 実機で試して chrome://policy に 1 つも出なかった
//      (2026-09-22)。Playwright の Chrome for Testing の bundle id は
//      `com.google.chrome.for.testing` (`com.google.ChromeForTesting` ではない)。
//      要るときの中身は README §5 に書いてある。
//
// --- ヘッドレスの種類 --------------------------------------------------------
//
// Playwright の既定のヘッドレスは `chromium_headless_shell` で、これは
// 機能を削った別物。**`channel: 'chromium'` を渡して「新しいヘッドレス」
// (中身は普通の Chrome) にする。**実測: どちらでも `navigator.hid` は
// 生えているが、機器を拾えるのは後者だけ…ではなく、**許可さえあれば
// どちらでも拾えた**。念のため機能の揃っている後者を使う。
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const FW = path.dirname(HERE);                 // firmware/
const DOCS = path.join(path.dirname(FW), 'docs');

export const USB_VID = 0x303A;
export const USB_PID = 0x811A;
export const DEFAULT_PORT = 8730;

// ---------------------------------------------------------------------------
// 引数 (ここは純粋な関数。ホストテストが直に叩く)
// ---------------------------------------------------------------------------
export function parseArgs(argv) {
  const out = {
    commit: true, port: DEFAULT_PORT, page: 'local', timeoutMs: 300000,
    keepProfile: false, verbose: false,
  };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a === '--image' || a === '-i') out.image = argv[++i];
    else if (a === '--page') out.page = argv[++i];
    else if (a === '--port') out.port = Number(argv[++i]);
    else if (a === '--timeout') out.timeoutMs = Number(argv[++i]) * 1000;
    else if (a === '--serial') out.serial = argv[++i];
    else if (a === '--no-commit') out.commit = false;
    else if (a === '--commit') out.commit = true;
    else if (a === '--keep-profile') out.keepProfile = true;
    else if (a === '--verbose' || a === '-v') out.verbose = true;
    else if (a === '--help' || a === '-h') out.help = true;
    else if (!out.image && !a.startsWith('-')) out.image = a;
    else throw new Error(`知らない引数: ${a}`);
  }
  if (!Number.isFinite(out.port) || out.port <= 0 || out.port > 65535) {
    throw new Error(`--port が変です: ${out.port}`);
  }
  if (!Number.isFinite(out.timeoutMs) || out.timeoutMs <= 0) {
    throw new Error('--timeout が変です');
  }
  return out;
}

/** 開く URL。`local` なら手元の docs/ を配って 127.0.0.1 で開く。 */
export function pageUrl(opts) {
  if (opts.page === 'local') return `http://127.0.0.1:${opts.port}/`;
  if (!/^https?:\/\//.test(opts.page)) {
    throw new Error(`--page は local か http(s) の URL: ${opts.page}`);
  }
  return opts.page;
}

/** 許可を書き込む相手のオリジン (末尾のパスは落とす)。 */
export function originOf(url) {
  return new URL(url).origin;
}

// ---------------------------------------------------------------------------
// 完了の読み取り (ページの文面をそのまま読む。ここも純粋)
// ---------------------------------------------------------------------------
// 成功の文面は docs/js/ota.js の attachOtaUi が出すもの:
//   書き込んで切り替えました (58.9 秒)。いまは 0123456789abcdef… で動いています。
//   書き込みました (58.9 秒)。**まだ切り替えていません。**
export function parseDone(text) {
  if (!text) return null;
  const secs = text.match(/\((\d+(?:\.\d+)?)\s*秒\)/);
  const sha = text.match(/いまは\s*([0-9a-f]{8,64})/i);
  const committed = text.includes('切り替えました');
  const notCommitted = text.includes('まだ切り替えていません');
  if (!committed && !notCommitted) return null;
  return {
    committed,
    seconds: secs ? Number(secs[1]) : null,
    sha16: sha ? sha[1].toLowerCase() : null,
  };
}

// ---------------------------------------------------------------------------
// 手元の docs/ を配る小さな HTTP (依存を足さない)
// ---------------------------------------------------------------------------
const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.bin': 'application/octet-stream',
  '.svg': 'image/svg+xml',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
};

export function contentType(file) {
  return TYPES[path.extname(file).toLowerCase()] || 'application/octet-stream';
}

/** `root` の下だけを配る。`..` で外へ出られない。 */
export function resolveUnder(root, urlPath) {
  const clean = decodeURIComponent(urlPath.split('?')[0]);
  const rel = clean.endsWith('/') ? clean + 'index.html' : clean;
  const full = path.resolve(root, '.' + rel);
  if (full !== root && !full.startsWith(root + path.sep)) return null;
  return full;
}

function serve(root, port) {
  const server = http.createServer((req, res) => {
    const file = resolveUnder(root, req.url || '/');
    if (file === null || !fs.existsSync(file) || fs.statSync(file).isDirectory()) {
      res.statusCode = 404;
      res.end('not found');
      return;
    }
    res.setHeader('Content-Type', contentType(file));
    res.end(fs.readFileSync(file));
  });
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, '127.0.0.1', () => resolve(server));
  });
}

// ---------------------------------------------------------------------------
// 使い捨てプロファイルに「この機器を許可した」と書いておく
// ---------------------------------------------------------------------------
// Chrome の形そのまま (chrome/browser/hid/hid_chooser_context.cc の
// DeviceInfoToValue)。**シリアル番号がある機器だけ**この形で永続する。
export function hidGrantPreferences(origin, device) {
  return {
    profile: {
      content_settings: {
        exceptions: {
          hid_chooser_data: {
            [`${origin},*`]: {
              last_modified: '13400000000000000',
              setting: {
                'chosen-objects': [{
                  name: device.name,
                  'product-id': device.productId,
                  'serial-number': device.serial,
                  'vendor-id': device.vendorId,
                }],
              },
            },
          },
        },
      },
    },
  };
}

/** node-hid で stackee のシリアル番号を拾う (無ければ null)。 */
async function findDevice() {
  let hid;
  try {
    const require = (await import('node:module')).createRequire(import.meta.url);
    hid = require('node-hid');
  } catch (e) {
    return null;
  }
  for (const d of hid.devices(USB_VID, USB_PID)) {
    if (d.serialNumber) {
      return {
        name: d.product || 'stackee',
        vendorId: USB_VID, productId: USB_PID, serial: d.serialNumber,
      };
    }
  }
  return null;
}

// Playwright はこの木の依存ではない (操作盤の配信物にも node-hid の隣にも
// 入れたくない)。**入っているものを探して使う**: まず普通に import し、
// 駄目なら `npm root -g` の下を見る。
async function loadPlaywright() {
  try {
    return await import('playwright');
  } catch (e) {
    const { execFileSync } = await import('node:child_process');
    let root;
    try {
      root = execFileSync('npm', ['root', '-g'], { encoding: 'utf8' }).trim();
    } catch (e2) {
      throw new Error('playwright が見つかりません。`npm i -g playwright` か'
        + ' `npm i playwright` を入れてください。');
    }
    const at = path.join(root, 'playwright', 'index.mjs');
    if (!fs.existsSync(at)) {
      throw new Error(`playwright が見つかりません (${at})。`
        + ' `npm i -g playwright` を入れてください。');
    }
    return await import(fileURLToPath(new URL('file://' + at)));
  }
}

// ---------------------------------------------------------------------------
const USAGE = `使い方:
  node tools/ota_browser.mjs --image <stackee.bin> [--no-commit] [-v]

  --page local            手元の docs/ を 127.0.0.1 で配って開く (既定)
  --page <URL>            公開ページ (https://takashi.company/stackee/) を開く
  --port <n>              手元で配るときの番号 (既定 ${DEFAULT_PORT})
  --serial <文字列>        機器のシリアル番号 (既定は node-hid で拾う)
  --timeout <秒>          書き込み全体の上限 (既定 300)
  --keep-profile          使い捨てプロファイルを消さない (調べるとき)
  --no-commit             書くだけで切り替えない (ページのチェックを外す)

★ ウィンドウは出さない (ヘッドレス)。機器の許可は使い捨てプロファイルに
  書いてから開くので、選択ダイアログは出ない。
`;

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (args.help || !args.image) {
    process.stdout.write(USAGE);
    return args.help ? 0 : 2;
  }
  const imagePath = path.resolve(args.image);
  if (!fs.existsSync(imagePath)) throw new Error(`像がありません: ${imagePath}`);
  const size = fs.statSync(imagePath).size;

  const device = args.serial
    ? { name: 'stackee', vendorId: USB_VID, productId: USB_PID, serial: args.serial }
    : await findDevice();
  if (!device) {
    throw new Error('stackee が USB に見えません (node-hid で拾えませんでした)。'
      + ' --serial でシリアル番号を渡すこともできます。');
  }

  const { chromium } = await loadPlaywright();

  let server = null;
  if (args.page === 'local') {
    server = await serve(path.resolve(DOCS), args.port);
    process.stderr.write(`# [serve] ${DOCS} を http://127.0.0.1:${args.port}/ で配っています\n`);
  }
  const url = pageUrl(args);
  const origin = originOf(url);

  const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'stackee-ota-'));
  fs.mkdirSync(path.join(profile, 'Default'), { recursive: true });
  fs.writeFileSync(path.join(profile, 'Default', 'Preferences'),
                   JSON.stringify(hidGrantPreferences(origin, device)));
  process.stderr.write(`# [grant] ${origin} に ${device.name}`
    + ` (VID ${USB_VID.toString(16)} / PID ${USB_PID.toString(16)}`
    + ` / シリアル ${device.serial}) を許可しました\n`);

  const started = Date.now();
  let ctx = null;
  let code = 1;
  try {
    // ★ channel: 'chromium' = 「新しいヘッドレス」(中身は普通の Chrome)。
    //   既定の headless shell は機能を削った別物なので使わない。
    ctx = await chromium.launchPersistentContext(profile, {
      headless: true, channel: 'chromium',
    });
    const page = ctx.pages()[0] || await ctx.newPage();
    // ★ ページは書き込む前に window.confirm() で聞く。人が「はい」を押す
    //   ところ。既定では Playwright が黙って「いいえ」にするので受ける。
    page.on('dialog', (d) => d.accept());
    if (args.verbose) {
      page.on('console', (m) => process.stderr.write(`#   [page] ${m.text()}\n`));
    }
    await page.goto(url, { waitUntil: 'load' });
    process.stderr.write(`# [open] ${url}\n`);

    // 1. 接続する (許可済みなので選択ダイアログは出ない)
    await page.selectOption('#conn-transport', 'hid');
    await page.click('#btn-connect');
    await page.waitForFunction(
      () => document.getElementById('conn-state').textContent.includes('接続'),
      null, { timeout: 30000 });
    const conn = await page.textContent('#conn-state');
    process.stderr.write(`# [connect] ${conn.trim()}\n`);

    // 2. いま載っている版を見る
    await page.click('#btn-ota-info');
    await page.waitForFunction(
      () => document.getElementById('ota-running-ver').textContent !== '—',
      null, { timeout: 30000 });
    const before = {
      running: (await page.textContent('#ota-running')).trim(),
      version: (await page.textContent('#ota-running-ver')).trim(),
      sha256: (await page.textContent('#ota-running-sha')).trim(),
      next: (await page.textContent('#ota-next')).trim(),
    };
    process.stderr.write(`# [info] いま ${before.running} / ${before.version}`
      + ` / ${before.sha256}\n`);

    // 3. 手元の .bin を「ファームを選ぶ」に渡す
    await page.setInputFiles('#ota-file', imagePath);
    await page.waitForFunction(
      () => document.getElementById('ota-new-sha').textContent !== '—',
      null, { timeout: 30000 });
    const chosen = {
      name: (await page.textContent('#ota-new-name')).trim(),
      size: (await page.textContent('#ota-new-size')).trim(),
      sha256: (await page.textContent('#ota-new-sha')).trim(),
      fileSha: (await page.textContent('#ota-new-file-sha')).trim(),
    };
    process.stderr.write(`# [pick] ${chosen.name} / ${chosen.size}`
      + ` / 名札 ${chosen.sha256}\n`);

    if (!args.commit) {
      await page.uncheck('#chk-ota-commit');
    }

    // 4. 書き込む
    await page.waitForSelector('#btn-ota-write:not([disabled])', { timeout: 30000 });
    const writeAt = Date.now();
    await page.click('#btn-ota-write');
    let lastStep = '';
    let lastPct = -10;
    const deadline = writeAt + args.timeoutMs;
    for (;;) {
      if (Date.now() > deadline) throw new Error('書き込みが終わりません (時間切れ)');
      const state = await page.evaluate(() => ({
        step: (document.getElementById('ota-step') || {}).textContent || '',
        done: document.getElementById('ota-done').hidden
          ? null : document.getElementById('ota-done').textContent,
        error: document.getElementById('ota-error').hidden
          ? null : document.getElementById('ota-error').textContent,
      }));
      // ★ 進み具合は 10% ごとだけ出す (毎回出すとログが埋まる)。
      if (state.step && state.step !== lastStep) {
        const pct = state.step.match(/^(\d+)%/);
        if (!pct || Number(pct[1]) >= lastPct + 10) {
          if (pct) lastPct = Number(pct[1]);
          process.stderr.write(`# [write] ${state.step}\n`);
        }
        lastStep = state.step;
      }
      if (state.error) {
        process.stderr.write(`\n# [error] ${state.error.trim()}\n`);
        throw new Error(state.error.trim());
      }
      if (state.done) {
        process.stderr.write('\n');
        const parsed = parseDone(state.done);
        const wall = (Date.now() - writeAt) / 1000;
        process.stderr.write(`# [done] ${state.done.trim()}\n`);
        // 5. 再起動後の表示をページから読む
        await page.waitForFunction(
          () => document.getElementById('ota-running-ver').textContent !== '—',
          null, { timeout: 60000 });
        const after = {
          running: (await page.textContent('#ota-running')).trim(),
          version: (await page.textContent('#ota-running-ver')).trim(),
          sha256: (await page.textContent('#ota-running-sha')).trim(),
          next: (await page.textContent('#ota-next')).trim(),
        };
        process.stderr.write(`# [after] いま ${after.running} / ${after.version}`
          + ` / ${after.sha256}\n`);
        const kbs = size / 1024 / Math.max(0.001, parsed && parsed.seconds
          ? parsed.seconds : wall);
        process.stderr.write(`# 転送 ${(parsed && parsed.seconds) || '?'} 秒`
          + ` (${kbs.toFixed(1)} KB/s) / ボタンを押してから ${wall.toFixed(1)} 秒\n`);
        process.stdout.write(JSON.stringify({
          ok: true, committed: !!(parsed && parsed.committed),
          seconds: parsed && parsed.seconds, kbs: Number(kbs.toFixed(1)),
          wall_s: Number(wall.toFixed(1)), size,
          page: url, before, chosen, after,
          elapsed_s: Number(((Date.now() - started) / 1000).toFixed(1)),
        }) + '\n');
        code = 0;
        break;
      }
      await new Promise((r) => setTimeout(r, 250));
    }
  } finally {
    if (ctx) await ctx.close();
    if (server) server.close();
    if (!args.keepProfile) fs.rmSync(profile, { recursive: true, force: true });
    else process.stderr.write(`# プロファイル: ${profile}\n`);
  }
  return code;
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().then((c) => process.exit(c), (err) => {
    process.stderr.write(`\n${err && err.stack ? err.stack : err}\n`);
    process.exit(1);
  });
}
