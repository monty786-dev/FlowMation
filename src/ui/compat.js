/* FlowBrowser UI - compatibility layer for old browser engines (Wine's MSHTML / Gecko under Winlator).
   Loaded first by every UI page. Everything is added only when the engine lacks it, so on Chromium / WebView2 this file
   does nothing. The UI code itself is plain ES5 (checked by tests/lint_es5.js); this adds the library calls it uses. */
(function () {
  'use strict';
  var W = window, D = document;
  function def(obj, name, fn) {
    if (obj && !obj[name]) { try { Object.defineProperty(obj, name, { value: fn, writable: true, configurable: true }); } catch (e) { obj[name] = fn; } }
  }

  /* ---------------------------------------------------------------- Promise */
  (function () {
    var NativeOk = typeof W.Promise === 'function' && W.Promise.resolve && W.Promise.reject && W.Promise.all && W.Promise.prototype && W.Promise.prototype.then;
    if (NativeOk) {
      def(W.Promise.prototype, 'finally', function (f) {
        var C = this.constructor || W.Promise;
        return this.then(function (v) { return C.resolve(f()).then(function () { return v; }); }, function (e) { return C.resolve(f()).then(function () { throw e; }); });
      });
      return;
    }
    function flush(p) {
      if (!p._q.length) return;
      var q = p._q; p._q = [];
      setTimeout(function () { for (var i = 0; i < q.length; i++) run(q[i], p); }, 0);
    }
    function run(h, p) {
      var cb = p._s === 1 ? h.ok : h.bad;
      if (typeof cb !== 'function') { (p._s === 1 ? h.res : h.rej)(p._v); return; }
      try { h.res(cb(p._v)); } catch (e) { h.rej(e); }
    }
    function fail(p, e) { p._s = 2; p._v = e; flush(p); }
    function settle(p, v) {
      if (v === p) { fail(p, new TypeError('A promise cannot be resolved with itself')); return; }
      if (v && (typeof v === 'object' || typeof v === 'function')) {
        var then;
        try { then = v.then; } catch (e) { fail(p, e); return; }
        if (typeof then === 'function') {
          var called = false;
          try {
            then.call(v, function (x) { if (!called) { called = true; settle(p, x); } }, function (e) { if (!called) { called = true; fail(p, e); } });
          } catch (e2) { if (!called) { called = true; fail(p, e2); } }
          return;
        }
      }
      p._s = 1; p._v = v; flush(p);
    }
    function P(fn) {
      if (!(this instanceof P)) throw new TypeError('Promise must be called with new');
      var self = this, done = false;
      self._s = 0; self._v = undefined; self._q = [];
      try {
        fn(function (v) { if (!done) { done = true; settle(self, v); } }, function (e) { if (!done) { done = true; fail(self, e); } });
      } catch (e) { if (!done) { done = true; fail(self, e); } }
    }
    P.prototype.then = function (ok, bad) {
      var self = this;
      return new P(function (res, rej) {
        self._q.push({ ok: ok, bad: bad, res: res, rej: rej });
        if (self._s) flush(self);
      });
    };
    P.prototype['catch'] = function (f) { return this.then(undefined, f); };
    P.prototype['finally'] = function (f) {
      return this.then(function (v) { return P.resolve(f()).then(function () { return v; }); }, function (e) { return P.resolve(f()).then(function () { throw e; }); });
    };
    P.resolve = function (v) { return v instanceof P ? v : new P(function (res) { res(v); }); };
    P.reject = function (e) { return new P(function (res, rej) { rej(e); }); };
    P.all = function (list) {
      return new P(function (res, rej) {
        var n = list.length, out = new Array(n), left = n, i;
        if (!n) { res(out); return; }
        function one(i) { P.resolve(list[i]).then(function (v) { out[i] = v; if (--left === 0) res(out); }, rej); }
        for (i = 0; i < n; i++) one(i);
      });
    };
    P.race = function (list) { return new P(function (res, rej) { for (var i = 0; i < list.length; i++) P.resolve(list[i]).then(res, rej); }); };
    W.Promise = P;
  })();

  /* ---------------------------------------------------------------- language */
  def(Object, 'assign', function (t) {
    if (t == null) throw new TypeError('Cannot convert undefined or null to object');
    var o = Object(t);
    for (var i = 1; i < arguments.length; i++) {
      var s = arguments[i];
      if (s != null) for (var k in s) if (Object.prototype.hasOwnProperty.call(s, k)) o[k] = s[k];
    }
    return o;
  });
  def(Object, 'values', function (o) { var r = []; for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) r.push(o[k]); return r; });
  def(Object, 'entries', function (o) { var r = []; for (var k in o) if (Object.prototype.hasOwnProperty.call(o, k)) r.push([k, o[k]]); return r; });
  def(Array, 'from', function (x, f) {
    var r = [], n = x && x.length != null ? x.length : 0, i;
    for (i = 0; i < n; i++) r.push(f ? f(x[i], i) : x[i]);
    return r;
  });
  def(Array.prototype, 'find', function (f, t) { for (var i = 0; i < this.length; i++) if (f.call(t, this[i], i, this)) return this[i]; return undefined; });
  def(Array.prototype, 'findIndex', function (f, t) { for (var i = 0; i < this.length; i++) if (f.call(t, this[i], i, this)) return i; return -1; });
  def(Array.prototype, 'includes', function (v, from) {
    for (var i = from || 0; i < this.length; i++) if (this[i] === v || (v !== v && this[i] !== this[i])) return true;
    return false;
  });
  def(Array.prototype, 'fill', function (v, s, e) {
    var n = this.length, i = s == null ? 0 : s < 0 ? Math.max(n + s, 0) : Math.min(s, n), j = e == null ? n : e < 0 ? Math.max(n + e, 0) : Math.min(e, n);
    for (; i < j; i++) this[i] = v;
    return this;
  });
  def(String.prototype, 'includes', function (s, p) { return this.indexOf(s, p || 0) !== -1; });
  def(String.prototype, 'startsWith', function (s, p) { p = p || 0; return this.substr(p, String(s).length) === String(s); });
  def(String.prototype, 'endsWith', function (s, e) { var t = String(this); e = e == null || e > t.length ? t.length : e; return t.substring(e - String(s).length, e) === String(s); });
  def(String.prototype, 'repeat', function (n) { var r = '', s = String(this); for (var i = 0; i < n; i++) r += s; return r; });
  def(String.prototype, 'padStart', function (n, f) { var s = String(this); f = f === undefined ? ' ' : String(f); while (s.length < n && f) s = f + s; return s.length > n ? s.slice(s.length - n) : s; });
  def(String.prototype, 'padEnd', function (n, f) { var s = String(this); f = f === undefined ? ' ' : String(f); while (s.length < n && f) s += f; return s.slice(0, Math.max(n, String(this).length)); });
  def(Number, 'isInteger', function (v) { return typeof v === 'number' && isFinite(v) && Math.floor(v) === v; });
  def(Number, 'isFinite', function (v) { return typeof v === 'number' && isFinite(v); });
  def(Number, 'isNaN', function (v) { return typeof v === 'number' && v !== v; });
  def(Math, 'trunc', function (v) { return v < 0 ? Math.ceil(v) : Math.floor(v); });
  def(Math, 'sign', function (v) { v = +v; return v > 0 ? 1 : v < 0 ? -1 : v; });

  /* a WeakMap that keeps its data in a hidden property of the key (enough for the UI's use: DOM nodes and plain objects) */
  if (typeof W.WeakMap !== 'function') {
    var wid = 0;
    W.WeakMap = function () { this._k = '__wm' + (++wid) + '_' + Math.random().toString(36).slice(2); };
    W.WeakMap.prototype.get = function (o) { var e = o && o[this._k]; return e ? e.v : undefined; };
    W.WeakMap.prototype.has = function (o) { return !!(o && o[this._k]); };
    W.WeakMap.prototype.set = function (o, v) { try { Object.defineProperty(o, this._k, { value: { v: v }, configurable: true }); } catch (e) { o[this._k] = { v: v }; } return this; };
    W.WeakMap.prototype['delete'] = function (o) { if (o && o[this._k]) { try { delete o[this._k]; } catch (e) { o[this._k] = null; } return true; } return false; };
  }

  /* ---------------------------------------------------------------- DOM */
  var EP = W.Element && W.Element.prototype;
  if (EP) {
    def(EP, 'matches', EP.msMatchesSelector || EP.webkitMatchesSelector || EP.mozMatchesSelector || function (s) {
      var all = (this.ownerDocument || D).querySelectorAll(s);
      for (var i = 0; i < all.length; i++) if (all[i] === this) return true;
      return false;
    });
    def(EP, 'closest', function (s) { var e = this; while (e && e.nodeType === 1) { if (e.matches(s)) return e; e = e.parentNode; } return null; });
    def(EP, 'remove', function () { if (this.parentNode) this.parentNode.removeChild(this); });
  }
  /* DOMTokenList.toggle(name, force) is ignored by IE-style engines */
  try {
    var probe = D.createElement('div');
    probe.classList.toggle('x', false);
    if (probe.classList.contains('x') && W.DOMTokenList) {
      var oldToggle = W.DOMTokenList.prototype.toggle;
      W.DOMTokenList.prototype.toggle = function (n, force) {
        if (arguments.length > 1) { if (force) this.add(n); else this.remove(n); return !!force; }
        return oldToggle.call(this, n);
      };
    }
  } catch (e) { /* no classList at all: nothing sensible to add here */ }
})();
