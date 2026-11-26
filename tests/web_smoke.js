// pond — a numerically honest wave tank as screen candy
// Copyright (C) 2026 Mico <https://github.com/micomrkaic>; GPL-3.0-or-later
//
// The browser build, in a headless Chromium (WebGL2 on SwiftShader): it
// starts, gets a GL context, takes its options from the address (?attract
// must reach main as --attract and find the preloaded demos), draws, takes a
// click, and says nothing on the console's error channel.
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
  const url = `http://127.0.0.1:${server.address().port}/?attract&preset=pool`;
  const browser = await chromium.launch({ args: ['--use-gl=angle', '--use-angle=swiftshader', '--enable-unsafe-swiftshader'] });
  const page = await browser.newPage({ viewport: { width: 1280, height: 800 } });
  const logs = [], errors = [];
  page.on('console', m => (m.type() === 'error' ? errors : logs).push(m.text()));
  page.on('pageerror', e => errors.push('pageerror: ' + e.message));

  await page.goto(url, { waitUntil: 'load' });
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline && !logs.some(l => l.startsWith('attract:'))) await page.waitForTimeout(250);
  await page.waitForTimeout(2000);
  await page.mouse.click(640, 450);
  await page.waitForTimeout(1000);
  if (shot) await page.screenshot({ path: shot });
  await browser.close();
  server.close();

  let fails = 0;
  const check = (ok, what) => { console.log(`${what}  ${ok ? ' ok' : ' FAILED'}`); if (!ok) fails++; };
  check(logs.some(l => l.startsWith('GL:')), 'a WebGL2 context');
  check(logs.some(l => l.startsWith('caustics:')), 'the scene is built');
  check(logs.some(l => /^attract: [1-9]/.test(l)), '?attract reaches the program and finds the demos');
  check(errors.length === 0, 'nothing on the error channel' + (errors.length ? ': ' + errors.slice(0, 5).join(' | ') : ''));
  console.log(fails ? `${fails} web test(s) failed` : 'all web tests passed');
  process.exit(fails ? 1 : 0);
})().catch(e => { console.error(e); process.exit(1); });
