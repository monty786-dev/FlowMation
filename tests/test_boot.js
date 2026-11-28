'use strict';
/* The start-up guard (src/ui/fbboot.js): silent on WebView2 / Chromium; in the Wine / Gecko engine it must turn a blank
   page into a visible message and hand the errors to the host (window.__fbPullDiag, polled into host.log). */
const { chromium } = require('playwright');
const http = require('http'), fs = require('fs'), path = require('path');
const UI = path.resolve(__dirname, '../src/ui');
let total = 0, failed = 0;
function check(ok, msg) { total++; if (!ok) { failed++; console.log('  FAIL: ' + msg); } }
const sleep = ms => new Promise(r => setTimeout(r, ms));

const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json' };
const srv = http.createServer((req, res) => {
  const u = new URL(req.url, 'http://x');
  const f = u.pathname === '/catalog.json' ? path.resolve(__dirname, '../src/core/catalog.json') : path.join(UI, u.pathname.replace(/\.\./g, ''));
  if (!fs.existsSync(f) || !fs.statSync(f).isFile()) { res.writeHead(404); return res.end('nf'); }
  res.writeHead(200, { 'content-type': (types[path.extname(f)] || 'text/plain') + '; charset=utf-8' }); res.end(fs.readFileSync(f));
});

(async () => {
  await new Promise(ok => srv.listen(0, '127.0.0.1', ok));
  const base = 'http://127.0.0.1:' + srv.address().port;
  const browser = await chromium.launch({ args: ['--no-sandbox'] });

  /* 1. a normal engine: the guard does nothing */
  {
    const ctx = await browser.newContext(); const page = await ctx.newPage();
    await page.goto(base + '/shell.html'); await sleep(300);
    const r = await page.evaluate(() => ({ host: window.__fbHostIE, pull: typeof window.__fbPullDiag, box: !!document.getElementById('fb-diag'), rep: typeof window.__fbReport }));
    check(r.host === undefined && r.pull === 'undefined' && !r.box, 'no guard activity on a modern engine');
    check(r.rep === 'function', '__fbReport exists everywhere (the UI scripts call it)');
    await ctx.close();
  }

  /* 2. an IE-style engine (?host=ie stands in for document mode / Trident): errors become visible and reach the host */
  {
    const ctx = await browser.newContext(); const page = await ctx.newPage();
    await page.goto(base + '/shell.html?host=ie'); await sleep(300);
    check(await page.evaluate(() => window.__fbHostIE === 1), 'engine recognised without relying on anything but the marker');
    await page.evaluate(() => { setTimeout(function () { throw new Error('boom from a page script'); }, 0); });
    await sleep(300);
    const box = await page.evaluate(() => { const b = document.getElementById('fb-diag'); return b ? b.textContent : null; });
    check(box && /boom from a page script/.test(box), 'a script error is shown on the page, not hidden behind a white screen');
    check(box && /document mode/.test(box) && /JSON:/.test(box), 'the message includes what the engine supports');
    const diag = await page.evaluate(() => window.__fbPullDiag());
    check(/page start/.test(diag) && /error: .*boom from a page script/.test(diag), 'the host can pull the start-up line and the error: ' + JSON.stringify(diag).slice(0, 120));
    check((await page.evaluate(() => window.__fbPullDiag())) === '', 'pulling empties the queue');
    /* nothing answers the shell here (no host), so it never reports ready: the watchdog must say so */
    await sleep(6300);
    const late = await page.evaluate(() => window.__fbPullDiag());
    check(/did not finish starting/.test(late), 'a page that never starts is reported after 6 s');
    await ctx.close();
  }

  /* 3. a slow start is not an error: the notice disappears once the page reports it is up */
  {
    const ctx = await browser.newContext(); const page = await ctx.newPage();
    await page.goto(base + '/newtab.html?host=ie');
    await sleep(900);
    check(await page.evaluate(() => !document.getElementById('fb-diag')), 'newtab.html marks itself started, so no notice');
    await page.evaluate(() => { window.__fbBooted = 0; window.__fbReport = window.__fbReport; });
    await sleep(6600);
    check(await page.evaluate(() => !!document.getElementById('fb-diag')), 'the "still waiting" notice appears when a page does not report in');
    await page.evaluate(() => { window.__fbBooted = 1; });
    await sleep(900);
    check(await page.evaluate(() => !document.getElementById('fb-diag')), 'and goes away again when the page does come up');
    await ctx.close();
  }

  /* 4. console.error in an engine without console reports instead of throwing */
  {
    const ctx = await browser.newContext(); await ctx.addInitScript({ content: 'try { delete window.console; } catch (e) {} try { Object.defineProperty(window, "console", { value: undefined, configurable: true, writable: true }); } catch (e) {}' });
    const page = await ctx.newPage();
    await page.goto(base + '/shell.html?host=ie'); await sleep(300);
    const r = await page.evaluate(() => { try { window.console.error(new Error('listener failed')); return 'ok'; } catch (e) { return 'threw ' + e.message; } });
    check(r === 'ok', 'console.error never throws (' + r + ')');
    const box = await page.evaluate(() => { const b = document.getElementById('fb-diag'); return b ? b.textContent : ''; });
    check(/listener failed/.test(box), 'and the message reaches the visible notice');
    await ctx.close();
  }

  await browser.close(); srv.close();
  console.log(total + ' checks, ' + failed + ' failed');
  process.exit(failed ? 1 : 0);
})();
