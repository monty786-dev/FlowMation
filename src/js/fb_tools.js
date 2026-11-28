/* FlowBrowser page tools: element picker (with grouping of similar elements, page categories) and the macro recorder.
   Injected after fb_runtime.js into every page and frame. The host starts / stops them through window.__fb.tools.
   Messages to the host carry a secret token that page scripts cannot read (it lives only in this closure). */
(function () {
  'use strict';
  var FB = window.__fb;
  if (!FB || (FB.tools && FB.tools.version)) return;
  var TOKEN = '@@FB_TOKEN@@';
  var H = FB.helpers, norm = H.norm, lc = H.lc, cut = H.cut;
  var IS_TOP = window === window.top;

  /* ============================================================ talking to the host */

  function hostPost(obj) {
    obj.token = TOKEN;
    var s = JSON.stringify(obj);
    try {
      if (window.chrome && window.chrome.webview && window.chrome.webview.postMessage) window.chrome.webview.postMessage(s);
      else if (typeof window.__fbPost === 'function') window.__fbPost(s);
    } catch (e) { /* host gone */ }
  }

  /* frames: payloads travel upward, every level prefixes its iframe's selector. Same-origin frames call the parent's
     tools directly (works in every engine); otherwise parent.postMessage carries them. */
  function send(payload) {
    if (IS_TOP) { hostPost(payload); return; }
    try {
      var up = window.parent.__fb && window.parent.__fb.tools && window.parent.__fb.tools._up;
      if (up) { up(JSON.stringify(payload), window); return; }          /* a string: no object crosses script engines */
    } catch (e0) { /* cross-origin parent: fall back to postMessage */ }
    try { window.parent.postMessage({ __fbUp: 1, payload: payload }, '*'); } catch (e) { /* ignore */ }
  }

  function frameElementFor(source) {
    var fr = document.querySelectorAll('iframe,frame');
    for (var i = 0; i < fr.length; i++) {
      if (fr[i].contentWindow === source) return fr[i];
      try { if (source && source.document && fr[i].contentDocument === source.document) return fr[i]; } catch (e) { /* cross-origin */ }
    }
    return null;
  }

  function broadcast(cmd) {
    var fr = document.querySelectorAll('iframe,frame');
    for (var i = 0; i < fr.length; i++) {
      try {
        var ft = fr[i].contentWindow.__fb && fr[i].contentWindow.__fb.tools;
        if (ft && ft.cmd) { ft.cmd(JSON.stringify(cmd)); continue; }
      } catch (e0) { /* cross-origin */ }
      try { fr[i].contentWindow.postMessage({ __fbCmd: 1, cmd: cmd }, '*'); } catch (e) { /* ignore */ }
    }
  }

  function onUp(p, source) {
    if (typeof p === 'string') { try { p = JSON.parse(p); } catch (e) { return; } }
    {
      var el = frameElementFor(source);
      if (el) {
        var sel = bestSelector(el);
        var prefix = sel.selector + ' >> ';
        if (p.selector) { p.selector = prefix + p.selector; p.alts = (p.alts || []).map(function (a) { return prefix + a; }); }
        if (p.step && p.step.selector) { p.step.selector = prefix + p.step.selector; p.step.alts = (p.step.alts || []).map(function (a) { return prefix + a; }); }
        if (p.step && p.step.target) p.step.target = prefix + p.step.target;
      } else { p.cross_origin_frame = true; }
      send(p);
    }
  }

  window.addEventListener('message', function (ev) {
    var d = ev.data;
    if (!d || typeof d !== 'object') return;
    if (d.__fbUp && d.payload) onUp(d.payload, ev.source);
    else if (d.__fbCmd && ev.source === window.parent && d.cmd) runCmd(d.cmd);
  });

  function runCmd(cmd) {
    if (cmd.name === 'picker-start') picker.start(cmd.opts || {}, true);
    else if (cmd.name === 'picker-stop') picker.stop(true);
    else if (cmd.name === 'recorder-start') recorder.start(true);
    else if (cmd.name === 'recorder-stop') recorder.stop(true);
    broadcast(cmd);
  }

  /* ============================================================ selector generation */

  var UNSTABLE = /\d{4,}|[0-9a-f]{8,}|^:r|^react-|^ember\d|^ui-id|^mat-|^radix-|^headlessui|^__|^css-|^sc-|^jsx-|^_[a-z0-9]{5,}$/i;

  /* DOM events are retargeted to the shadow host at document level; the composed path still has the real element */
  function realTarget(ev) {
    var p = ev.composedPath ? ev.composedPath() : null;
    var t = p && p.length ? p[0] : ev.target;
    return t && t.nodeType === 1 ? t : ev.target;
  }

  function stableClass(c) { return c && !UNSTABLE.test(c) && c.length < 40 && !/^(active|hover|focus|open|show|hidden|selected|disabled|is-|has-)/i.test(c); }
  function classes(el) { return (el.classList ? Array.prototype.slice.call(el.classList) : []).filter(stableClass); }

  function unique(sel, el) {
    try { var r = FB._resolveOne(sel); return r.length === 1 && r[0] === el; } catch (e) { return false; }
  }

  function indexIn(sel, el) {
    try { return FB._resolveOne(sel).indexOf(el); } catch (e) { return -1; }
  }

  function q(s) { return '"' + String(s).replace(/\\/g, '\\\\').replace(/"/g, '\\"') + '"'; }

  function cssPath(el) {
    var parts = [], cur = el, depth = 0;
    while (cur && cur.nodeType === 1 && cur !== document.documentElement && depth < 7) {
      var tag = cur.tagName.toLowerCase(), seg = tag;
      if (cur.id && !UNSTABLE.test(cur.id)) { parts.unshift('#' + H.cssEsc(cur.id)); break; }
      var cl = classes(cur).slice(0, 2);
      if (cl.length) seg += '.' + cl.map(H.cssEsc).join('.');
      var par = cur.parentElement;
      if (par) {
        var same = Array.prototype.filter.call(par.children, function (c) { return c.tagName === cur.tagName; });
        var sameCls = cl.length ? same.filter(function (c) { return cl.every(function (k) { return c.classList.contains(k); }); }) : same;
        if (sameCls.length > 1) seg += ':nth-of-type(' + (same.indexOf(cur) + 1) + ')';
      }
      parts.unshift(seg);
      var sel = parts.join(' > ');
      if (unique('css:' + sel, el)) return sel;
      cur = cur.parentElement;
      depth++;
    }
    return parts.join(' > ');
  }

  function labelText(el) { return norm(H.labelFor ? H.labelFor(el) : ''); }

  /* returns {selector, alts[]}: the most readable unique selector first, other unique ones as fallbacks */
  function bestSelector(el) {
    var cands = [], tag = el.tagName.toLowerCase();
    function add(s) { if (s && cands.indexOf(s) < 0 && unique(s, el)) cands.push(s); }
    var tid = el.getAttribute('data-testid') || el.getAttribute('data-test') || el.getAttribute('data-cy') || el.getAttribute('data-qa');
    if (tid) add('testid:' + tid);
    if (el.id && !UNSTABLE.test(el.id)) add('css:#' + H.cssEsc(el.id));
    var name = el.getAttribute('name');
    if (name && /^(input|select|textarea|button|form)$/.test(tag)) {
      var typ = el.getAttribute('type');
      add('css:' + tag + H.attrSel('name', name) + ((tag === 'input' && (typ === 'radio' || typ === 'checkbox') && el.value) ? H.attrSel('value', el.value) : ''));
    }
    var fields = /^(input|select|textarea)$/.test(tag) || el.isContentEditable;
    if (fields) {
      var lt = labelText(el);
      if (lt && lt.length <= 50 && el.type !== 'radio' && el.type !== 'checkbox') add('label:' + q(lt));
      if (el.placeholder) add('placeholder:' + q(el.placeholder));
    }
    var al = el.getAttribute('aria-label');
    if (al && al.length <= 60) add('aria:' + al);
    if (/^(a|button|summary|h1|h2|h3|h4|h5|h6|label|option)$/.test(tag) || el.getAttribute('role') === 'button' || (tag === 'input' && /^(submit|button|reset)$/.test(el.type))) {
      var t = tag === 'input' ? norm(el.value) : norm(el.textContent);
      if (t && t.length <= 40) {
        var s1 = 'text:' + q(t);
        if (unique(s1, el)) add(s1);
        else { var ix = indexIn(s1, el); if (ix >= 0 && ix < 6) add(s1 + ' >> nth=' + (ix + 1)); }
      }
    }
    if (el.getAttribute('title') && tag !== 'html') add('title:' + el.getAttribute('title'));
    if (tag === 'img' && el.alt) add('alt:' + el.alt);
    var path = cssPath(el);
    if (path) add('css:' + path);
    if (!cands.length) {                                                      /* last resort: absolute XPath-like path */
      var xp = '', n = el;
      while (n && n.nodeType === 1) {
        var i = 1, sib = n.previousElementSibling;
        while (sib) { if (sib.tagName === n.tagName) i++; sib = sib.previousElementSibling; }
        xp = '/' + n.tagName.toLowerCase() + '[' + i + ']' + xp;
        n = n.parentElement;
      }
      cands.push('xpath:/' + xp);
    }
    return { selector: cands[0], alts: cands.slice(1, 4) };
  }

  /* ============================================================ describing & grouping */

  function describe(el) {
    var b = bestSelector(el), tag = el.tagName.toLowerCase();
    var attrs = {};
    Array.prototype.slice.call(el.attributes || []).slice(0, 30).forEach(function (a) { attrs[a.name] = cut(a.value, 200); });
    var kind = H.kindOf(el);
    var d = { tag: tag, selector: b.selector, alts: b.alts, text: cut(el.innerText != null ? el.innerText : el.textContent, 300), attrs: attrs, kind: kind, rect: null };
    var r = el.getBoundingClientRect();
    d.rect = { x: Math.round(r.left), y: Math.round(r.top), w: Math.round(r.width), h: Math.round(r.height) };
    if (el.href) d.href = el.href;
    if (el.src || el.currentSrc) d.src = el.currentSrc || el.src;
    if (el.value != null && tag !== 'li' && tag !== 'button') d.value = kind === 'password' ? '' : cut(el.value, 200);
    if (tag === 'select') d.options = Array.prototype.slice.call(el.options).slice(0, 100).map(function (o) { return norm(o.text); });
    var tb = el.closest && el.closest('table');
    if (tag === 'table' || tb) { var tel = tag === 'table' ? el : tb; d.table = { selector: bestSelector(tel).selector, rows: tel.rows ? tel.rows.length : 0 }; }
    d.suggest = suggest(el, d);
    return d;
  }

  /* what the panel offers as actions for this element */
  function suggest(el, d) {
    var s = [], tag = d.tag, k = d.kind;
    if (/^(input|select|textarea)$/.test(tag) || el.isContentEditable) {
      if (k === 'file') s.push('upload_file');
      else if (k === 'checkbox' || k === 'radio') s.push('check');
      else if (k === 'range') s.push('set_slider');
      else if (tag === 'select') s.push('select_option');
      else s.push('fill', 'type_text');
      s.push('get_text');
    } else {
      s.push('click', 'hover', 'get_text', 'get_attribute');
      if (tag === 'table' || d.table) s.push('extract_table');
      s.push('wait_element', 'element_exists', 'scroll', 'drag_drop');
    }
    return s;
  }

  function itemSig(el) { return el.tagName + '.' + classes(el).sort().join('.'); }

  function relPath(root, el) {   /* selector of el inside root, class-based so it works for every similar item */
    var parts = [], cur = el;
    while (cur && cur !== root) {
      var seg = cur.tagName.toLowerCase(), cl = classes(cur).slice(0, 2);
      if (cl.length) seg += '.' + cl.map(H.cssEsc).join('.');
      else {
        var par = cur.parentElement, same = Array.prototype.filter.call(par.children, function (c) { return c.tagName === cur.tagName; });
        if (same.length > 1) seg += ':nth-of-type(' + (same.indexOf(cur) + 1) + ')';
      }
      parts.unshift(seg);
      cur = cur.parentElement;
      if (parts.length >= 3 && parts.join(' ').indexOf('.') >= 0) break;
    }
    return parts.join(' ');
  }

  function guessName(el, sel, used) {
    var n = '';
    var cl = classes(el);
    if (cl.length) n = cl[0];
    if (!n || n.length < 3) n = /^h[1-6]$/i.test(el.tagName) ? 'heading' : el.tagName === 'A' ? 'link' : el.tagName === 'IMG' ? 'image' : el.tagName === 'TIME' ? 'date' : 'field';
    n = n.replace(/[^\wÀ-￿]+/g, '_').replace(/^_+|_+$/g, '').toLowerCase() || 'field';
    var base = n, i = 2;
    while (used[n]) n = base + '_' + (i++);
    used[n] = 1;
    return n;
  }

  /* From one picked element: all similar items on the page and the fields inside them. */
  function similar(el) {
    var cur = el, best = null;
    for (var depth = 0; cur && cur !== document.body && depth < 8; depth++, cur = cur.parentElement) {
      var sig = itemSig(cur);
      var par = cur.parentElement;
      if (!par) break;
      var sibs = Array.prototype.filter.call(par.children, function (c) { return itemSig(c) === sig; });
      if (sibs.length >= 2 && cur.textContent.trim().length > 0) { best = { item: cur, items: sibs }; if (sibs.length >= 3 || depth >= 2) break; }
    }
    if (!best) return { ok: true, found: false };
    var item = best.item, items = best.items;
    var cl = classes(item).slice(0, 3);
    var itemSel = item.tagName.toLowerCase() + (cl.length ? '.' + cl.map(H.cssEsc).join('.') : '');
    var parentSel = best.item.parentElement && best.item.parentElement !== document.body ? bestSelector(best.item.parentElement).selector : '';
    var full = 'css:' + itemSel;
    var matched = [];
    try { matched = FB._resolveOne(full); } catch (e) { matched = []; }
    if (matched.length < items.length && parentSel && /^css:/.test(parentSel)) { full = parentSel + ' >> css:' + itemSel; try { matched = FB._resolveOne(full); } catch (e) { matched = []; } }
    if (matched.length < 2) { full = 'css:' + cssPath(item); matched = items; }

    /* leaf-ish descendants that carry data: text, link, image */
    var used = {}, fields = [], seen = {};
    var leaves = item.querySelectorAll('*');
    Array.prototype.forEach.call(leaves, function (n) {
      if (/^(script|style|svg|path|br|hr)$/i.test(n.tagName)) return;
      var hasKids = Array.prototype.some.call(n.children, function (c) { return norm(c.textContent) && !/^(script|style|svg)$/i.test(c.tagName); });
      var own = norm(n.innerText || n.textContent);
      var data = [];
      if (n.tagName === 'IMG') data.push({ attr: 'src' });
      if (n.tagName === 'A' && n.getAttribute('href')) data.push({ attr: 'href' });
      if (own && !hasKids) data.push({ attr: 'text' });
      if (n.tagName === 'A' && own && hasKids === false && data.length === 2) { /* link: text first, then href */ data.reverse(); }
      data.forEach(function (d) {
        var rel = relPath(item, n);
        var key = rel + '|' + d.attr;
        if (!rel || seen[key]) return;
        seen[key] = 1;
        var vals = items.slice(0, 5).map(function (it) {
          var f = null;
          try { f = FB._resolveOne('css:' + rel, it)[0]; } catch (e) { f = null; }
          if (!f) return '';
          return d.attr === 'text' ? norm(f.innerText || f.textContent) : (d.attr === 'href' ? f.href : (f.currentSrc || f.src || f.getAttribute(d.attr) || ''));
        });
        var present = vals.filter(Boolean).length;
        if (present < Math.max(1, Math.ceil(vals.length * 0.6))) return;       /* a field that most items do not have is noise */
        var nm = guessName(n, rel, used);
        if (d.attr === 'href' && nm === 'link') nm = 'link';
        fields.push({ name: d.attr === 'href' && !/link|url/.test(nm) ? nm + '_link' : d.attr === 'src' && !/image|img|photo/.test(nm) ? nm + '_image' : nm, selector: 'css:' + rel, attr: d.attr === 'text' ? undefined : d.attr, sample: vals.slice(0, 3).map(function (v) { return cut(v, 80); }) });
      });
    });
    if (!fields.length) fields.push({ name: 'text', selector: '', sample: items.slice(0, 3).map(function (i) { return cut(i.textContent, 80); }) });
    var uniq = {};
    fields.forEach(function (f) { var n = f.name, i = 2; while (uniq[n]) n = f.name + '_' + (i++); uniq[n] = 1; f.name = n; });
    return { ok: true, found: true, item_selector: full, count: matched.length >= 2 ? matched.length : items.length, fields: fields.slice(0, 15) };
  }

  /* The whole page sorted into categories, so the person can pick what to extract. */
  function categorize() {
    var cats = [], seen = [];
    function add(c) { cats.push(c); }
    var q$ = function (s) { return Array.prototype.slice.call(document.querySelectorAll(s)); };
    q$('table,[role=table],[role=grid]').slice(0, 30).forEach(function (t, i) {
      if (!H.visible(t)) return;
      var r = H.tableToRows(t, 'auto');
      add({ category: 'Tables', label: (t.caption ? norm(t.caption.textContent) : 'Table ' + (i + 1)) + ' (' + r.rows.length + ' rows' + (r.columns.length ? ', columns: ' + cut(r.columns.join(', '), 60) : '') + ')', selector: bestSelector(t).selector, count: r.rows.length, step: 'extract_table', sample: cut(JSON.stringify(r.rows.slice(0, 2)), 200) });
    });
    /* groups of similar children (cards, search results, list items) */
    var parents = q$('ul,ol,div,section,main,tbody,nav,article,dl,body').filter(function (p) { return p.children.length >= 3 && H.visible(p); });
    parents.forEach(function (p) {
      var groups = {};
      Array.prototype.forEach.call(p.children, function (c) { if (norm(c.textContent)) { var k = itemSig(c); (groups[k] = groups[k] || []).push(c); } });
      Object.keys(groups).forEach(function (k) {
        var g = groups[k];
        if (g.length < 3 || /^(TR|TD|TH|OPTION|SCRIPT|STYLE|BR)\./.test(k)) return;
        if (g.some(function (x) { return seen.indexOf(x) >= 0; })) return;
        if (p.closest && p.closest('table,[role=table],[role=grid],[role=treegrid]')) return;      /* already listed as a table */
        if (cut(g[0].textContent, 5).length < 2) return;
        g.forEach(function (x) { seen.push(x); });
        var sim = similar(g[0]);
        if (!sim.found) return;
        add({ category: 'Repeated items', label: g.length + ' similar <' + g[0].tagName.toLowerCase() + '> items: ' + cut(g[0].textContent, 50), selector: sim.item_selector, count: sim.count, step: 'extract_list', fields: sim.fields, sample: sim.fields.map(function (f) { return f.name + '=' + (f.sample[0] || ''); }).join(' | ') });
      });
    });
    var hs = q$('h1,h2,h3,h4,h5,h6').filter(function (h) { return norm(h.textContent); });
    if (hs.length) add({ category: 'Headings', label: hs.length + ' headings', selector: 'css:h1,h2,h3,h4,h5,h6', count: hs.length, step: 'extract_page', kinds: ['headings'], sample: cut(hs.slice(0, 3).map(function (h) { return norm(h.textContent); }).join(' | '), 160) });
    var ls = q$('a[href]');
    if (ls.length) add({ category: 'Links', label: ls.length + ' links', selector: 'css:a[href]', count: ls.length, step: 'extract_page', kinds: ['links'], sample: cut(ls.slice(0, 3).map(function (a) { return norm(a.textContent); }).join(' | '), 160) });
    var im = q$('img');
    if (im.length) add({ category: 'Images', label: im.length + ' images', selector: 'css:img', count: im.length, step: 'extract_page', kinds: ['images'], sample: cut(im.slice(0, 2).map(function (i) { return i.alt || i.src; }).join(' | '), 160) });
    q$('form').slice(0, 20).forEach(function (f, i) {
      var n = f.querySelectorAll('input:not([type=hidden]),select,textarea').length;
      if (n) add({ category: 'Forms', label: 'Form ' + (i + 1) + ': ' + n + ' fields' + (f.getAttribute('action') ? ' → ' + cut(f.getAttribute('action'), 40) : ''), selector: bestSelector(f).selector, count: n, step: 'autofill_form' });
    });
    var inp = q$('input:not([type=hidden]),select,textarea').filter(H.visible);
    if (inp.length) add({ category: 'Form fields', label: inp.length + ' fields', selector: 'css:input,select,textarea', count: inp.length, step: 'extract_page', kinds: ['inputs'], sample: cut(inp.slice(0, 4).map(function (i) { return labelText(i) || i.name || i.type; }).join(' | '), 160) });
    var bt = q$('button,[role=button],input[type=submit],input[type=button]').filter(H.visible);
    if (bt.length) add({ category: 'Buttons', label: bt.length + ' buttons', selector: 'css:button,[role=button],input[type=submit],input[type=button]', count: bt.length, step: 'extract_page', kinds: ['buttons'], sample: cut(bt.slice(0, 4).map(function (b) { return norm(b.textContent || b.value); }).join(' | '), 160) });
    var ps = q$('p,blockquote,li').filter(function (p) { return norm(p.textContent).length > 20; });
    if (ps.length) add({ category: 'Text blocks', label: ps.length + ' paragraphs / list items', selector: 'css:p', count: ps.length, step: 'extract_page', kinds: ['text'] });
    var mc = q$('meta[name],meta[property]').length;
    if (mc) add({ category: 'Page info', label: 'Title, description and ' + mc + ' meta tags', selector: '', count: mc, step: 'extract_page', kinds: ['meta'] });
    return { ok: true, url: location.href, title: document.title, groups: cats };
  }

  /* ============================================================ picker */

  var picker = (function () {
    var on = false, box = null, tip = null, hover = null, mode = 'element';

    function ensure() {
      if (box) return;
      function mk(css) { var d = document.createElement('div'); d.setAttribute('data-fb-ui', '1'); d.style.cssText = 'all:initial;position:fixed;z-index:2147483647;pointer-events:none;' + css; return d; }
      box = mk('border:2px solid #E95420;background:rgba(233,84,32,.18);border-radius:2px;display:none;');
      tip = mk('background:#2C001E;color:#fff;font:12px/1.4 Ubuntu,"Segoe UI",sans-serif;padding:3px 7px;border-radius:3px;display:none;max-width:420px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;');
      (document.documentElement || document.body).appendChild(box);
      (document.documentElement || document.body).appendChild(tip);
    }

    function target(ev) {
      var e = realTarget(ev);
      if (!e || e.nodeType !== 1) return null;
      if (e.getAttribute && e.getAttribute('data-fb-ui')) return null;
      return e;
    }

    function move(ev) {
      var t = target(ev);
      if (!t) return;
      hover = t;
      var r = t.getBoundingClientRect();
      box.style.cssText += ';display:block;left:' + r.left + 'px;top:' + r.top + 'px;width:' + r.width + 'px;height:' + r.height + 'px';
      var b = bestSelector(t);
      tip.textContent = '<' + t.tagName.toLowerCase() + '>  ' + b.selector;
      tip.style.cssText += ';display:block;left:' + Math.max(0, r.left) + 'px;top:' + (r.top > 28 ? r.top - 26 : r.bottom + 4) + 'px';
    }

    function stopEvent(ev) { if (!target(ev) && ev.type !== 'keydown') return; ev.preventDefault(); ev.stopPropagation(); ev.stopImmediatePropagation(); }

    function click(ev) {
      var t = target(ev);
      if (!t) return;
      ev.preventDefault(); ev.stopPropagation(); ev.stopImmediatePropagation();
      var d = describe(t);
      var out = { kind: 'pick', mode: mode, element: d };
      if (mode === 'similar' || mode === 'element') out.similar = similar(t);
      send(out);
    }

    function key(ev) { if (ev.key === 'Escape') { send({ kind: 'pick_cancel' }); stop(); } }

    function start(opts, fromParent) {
      if (on) stop(true);
      on = true; mode = opts.mode || 'element';
      ensure();
      document.addEventListener('mousemove', move, true);
      document.addEventListener('click', click, true);
      ['mousedown', 'mouseup', 'pointerdown', 'pointerup', 'dblclick', 'contextmenu', 'submit'].forEach(function (n) { document.addEventListener(n, stopEvent, true); });
      document.addEventListener('keydown', key, true);
      if (!fromParent) broadcast({ name: 'picker-start', opts: opts });
    }

    function stop(fromParent) {
      if (!on) return;
      on = false;
      document.removeEventListener('mousemove', move, true);
      document.removeEventListener('click', click, true);
      ['mousedown', 'mouseup', 'pointerdown', 'pointerup', 'dblclick', 'contextmenu', 'submit'].forEach(function (n) { document.removeEventListener(n, stopEvent, true); });
      document.removeEventListener('keydown', key, true);
      if (box) box.style.display = 'none';
      if (tip) tip.style.display = 'none';
      if (!fromParent) broadcast({ name: 'picker-stop' });
    }

    return { start: start, stop: stop, active: function () { return on; } };
  })();

  /* ============================================================ recorder */

  var recorder = (function () {
    var on = false, pendingFill = null, timer = null, lastClickKey = '', lastClickAt = 0;
    var secretCount = 0;

    function isSecret(el) {
      var ac = (el.getAttribute('autocomplete') || '').toLowerCase();
      return el.type === 'password' || /password|cc-number|cc-csc|one-time-code/.test(ac);
    }

    function emit(step, mergeKey, extra) {
      var p = { kind: 'rec', step: step };
      if (mergeKey) p.merge_key = mergeKey;
      if (extra) for (var k in extra) p[k] = extra[k];
      send(p);
    }

    function withSel(step, el) {
      var b = bestSelector(el);
      step.selector = b.selector;
      if (b.alts.length) step.alts = b.alts;
      return step;
    }

    function nameOf(el) { var t = labelText(el) || norm(el.textContent) || el.getAttribute('placeholder') || el.name || el.id || el.tagName.toLowerCase(); return cut(t, 40); }

    function flushFill() {
      if (!pendingFill) return;
      var el = pendingFill; pendingFill = null; clearTimeout(timer);
      if (el.isConnected === false) return;
      var step, secret = isSecret(el);
      var key = 'fill:' + bestSelector(el).selector;
      if (secret) {
        secretCount++;
        var v = 'password' + (secretCount > 1 ? '_' + secretCount : '');
        step = withSel({ type: 'fill', name: 'Fill ' + nameOf(el), value: '{{' + v + '}}' }, el);
        emit(step, key, { secret_var: v });                 /* the real value is never sent to the host */
      } else if (el.isContentEditable) {
        emit(withSel({ type: 'fill', name: 'Fill ' + nameOf(el), value: norm(el.textContent) }, el), key);
      } else {
        emit(withSel({ type: 'fill', name: 'Fill ' + nameOf(el), value: el.value }, el), key);
      }
    }

    function onInput(ev) {
      var el = realTarget(ev);
      if (!el || el.nodeType !== 1) return;
      var k = H.kindOf(el);
      if (k === 'select' || k === 'checkbox' || k === 'radio' || k === 'file' || k === 'range') return;
      if (el.tagName !== 'INPUT' && el.tagName !== 'TEXTAREA' && !el.isContentEditable) return;
      if (pendingFill && pendingFill !== el) flushFill();
      pendingFill = el;
      clearTimeout(timer);
      timer = setTimeout(flushFill, 600);
    }

    function onChange(ev) {
      var el = realTarget(ev);
      if (!el || el.nodeType !== 1) return;
      var k = H.kindOf(el);
      if (pendingFill && pendingFill !== el) flushFill();                      /* keep the order the person did things in */
      if (k === 'select') {
        var o = el.selectedOptions;
        var val = Array.prototype.map.call(o, function (x) { return norm(x.text); }).join('|');
        emit(withSel({ type: 'select_option', name: 'Choose ' + cut(val, 30), value: val }, el), 'sel:' + bestSelector(el).selector);
      } else if (k === 'checkbox') {
        emit(withSel({ type: 'check', name: (el.checked ? 'Check ' : 'Uncheck ') + nameOf(el), state: el.checked ? 'check' : 'uncheck' }, el));
      } else if (k === 'radio') {
        emit(withSel({ type: 'check', name: 'Select ' + nameOf(el), state: 'check' }, el));
      } else if (k === 'range') {
        emit(withSel({ type: 'set_slider', name: 'Set slider ' + nameOf(el), value: el.value }, el), 'rng:' + bestSelector(el).selector);
      } else if (k === 'file') {
        var names = Array.prototype.map.call(el.files || [], function (f) { return f.name; });
        emit(withSel({ type: 'upload_file', name: 'Upload ' + cut(names.join(', '), 40), files: names, mode: 'auto' }, el), null, { needs_file_path: true });
      } else flushFill();
    }

    function onClick(ev) {
      var el = realTarget(ev);
      if (!el || el.nodeType !== 1 || (el.getAttribute && el.getAttribute('data-fb-ui'))) return;
      if (ev.isTrusted === false) return;
      flushFill();
      var k = H.kindOf(el);
      if (/^(text|search|email|url|tel|number|password|textarea|date|time|datetime-local|month|week|color|select|range|file)$/.test(k) || el.isContentEditable) return;   /* focus clicks: the value is recorded instead */
      if (k === 'checkbox' || k === 'radio') return;                                                                        /* recorded by the change event */
      var lab = el.closest && el.closest('label');
      if (lab && lab.control && /^(checkbox|radio|file)$/.test(lab.control.type || '')) return;
      var target = el.closest ? (el.closest('a,button,[role=button],[role=link],[role=menuitem],[role=tab],summary,input[type=submit],input[type=button]') || el) : el;
      var b = bestSelector(target);
      var now = Date.now(), key = b.selector;
      if (key === lastClickKey && now - lastClickAt < 450) {                   /* second click of a double click: turn the last step into count=2 */
        emit({ type: 'click', name: 'Double click ' + nameOf(target), selector: b.selector, alts: b.alts, count: 2 }, 'click:' + key);
        lastClickAt = now; return;
      }
      lastClickKey = key; lastClickAt = now;
      var step = { type: 'click', name: 'Click ' + nameOf(target), selector: b.selector };
      if (b.alts.length) step.alts = b.alts;
      if (ev.button === 2) step.button = 'right';
      emit(step, 'click:' + key);
    }

    function onContext(ev) { /* right click is recorded through click-like handling */
      var el = realTarget(ev); if (!el || el.nodeType !== 1) return;
      var b = bestSelector(el);
      emit({ type: 'click', name: 'Right click ' + nameOf(el), selector: b.selector, alts: b.alts, button: 'right' });
    }

    var NAMED = { Enter: 'Enter', Tab: 'Tab', Escape: 'Escape', ArrowDown: 'ArrowDown', ArrowUp: 'ArrowUp', PageDown: 'PageDown', PageUp: 'PageUp' };
    function onKey(ev) {
      if (!NAMED[ev.key] || ev.isTrusted === false) return;
      var el = realTarget(ev);
      if (ev.key === 'Tab') return;                       /* focus moves are not worth a step */
      if (ev.key === 'Enter' && el && el.tagName === 'TEXTAREA') return;
      flushFill();
      var step = { type: 'press_key', name: 'Press ' + ev.key, key: ev.key };
      if (el && el.nodeType === 1 && el !== document.body && el !== document.documentElement) { var b = bestSelector(el); step.selector = b.selector; if (b.alts.length) step.alts = b.alts; }
      emit(step);
    }

    var dragSrc = null;
    function onDragStart(ev) { dragSrc = realTarget(ev); }
    function onDrop(ev) {
      var dst = realTarget(ev);
      if (!dragSrc || !dst || dst.nodeType !== 1 || (ev.dataTransfer && ev.dataTransfer.files && ev.dataTransfer.files.length)) return;
      var a = bestSelector(dragSrc), b = bestSelector(dst);
      emit({ type: 'drag_drop', name: 'Drag ' + nameOf(dragSrc), selector: a.selector, alts: a.alts, target: b.selector, method: 'html5' });
      dragSrc = null;
    }

    function start(fromParent) {
      if (on) return;
      on = true; secretCount = 0;
      document.addEventListener('input', onInput, true);
      document.addEventListener('change', onChange, true);
      document.addEventListener('click', onClick, true);
      document.addEventListener('contextmenu', onContext, true);
      document.addEventListener('keydown', onKey, true);
      document.addEventListener('dragstart', onDragStart, true);
      document.addEventListener('drop', onDrop, true);
      window.addEventListener('beforeunload', flushFill, true);
      if (!fromParent) broadcast({ name: 'recorder-start' });
    }

    function stop(fromParent) {
      if (!on) return;
      flushFill();
      on = false;
      document.removeEventListener('input', onInput, true);
      document.removeEventListener('change', onChange, true);
      document.removeEventListener('click', onClick, true);
      document.removeEventListener('contextmenu', onContext, true);
      document.removeEventListener('keydown', onKey, true);
      document.removeEventListener('dragstart', onDragStart, true);
      document.removeEventListener('drop', onDrop, true);
      window.removeEventListener('beforeunload', flushFill, true);
      if (!fromParent) broadcast({ name: 'recorder-stop' });
    }

    return { start: start, stop: stop, active: function () { return on; } };
  })();

  /* ============================================================ selector tester: highlight what a selector matches */

  function flash(els) {
    var root = document.documentElement || document.body;
    var boxes = els.slice(0, 60).map(function (e) {
      var r = e.getBoundingClientRect();
      var b = document.createElement('div');
      b.setAttribute('data-fb-ui', '1');
      b.style.cssText = 'all:initial;position:fixed;z-index:2147483646;pointer-events:none;border:2px solid #E95420;background:rgba(233,84,32,.25);border-radius:2px;left:' + r.left + 'px;top:' + r.top + 'px;width:' + r.width + 'px;height:' + r.height + 'px';
      root.appendChild(b);
      return b;
    });
    if (els[0] && els[0].scrollIntoView) { try { els[0].scrollIntoView({ block: 'center', inline: 'nearest' }); } catch (e) { /* ignore */ } }
    setTimeout(function () { boxes.forEach(function (b) { if (b.parentNode) b.parentNode.removeChild(b); }); }, 2200);
  }

  function testSelector(sel) {
    var els;
    try { els = FB._resolveOne(sel); } catch (e) { return { ok: false, error: String(e && e.message || e) }; }
    flash(els);
    return { ok: true, count: els.length, samples: els.slice(0, 5).map(function (e) { return { tag: e.tagName.toLowerCase(), text: cut(norm(e.innerText || e.value || e.alt || e.getAttribute('aria-label') || ''), 80) }; }) };
  }

  /* The host keeps the "recording" flag per tab; a page that loads while recording asks for it. */
  function announce() { send({ kind: 'tools_ready', url: location.href, top: IS_TOP }); }

  FB.tools = {
    version: 1,
    picker: picker,
    recorder: recorder,
    describe: function (sel) { return describe(FB._resolveOne(sel)[0]); },
    similar: function (sel) { return similar(FB._resolveOne(sel)[0]); },
    categorize: categorize,
    bestSelector: bestSelector,
    /* called by the host: JSON string in, so no object crosses worlds */
    cmd: function (json) {
      var c = typeof json === 'string' ? JSON.parse(json) : json;
      runCmd(c);
      return 'ok';
    },
    call: function (name, argJson) {
      var a = argJson ? JSON.parse(argJson) : {};
      if (name === 'categorize') return JSON.stringify(categorize());
      if (name === 'describe') return JSON.stringify(describe(FB._resolveOne(a.selector)[0]));
      if (name === 'similar') return JSON.stringify(similar(FB._resolveOne(a.selector)[0]));
      if (name === 'test') return JSON.stringify(testSelector(a.selector));
      return JSON.stringify({ ok: false, error: 'unknown' });
    },
    announce: announce,
    _up: onUp
  };

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', announce); else announce();
})();
