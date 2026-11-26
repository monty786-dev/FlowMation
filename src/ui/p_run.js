/* Studio panel - running flows: control bar, step highlighting, log, variables. */
(function () {
  'use strict';
  var Run = window.Run = {};
  var logBox, filter = 'all', sumEl, varsBox, initBox, bar = {};
  var MAXLOG = 2000;

  /* ------------------------------------------------------------ shared: table preview */
  window.GridView = function (rows, limit) {
    limit = limit || 50;
    if (!Array.isArray(rows) || !rows.length) return h('div.muted.small', { text: Array.isArray(rows) ? '(empty)' : '' });
    var objs = rows[0] && typeof rows[0] === 'object' && !Array.isArray(rows[0]);
    var cols = [];
    if (objs) rows.slice(0, 500).forEach(function (r) { if (r && typeof r === 'object') Object.keys(r).forEach(function (k) { if (k[0] !== '_' && cols.indexOf(k) < 0) cols.push(k); }); });
    else { var n = 0; rows.slice(0, 200).forEach(function (r) { n = Math.max(n, Array.isArray(r) ? r.length : 1); }); for (var i = 0; i < n; i++) cols.push(String(i + 1)); }
    var t = h('table.grid', h('thead', h('tr', h('th.rn', { text: '#' }), cols.map(function (c) { return h('th', { text: c }); }))));
    var tb = h('tbody');
    rows.slice(0, limit).forEach(function (r, ri) {
      tb.appendChild(h('tr', h('td.rn', { text: String(ri + 1) }), cols.map(function (c, ci) {
        var v = objs ? (r ? r[c] : '') : (Array.isArray(r) ? r[ci] : (ci === 0 ? r : ''));
        return h('td', { title: typeof v === 'object' ? JSON.stringify(v) : String(v == null ? '' : v), text: v == null ? '' : typeof v === 'object' ? JSON.stringify(v) : String(v) });
      })));
    });
    t.appendChild(tb);
    return h('div', h('div.scroll', { style: { maxHeight: '260px', border: '1px solid var(--border)', borderRadius: '4px' } }, t),
      rows.length > limit ? h('div.muted.small', { text: 'Showing ' + limit + ' of ' + rows.length + ' rows' }) : h('div.muted.small', { text: rows.length + ' row(s)' }));
  };
  function isTable(v) { return Array.isArray(v) && v.length && v[0] && typeof v[0] === 'object'; }

  /* ------------------------------------------------------------ starting a run */
  function runnableFlow(steps) {
    var f = cloneJ(P.flow);
    if (steps) f.steps = steps;
    var d = P.settings.automation || {};
    f.settings = Object.assign({}, f.settings);
    if (d.step_delay_ms && !f.settings.step_delay_ms) f.settings.step_delay_ms = d.step_delay_ms;
    return f;
  }
  function begin(flow, mode) {
    if (P.run.state !== 'idle') { toast('A flow is already running.', 'err'); return; }
    P.run.log = []; P.run.stepState = {}; P.run.vars = null; P.run.path = null; P.run.started = Date.now(); P.run.ended = 0;
    P.emit('run:reset'); Flow.draw(); drawLog();
    var bps = Object.keys(P.run.breakpoints);
    Host.send('run', { flow: flow, mode: mode, breakpoints: bps, vars: {} });
    setState('running');
  }
  Run.start = function (mode) {
    if (!P.flow.steps.length) { toast('Add some steps first.', 'err'); return; }
    Host.call('validate', { flow: P.flow }, 8000).then(function (r) {
      var errs = (r.issues || []).filter(function (i) { return i.level === 'error'; });
      if (errs.length) { toast('Fix this first: ' + errs[0].message + (errs[0].path !== '' ? ' (step ' + errs[0].path + ')' : ''), 'err'); P.sel = errs[0].path; Flow.draw(); Panel.show('flow'); return; }
      begin(runnableFlow(), mode || 'run');
    }, function () { begin(runnableFlow(), mode || 'run'); });
  };
  Run.runSteps = function (steps) {
    var f = runnableFlow(steps);
    f.name = 'Single step';
    if (P.run.vars) Object.keys(P.run.vars).forEach(function (k) { if (!/^(page|env)$/.test(k) && f.variables[k] === undefined) f.variables[k] = P.run.vars[k]; });
    begin(f, 'run');
  };
  Run.stop = function () { if (P.run.state === 'idle') return; Host.send('run.stop'); setState('stopping'); };
  Run.pause = function () { if (P.run.state === 'running') Host.send('run.pause'); };
  Run.resume = function (action) { if (P.run.state === 'paused') { Host.send('run.resume', { action: action }); setState('running'); } };

  function setState(st, path) {
    P.run.state = st; if (path !== undefined) P.run.path = path;
    var idle = st === 'idle', paused = st === 'paused';
    bar.run.classList.toggle('hide', !idle); bar.step.classList.toggle('hide', !idle);
    bar.pause.classList.toggle('hide', st !== 'running'); bar.cont.classList.toggle('hide', !paused); bar.next.classList.toggle('hide', !paused);
    bar.stop.classList.toggle('hide', idle);
    bar.status.textContent = ({ idle: P.run.ended ? 'Finished' : 'Ready', running: 'Running…', paused: 'Paused at step ' + (P.run.path || ''), stopping: 'Stopping…' })[st] || st;
    bar.status.className = 'status ' + st;
    P.emit('run:state', st);
    Host.send('relay', { to: 'shell', msg: { ev: 'automation', recording: P.rec.on, state: st, step: P.run.curName || '' } });
  }
  Run.notify = function () { Host.send('relay', { to: 'shell', msg: { ev: 'automation', recording: P.rec.on, state: P.run.state, step: P.run.curName || '' } }); };

  /* ------------------------------------------------------------ events from the engine */
  function addLog(level, msg, path) {
    P.run.log.push({ t: Date.now(), level: level, msg: msg, path: path });
    if (P.run.log.length > MAXLOG) P.run.log.splice(0, P.run.log.length - MAXLOG);
    appendLogRow(P.run.log[P.run.log.length - 1]);
  }
  Host.on('run.event', function (m) {
    var e = m.e || {}, name = e.name || e.type || '';
    switch (e.ev) {
      case 'flow_start': addLog('info', 'Flow "' + (e.name || '') + '" started (' + (e.steps || 0) + ' steps)'); break;
      case 'step_start': P.run.stepState[e.path] = { st: 'run' }; P.run.curName = (P.summary(e.step || { type: e.type, name: e.name }).label); P.emit('run:step', { path: e.path }); Run.notify(); break;
      case 'step_end': P.run.stepState[e.path] = { st: e.ok ? 'ok' : 'err', ms: e.ms, err: (P.run.stepState[e.path] || {}).err }; P.emit('run:step', { path: e.path });
        addLog(e.ok ? 'step' : 'error', (e.ok ? '✔ ' : '✖ ') + e.path + '  ' + (name || (P.defs[e.type] || {}).label || e.type) + '  (' + fmtMs(e.ms || 0) + ')', e.path); break;
      case 'step_error': {
        var cur = P.run.stepState[e.path] || {}; cur.err = e.error; if (!e.retry) cur.st = 'err'; P.run.stepState[e.path] = cur; P.emit('run:step', { path: e.path });
        addLog(e.retry ? 'warn' : 'error', (e.retry ? 'Retry ' + e.retry + ' – ' : '') + 'Step ' + e.path + ': ' + e.error, e.path); break; }
      case 'log': addLog(e.level === 'error' ? 'error' : e.level === 'warn' ? 'warn' : 'info', e.msg); break;
      case 'captcha': addLog('warn', '🤖 CAPTCHA detected (' + (e.kind || 'unknown') + ')'); break;
      case 'vars': P.run.vars = e.vars || {}; P.emit('run:vars'); break;
      case 'var': if (!P.run.vars) P.run.vars = {}; P.run.vars[e.name] = e.value; break;
      case 'flow_end':
        P.run.ended = Date.now();
        addLog(e.result === 'ok' ? 'ok' : e.result === 'stopped' ? 'warn' : 'error',
          e.result === 'ok' ? '✔ Flow finished in ' + fmtMs(e.ms || 0) : e.result === 'stopped' ? 'Flow stopped' : '✖ Flow failed' + (e.path !== undefined && e.path !== '' ? ' at step ' + e.path : '') + ': ' + (e.error || ''), e.path);
        if (e.result === 'ok') toast('Flow finished', 'ok'); else if (e.result === 'error') { toast('Flow failed: ' + (e.error || ''), 'err'); if (e.path !== undefined && e.path !== '' && !/^\d+\.flow/.test(e.path)) { P.sel = e.path; } }
        P.run.curName = '';
        break;
    }
  });
  Host.on('run.state', function (m) {
    if (m.state === 'idle') { P.run.ended = P.run.ended || Date.now(); setState('idle'); Flow.draw(); drawVars(); }
    else setState(m.state, m.path);
    if (m.state === 'paused') { P.sel = m.path; Flow.draw(); Panel.show('flow', true); }
  });

  /* ------------------------------------------------------------ log tab */
  var ICON = { info: 'ℹ', warn: '⚠', error: '✖', ok: '✔', step: '·' };
  function rowFor(l) {
    return h('div.lg.' + l.level, { 'data-level': l.level, onclick: function () { if (l.path != null && l.path !== '') { P.sel = l.path; Flow.draw(); Panel.show('flow'); } } },
      h('span.t', { text: new Date(l.t).toLocaleTimeString() }), h('span.i', { text: ICON[l.level] || '·' }), h('span.m', { text: l.msg }));
  }
  function visible(l) { return filter === 'all' ? l.level !== 'step' || true : filter === 'warn' ? l.level === 'warn' || l.level === 'error' : filter === 'error' ? l.level === 'error' : filter === 'info' ? l.level !== 'step' : true; }
  function appendLogRow(l) {
    if (!logBox) return;
    if (visible(l)) { var atEnd = logBox.scrollTop + logBox.clientHeight >= logBox.scrollHeight - 30; logBox.appendChild(rowFor(l)); if (atEnd) logBox.scrollTop = logBox.scrollHeight; }
    updateSummary();
  }
  function updateSummary() {
    var e = P.run.log.filter(function (l) { return l.level === 'error'; }).length, w = P.run.log.filter(function (l) { return l.level === 'warn'; }).length;
    sumEl.textContent = P.run.log.length ? P.run.log.length + ' entries · ' + e + ' error(s) · ' + w + ' warning(s)' : 'Nothing has run yet.';
  }
  function drawLog() { if (!logBox) return; clear(logBox); P.run.log.filter(visible).forEach(function (l) { logBox.appendChild(rowFor(l)); }); updateSummary(); logBox.scrollTop = logBox.scrollHeight; }
  function logText() { return P.run.log.map(function (l) { return new Date(l.t).toISOString() + '  ' + l.level.toUpperCase() + '  ' + l.msg; }).join('\r\n'); }

  Run.initLog = function (page) {
    sumEl = h('span.muted.small');
    var chips = [['all', 'All'], ['info', 'Messages'], ['warn', 'Warnings + errors'], ['error', 'Errors']].map(function (c) {
      return h('button.btn.sm' + (c[0] === filter ? '.on' : ''), { text: c[1], onclick: function (e) { filter = c[0]; $$('.btn', e.target.parentNode).forEach(function (b) { b.classList.remove('on'); }); e.target.classList.add('on'); drawLog(); } });
    });
    logBox = h('div.log');
    page.appendChild(h('div.toolbar', chips, h('span.grow'), h('button.btn.sm', { text: 'Copy', onclick: function () { Host.send('clipboard', { text: logText() }); toast('Log copied'); } }),
      h('button.btn.sm', { text: 'Save…', onclick: function () { Host.call('file.save_text', { title: 'Save log', name: 'flow-log.txt', text: logText(), filters: [['Text', '*.txt']] }, 0).then(function (r) { if (r.path) toast('Saved', 'ok'); }, function (e) { toast(e.message, 'err'); }); } }),
      h('button.btn.sm', { text: 'Clear', onclick: function () { P.run.log = []; drawLog(); } })));
    page.appendChild(h('div.pad', sumEl)); page.appendChild(logBox);
    drawLog();
  };

  /* ------------------------------------------------------------ variables tab */
  function parseVal(s) {
    s = String(s); var t = s.trim();
    if (/^(true|false|null)$/.test(t) || /^-?\d+(\.\d+)?$/.test(t) || /^[\[{]/.test(t)) { try { return JSON.parse(t); } catch (e) { return s; } }
    return s;
  }
  function drawInit() {
    clear(initBox);
    var vars = P.flow.variables = P.flow.variables || {}, keys = Object.keys(vars);
    keys.forEach(function (k) {
      var kin = h('input', { type: 'text', value: k }), v = vars[k], vin = h('input', { type: 'text', value: typeof v === 'string' ? v : JSON.stringify(v) });
      kin.addEventListener('change', function () { if (!kin.value || kin.value === k) { kin.value = k; return; } P.snap(); var nv = {}; Object.keys(vars).forEach(function (x) { nv[x === k ? kin.value : x] = vars[x]; }); P.flow.variables = nv; P.changed(); drawInit(); });
      vin.addEventListener('input', function () { P.snap('var:' + k); vars[k] = parseVal(vin.value); P.dirty = true; });
      initBox.appendChild(h('div.row', h('div', { style: { width: '36%' } }, kin), h('div.grow', vin), h('button.btn.sm.flat.danger', { text: '✕', onclick: function () { P.snap(); delete vars[k]; P.changed(); drawInit(); } })));
    });
    if (!keys.length) initBox.appendChild(h('div.muted.small', { text: 'No starting values. Variables are also created by steps like "Extract table" or "Read Excel".' }));
    initBox.appendChild(h('div', h('button.btn.sm', { text: '＋ Add variable', onclick: function () { var n = 'var' + (Object.keys(vars).length + 1); P.snap(); vars[n] = ''; P.changed(); drawInit(); } })));
  }
  function drawVars() {
    if (!varsBox) return; clear(varsBox);
    var v = P.run.vars;
    if (!v) { varsBox.appendChild(h('div.muted.small', { text: 'Run the flow to see the values here.' })); return; }
    Object.keys(v).filter(function (k) { return k !== 'env'; }).forEach(function (k) {
      var val = v[k], card = h('details.vcard', h('summary', h('b', { text: k }), ' ', h('span.chip', { text: Array.isArray(val) ? 'list · ' + val.length : val && typeof val === 'object' ? 'object' : typeof val }),
        h('span.muted.small.ell', { style: { marginLeft: '8px' }, text: Array.isArray(val) || (val && typeof val === 'object') ? '' : String(val).slice(0, 80) })));
      if (isTable(val)) {
        card.appendChild(GridView(val));
        card.appendChild(h('div.small.muted', { style: { marginTop: '4px' }, text: 'This preview shows the first rows only. “Save to Excel…” writes every row of the last run.' }));
        card.appendChild(h('div.row', { style: { marginTop: '6px' } }, h('button.btn.sm', { text: '📊 Save to Excel…', onclick: function () { Data.exportVar(k); } }), h('button.btn.sm', { text: 'Copy preview as JSON', onclick: function () { Host.send('clipboard', { text: JSON.stringify(val, null, 2) }); toast('Copied'); } })));
      } else card.appendChild(h('pre.mono.pre', { text: typeof val === 'object' ? JSON.stringify(val, null, 2) : String(val) }));
      varsBox.appendChild(card);
    });
  }
  Run.initVars = function (page) {
    initBox = h('div.col'); varsBox = h('div.col');
    page.appendChild(h('div.pad', h('h4', { text: 'Starting values' }), h('p.muted.small', { text: 'Available as {{name}} when the flow starts. Use them for settings such as a user name or a file path.' }), initBox,
      h('div.sep'), h('h4', { text: 'Values from the last run' }), varsBox,
      h('div.sep'), h('details', h('summary', { text: 'Cheat sheet: using variables' }), h('div.small.cheat', { html:
        '<p><code>{{name}}</code> inserts a variable anywhere. <code>{{row.First name}}</code> reads a column of the current Excel row; <code>{{rows[0].Email}}</code>, <code>{{list.length}}</code>.</p>' +
        '<p>Filters: <code>{{x | trim}}</code> <code>upper lower title</code> <code>number int round:2 abs</code> <code>default:"n/a"</code> <code>length first last join:", " split:","</code> <code>replace:"a":"b"</code> <code>substr:0:5</code> <code>urlencode json date:"YYYY-MM-DD"</code>.</p>' +
        '<p>Built-in: <code>{{page.url}}</code> <code>{{page.title}}</code> <code>{{page.text}}</code> (refreshed by "Wait until"), <code>{{error}}</code> after a failed step, <code>{{index}}</code> in loops.</p>' }))));
    P.on('flow:loaded', drawInit); P.on('run:vars', drawVars);
    drawInit(); drawVars();
  };

  /* ------------------------------------------------------------ run bar */
  Run.initBar = function (host) {
    bar.run = h('button.btn.primary.sm', { title: 'Run the whole flow (F9)', text: '▶ Run', onclick: function () { Run.start('run'); } });
    bar.step = h('button.btn.sm', { title: 'Pause before every step and move on with Next', text: '⏭ Step by step', onclick: function () { Run.start('step'); } });
    bar.pause = h('button.btn.sm.hide', { text: '⏸ Pause', onclick: Run.pause });
    bar.cont = h('button.btn.primary.sm.hide', { text: '▶ Continue', onclick: function () { Run.resume('continue'); } });
    bar.next = h('button.btn.sm.hide', { text: '⏭ Next step', onclick: function () { Run.resume('step'); } });
    bar.stop = h('button.btn.sm.danger.hide', { text: '■ Stop', onclick: Run.stop });
    bar.status = h('span.status.idle', { text: 'Ready' });
    host.appendChild(h('div.runbar', bar.run, bar.step, bar.pause, bar.cont, bar.next, bar.stop, bar.status));
  };
  Run.state = function () { return P.run.state; };
})();
