/* FlowBrowser page runtime. Injected into every page; the automation runner calls window.__fb.exec(op, args).
   Every op answers {ok:true,...} or {ok:false, code, error}. No external dependencies. */
(function () {
  'use strict';
  if (window.__fb && window.__fb.version) return;

  var NS = {};
  function fail(code, msg) { var e = new Error(msg); e.code = code; throw e; }
  function norm(s) { return (s == null ? '' : String(s)).replace(/\s+/g, ' ').trim(); }

  /* The page may be running in an older engine (Wine's MSHTML / Gecko): no isConnected, getRootNode, MouseEvent constructor,
     DataTransfer constructor, File constructor or Object.assign. These helpers keep every op working there. */
  function extend(a, b) { if (b) for (var k in b) if (Object.prototype.hasOwnProperty.call(b, k)) a[k] = b[k]; return a; }
  function connected(el) {
    if (el.isConnected !== undefined) return el.isConnected;
    var n = el;
    for (var i = 0; n && i < 400; i++) { if (n.nodeType === 9) return true; n = n.parentNode || n.host || null; }
    return false;
  }
  function rootOf(n) { if (n.getRootNode) return n.getRootNode(); while (n.parentNode) n = n.parentNode; return n; }
  function mouseEvent(W, type, init) {
    try { return new W.MouseEvent(type, init); } catch (e) { /* no constructor */ }
    var ev = W.document.createEvent('MouseEvents');
    ev.initMouseEvent(type, !!init.bubbles, !!init.cancelable, W, init.detail || 0, 0, 0, init.clientX || 0, init.clientY || 0, false, false, false, false, init.button || 0, null);
    return ev;
  }
  function newDT(files) {
    var dt = null;
    try { dt = new DataTransfer(); } catch (e) { dt = null; }
    if (dt && dt.items) { if (files) files.forEach(function (f) { dt.items.add(f); }); return dt; }
    var data = {}, types = files && files.length ? ['Files'] : [];
    return { effectAllowed: 'uninitialized', dropEffect: 'none', files: files || [], items: [], types: types,
             setData: function (t, v) { data[t] = String(v); if (types.indexOf(t) < 0) types.push(t); },
             getData: function (t) { return data[t] === undefined ? '' : data[t]; },
             clearData: function (t) { if (t) delete data[t]; else data = {}; }, setDragImage: function () { /* nothing */ } };
  }
  function lc(s) { return norm(s).toLowerCase(); }
  function cut(s, n) { s = norm(s); return s.length > n ? s.slice(0, n) + '…' : s; }
  function win(el) { return (el.ownerDocument && el.ownerDocument.defaultView) || window; }
  function isEl(x) { return x && x.nodeType === 1; }

  /* ============================================================ selectors */

  var PREFIX = /^(css|xpath|text|label|placeholder|nth|id|name|testid|aria|alt|title)\s*[:=]\s*([\s\S]*)$/i;

  function splitChain(sel) {
    var parts = [], cur = '', q = null, depth = 0;
    for (var i = 0; i < sel.length; i++) {
      var c = sel[i];
      if (q) { cur += c; if (c === '\\') { cur += sel[++i] || ''; } else if (c === q) q = null; continue; }
      if (c === '"' || c === "'") { q = c; cur += c; continue; }
      if (c === '[' || c === '(') depth++;
      if (c === ']' || c === ')') depth--;
      if (depth <= 0 && sel.substr(i, 4) === ' >> ') { parts.push(cur); cur = ''; i += 3; continue; }
      cur += c;
    }
    parts.push(cur);
    return parts.map(function (p) { return p.trim(); }).filter(Boolean);
  }

  function parsePart(p) {
    var m = PREFIX.exec(p);
    if (m) return { kind: m[1].toLowerCase(), value: m[2].trim() };
    if (/^\(?\/\//.test(p) || /^\.\//.test(p)) return { kind: 'xpath', value: p };
    return { kind: 'css', value: p };
  }

  function cssEsc(s) { return (window.CSS && CSS.escape) ? CSS.escape(s) : String(s).replace(/[^\w-]/g, '\\$&'); }
  function attrSel(name, v) { return '[' + name + '="' + String(v).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"]'; }

  function allShadowHosts(root, out) {
    var els = root.querySelectorAll ? root.querySelectorAll('*') : [];
    for (var i = 0; i < els.length; i++) if (els[i].shadowRoot) { out.push(els[i].shadowRoot); allShadowHosts(els[i].shadowRoot, out); }
    return out;
  }

  function queryCss(root, css) {
    var res;
    try { res = Array.prototype.slice.call(root.querySelectorAll(css)); }
    catch (e) { fail('bad_selector', 'Invalid CSS selector "' + css + '": ' + (e.message || e)); }
    if (root.matches && !res.length) { try { if (root.matches(css)) res = [root]; } catch (e) { /* ignore */ } }
    if (res.length) return res;
    var hosts = allShadowHosts(root, []);                 /* nothing in the light DOM: look inside shadow roots */
    for (var i = 0; i < hosts.length; i++) res = res.concat(Array.prototype.slice.call(hosts[i].querySelectorAll(css)));
    return res;
  }

  function queryXpath(root, xp) {
    var doc = root.nodeType === 9 ? root : root.ownerDocument, out = [];
    try {
      var snap = doc.evaluate(xp, root, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
      for (var i = 0; i < snap.snapshotLength; i++) { var n = snap.snapshotItem(i); if (isEl(n)) out.push(n); else if (n && n.parentElement) out.push(n.parentElement); }
    } catch (e) { fail('bad_selector', 'Invalid XPath "' + xp + '": ' + (e.message || e)); }
    return out;
  }

  function parseText(v) {
    var m = /^"([\s\S]*)"$/.exec(v) || /^'([\s\S]*)'$/.exec(v);
    return m ? { text: lc(m[1]), exact: true } : { text: lc(v), exact: false };
  }

  function everyElement(root) {
    var out = Array.prototype.slice.call(root.querySelectorAll ? root.querySelectorAll('*') : []);
    var hosts = allShadowHosts(root, []);
    for (var i = 0; i < hosts.length; i++) out = out.concat(Array.prototype.slice.call(hosts[i].querySelectorAll('*')));
    return out;
  }

  var SKIP_TEXT = { SCRIPT: 1, STYLE: 1, NOSCRIPT: 1, HEAD: 1, META: 1, LINK: 1, TITLE: 1, TEMPLATE: 1 };

  function ownText(el) {
    var t = '';
    for (var n = el.firstChild; n; n = n.nextSibling) if (n.nodeType === 3) t += n.nodeValue + ' ';
    return norm(t);
  }

  function labelsOf(el) {   /* short strings that name an element for text: matching */
    var out = [], tag = el.tagName;
    out.push(ownText(el));
    var a = el.getAttribute && el.getAttribute('aria-label'); if (a) out.push(a);
    if (tag === 'INPUT' && /^(button|submit|reset)$/i.test(el.type)) out.push(el.value);
    if (tag === 'IMG' && el.alt) out.push(el.alt);
    if (el.title) out.push(el.title);
    return out.map(norm).filter(Boolean);
  }

  function queryText(root, spec) {
    var exact = [], loose = [], els = everyElement(root);
    var i, j;
    for (i = 0; i < els.length; i++) {
      var el = els[i];
      if (SKIP_TEXT[el.tagName]) continue;
      var ls = labelsOf(el), hitE = false, hitL = false;
      for (j = 0; j < ls.length; j++) {
        var l = ls[j].toLowerCase();
        if (l === spec.text) hitE = true; else if (!spec.exact && l.indexOf(spec.text) >= 0) hitL = true;
      }
      if (hitE) exact.push(el); else if (hitL) loose.push(el);
    }
    var res = exact.concat(loose);
    if (res.length) return res;
    /* text split across child elements: the smallest element whose whole text matches */
    var cand = [];
    for (i = 0; i < els.length; i++) {
      var e2 = els[i];
      if (SKIP_TEXT[e2.tagName]) continue;
      var t = lc(e2.textContent);
      if (spec.exact ? t === spec.text : t.indexOf(spec.text) >= 0) cand.push(e2);
    }
    return cand.filter(function (c) { return !cand.some(function (o) { return o !== c && c.contains(o); }); });
  }

  function controlOfLabel(lab) {
    if (lab.control) return lab.control;
    var f = lab.getAttribute('for');
    if (f) { var r = rootOf(lab); var t = (r.getElementById ? r.getElementById(f) : null) || document.getElementById(f); if (t) return t; }
    return lab.querySelector('input,select,textarea,button,[contenteditable],[role=textbox],[role=combobox]');
  }

  function interactive(el) {
    return /^(INPUT|SELECT|TEXTAREA|BUTTON|A|SUMMARY)$/.test(el.tagName) || !!el.getAttribute('role') || el.isContentEditable === true && el.getAttribute('contenteditable') !== null;
  }

  function queryLabel(root, spec) {
    var out = [], els = everyElement(root), i;
    function match(t) { t = lc(t); return spec.exact ? t === spec.text : t.indexOf(spec.text) >= 0; }
    for (i = 0; i < els.length; i++) {
      var el = els[i];
      if (el.tagName === 'LABEL' && match(el.textContent)) { var c = controlOfLabel(el); if (c && out.indexOf(c) < 0) out.push(c); }
      var al = el.getAttribute('aria-label');
      if (al && match(al) && interactive(el) && out.indexOf(el) < 0) out.push(el);
      var lb = el.getAttribute('aria-labelledby');
      if (lb) {
        var txt = lb.split(/\s+/).map(function (id) { var n = document.getElementById(id); return n ? n.textContent : ''; }).join(' ');
        if (match(txt) && out.indexOf(el) < 0) out.push(el);
      }
    }
    return out;
  }

  function queryPart(root, part) {
    var v = part.value;
    switch (part.kind) {
      case 'css': return queryCss(root, v);
      case 'xpath': return queryXpath(root, v);
      case 'text': return queryText(root, parseText(v));
      case 'label': return queryLabel(root, parseText(v));
      case 'placeholder': return queryCss(root, '[placeholder]').filter(function (e) { var p = lc(e.getAttribute('placeholder')), s = parseText(v); return s.exact ? p === s.text : p.indexOf(s.text) >= 0; });
      case 'id': return queryCss(root, attrSel('id', v));
      case 'name': return queryCss(root, attrSel('name', v));
      case 'testid': return queryCss(root, attrSel('data-testid', v) + ',' + attrSel('data-test', v) + ',' + attrSel('data-cy', v) + ',' + attrSel('data-qa', v));
      case 'aria': return queryCss(root, attrSel('aria-label', v));
      case 'alt': return queryCss(root, attrSel('alt', v));
      case 'title': return queryCss(root, attrSel('title', v));
    }
    return [];
  }

  /* all elements matching one selector string (with " >> " chains through frames and shadow roots) */
  function resolveAllOne(sel, scope) {
    var parts = splitChain(sel);
    if (!parts.length) fail('bad_selector', 'The selector is empty');
    var roots = [scope || document], list = [];
    for (var i = 0; i < parts.length; i++) {
      var p = parsePart(parts[i]);
      if (p.kind === 'nth') {
        var n = parseInt(p.value, 10);
        if (isNaN(n) || n === 0) fail('bad_selector', 'nth needs a number like nth=2 (1 = first, -1 = last)');
        var pick = n > 0 ? list[n - 1] : list[list.length + n];
        list = pick ? [pick] : [];
        roots = list;
        continue;
      }
      list = [];
      for (var r = 0; r < roots.length; r++) {
        var rt = roots[r];
        if (isEl(rt) && (rt.tagName === 'IFRAME' || rt.tagName === 'FRAME')) {
          var d = null;
          try { d = rt.contentDocument; } catch (e) { d = null; }
          if (!d) fail('cross_origin_frame', 'This frame belongs to another website, so its content cannot be reached');
          rt = d;
        }
        var found = queryPart(rt, p);
        for (var k = 0; k < found.length; k++) if (list.indexOf(found[k]) < 0) list.push(found[k]);
      }
      roots = list;
      if (!list.length) return [];
    }
    return list;
  }

  /* args.selector with args.alts as fallbacks ("self healing") */
  function resolveAll(args, scope) {
    var sels = [args.selector].concat(args.alts || []).filter(Boolean);
    if (!sels.length) fail('bad_selector', 'No selector given');
    var firstErr = null;
    for (var i = 0; i < sels.length; i++) {
      try { var r = resolveAllOne(sels[i], scope); if (r.length) { r.used = sels[i]; return r; } }
      catch (e) { if (!firstErr) firstErr = e; if (i === sels.length - 1 && !(args.alts && args.alts.length)) throw e; }
    }
    if (firstErr && firstErr.code === 'bad_selector' && sels.length === 1) throw firstErr;
    return [];
  }

  function find(args) {
    var all = resolveAll(args);
    if (!all.length) fail('not_found', 'Element not found: ' + args.selector);
    return all[0];
  }

  NS._resolveAll = resolveAll;
  NS._resolveOne = resolveAllOne;

  /* ============================================================ geometry */

  function visibleBasic(el) {
    if (!connected(el)) return false;
    var cs = win(el).getComputedStyle(el);
    if (cs.display === 'none' || cs.visibility === 'hidden' || cs.visibility === 'collapse') return false;
    var r = el.getClientRects();
    if (!r.length) return false;
    for (var i = 0; i < r.length; i++) if (r[i].width > 0 && r[i].height > 0) return true;
    return el.tagName === 'INPUT' && el.type !== 'hidden' ? false : (el.offsetWidth > 0 || el.offsetHeight > 0);
  }

  function frameOffset(el) {
    var x = 0, y = 0, w = win(el);
    while (w !== w.parent && w.frameElement) {
      var fr = w.frameElement, rr = fr.getBoundingClientRect(), cs = w.parent.getComputedStyle(fr);
      x += rr.left + fr.clientLeft + (parseFloat(cs.paddingLeft) || 0);
      y += rr.top + fr.clientTop + (parseFloat(cs.paddingTop) || 0);
      w = w.parent;
    }
    return { x: x, y: y };
  }

  function deepFromPoint(doc, x, y) {
    var e = doc.elementFromPoint(x, y);
    while (e && e.shadowRoot) { var inner = e.shadowRoot.elementFromPoint(x, y); if (!inner || inner === e) break; e = inner; }
    return e;
  }

  function describe(e) { return { tag: e.tagName.toLowerCase(), id: e.id || '', cls: cut(e.className && e.className.baseVal === undefined ? e.className : '', 60), text: cut(e.textContent, 40) }; }

  function rectOf(el, scroll) {
    if (scroll !== false) { try { el.scrollIntoView({ block: 'center', inline: 'center', behavior: 'instant' }); } catch (e) { el.scrollIntoView(); } }
    var rects = el.getClientRects(), r = null;
    for (var i = 0; i < rects.length; i++) if (rects[i].width > 0 && rects[i].height > 0) { r = rects[i]; break; }
    var vis = visibleBasic(el);
    if (!r) r = el.getBoundingClientRect();
    var off = frameOffset(el);
    var lx = r.left + r.width / 2, ly = r.top + r.height / 2;
    var out = { ok: true, visible: vis && r.width > 0 && r.height > 0, x: lx + off.x, y: ly + off.y, w: r.width, h: r.height,
                tag: el.tagName.toLowerCase(), disabled: !!(el.disabled || el.getAttribute('aria-disabled') === 'true'), covered: null };
    var top = window.top === window ? window : window;
    out.px = r.left + off.x + (top.pageXOffset || 0); out.py = r.top + off.y + (top.pageYOffset || 0);
    if (out.visible) {
      var hit = deepFromPoint(el.ownerDocument, lx, ly);
      if (hit && hit !== el && !el.contains(hit) && !hit.contains(el)) {
        var lab = hit.closest && hit.closest('label');
        var pe = win(hit).getComputedStyle(hit).pointerEvents;
        if (!(lab && lab.control === el) && pe !== 'none') out.covered = describe(hit);
      }
    }
    return out;
  }

  /* ============================================================ value helpers */

  function setNative(el, v) {
    var W = win(el), proto = el.tagName === 'TEXTAREA' ? W.HTMLTextAreaElement.prototype : el.tagName === 'SELECT' ? W.HTMLSelectElement.prototype : W.HTMLInputElement.prototype;
    var d = Object.getOwnPropertyDescriptor(proto, 'value');
    if (d && d.set) d.set.call(el, v); else el.value = v;
  }

  function fire(el, type, init) {
    var W = win(el), ev;
    try { ev = (type === 'input' && W.InputEvent) ? new W.InputEvent('input', extend({ bubbles: true, composed: true, inputType: 'insertText' }, init)) : new W.Event(type, { bubbles: true, composed: true }); }
    catch (e) { ev = document.createEvent('Event'); ev.initEvent(type, true, true); }
    el.dispatchEvent(ev);
  }

  function truthy(v) { return /^(true|yes|y|1|on|checked|x|ja|oui|si)$/i.test(norm(v)); }

  function textOfControl(el) {
    var t = '';
    if (el.labels && el.labels.length) t = el.labels[0].textContent;
    if (!t) { var l = el.closest && el.closest('label'); if (l) t = l.textContent; }
    if (!t && el.getAttribute('aria-label')) t = el.getAttribute('aria-label');
    return norm(t);
  }

  function kindOf(el) {
    var t = el.tagName;
    if (t === 'SELECT') return 'select';
    if (t === 'TEXTAREA') return 'textarea';
    if (t === 'INPUT') return (el.type || 'text').toLowerCase();
    if (el.isContentEditable) return 'editable';
    var role = el.getAttribute && el.getAttribute('role');
    if (role === 'checkbox' || role === 'switch') return 'aria-checkbox';
    if (role === 'textbox' || role === 'combobox' || role === 'searchbox') return 'aria-textbox';
    return t.toLowerCase();
  }

  function setChecked(el, want) {
    var cur = el.tagName === 'INPUT' ? el.checked : el.getAttribute('aria-checked') === 'true';
    if (cur !== want) el.click();
    return el.tagName === 'INPUT' ? el.checked : el.getAttribute('aria-checked') === 'true';
  }

  function selectOption(el, value, by) {
    var opts = Array.prototype.slice.call(el.options), want = String(value == null ? '' : value), hit = [], i;
    var parts = el.multiple ? want.split('|').map(function (s) { return s.trim(); }).filter(function (s) { return s !== ''; }) : [want];
    if (!parts.length) parts = [''];
    var chosen = [];
    parts.forEach(function (w) {
      var m = null;
      if (by === 'index') { var n = parseInt(w, 10); m = opts[n - 1] || null; }
      else {
        if (by === 'auto' || by === 'value') for (i = 0; i < opts.length && !m; i++) if (opts[i].value === w) m = opts[i];
        if (!m && (by === 'auto' || by === 'text')) {
          for (i = 0; i < opts.length && !m; i++) if (norm(opts[i].text) === norm(w)) m = opts[i];
          for (i = 0; i < opts.length && !m; i++) if (lc(opts[i].text) === lc(w)) m = opts[i];
          for (i = 0; i < opts.length && !m && w !== ''; i++) if (lc(opts[i].text).indexOf(lc(w)) >= 0) m = opts[i];
        }
      }
      if (!m) fail('no_option', 'Option "' + w + '" not found. Available: ' + opts.slice(0, 15).map(function (o) { return '"' + norm(o.text) + '"'; }).join(', ') + (opts.length > 15 ? ', …' : ''));
      chosen.push(m);
    });
    if (el.multiple) opts.forEach(function (o) { o.selected = chosen.indexOf(o) >= 0; }); else chosen[0].selected = true;
    fire(el, 'input'); fire(el, 'change');
    return chosen.map(function (o) { return norm(o.text); }).join(' | ');
  }

  function dateValue(type, v) {
    v = norm(v);
    var iso = { date: /^\d{4}-\d{2}-\d{2}$/, 'datetime-local': /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}/, time: /^\d{2}:\d{2}/, month: /^\d{4}-\d{2}$/, week: /^\d{4}-W\d{2}$/ }[type];
    if (!iso || iso.test(v)) return v;
    var d = new Date(v);
    if (isNaN(d)) fail('bad_value', 'For a ' + type + ' field use the format ' + ({ date: 'YYYY-MM-DD', time: 'HH:MM', 'datetime-local': 'YYYY-MM-DDTHH:MM', month: 'YYYY-MM', week: 'YYYY-Www' })[type] + ' (got "' + v + '")');
    function p(n) { return (n < 10 ? '0' : '') + n; }
    var day = d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate());
    if (type === 'date') return day;
    if (type === 'month') return day.slice(0, 7);
    if (type === 'time') return p(d.getHours()) + ':' + p(d.getMinutes());
    return day + 'T' + p(d.getHours()) + ':' + p(d.getMinutes());
  }

  function setSlider(el, value) {
    if (el.tagName !== 'INPUT' || el.type !== 'range') fail('unsupported', 'Only <input type=range> sliders can be set directly. For a custom slider use drag_drop, or focus it and press ArrowLeft / ArrowRight');
    var n = parseFloat(String(value).replace(',', '.'));
    if (isNaN(n)) fail('bad_value', '"' + value + '" is not a number');
    var min = parseFloat(el.min); if (isNaN(min)) min = 0;
    var max = parseFloat(el.max); if (isNaN(max)) max = 100;
    var step = parseFloat(el.step); if (isNaN(step) || step <= 0) step = 1;
    n = Math.max(min, Math.min(max, n));
    n = min + Math.round((n - min) / step) * step;
    setNative(el, String(Math.round(n * 1e6) / 1e6));
    fire(el, 'input'); fire(el, 'change');
    return el.value;
  }

  function radioGroup(el) {
    var root = el.form || rootOf(el), n = el.name;
    if (!n) return [el];
    return Array.prototype.slice.call(root.querySelectorAll('input[type=radio]')).filter(function (r) { return r.name === n; });
  }

  function fillEl(el, value, mode) {
    var kind = kindOf(el), v = value == null ? '' : String(value);
    if (el.tagName === 'LABEL') { var c = controlOfLabel(el); if (c) { el = c; kind = kindOf(el); } }
    switch (kind) {
      case 'select': return { kind: kind, value: selectOption(el, v, 'auto') };
      case 'checkbox': case 'aria-checkbox': return { kind: kind, value: setChecked(el, truthy(v)) };
      case 'radio': {
        var grp = radioGroup(el), hit = null;
        grp.forEach(function (r) { if (!hit && (lc(r.value) === lc(v) || lc(textOfControl(r)) === lc(v))) hit = r; });
        if (!hit && truthy(v)) hit = el;
        if (!hit) { if (/^(false|no|n|0|off)$/i.test(v)) return { kind: kind, value: false }; fail('no_option', 'No radio button "' + v + '" in this group. Options: ' + grp.map(function (r) { return '"' + (textOfControl(r) || r.value) + '"'; }).join(', ')); }
        if (!hit.checked) hit.click();
        return { kind: kind, value: hit.value };
      }
      case 'range': return { kind: kind, value: setSlider(el, v) };
      case 'file': fail('unsupported', 'This is a file field: use the "Upload file" step');
      case 'date': case 'time': case 'datetime-local': case 'month': case 'week':
        el.focus(); setNative(el, dateValue(kind, v)); fire(el, 'input'); fire(el, 'change'); return { kind: kind, value: el.value };
      case 'editable': case 'aria-textbox':
        el.focus();
        if (kind === 'aria-textbox' && el.tagName === 'INPUT') { setNative(el, v); fire(el, 'input'); fire(el, 'change'); return { kind: kind, value: v }; }
        document.execCommand('selectAll', false, null);
        if (!document.execCommand('insertText', false, v)) { el.textContent = v; fire(el, 'input'); }
        return { kind: kind, value: v };
      case 'button': case 'submit': case 'reset': case 'image': fail('unsupported', 'This is a button, not a field');
      default:
        if (el.tagName !== 'INPUT' && el.tagName !== 'TEXTAREA') fail('unsupported', 'This element (<' + el.tagName.toLowerCase() + '>) is not a form field');
        if (el.readOnly) fail('readonly', 'The field is read-only');
        if (el.disabled) fail('disabled', 'The field is disabled');
        el.focus();
        setNative(el, v);
        fire(el, 'input', { data: v });
        fire(el, 'change');
        return { kind: kind, value: el.value };
    }
  }

  /* ============================================================ ops */

  var ops = {};

  ops.rect = function (a) { return rectOf(find(a), a.scroll); };

  ops.exists = function (a) {
    var all = resolveAll(a), el = all[0], vis = false;
    if (el) vis = visibleBasic(el);
    if (!el && !a.optional) fail('not_found', 'Element not found: ' + a.selector);
    return { ok: true, exists: !!el && (!a.visible_only || vis), visible: vis, count: all.length };
  };

  ops.count = function (a) { return { ok: true, count: resolveAll(a).length }; };

  ops.focus = function (a) { var el = find(a); try { el.scrollIntoView({ block: 'center' }); } catch (e) { /* ignore */ } el.focus(); return { ok: true }; };

  ops.click_js = function (a) {
    var el = find(a);
    el.scrollIntoView({ block: 'center', inline: 'center' });
    var n = a.count || 1, W = win(el);
    if (a.button === 'right') { el.dispatchEvent(mouseEvent(W, 'contextmenu', { bubbles: true, cancelable: true, button: 2, view: W })); return { ok: true }; }
    if (a.button === 'middle') { el.dispatchEvent(mouseEvent(W, 'auxclick', { bubbles: true, cancelable: true, button: 1, view: W })); return { ok: true }; }
    for (var i = 0; i < n; i++) el.click();
    if (n > 1) el.dispatchEvent(mouseEvent(W, 'dblclick', { bubbles: true, cancelable: true, detail: 2, view: W }));
    return { ok: true };
  };

  ops.fill = function (a) {
    var el = find(a), r = fillEl(el, a.value, a.mode);
    return { ok: true, kind: r.kind, value: r.value };
  };

  ops.prepare_type = function (a) {
    var el = find(a), kind = kindOf(el);
    if (el.tagName === 'LABEL') { var c = controlOfLabel(el); if (c) el = c; }
    try { el.scrollIntoView({ block: 'center' }); } catch (e) { /* ignore */ }
    el.focus();
    if (a.clear !== false) {
      if (el.select && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA')) { try { el.select(); } catch (e) { setNative(el, ''); } }
      else if (el.isContentEditable) document.execCommand('selectAll', false, null);
      if ((el.value || '') !== '' && el.selectionStart === el.selectionEnd && el.value) { setNative(el, ''); fire(el, 'input'); }   /* types that cannot select (e.g. number) */
    } else if (el.setSelectionRange && el.value != null) { try { el.setSelectionRange(el.value.length, el.value.length); } catch (e) { /* ignore */ } }
    return { ok: true, kind: kind };
  };

  ops.select_option = function (a) {
    var el = find(a);
    if (el.tagName !== 'SELECT') fail('unsupported', 'This is not a native drop-down. Click it to open the list, then click the option (e.g. text:"' + cut(a.value, 30) + '")');
    return { ok: true, selected: selectOption(el, a.value, a.by || 'auto') };
  };

  ops.check = function (a) {
    var el = find(a);
    if (el.tagName === 'LABEL') { var c = controlOfLabel(el); if (c) el = c; }
    var k = kindOf(el);
    if (k !== 'checkbox' && k !== 'radio' && k !== 'aria-checkbox') fail('unsupported', 'This is not a check box or radio button');
    var cur = el.tagName === 'INPUT' ? el.checked : el.getAttribute('aria-checked') === 'true';
    var want = a.state === 'uncheck' ? false : a.state === 'toggle' ? !cur : true;
    return { ok: true, checked: setChecked(el, want) };
  };

  ops.set_slider = function (a) { return { ok: true, value: setSlider(find(a), a.value) }; };

  ops.submit_form = function (a) {
    var el = find(a), f = el.tagName === 'FORM' ? el : el.closest('form');
    if (!f) fail('unsupported', 'The element is not inside a form');
    if (f.requestSubmit) f.requestSubmit(); else f.submit();
    return { ok: true };
  };

  function scroller() {
    var se = document.scrollingElement || document.documentElement;
    if (se.scrollHeight > se.clientHeight + 4) return se;
    var best = null, bestA = 0, all = document.querySelectorAll('*');
    for (var i = 0; i < all.length; i++) {
      var e = all[i];
      if (e.scrollHeight > e.clientHeight + 40 && /(auto|scroll)/.test(getComputedStyle(e).overflowY)) {
        var area = e.clientWidth * e.clientHeight;
        if (area > bestA) { best = e; bestA = area; }
      }
    }
    return best || se;
  }

  ops.scroll = function (a) {
    if (a.to === 'element') { find(a).scrollIntoView({ block: 'center', behavior: 'instant' }); return { ok: true }; }
    var s = scroller();
    if (a.to === 'top') s.scrollTop = 0;
    else if (a.to === 'by') s.scrollTop += a.amount || 500;
    else s.scrollTop = s.scrollHeight;
    return { ok: true, y: s.scrollTop, height: s.scrollHeight };
  };

  ops.get = function (a) {
    var el = find(a), w = a.what || 'text';
    var v;
    switch (w) {
      case 'value': v = el.value != null ? el.value : el.getAttribute('value') || ''; break;
      case 'html': v = el.innerHTML; break;
      case 'outerhtml': v = el.outerHTML; break;
      case 'checked': v = el.tagName === 'INPUT' ? !!el.checked : el.getAttribute('aria-checked') === 'true'; break;
      case 'selected': v = el.tagName === 'SELECT' ? Array.prototype.slice.call(el.selectedOptions).map(function (o) { return norm(o.text); }).join(' | ') : ''; break;
      case 'href': v = el.href || el.getAttribute('href') || ''; break;
      case 'src': v = el.currentSrc || el.src || el.getAttribute('src') || ''; break;
      default: v = (el.innerText != null && el.innerText !== '' ? el.innerText : el.textContent) || ''; v = String(v).trim();
    }
    return { ok: true, value: v };
  };

  ops.attr = function (a) {
    var el = find(a), n = a.attribute;
    if (n === 'innerText' || n === 'textContent' || n === 'value' || n === 'checked' || n === 'href' || n === 'src') {
      var v = el[n]; return { ok: true, value: v == null ? null : v };
    }
    return { ok: true, value: el.getAttribute(n) };
  };

  ops.has_text = function (a) {
    var t = lc(a.text), body = document.body ? document.body.innerText : '';
    return { ok: true, found: lc(body).indexOf(t) >= 0 };
  };

  ops.page_info = function () {
    return { ok: true, url: location.href, title: document.title, ready: document.readyState, text: cut(document.body ? document.body.innerText : '', 5000) };
  };

  /* ---------- tables ---------- */

  function cellText(c) { return norm(c.innerText != null && c.innerText !== '' ? c.innerText : c.textContent); }

  function tableGrid(t) {
    var rows, cells;
    if (t.tagName === 'TABLE') { rows = Array.prototype.slice.call(t.rows); cells = function (r) { return Array.prototype.slice.call(r.cells); }; }
    else {
      rows = Array.prototype.slice.call(t.querySelectorAll('[role=row]'));
      if (!rows.length) rows = Array.prototype.slice.call(t.children);
      cells = function (r) {
        var c = r.querySelectorAll('[role=cell],[role=gridcell],[role=columnheader],[role=rowheader],th,td');
        return c.length ? Array.prototype.slice.call(c) : Array.prototype.slice.call(r.children);
      };
    }
    var grid = [], span = [], headerRows = 0, hasThead = false;
    rows.forEach(function (tr, ri) {
      var line = [], col = 0, cs = cells(tr), allTh = cs.length > 0;
      function drain() { while (span[col] && span[col].n > 0) { line[col] = span[col].v; span[col].n--; col++; } }
      cs.forEach(function (c) {
        drain();
        var cspan = Math.max(1, Math.min(parseInt(c.colSpan || c.getAttribute('colspan') || 1, 10) || 1, 50));
        var rspan = Math.max(1, Math.min(parseInt(c.rowSpan || c.getAttribute('rowspan') || 1, 10) || 1, 200));
        var v = cellText(c);
        for (var k = 0; k < cspan; k++) { line[col] = k === 0 ? v : ''; if (rspan > 1) span[col] = { n: rspan - 1, v: k === 0 ? v : '' }; col++; }
        if (!(c.tagName === 'TH' || /columnheader/.test(c.getAttribute('role') || ''))) allTh = false;
      });
      drain();
      for (var i = 0; i < line.length; i++) if (line[i] === undefined) line[i] = '';
      if (tr.parentElement && tr.parentElement.tagName === 'THEAD') hasThead = true;
      grid.push(line);
      if (allTh && headerRows === ri) headerRows = ri + 1;
    });
    return { grid: grid, headerRows: headerRows, hasThead: hasThead };
  }

  function uniqueKeys(names) {
    var seen = {};
    return names.map(function (n, i) {
      var base = norm(n) || ('Column ' + (i + 1)), k = base, c = 2;
      while (seen[k]) k = base + '_' + c++;
      seen[k] = 1;
      return k;
    });
  }

  function tableToRows(el, headers) {
    var g = tableGrid(el), grid = g.grid.filter(function (r) { return r.some(function (c) { return c !== ''; }); });
    var mode = headers || 'auto', hasHead = mode === 'first_row' ? true : mode === 'none' ? false : (g.headerRows > 0 || g.hasThead);
    if (!grid.length) return { rows: [], columns: [] };
    if (!hasHead) return { rows: grid, columns: [] };
    var cols = uniqueKeys(grid[0]);
    var rows = grid.slice(1).map(function (r) { var o = {}; cols.forEach(function (c, i) { o[c] = r[i] === undefined ? '' : r[i]; }); return o; });
    return { rows: rows, columns: cols };
  }

  ops.extract_table = function (a) {
    var el = find(a);
    if (el.tagName !== 'TABLE' && !el.matches('[role=table],[role=grid],[role=treegrid]')) {
      var inner = el.querySelector('table'); if (inner) el = inner;
    }
    var r = tableToRows(el, a.headers);
    return { ok: true, rows: r.rows, columns: r.columns };
  };

  /* ---------- lists of similar items ---------- */

  function fieldValue(item, f) {
    var el = item;
    if (f.selector) {
      var found = [];
      try { found = resolveAllOne(f.selector, item); } catch (e) { found = []; }
      el = found[0];
      if (!el) return '';
    }
    var at = f.attr || 'text';
    switch (at) {
      case 'text': return norm(el.innerText != null && el.innerText !== '' ? el.innerText : el.textContent);
      case 'html': return el.innerHTML;
      case 'outerhtml': return el.outerHTML;
      case 'value': return el.value != null ? el.value : '';
      case 'href': return el.href || el.getAttribute('href') || (el.closest && el.closest('a') ? el.closest('a').href : '') || '';
      case 'src': return el.currentSrc || el.src || el.getAttribute('src') || '';
      case 'checked': return !!el.checked;
      default: { var v = el.getAttribute(at); return v == null ? '' : v; }
    }
  }

  ops.extract_list = function (a) {
    var items = resolveAll(a);
    if (!items.length) fail('not_found', 'No items found for ' + a.selector);
    var names = uniqueKeys((a.fields || []).map(function (f, i) { return f.name || ('field' + (i + 1)); }));
    var rows = items.map(function (it) {
      var o = {};
      (a.fields || []).forEach(function (f, i) { o[names[i]] = fieldValue(it, f); });
      return o;
    });
    return { ok: true, rows: rows, count: rows.length };
  };

  ops.signature = function (a) {
    var items = resolveAll({ selector: a.item_selector });
    return { ok: true, sig: items.length + '|' + cut(items[0] ? items[0].textContent : '', 80) + '|' + cut(items.length ? items[items.length - 1].textContent : '', 40) };
  };

  /* ---------- whole page ---------- */

  function labelFor(el) {
    var t = textOfControl(el);
    if (!t) { var lb = el.getAttribute('aria-labelledby'); if (lb) t = norm(lb.split(/\s+/).map(function (id) { var n = document.getElementById(id); return n ? n.textContent : ''; }).join(' ')); }
    if (!t) t = el.getAttribute('placeholder') || el.title || el.name || el.id || '';
    return norm(t);
  }

  ops.extract_page = function (a) {
    var want = (a.kinds && a.kinds.length) ? a.kinds.map(lc) : ['headings', 'links', 'images', 'tables', 'lists', 'buttons', 'inputs', 'forms', 'text', 'meta'];
    function on(k) { return want.indexOf(k) >= 0; }
    var d = {}, q = function (s) { return Array.prototype.slice.call(document.querySelectorAll(s)); };
    if (on('headings')) d.headings = q('h1,h2,h3,h4,h5,h6,[role=heading]').map(function (h) { return { level: parseInt(h.tagName[1], 10) || parseInt(h.getAttribute('aria-level'), 10) || 0, text: cellText(h) }; }).filter(function (h) { return h.text; }).slice(0, 1000);
    if (on('links')) d.links = q('a[href]').slice(0, 3000).map(function (l) { return { text: cellText(l) || l.getAttribute('aria-label') || l.title || '', href: l.href }; });
    if (on('images')) d.images = q('img').slice(0, 1000).map(function (i) { return { alt: i.alt || '', src: i.currentSrc || i.src, width: i.naturalWidth, height: i.naturalHeight }; });
    if (on('tables')) d.tables = q('table,[role=table],[role=grid]').slice(0, 30).map(function (t, i) {
      var r = tableToRows(t, 'auto');
      return { index: i + 1, caption: t.caption ? cellText(t.caption) : '', columns: r.columns, rows: r.rows.slice(0, 1000) };
    });
    if (on('lists')) d.lists = q('ul,ol,dl').filter(function (l) { return !l.closest('nav') && l.children.length > 1; }).slice(0, 80).map(function (l) {
      return { type: l.tagName.toLowerCase(), items: Array.prototype.slice.call(l.children).slice(0, 300).map(cellText).filter(Boolean) };
    });
    if (on('buttons')) d.buttons = q('button,input[type=button],input[type=submit],input[type=reset],[role=button]').slice(0, 500).map(function (b) {
      return { text: cellText(b) || b.value || b.getAttribute('aria-label') || '', type: b.type || 'button', disabled: !!b.disabled };
    });
    if (on('inputs')) d.inputs = q('input:not([type=hidden]),select,textarea').slice(0, 800).map(function (i) {
      var o = { tag: i.tagName.toLowerCase(), type: kindOf(i), name: i.name || '', id: i.id || '', label: labelFor(i), placeholder: i.placeholder || '', required: !!i.required, value: i.type === 'password' ? '' : (i.value || '') };
      if (i.tagName === 'SELECT') o.options = Array.prototype.slice.call(i.options).slice(0, 200).map(function (op) { return norm(op.text); });
      return o;
    });
    if (on('forms')) d.forms = q('form').slice(0, 100).map(function (f, i) {
      return { index: i + 1, id: f.id || '', name: f.name || '', action: f.action || '', method: (f.method || 'get').toLowerCase(), fields: f.elements.length };
    });
    if (on('text')) d.text = q('p,blockquote,pre,li,figcaption').map(cellText).filter(function (t) { return t.length > 20; }).slice(0, 600);
    if (on('meta')) {
      var m = { title: document.title, url: location.href, lang: document.documentElement.lang || '' };
      q('meta[name],meta[property]').forEach(function (mt) { var k = mt.getAttribute('name') || mt.getAttribute('property'); if (k && mt.content) m[k] = mt.content; });
      var can = document.querySelector('link[rel=canonical]'); if (can) m.canonical = can.href;
      d.meta = m;
    }
    return { ok: true, data: d };
  };

  /* ---------- forms ---------- */

  function keyOf(s) { return String(s || '').toLowerCase().replace(/[^a-z0-9À-￿]+/g, ''); }

  ops.autofill_form = function (a) {
    var root = a.root ? find({ selector: a.root }) : document;
    var controls = Array.prototype.slice.call(root.querySelectorAll('input,select,textarea,[contenteditable=""],[contenteditable=true],[role=textbox],[role=combobox]'))
      .filter(function (c) { return !/^(hidden|submit|button|reset|image|file)$/i.test(c.type || '') && !c.disabled && visibleBasic(c); });
    var used = [], report = [];
    function keysFor(c) {
      var ks = [labelFor(c), c.name, c.id, c.getAttribute('placeholder'), c.getAttribute('aria-label'), c.title, c.getAttribute('data-testid')].map(keyOf).filter(Boolean);
      if (c.type === 'radio' && c.name) ks.push(keyOf(c.name));
      return ks;
    }
    var meta = controls.map(function (c) { return { el: c, keys: keysFor(c) }; });
    Object.keys(a.data).forEach(function (field) {
      var fk = keyOf(field), hit = null, i;
      if (fk) {
        for (i = 0; i < meta.length && !hit; i++) if (used.indexOf(meta[i].el) < 0 && meta[i].keys.indexOf(fk) >= 0) hit = meta[i];
        for (i = 0; i < meta.length && !hit; i++) if (used.indexOf(meta[i].el) < 0 && meta[i].keys.some(function (k) { return k.length > 2 && (k.indexOf(fk) >= 0 || (fk.length > 3 && fk.indexOf(k) >= 0)); })) hit = meta[i];
      }
      var rep = { field: field, matched: false, label: '' };
      if (hit) {
        try { var r = fillEl(hit.el, a.data[field], 'auto'); rep.matched = true; rep.label = labelFor(hit.el); rep.kind = r.kind; used.push(hit.el); if (hit.el.type === 'radio') radioGroup(hit.el).forEach(function (x) { used.push(x); }); }
        catch (e) { rep.error = e.message; rep.label = labelFor(hit.el); used.push(hit.el); }
      }
      report.push(rep);
    });
    if (a.submit) {
      var scope = (root === document ? (controls[0] && controls[0].closest('form')) : (root.tagName === 'FORM' ? root : root.closest('form') || root)) || document;
      var btn = scope.querySelector('button[type=submit],input[type=submit]') || scope.querySelector('button:not([type=button]):not([type=reset])');
      var form = scope.tagName === 'FORM' ? scope : (btn && btn.form);
      if (btn) btn.click(); else if (form) { if (form.requestSubmit) form.requestSubmit(); else form.submit(); } else fail('unsupported', 'No submit button found');
    }
    return { ok: true, report: report };
  };

  /* ---------- files & drag and drop ---------- */

  /* The file input that belongs to el: itself, one inside it, or the control of its <label>. With `nearby`, as a last
     resort also the first file input within a few ancestors (only a guess: used after the file-chooser route failed). */
  function nearFileInput(el, nearby) {
    if (el.tagName === 'INPUT' && el.type === 'file') return el;
    var f = el.querySelector && el.querySelector('input[type=file]'); if (f) return f;
    if (el.tagName === 'LABEL') { var c = controlOfLabel(el); if (c && c.type === 'file') return c; }
    if (!nearby) return null;
    var p = el;
    for (var i = 0; i < 3 && p && p.parentElement; i++) { p = p.parentElement; var g = p.querySelector('input[type=file]'); if (g) return g; }
    return null;
  }

  ops.target_kind = function (a) {
    var el = find(a), f = nearFileInput(el, false);
    var kind = el.tagName === 'INPUT' && el.type === 'file' ? 'file_input' : f ? 'has_file_input' : (/^(BUTTON|A)$/.test(el.tagName) || el.getAttribute('role') === 'button' || (el.tagName === 'INPUT' && /button|submit/.test(el.type))) ? 'button' : 'other';
    return { ok: true, kind: kind, multiple: !!(f && f.multiple), accept: (f && f.accept) || '' };
  };

  NS.handle = function (kind, a) {
    if (kind === 'file_input') return nearFileInput(find(a), false);
    if (kind === 'file_input_near') return nearFileInput(find(a), true);
    if (kind === 'element') return find(a);
    return null;
  };

  function b64File(f) {
    var bin = atob(f.b64), arr = new Uint8Array(bin.length);
    for (var i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);
    var type = f.type || 'application/octet-stream';
    try { return new File([arr], f.name, { type: type }); } catch (e) { /* engines without the File constructor */ }
    var b = new Blob([arr], { type: type });
    try { b.name = f.name; b.lastModified = Date.now(); } catch (e2) { /* ignore */ }
    return b;
  }

  function dragEvt(W, type, dt, x, y) {
    var ev;
    try { ev = new W.DragEvent(type, { bubbles: true, cancelable: true, composed: true, clientX: x, clientY: y, dataTransfer: dt, view: W }); if (!ev.dataTransfer) throw new Error('no dataTransfer'); }
    catch (e) { ev = mouseEvent(W, type, { bubbles: true, cancelable: true, clientX: x, clientY: y, view: W }); try { ev.dataTransfer = dt; } catch (e2) { try { Object.defineProperty(ev, 'dataTransfer', { value: dt }); } catch (e3) { /* ignore */ } } }
    return ev;
  }

  ops.drop_files = function (a) {
    var el = find(a), files = a.files.map(b64File), dt = newDT(files);
    var r = el.getBoundingClientRect(), x = r.left + r.width / 2, y = r.top + r.height / 2, W = win(el);
    ['dragenter', 'dragover', 'drop'].forEach(function (t) { el.dispatchEvent(dragEvt(W, t, dt, x, y)); });
    var inp = nearFileInput(el, false);                             /* many widgets only listen on their <input> */
    if (inp && (!inp.files || !inp.files.length)) {
      try { inp.files = dt.files; fire(inp, 'input'); fire(inp, 'change'); } catch (e) { /* ignore */ }
    }
    return { ok: true, count: files.length };
  };

  ops.html5_drag = function (a) {
    var src = find({ selector: a.selector }), dst = find({ selector: a.target }), dt = newDT(null), W = win(src);
    src.scrollIntoView({ block: 'center' });
    var s = src.getBoundingClientRect(), d = dst.getBoundingClientRect();
    var sx = s.left + s.width / 2, sy = s.top + s.height / 2, dx = d.left + d.width / 2, dy = d.top + d.height / 2;
    src.dispatchEvent(dragEvt(W, 'dragstart', dt, sx, sy));
    dst.dispatchEvent(dragEvt(W, 'dragenter', dt, dx, dy));
    dst.dispatchEvent(dragEvt(W, 'dragover', dt, dx, dy));
    dst.dispatchEvent(dragEvt(W, 'drop', dt, dx, dy));
    src.dispatchEvent(dragEvt(W, 'dragend', dt, dx, dy));
    return { ok: true };
  };

  /* ---------- CAPTCHA detection (never solving) ---------- */

  function bigVisible(el, minW, minH) { var r = el.getBoundingClientRect(); return visibleBasic(el) && r.width >= minW && r.height >= minH; }

  ops.captcha = function () {
    var i, f, frames = document.querySelectorAll('iframe'), found = null;
    function respEmpty(sel) { var n = document.querySelectorAll(sel); if (!n.length) return true; for (var k = 0; k < n.length; k++) if (n[k].value) return false; return true; }
    for (i = 0; i < frames.length && !found; i++) {
      f = frames[i];
      var src = f.src || '';
      if (/recaptcha\/(api2|enterprise)\/bframe/.test(src) && bigVisible(f, 200, 200) && getComputedStyle(f.parentElement.parentElement || f.parentElement).visibility !== 'hidden') found = 'reCAPTCHA image challenge';
      else if (/recaptcha\/(api2|enterprise)\/anchor/.test(src) && bigVisible(f, 100, 40) && respEmpty('textarea[name=g-recaptcha-response]')) found = 'reCAPTCHA "I\'m not a robot"';
      else if (/hcaptcha\.com/.test(src) && !/invisible/.test(src) && bigVisible(f, 80, 40) && respEmpty('[name=h-captcha-response]')) found = 'hCaptcha';
      else if (/challenges\.cloudflare\.com/.test(src) && bigVisible(f, 80, 30) && respEmpty('[name=cf-turnstile-response]')) found = 'Cloudflare Turnstile';
      else if (/arkoselabs|funcaptcha/.test(src) && bigVisible(f, 100, 100)) found = 'Arkose Labs challenge';
      else if (/captcha-delivery\.com/.test(src) && bigVisible(f, 100, 100)) found = 'DataDome challenge';
    }
    if (!found && (/^just a moment/i.test(document.title) || document.getElementById('challenge-form') || document.querySelector('#cf-challenge-running, .cf-browser-verification'))) found = 'Cloudflare verification page';
    if (!found && document.querySelector('.geetest_panel, .geetest_widget, .gt_popup')) found = 'GeeTest';
    if (!found && /\/sorry\/index/.test(location.href) && /google\./.test(location.hostname)) found = 'Google "unusual traffic" check';
    if (!found && document.body) {
      var t = document.body.innerText || '';
      if (t.length < 3000 && /(verify (that )?you are (a )?human|press (&|and) hold|i'?m not a robot|confirm you are human)/i.test(t)) found = 'human verification';
    }
    return { ok: true, found: !!found, kind: found || '' };
  };

  /* ============================================================ entry points */

  NS.version = 1;
  NS.ops = ops;
  NS.helpers = { norm: norm, lc: lc, cut: cut, kindOf: kindOf, visible: visibleBasic, rectOf: rectOf, labelFor: labelFor, cssEsc: cssEsc, attrSel: attrSel, splitChain: splitChain, fillEl: fillEl, tableToRows: tableToRows, nearFileInput: nearFileInput, frameOffset: frameOffset };

  NS.exec = function (op, args) {
    try {
      var fn = ops[op];
      if (!fn) return { ok: false, code: 'unknown_op', error: 'Unknown page operation: ' + op };
      var r = fn(args || {});
      return r && typeof r.then === 'function' ? r.then(null, errToResult) : r;
    } catch (e) { return errToResult(e); }
  };

  function errToResult(e) { return { ok: false, code: e.code || 'error', error: e && e.message ? e.message : String(e) }; }

  Object.defineProperty(window, '__fb', { value: NS, configurable: true, writable: false });
})();
