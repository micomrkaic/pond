// pond — a numerically honest wave tank as screen candy
// Copyright (C) 2026 Mico <https://github.com/micomrkaic>; GPL-3.0-or-later
//
// The browser build, in a headless Chromium (WebGL2 on SwiftShader): it
// starts, gets a GL context, takes its options from the address (?attract
// must reach main as --attract and find the preloaded demos), draws, takes a
// click, and says nothing on the console's error channel.  Then a second page:
// the sea in a gale (?preset=sea, by name), breaking turned on with shift-B
// through the page's own key function, so the foam's shader runs in WebGL too.
//
//   make web && node tests/web_smoke.js build/web [shot.png]
//
// Needs playwright (npm i playwright && npx playwright install chromium).
const http = require('http');
const fs = require('fs');
const path = require('path');
const { chromium } = require('playwright');

const dir = path.resolve(process.argv[2] || 'build/web');
const shot = process.argv[3];
const types = { '.html': 'text/html', '.js': 'text/javascript', '.wasm': 'application/wasm', '.data': 'application/octet-stream' };

const server = http.createServer((req, res) => {
  let file = path.join(dir, decodeURIComponent(new URL(req.url, 'http://x').pathname));
  if (!file.startsWith(dir)) { res.writeHead(403); res.end(); return; }
  if (fs.existsSync(file) && fs.statSync(file).isDirectory()) file = path.join(file, 'index.html');
  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' });
    res.end(buf);
  });
});

(async () => {
  await new Promise(r => server.listen(0, '127.0.0.1', r));
  const url = `http://127.0.0.1:${server.address().port}/?attract&preset=pool&grid=128`;
  const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const logs = [], errors = [];
  page.on('console', m => (m.type() === 'error' ? errors : logs).push(m.text()));
  page.on('pageerror', e => errors.push('pageerror: ' + e.message));

  await page.goto(url, { waitUntil: 'load' });
  const page2 = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const logs2 = [], errors2 = [];
  page2.on('console', m => (m.type() === 'error' ? errors2 : logs2).push(m.text()));
  page2.on('pageerror', e => errors2.push('pageerror: ' + e.message));
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline && !logs.some(l => l.startsWith('attract:'))) await page.waitForTimeout(250);
  await page.waitForTimeout(2000);
  await page.mouse.click(640, 450);
  await page.waitForTimeout(1000);
  // a picture for a person to look at, not a check: software GL on a small CI
  // runner can be too busy to hand one over in time, and that is no failure
  if (shot) await page.screenshot({ path: shot, timeout: 15000 }).catch(e => console.log('no screenshot: ' + e.message.split('\n')[0]));
  await page.close();

  /* the sea in a gale; shift-B, as the keyboard (and the touch bar) sends it */
  await page2.goto(url.replace(/\?.*$/, '?preset=sea&wind=25&break-slope=0.1&grid=128'), { waitUntil: 'load' });
  const until = Date.now() + 30000;
  while (Date.now() < until && !logs2.some(l => l.startsWith('caustics:'))) await page2.waitForTimeout(250);
  const pressed = await page2.evaluate(() => {
    if (!window.Module || !Module.ccall) return false;
    Module.ccall('pond_key', null, ['number', 'number'], ['b'.charCodeAt(0), 1]);
    return true;
  });
  await page2.waitForTimeout(4000);
  await browser.close();
  server.close();

  let fails = 0;
  const check = (ok, what) => { console.log(`${what}  ${ok ? ' ok' : ' FAILED'}`); if (!ok) fails++; };
  check(logs.some(l => l.startsWith('GL:')), 'a WebGL2 context');
  check(logs.some(l => l.startsWith('caustics:')), 'the scene is built');
  check(logs.some(l => /^attract: [1-9]/.test(l)), '?attract reaches the program and finds the demos');
  check(errors.length === 0, 'nothing on the error channel' + (errors.length ? ': ' + errors.slice(0, 5).join(' | ') : ''));
  check(pressed && logs2.some(l => l.startsWith('caustics:')), 'the sea, by name, and shift-B through the page\'s key function');
  check(errors2.length === 0, 'breaking and its foam, with nothing on the error channel' + (errors2.length ? ': ' + errors2.slice(0, 5).join(' | ') : ''));
  console.log(fails ? `${fails} web test(s) failed` : 'all web tests passed');
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
