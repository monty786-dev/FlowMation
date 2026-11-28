'use strict';
/* Test host: runs fbrun against a real Chromium over the Chrome DevTools Protocol.
   This is the same contract the Windows app implements with WebView2 (FbBackend), so what passes here exercises
   the runner and the page runtime exactly as the app will. */
const { chromium } = require('playwright');
const http = require('http');
const fs = require('fs');
const path = require('path');
const readline = require('readline');
const { spawn } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const SITE = path.join(__dirname, 'site');
const RUNTIME = fs.readFileSync(path.join(ROOT, 'src/js/fb_runtime.js'), 'utf8');
const LEGACY_JS = fs.readFileSync(path.join(ROOT, 'src/js/fb_legacy.js'), 'utf8');
/* FB_LEGACY=1 runs everything the way the Wine/Gecko build does: no DevTools input or handles (fbrun --legacy turns the
   runner's calls into page scripts) and a page engine that lacks the modern APIs the page scripts must not rely on. */
const LEGACY = !!process.env.FB_LEGACY;
const STRIP_MODERN = `(function () {
  function kill(o, k) { try { delete o[k]; } catch (e) {} try { if (o[k] !== undefined) Object.defineProperty(o, k, { value: undefined, configurable: true }); } catch (e) {} }
  kill(Element.prototype, 'closest'); kill(Element.prototype, 'matches'); kill(Node.prototype, 'getRootNode');
  try { Object.defineProperty(Node.prototype, 'isConnected', { get: function () { return undefined; }, configurable: true }); } catch (e) {}
  ['MouseEvent', 'KeyboardEvent', 'InputEvent', 'DragEvent', 'PointerEvent', 'DataTransfer', 'File', 'CustomEvent'].forEach(function (n) { kill(window, n); });
  kill(Object, 'assign'); kill(window, 'CSS');
})();`;

function items(page, per) {
  const out = [];
  for (let i = 1; i <= per; i++) {
    const n = (page - 1) * per + i;
    out.push(`<div class="card"><h3 class="title">Product ${n}</h3><span class="price">$${n}.99</span><a class="more" href="/p/${n}">Details</a><img src="/img/${n}.png" alt="p${n}"></div>`);
  }
  return out.join('\n');
}

function startSite() {
  const srv = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const send = (body, type) => { res.writeHead(200, { 'content-type': type || 'text/html; charset=utf-8' }); res.end(body); };
    if (u.pathname === '/list') {
      const p = parseInt(u.searchParams.get('page') || '1', 10);
      const next = p < 3 ? `<a class="next" href="/list?page=${p + 1}">Next</a>` : '<a class="next disabled" aria-disabled="true">Next</a>';
      return send(`<!doctype html><meta charset="utf-8"><title>List ${p}</title><h1>Page ${p}</h1><div id="items">${items(p, 4)}</div>${next}`);
    }
    if (u.pathname === '/spa') {
      return send(`<!doctype html><meta charset="utf-8"><title>SPA</title><div id="items"></div><button id="next">Next</button>
<script>var p=1;function draw(){var h='';for(var i=1;i<=4;i++){var n=(p-1)*4+i;h+='<div class="card"><h3 class="title">Product '+n+'</h3><span class="price">$'+n+'.99</span></div>'}document.getElementById('items').innerHTML=h;document.getElementById('next').disabled=p>=3}
draw();document.getElementById('next').onclick=function(){setTimeout(function(){p++;draw()},150)}</script>`);
    }
    if (u.pathname === '/scroll') {
      return send(`<!doctype html><meta charset="utf-8"><title>Scroll</title><style>.card{height:120px;border-bottom:1px solid #ccc}</style><div id="items"></div>
<script>var n=0;function more(){for(var i=0;i<8&&n<24;i++){n++;var d=document.createElement('div');d.className='card';d.innerHTML='<h3 class="title">Item '+n+'</h3>';document.getElementById('items').appendChild(d)}}
more();window.addEventListener('scroll',function(){if(innerHeight+scrollY>=document.documentElement.scrollHeight-50)setTimeout(more,100)})</script>`);
    }
    if (u.pathname.startsWith('/recaptcha/')) return send('<!doctype html><meta charset="utf-8"><body><div style="border:1px solid #aaa;padding:20px">I\'m not a robot <input type="checkbox"></div></body>');
    if (u.pathname.startsWith('/p/') || u.pathname.startsWith('/img/') || u.pathname === '/i.png') return send('<!doctype html><title>detail</title><h1>Detail</h1>');
    const file = path.join(SITE, u.pathname === '/' ? 'form.html' : u.pathname.replace(/\.\./g, ''));
    if (u.pathname === '/page2') return send(fs.readFileSync(path.join(SITE, 'page2.html')));
    if (fs.existsSync(file) && fs.statSync(file).isFile()) return send(fs.readFileSync(file));
    res.writeHead(404); res.end('not found');
  });
  return new Promise(ok => srv.listen(0, '127.0.0.1', () => ok({ url: 'http://127.0.0.1:' + srv.address().port, close: () => srv.close() })));
}

class Host {
  constructor(ctx, opts) {
    this.ctx = ctx; this.opts = opts || {};
    this.pending = {}; this.waiters = {};
    this.dialogs = 'accept';
    this.events = [];
  }
  async attach(page) {
    this.page = page;
    this.cdp = await this.ctx.newCDPSession(page);
    await this.cdp.send('Page.enable');
    this.cdp.on('Page.fileChooserOpened', p => this.signal('Page.fileChooserOpened', p));
    if (!page.__fbHooked) {
      page.__fbHooked = true;
      page.on('request', r => { if (r.isNavigationRequest() && r.frame() === page.mainFrame() && page === this.page) this.signal('Navigation.started'); });
      page.on('load', () => { if (page === this.page) this.signal('Navigation.completed'); });
      page.on('dialog', d => { if (this.dialogs === 'dismiss') d.dismiss().catch(() => {}); else if (this.dialogs === 'accept') d.accept().catch(() => {}); });
    }
  }
  signal(name, params) {
    const w = this.waiters[name];
    if (w) { delete this.waiters[name]; clearTimeout(w.t); w.resolve({ status: 0, params }); return; }
    this.pending[name] = params === undefined ? {} : params;
  }
  waitEvent(name, timeout) {
    if (name in this.pending) { const p = this.pending[name]; delete this.pending[name]; return Promise.resolve({ status: 0, params: p }); }
    if (!timeout) return Promise.resolve({ status: 1 });
    return new Promise(resolve => {
      const t = setTimeout(() => { delete this.waiters[name]; resolve({ status: 1 }); }, timeout);
      this.waiters[name] = { resolve, t };
    });
  }
  async handle(m) {
    try {
      switch (m.op) {
        case 'cdp': return { ok: true, result: await this.cdp.send(m.method, m.params || {}) };
        case 'navigate':
          await this.page.goto(m.url, { timeout: m.timeout || 15000, waitUntil: m.wait ? 'load' : 'commit' });
          delete this.pending['Navigation.started']; delete this.pending['Navigation.completed'];
          return { ok: true };
        case 'wait_event': return Object.assign({ ok: true }, await this.waitEvent(m.name, m.timeout));
        case 'tab': return await this.tab(m.action, m.arg);
        case 'option':
          if (m.name === 'dialogs') this.dialogs = m.value;
          if (m.name === 'user_agent') await this.cdp.send('Emulation.setUserAgentOverride', { userAgent: m.value });
          return { ok: true };
        case 'ask': {
          this.events.push({ ev: 'ask', kind: m.kind, text: m.text });
          if (m.kind === 'captcha') {
            if (this.opts.onCaptcha) { await this.opts.onCaptcha(this.page); return { ok: true, status: 0 }; }
            return { ok: true, status: 2 };
          }
          if (m.kind === 'input') return { ok: true, status: 0, answer: (this.opts.answers && this.opts.answers[m.text]) || m.def };
          return { ok: true, status: 0 };
        }
        default: return { ok: false, error: 'unknown op ' + m.op };
      }
    } catch (e) { return { ok: false, error: String(e.message || e).split('\n')[0] }; }
  }
  async tab(action, arg) {
    const pages = () => this.ctx.pages();
    if (action === 'new') {
      const p = await this.ctx.newPage();
      await this.attach(p);
      if (arg) await p.goto(arg);
      return { ok: true };
    }
    if (action === 'switch') {
      const all = pages(); let t = null;
      if (/^\d+$/.test(arg)) t = all[parseInt(arg, 10) - 1];
      else for (const p of all) if (!t && (await p.title()).toLowerCase().includes(arg.toLowerCase())) t = p;
      if (!t) return { ok: false, error: 'No tab "' + arg + '"' };
      await this.attach(t); await t.bringToFront();
      return { ok: true };
    }
    if (action === 'close') { const p = this.page; await p.close(); const all = pages(); await this.attach(all[all.length - 1]); return { ok: true }; }
    if (action === 'back') { await this.page.goBack({ waitUntil: 'load' }); return { ok: true }; }
    if (action === 'forward') { await this.page.goForward({ waitUntil: 'load' }); return { ok: true }; }
    if (action === 'reload') { await this.page.reload({ waitUntil: 'load' }); return { ok: true }; }
    return { ok: false, error: 'unknown tab op' };
  }
}

/* shared browser for a whole test run */
let shared = null;
async function env() {
  if (shared) return shared;
  const site = await startSite();
  const browser = await chromium.launch({ args: ['--no-sandbox'] });
  shared = { site, browser, close: async () => { await browser.close(); site.close(); shared = null; } };
  return shared;
}

/* runs a flow object; resolves {code, stderr, events, vars, host, page, ctx} (call result.done() when finished with the page) */
async function runFlow(flow, opts) {
  opts = opts || {};
  const e = await env();
  const ctx = await e.browser.newContext({ viewport: { width: 1100, height: 800 } });
  if (LEGACY) {
    await ctx.addInitScript({ content: STRIP_MODERN });
    await ctx.addInitScript({ content: LEGACY_JS });
  }
  await ctx.addInitScript({ content: RUNTIME });
  const host = new Host(ctx, opts);
  await host.attach(await ctx.newPage());
  const dir = fs.mkdtempSync(path.join(require('os').tmpdir(), 'fbweb_'));
  const flowPath = path.join(dir, 'flow.fbflow');
  fs.writeFileSync(flowPath, JSON.stringify(flow));
  const args = [flowPath, '--catalog', path.join(ROOT, 'src/core/catalog.json'), '--base', dir, '--var', 'base=' + e.site.url, '--var', 'dir=' + dir];
  if (LEGACY) args.push('--legacy');
  const child = spawn(opts.fbrun || process.env.FBRUN || '/tmp/fbrun', args, { stdio: ['pipe', 'pipe', 'pipe'] });
  let stderr = '';
  child.stderr.on('data', d => { stderr += d; });
  const rl = readline.createInterface({ input: child.stdout });
  const events = [];
  let chain = Promise.resolve();
  rl.on('line', line => {
    chain = chain.then(async () => {
      let m; try { m = JSON.parse(line); } catch (x) { return; }
      if (m.op === 'emit') { events.push(m.event); if (opts.onEvent) opts.onEvent(m.event, host); return; }
      const r = await host.handle(m);
      r.id = m.id;
      child.stdin.write(JSON.stringify(r) + '\n');
    });
  });
  const code = await new Promise(ok => child.on('close', ok));
  await chain;
  const varsEv = events.filter(x => x.ev === 'vars').pop();
  return { code, stderr, events: events.concat(host.events), vars: varsEv ? varsEv.vars : {}, host, page: host.page, ctx, dir, site: e.site,
           done: () => ctx.close() };
}

module.exports = { runFlow, env, startSite, LEGACY, STRIP_MODERN, LEGACY_JS, RUNTIME };
