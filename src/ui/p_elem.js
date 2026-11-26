/* Studio panel - Elements tab: pick elements, group similar items, scan the page by category, test selectors. */
(function () {
  'use strict';
  var Elem = window.Elem = {};
  var box, scanBox, cardBox, simBox, testIn, testOut, pickBtn, simBtn, state = { cb: null, mode: null }, current = null;

  function pageCall(name, arg) {
    return Host.call('page.call', { name: name, arg: arg || {} }, 20000).then(function (r) { try { return typeof r.result === 'string' ? JSON.parse(r.result) : r.result; } catch (e) { throw new Error('The page did not answer'); } });
  }
  function pageExec(op, args) { return Host.call('page.exec', { op: op, args: args || {} }, 30000).then(function (r) { return r.result; }); }
  function uniqueVar(base) {
    var used = {}; P.knownVars().forEach(function (v) { used[v] = 1; });
    if (P.rec.on) P.rec.steps.forEach(function (s) { if (s.result_var) used[s.result_var] = 1; });
    var n = base, i = 2; while (used[n]) n = base + i++;
    return n;
  }
  function stepFor(type, sel, extra) { var s = P.newStep(type); if (sel != null) s.selector = sel.selector || sel; if (sel && sel.alts && sel.alts.length) s.alts = sel.alts; return Object.assign(s, extra || {}); }

  /* ------------------------------------------------------------ picking */
  function startPick(mode, cb) { state.cb = cb || null; state.mode = mode; Host.call('pick.start', { mode: mode }).then(function () { setPicking(true); }, function (e) { toast(e.message, 'err'); state.cb = null; }); }
  function setPicking(on) { if (pickBtn) { pickBtn.classList.toggle('on', on); simBtn.classList.toggle('on', on && state.mode === 'similar'); } if (!on) state.mode = null; }
  Elem.pickOnce = function (cb) { toast('Click the element on the page (Esc cancels)'); startPick('element', cb); };
  Elem.stopPick = function () { Host.send('pick.stop'); setPicking(false); state.cb = null; };

  Elem.onMsg = function (m) {
    if (m.kind === 'pick_cancel') { state.cb = null; setPicking(false); return; }
    if (m.kind !== 'pick') return;
    Host.send('pick.stop'); setPicking(false);
    if (state.cb) { var cb = state.cb; state.cb = null; cb(m.element, m.similar); return; }
    current = { el: m.element, sim: m.similar && m.similar.found ? m.similar : null, mode: m.mode };
    drawPicked(); Panel.show('elements');
  };

  /* ------------------------------------------------------------ selector tester */
  Elem.test = function (sel, out) {
    if (!sel) { toast('Type or pick a selector first.', 'err'); return Promise.resolve(); }
    return pageCall('test', { selector: sel }).then(function (r) {
      var msg = !r.ok ? '⚠ ' + r.error : r.count === 0 ? '✖ No element matches.' : '✔ ' + r.count + ' element(s) match' + (r.count > 1 ? ' (the step uses the first one unless it extracts a list)' : '.');
      if (out) { clear(out); out.appendChild(h('div', { text: msg })); (r.samples || []).forEach(function (s) { out.appendChild(h('div.small.muted.ell', { text: '<' + s.tag + '>  ' + s.text })); }); }
      else toast(msg, r.ok && r.count ? 'ok' : 'err');
    }, function (e) { toast(e.message, 'err'); });
  };

  /* ------------------------------------------------------------ picked element card */
  var ACTION_HINT = { click: 'Click', hover: 'Hover', fill: 'Fill with value', type_text: 'Type like a person', get_text: 'Read text', get_attribute: 'Read attribute', select_option: 'Choose option', check: 'Check / uncheck',
    set_slider: 'Set slider', upload_file: 'Upload file', extract_table: 'Extract table', wait_element: 'Wait for it', element_exists: 'Does it exist?', scroll: 'Scroll to it', drag_drop: 'Drag & drop' };
  function drawPicked() {
    clear(cardBox); clear(simBox);
    if (!current) { cardBox.appendChild(h('div.muted.small.pad', { text: 'Nothing picked yet. Press "Pick element" and click anything on the page.' })); return; }
    var d = current.el, selIn = h('input.mono', { type: 'text', value: d.selector, spellcheck: 'false' });
    var valIn = h('input', { type: 'text', value: d.kind === 'select' ? '' : d.value || '', placeholder: d.kind === 'select' ? 'option text' : 'value to enter' });
    var optSel = d.options ? h('select', [h('option', { value: '', text: '— choose option —' })].concat(d.options.map(function (o) { return h('option', { value: o, text: o }); }))) : null;
    if (optSel) optSel.onchange = function () { valIn.value = optSel.value; };
    function cur() { return { selector: selIn.value.trim() || d.selector, alts: selIn.value.trim() && selIn.value.trim() !== d.selector ? [] : d.alts }; }
    var needsValue = (d.suggest || []).some(function (t) { return t === 'fill' || t === 'type_text' || t === 'select_option' || t === 'set_slider'; });
    cardBox.appendChild(h('div.card',
      h('div.row', h('b', { text: '<' + d.tag + '>' }), h('span.chip', { text: d.kind || 'element' }), h('span.grow'), h('button.btn.sm.flat', { text: 'Highlight', onclick: function () { Elem.test(selIn.value); } })),
      d.text ? h('div.small.muted.ell', { text: '“' + d.text.slice(0, 120) + '”' }) : null,
      h('div.small.muted', { text: 'Selector' }), selIn,
      (d.alts && d.alts.length) ? h('div.small.muted', { text: 'Fallbacks: ' + d.alts.join('   ') }) : null,
      needsValue ? h('div.col', h('div.small.muted', { text: 'Value (you can use {{row.Column}} later)' }), optSel, valIn) : null,
      h('div.small.muted', { text: 'Add a step:' }),
      h('div.row.wrap', (d.suggest || ['click', 'get_text']).map(function (t) {
        return h('button.btn.sm', { text: ((P.defs[t] || {}).icon || '') + ' ' + (ACTION_HINT[t] || (P.defs[t] || {}).label || t), onclick: function () { addAction(t, cur(), d, valIn.value); } });
      }))));
    if (current.sim) drawSimilar(current.sim);
    else if (current.mode === 'similar') simBox.appendChild(h('div.card', h('div.small.muted', { text: 'No group of similar items was found around this element. Try clicking one product / row / card (not the page background).' })));
  }
  function addAction(type, sel, d, value) {
    var s = stepFor(type, sel);
    switch (type) {
      case 'fill': case 'type_text': s[type === 'fill' ? 'value' : 'text'] = value; s.name = 'Fill ' + (d.attrs && (d.attrs.placeholder || d.attrs.name) || d.tag); break;
      case 'select_option': s.value = value; break;
      case 'set_slider': s.value = value; break;
      case 'check': s.state = 'check'; break;
      case 'get_text': s.result_var = uniqueVar('text'); break;
      case 'get_attribute': s.attribute = d.href ? 'href' : d.src ? 'src' : 'value'; s.result_var = uniqueVar('attr'); break;
      case 'element_exists': s.result_var = uniqueVar('exists'); break;
      case 'extract_table': s.selector = (d.table && d.table.selector) || sel.selector; s.result_var = uniqueVar('table'); delete s.alts; break;
      case 'upload_file':
        Host.call('file.dialog', { mode: 'open', multi: true, title: 'Choose the file(s) to upload', filters: [['All files', '*.*']] }, 0).then(function (r) { s.files = r.paths || (r.path ? [r.path] : []); P.addSteps(s); }, function () { s.files = []; P.addSteps(s); });
        return;
      case 'drag_drop': toast('Now pick the place to drop it on.'); startPick('element', function (t) { s.target = t.selector; P.addSteps(s); }); return;
    }
    P.addSteps(s);
  }

  /* ------------------------------------------------------------ similar items card */
  function drawSimilar(sim) {
    clear(simBox);
    var fields = sim.fields.map(function (f) { return { on: true, name: f.name, selector: f.selector, attr: f.attr || 'text', sample: f.sample || [] }; });
    var varIn = h('input', { type: 'text', value: uniqueVar('items') }), nextIn = h('input.mono', { type: 'text', placeholder: 'css:a.next   (optional)' }), pages = h('input', { type: 'text', value: '1' }), scrollChk = h('input', { type: 'checkbox' });
    var tb = h('table.grid', { style: { width: '100%' } }, h('thead', h('tr', h('th', { text: '' }), h('th', { text: 'Column' }), h('th', { text: 'Read' }), h('th', { text: 'Examples' }))));
    var body = h('tbody');
    fields.forEach(function (f) {
      var c = h('input', { type: 'checkbox', checked: true }), n = h('input', { type: 'text', value: f.name }), a = h('select', ['text', 'href', 'src', 'value', 'html'].map(function (x) { return h('option', { value: x, text: x }); }));
      a.value = ['text', 'href', 'src', 'value', 'html'].indexOf(f.attr) >= 0 ? f.attr : 'text';
      c.onchange = function () { f.on = c.checked; }; n.oninput = function () { f.name = n.value; }; a.onchange = function () { f.attr = a.value; };
      body.appendChild(h('tr', h('td', c), h('td', { style: { minWidth: '90px' } }, n), h('td', a), h('td.small.muted', { title: f.sample.join('\n'), text: f.sample.slice(0, 2).join(' | ') })));
    });
    tb.appendChild(body);
    function build() {
      var fs = fields.filter(function (f) { return f.on && f.name; }).map(function (f) { var o = { name: f.name, selector: f.selector }; if (f.attr && f.attr !== 'text') o.attr = f.attr; return o; });
      if (!fs.length) { toast('Select at least one column.', 'err'); return null; }
      var s = P.newStep('extract_list'); s.item_selector = sim.item_selector; s.fields = fs; s.result_var = varIn.value.trim() || 'items';
      if (nextIn.value.trim()) { s.next_selector = nextIn.value.trim(); s.max_pages = Number(pages.value) || 5; }
      if (scrollChk.checked) s.scroll_load = true;
      s.name = 'Extract ' + s.result_var;
      return s;
    }
    simBox.appendChild(h('div.card', h('div.row', h('b', { text: '🧩 ' + sim.count + ' similar items found' }), h('span.grow'), h('button.btn.sm.flat', { text: 'Highlight', onclick: function () { Elem.test(sim.item_selector); } })),
      h('div.small.muted', { text: 'Item selector' }), h('div.mono.small.ell', { title: sim.item_selector, text: sim.item_selector }),
      h('div.small.muted', { text: 'Choose the data to collect from every item:' }), h('div.scroll', { style: { maxHeight: '220px' } }, tb),
      h('div.row', h('span.small.muted', { text: 'Save in variable' }), h('div.grow', varIn)),
      h('details', h('summary.small.muted', { text: 'Several pages / infinite scroll' }), h('div.col', { style: { paddingTop: '6px' } },
        h('div.small.muted', { text: '"Next page" button' }), h('div.row', h('div.grow', nextIn), h('button.btn.sm', { text: '🎯 Pick', onclick: function () { Elem.pickOnce(function (d) { nextIn.value = d.selector; if (pages.value === '1') pages.value = '5'; }); } })),
        h('div.row', h('span.small.muted', { text: 'Max pages' }), h('div', { style: { width: '70px' } }, pages)), h('label.chk.small', scrollChk, 'Load more by scrolling down (infinite scroll)'))),
      h('div.row.wrap', h('button.btn.primary.sm', { text: '＋ Add "Extract repeated items"', onclick: function () { var s = build(); if (s) P.addSteps(s); } }),
        h('button.btn.sm', { text: 'Preview data', onclick: function () { var s = build(); if (s) previewList(s); } }))));
  }
  function previewList(s) {
    pageExec('extract_list', { selector: s.item_selector, fields: s.fields }).then(function (r) {
      if (!r || !r.ok) { toast((r && r.error) || 'Preview failed', 'err'); return; }
      modal({ title: 'Preview: ' + r.count + ' item(s) on this page', width: '760px', body: GridView(r.rows, 30) });
    }, function (e) { toast(e.message, 'err'); });
  }

  /* ------------------------------------------------------------ page scan */
  var CAT_ICON = { 'Tables': '📊', 'Repeated items': '🧩', 'Headings': '🔠', 'Links': '🔗', 'Images': '🖼', 'Forms': '📝', 'Form fields': '⌨', 'Buttons': '🔘', 'Text blocks': '📄', 'Page info': 'ℹ' };
  Elem.scan = function () {
    clear(scanBox); scanBox.appendChild(h('div.muted.small.pad', { text: 'Scanning the page…' }));
    pageCall('categorize').then(function (r) {
      clear(scanBox);
      if (!r.ok) { scanBox.appendChild(h('div.muted.small.pad', { text: 'Could not scan: ' + (r.error || '') })); return; }
      var groups = r.groups || [], cats = [];
      groups.forEach(function (g) { if (cats.indexOf(g.category) < 0) cats.push(g.category); });
      scanBox.appendChild(h('div.small.muted.pad', { text: 'Scanned: ' + (r.title || r.url) }));
      if (!groups.length) scanBox.appendChild(h('div.muted.small.pad', { text: 'Nothing recognisable here (the page may still be loading).' }));
      cats.forEach(function (c) {
        var items = groups.filter(function (g) { return g.category === c; });
        scanBox.appendChild(h('details.cat', { open: c === 'Tables' || c === 'Repeated items' || items.length === 1 }, h('summary', h('span', { text: (CAT_ICON[c] || '•') + ' ' + c }), ' ', h('span.chip', { text: String(items.length > 1 ? items.length : (items[0].count || 1)) })),
          items.map(function (g) { return groupRow(g); })));
      });
    }, function (e) { clear(scanBox); scanBox.appendChild(h('div.muted.small.pad', { text: e.message })); });
  };
  function groupRow(g) {
    return h('div.grp', h('div.row', h('div.grow', h('div.ell', { title: g.label, text: g.label }), g.sample ? h('div.small.muted.ell', { title: g.sample, text: g.sample }) : null),
      h('div.col', { style: { gap: '3px' } },
        h('button.btn.sm.primary', { text: g.step === 'autofill_form' ? '＋ Auto-fill' : '＋ Extract', onclick: function () { addGroup(g); } }),
        h('button.btn.sm', { text: 'Preview', onclick: function () { previewGroup(g); } }))));
  }
  function addGroup(g) {
    var s;
    if (g.step === 'extract_table') { s = P.newStep('extract_table'); s.selector = g.selector; s.result_var = uniqueVar('table'); s.name = 'Extract ' + s.result_var; }
    else if (g.step === 'extract_list') { s = P.newStep('extract_list'); s.item_selector = g.selector; s.fields = (g.fields || []).map(function (f) { var o = { name: f.name, selector: f.selector }; if (f.attr) o.attr = f.attr; return o; }); s.result_var = uniqueVar('items'); s.name = 'Extract ' + s.result_var; }
    else if (g.step === 'autofill_form') { s = P.newStep('autofill_form'); s.root = g.selector; s.name = 'Fill form from row'; }
    else { s = P.newStep('extract_page'); s.kinds = g.kinds || []; s.result_var = uniqueVar((g.kinds && g.kinds[0]) || 'page'); s.name = 'Read ' + g.category.toLowerCase(); }
    P.addSteps(s);
  }
  function previewGroup(g) {
    var p;
    if (g.step === 'extract_table') p = pageExec('extract_table', { selector: g.selector }).then(function (r) { return r && r.ok ? r.rows : Promise.reject(new Error((r && r.error) || 'failed')); });
    else if (g.step === 'extract_list') p = pageExec('extract_list', { selector: g.selector, fields: g.fields }).then(function (r) { return r && r.ok ? r.rows : Promise.reject(new Error((r && r.error) || 'failed')); });
    else if (g.step === 'autofill_form') { Elem.test(g.selector); return; }
    else p = pageExec('extract_page', { kinds: g.kinds || [] }).then(function (r) { return r && r.ok ? r.data : Promise.reject(new Error((r && r.error) || 'failed')); });
    p.then(function (data) {
      var body = Array.isArray(data) ? GridView(data, 40) : h('div.col', Object.keys(data).map(function (k) { return h('div', h('b', { text: k }), Array.isArray(data[k]) && data[k].length && typeof data[k][0] === 'object' ? GridView(data[k], 30) : h('pre.mono.pre', { text: JSON.stringify(data[k], null, 1).slice(0, 1500) })); }));
      modal({ title: 'Preview: ' + g.label, width: '760px', body: body });
    }, function (e) { toast(e.message, 'err'); });
  }

  /* ------------------------------------------------------------ build the tab */
  Elem.init = function (page) {
    pickBtn = h('button.btn.primary', { text: '🎯 Pick element', title: 'Click anything on the page', onclick: function () { if (pickBtn.classList.contains('on')) Elem.stopPick(); else startPick('element'); } });
    simBtn = h('button.btn', { text: '🧩 Pick similar items', title: 'Click one product / row / card to find all like it', onclick: function () { if (simBtn.classList.contains('on')) Elem.stopPick(); else startPick('similar'); } });
    cardBox = h('div'); simBox = h('div'); scanBox = h('div');
    testIn = h('input.mono', { type: 'text', placeholder: 'Test a selector, e.g. css:.price  text:"Add to cart"  label:Email', spellcheck: 'false' });
    testOut = h('div.small.col', { style: { gap: '2px' } });
    testIn.addEventListener('keydown', function (e) { if (e.key === 'Enter') Elem.test(testIn.value, testOut); });
    page.appendChild(h('div.pad.col',
      h('div.row.wrap', pickBtn, simBtn, h('button.btn', { text: '📋 Scan page', onclick: Elem.scan })),
      h('div.small.muted', { id: 'addto', text: '' }),
      h('div.row', h('div.grow', testIn), h('button.btn.sm', { text: 'Test', onclick: function () { Elem.test(testIn.value, testOut); } })), testOut));
    page.appendChild(cardBox); page.appendChild(simBox);
    page.appendChild(h('div.pad', h('h4', { text: 'Everything on this page, by category' }), h('p.muted.small', { text: 'Tables, repeated items (products, results, cards), headings, links, images, forms, buttons and text — click "＋ Extract" to add a step.' })));
    page.appendChild(scanBox);
    drawPicked();
    P.on('rec:changed', function () { /* keep the hint current */ });
    setInterval(function () { var a = $('#addto'); if (a) a.textContent = 'Steps are added to: ' + (P.rec.on ? 'the recording' : 'your flow (after the selected step)'); }, 600);
  };
})();
