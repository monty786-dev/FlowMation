/* In-page mock of the native host, injected before the UI scripts run. It implements the UI<->host protocol
   (see docs/PROTOCOL.md) with in-memory state so the panel and shell can be tested in plain Chromium. */
(function () {
  var store = {}, lib = {}, versions = {}, tabs = [{ id: 1, title: 'Example', url: 'https://example.com/', loading: false, canBack: false, canForward: false }];
  var M = window.__mock = { sent: [], relayed: [], store: store, lib: lib, dialogPath: 'C:\\Data\\input.xlsx', previewRows: null, running: null, tabs: tabs, asked: [] };
  function deliver(o) { setTimeout(function () { window.__fbDeliver(o); }, 0); }
  M.deliver = deliver;
  function reply(m, extra) { var o = { id: m.id, ok: true }; if (extra) for (var k in extra) o[k] = extra[k]; deliver(o); }
  function fail(m, msg) { deliver({ id: m.id, ok: false, error: msg }); }
  function pushTabs() { deliver({ ev: 'tabs', tabs: tabs, active: tabs[0] && tabs[0].id }); }

  window.__fbMockPost = function (s) {
    var m = JSON.parse(s); M.sent.push(m);
    switch (m.cmd) {
      case 'store.get': return reply(m, { value: store[m.key] === undefined ? null : store[m.key] });
      case 'store.set': store[m.key] = m.value; if (m.key === 'settings') deliver({ ev: 'settings', settings: m.value }); return;
      case 'app.info': return reply(m, { version: '1.0.0-test' });
      case 'lib.list': return reply(m, { flows: Object.keys(lib).map(function (n) { return { name: n, title: lib[n].name, steps: lib[n].steps.length, description: lib[n].description, updated: '2026-11-24 10:00' }; }) });
      case 'lib.load': if (!lib[m.name]) return fail(m, 'No such flow'); return reply(m, { flow: JSON.parse(JSON.stringify(lib[m.name])) });
      case 'lib.save': if (lib[m.name]) { (versions[m.name] = versions[m.name] || []).push({ id: 'v' + Date.now(), when: '2026-11-24 09:00' }); } lib[m.name] = JSON.parse(JSON.stringify(m.flow)); return reply(m, { name: m.name });
      case 'lib.delete': delete lib[m.name]; return reply(m);
      case 'lib.versions': return reply(m, { versions: versions[m.name] || [] });
      case 'validate': {
        var issues = [];
        (function walk(steps, base) { (steps || []).forEach(function (s, i) { var p = base + i; if (s.type === 'click' && !s.selector) issues.push({ path: p, level: 'error', message: 'Selector is required' });
          ['body', 'then', 'else', 'catch'].forEach(function (k) { if (Array.isArray(s[k])) walk(s[k], p + '.' + k + '.'); }); }); })(m.flow.steps, '');
        return reply(m, { issues: issues });
      }
      case 'file.dialog': return reply(m, { path: M.dialogPath, paths: [M.dialogPath] });
      case 'data.preview': return reply(m, { sheets: ['Customers', 'Orders'], sheet: m.sheet || 'Customers', total_rows: 3, rows: M.previewRows || [['First name', 'Email', 'Newsletter', 'Photo'], ['Ann', 'ann@x.com', 'yes', 'C:\\p\\a.png'], ['Bob', 'bob@x.com', 'no', 'C:\\p\\b.png']] });
      case 'data.export': M.exported = m; return reply(m);
      case 'page.call': {
        if (m.name === 'categorize') return reply(m, { result: JSON.stringify({ ok: true, url: 'https://shop.test/', title: 'Shop', groups: [
          { category: 'Tables', label: 'Prices (3 rows, columns: Name, Qty)', selector: 'css:#prices', count: 3, step: 'extract_table', sample: 'Apple | 3' },
          { category: 'Repeated items', label: '4 similar <div> items: Product 1', selector: 'css:.card', count: 4, step: 'extract_list', fields: [{ name: 'title', selector: 'css:.title' }, { name: 'link', selector: 'css:a.more', attr: 'href' }], sample: 'Product 1' },
          { category: 'Headings', label: '3 headings', selector: '', count: 3, step: 'extract_page', kinds: ['headings'], sample: 'Shop' },
          { category: 'Forms', label: 'Login form (3 fields)', selector: 'css:#login', count: 1, step: 'autofill_form', sample: '' }] }) });
        if (m.name === 'test') return reply(m, { result: JSON.stringify({ ok: true, count: 2, samples: [{ tag: 'a', text: 'one' }, { tag: 'a', text: 'two' }] }) });
        return reply(m, { result: '{}' });
      }
      case 'page.exec': return reply(m, { result: m.op === 'extract_list' ? { ok: true, count: 2, rows: [{ title: 'A' }, { title: 'B' }] } : { ok: true, rows: [{ a: 1 }, { a: 2 }], columns: ['a'] } });
      case 'pick.start': M.picking = m.mode; return reply(m);
      case 'pick.stop': M.picking = null; return;
      case 'rec.start': M.recording = true; return reply(m);
      case 'rec.stop': M.recording = false; return;
      case 'relay': M.relayed.push(m); return;
      case 'run': return M.onRun ? M.onRun(m) : defaultRun(m);
      case 'run.stop': deliver({ ev: 'run.state', state: 'idle' }); return;
      case 'run.pause': deliver({ ev: 'run.state', state: 'paused', path: '1' }); return;
      case 'run.resume': return;
      case 'ask.answer': M.asked.push(m); return;
      case 'schedule.list': return reply(m, { tasks: [{ name: 'daily-report', next: '2026-11-25 09:00' }] });
      case 'schedule.create': M.scheduled = m; return reply(m);
      case 'backup.create': return reply(m, { path: 'C:\\b\\all.fbbackup', count: 3 });
      case 'backup.restore': M.restoreMode = m.mode; return reply(m, { path: 'C:\\b\\all.fbbackup', report: { added: 2, replaced: 1, skipped: 0, renamed: 0 } });
      case 'flow.import': return reply(m, { flow: { format: 'flowbrowser-flow', version: 1, name: 'Imported', settings: {}, variables: {}, steps: [{ type: 'navigate', url: 'https://x.test' }] } });
      case 'flow.export': return reply(m, { path: 'C:\\x\\f.fbflow' });
      case 'file.save_text': return reply(m, { path: 'C:\\x\\log.txt' });
      case 'layout': M.layout = m; return;
      default: return m.id != null ? reply(m) : undefined;
    }
  };
  function defaultRun(m) {
    var steps = m.flow.steps, i = 0;
    deliver({ ev: 'run.state', state: 'running' });
    deliver({ ev: 'run.event', e: { ev: 'flow_start', name: m.flow.name, steps: steps.length } });
    steps.forEach(function (s, idx) {
      var p = String(idx);
      deliver({ ev: 'run.event', e: { ev: 'step_start', path: p, type: s.type, name: s.name || '' } });
      deliver({ ev: 'run.event', e: { ev: 'step_end', path: p, type: s.type, ok: true, ms: 12 + idx } });
    });
    deliver({ ev: 'run.event', e: { ev: 'vars', vars: { rows: [{ Name: 'Ann', Qty: 1 }, { Name: 'Bob', Qty: 2 }], title: 'Hello' } } });
    deliver({ ev: 'run.event', e: { ev: 'flow_end', result: 'ok', ms: 99 } });
    deliver({ ev: 'run.state', state: 'idle' });
  }
  M.pushTabs = pushTabs;
})();
