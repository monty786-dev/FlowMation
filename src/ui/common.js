/* FlowBrowser UI - shared helpers: host bridge, DOM builder, toasts, modals.
   The UI talks to the native host with JSON strings through chrome.webview.postMessage:
     UI -> host   {id, cmd, ...args}           host -> UI  {id, ok, ...result}   (reply to a request)
                                                          {ev, ...}               (event, no id)                       */
(function () {
  'use strict';
  var wv = window.chrome && window.chrome.webview;
  var nextId = 1, pending = {}, listeners = {};

  function post(o) {
    var s = JSON.stringify(o);
    if (wv) wv.postMessage(s);
    else if (window.__fbMockPost) window.__fbMockPost(s);
  }

  function dispatch(m) {
    if (typeof m === 'string') { try { m = JSON.parse(m); } catch (e) { return; } }
    if (!m || typeof m !== 'object') return;
    if (m.id != null && pending[m.id]) {
      var p = pending[m.id]; delete pending[m.id]; clearTimeout(p.t);
      if (m.ok === false) p.rej(new Error(m.error || 'failed')); else p.res(m);
      return;
    }
    if (m.ev) (listeners[m.ev] || []).slice().forEach(function (f) { try { f(m); } catch (e) { console.error(e); } });
  }
  if (wv) wv.addEventListener('message', function (e) { dispatch(e.data); });
  window.__fbDeliver = dispatch;

  window.Host = {
    /* request with a reply; rejects on error / timeout */
    call: function (cmd, data, timeout) {
      return new Promise(function (res, rej) {
        var id = nextId++, o = { id: id, cmd: cmd };
        if (data) for (var k in data) o[k] = data[k];
        pending[id] = { res: res, rej: rej, t: timeout === 0 ? 0 : setTimeout(function () { delete pending[id]; rej(new Error('The app did not answer (' + cmd + ')')); }, timeout || 60000) };
        post(o);
      });
    },
    send: function (cmd, data) { var o = { cmd: cmd }; if (data) for (var k in data) o[k] = data[k]; post(o); },
    on: function (ev, fn) { (listeners[ev] = listeners[ev] || []).push(fn); },
    available: !!wv
  };

  /* ---------- tiny DOM builder: h('div.cls#id', {attr}, child, ...) ---------- */
  window.h = function (sel, attrs) {
    var m = /^([a-z0-9]*)((?:[.#][\w-]+)*)$/i.exec(sel) || [], el = document.createElement(m[1] || 'div');
    (m[2] || '').replace(/([.#])([\w-]+)/g, function (_, t, n) { if (t === '.') el.classList.add(n); else el.id = n; });
    var i = 1;
    if (attrs && typeof attrs === 'object' && !(attrs instanceof Node) && !Array.isArray(attrs)) {
      i = 2;
      for (var k in attrs) {
        var v = attrs[k];
        if (v == null || v === false) continue;
        if (k === 'text') el.textContent = v;
        else if (k === 'html') el.innerHTML = v;
        else if (k.slice(0, 2) === 'on') el.addEventListener(k.slice(2), v);
        else if (k === 'value') el.value = v;
        else if (k === 'checked' || k === 'disabled' || k === 'selected') el[k] = !!v;
        else if (k === 'style' && typeof v === 'object') Object.assign(el.style, v);
        else el.setAttribute(k, v === true ? '' : v);
      }
    }
    for (; i < arguments.length; i++) add(el, arguments[i]);
    return el;
  };
  function add(el, c) {
    if (c == null || c === false) return;
    if (Array.isArray(c)) c.forEach(function (x) { add(el, x); });
    else el.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
  window.clear = function (el) { while (el.firstChild) el.removeChild(el.firstChild); return el; };
  window.$ = function (s, root) { return (root || document).querySelector(s); };
  window.$$ = function (s, root) { return Array.prototype.slice.call((root || document).querySelectorAll(s)); };

  var tbox = null;
  window.toast = function (msg, kind, ms) {
    if (!tbox) { tbox = h('div#toasts'); document.body.appendChild(tbox); }
    var t = h('div.toast' + (kind ? '.' + kind : ''), { text: msg });
    tbox.appendChild(t);
    setTimeout(function () { t.remove(); }, ms || (kind === 'err' ? 7000 : 3200));
  };

  /* ---------- modal: ui.modal({title, body, buttons:[{label,primary,value}]}) -> Promise(value) ---------- */
  window.modal = function (o) {
    return new Promise(function (resolve) {
      var bg = h('div.modal-bg'), done = false;
      function close(v) { if (done) return; done = true; bg.remove(); document.removeEventListener('keydown', key, true); resolve(v); }
      function key(e) { if (e.key === 'Escape') { e.stopPropagation(); close(undefined); } }
      var foot = h('footer');
      (o.buttons || [{ label: 'OK', primary: true, value: true }]).forEach(function (b) {
        foot.appendChild(h('button.btn' + (b.primary ? '.primary' : '') + (b.danger ? '.danger' : ''), { text: b.label, onclick: function () { close(b.get ? b.get() : b.value); } }));
      });
      var m = h('div.modal', { style: o.width ? { width: o.width } : null }, h('header', { text: o.title || '' }), h('div.body', o.body), foot);
      bg.appendChild(m);
      bg.addEventListener('mousedown', function (e) { if (e.target === bg) close(undefined); });
      document.addEventListener('keydown', key, true);
      document.body.appendChild(bg);
      if (o.setup) o.setup(close);
      var f = $('input,textarea,select', m) || $('.btn.primary', m);
      if (f) setTimeout(function () { f.focus(); if (f.select && f.tagName !== 'SELECT') f.select(); }, 20);
    });
  };
  window.confirmBox = function (msg, okLabel, danger) {
    return modal({ title: 'Please confirm', body: h('div', { text: msg }), buttons: [{ label: 'Cancel', value: false }, { label: okLabel || 'OK', primary: !danger, danger: danger, value: true }] });
  };
  window.promptBox = function (title, def, hint) {
    var inp = h('input', { type: 'text', value: def || '' });
    return modal({ title: title, body: [hint ? h('p.muted.small', { text: hint }) : null, inp], buttons: [{ label: 'Cancel', value: null }, { label: 'OK', primary: true, get: function () { return inp.value; } }] });
  };

  window.applyTheme = function (t) { document.documentElement.setAttribute('data-theme', t || 'light'); };
  window.fmtMs = function (ms) { return ms < 1000 ? ms + ' ms' : (ms / 1000).toFixed(ms < 10000 ? 2 : 1) + ' s'; };
  window.cloneJ = function (o) { return o === undefined ? undefined : JSON.parse(JSON.stringify(o)); };
  window.debounce = function (fn, ms) { var t; return function () { var a = arguments, s = this; clearTimeout(t); t = setTimeout(function () { fn.apply(s, a); }, ms); }; };
  window.basename = function (p) { return String(p || '').split(/[\\/]/).pop(); };
})();
