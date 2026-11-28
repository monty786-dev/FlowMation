/* Studio panel - Flow designer: step tree with nesting, drag & drop, palette, inspector, save / open. */
(function () {
  'use strict';
  var Flow = window.Flow = {};
  var tree, inspector, nameInput, issuesBox, issueBtn, issues = [], dragPath = null, dropMark = null;

  var LABELS = { then: 'Then', 'else': 'Else', body: 'Do', 'catch': 'If it fails' };

  /* ------------------------------------------------------------ tree */
  function stepRow(step, path) {
    var s = P.summary(step), def = P.defs[step.type], st = P.run.stepState[path];
    var iss = issues.filter(function (i) { return i.path === path; });
    var row = h('div.step' + (P.sel === path ? '.sel' : '') + (step.enabled === false ? '.off' : '') + (st ? '.' + st.st : '') + (P.run.path === path && P.run.state === 'paused' ? '.paused' : ''), { 'data-path': path, draggable: 'true',
      onclick: function (e) { if (e.target.closest('.act,.bp')) return; P.sel = path; Flow.draw(); },
      ondblclick: function () { P.sel = path; Flow.draw(); var f = $('input', inspector); if (f) f.focus(); },
      ondragstart: function (e) { dragPath = path; e.dataTransfer.setData('text/plain', path); e.dataTransfer.effectAllowed = 'move'; setTimeout(function () { row.classList.add('dragging'); }, 0); },
      ondragend: function () { dragPath = null; clearMark(); row.classList.remove('dragging'); },
      ondragover: function (e) {
        if (dragPath == null || dragPath === path || path.indexOf(dragPath + '.') === 0) return;
        e.preventDefault(); e.stopPropagation();
        var r = row.getBoundingClientRect(), before = e.clientY < r.top + r.height / 2;
        clearMark(); row.classList.add(before ? 'drop-before' : 'drop-after'); dropMark = row;
        row.setAttribute('data-before', before ? '1' : '');
      },
      ondrop: function (e) {
        if (dragPath == null) return; e.preventDefault(); e.stopPropagation();
        var sp = P.splitPath(path), idx = sp.idx + (row.getAttribute('data-before') ? 0 : 1), from = dragPath; dragPath = null; clearMark();
        P.move(from, sp.arrPath, idx);
      }
    },
    h('span.bp' + (P.run.breakpoints[path] ? '.on' : ''), { title: 'Breakpoint: pause here when running step by step', text: '●', onclick: function () { if (P.run.breakpoints[path]) delete P.run.breakpoints[path]; else P.run.breakpoints[path] = 1; Flow.draw(); } }),
    h('span.grip', { text: '⠿' }), h('span.ico', { text: s.icon }),
    h('div.txt.grow', h('b', { text: s.label }), s.detail ? h('span.det', { text: ' ' + s.detail }) : null,
      step.comment ? h('div.cmt', { text: step.comment }) : null,
      iss.length ? h('div.iss' + (iss.some(function (i) { return i.level === 'error'; }) ? '.e' : ''), { text: iss.map(function (i) { return i.message; }).join(' · ') }) : null),
    h('span.badge', { text: st ? (st.st === 'run' ? '…' : st.st === 'ok' ? '✔ ' + fmtMs(st.ms || 0) : '✖') : '', title: st && st.err ? st.err : '' }),
    step.on_error && step.on_error !== 'inherit' ? h('span.chip', { text: step.on_error }) : null,
    h('span.act',
      h('button.btn.sm.flat.icon', { title: 'Run only this step', text: '▷', onclick: function () { Run.runSteps([cloneJ(step)]); } }),
      h('button.btn.sm.flat.icon', { title: 'Enable / disable', text: step.enabled === false ? '☐' : '☑', onclick: function () { P.snap(); if (step.enabled === false) delete step.enabled; else step.enabled = false; P.changed(); } }),
      h('button.btn.sm.flat.icon', { title: 'Move up', text: '↑', onclick: function () { moveBy(path, -1); } }),
      h('button.btn.sm.flat.icon', { title: 'Move down', text: '↓', onclick: function () { moveBy(path, 1); } }),
      h('button.btn.sm.flat.icon', { title: 'Duplicate', text: '⎘', onclick: function () { P.dup(path); } }),
      h('button.btn.sm.flat.icon.danger', { title: 'Delete', text: '✕', onclick: function () { P.remove(path); } })));
    var out = [row];
    P.nestedKeys(def).forEach(function (k) {
      var ap = path + '.' + k;
      if (k === 'else' && !(step['else'] && step['else'].length) && P.sel !== path) { /* collapsed empty else */
        out.push(h('div.nest', h('div.nhead', h('span', { text: LABELS['else'] }), h('button.btn.sm.flat', { text: '＋ add', onclick: function () { Flow.add({ arrPath: ap, idx: 0 }); } })))); return;
      }
      out.push(nest(step[k] || [], ap, LABELS[k] || k));
    });
    return out;
  }
  function moveBy(path, d) {
    var sp = P.splitPath(path), arr = P.arrAt(sp.arrPath), ni = sp.idx + d;
    if (!arr || ni < 0 || ni >= arr.length) return;
    P.snap(); var t = arr[sp.idx]; arr[sp.idx] = arr[ni]; arr[ni] = t;
    P.sel = (sp.arrPath ? sp.arrPath + '.' : '') + ni; P.changed();
  }
  function clearMark() { if (dropMark) { dropMark.classList.remove('drop-before', 'drop-after'); dropMark = null; } $$('.nest.drop', tree).forEach(function (n) { n.classList.remove('drop'); }); }
  function nest(arr, arrPath, label) {
    var box = h('div.nest', {
      ondragover: function (e) { if (dragPath == null || arrPath.indexOf(dragPath + '.') === 0) return; if (e.target.closest('.step') && e.target.closest('.nest') === box) return; e.preventDefault(); e.stopPropagation(); clearMark(); box.classList.add('drop'); },
      ondrop: function (e) { if (dragPath == null || e.target.closest('.step')) return; e.preventDefault(); e.stopPropagation(); var from = dragPath; dragPath = null; clearMark(); P.move(from, arrPath, arr.length); }
    }, h('div.nhead', h('span', { text: label }), h('button.btn.sm.flat', { text: '＋ add step', onclick: function () { Flow.add({ arrPath: arrPath, idx: arr.length }); } })));
    arr.forEach(function (s, i) { box.appendChild(h('div.node', stepRow(s, arrPath + '.' + i))); });
    if (!arr.length) box.appendChild(h('div.empty.small.muted', { text: 'Empty. Drag a step here or press "add step".' }));
    return box;
  }

  Flow.draw = function () {
    if (!tree) return;
    var top = tree.scrollTop;
    clear(tree);
    var steps = P.flow.steps;
    steps.forEach(function (s, i) { tree.appendChild(h('div.node', stepRow(s, String(i)))); });
    if (!steps.length) tree.appendChild(h('div.welcome',
      h('div', { style: { fontSize: '40px' }, text: '🧩' }), h('h3', { text: 'This flow is empty' }),
      h('p.muted', { text: 'Add steps from the list, or press Record in the toolbar and do the task once in the browser.' }),
      h('div.row', { style: { justifyContent: 'center' } }, h('button.btn.primary', { text: '＋ Add a step', onclick: function () { Flow.add(); } }), h('button.btn', { text: '⏺ Record a task', onclick: function () { Rec.toggle(); } }))));
    else tree.appendChild(h('div', { style: { padding: '8px' } }, h('button.btn.sm', { text: '＋ Add step', onclick: function () { Flow.add(); } })));
    tree.scrollTop = top;
    drawInspector();
    nameInput.value = P.flow.name || '';
    $('#flowtitle').textContent = (P.flowName || '(not saved)') + (P.dirty ? ' •' : '');
    $('#undo').disabled = !P.undo.length; $('#redo').disabled = !P.redo.length;
    $('#bpcount').textContent = Object.keys(P.run.breakpoints).length ? Object.keys(P.run.breakpoints).length + ' breakpoint(s)' : '';
  };
  function drawInspector() {
    clear(inspector);
    var step = P.sel ? P.stepAt(P.sel) : null;
    inspector.classList.toggle('hide', !step);
    if (!step) return;
    var s = P.summary(step);
    inspector.appendChild(h('div.ihead', h('span.ico', { text: s.icon }), h('b.grow', { text: (P.defs[step.type] || {}).label || step.type }), h('button.btn.sm.flat', { text: '✕', title: 'Close', onclick: function () { P.sel = null; Flow.draw(); } })));
    inspector.appendChild(h('div.ibody', Form.render(step, { key: P.sel })));
  }

  /* ------------------------------------------------------------ palette */
  Flow.add = function (at, type) {
    if (type) { place(P.newStep(type), at); return; }
    Flow.palette(function (t) { place(P.newStep(t), at); });
  };
  Flow.palette = function (cb) {
    var q = h('input', { type: 'search', placeholder: 'Search steps: click, upload, excel, extract, wait …' }), list = h('div.palette'), closeFn = null;
    function draw() {
      clear(list);
      var f = q.value.trim().toLowerCase(), any = false;
      P.cats.forEach(function (c) {
        var items = P.catalog.steps.filter(function (d) { return d.cat === c && (!f || (d.label + ' ' + d.desc + ' ' + d.type).toLowerCase().indexOf(f) >= 0); });
        if (!items.length) return; any = true;
        list.appendChild(h('div.pcat', { text: c }));
        items.forEach(function (d) {
          list.appendChild(h('div.pitem', { onclick: function () { if (closeFn) closeFn(d.type); } }, h('span.ico', { text: d.icon }), h('div', h('b', { text: d.label }), h('div.small.muted', { text: d.desc }))));
        });
      });
      if (!any) list.appendChild(h('div.muted', { text: 'No step matches.' }));
    }
    modal({ title: 'Add a step', width: '600px', setup: function (c) { closeFn = c; }, body: [q, h('div', { style: { height: '8px' } }), list], buttons: [{ label: 'Cancel', value: null }] }).then(function (t) { if (t) cb(t); });
    q.oninput = draw; draw();
    q.addEventListener('keydown', function (e) { if (e.key === 'Enter') { var f = $('.pitem', list); if (f) f.click(); } });
  };
  function place(step, at) {
    if (at && at.arrPath != null) P.insert(at.arrPath, at.idx, step);
    else { var ip = P.insertionPoint(); P.insert(ip.arrPath, ip.idx, step); }
  }

  /* ------------------------------------------------------------ save / open / settings */
  Flow.confirmDiscard = function () { return P.dirty && P.flow.steps.length ? confirmBox('This flow has unsaved changes. Discard them?', 'Discard', true) : Promise.resolve(true); };
  Flow.newFlow = function () { Flow.confirmDiscard().then(function (ok) { if (ok) { P.setFlow(P.newFlow('New flow'), null); Panel.show('flow'); } }); };
  Flow.save = function (asNew) {
    var name = P.flowName;
    function go(n) {
      if (!n || !n.trim()) return Promise.resolve();
      P.flow.name = P.flow.name && P.flow.name !== 'New flow' ? P.flow.name : n.trim();
      return Host.call('lib.save', { name: n.trim(), flow: P.flow, keep_versions: (P.settings.keepVersions != null ? P.settings.keepVersions : 10) }).then(function (r) {
        P.flowName = r.name || n.trim(); P.dirty = false; Flow.draw(); toast('Saved "' + P.flowName + '"', 'ok'); P.emit('lib:changed');
      }, function (e) { toast('Could not save: ' + e.message, 'err'); });
    }
    if (name && !asNew) return go(name);
    return promptBox(asNew ? 'Save as' : 'Save flow', P.flow.name || '', 'Flows are saved in your FlowBrowser library and can be backed up from the Library tab.').then(go);
  };
  Flow.open = function (name) {
    return Flow.confirmDiscard().then(function (ok) {
      if (!ok) return;
      return Host.call('lib.load', { name: name }).then(function (r) { P.setFlow(r.flow, name); Panel.show('flow'); }, function (e) { toast('Could not open: ' + e.message, 'err'); });
    });
  };
  Flow.settingsDialog = function () {
    var f = P.flow, st = f.settings = f.settings || {};
    function sel(opts, v) { var s = h('select', opts.map(function (o) { return h('option', { value: o[0], text: o[1] }); })); s.value = v; return s; }
    var name = h('input', { type: 'text', value: f.name || '' }), desc = h('textarea', { rows: 2 }), to = h('input', { type: 'text', value: st.timeout_ms != null ? st.timeout_ms : 15000 }),
      sd = h('input', { type: 'text', value: st.step_delay_ms || 0 }), oe = sel([['stop', 'Stop the flow'], ['continue', 'Skip the step and continue']], st.on_error || 'stop'),
      cp = sel([['pause', 'Pause so I can solve it'], ['stop', 'Stop the flow'], ['off', 'Do not check']], st.captcha || 'pause'),
      dl = sel([['accept', 'Accept (OK / Yes)'], ['dismiss', 'Dismiss (Cancel / No)'], ['manual', 'Let me answer']], st.dialogs || 'accept'), ua = h('input', { type: 'text', value: st.user_agent || '', placeholder: 'default' });
    desc.value = f.description || '';
    function fld(l, c, hint) { return h('label.col', { style: { gap: '3px' } }, h('span.small.muted', { text: l }), c, hint ? h('span.small.muted', { text: hint }) : null); }
    modal({ title: 'Flow settings', body: h('div.col', fld('Flow name', name), fld('Description', desc), fld('Wait up to (ms) for each element / page', to, 'Default 15000. Applies to every step unless the step has its own timeout.'),
      fld('Pause between steps (ms)', sd, 'Slow down for websites that react slowly.'), fld('When a step fails', oe), fld('When a CAPTCHA appears', cp, 'FlowBrowser never solves CAPTCHAs; it waits for you.'), fld('JavaScript alert / confirm dialogs', dl), fld('User agent override', ua)),
      buttons: [{ label: 'Cancel', value: false }, { label: 'OK', primary: true, value: true }] }).then(function (ok) {
      if (!ok) return;
      P.snap();
      f.name = name.value || 'New flow'; f.description = desc.value;
      st.timeout_ms = Number(to.value) || 15000; st.step_delay_ms = Number(sd.value) || 0; st.on_error = oe.value; st.captcha = cp.value; st.dialogs = dl.value;
      if (ua.value.trim()) st.user_agent = ua.value.trim(); else delete st.user_agent;
      P.changed();
    });
  };

  /* ------------------------------------------------------------ validation */
  var validate = debounce(function () {
    if (!P.flow) return;
    Host.call('validate', { flow: P.flow }, 8000).then(function (r) {
      issues = Array.isArray(r.issues) ? r.issues : [];
      var e = issues.filter(function (i) { return i.level === 'error'; }).length, w = issues.length - e;
      issueBtn.textContent = e || w ? (e ? '✖ ' + e : '') + (e && w ? '  ' : '') + (w ? '⚠ ' + w : '') : '✔ OK';
      issueBtn.className = 'btn sm flat' + (e ? ' danger' : '');
      Flow.draw();
    }, function () { /* validation is advisory */ });
  }, 700);
  function showIssues() {
    if (!issues.length) { toast('No problems found.', 'ok'); return; }
    var closeFn;
    modal({ title: 'Problems in this flow', setup: function (c) { closeFn = c; }, body: h('div.col', issues.map(function (i) {
      return h('div.row', { style: { cursor: 'pointer' }, onclick: function () { P.sel = i.path; Flow.draw(); closeFn(true); } },
        h('span.chip.' + (i.level === 'error' ? 'err' : 'warn'), { text: i.level }), h('span.muted.small', { text: 'step ' + (i.path === '' ? '-' : i.path) }), h('span.grow', { text: i.message }));
    })), buttons: [{ label: 'Close', primary: true, value: true }] });
  }

  /* ------------------------------------------------------------ build the tab */
  Flow.init = function (page) {
    nameInput = h('input.flowname', { type: 'text', placeholder: 'Flow name' });
    nameInput.addEventListener('input', function () { P.snap('flowname'); P.flow.name = nameInput.value; P.dirty = true; $('#flowtitle').textContent = (P.flowName || '(not saved)') + ' •'; });
    issueBtn = h('button.btn.sm.flat', { title: 'Show problems', text: '✔ OK', onclick: showIssues });
    tree = h('div#tree.tree'); inspector = h('div#inspector.inspector.hide');
    page.appendChild(h('div.toolbar',
      h('button.btn.sm', { text: '🆕 New', onclick: Flow.newFlow }), h('button.btn.sm', { text: '📂 Open', onclick: function () { Panel.show('library'); } }),
      h('button.btn.sm.primary', { text: '💾 Save', onclick: function () { Flow.save(false); } }), h('button.btn.sm', { text: 'Save as…', onclick: function () { Flow.save(true); } }),
      h('span.grow'), h('button.btn.sm.flat.icon', { id: 'undo', title: 'Undo (Ctrl+Z)', text: '↶', onclick: P.doUndo }), h('button.btn.sm.flat.icon', { id: 'redo', title: 'Redo (Ctrl+Y)', text: '↷', onclick: P.doRedo }),
      h('button.btn.sm', { text: '＋ Add step', onclick: function () { Flow.add(); } })));
    page.appendChild(h('div.flowhead', nameInput, h('span.muted.small', { id: 'flowtitle' }), h('span.grow'), h('span.small.muted', { id: 'bpcount' }), issueBtn, h('button.btn.sm.flat.icon', { title: 'Flow settings (timeout, errors, CAPTCHA…)', text: '⚙', onclick: Flow.settingsDialog })));
    page.appendChild(tree); page.appendChild(inspector);
    P.on('flow:changed', function (info) { validate(); if (info && info.noInspector) drawTreeOnly(); else Flow.draw(); });
    P.on('flow:loaded', function () { issues = []; validate(); });
    P.on('run:step', function (e) { var row = $('.step[data-path="' + e.path + '"]', tree); if (row) { row.classList.remove('run', 'ok', 'err'); if (P.run.stepState[e.path]) row.classList.add(P.run.stepState[e.path].st); var b = $('.badge', row); var st = P.run.stepState[e.path]; if (b && st) b.textContent = st.st === 'run' ? '…' : st.st === 'ok' ? '✔ ' + fmtMs(st.ms || 0) : '✖'; if (b && st && st.err) b.title = st.err; if (st && st.st === 'run' && row.scrollIntoView) row.scrollIntoView({ block: 'nearest' }); } });
    P.on('run:state', function () { $$('.step.paused', tree).forEach(function (r) { r.classList.remove('paused'); }); var row = P.run.state === 'paused' && $('.step[data-path="' + P.run.path + '"]', tree); if (row) row.classList.add('paused'); });
    document.addEventListener('keydown', function (e) {
      if (!page.classList.contains('on') || /^(INPUT|TEXTAREA|SELECT)$/.test((e.target || {}).tagName)) return;
      var c = e.ctrlKey || e.metaKey, k = e.key.toLowerCase();
      if (c && k === 'z' && !e.shiftKey) { e.preventDefault(); P.doUndo(); } else if (c && (k === 'y' || (k === 'z' && e.shiftKey))) { e.preventDefault(); P.doRedo(); }
      else if (c && k === 's') { e.preventDefault(); Flow.save(false); }
      else if (k === 'delete' && P.sel) { P.remove(P.sel); }
      else if (c && k === 'c' && P.sel) { P.clip = cloneJ(P.stepAt(P.sel)); toast('Step copied'); }
      else if (c && k === 'v' && P.clip) { P.addSteps(cloneJ(P.clip), 'flow'); }
    });
  };
  function drawTreeOnly() {
    var top = tree.scrollTop; clear(tree);
    P.flow.steps.forEach(function (s, i) { tree.appendChild(h('div.node', stepRow(s, String(i)))); });
    tree.appendChild(h('div', { style: { padding: '8px' } }, h('button.btn.sm', { text: '＋ Add step', onclick: function () { Flow.add(); } })));
    tree.scrollTop = top;
    $('#undo').disabled = !P.undo.length; $('#redo').disabled = !P.redo.length;
    $('#flowtitle').textContent = (P.flowName || '(not saved)') + (P.dirty ? ' •' : '');
  }
})();
