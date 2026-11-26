'use strict';
const { runFlow, env } = require('./bridge');
const fs = require('fs'), path = require('path'), os = require('os');

const S = (type, p) => Object.assign({ type }, p || {});
const flow = (steps, extra) => Object.assign({ format: 'flowbrowser-flow', version: 1, name: 't', variables: {}, settings: { timeout_ms: 6000 }, steps }, extra || {});
let fails = 0, total = 0;
function check(cond, msg) { total++; if (!cond) { fails++; console.log('  FAIL:', msg); } }
const eq = (a, b, msg) => check(JSON.stringify(a) === JSON.stringify(b), msg + ' — got ' + JSON.stringify(a) + ' want ' + JSON.stringify(b));
const only = process.argv.slice(2);

const scenarios = {};

scenarios.form = async () => {
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/form.html' }),
    S('fill', { selector: 'css:#fn', value: 'Ana' }),
    S('fill', { selector: 'label:Email', value: 'ana@example.com' }),
    S('fill', { selector: 'css:#pw', value: 's3cret & "q"' }),
    S('fill', { selector: 'label:Biography', value: 'Line one\nLine two' }),
    S('select_option', { selector: 'css:#country', value: 'India' }),
    S('select_option', { selector: 'css:#multi', value: 'a|c' }),
    S('check', { selector: 'label:I agree to terms', state: 'check' }),
    S('check', { selector: 'css:#news', state: 'uncheck' }),
    S('check', { selector: 'label:Pro' }),
    S('set_slider', { selector: 'css:#vol', value: '7' }),
    S('fill', { selector: 'css:#dob', value: '15 Mar 2024' }),
    S('fill', { selector: 'label:Alarm time', value: '14:05' }),
    S('fill', { selector: 'css:#age', value: '42' }),
    S('fill', { selector: 'css:#city', value: 'Pune' }),
    S('fill', { selector: 'label:Notes', value: 'rich text ok' }),
    S('type_text', { selector: 'css:#fn', text: 'Ana Maria', clear: true, delay_ms: 5 }),
    S('press_key', { key: 'Enter', selector: 'css:#fn' }),
    S('run_js', { code: 'return {state: state(), submitted: window.__submitted}', result_var: 'res' }),
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300));
  check(r.code === 0, 'form flow ran');
  const s = r.vars.res && r.vars.res.state;
  eq(r.vars.res && r.vars.res.submitted, 1, 'Enter submitted the form once');
  if (s) {
    eq(s.first, 'Ana Maria', 'type_text replaced the first name');
    eq(s.email, 'ana@example.com', 'email by label');
    eq(s.pw, 's3cret & "q"', 'password');
    eq(s.bio, 'Line one\nLine two', 'textarea by aria-label');
    eq(s.country, 'in', 'select by visible text');
    eq(s.multi, ['a', 'c'], 'multi select');
    eq(s.agree, true, 'checkbox via label'); eq(s.news, false, 'uncheck');
    eq(s.plan, 'pro', 'radio via label');
    eq(s.vol, '8', 'slider snapped to step');
    eq(s.dob, '2024-03-15', 'date parsed from "15 Mar 2024"');
    eq(s.tm, '14:05', 'time'); eq(s.age, '42', 'number'); eq(s.city, 'Pune', 'datalist input');
    eq(s.rte, 'rich text ok', 'contenteditable');
  }
  await r.done();
};

scenarios.click = async () => {
  const t0 = Date.now();
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/form.html' }),
    S('click', { selector: 'css:#late' }),                         // covered by an overlay for ~1.2s: must wait, not click the overlay
    S('click', { selector: 'css:#appear' }),                       // does not exist for 0.8s
    S('run_js', { code: 'return {late: window.__late||0, appear: window.__appear||0}', result_var: 'clicks' }),
    S('navigate', { url: '{{base}}/table.html' }),
    S('click', { selector: 'text:Go to page 2' }),                 // navigation: must wait for the new page
    S('get_text', { selector: 'css:h1', result_var: 'h1' }),
    S('back'),
    S('element_exists', { selector: 'css:#t1', result_var: 'onTable' }),
    S('navigate', { url: '{{base}}/dialogs.html' }),
    S('click', { selector: 'css:#al' }),
    S('run_js', { code: 'return document.title', result_var: 'title' }),
    S('handle_dialog', { action: 'dismiss' }),
    S('click', { selector: 'css:#cf' }),
    S('get_text', { selector: 'css:#r', result_var: 'dismissed' }),
    S('handle_dialog', { action: 'accept' }),
    S('click', { selector: 'css:#cf' }),
    S('get_text', { selector: 'css:#r', result_var: 'accepted' }),
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300), (Date.now() - t0) + 'ms');
  check(r.code === 0, 'click flow ran');
  eq(r.vars.clicks, { late: 1, appear: 1 }, 'clicked the real buttons after overlay/delay');
  eq(r.vars.h1, 'Page 2', 'click waited for navigation');
  eq(r.vars.onTable, true, 'back works');
  eq(r.vars.title, 'after-alert', 'alert auto-accepted');
  eq(r.vars.dismissed, 'confirm:false', 'confirm dismissed'); eq(r.vars.accepted, 'confirm:true', 'confirm accepted');
  await r.done();
};


scenarios.extract = async () => {
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/table.html' }),
    S('extract_table', { selector: 'css:#t1', headers: 'auto', result_var: 't1' }),
    S('extract_table', { selector: 'css:#t2', result_var: 't2' }),
    S('extract_table', { selector: 'css:#grid', headers: 'auto', result_var: 'grid' }),
    S('extract_table', { selector: 'css:#t1', headers: 'none', result_var: 't1raw' }),
    S('extract_page', { result_var: 'page' }),
    S('extract_page', { result_var: 'onlyHeadings', kinds: ['headings'] }),
    S('get_text', { selector: 'css:#menu li', result_var: 'first_li' }),
    S('get_text', { selector: 'css:#l2', what: 'href', result_var: 'href' }),
    S('get_attribute', { selector: 'css:img', attribute: 'alt', result_var: 'alt' }),
    S('count_elements', { selector: 'css:.dup', result_var: 'dups' }),
    S('get_text', { selector: 'css:.dup >> nth=3', result_var: 'third' }),
    S('element_exists', { selector: 'css:#nope', result_var: 'ex1' }),
    S('element_exists', { selector: 'text:Go to page 2', result_var: 'ex2' }),
    S('get_text', { selector: 'xpath://li[2]', result_var: 'xp' }),
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300));
  check(r.code === 0, 'extract flow ran');
  eq(r.vars.t1 && r.vars.t1.length, 3, 't1 rows');
  eq(r.vars.t1 && r.vars.t1[0], { Name: 'Apple', Qty: '3', Price: '$1.20' }, 't1 first row keyed by header');
  eq(r.vars.t2, [{ Region: 'North', Q: 'Q1', Sales: '10' }, { Region: 'North', Q: 'Q2', Sales: '20' }, { Region: 'South', Q: 'Q1', Sales: '5' }], 'rowspan expanded');
  eq(r.vars.grid, [{ City: 'Pune', Pop: '7' }, { City: 'Paris', Pop: '2' }], 'ARIA grid');
  eq(r.vars.t1raw && r.vars.t1raw[0], ['Name', 'Qty', 'Price'], 'headers none gives arrays');
  const pg = r.vars.page || {};
  eq(Object.keys(pg).sort(), ['buttons', 'forms', 'headings', 'images', 'inputs', 'links', 'lists', 'meta', 'tables', 'text'], 'extract_page categories');
  eq(pg.headings && pg.headings.map(h => h.text), ['Prices', 'Fruit', 'Regions'], 'headings');
  eq(pg.tables && pg.tables.length, 3, 'tables found');
  check(pg.links && pg.links.some(l => l.href === 'https://example.org/x' && l.text === 'External'), 'links with text and absolute href');
  eq(pg.lists && pg.lists[0].items, ['One', 'Two', 'Three'], 'list items');
  eq(Object.keys(r.vars.onlyHeadings), ['headings'], 'kinds filter');
  eq(r.vars.first_li, 'One', 'get_text'); eq(r.vars.href, 'https://example.org/x', 'href'); eq(r.vars.alt, 'logo', 'attribute');
  eq(r.vars.dups, 3, 'count'); eq(r.vars.third, 'third', 'nth=3');
  eq(r.vars.ex1, false, 'exists false'); eq(r.vars.ex2, true, 'exists by text'); eq(r.vars.xp, 'Two', 'xpath');
  await r.done();
};

scenarios.list = async () => {
  const fields = [{ name: 'title', selector: 'css:.title' }, { name: 'price', selector: 'css:.price' }, { name: 'link', selector: 'css:a.more', attr: 'href' }, { name: 'img', selector: 'css:img', attr: 'src' }];
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/list?page=1' }),
    S('extract_list', { item_selector: 'css:.card', fields, result_var: 'paged', max_pages: 5, next_selector: 'css:a.next', page_wait_ms: 100 }),
    S('navigate', { url: '{{base}}/spa' }),
    S('extract_list', { item_selector: 'css:.card', fields: fields.slice(0, 2), result_var: 'spa', max_pages: 5, next_selector: 'css:#next', page_wait_ms: 100 }),
    S('navigate', { url: '{{base}}/scroll' }),
    S('extract_list', { item_selector: 'css:.card', fields: [{ name: 'title', selector: 'css:.title' }], result_var: 'infinite', max_pages: 10, scroll_load: true, page_wait_ms: 400 }),
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300));
  check(r.code === 0, 'list flow ran');
  eq(r.vars.paged && r.vars.paged.length, 12, 'three pages of four, stopped at the disabled Next');
  eq(r.vars.paged && r.vars.paged[0], { title: 'Product 1', price: '$1.99', link: r.site.url + '/p/1', img: r.site.url + '/img/1.png' }, 'fields incl. href/src');
  eq(r.vars.paged && r.vars.paged[11].title, 'Product 12', 'last item');
  eq(r.vars.spa && r.vars.spa.map(x => x.title), Array.from({ length: 12 }, (_, i) => 'Product ' + (i + 1)), 'single-page-app pagination (no navigation)');
  eq(r.vars.infinite && r.vars.infinite.length, 24, 'infinite scroll loaded everything');
  await r.done();
};

scenarios.upload = async () => {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fbup_'));
  const a = path.join(dir, 'a.txt'), b = path.join(dir, 'b report.txt'), img = path.join(dir, 'p.png');
  fs.writeFileSync(a, 'hello'); fs.writeFileSync(b, '0123456789'); fs.writeFileSync(img, Buffer.from('89504e470d0a1a0a', 'hex'));
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/upload.html' }),
    S('upload_file', { selector: 'css:#plain', files: [a, b] }),
    S('upload_file', { selector: 'css:#btn', files: [a] }),                       // button that opens a chooser for a hidden input
    S('upload_file', { selector: 'css:#fakelabel', files: [img] }),               // label for an invisible input
    S('upload_file', { selector: 'css:#drop', files: [b] }),                      // dropzone that contains an input
    S('upload_file', { selector: 'css:#zone2', files: [a, img], mode: 'drop' }),  // pure drop zone, no input anywhere
    S('upload_file', { selector: 'css:#zone2', files: ['{{dir}}/missing.txt'] , on_error: 'continue' }),
    S('get_text', { selector: 'css:#result', result_var: 'result' }),
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300));
  check(r.code === 0, 'upload flow ran');
  const res = r.vars.result || '';
  console.log('  result:', res);
  check(res.includes('plain:a.txt/5/text/plain,b report.txt/10/text/plain;'), 'plain multi-file input');
  check(res.includes('hidden:a.txt/5/text/plain;'), 'button -> file chooser -> hidden input');
  check(res.includes('lab:p.png/8/image/png;'), 'label of invisible input');
  check(res.includes('indrop:b report.txt/10/'), 'dropzone containing an input');
  check(res.includes('zone2:a.txt/5/text/plain,p.png/8/image/png;'), 'pure drop zone received DataTransfer files');
  check(r.events.some(e => e.ev === 'step_error' && /File not found/.test(e.error || '')), 'missing file reported clearly');
  await r.done();
};

scenarios.captcha = async () => {
  const mk = (mode, withCheck) => flow([S('navigate', { url: '{{base}}/captcha.html' }), S('set_var', { var: 'after', value: 'continued' })], { settings: { timeout_ms: 4000, captcha: mode } });
  // pause: the human solves it, the flow continues
  let asked = 0;
  let r = await runFlow(mk('pause'), { onCaptcha: async page => { asked++; await page.evaluate(() => { document.querySelector('textarea[name=g-recaptcha-response]').value = 'tok'; document.getElementById('rc').remove(); }); } });
  check(r.code === 0 && r.vars.after === 'continued', 'pause: flow continued after the person solved it (exit ' + r.code + ' ' + r.stderr.trim().slice(0, 200) + ')');
  eq(asked, 1, 'the person was asked exactly once');
  check(r.events.some(e => e.ev === 'captcha' && /robot/i.test(e.kind)), 'captcha event emitted');
  await r.done();
  // stop
  r = await runFlow(mk('stop'));
  check(r.code === 1 && /does not solve CAPTCHAs/.test(r.stderr), 'stop: flow stops with an explanation (' + r.stderr.trim().slice(0, 160) + ')');
  check(r.vars.after === undefined, 'stop: nothing ran afterwards');
  await r.done();
  // off
  asked = 0;
  r = await runFlow(mk('off'), { onCaptcha: async () => { asked++; } });
  check(r.code === 0 && asked === 0 && r.vars.after === 'continued', 'off: no detection, no prompt');
  await r.done();
  // user presses Stop in the prompt
  r = await runFlow(mk('pause'));          // host answers status 2 (cancelled)
  eq(r.code, 2, 'cancelled prompt stops the flow (exit 2)');
  await r.done();
  // an ordinary page must not be flagged
  r = await runFlow(flow([S('navigate', { url: '{{base}}/form.html' }), S('captcha_check'), S('set_var', { var: 'ok', value: 'yes' })], { settings: { captcha: 'stop' } }));
  check(r.code === 0 && r.vars.ok === 'yes', 'no false positive on a normal form page');
  await r.done();
};

scenarios.shadow = async () => {
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/shadow.html' }),
    S('click', { selector: 'css:#sb' }),                                           // inside an open shadow root
    S('fill', { selector: 'css:#si', value: 'typed in shadow' }),
    S('fill', { selector: 'css:iframe#fr >> css:#fi', value: 'typed in frame' }), // same-origin iframe
    S('click', { selector: 'css:iframe#fr >> text:Frame Button' }),
    S('run_js', { code: 'var s=document.getElementById("card").shadowRoot.getElementById("si").value; var f=document.getElementById("fr").contentDocument.getElementById("fi").value; return {sb: window.__sb, s: s, f: f, fb: window.__fb_clicks}', result_var: 'st' }),
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300));
  check(r.code === 0, 'shadow/frame flow ran');
  eq(r.vars.st, { sb: 1, s: 'typed in shadow', f: 'typed in frame', fb: 1 }, 'shadow DOM + iframe');
  await r.done();
};

scenarios.dnd = async () => {
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/dnd.html' }),
    S('drag_drop', { selector: 'css:#card', target: 'css:#zone', method: 'html5' }),
    S('drag_drop', { selector: 'css:#box', target: 'css:#zone', method: 'mouse' }),
    S('get_text', { selector: 'css:#res', result_var: 'res' }),
    S('get_text', { selector: 'css:#pos', result_var: 'pos' }),
    S('press_key', { key: 'Tab' }),
    S('focus', { selector: 'css:#card' }),
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300));
  check(r.code === 0, 'dnd flow ran');
  eq(r.vars.res, 'dropped:card', 'HTML5 drag and drop');
  check(/^\d+,\d+$/.test(r.vars.pos || '') && r.vars.pos !== '20,200', 'mouse drag moved the box: ' + r.vars.pos);
  await r.done();
};

scenarios.resilience = async () => {
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/form.html' }),
    S('click', { selector: 'css:#renamed-button', alts: ['css:#go', 'text:Send'], after: 'none' }),         // self-healing: first selector is stale
    S('run_js', { code: 'return window.__submitted||0', result_var: 'sub' }),
    S('click', { selector: 'css:#does-not-exist', timeout_ms: 600, on_error: 'continue' }),
    S('set_var', { var: 'got_here', value: 'yes' }),
    S('try', { body: [S('fill', { selector: 'css:#gone', value: 'x', timeout_ms: 500 })], catch: [S('set_var', { var: 'caught', value: '{{error}}' })] }),
    S('wait_text', { text: 'Sign up', timeout_ms: 1000 }),
    S('wait_element', { selector: 'css:#overlay', state: 'detached', timeout_ms: 4000 }),
    S('wait_until', { condition: '{{page.title}} == "Form"', timeout_ms: 2000, interval_ms: 100 }),
    S('select_option', { selector: 'css:#country', value: 'Atlantis' }),                                      // fails with the list of options
  ]));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 400));
  eq(r.code, 1, 'ends with the expected failure');
  eq(r.vars.sub, 1, 'alternative selector healed the broken one');
  eq(r.vars.got_here, 'yes', 'on_error=continue keeps going');
  check(/Element not found: css:#gone/.test(r.vars.caught || ''), 'try/catch message is friendly: ' + r.vars.caught);
  check(/Option "Atlantis" not found. Available: "--", "United States", "India", "Germany"/.test(r.stderr), 'option error lists the choices: ' + r.stderr.trim().slice(0, 200));
  await r.done();
};

scenarios.autofill = async () => {
  const r = await runFlow(flow([
    S('navigate', { url: '{{base}}/form.html' }),
    S('wait_element', { selector: 'css:#overlay', state: 'detached' }),
    S('autofill_form', { data: 'row', result_var: 'report', submit: true }),
    S('run_js', { code: 'return {state: state(), submitted: window.__submitted}', result_var: 'res' }),
  ], { variables: { row: { 'First name': 'Bob', 'E-mail': 'bob@x.org', Country: 'Germany', 'I agree to terms': 'yes', Plan: 'free', Age: 33, Nonexistent: 'zzz', _row: 7 } } }));
  console.log('  exit', r.code, r.stderr.trim().slice(0, 300));
  check(r.code === 0, 'autofill flow ran');
  const s = (r.vars.res || {}).state || {};
  eq([s.first, s.email, s.country, s.agree, s.plan, s.age], ['Bob', 'bob@x.org', 'de', true, 'free', '33'], 'fields matched by label/name');
  eq((r.vars.res || {}).submitted, 1, 'submitted');
  const rep = r.vars.report || [];
  eq(rep.filter(x => !x.matched).map(x => x.field), ['Nonexistent'], 'unmatched field reported');
  await r.done();
};

(async () => {
  for (const name of Object.keys(scenarios)) {
    if (only.length && !only.includes(name)) continue;
    console.log('▶', name);
    try { await scenarios[name](); } catch (e) { fails++; console.log('  EXCEPTION', e.stack); }
  }
  console.log(`${total} checks, ${fails} failed`);
  await (await env()).close();
  process.exit(fails ? 1 : 0);
})();
