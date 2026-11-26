/* Studio panel - catalog-driven parameter forms. Every step type is edited with the same renderer. */
(function () {
  'use strict';
  var Form = window.Form = {};
  var KINDS = ['headings', 'links', 'images', 'tables', 'lists', 'buttons', 'inputs', 'forms', 'text', 'meta'];
  var ATTRS = ['text', 'href', 'src', 'value', 'html', 'outerhtml', 'checked', 'title', 'alt', 'id', 'class', 'name', 'data-id'];
  var uid = 0;

  /* small helper: a variable insert button next to text inputs */
  function varButton(input, onset) {
    return h('button.btn.sm.flat', { type: 'button', title: 'Insert a variable', text: '{ }', onclick: function (e) {
      e.preventDefault();
      var vars = P.knownVars().concat(['page.url', 'page.title', 'error']);
      var sel = h('select', { size: Math.min(10, vars.length + 1) }, vars.map(function (v) { return h('option', { value: v, text: v }); }));
      modal({ title: 'Insert variable', body: [h('p.muted.small', { text: 'The value is replaced each time the flow runs. For a table row use e.g. row.ColumnName.' }), sel],
        buttons: [{ label: 'Cancel', value: null }, { label: 'Insert', primary: true, get: function () { return sel.value; } }] }).then(function (v) {
        if (!v) return;
        var pos = input.selectionStart == null ? input.value.length : input.selectionStart;
        input.value = input.value.slice(0, pos) + '{{' + v + '}}' + input.value.slice(pos);
        onset(input.value); input.focus();
      });
    } });
  }

  function textControl(step, p, onset, multi, mono) {
    var cur = step[p.name] == null ? '' : step[p.name];
    var inp = multi ? h('textarea', { rows: multi === 'code' ? 7 : 3, spellcheck: 'false' }) : h('input', { type: 'text', spellcheck: 'false', autocomplete: 'off' });
    inp.value = typeof cur === 'object' ? JSON.stringify(cur) : String(cur);
    if (p.default !== undefined && p.kind !== 'bool') inp.placeholder = String(p.default);
    if (mono) inp.classList.add('mono');
    inp.addEventListener('input', function () { onset(inp.value); });
    return multi ? inp : h('div.row', h('div.grow', inp), varButton(inp, onset));
  }

  function fileControl(step, p, onset, save) {
    var inp = h('input', { type: 'text', value: step[p.name] || '', placeholder: save ? 'C:\\Data\\result.xlsx' : 'C:\\Data\\input.xlsx', spellcheck: 'false' });
    inp.addEventListener('input', function () { onset(inp.value); });
    return h('div.row', h('div.grow', inp), varButton(inp, onset), h('button.btn.sm', { type: 'button', text: 'Browse…', onclick: function () {
      Host.call('file.dialog', { mode: save ? 'save' : 'open', title: p.label, filters: Form.filtersFor(step.type, p.name), name: inp.value ? basename(inp.value) : '' }, 0).then(function (r) {
        if (r.path) { inp.value = r.path; onset(r.path); }
      }, function (e) { toast(e.message, 'err'); });
    } }));
  }
  Form.filtersFor = function (type, name) {
    if (/^excel_/.test(type)) return [['Excel workbook', '*.xlsx'], ['All files', '*.*']];
    if (/^csv_/.test(type)) return [['CSV', '*.csv;*.txt'], ['All files', '*.*']];
    if (type === 'json_write') return [['JSON', '*.json'], ['All files', '*.*']];
    if (type === 'screenshot') return [['PNG image', '*.png'], ['All files', '*.*']];
    if (type === 'upload_file') return [['All files', '*.*']];
    return [['All files', '*.*']];
  };

  function filesControl(step, p, onset) {
    var wrap = h('div.col');
    function list() { var v = step[p.name]; return Array.isArray(v) ? v : (v ? [String(v)] : []); }
    function draw() {
      clear(wrap);
      var items = list();
      items.forEach(function (f, i) {
        var inp = h('input', { type: 'text', value: f, spellcheck: 'false', placeholder: 'C:\\Files\\document.pdf  or  {{row.File}}' });
        inp.addEventListener('input', function () { var a = list().slice(); a[i] = inp.value; step[p.name] = a; onset(a); });
        wrap.appendChild(h('div.row', h('div.grow', inp), varButton(inp, function (v) { var a = list().slice(); a[i] = v; step[p.name] = a; onset(a); }),
          h('button.btn.sm.flat.danger', { type: 'button', text: '✕', onclick: function () { var a = list().slice(); a.splice(i, 1); step[p.name] = a; onset(a); draw(); } })));
      });
      wrap.appendChild(h('div.row', h('button.btn.sm', { type: 'button', text: '📎 Choose file(s)…', onclick: function () {
        Host.call('file.dialog', { mode: 'open', multi: true, title: 'Choose the file(s) to upload', filters: [['All files', '*.*']] }, 0).then(function (r) {
          var paths = r.paths || (r.path ? [r.path] : []); if (!paths.length) return;
          var a = list().concat(paths); step[p.name] = a; onset(a); draw();
        }, function (e) { toast(e.message, 'err'); });
      } }), h('button.btn.sm', { type: 'button', text: '＋ Add path', onclick: function () { var a = list().concat(['']); step[p.name] = a; onset(a); draw(); } })));
    }
    draw(); return wrap;
  }

  function kvControl(step, p, onset) {
    var wrap = h('div.col');
    function pairs() { var o = step[p.name] && typeof step[p.name] === 'object' ? step[p.name] : {}; return Object.keys(o).map(function (k) { return [k, o[k]]; }); }
    function commit(rows) { var o = {}; rows.forEach(function (r) { if (r[0] !== '') o[r[0]] = r[1]; }); step[p.name] = o; onset(o); }
    function draw() {
      clear(wrap);
      var rows = pairs();
      rows.forEach(function (r, i) {
        var k = h('input', { type: 'text', value: r[0], placeholder: 'name' }), v = h('input', { type: 'text', value: r[1], placeholder: 'value, e.g. {{row.Email}}' });
        function upd() { rows[i] = [k.value, v.value]; commit(rows); }
        k.addEventListener('input', upd); v.addEventListener('input', upd);
        wrap.appendChild(h('div.row', h('div', { style: { width: '38%' } }, k), h('div.grow', v), h('button.btn.sm.flat.danger', { type: 'button', text: '✕', onclick: function () { rows.splice(i, 1); commit(rows); draw(); } })));
      });
      wrap.appendChild(h('div', h('button.btn.sm', { type: 'button', text: '＋ Add', onclick: function () { rows.push(['', '']); draw(); } })));
    }
    draw(); return wrap;
  }

  function listControl(step, p, onset) {
    if (step.type === 'extract_page' && p.name === 'kinds') {
      var cur = Array.isArray(step.kinds) ? step.kinds : [];
      return h('div.row.wrap', KINDS.map(function (k) {
        var c = h('input', { type: 'checkbox', checked: cur.indexOf(k) >= 0 });
        c.addEventListener('change', function () { var a = (Array.isArray(step.kinds) ? step.kinds : []).filter(function (x) { return x !== k; }); if (c.checked) a.push(k); step.kinds = a; onset(a); });
        return h('label.chk', { style: { marginRight: '8px' } }, c, k);
      }));
    }
    var wrap = h('div.col');
    function items() { return Array.isArray(step[p.name]) ? step[p.name].slice() : []; }
    function draw() {
      clear(wrap);
      items().forEach(function (val, i) {
        var inp = h('input', { type: 'text', value: val, placeholder: 'value, e.g. {{row.Name}}' });
        inp.addEventListener('input', function () { var a = items(); a[i] = inp.value; step[p.name] = a; onset(a); });
        wrap.appendChild(h('div.row', h('span.muted.small', { style: { width: '22px' }, text: String(i + 1) }), h('div.grow', inp), varButton(inp, function (v) { var a = items(); a[i] = v; step[p.name] = a; onset(a); }),
          h('button.btn.sm.flat.danger', { type: 'button', text: '✕', onclick: function () { var a = items(); a.splice(i, 1); step[p.name] = a; onset(a); draw(); } })));
      });
      wrap.appendChild(h('div', h('button.btn.sm', { type: 'button', text: '＋ Add', onclick: function () { var a = items(); a.push(''); step[p.name] = a; onset(a); draw(); } })));
    }
    draw(); return wrap;
  }

  function fieldsControl(step, p, onset) {
    var wrap = h('div.col'), dl = 'attrs' + (++uid);
    function rows() { return Array.isArray(step[p.name]) ? step[p.name] : []; }
    function commit(a) { step[p.name] = a; onset(a); }
    function draw() {
      clear(wrap);
      var rs = rows();
      wrap.appendChild(h('datalist', { id: dl }, ATTRS.map(function (a) { return h('option', { value: a }); })));
      if (rs.length) wrap.appendChild(h('div.row.muted.small', h('div', { style: { width: '26%' }, text: 'Column name' }), h('div.grow', { text: 'Selector inside the item (empty = the item)' }), h('div', { style: { width: '19%' }, text: 'Read' }), h('div', { style: { width: '24px' } })));
      rs.forEach(function (f, i) {
        var n = h('input', { type: 'text', value: f.name || '', placeholder: 'title' }), s = h('input.mono', { type: 'text', value: f.selector || '', placeholder: 'css:.title' }),
          a = h('input', { type: 'text', value: f.attr || 'text', list: dl });
        function upd() { var o = { name: n.value, selector: s.value }; if (a.value && a.value !== 'text') o.attr = a.value; var arr = rows().slice(); arr[i] = o; commit(arr); }
        [n, s, a].forEach(function (x) { x.addEventListener('input', upd); });
        wrap.appendChild(h('div.row', h('div', { style: { width: '26%' } }, n), h('div.grow', s), h('div', { style: { width: '19%' } }, a),
          h('button.btn.sm.flat.danger', { type: 'button', text: '✕', onclick: function () { var arr = rows().slice(); arr.splice(i, 1); commit(arr); draw(); } })));
      });
      wrap.appendChild(h('div.row', h('button.btn.sm', { type: 'button', text: '＋ Add field', onclick: function () { var arr = rows().slice(); arr.push({ name: 'field' + (arr.length + 1), selector: '' }); commit(arr); draw(); } }),
        h('span.muted.small', { text: 'Tip: use the Elements tab → "Pick similar items" to fill this automatically.' })));
    }
    draw(); return wrap;
  }

  function selectorControl(step, p, onset, rerender) {
    var inp = h('input.mono', { type: 'text', value: step[p.name] || '', spellcheck: 'false', placeholder: 'css:#id   text:"Sign in"   label:Email   xpath://a …' });
    inp.addEventListener('input', function () { onset(inp.value); });
    var wrap = h('div.col');
    var main = h('div.row', h('div.grow', inp), varButton(inp, onset),
      h('button.btn.sm', { type: 'button', title: 'Click an element on the page to fill this in', text: '🎯 Pick', onclick: function () {
        Elem.pickOnce(function (d) {
          inp.value = d.selector; onset(d.selector);
          if (p.name === 'selector' && d.alts && d.alts.length) { step.alts = d.alts; P.changed({ noInspector: true }); rerender(); }
        });
      } }),
      h('button.btn.sm', { type: 'button', title: 'Highlight matching elements on the page', text: 'Test', onclick: function () { Elem.test(inp.value); } }));
    wrap.appendChild(main);
    if (p.name === 'selector' && Array.isArray(step.alts) && step.alts.length) {
      wrap.appendChild(h('div.muted.small', { text: 'Fallback selectors (used automatically if the one above stops working):' }));
      step.alts.forEach(function (a, i) {
        wrap.appendChild(h('div.row', h('span.mono.grow.ell.small', { text: a }), h('button.btn.sm.flat.danger', { type: 'button', text: '✕', onclick: function () { step.alts.splice(i, 1); if (!step.alts.length) delete step.alts; P.changed({ noInspector: true }); rerender(); } })));
      });
    }
    return wrap;
  }

  /* one labelled control for a param */
  function control(step, p, onset, rerender) {
    switch (p.kind) {
      case 'bool': {
        var cur = step[p.name]; if (cur == null) cur = p.default === true;
        var c = h('input', { type: 'checkbox', checked: !!cur });
        c.addEventListener('change', function () { onset(c.checked); });
        return h('label.chk', c, h('span', { text: p.label }));
      }
      case 'enum': {
        var sel = h('select', (p.options || []).map(function (o) { return h('option', { value: o, text: o }); }));
        sel.value = step[p.name] != null ? step[p.name] : (p.default != null ? p.default : (p.options || [])[0]);
        sel.addEventListener('change', function () { onset(sel.value); });
        return sel;
      }
      case 'selector': return selectorControl(step, p, onset, rerender);
      case 'file_open': return fileControl(step, p, onset, false);
      case 'file_save': return fileControl(step, p, onset, true);
      case 'files': return filesControl(step, p, onset);
      case 'kv': return kvControl(step, p, onset);
      case 'list': return listControl(step, p, onset);
      case 'fields': return fieldsControl(step, p, onset);
      case 'longtext': return textControl(step, p, onset, true);
      case 'code': return textControl(step, p, onset, 'code', true);
      case 'var': {
        var dl = 'vars' + (++uid), inp = textControl(step, p, onset, false);
        var input = inp.querySelector('input'); input.setAttribute('list', dl);
        inp.appendChild(h('datalist', { id: dl }, P.knownVars().map(function (v) { return h('option', { value: v }); })));
        return inp;
      }
      case 'condition': {
        var ci = textControl(step, p, onset, false);
        return h('div.col', ci, h('div.muted.small', { text: 'Examples: {{count}} > 0 · {{row.Status}} == "Done" · {{page.text}} contains "Welcome" · {{x}} is empty · and / or' }));
      }
      case 'number': case 'url': case 'text': default: return textControl(step, p, onset, false);
    }
  }

  /* Form.render(step, path, {onChange}) -> element. Mutates `step` in place (a reference into P.flow). */
  Form.render = function (step, opts) {
    opts = opts || {};
    var def = P.defs[step.type], root = h('div.col.form'), key = 'form:' + (opts.key || '');
    function rerender() { var p = root.parentNode; if (!p) return; var n = Form.render(step, opts); p.replaceChild(n, root); root = n; }
    function setter(name, kind) {
      return function (v) {
        if (opts.snap) opts.snap(key + name);
        else P.snap(key + name);
        if (v === '' || v == null || (Array.isArray(v) && !v.length && kind !== 'steps')) delete step[name]; else step[name] = v;
        if (opts.onChange) opts.onChange(step, name); else P.changed({ noInspector: true });
      };
    }
    if (!def) { root.appendChild(h('div.muted', { text: 'Unknown step type "' + step.type + '". It will be kept as is.' })); return root; }
    root.appendChild(h('div.muted.small', { text: def.desc }));
    var nm = h('input', { type: 'text', value: step.name || '', placeholder: def.label });
    nm.addEventListener('input', function () { setter('name')(nm.value); });
    root.appendChild(h('label.col', h('span.small.muted', { text: 'Step name (shown in the list and log)' }), nm));
    def.params.forEach(function (p) {
      if (p.kind === 'steps') return;
      var onset = setter(p.name, p.kind);
      var c = control(step, p, function (v) {
        if (p.kind === 'number' && v !== '' && /^-?\d+(\.\d+)?$/.test(String(v).trim())) v = Number(v);
        onset(v);
      }, rerender);
      if (p.kind === 'bool') { root.appendChild(c); return; }
      root.appendChild(h('div.col', { style: { gap: '3px' } }, h('span.small.muted', p.label, p.required ? h('b', { style: { color: 'var(--orange)' }, text: ' *' }) : ''), c));
    });
    /* common options */
    var more = h('details.more', h('summary.small.muted', { text: 'More options (errors, retries, timeout, notes)' }));
    var inner = h('div.col', { style: { paddingTop: '6px' } });
    (P.catalog.common || []).forEach(function (p) {
      if (p.name === 'name') return;
      var c = control(step, p, function (v) {
        if (p.kind === 'number' && v !== '' && /^-?\d+(\.\d+)?$/.test(String(v).trim())) v = Number(v);
        if (p.name === 'enabled') { P.snap(); if (v) delete step.enabled; else step.enabled = false; P.changed({ noInspector: true }); return; }
        setter(p.name)(v);
      }, rerender);
      inner.appendChild(p.kind === 'bool' ? c : h('div.col', { style: { gap: '3px' } }, h('span.small.muted', { text: p.label }), c));
    });
    more.appendChild(inner); root.appendChild(more);
    if (['on_error', 'retries', 'timeout_ms', 'comment'].some(function (k) { return step[k] != null; }) || step.enabled === false) more.open = true;
    return root;
  };
})();
