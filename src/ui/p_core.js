/* Studio panel - core state: catalog, flow model, undo/redo, small event bus. */
(function () {
  'use strict';
  var bus = {};
  var P = window.P = {
    catalog: null, defs: {}, flow: null, flowName: null, dirty: false, sel: null,
    undo: [], redo: [], lastSnapKey: '', lastSnapAt: 0,
    settings: {}, tabs: [], activeTab: 0,
    run: { state: 'idle', path: null, log: [], vars: null, stepState: {}, started: 0, ended: 0, breakpoints: {} },
    rec: { on: false, steps: [] },
    clip: null
  };
  P.on = function (n, f) { (bus[n] = bus[n] || []).push(f); };
  P.emit = function (n, a) { (bus[n] || []).slice().forEach(function (f) { try { f(a); } catch (e) { console.error(n, e); } }); };

  /* ------------------------------------------------------------ catalog */
  P.loadCatalog = function () {
    return Host.getText('catalog.json').then(function (t) { return JSON.parse(t); }).then(function (c) {
      P.catalog = c; P.defs = {}; P.cats = [];
      c.steps.forEach(function (s) { P.defs[s.type] = s; if (P.cats.indexOf(s.cat) < 0) P.cats.push(s.cat); });
    });
  };
  var COMMON = ['type', 'name', 'enabled', 'on_error', 'retries', 'retry_delay_ms', 'timeout_ms', 'comment', 'alts'];
  P.isCommon = function (k) { return COMMON.indexOf(k) >= 0; };
  P.nestedKeys = function (def) { return (def && def.params || []).filter(function (p) { return p.kind === 'steps'; }).map(function (p) { return p.name; }); };

  /* ------------------------------------------------------------ flow */
  P.newFlow = function (name) {
    return { format: 'flowbrowser-flow', version: 1, name: name || 'New flow', description: '', created: new Date().toISOString().slice(0, 19).replace('T', ' '),
      settings: Object.assign({ timeout_ms: 15000, step_delay_ms: 0, on_error: 'stop', captcha: 'pause', dialogs: 'accept' }, (P.settings && P.settings.automation) || {}), variables: {}, steps: [] };
  };
  P.setFlow = function (flow, name) {
    flow.settings = flow.settings || {}; flow.variables = flow.variables || {}; flow.steps = flow.steps || [];
    P.flow = flow; P.flowName = name || null; P.dirty = false; P.sel = null; P.undo = []; P.redo = []; P.run.stepState = {};
    P.emit('flow:loaded'); P.emit('flow:changed', {});
  };
  P.newStep = function (type) {
    var def = P.defs[type], s = { type: type };
    if (!def) return s;
    def.params.forEach(function (p) {
      if (p.kind === 'steps') s[p.name] = [];
      else if (p['default'] !== undefined && p.kind !== 'bool') s[p.name] = p['default'];
      else if (p.kind === 'bool' && p['default'] !== undefined) s[p.name] = p['default'];
    });
    return s;
  };

  /* path "1.body.2" -> {arr, idx, step}; path "" -> root */
  function tokens(path) { return path === '' || path == null ? [] : String(path).split('.').map(function (t) { return /^\d+$/.test(t) ? +t : t; }); }
  P.arrAt = function (arrPath, create) {
    var arr = P.flow.steps, t = tokens(arrPath);
    for (var i = 0; i < t.length; i += 2) {
      var st = arr[t[i]]; if (!st) return null;
      var key = t[i + 1];
      if (!Array.isArray(st[key])) { if (!create) return null; st[key] = []; }
      arr = st[key];
    }
    return arr;
  };
  P.splitPath = function (path) {      /* "1.body.2" -> {arrPath:"1.body", idx:2} */
    var t = tokens(path), idx = t.pop();
    t.pop();                          /* the key preceding the index is part of arrPath */
    return { arrPath: String(path).replace(/\.?\d+$/, ''), idx: idx };
  };
  P.stepAt = function (path) {
    var sp = P.splitPath(path), arr = P.arrAt(sp.arrPath);
    return arr ? arr[sp.idx] : null;
  };
  P.snap = function (key) {
    var now = Date.now();
    if (key && key === P.lastSnapKey && now - P.lastSnapAt < 1500) { P.lastSnapAt = now; return; }
    P.undo.push(JSON.stringify(P.flow)); if (P.undo.length > 100) P.undo.shift();
    P.redo = []; P.lastSnapKey = key || ''; P.lastSnapAt = now;
  };
  P.changed = function (info) { P.dirty = true; P.run.stepState = info && info.keepRun ? P.run.stepState : {}; P.emit('flow:changed', info || {}); };
  P.doUndo = function () { if (!P.undo.length) return; P.redo.push(JSON.stringify(P.flow)); P.flow = JSON.parse(P.undo.pop()); P.dirty = true; P.sel = null; P.emit('flow:changed', {}); };
  P.doRedo = function () { if (!P.redo.length) return; P.undo.push(JSON.stringify(P.flow)); P.flow = JSON.parse(P.redo.pop()); P.dirty = true; P.sel = null; P.emit('flow:changed', {}); };

  P.insert = function (arrPath, idx, step) {
    P.snap();
    var arr = P.arrAt(arrPath, true); if (!arr) return null;
    if (idx == null || idx > arr.length) idx = arr.length;
    arr.splice(idx, 0, step);
    P.sel = (arrPath ? arrPath + '.' : '') + idx;
    P.changed(); return P.sel;
  };
  P.remove = function (path) {
    var sp = P.splitPath(path), arr = P.arrAt(sp.arrPath); if (!arr) return;
    P.snap(); arr.splice(sp.idx, 1);
    if (P.sel && (P.sel === path || P.sel.indexOf(path + '.') === 0)) P.sel = null;
    P.changed();
  };
  P.move = function (path, toArrPath, toIdx) {
    var sp = P.splitPath(path), arr = P.arrAt(sp.arrPath), step = arr && arr[sp.idx]; if (!step) return;
    if (toArrPath === path || toArrPath.indexOf(path + '.') === 0) return;        /* cannot drop into itself */
    P.snap();
    var dest = P.arrAt(toArrPath, true);
    arr.splice(sp.idx, 1);
    if (dest === arr && toIdx > sp.idx) toIdx--;
    /* removing an earlier sibling shifts paths that run through the same array */
    if (toArrPath.indexOf(sp.arrPath + (sp.arrPath ? '.' : '')) === 0 && sp.arrPath !== toArrPath) {
      var rest = toArrPath.slice(sp.arrPath.length + (sp.arrPath ? 1 : 0)).split('.'), first = +rest[0];
      if (first > sp.idx) { rest[0] = String(first - 1); toArrPath = (sp.arrPath ? sp.arrPath + '.' : '') + rest.join('.'); dest = P.arrAt(toArrPath, true); }
    }
    if (toIdx == null || toIdx > dest.length) toIdx = dest.length;
    dest.splice(toIdx, 0, step);
    P.sel = (toArrPath ? toArrPath + '.' : '') + toIdx;
    P.changed();
  };
  P.dup = function (path) {
    var sp = P.splitPath(path), arr = P.arrAt(sp.arrPath); if (!arr) return;
    P.snap(); arr.splice(sp.idx + 1, 0, cloneJ(arr[sp.idx]));
    P.sel = (sp.arrPath ? sp.arrPath + '.' : '') + (sp.idx + 1); P.changed();
  };
  /* where does "add step" put a new step? after the selection, else at the end of the flow */
  P.insertionPoint = function () {
    if (P.sel) { var sp = P.splitPath(P.sel); return { arrPath: sp.arrPath, idx: sp.idx + 1 }; }
    return { arrPath: '', idx: P.flow.steps.length };
  };

  /* ------------------------------------------------------------ describing steps */
  var KEYP = ['url', 'selector', 'item_selector', 'file', 'tab', 'key', 'result_var', 'var', 'source', 'message', 'text', 'prompt', 'condition', 'seconds', 'code', 'expression', 'action', 'what', 'value'];
  P.summary = function (step) {
    var def = P.defs[step.type];
    var label = step.name || (def ? def.label : step.type);
    var detail = '';
    if (!step.name) {
      for (var i = 0; i < KEYP.length; i++) { var v = step[KEYP[i]]; if (v != null && v !== '' && typeof v !== 'object') { detail = String(v); break; } }
      if (!detail && Array.isArray(step.files) && step.files.length) detail = step.files.map(basename).join(', ');
      if (detail.length > 60) detail = detail.slice(0, 58) + '…';
    }
    return { label: label, detail: detail, icon: def ? def.icon : '❓' };
  };
  /* variable names the flow defines or uses, for the {{ }} helper and drop-downs */
  P.knownVars = function () {
    var out = {};
    function walk(steps) {
      (steps || []).forEach(function (s) {
        ['result_var', 'var', 'as', 'index_as', 'error_var'].forEach(function (k) { if (typeof s[k] === 'string' && s[k] && s[k].indexOf('{{') < 0) out[s[k]] = 1; });
        Object.keys(s).forEach(function (k) { if (Array.isArray(s[k]) && s[k].length && s[k][0] && typeof s[k][0] === 'object' && s[k][0].type) walk(s[k]); });
      });
    }
    if (P.flow) { Object.keys(P.flow.variables || {}).forEach(function (k) { out[k] = 1; }); walk(P.flow.steps); }
    if (P.run.vars) Object.keys(P.run.vars).forEach(function (k) { if (!/^(page|env)$/.test(k)) out[k] = 1; });
    return Object.keys(out).sort();
  };
  /* "{{row.First name}}" for a column / key; brackets when the name has awkward characters */
  P.tpl = function (base, key) { return '{{' + base + (/^[\w ]+$/.test(key) && !/^\d/.test(key) ? '.' + key : '["' + String(key).replace(/"/g, '') + '"]') + '}}'; };

  /* ------------------------------------------------------------ misc */
  P.target = function () { return P.rec.on ? 'rec' : 'flow'; };
  /* add steps to the flow (after the selection) or to the recording */
  P.addSteps = function (steps, where) {
    where = where || P.target();
    if (!Array.isArray(steps)) steps = [steps];
    if (where === 'rec') { steps.forEach(function (s) { P.rec.steps.push(s); }); P.emit('rec:changed'); return; }
    var ip = P.insertionPoint();
    P.snap();
    var arr = P.arrAt(ip.arrPath, true), at = ip.idx;
    steps.forEach(function (s, i) { arr.splice(at + i, 0, s); });
    P.sel = (ip.arrPath ? ip.arrPath + '.' : '') + (at + steps.length - 1);
    P.changed();
    toast(steps.length === 1 ? 'Added: ' + P.summary(steps[0]).label : 'Added ' + steps.length + ' steps', 'ok');
  };
  P.pageUrl = function () { var t = P.tabs.filter(function (x) { return x.id === P.activeTab; })[0]; return t ? t.url : ''; };
})();
