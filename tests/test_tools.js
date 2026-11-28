'use strict';
const { runFlow, env, LEGACY, STRIP_MODERN, LEGACY_JS } = require('./bridge');
const fs = require('fs'), path = require('path'), os = require('os');
const ROOT = path.resolve(__dirname, '..');
const RUNTIME = fs.readFileSync(path.join(ROOT, 'src/js/fb_runtime.js'), 'utf8');
const TOOLS = fs.readFileSync(path.join(ROOT, 'src/js/fb_tools.js'), 'utf8').replace('@@FB_TOKEN@@', 'T0K3N');
let fails = 0, total = 0;
function check(c, m) { total++; if (!c) { fails++; console.log('  FAIL:', m); } }
const eq = (a, b, m) => check(JSON.stringify(a) === JSON.stringify(b), m + ' — got ' + JSON.stringify(a) + ' want ' + JSON.stringify(b));

async function newPage(e) {
  const ctx = await e.browser.newContext({ viewport: { width: 1100, height: 800 } });
  const msgs = [];
  if (LEGACY) {                                  /* FB_LEGACY=1: old engine, messages are polled like the Wine host does */
    await ctx.addInitScript({ content: STRIP_MODERN });
    await ctx.addInitScript({ content: LEGACY_JS });
  }
  await ctx.addInitScript({ content: RUNTIME });
  await ctx.addInitScript({ content: TOOLS });
  if (!LEGACY) await ctx.exposeFunction('__fbPost', s => { msgs.push(JSON.parse(s)); });
  const page = await ctx.newPage();
  if (LEGACY) {
    const timer = setInterval(async () => {
      try { const s = await page.evaluate(() => window.__fbPullTools && window.__fbPullTools()); if (s) JSON.parse(s).forEach(x => msgs.push(x)); } catch (x) { /* navigating */ }
    }, 20);
    ctx.on('close', () => clearInterval(timer));
  }
  return { ctx, page, msgs };
}

/* what the panel does with the stream of 'rec' messages */
function applySteps(msgs) {
  const steps = [];
  for (const m of msgs) {
    if (m.kind !== 'rec') continue;
    const last = steps[steps.length - 1];
    if (m.merge_key && last && last.key === m.merge_key) { steps[steps.length - 1] = { key: m.merge_key, step: m.step, meta: m }; }
    else steps.push({ key: m.merge_key, step: m.step, meta: m });
  }
  return steps;
}

(async () => {
  const e = await env();
  const base = e.site.url;

  console.log('▶ recorder');
  {
    const { ctx, page, msgs } = await newPage(e);
    await page.goto(base + '/form.html');
    await page.waitForFunction(() => !document.getElementById('overlay'));
    check(msgs.some(m => m.kind === 'tools_ready' && m.token === 'T0K3N'), 'tools announce themselves with the token');
    await page.evaluate(() => window.__fb.tools.cmd({ name: 'recorder-start' }));
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fbrec_'));
    const up = path.join(dir, 'doc.txt'); fs.writeFileSync(up, 'x');
    await page.fill('#fn', 'Ana');
    await page.fill('input[name=email]', 'ana@example.com');
    await page.fill('#pw', 'TopSecret!123');
    await page.selectOption('#country', 'in');
    await page.check('#agree');
    await page.uncheck('#news');
    await page.check('input[value=pro]');
    await page.click('#go');
    await page.waitForTimeout(900);
    await page.evaluate(() => window.__fb.tools.cmd({ name: 'recorder-stop' }));
    const raw = JSON.stringify(msgs);
    check(!raw.includes('TopSecret'), 'the password never leaves the page');
    const steps = applySteps(msgs);
    const types = steps.map(s => s.step.type);
    console.log('  recorded:', steps.map(s => s.step.type + ' ' + (s.step.selector || '') + (s.step.value !== undefined ? ' = ' + s.step.value : '')).join(' | '));
    eq(types, ['fill', 'fill', 'fill', 'select_option', 'check', 'check', 'check', 'click'], 'recorded step types');
    const pw = steps[2];
    eq([pw.step.value, pw.meta.secret_var], ['{{password}}', 'password'], 'password placeholder');
    eq(steps[3].step.value, 'India', 'select recorded by visible text');
    eq(steps.slice(4, 7).map(s => s.step.state), ['check', 'uncheck', 'check'], 'check states');
    check(steps.every(s => s.step.selector && !/^css:(html|body)/.test(s.step.selector)), 'every step has a selector');
    check(steps[0].step.selector === 'css:#fn', 'id preferred: ' + steps[0].step.selector);
    check(steps[1].step.selector.startsWith('css:input[name="email"]'), 'name attribute: ' + steps[1].step.selector);
    await ctx.close();

    /* replay the recording on a fresh page */
    const flowSteps = [{ type: 'navigate', url: '{{base}}/form.html' }, { type: 'wait_element', selector: 'css:#overlay', state: 'detached' }]
      .concat(steps.map(s => s.step));
    const r = await runFlow({ format: 'flowbrowser-flow', version: 1, name: 'replay', variables: { password: 'TopSecret!123' }, settings: { timeout_ms: 5000 },
      steps: flowSteps.concat([{ type: 'run_js', code: 'return {s: state(), n: window.__submitted}', result_var: 'res' }]) });
    console.log('  replay exit', r.code, r.stderr.trim().slice(0, 300));
    check(r.code === 0, 'recorded flow replays');
    const s = (r.vars.res || {}).s || {};
    eq([s.first, s.email, s.pw, s.country, s.agree, s.news, s.plan, (r.vars.res || {}).n], ['Ana', 'ana@example.com', 'TopSecret!123', 'in', true, false, 'pro', 1], 'replay reproduced the form');
    await r.done();
  }

  console.log('▶ recorder: upload, frames, shadow DOM');
  {
    const { ctx, page, msgs } = await newPage(e);
    await page.goto(base + '/upload.html');
    await page.evaluate(() => window.__fb.tools.cmd({ name: 'recorder-start' }));
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fbrec_')); const f = path.join(dir, 'doc.txt'); fs.writeFileSync(f, 'x');
    await page.setInputFiles('#plain', f);
    await page.waitForTimeout(200);
    let st = applySteps(msgs);
    eq(st.map(s => s.step.type), ['upload_file'], 'upload recorded');
    eq(st[0] && st[0].step.files, ['doc.txt'], 'file name only (the browser hides paths)');
    check(st[0] && st[0].meta.needs_file_path === true, 'flagged: needs a file path');
    msgs.length = 0;
    await page.goto(base + '/shadow.html');
    await page.evaluate(() => window.__fb.tools.cmd({ name: 'recorder-start' }));
    await page.waitForTimeout(300);
    await page.click('#sb');
    await page.frameLocator('#fr').locator('#fb').click();
    await page.waitForTimeout(400);
    st = applySteps(msgs);
    console.log('  recorded:', st.map(s => s.step.type + ' ' + s.step.selector).join(' | '));
    check(st.length === 2 && st[0].step.selector === 'css:#sb', 'shadow DOM click selector: ' + (st[0] && st[0].step.selector));
    check(st[1] && /^css:#fr >> /.test(st[1].step.selector), 'iframe selector prefixed: ' + (st[1] && st[1].step.selector));
    await ctx.close();
    /* replay both */
    const r = await runFlow({ format: 'flowbrowser-flow', version: 1, name: 'r', variables: {}, settings: { timeout_ms: 4000 },
      steps: [{ type: 'navigate', url: '{{base}}/shadow.html' }].concat(st.map(s => s.step)).concat([{ type: 'run_js', code: 'return {sb: window.__sb, fb: window.__fb_clicks}', result_var: 'res' }]) });
    check(r.code === 0, 'frame + shadow recording replays: ' + r.stderr.trim().slice(0, 200));
    eq(r.vars.res, { sb: 1, fb: 1 }, 'both clicks landed');
    await r.done();
  }

  console.log('▶ picker + grouping');
  {
    const { ctx, page, msgs } = await newPage(e);
    await page.goto(base + '/list?page=1');
    await page.evaluate(() => window.__fb.tools.cmd({ name: 'picker-start', opts: { mode: 'similar' } }));
    await page.hover('.card:nth-child(2) .title');
    await page.click('.card:nth-child(2) .title');
    await page.waitForTimeout(200);
    const pick = msgs.find(m => m.kind === 'pick');
    check(!!pick, 'pick message received');
    if (pick) {
      check(pick.element.tag === 'h3' && /Product 2/.test(pick.element.text), 'element described');
      check(pick.similar.found && pick.similar.count === 4, 'four similar items found: ' + JSON.stringify(pick.similar).slice(0, 200));
      const names = (pick.similar.fields || []).map(f => f.name);
      check(names.includes('title') && names.includes('price'), 'fields named from classes: ' + names);
      check((pick.similar.fields || []).some(f => f.attr === 'href'), 'link field has attr=href');
      check((pick.similar.fields || []).some(f => f.attr === 'src'), 'image field has attr=src');
      /* the proposed selector + fields are directly usable by extract_list */
      const r = await runFlow({ format: 'flowbrowser-flow', version: 1, name: 'x', variables: {}, settings: { timeout_ms: 4000 }, steps: [
        { type: 'navigate', url: '{{base}}/list?page=1' },
        { type: 'extract_list', item_selector: pick.similar.item_selector, fields: pick.similar.fields.map(f => ({ name: f.name, selector: f.selector, attr: f.attr })), result_var: 'rows' }] });
      check(r.code === 0 && r.vars.rows && r.vars.rows.length === 4, 'extract_list works with the suggested fields: ' + r.stderr.trim().slice(0, 200));
      await r.done();
    }
    await page.evaluate(() => window.__fb.tools.cmd({ name: 'picker-stop' }));
    const before = await page.evaluate(() => location.href);
    await page.click('a.more >> nth=0').catch(() => {});             // picking is off: the link works again
    await page.waitForTimeout(300);
    check((await page.evaluate(() => location.href)) !== before, 'clicks work normally after the picker stops');

    await page.goto(base + '/table.html');
    const cats = JSON.parse(await page.evaluate(() => window.__fb.tools.call('categorize')));
    const names = cats.groups.map(g => g.category);
    console.log('  categories on table.html:', cats.groups.map(g => g.category + ' [' + g.label + ']').join('; '));
    check(names.includes('Tables') && names.includes('Headings') && names.includes('Links') && names.includes('Images'), 'page categorised');
    check(cats.groups.filter(g => g.category === 'Tables').length === 3, 'three tables listed');
    await page.goto(base + '/list?page=1');
    const c2 = JSON.parse(await page.evaluate(() => window.__fb.tools.call('categorize')));
    const rep = c2.groups.find(g => g.category === 'Repeated items');
    check(rep && rep.count === 4 && rep.step === 'extract_list' && rep.fields.length >= 3, 'repeated product cards detected: ' + (rep && rep.label));
    const t1 = JSON.parse(await page.evaluate(sel => window.__fb.tools.call('test', JSON.stringify({ selector: sel })), rep.selector));
    check(t1.ok && t1.count === 4 && t1.samples.length === 4, 'selector tester counts matches: ' + JSON.stringify(t1).slice(0, 120));
    check(await page.evaluate(() => document.querySelectorAll('[data-fb-ui]').length) >= 4, 'matches are highlighted');
    const t2 = JSON.parse(await page.evaluate(() => window.__fb.tools.call('test', JSON.stringify({ selector: 'css:#nope' }))));
    check(t2.ok && t2.count === 0, 'selector tester: no match is count 0');
    const t3 = JSON.parse(await page.evaluate(() => window.__fb.tools.call('test', JSON.stringify({ selector: 'xpath:///[' }))));
    check(t3.ok === false || t3.count === 0, 'selector tester: bad selector does not throw');
    await ctx.close();
  }

  console.log(`${total} checks, ${fails} failed`);
  await e.close();
  process.exit(fails ? 1 : 0);
})().catch(async err => { console.log('EXCEPTION', err.stack); try { await (await env()).close(); } catch (x) {} process.exit(1); });
