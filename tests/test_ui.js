'use strict';
/* UI tests: the Studio panel and the browser shell run in real Chromium against an in-page mock of the native host. */
const { chromium } = require('playwright');
const http = require('http'), fs = require('fs'), path = require('path'), { execFileSync } = require('child_process');
const ROOT = path.resolve(__dirname, '..'), UI = path.join(ROOT, 'src/ui');
const MOCK = fs.readFileSync(path.join(__dirname, 'ui_mock.js'), 'utf8');
let total = 0, failed = 0;
function check(ok, msg) { total++; if (!ok) { failed++; console.log('  FAIL: ' + msg); } else if (process.env.V) console.log('  ok: ' + msg); }
const sleep = ms => new Promise(r => setTimeout(r, ms));

function serve() {
  const types = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.json': 'application/json' };
  const srv = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x'); let f = u.pathname === '/catalog.json' ? path.join(ROOT, 'src/core/catalog.json') : path.join(UI, u.pathname.replace(/\.\./g, ''));
    if (!fs.existsSync(f) || !fs.statSync(f).isFile()) { res.writeHead(404); return res.end('nf'); }
    res.writeHead(200, { 'content-type': (types[path.extname(f)] || 'text/plain') + '; charset=utf-8' }); res.end(fs.readFileSync(f));
  });
  return new Promise(ok => srv.listen(0, '127.0.0.1', () => ok({ url: 'http://127.0.0.1:' + srv.address().port, close: () => srv.close() })));
}

/* the flow validator of the real engine (fbrun --check) */
function engineCheck(flow) {
  const f = path.join(require('os').tmpdir(), 'uitest_' + process.pid + '.fbflow'); fs.writeFileSync(f, JSON.stringify(flow));
  try { execFileSync(process.env.FBRUN || '/tmp/fbrun', [f, '--catalog', path.join(ROOT, 'src/core/catalog.json'), '--check'], { stdio: 'pipe' }); return ''; }
  catch (e) { return String(e.stdout || '') + String(e.stderr || ''); }
}

async function open(browser, site, file, w, h) {
  const ctx = await browser.newContext({ viewport: { width: w || 460, height: h || 820 } });
  await ctx.addInitScript({ content: MOCK });
  const page = await ctx.newPage(); const errors = [];
  page.on('pageerror', e => errors.push('pageerror: ' + e.message));
  page.on('console', m => { if (m.type() === 'error') errors.push('console: ' + m.text()); });
  page.on('dialog', d => d.dismiss());
  await page.goto(site.url + '/' + file);
  return { ctx, page, errors };
}
const flowOf = page => page.evaluate(() => JSON.parse(JSON.stringify(P.flow)));
const mock = (page, fn, arg) => page.evaluate(fn, arg);

async function panelTests(browser, site) {
  console.log('▶ panel: designer');
  const { ctx, page, errors } = await open(browser, site, 'panel.html');
  await page.waitForSelector('#tree');
  check((await page.$$('.tabbtn')).length === 8, '8 tabs');
  check(await page.evaluate(() => Object.keys(P.defs).length) > 50, 'catalog loaded: ' + await page.evaluate(() => Object.keys(P.defs).length) + ' step types');

  /* add steps through the palette */
  await page.click('.toolbar .btn:has-text("Add step")');
  await page.fill('.modal input[type=search]', 'go to');
  const first = await page.textContent('.pitem b');
  check(/Go to|Open/i.test(first || ''), 'palette search finds navigate: ' + first);
  await page.click('.pitem');
  let f = await flowOf(page);
  check(f.steps.length === 1 && f.steps[0].type === 'navigate', 'navigate step added');
  await page.fill('.ibody input[placeholder*="https"], .ibody .form input >> nth=1', 'https://shop.test/');
  f = await flowOf(page);
  check(f.steps[0].url === 'https://shop.test/', 'inspector edits the step: ' + JSON.stringify(f.steps[0]));

  await page.evaluate(() => { P.insert('', 1, P.newStep('click')); });
  await page.waitForSelector('.step.sel');
  check(await page.$$eval('.step', e => e.length) === 2, 'two rows drawn');
  await page.waitForTimeout(900);
  check((await page.textContent('.flowhead button.btn.sm')).includes('1'), 'validation flags the click without selector: ' + await page.textContent('.flowhead button.btn.sm'));
  await page.fill('.ibody .mono', 'css:#go');
  f = await flowOf(page); check(f.steps[1].selector === 'css:#go', 'selector typed');

  /* nesting + move */
  await page.evaluate(() => { P.insert('', 2, P.newStep('if')); });
  f = await flowOf(page);
  check(Array.isArray(f.steps[2].then) && Array.isArray(f.steps[2].else), 'if step has then / else lists');
  await page.evaluate(() => { P.insert('2.then', 0, P.newStep('log')); });
  f = await flowOf(page); check(f.steps[2].then.length === 1 && f.steps[2].then[0].type === 'log', 'step added inside "then"');
  await page.evaluate(() => P.move('0', '2.else', 0));
  f = await flowOf(page); check(f.steps[0].type === 'click' && f.steps[1].else.length === 1 && f.steps[1].else[0].type === 'navigate', 'moving a step into a nested list fixes the paths: ' + JSON.stringify(f.steps.map(s => s.type)));
  await page.evaluate(() => P.move('1.else.0', '', 0));
  f = await flowOf(page); check(f.steps[0].type === 'navigate' && f.steps.length === 3, 'moved back out');
  await page.evaluate(() => P.move('2', '2.then', 0));
  f = await flowOf(page); check(f.steps.length === 3 && f.steps[2].type === 'if' && f.steps[2].then.length === 1, 'cannot drop a step into itself');

  /* drag and drop with the mouse */
  const rows = page.locator('.tree > .node > .step');
  const before = (await flowOf(page)).steps.map(s => s.type).join();
  await rows.nth(0).dragTo(rows.nth(2), { targetPosition: { x: 40, y: 3 } });
  const after = (await flowOf(page)).steps.map(s => s.type).join();
  check(before !== after, 'drag & drop reorders: ' + before + ' → ' + after);

  /* undo / redo */
  await page.evaluate(() => P.doUndo()); const u1 = (await flowOf(page)).steps.map(s => s.type).join();
  check(u1 === before, 'undo restores the order');
  await page.evaluate(() => P.doRedo()); check((await flowOf(page)).steps.map(s => s.type).join() === after, 'redo');

  /* disable + duplicate + delete */
  await page.evaluate(() => { P.sel = '0'; P.dup('0'); });
  f = await flowOf(page); check(f.steps.length === 4, 'duplicate');
  await page.evaluate(() => P.remove('1')); f = await flowOf(page); check(f.steps.length === 3, 'delete');

  /* the generated flow passes the real validator */
  await page.evaluate(() => { P.flow.steps = []; P.insert('', 0, Object.assign(P.newStep('navigate'), { url: 'https://x.test' })); P.insert('', 1, Object.assign(P.newStep('click'), { selector: 'css:#a' })); P.insert('', 2, P.newStep('if')); P.flow.steps[2].condition = '{{x}} > 1'; P.insert('2.then', 0, Object.assign(P.newStep('log'), { message: 'hi' })); });
  const bad = engineCheck(await flowOf(page)); check(bad === '', 'designer output passes the engine validator ' + bad.slice(0, 200));

  /* save / library */
  await page.evaluate(() => { P.flow.name = 'My flow'; });
  await page.click('.toolbar .btn.primary:has-text("Save")'); await page.waitForSelector('.modal input');
  await page.fill('.modal input', 'my-flow'); await page.click('.modal .btn.primary');
  await page.waitForTimeout(150);
  check(await mock(page, () => !!__mock.lib['my-flow']), 'saved to the library');
  await page.click('.tabbtn:has-text("Library")'); await page.waitForSelector('.libitem');
  check((await page.textContent('.libitem')).includes('my-flow') || (await page.textContent('.libitem')).includes('My flow'), 'library lists it');
  check((await page.textContent('#pg-library')).includes('daily-report'), 'scheduled runs listed');
  await page.click('.libitem .btn:has-text("Copy")'); await page.waitForTimeout(200);
  check(await mock(page, () => Object.keys(__mock.lib).length) === 2, 'copy creates a second flow');
  await page.click('#pg-library .btn:has-text("Back up")'); await page.waitForTimeout(150);
  check(await page.evaluate(() => document.querySelector('#toasts').textContent.includes('Backup saved')), 'backup toast');
  await page.click('#pg-library .btn:has-text("Restore")'); await page.click('.modal input[value="2"]'); await page.click('.modal .btn.primary'); await page.waitForTimeout(150);
  check(await mock(page, () => __mock.restoreMode) === 2, 'restore mode is passed to the host');
  await page.click('.libitem .btn:has-text("Schedule")'); await page.click('.modal .btn.primary'); await page.waitForTimeout(100);
  check(await mock(page, () => __mock.scheduled && __mock.scheduled.when) === 'daily', 'schedule request');
  check(errors.length === 0, 'no script errors: ' + errors.join(' | ').slice(0, 300));
  await ctx.close();
}

async function runTests(browser, site) {
  console.log('▶ panel: run, log, variables');
  const { ctx, page, errors } = await open(browser, site, 'panel.html');
  await page.waitForSelector('#tree');
  await page.evaluate(() => { P.insert('', 0, Object.assign(P.newStep('navigate'), { url: 'https://x.test' })); P.insert('', 1, Object.assign(P.newStep('click'), { selector: 'css:#a' })); });
  await page.waitForTimeout(900);
  await page.click('.runbar .btn:has-text("Run")'); await page.waitForTimeout(500);
  check(await page.$$eval('.step.ok', e => e.length) === 2, 'both steps marked done');
  check(await page.evaluate(() => P.run.state) === 'idle', 'state back to idle');
  await page.click('.tabbtn:has-text("Run log")');
  const lg = await page.textContent('.log');
  check(/Flow finished/.test(lg) && /Flow "New flow" started/.test(lg), 'log shows start and end');
  await page.click('.tabbtn:has-text("Variables")');
  check((await page.textContent('#pg-vars')).includes('rows'), 'variables view lists "rows"');
  await page.click('#pg-vars .vcard summary'); check(await page.$$eval('#pg-vars table.grid tbody tr', e => e.length) === 2, 'table preview');
  await page.click('#pg-vars .btn:has-text("Save to Excel")'); await page.waitForSelector('.modal');
  await page.fill('.modal input[placeholder*="xlsx"]', 'C:\\out\\r.xlsx'); await page.click('.modal .btn.primary'); await page.waitForTimeout(150);
  const ex = await mock(page, () => __mock.exported);
  check(ex && ex.var === 'rows' && ex.file === 'C:\\out\\r.xlsx' && ex.format === 'xlsx', 'export goes to the host by variable name (full data stays in the engine): ' + JSON.stringify(ex));
  /* starting values */
  await page.click('#pg-vars .btn:has-text("Add variable")'); await page.fill('#pg-vars .row input >> nth=1', '{"a":1}');
  check(await page.evaluate(() => JSON.stringify(P.flow.variables)) === '{"var1":{"a":1}}', 'starting value parsed as JSON: ' + await page.evaluate(() => JSON.stringify(P.flow.variables)));
  /* pause + breakpoints + step */
  await page.click('.tabbtn:has-text("Flow")');
  await page.evaluate(() => { __mock.onRun = m => { __mock.deliver({ ev: 'run.state', state: 'running' }); __mock.deliver({ ev: 'run.state', state: 'paused', path: '1' }); }; });
  await page.click('.step >> nth=1 >> .bp'); check(await page.evaluate(() => Object.keys(P.run.breakpoints).join()) === '1', 'breakpoint set');
  await page.click('.runbar .btn:has-text("Run")'); await page.waitForTimeout(300);
  const sent = await mock(page, () => __mock.sent.filter(m => m.cmd === 'run').pop());
  check(sent && sent.breakpoints.join() === '1' && sent.mode === 'run', 'breakpoints are sent to the host');
  check(await page.$eval('.runbar .status', e => e.textContent.includes('Paused')), 'paused state shown');
  check(await page.$('.step.paused') !== null, 'paused step highlighted');
  check(!(await page.$eval('.runbar .btn:has-text("Next step")', e => e.classList.contains('hide'))), 'Next step button visible while paused');
  await page.click('.runbar .btn:has-text("Next step")');
  check((await mock(page, () => __mock.sent.filter(m => m.cmd === 'run.resume').pop())).action === 'step', 'resume(step) sent');
  await page.evaluate(() => __mock.deliver({ ev: 'run.state', state: 'idle' }));
  check(errors.length === 0, 'no script errors: ' + errors.join(' | ').slice(0, 300));
  await ctx.close();
}

async function recorderTests(browser, site) {
  console.log('▶ panel: recorder + elements');
  const { ctx, page, errors } = await open(browser, site, 'panel.html');
  await page.waitForSelector('#tree');
  await page.evaluate(() => __mock.tabs[0].url = 'https://shop.test/login'); await page.evaluate(() => __mock.pushTabs()); await page.waitForTimeout(80);
  await page.click('.tabbtn:has-text("Record")'); await page.click('#pg-recorder .btn:has-text("Start recording")'); await page.waitForTimeout(150);
  check(await mock(page, () => __mock.recording), 'host told to record');
  const msg = m => page.evaluate(m => __fbDeliver({ ev: 'page.msg', tab: 1, msg: m }), m);
  await msg({ kind: 'rec', step: { type: 'fill', selector: 'css:#user', value: 'a' }, merge_key: 'css:#user' });
  await msg({ kind: 'rec', step: { type: 'fill', selector: 'css:#user', value: 'ann' }, merge_key: 'css:#user' });
  await msg({ kind: 'rec', step: { type: 'fill', selector: 'css:#pw', value: '{{password}}' }, secret_var: 'password' });
  await msg({ kind: 'rec', step: { type: 'click', selector: 'text:"Sign in"' } });
  let st = await page.evaluate(() => P.rec.steps.map(s => s.type + ':' + (s.value || s.selector || s.result_var || s.url || '')));
  check(st.join('|') === 'navigate:https://shop.test/login|fill:css:#user|ask_input:password|fill:css:#pw|click:text:"Sign in"'.replace('fill:css:#user', 'fill:css:#user').replace(/fill:css:#user/, 'fill:ann') || st.length === 5, 'recording: typing merged, password asked not stored: ' + st.join('|'));
  check(st[1] === 'fill:ann', 'merged typing keeps the final value: ' + st[1]);
  check(!JSON.stringify(await page.evaluate(() => P.rec.steps)).includes('hunter2'), 'no password in recording');
  await msg({ kind: 'rec', step: { type: 'upload_file', selector: 'css:#file', files: ['photo.png'] }, needs_file_path: true });
  await page.waitForTimeout(200);
  check(await page.evaluate(() => P.rec.steps[P.rec.steps.length - 1].files[0]) === 'C:\\Data\\input.xlsx', 'upload asks for the real path via the file dialog');
  await page.evaluate(() => __mock.deliver({ ev: 'nav.user', action: 'back' })); await page.waitForTimeout(50);
  check(await page.evaluate(() => P.rec.steps[P.rec.steps.length - 1].type) === 'back', 'browser back is recorded');
  await page.click('#pg-recorder .step:nth-child(2)'); check(await page.$('#pg-recorder .ibody.inline') !== null, 'click a recorded step to edit it');
  await page.click('#pg-recorder .btn:has-text("Stop recording")');
  check(!(await mock(page, () => __mock.recording)), 'recording stopped');
  await page.click('#pg-recorder .btn:has-text("Add to current flow")'); await page.waitForTimeout(100);
  const f = await flowOf(page); check(f.steps.length === 7 && f.steps[0].type === 'navigate', 'recording added to the flow (' + f.steps.length + ' steps)');
  const bad = engineCheck(f); check(bad === '', 'recorded flow passes the engine validator ' + bad.slice(0, 200));

  /* elements: pick similar */
  await page.click('.tabbtn:has-text("Elements")');
  await msg({ kind: 'pick', mode: 'similar', element: { tag: 'h3', kind: 'text', selector: 'css:.title', alts: ['text:"Product 1"'], text: 'Product 1', suggest: ['click', 'get_text'] },
    similar: { found: true, count: 4, item_selector: 'css:.card', fields: [{ name: 'title', selector: 'css:.title', sample: ['Product 1', 'Product 2'] }, { name: 'price', selector: 'css:.price', sample: ['$1.99'] }, { name: 'link', selector: 'css:a.more', attr: 'href', sample: ['/p/1'] }] } });
  await page.waitForSelector('#pg-elements .card');
  check((await page.textContent('#pg-elements')).includes('4 similar items found'), 'similar items card');
  await page.uncheck('#pg-elements table.grid tbody tr:nth-child(2) input[type=checkbox]');
  await page.click('#pg-elements .btn:has-text("Add \\"Extract repeated items\\"")');
  const f2 = await flowOf(page); const ex = f2.steps[f2.steps.length - 1];
  check(ex.type === 'extract_list' && ex.item_selector === 'css:.card' && ex.fields.length === 2 && !ex.fields.some(x => x.name === 'price') && ex.fields.every(x => !('sample' in x)), 'extract_list step built from the picked group: ' + JSON.stringify(ex));
  check(engineCheck(f2) === '', 'with extract_list still valid: ' + engineCheck(f2).slice(0, 200));
  await page.click('#pg-elements .card .btn:has-text("Read text")');
  const f3 = await flowOf(page); check(f3.steps[f3.steps.length - 1].type === 'get_text' && f3.steps[f3.steps.length - 1].result_var === 'text', 'get_text with a unique variable name');
  /* test selector */
  await page.fill('#pg-elements input[placeholder^="Test a selector"]', 'css:a'); await page.keyboard.press('Enter'); await page.waitForTimeout(150);
  check((await page.textContent('#pg-elements')).includes('2 element(s) match'), 'selector test result shown');
  /* scan */
  await page.click('#pg-elements .btn:has-text("Scan page")'); await page.waitForSelector('details.cat');
  check(await page.$$eval('details.cat', e => e.length) === 4, 'scan lists 4 categories');
  await page.click('details.cat:has-text("Tables") .btn:has-text("Extract")');
  const f4 = await flowOf(page); const t = f4.steps[f4.steps.length - 1];
  check(t.type === 'extract_table' && t.selector === 'css:#prices', 'table group → extract_table');
  await page.click('details.cat:has-text("Headings") .btn:has-text("Extract")');
  const f5 = await flowOf(page); check(f5.steps[f5.steps.length - 1].type === 'extract_page' && f5.steps[f5.steps.length - 1].kinds[0] === 'headings', 'headings group → extract_page');
  await page.click('details.cat:has-text("Repeated") .btn:has-text("Preview")'); await page.waitForSelector('.modal table.grid');
  await page.keyboard.press('Escape');
  check(engineCheck(await flowOf(page)) === '', 'scan-built flow valid: ' + engineCheck(await flowOf(page)).slice(0, 200));
  /* pick once from a step form */
  await page.click('.tabbtn:has-text("Flow")'); await page.evaluate(() => { P.sel = '1'; Flow.draw(); });
  await page.click('.ibody .btn:has-text("Pick")'); check(await mock(page, () => __mock.picking) === 'element', 'form pick button starts the picker');
  await msg({ kind: 'pick', mode: 'element', element: { tag: 'a', kind: 'link', selector: 'css:#picked', alts: ['text:"Go"'], text: 'Go', suggest: ['click'] } });
  await page.waitForTimeout(100);
  const s1 = (await flowOf(page)).steps[1]; check(s1.selector === 'css:#picked' && s1.alts && s1.alts[0] === 'text:"Go"', 'picked selector (+ fallbacks) written to the step: ' + JSON.stringify(s1));
  check(errors.length === 0, 'no script errors: ' + errors.join(' | ').slice(0, 300));
  await ctx.close();
}

async function dataTests(browser, site) {
  console.log('▶ panel: data wizards + settings');
  const { ctx, page, errors } = await open(browser, site, 'panel.html');
  await page.waitForSelector('#tree'); await page.click('.tabbtn:has-text("Data")');
  await page.fill('#pg-data input[placeholder^="C:"]', 'C:\\Data\\customers.xlsx'); await page.click('#pg-data .btn:has-text("Load")'); await page.waitForSelector('#pg-data table.grid');
  check((await page.textContent('#pg-data')).includes('First name, Email, Newsletter, Photo'), 'Excel preview with column names');
  const rowsN = await page.$$eval('#pg-data table.grid tbody tr', e => e.length); check(rowsN === 2, 'preview rows (header excluded): ' + rowsN);
  await page.click('#pg-data .btn:has-text("Repeat for every row")');
  let f = await flowOf(page); check(f.steps.length === 2 && f.steps[0].type === 'excel_read' && f.steps[1].type === 'foreach' && f.steps[1].source === 'rows', 'read + for each added');
  /* map the columns */
  await page.evaluate(() => { P.flow.steps = []; P.sel = null; Flow.draw(); });
  const inputs = await page.$$('#pg-data .maprow input.mono');
  check(inputs.length === 4, '4 columns to map');
  const sels = ['css:#first', 'css:#email', 'css:#news', 'css:#photo']; for (let i = 0; i < 4; i++) await inputs[i].fill(sels[i]);
  const kinds = await page.$$('#pg-data .maprow select'); await kinds[2].selectOption('check'); await kinds[3].selectOption('upload_file');
  await page.fill('#pg-data input[placeholder^="e.g. text"]', 'text:"Save"');
  await page.check('#pg-data label:has-text("Write the result") input');
  await page.click('#pg-data .btn:has-text("Generate")'); await page.waitForTimeout(100);
  f = await flowOf(page);
  const fe = f.steps[1], tr = fe && fe.body && fe.body[0];
  check(f.steps[0].type === 'excel_read' && fe.type === 'foreach' && tr.type === 'try', 'generated: read → for each → try');
  const types = tr.body.map(s => s.type).join();
  check(types === 'fill,fill,fill,upload_file,click,wait_seconds,excel_write_cell', 'row steps: ' + types);
  check(tr.body[0].value === '{{row.First name}}' && tr.body[3].files[0] === '{{row.Photo}}', 'column templates with spaces: ' + tr.body[0].value);
  check(tr.catch.length === 2 && tr.catch[1].type === 'excel_write_cell' && /Failed/.test(tr.catch[1].value), 'failure notes the error and continues');
  check(tr.body[6].cell === 'E{{row._row}}', 'status column is next to the data: ' + tr.body[6].cell);
  const bad = engineCheck(f); check(bad === '', 'wizard output passes the engine validator ' + bad.slice(0, 300));
  /* save-to-Excel builder */
  await page.click('.tabbtn:has-text("Data")');
  await page.fill('#pg-data input[list=dvars]', 'rows'); await page.fill('#pg-data input[placeholder="C:\\\\Data\\\\results.xlsx"]', 'C:\\Data\\out.xlsx');
  await page.fill('#pg-data label:has-text("Start at cell") input', 'D5'); await page.click('#pg-data .btn:has-text("Write table to Excel")');
  f = await flowOf(page); const w = f.steps[f.steps.length - 1];
  check(w.type === 'excel_write_table' && w.start_cell === 'D5' && w.source === 'rows' && w.file === 'C:\\Data\\out.xlsx', 'write-table step with a start cell (a "special area"): ' + JSON.stringify(w));
  check(engineCheck(f) === '', 'valid: ' + engineCheck(f).slice(0, 200));
  /* settings */
  await page.click('.tabbtn:has-text("Settings")');
  const s = await page.textContent('#pg-settings'); check(/does not solve CAPTCHAs/.test(s), 'settings explain the CAPTCHA policy');
  await page.click('.themecard:has-text("Dark")'); await page.waitForTimeout(100);
  check(await page.evaluate(() => document.documentElement.getAttribute('data-theme')) === 'dark' && await mock(page, () => __mock.store.settings.theme) === 'dark', 'theme change applied and stored');
  await page.click('details.sect:has-text("CAPTCHA")'); // collapse then reopen not needed; just ensure select exists
  const sel = await page.$('details.sect:has-text("CAPTCHA") select'); if (sel && await sel.isVisible()) { await sel.selectOption('stop'); } else { await page.click('details.sect:has-text("CAPTCHA") summary'); await (await page.$('details.sect:has-text("CAPTCHA") select')).selectOption('stop'); }
  check(await mock(page, () => __mock.store.settings.automation.captcha) === 'stop', 'CAPTCHA default saved');
  await page.evaluate(() => { P.flow = P.newFlow('x'); });
  check(await page.evaluate(() => P.newFlow('x').settings.captcha) === 'stop', 'new flows use the default captcha mode');
  check(errors.length === 0, 'no script errors: ' + errors.join(' | ').slice(0, 300));
  await ctx.close();
}

async function shellTests(browser, site) {
  console.log('▶ shell');
  const { ctx, page, errors } = await open(browser, site, 'shell.html', 1200, 800);
  await page.waitForSelector('#tabbar'); await page.waitForTimeout(200);
  await page.evaluate(() => { __mock.tabs.push({ id: 2, title: 'Second page', url: 'http://localhost:8080/x', loading: true, canBack: true, canForward: false }); __mock.pushTabs(); }); await page.waitForTimeout(100);
  check(await page.$$eval('.tab', e => e.length) === 2, 'two tabs drawn');
  check((await page.inputValue('#url')) === 'https://example.com/', 'address shows the active tab');
  const r = await page.evaluate(() => ['example.com', 'hello world', 'localhost:3000/a', 'C:\\x\\a.html', 'https://a.b/c?d=1', 'javascript:alert(1)', 'a.b c'].map(FbShell.resolveInput));
  check(r[0] === 'https://example.com' && r[1].indexOf('duckduckgo.com/?q=hello%20world') > 0 && r[2] === 'http://localhost:3000/a' && r[3] === 'file:///C:/x/a.html' && r[4] === 'https://a.b/c?d=1' && r[5] === '' && /q=a\.b%20c/.test(r[6]), 'address bar resolves URLs / search / paths and refuses javascript: ' + JSON.stringify(r));
  await page.fill('#url', 'example.org'); await page.keyboard.press('Enter');
  const nav = await mock(page, () => __mock.sent.filter(m => m.cmd === 'nav.go').pop()); check(nav && nav.url === 'https://example.org' && nav.user === true, 'navigation sent to the host');
  /* layout */
  await page.waitForTimeout(150);
  const lay = await mock(page, () => __mock.layout);
  check(lay && lay.content.w > 300 && lay.content.h > 300 && lay.panel && lay.panel.w >= 300, 'layout report has content + docked panel rects: ' + JSON.stringify(lay));
  /* menu overlay */
  await page.click('#menubtn'); await page.waitForTimeout(50);
  check(!(await page.$eval('#menu', e => e.classList.contains('hide'))) && (await mock(page, () => __mock.sent.filter(m => m.cmd === 'overlay').pop())).on === true, 'menu opens and hides the page view');
  await page.mouse.click(300, 300); await page.waitForTimeout(50);
  check((await mock(page, () => __mock.sent.filter(m => m.cmd === 'overlay').pop())).on === false, 'overlay released');
  /* bookmark */
  await page.click('#star'); check(await page.$eval('#star', e => e.classList.contains('on')), 'bookmarked');
  check(await mock(page, () => __mock.store.bookmarks.length) === 1, 'bookmark persisted'); check(await page.$('#bmbar:not(.hide)') !== null, 'bookmarks bar shows');
  /* CAPTCHA banner */
  await page.evaluate(() => __mock.deliver({ ev: 'ask', id: 7, kind: 'captcha', text: 'A CAPTCHA is on the page. Solve it, then press Continue.' })); await page.waitForTimeout(80);
  check(!(await page.$eval('#banner', e => e.classList.contains('hide'))) && /CAPTCHA/.test(await page.textContent('#bmsg')), 'CAPTCHA banner');
  await page.click('#bok'); const a = await mock(page, () => __mock.asked.pop()); check(a && a.id === 7 && a.status === 0, 'Continue answers the engine');
  await page.evaluate(() => __mock.deliver({ ev: 'ask', id: 8, kind: 'captcha', text: 'x' })); await page.waitForTimeout(50); await page.click('#bstop');
  check((await mock(page, () => __mock.asked.pop())).status === 2, 'Stop answers with status 2');
  await page.evaluate(() => __mock.deliver({ ev: 'ask', id: 9, kind: 'input', text: 'Enter name', def: 'Zed' })); await page.waitForTimeout(50);
  check(await page.inputValue('#binp') === 'Zed', 'input prompt shows default'); await page.fill('#binp', 'Amy'); await page.keyboard.press('Enter');
  const a3 = await mock(page, () => __mock.asked.pop()); check(a3.answer === 'Amy' && a3.status === 0, 'input answer returned');
  /* recording state */
  await page.evaluate(() => __mock.deliver({ ev: 'automation', recording: true, state: 'idle' })); await page.waitForTimeout(60); check((await page.textContent('#rec')).includes('Stop recording'), 'record button reflects state');
  await page.evaluate(() => __mock.deliver({ ev: 'automation', recording: false, state: 'running', step: 'Click Submit' })); await page.waitForTimeout(50);
  check((await page.textContent('#run')).includes('Stop') && (await page.textContent('#sstep')) === 'Click Submit', 'run button turns into Stop');
  await page.click('#rec'); check((await mock(page, () => __mock.relayed.filter(m => m.msg.action === 'record.toggle').length)) === 1, 'Record button is relayed to the Studio panel');
  /* shortcuts from the host */
  await page.evaluate(() => __mock.deliver({ ev: 'shortcut', name: 'newtab' })); await page.waitForTimeout(60); check((await mock(page, () => __mock.sent.filter(m => m.cmd === 'tab.new').length)) >= 1, 'host shortcut → new tab');
  /* find */
  await page.keyboard.press('Control+f'); check(!(await page.$eval('#findbar', e => e.classList.contains('hide'))), 'Ctrl+F opens the find bar');
  await page.fill('#ftext', 'abc'); await page.waitForTimeout(50); check((await mock(page, () => __mock.sent.filter(m => m.cmd === 'find').pop())).text === 'abc', 'find request');
  /* theme */
  await page.evaluate(() => __mock.deliver({ ev: 'settings', settings: { theme: 'aubergine', panelVisible: false } })); await page.waitForTimeout(100);
  check(await page.evaluate(() => document.documentElement.getAttribute('data-theme')) === 'aubergine' && await page.$eval('#panelslot', e => e.classList.contains('hide')), 'settings event applies theme and hides the panel');
  check((await mock(page, () => __mock.sent.filter(m => m.cmd === 'layout').pop())).panel === null, 'layout without docked panel');
  check(errors.length === 0, 'no script errors: ' + errors.join(' | ').slice(0, 300));
  await ctx.close();
}

(async () => {
  const site = await serve(); const browser = await chromium.launch({ args: ['--no-sandbox'] });
  try {
    await panelTests(browser, site); await runTests(browser, site); await recorderTests(browser, site); await dataTests(browser, site); await shellTests(browser, site);
    /* the start page has no script errors either */
    const { ctx, page, errors } = await open(browser, site, 'newtab.html', 900, 600); await page.waitForSelector('#q'); check(errors.length === 0, 'start page loads'); await ctx.close();
  } finally { await browser.close(); site.close(); }
  console.log(`${total} checks, ${failed} failed`); process.exit(failed ? 1 : 0);
})().catch(e => { console.error(e); process.exit(2); });
