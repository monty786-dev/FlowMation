/* FlowBrowser legacy-engine layer ("Wine / Gecko build"). Injected into every page and frame BEFORE fb_runtime.js,
   only by hosts whose browser has no DevTools protocol (Wine's MSHTML on Android/Winlator).

   It does four things:
     1. tiny polyfills the page runtime relies on (Element.matches / closest);
     2. the page -> host transport: messages are queued and the host polls window.__fbPullTools();
     3. dialog policy for alert / confirm / prompt (there is no protocol to answer them);
     4. an emulation of the DevTools calls the runner uses - mouse, keyboard, text insertion, evaluate with
        object handles - as window.__fbLegacy.*; src/core/fb_legacy.c turns the runner's calls into these.

   Everything here is ES5 and must not use Promise, arrow functions, let/const or classes: Wine runs page scripts
   in its own JScript engine. Events are synthetic (isTrusted is false); that is the price of having no protocol. */
(function () {
  'use strict';
  var W = window, D = document;
  if (W.__fbLegacy) return;
  var L = {};
  L.version = 1;

  /* ------------------------------------------------------------ 1. polyfills (only when missing) */
  var EP = W.Element && W.Element.prototype;
  if (EP) {
    if (!EP.matches) {
      EP.matches = EP.msMatchesSelector || EP.webkitMatchesSelector || EP.mozMatchesSelector || function (s) {
        var all = (this.ownerDocument || D).querySelectorAll(s);
        for (var i = 0; i < all.length; i++) if (all[i] === this) return true;
        return false;
      };
    }
    if (!EP.closest) {
      EP.closest = function (s) {
        var e = this;
        while (e && e.nodeType === 1) { if (e.matches(s)) return e; e = e.parentNode; }
        return null;
      };
    }
  }

  /* ------------------------------------------------------------ 2. transport (page -> host is polled) */
  var outq = [];
  W.__fbPost = function (s) { outq.push(String(s)); };
  L.pull = function () {
    if (!outq.length) return '';
    var a = outq; outq = [];
    return '[' + a.join(',') + ']';
  };
  W.__fbPullTools = L.pull;

  /* ------------------------------------------------------------ 3. dialog policy */
  L.dialogs = 'manual';                       /* accept | dismiss | manual, set by the host */
  var oAlert = W.alert, oConfirm = W.confirm, oPrompt = W.prompt;
  function orig(fn, args) { try { return fn ? fn.apply(W, args) : undefined; } catch (e) { return undefined; } }
  try {
    W.alert = function (m) { if (L.dialogs === 'manual') return orig(oAlert, [m]); return undefined; };
    W.confirm = function (m) { if (L.dialogs === 'accept') return true; if (L.dialogs === 'dismiss') return false; return orig(oConfirm, [m]); };
    W.prompt = function (m, d) { if (L.dialogs === 'accept') return d == null ? '' : String(d); if (L.dialogs === 'dismiss') return null; return orig(oPrompt, [m, d]); };
  } catch (e) { /* read-only in this engine: dialogs stay manual */ }

  /* ------------------------------------------------------------ evaluate with handles and async results */
  var handles = {}, hn = 0, waits = {}, wn = 0, asyncOk = null;

  function keep(o) { hn++; handles[hn] = o; return hn; }
  L.handle = function (n) { return handles[n]; };
  L.drop = function (n) { delete handles[n]; return ''; };

  function canAsync() {
    if (asyncOk === null) { try { new Function('return (async function () {})'); asyncOk = true; } catch (e) { asyncOk = false; } }
    return asyncOk;
  }

  function failStr(e, code) {
    var name = (e && e.name) || 'Error', msg = e && e.message ? e.message : String(e);
    if (name === 'SyntaxError' && code && /\breturn\b/.test(code)) msg = 'Illegal return (' + msg + ')';
    return JSON.stringify({ ok: false, error: name + ': ' + msg });
  }

  function pack(r, byValue) {
    if (r === undefined) return '{"ok":true,"undef":true}';
    if (!byValue && r !== null && typeof r === 'object') return '{"ok":true,"handle":' + keep(r) + '}';
    var s;
    try { s = JSON.stringify(r); } catch (e) { s = JSON.stringify(String(r)); }
    if (s === undefined) return '{"ok":true,"undef":true}';
    return '{"ok":true,"value":' + s + '}';
  }

  /* code: the expression the runner would hand to Runtime.evaluate. Answers a JSON string. */
  L.run = function (code, byValue) {
    var r;
    try {
      if (!canAsync() && /^\(async function\(\)\{\n/.test(code)) code = code.replace(/^\(async function\(\)\{\n/, '(function(){\n');
      r = W.eval(code);
    } catch (e) { return failStr(e, code); }
    if (r && (typeof r === 'object' || typeof r === 'function') && typeof r.then === 'function') {
      var id = ++wn, w = waits[id] = { done: false, failed: false };
      r.then(function (v) { w.done = true; w.v = v; }, function (e) { w.done = true; w.failed = true; w.e = e; });
      return '{"ok":true,"pending":' + id + '}';
    }
    return pack(r, byValue);
  };

  L.poll = function (id, byValue) {
    var w = waits[id];
    if (!w) return '{"ok":false,"error":"The script result is no longer available"}';
    if (!w.done) return '{"ok":true,"pending":' + id + '}';
    delete waits[id];
    return w.failed ? failStr(w.e, '') : pack(w.v, byValue);
  };

  /* ------------------------------------------------------------ helpers shared by mouse / keyboard */
  function winOf(el) { return (el.ownerDocument && el.ownerDocument.defaultView) || W; }

  function deepAt(doc, x, y) {
    var e = doc.elementFromPoint(x, y), guard = 0;
    while (e && guard++ < 12) {
      if (e.shadowRoot && e.shadowRoot.elementFromPoint) {
        var s = e.shadowRoot.elementFromPoint(x, y);
        if (s && s !== e) { e = s; continue; }
      }
      if (e.tagName === 'IFRAME' || e.tagName === 'FRAME') {
        var cd = null;
        try { cd = e.contentDocument; } catch (err) { cd = null; }
        if (!cd) break;
        var r = e.getBoundingClientRect(), cs = winOf(e).getComputedStyle(e);
        var nx = x - r.left - e.clientLeft - (parseFloat(cs.paddingLeft) || 0), ny = y - r.top - e.clientTop - (parseFloat(cs.paddingTop) || 0);
        var inner = cd.elementFromPoint(nx, ny);
        if (!inner) break;
        e = inner; x = nx; y = ny; doc = cd;
        continue;
      }
      break;
    }
    return e ? { el: e, x: x, y: y } : null;
  }

  function activeDeep() {
    var a = D.activeElement, guard = 0;
    while (a && guard++ < 12) {
      if (a.tagName === 'IFRAME' || a.tagName === 'FRAME') {
        var cd = null;
        try { cd = a.contentDocument; } catch (e) { cd = null; }
        if (!cd || !cd.activeElement || cd.activeElement === cd.body && !cd.hasFocus()) break;
        a = cd.activeElement;
      } else if (a.shadowRoot && a.shadowRoot.activeElement) a = a.shadowRoot.activeElement;
      else break;
    }
    return a;
  }

  function mkEvent(el, type, bubbles, cancelable) {
    var d = el.ownerDocument || D, e = d.createEvent('Event');
    e.initEvent(type, bubbles, cancelable);
    return e;
  }

  function mkMouse(el, type, x, y, o) {
    var w = winOf(el), d = el.ownerDocument || D, e;
    o = o || {};
    var init = { bubbles: o.noBubble ? false : true, cancelable: true, view: w, detail: o.detail || 0, screenX: x, screenY: y, clientX: x, clientY: y,
                 ctrlKey: !!o.ctrl, altKey: !!o.alt, shiftKey: !!o.shift, metaKey: !!o.meta, button: o.button || 0, buttons: o.buttons || 0, relatedTarget: o.related || null };
    try { e = new w.MouseEvent(type, init); }
    catch (err) {
      try {
        e = d.createEvent('MouseEvents');
        e.initMouseEvent(type, init.bubbles, true, w, init.detail, x, y, x, y, init.ctrlKey, init.altKey, init.shiftKey, init.metaKey, init.button, init.relatedTarget);
      } catch (err2) { e = mkEvent(el, type, init.bubbles, true); e.clientX = x; e.clientY = y; e.button = init.button; }
    }
    return e;
  }

  function mkKey(el, type, o) {
    var w = winOf(el), d = el.ownerDocument || D, e;
    var init = { bubbles: true, cancelable: true, view: w, key: o.key, code: o.code, ctrlKey: !!o.ctrl, altKey: !!o.alt, shiftKey: !!o.shift, metaKey: !!o.meta,
                 keyCode: o.vk, which: o.vk, charCode: type === 'keypress' ? (o.key && o.key.length === 1 ? o.key.charCodeAt(0) : 0) : 0 };
    try { e = new w.KeyboardEvent(type, init); }
    catch (err) { e = mkEvent(el, type, true, true); }
    /* some engines ignore keyCode / which / charCode in the constructor: define them when they differ */
    try { if (e.keyCode !== init.keyCode) Object.defineProperty(e, 'keyCode', { get: function () { return init.keyCode; } }); } catch (e1) { /* ignore */ }
    try { if (e.which !== init.which) Object.defineProperty(e, 'which', { get: function () { return init.which; } }); } catch (e2) { /* ignore */ }
    try { if (e.key !== init.key) Object.defineProperty(e, 'key', { get: function () { return init.key; } }); } catch (e3) { /* ignore */ }
    try { if (e.code !== init.code) Object.defineProperty(e, 'code', { get: function () { return init.code; } }); } catch (e4) { /* ignore */ }
    try { if (e.ctrlKey !== init.ctrlKey) Object.defineProperty(e, 'ctrlKey', { get: function () { return init.ctrlKey; } }); } catch (e5) { /* ignore */ }
    try { if (e.shiftKey !== init.shiftKey) Object.defineProperty(e, 'shiftKey', { get: function () { return init.shiftKey; } }); } catch (e6) { /* ignore */ }
    try { if (e.altKey !== init.altKey) Object.defineProperty(e, 'altKey', { get: function () { return init.altKey; } }); } catch (e7) { /* ignore */ }
    return e;
  }

  /* true when nobody called preventDefault */
  function send(el, ev) { try { return el.dispatchEvent(ev); } catch (e) { return true; } }

  function setNative(el, v) {
    var w = winOf(el), proto = el.tagName === 'TEXTAREA' ? w.HTMLTextAreaElement.prototype : w.HTMLInputElement.prototype;
    var d = Object.getOwnPropertyDescriptor ? Object.getOwnPropertyDescriptor(proto, 'value') : null;
    if (d && d.set) d.set.call(el, v); else el.value = v;
  }

  function fireInput(el, data, type) {
    var w = winOf(el), e;
    try { e = new w.InputEvent('input', { bubbles: true, cancelable: false, data: data == null ? null : data, inputType: type || 'insertText' }); }
    catch (err) { e = mkEvent(el, 'input', true, false); }
    send(el, e);
  }

  function fireChange(el) { send(el, mkEvent(el, 'change', true, false)); }

  function isTextControl(el) {
    if (!el) return false;
    if (el.tagName === 'TEXTAREA') return true;
    return el.tagName === 'INPUT' && /^(text|search|url|tel|email|password|number|)$/i.test(el.type || '');
  }

  function caret(el) {
    var s = null, e = null;
    try { s = el.selectionStart; e = el.selectionEnd; } catch (x) { s = e = null; }
    var len = String(el.value).length;
    if (el.__fbAll) return { s: 0, e: len };           /* select-all on a field whose selection cannot be read (number, email) */
    if (s == null || e == null) return { s: len, e: len };
    return { s: s, e: e };
  }

  function setCaret(el, p) { try { el.setSelectionRange(p, p); } catch (e) { /* number / email */ } }

  function insertText(el, text) {
    if (isTextControl(el)) {
      if (el.readOnly || el.disabled) return false;
      var v = String(el.value), c = caret(el), nv = v.slice(0, c.s) + text + v.slice(c.e);
      if (el.maxLength > 0 && nv.length > el.maxLength) nv = nv.slice(0, el.maxLength);
      setNative(el, nv);
      el.__fbAll = false;
      setCaret(el, Math.min(c.s + text.length, nv.length));
      fireInput(el, text);
      return true;
    }
    if (el && el.isContentEditable) {
      var ok = false;
      try { ok = D.execCommand('insertText', false, text); } catch (e) { ok = false; }
      if (!ok) {
        try {
          var sel = winOf(el).getSelection(), rg = sel.rangeCount ? sel.getRangeAt(0) : null;
          if (rg) { rg.deleteContents(); var tn = (el.ownerDocument || D).createTextNode(text); rg.insertNode(tn); rg.setStartAfter(tn); rg.collapse(true); sel.removeAllRanges(); sel.addRange(rg); }
          else el.appendChild((el.ownerDocument || D).createTextNode(text));
          fireInput(el, text);
        } catch (e2) { return false; }
      }
      return true;
    }
    return false;
  }

  function deleteText(el, forward) {
    if (isTextControl(el)) {
      if (el.readOnly || el.disabled) return;
      var v = String(el.value), c = caret(el), s = c.s, e = c.e;
      if (s === e) { if (forward) e = Math.min(v.length, e + 1); else s = Math.max(0, s - 1); }
      if (s === e) return;
      setNative(el, v.slice(0, s) + v.slice(e));
      el.__fbAll = false;
      setCaret(el, s);
      fireInput(el, null, forward ? 'deleteContentForward' : 'deleteContentBackward');
    } else if (el && el.isContentEditable) {
      try { D.execCommand(forward ? 'forwardDelete' : 'delete', false, null); } catch (e) { /* ignore */ }
    }
  }

  function isFocusable(e) {
    if (!e || e.nodeType !== 1 || e.disabled) return false;
    var t = e.tagName;
    if (t === 'INPUT') return e.type !== 'hidden';
    if (t === 'SELECT' || t === 'TEXTAREA' || t === 'BUTTON' || t === 'SUMMARY') return true;
    if (t === 'A' || t === 'AREA') return e.hasAttribute('href');
    if (e.isContentEditable && e.getAttribute('contenteditable') !== null) return true;
    var ti = e.getAttribute('tabindex');
    return ti !== null && ti !== '' && !isNaN(parseInt(ti, 10));
  }

  function focusTarget(el) {
    var e = el;
    while (e && e.nodeType === 1) { if (isFocusable(e)) return e; e = e.parentNode || (e.getRootNode && e.getRootNode().host); }
    return null;
  }

  function tabbables(doc) {
    var all = doc.querySelectorAll('input,select,textarea,button,a[href],area[href],summary,[tabindex],[contenteditable]'), out = [], i;
    for (i = 0; i < all.length; i++) {
      var e = all[i], ti = e.getAttribute('tabindex');
      if (!isFocusable(e)) continue;
      if (ti !== null && parseInt(ti, 10) < 0) continue;
      var r = e.getClientRects();
      if (!r.length) continue;
      out.push(e);
    }
    return out;
  }

  /* ------------------------------------------------------------ mouse */
  var M = { over: null, down: null, downBtn: 0, downX: 0, downY: 0, src: null, drag: false, dragTarget: null, dt: null, dragCancelled: false };

  function makeDT() {
    try { return new W.DataTransfer(); } catch (e) { /* no constructor */ }
    var data = {}, types = [];
    return {
      effectAllowed: 'uninitialized', dropEffect: 'none', files: [], items: [], types: types,
      setData: function (t, v) { data[t] = String(v); if (types.indexOf(t) < 0) types.push(t); },
      getData: function (t) { return data[t] === undefined ? '' : data[t]; },
      clearData: function (t) { if (t) delete data[t]; else { data = {}; types.length = 0; } },
      setDragImage: function () { /* nothing to draw */ }
    };
  }

  function dragEv(el, type, dt, x, y) {
    var w = winOf(el), e;
    try { e = new w.DragEvent(type, { bubbles: true, cancelable: true, view: w, clientX: x, clientY: y, dataTransfer: dt }); if (e.dataTransfer) return e; } catch (err) { /* fall through */ }
    e = mkMouse(el, type, x, y, {});
    try { e.dataTransfer = dt; } catch (e2) { try { Object.defineProperty(e, 'dataTransfer', { value: dt }); } catch (e3) { /* ignore */ } }
    return e;
  }

  function draggableOf(el) {
    var e = el;
    while (e && e.nodeType === 1) {
      var d = e.getAttribute('draggable');
      if (d === 'true') return e;
      if (d === null && (e.tagName === 'IMG' || (e.tagName === 'A' && e.hasAttribute('href')))) return e;
      e = e.parentNode;
    }
    return null;
  }

  function hover(hit) {
    var el = hit.el;
    if (el === M.over) return;
    var old = M.over;
    M.over = el;
    if (old) {
      send(old, mkMouse(old, 'mouseout', hit.x, hit.y, { related: el }));
      send(old, mkMouse(old, 'mouseleave', hit.x, hit.y, { related: el, noBubble: true }));
    }
    send(el, mkMouse(el, 'mouseover', hit.x, hit.y, { related: old }));
    send(el, mkMouse(el, 'mouseenter', hit.x, hit.y, { related: old, noBubble: true }));
  }

  function clickNow(el, hit) {
    if (typeof el.click === 'function') el.click();
    else send(el, mkMouse(el, 'click', hit.x, hit.y, { detail: 1 }));
  }
  /* While the host intercepts the file chooser, a click may open a modal OS dialog. The next click then runs from a timer
     (once), so the call that asked for the click returns first instead of sitting inside the dialog's message loop. */
  L.deferClicks = false;
  L.setDefer = function (on) { L.deferClicks = !!on; return ''; };
  function leftClick(el, hit) {
    if (L.deferClicks) {
      L.deferClicks = false;
      setTimeout(function () { try { clickNow(el, hit); } catch (e) { /* the page threw: nothing to report */ } }, 0);
    } else clickNow(el, hit);
  }

  /* x, y: viewport coordinates of the top window, exactly what the runtime's "rect" op answers */
  L.mouse = function (type, x, y, button, clicks) {
    var hit = deepAt(D, x, y);
    if (!hit) return '';
    var el = hit.el, bi = button === 'right' ? 2 : button === 'middle' ? 1 : 0, bs = button === 'right' ? 2 : button === 'middle' ? 4 : 1;
    if (type === 'mouseMoved') {
      hover(hit);
      send(el, mkMouse(el, 'mousemove', hit.x, hit.y, { buttons: M.down ? 1 : 0 }));
      if (M.down && M.src && !M.drag && (Math.abs(x - M.downX) + Math.abs(y - M.downY) > 3)) {
        M.dt = makeDT();
        M.drag = send(M.src, dragEv(M.src, 'dragstart', M.dt, hit.x, hit.y));
        M.dragTarget = null;
      }
      if (M.drag) {
        send(M.src, dragEv(M.src, 'drag', M.dt, hit.x, hit.y));
        if (M.dragTarget && M.dragTarget !== el) send(M.dragTarget, dragEv(M.dragTarget, 'dragleave', M.dt, hit.x, hit.y));
        if (M.dragTarget !== el) send(el, dragEv(el, 'dragenter', M.dt, hit.x, hit.y));
        M.dragTarget = el;
        M.dragCancelled = !send(el, dragEv(el, 'dragover', M.dt, hit.x, hit.y));    /* preventDefault on dragover = "drop allowed" */
      }
      return '';
    }
    if (type === 'mousePressed') {
      hover(hit);
      var notPrevented = send(el, mkMouse(el, 'mousedown', hit.x, hit.y, { button: bi, buttons: bs, detail: clicks || 1 }));
      M.down = el; M.downBtn = bi; M.downX = x; M.downY = y; M.drag = false; M.dragTarget = null;
      M.src = bi === 0 ? draggableOf(el) : null;
      if (notPrevented && bi === 0) {
        var ft = focusTarget(el), cur = activeDeep();
        if (ft) {
          if (ft !== cur) {
            ft.focus();
            if (isTextControl(ft)) setCaret(ft, String(ft.value).length);
          }
        } else if (cur && cur !== D.body && cur.blur) { try { cur.blur(); } catch (e) { /* ignore */ } }
      }
      return '';
    }
    if (type === 'mouseReleased') {
      send(el, mkMouse(el, 'mouseup', hit.x, hit.y, { button: bi, buttons: 0, detail: clicks || 1 }));
      var downEl = M.down, wasDrag = M.drag;
      if (wasDrag) {
        if (M.dragCancelled) send(el, dragEv(el, 'drop', M.dt, hit.x, hit.y));
        send(M.src, dragEv(M.src, 'dragend', M.dt, hit.x, hit.y));
      } else if (downEl && (downEl === el || downEl.contains(el) || el.contains(downEl))) {
        var tgt = downEl.contains(el) ? downEl : el;               /* the click goes to the common ancestor */
        if (bi === 2) send(tgt, mkMouse(tgt, 'contextmenu', hit.x, hit.y, { button: 2, buttons: 2 }));
        else if (bi === 1) send(tgt, mkMouse(tgt, 'auxclick', hit.x, hit.y, { button: 1 }));
        else leftClick(tgt, hit);
        /* the runner presses / releases once per click: a second release with clickCount 2 is the double click */
        if (bi === 0 && clicks === 2) send(tgt, mkMouse(tgt, 'dblclick', hit.x, hit.y, { detail: 2 }));
      }
      M.down = null; M.src = null; M.drag = false; M.dt = null; M.dragTarget = null;
      return '';
    }
    return '';
  };

  /* ------------------------------------------------------------ keyboard */
  function submitForm(f) {
    var btn = f.querySelector('input[type=submit],button[type=submit],button:not([type]),input[type=image]');
    if (btn && !btn.disabled) { btn.click(); return; }
    var n = 0, i, els = f.elements;
    for (i = 0; i < els.length; i++) if (els[i].tagName === 'INPUT' && /^(text|search|url|tel|email|password|number|)$/i.test(els[i].type || '')) n++;
    if (n > 1) return;                                          /* implicit submission needs a button when there are several fields */
    var ok = send(f, mkEvent(f, 'submit', true, true));
    if (ok) { try { f.submit(); } catch (e) { /* ignore */ } }
  }

  function enterKey(el) {
    var t = el.tagName;
    if (t === 'TEXTAREA') { insertText(el, '\n'); return; }
    if (el.isContentEditable && t !== 'INPUT') {
      var ok = false;
      try { ok = D.execCommand('insertParagraph', false, null); } catch (e) { ok = false; }
      if (!ok) insertText(el, '\n');
      return;
    }
    if (t === 'A' || t === 'BUTTON' || t === 'SUMMARY' || el.getAttribute('role') === 'button' || el.getAttribute('role') === 'link') { el.click(); return; }
    if (t === 'INPUT') {
      if (/^(button|submit|reset|image|file)$/i.test(el.type)) { el.click(); return; }
      if (/^(checkbox|radio)$/i.test(el.type)) return;
      if (el.form) submitForm(el.form);
    }
  }

  function moveFocus(el, back) {
    var list = tabbables(el.ownerDocument || D), i = list.indexOf(el);
    var next = null;
    if (i < 0) next = back ? list[list.length - 1] : list[0];
    else next = list[i + (back ? -1 : 1)];
    if (next) { next.focus(); if (isTextControl(next) && next.select) { try { next.select(); } catch (e) { /* ignore */ } } }
    else if (el.blur) el.blur();
  }

  function radioStep(el, dir) {
    var root = el.form || el.ownerDocument, n = el.name, all = root.querySelectorAll('input[type=radio]'), g = [], i;
    for (i = 0; i < all.length; i++) if (all[i].name === n && !all[i].disabled) g.push(all[i]);
    var at = g.indexOf(el);
    if (at < 0 || g.length < 2) return;
    var nx = g[(at + dir + g.length) % g.length];
    nx.focus(); nx.click();
  }

  function arrowKey(el, key, o) {
    if (isTextControl(el)) {
      var c = caret(el), len = String(el.value).length, p = c.s === c.e ? c.s : (key === 'ArrowLeft' || key === 'Home' ? c.s : c.e);
      if (key === 'ArrowLeft') p = c.s === c.e ? Math.max(0, p - 1) : c.s;
      else if (key === 'ArrowRight') p = c.s === c.e ? Math.min(len, p + 1) : c.e;
      else if (key === 'Home' || key === 'ArrowUp') p = 0;
      else if (key === 'End' || key === 'ArrowDown') p = len;
      el.__fbAll = false;
      setCaret(el, p);
      return true;
    }
    if (el.tagName === 'SELECT') {
      var d = (key === 'ArrowDown' || key === 'ArrowRight') ? 1 : (key === 'ArrowUp' || key === 'ArrowLeft') ? -1 : 0;
      if (key === 'Home') el.selectedIndex = 0;
      else if (key === 'End') el.selectedIndex = el.options.length - 1;
      else if (d) el.selectedIndex = Math.max(0, Math.min(el.options.length - 1, el.selectedIndex + d));
      else return false;
      fireInput(el, null, 'insertReplacementText');
      fireChange(el);
      return true;
    }
    if (el.tagName === 'INPUT' && el.type === 'radio') {
      if (key === 'ArrowDown' || key === 'ArrowRight') { radioStep(el, 1); return true; }
      if (key === 'ArrowUp' || key === 'ArrowLeft') { radioStep(el, -1); return true; }
    }
    if (el.tagName === 'INPUT' && el.type === 'range') {
      var st = parseFloat(el.step) || 1, v = parseFloat(el.value) || 0;
      if (key === 'ArrowRight' || key === 'ArrowUp') v += st; else if (key === 'ArrowLeft' || key === 'ArrowDown') v -= st; else if (key === 'Home') v = parseFloat(el.min || 0); else if (key === 'End') v = parseFloat(el.max || 100);
      el.value = v; fireInput(el, null, 'insertReplacementText'); fireChange(el);
      return true;
    }
    return false;
  }

  function scrollKey(key) {
    var w = W, h = w.innerHeight || 600;
    if (key === 'PageDown' || key === ' ') w.scrollBy(0, h * 0.9);
    else if (key === 'PageUp') w.scrollBy(0, -h * 0.9);
    else if (key === 'ArrowDown') w.scrollBy(0, 40);
    else if (key === 'ArrowUp') w.scrollBy(0, -40);
    else if (key === 'Home') w.scrollTo(0, 0);
    else if (key === 'End') w.scrollTo(0, (D.documentElement.scrollHeight || D.body.scrollHeight));
  }

  /* type: keyDown | rawKeyDown | keyUp (CDP names). mods: 1 alt, 2 ctrl, 4 meta, 8 shift. text: what a printable key types. */
  L.key = function (type, key, code, vk, mods, text, cmd) {
    var el = activeDeep() || D.body || D.documentElement;
    var o = { key: key, code: code, vk: vk, ctrl: !!(mods & 2), alt: !!(mods & 1), shift: !!(mods & 8), meta: !!(mods & 4) };
    if (type === 'keyUp') {
      var allowedUp = send(el, mkKey(el, 'keyup', o));
      if (allowedUp && key === ' ' && !isTextControl(el) && !el.isContentEditable && el.tagName !== 'SELECT') {
        var t = el.tagName;
        if (t === 'BUTTON' || t === 'SUMMARY' || el.getAttribute('role') === 'button' || (t === 'INPUT' && /^(button|submit|reset|checkbox|radio|image)$/i.test(el.type))) el.click();
      }
      return '';
    }
    if (!send(el, mkKey(el, 'keydown', o))) return '';
    if (cmd === 'selectAll' || (o.ctrl && !o.alt && key.toLowerCase() === 'a')) {
      if (isTextControl(el)) { try { el.select(); } catch (e) { /* ignore */ } el.__fbAll = true; }
      else { try { D.execCommand('selectAll', false, null); } catch (e2) { /* ignore */ } }
      return '';
    }
    if (o.ctrl || o.meta) return '';                              /* copy / paste / undo are not available without a clipboard protocol */
    if (key === 'Enter') { send(el, mkKey(el, 'keypress', o)); enterKey(el); return ''; }
    if (key === 'Tab') { moveFocus(el, o.shift); return ''; }
    if (key === 'Backspace' || key === 'Delete') { deleteText(el, key === 'Delete'); return ''; }
    if (key === 'Escape') return '';
    if (/^[A-Za-z][A-Za-z0-9]+$/.test(key) && key !== 'Space') {   /* named keys: arrows, Home, End, Page keys, F-keys */
      if (!arrowKey(el, key, o) && (el === D.body || el === D.documentElement)) scrollKey(key);
      return '';
    }
    var ch = text || (key === 'Space' ? ' ' : key);
    if (ch) {
      if (send(el, mkKey(el, 'keypress', { key: ch, code: code, vk: ch.charCodeAt(0), ctrl: false, alt: false, shift: o.shift }))) {
        if (!insertText(el, ch) && ch === ' ' && (el === D.body || el === D.documentElement)) scrollKey(' ');
      }
    }
    return '';
  };

  /* Input.insertText */
  L.insert = function (text) {
    var el = activeDeep();
    if (!el || !insertText(el, String(text))) return 'nofocus';
    return '';
  };

  /* ------------------------------------------------------------ helpers the host can ask for */
  L.fileInfo = function (n) {                          /* what an <input type=file> holds, to verify an upload */
    var el = handles[n];
    if (!el) return JSON.stringify({ ok: false });
    var names = [], i;
    try { for (i = 0; el.files && i < el.files.length; i++) names.push(el.files[i].name); } catch (e) { /* ignore */ }
    return JSON.stringify({ ok: true, value: String(el.value || ''), files: names });
  };


  try { Object.defineProperty(W, '__fbLegacy', { value: L, configurable: true, writable: false }); }
  catch (e) { W.__fbLegacy = L; }
})();
