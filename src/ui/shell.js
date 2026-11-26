/* FlowBrowser shell: tabs, address bar, bookmarks, history, find, menu, layout of page / panel. */
(function () {
  'use strict';
  var NEWTAB = 'https://appassets.example/newtab.html';
  var DEF = { theme: 'light', home: '', search: 'https://duckduckgo.com/?q=%s', bookmarksBar: true, panelWidth: 460, panelMode: 'dock', panelVisible: true, defaultZoom: 100 };
  var S = { tabs: [], active: 0, settings: Object.assign({}, DEF), bookmarks: [], history: [], zoom: 1, auto: { recording: false, state: 'idle' }, overlay: 0, ask: null, version: '' };
  var el = {};
  ['tabs', 'newtab', 'back', 'fwd', 'reload', 'home', 'url', 'lock', 'star', 'rec', 'pick', 'run', 'ptoggle', 'menubtn', 'bmbar', 'findbar', 'ftext', 'fprev', 'fnext', 'fcase', 'fclose',
    'banner', 'bmsg', 'binp', 'bok', 'bstop', 'content', 'chint', 'split', 'panelslot', 'srun', 'sstep', 'szoom', 'sload', 'menu'].forEach(function (id) { el[id] = document.getElementById(id); });

  /* ------------------------------------------------------------ persistence */
  function load(key, def) { return Host.call('store.get', { key: key }).then(function (r) { return r.value == null ? def : r.value; }, function () { return def; }); }
  function save(key, val) { Host.send('store.set', { key: key, value: val }); }
  var saveHistory = debounce(function () { save('history', S.history); }, 1500);
  function mirrorSettings() { try { localStorage.setItem('fbSettings', JSON.stringify({ search: S.settings.search, theme: S.settings.theme })); } catch (e) { /* ignore */ } }

  /* ------------------------------------------------------------ overlay: native page views sit on top of HTML, so hide them while a menu / dialog is open */
  function overlay(d) {
    var was = S.overlay > 0;
    S.overlay = Math.max(0, S.overlay + d);
    if ((S.overlay > 0) !== was) Host.send('overlay', { on: S.overlay > 0 });
  }
  var _modal = window.modal;
  window.modal = function (o) { overlay(1); return _modal(o).then(function (v) { overlay(-1); return v; }); };

  /* ------------------------------------------------------------ layout */
  function rectOf(e) { var r = e.getBoundingClientRect(); return { x: Math.round(r.left), y: Math.round(r.top), w: Math.max(0, Math.round(r.width)), h: Math.max(0, Math.round(r.height)) }; }
  var reportLayout = debounce(function () {
    var st = S.settings, dock = st.panelVisible && st.panelMode === 'dock';
    Host.send('layout', { content: rectOf(el.content), panel: dock ? rectOf(el.panelslot) : null });
  }, 15);
  function applyPanel() {
    var st = S.settings, dock = st.panelVisible && st.panelMode === 'dock';
    el.panelslot.classList.toggle('hide', !dock);
    el.split.classList.toggle('hide', !dock);
    el.panelslot.style.width = Math.max(300, st.panelWidth | 0) + 'px';
    el.ptoggle.classList.toggle('on', !!st.panelVisible);
    Host.send('panel', { visible: !!st.panelVisible, mode: st.panelMode });
    reportLayout();
  }
  window.addEventListener('resize', reportLayout);
  if (window.ResizeObserver) { var ro = new ResizeObserver(reportLayout); ro.observe(el.content); ro.observe(el.panelslot); }
  (function splitter() {
    var drag = false;
    el.split.addEventListener('mousedown', function (e) { drag = true; el.split.classList.add('drag'); e.preventDefault(); });
    window.addEventListener('mousemove', function (e) {
      if (!drag) return;
      var w = window.innerWidth - e.clientX - 3;
      S.settings.panelWidth = Math.min(Math.max(300, w), Math.round(window.innerWidth * .8));
      el.panelslot.style.width = S.settings.panelWidth + 'px';
      reportLayout();
    });
    window.addEventListener('mouseup', function () { if (!drag) return; drag = false; el.split.classList.remove('drag'); persistSettings(); });
  })();
  function persistSettings() { save('settings', S.settings); mirrorSettings(); }

  /* ------------------------------------------------------------ tabs */
  function shown(u) { return !u || u === NEWTAB ? '' : u; }
  function tabNow() { for (var i = 0; i < S.tabs.length; i++) if (S.tabs[i].id === S.active) return S.tabs[i]; return null; }
  function renderTabs() {
    clear(el.tabs);
    S.tabs.forEach(function (t) {
      var fav = h('span.fav', t.favicon ? h('img', { src: t.favicon, alt: '' }) : (t.title || '·').trim().charAt(0).toUpperCase());
      var tab = h('div.tab' + (t.id === S.active ? '.on' : '') + (t.loading ? '.loading' : ''), {
        title: (t.title || '') + '\n' + shown(t.url),
        onmousedown: function (e) { if (e.button === 1) { e.preventDefault(); Host.send('tab.close', { id: t.id }); } else if (e.button === 0 && !e.target.closest('.x')) Host.send('tab.activate', { id: t.id }); }
      }, fav, h('span.ttl.ell', { text: t.title || (t.loading ? 'Loading…' : 'New tab') }),
      h('button.x', { text: '✕', title: 'Close tab', onclick: function (e) { e.stopPropagation(); Host.send('tab.close', { id: t.id }); } }));
      el.tabs.appendChild(tab);
    });
    var t = tabNow();
    el.back.disabled = !t || !t.canBack; el.fwd.disabled = !t || !t.canForward;
    el.back.style.opacity = el.back.disabled ? .35 : 1; el.fwd.style.opacity = el.fwd.disabled ? .35 : 1;
    el.reload.textContent = t && t.loading ? '✕' : '⟳';
    el.reload.title = t && t.loading ? 'Stop' : 'Reload (F5)';
    if (document.activeElement !== el.url) el.url.value = t ? shown(t.url) : '';
    var u = t ? t.url : '';
    el.lock.textContent = /^https:/.test(u) ? '🔒' : /^http:/.test(u) ? '⚠' : '🛈';
    el.lock.title = /^https:/.test(u) ? 'Secure connection' : /^http:/.test(u) ? 'Not secure' : '';
    el.sload.textContent = t && t.loading ? 'Loading…' : '';
    el.chint.textContent = S.tabs.length ? '' : 'No tab open. Press Ctrl+T.';
    document.title = (t && t.title ? t.title + ' - ' : '') + 'FlowBrowser';
    starState();
  }
  Host.on('tabs', function (m) { S.tabs = m.tabs || []; S.active = m.active; renderTabs(); });
  Host.on('nav.done', function (m) {
    if (!m.url || m.url === NEWTAB || /^about:/.test(m.url)) return;
    var last = S.history[0];
    if (last && last.url === m.url) { last.title = m.title || last.title; last.t = Date.now(); } else S.history.unshift({ url: m.url, title: m.title || m.url, t: Date.now() });
    if (S.history.length > 3000) S.history.length = 3000;
    saveHistory();
  });
  Host.on('panel.closed', function () { S.settings.panelVisible = false; applyPanel(); persistSettings(); });   /* the floating Studio window was closed with its own X */
  Host.on('zoom', function (m) { S.zoom = m.factor || 1; el.szoom.textContent = Math.round(S.zoom * 100) !== 100 ? 'Zoom ' + Math.round(S.zoom * 100) + '%' : ''; });

  el.newtab.onclick = function () { Host.send('tab.new', {}); };
  el.back.onclick = function () { Host.send('nav.back'); };
  el.fwd.onclick = function () { Host.send('nav.forward'); };
  el.reload.onclick = function () { var t = tabNow(); Host.send(t && t.loading ? 'nav.stop' : 'nav.reload'); };
  el.home.onclick = function () { Host.send('nav.go', { url: S.settings.home || NEWTAB }); };

  /* ------------------------------------------------------------ address bar */
  function resolveInput(txt) {
    txt = String(txt || '').trim();
    if (!txt) return '';
    if (/^javascript:/i.test(txt)) return '';
    if (/^[a-z][a-z0-9+.-]*:\/\//i.test(txt) || /^(about:blank|file:)/i.test(txt)) return txt;
    if (/^[a-z]:[\\/]/i.test(txt)) return 'file:///' + txt.replace(/\\/g, '/');
    if (!/\s/.test(txt) && (/^localhost(:\d+)?(\/|$)/i.test(txt) || /^\d{1,3}(\.\d{1,3}){3}(:\d+)?(\/|$)/.test(txt))) return 'http://' + txt;
    if (!/\s/.test(txt) && /^[^\/?#]+\.[a-z]{2,}(:\d+)?([\/?#].*)?$/i.test(txt)) return 'https://' + txt;
    return (S.settings.search || DEF.search).replace('%s', encodeURIComponent(txt));
  }
  el.url.addEventListener('keydown', function (e) {
    if (e.key === 'Enter') { var u = resolveInput(el.url.value); if (u) { Host.send('nav.go', { url: u, user: true }); el.url.blur(); } }
    if (e.key === 'Escape') { var t = tabNow(); el.url.value = t ? shown(t.url) : ''; el.url.blur(); }
  });
  el.url.addEventListener('focus', function () { el.url.select(); });

  /* ------------------------------------------------------------ bookmarks */
  function isBookmarked(u) { return S.bookmarks.some(function (b) { return b.url === u; }); }
  function starState() { var t = tabNow(), on = t && isBookmarked(t.url); el.star.textContent = on ? '★' : '☆'; el.star.classList.toggle('on', !!on); }
  function renderBookmarks() {
    var show = S.settings.bookmarksBar && S.bookmarks.length;
    el.bmbar.classList.toggle('hide', !show);
    clear(el.bmbar);
    S.bookmarks.slice(0, 30).forEach(function (b) { el.bmbar.appendChild(h('button.btn.flat.sm.ell', { title: b.url, text: '★ ' + (b.title || b.url), onclick: function () { Host.send('nav.go', { url: b.url }); } })); });
    reportLayout();
  }
  function toggleBookmark() {
    var t = tabNow(); if (!t || !t.url || t.url === NEWTAB) return;
    if (isBookmarked(t.url)) S.bookmarks = S.bookmarks.filter(function (b) { return b.url !== t.url; });
    else S.bookmarks.push({ title: t.title || t.url, url: t.url });
    save('bookmarks', S.bookmarks); renderBookmarks(); starState();
  }
  el.star.onclick = toggleBookmark;

  function listDialog(title, items, render, onClick, extra) {
    var q = h('input', { type: 'search', placeholder: 'Search…' }), box = h('div.lst');
    function draw() {
      clear(box);
      var f = q.value.toLowerCase();
      var shownItems = items().filter(function (i) { return !f || (i.title + ' ' + i.url).toLowerCase().indexOf(f) >= 0; }).slice(0, 300);
      if (!shownItems.length) box.appendChild(h('div.li.muted', { text: 'Nothing here.' }));
      shownItems.forEach(function (i) { box.appendChild(h('div.li', { onclick: function () { onClick(i); if (closeFn) closeFn(true); } }, render(i, draw))); });
    }
    var closeFn = null;
    q.oninput = draw; draw();
    return modal({ title: title, width: '640px', setup: function (c) { closeFn = c; }, body: [q, h('div', { style: { height: '8px' } }), box], buttons: [].concat(extra || [], [{ label: 'Close', primary: true, value: true }]) });
  }
  function showBookmarks() {
    listDialog('Bookmarks', function () { return S.bookmarks; }, function (b, redraw) {
      return [h('span.grow.ell', { text: b.title || b.url }), h('span.muted.small.ell', { style: { maxWidth: '220px' }, text: b.url }),
        h('button.btn.sm.flat.danger', { text: '✕', title: 'Remove', onclick: function (e) { e.stopPropagation(); S.bookmarks = S.bookmarks.filter(function (x) { return x !== b; }); save('bookmarks', S.bookmarks); renderBookmarks(); redraw(); } })];
    }, function (b) { Host.send('nav.go', { url: b.url }); });
  }
  function showHistory() {
    listDialog('History', function () { return S.history; }, function (i) {
      return [h('span.grow.ell', { text: i.title }), h('span.muted.small', { text: new Date(i.t).toLocaleString() })];
    }, function (i) { Host.send('nav.go', { url: i.url }); },
    [{ label: 'Clear history', danger: true, get: function () { S.history = []; save('history', []); toast('History cleared', 'ok'); return true; } }]);
  }
  function clearData() {
    var c1 = h('input', { type: 'checkbox', checked: true }), c2 = h('input', { type: 'checkbox', checked: true }), c3 = h('input', { type: 'checkbox' });
    modal({ title: 'Clear browsing data', body: h('div.col', h('label.chk', c1, 'Cookies and site data (signs you out of websites)'), h('label.chk', c2, 'Cached images and files'), h('label.chk', c3, 'Browsing history')),
      buttons: [{ label: 'Cancel', value: false }, { label: 'Clear', primary: true, get: function () {
        if (c1.checked || c2.checked) Host.send('clear.data', { cookies: c1.checked, cache: c2.checked });
        if (c3.checked) { S.history = []; save('history', []); }
        toast('Browsing data cleared', 'ok'); return true; } }] });
  }

  /* ------------------------------------------------------------ find in page */
  function findOpen(on) {
    el.findbar.classList.toggle('hide', !on); reportLayout();
    if (on) { el.ftext.focus(); el.ftext.select(); } else Host.send('find.clear');
  }
  function doFind(fwd) { if (el.ftext.value) Host.send('find', { text: el.ftext.value, forward: fwd, matchCase: el.fcase.checked }); }
  el.ftext.addEventListener('keydown', function (e) { if (e.key === 'Enter') doFind(!e.shiftKey); if (e.key === 'Escape') findOpen(false); });
  el.ftext.addEventListener('input', function () { doFind(true); });
  el.fnext.onclick = function () { doFind(true); }; el.fprev.onclick = function () { doFind(false); }; el.fclose.onclick = function () { findOpen(false); };

  /* ------------------------------------------------------------ automation buttons (the Studio panel does the work) */
  function relay(action, extra) { Host.send('relay', { to: 'panel', msg: Object.assign({ ev: 'ui.action', action: action }, extra || {}) }); }
  el.rec.onclick = function () { if (!S.settings.panelVisible) { S.settings.panelVisible = true; applyPanel(); persistSettings(); } relay('record.toggle'); };
  el.pick.onclick = function () { if (!S.settings.panelVisible) { S.settings.panelVisible = true; applyPanel(); persistSettings(); } relay('pick'); };
  el.run.onclick = function () { relay(S.auto.state === 'idle' ? 'run' : 'stop'); };
  el.ptoggle.onclick = function () { S.settings.panelVisible = !S.settings.panelVisible; applyPanel(); persistSettings(); };
  Host.on('automation', function (m) {
    S.auto = { recording: !!m.recording, state: m.state || 'idle', step: m.step || '' };
    el.rec.classList.toggle('on', S.auto.recording);
    el.rec.textContent = S.auto.recording ? '⏹ Stop recording' : '⏺ Record';
    var running = S.auto.state !== 'idle';
    el.run.textContent = running ? '■ Stop' : '▶ Run';
    el.srun.textContent = ({ idle: 'Ready', running: 'Running flow', paused: 'Paused', stopping: 'Stopping…' })[S.auto.state] || S.auto.state;
    if (S.auto.recording) el.srun.textContent = '● Recording';
    el.srun.className = 'run' + (running || S.auto.recording ? ' running' : '');
    el.sstep.textContent = m.step || '';
  });

  /* ------------------------------------------------------------ ask banner (CAPTCHA, messages, prompts) */
  Host.on('ask', function (m) {
    S.ask = m;
    el.banner.classList.remove('hide');
    el.bmsg.textContent = m.text || '';
    el.binp.classList.toggle('hide', m.kind !== 'input'); el.binp.value = m.def || '';
    el.bok.textContent = m.kind === 'captcha' ? "I've solved it — Continue" : 'OK';
    el.bstop.classList.toggle('hide', m.kind === 'input' && false);
    reportLayout();
    if (m.kind === 'input') el.binp.focus();
  });
  function answer(status) {
    if (!S.ask) return;
    Host.send('ask.answer', { id: S.ask.id, status: status, answer: el.binp.value });
    S.ask = null; el.banner.classList.add('hide'); reportLayout();
  }
  el.bok.onclick = function () { answer(0); }; el.bstop.onclick = function () { answer(2); };
  Host.on('ask.close', function () { S.ask = null; el.banner.classList.add('hide'); reportLayout(); });
  el.binp.addEventListener('keydown', function (e) { if (e.key === 'Enter') answer(0); });

  /* ------------------------------------------------------------ menu */
  function menuItems() {
    var zr = h('div.zoomrow', h('span.ico', { text: '🔍' }), h('span.grow', { text: 'Zoom' }),
      h('button.btn.sm', { text: '−', onclick: function (e) { e.stopPropagation(); Host.send('zoom', { delta: -0.1 }); } }),
      h('span', { style: { minWidth: '42px', textAlign: 'center' }, text: Math.round(S.zoom * 100) + '%' }),
      h('button.btn.sm', { text: '+', onclick: function (e) { e.stopPropagation(); Host.send('zoom', { delta: 0.1 }); } }),
      h('button.btn.sm', { text: 'Reset', onclick: function (e) { e.stopPropagation(); Host.send('zoom', { reset: true }); } }));
    var t = tabNow();
    return [
      ['＋', 'New tab', 'Ctrl+T', function () { Host.send('tab.new', {}); }],
      ['★', t && isBookmarked(t.url) ? 'Remove bookmark' : 'Bookmark this page', 'Ctrl+D', toggleBookmark],
      ['📚', 'Bookmarks…', '', showBookmarks],
      ['🕘', 'History…', 'Ctrl+H', showHistory],
      ['—'], zr, ['—'],
      ['🔎', 'Find in page', 'Ctrl+F', function () { findOpen(true); }],
      ['🖨', 'Print / Save as PDF…', 'Ctrl+P', function () { Host.send('print'); }],
      ['⛶', 'Full screen', 'F11', function () { Host.send('fullscreen'); }],
      ['🧹', 'Clear browsing data…', '', clearData],
      ['🛠', 'Developer tools', 'F12', function () { Host.send('devtools'); }],
      ['—'],
      ['◧', S.settings.panelMode === 'dock' ? 'Float the Studio panel in its own window' : 'Dock the Studio panel', '', function () { S.settings.panelMode = S.settings.panelMode === 'dock' ? 'float' : 'dock'; S.settings.panelVisible = true; applyPanel(); persistSettings(); }],
      ['⚙', 'Settings', '', function () { if (!S.settings.panelVisible) { S.settings.panelVisible = true; applyPanel(); persistSettings(); } relay('tab', { tab: 'settings' }); }],
      ['ℹ', 'About FlowBrowser', '', about]
    ];
  }
  function about() {
    modal({ title: 'About FlowBrowser', body: h('div.col',
      h('div', { style: { fontSize: '18px', fontWeight: 700, color: 'var(--orange)' }, text: 'FlowBrowser ' + S.version }),
      h('div', { text: 'A web automation browser: record, edit and run flows that click, fill, upload and extract — driven by Excel data.' }),
      h('div.muted.small', { text: 'CAPTCHAs: FlowBrowser detects them and waits for you to solve them (or stops). It does not solve or bypass them.' })) });
  }
  function openMenu() {
    clear(el.menu);
    menuItems().forEach(function (it) {
      if (it instanceof Node) { el.menu.appendChild(it); return; }
      if (it[0] === '—') { el.menu.appendChild(h('div.sep')); return; }
      el.menu.appendChild(h('div.it', { onclick: function () { closeMenu(); it[3](); } }, h('span.ico', { text: it[0] }), h('span', { text: it[1] }), h('span.k', { text: it[2] })));
    });
    el.menu.classList.remove('hide'); overlay(1);
  }
  function closeMenu() { if (el.menu.classList.contains('hide')) return; el.menu.classList.add('hide'); overlay(-1); }
  el.menubtn.onclick = function (e) { e.stopPropagation(); if (el.menu.classList.contains('hide')) openMenu(); else closeMenu(); };
  document.addEventListener('mousedown', function (e) { if (!el.menu.contains(e.target) && e.target !== el.menubtn) closeMenu(); });

  /* ------------------------------------------------------------ keyboard shortcuts (the host forwards them from page views too) */
  function shortcut(name) {
    switch (name) {
      case 'newtab': Host.send('tab.new', {}); break;
      case 'closetab': Host.send('tab.close', { id: S.active }); break;
      case 'focusurl': el.url.focus(); break;
      case 'find': findOpen(true); break;
      case 'reload': Host.send('nav.reload'); break;
      case 'back': Host.send('nav.back'); break;
      case 'forward': Host.send('nav.forward'); break;
      case 'bookmark': toggleBookmark(); break;
      case 'history': showHistory(); break;
      case 'print': Host.send('print'); break;
      case 'devtools': Host.send('devtools'); break;
      case 'zoomin': Host.send('zoom', { delta: 0.1 }); break;
      case 'zoomout': Host.send('zoom', { delta: -0.1 }); break;
      case 'zoomreset': Host.send('zoom', { reset: true }); break;
      case 'fullscreen': Host.send('fullscreen'); break;
      case 'record': el.rec.click(); break;
      case 'run': el.run.click(); break;
      case 'panel': el.ptoggle.click(); break;
      case 'nexttab': case 'prevtab': {
        var i = S.tabs.findIndex(function (t) { return t.id === S.active; });
        if (i >= 0 && S.tabs.length) Host.send('tab.activate', { id: S.tabs[(i + (name === 'nexttab' ? 1 : S.tabs.length - 1)) % S.tabs.length].id });
        break; }
      case 'escape': findOpen(false); break;
    }
  }
  Host.on('shortcut', function (m) { shortcut(m.name); });
  var KEYS = { 't': 'newtab', 'w': 'closetab', 'l': 'focusurl', 'f': 'find', 'r': 'reload', 'd': 'bookmark', 'h': 'history', 'p': 'print', '=': 'zoomin', '+': 'zoomin', '-': 'zoomout', '0': 'zoomreset' };
  document.addEventListener('keydown', function (e) {
    var k = e.key.toLowerCase(), c = e.ctrlKey || e.metaKey, n = null;
    if (c && e.shiftKey && k === 'r') n = 'record';
    else if (c && k === 'tab') n = e.shiftKey ? 'prevtab' : 'nexttab';
    else if (c && !e.shiftKey && KEYS[k]) n = KEYS[k];
    else if (e.altKey && k === 'arrowleft') n = 'back'; else if (e.altKey && k === 'arrowright') n = 'forward';
    else if (k === 'f5') n = 'reload'; else if (k === 'f12') n = 'devtools'; else if (k === 'f11') n = 'fullscreen'; else if (k === 'f9') n = 'run'; else if (k === 'f8') n = 'panel';
    if (n) { e.preventDefault(); shortcut(n); }
  });

  Host.on('settings', function (m) { applySettings(m.settings); });
  function applySettings(s) {
    S.settings = Object.assign({}, DEF, s || {});
    applyTheme(S.settings.theme); mirrorSettings(); applyPanel(); renderBookmarks();
  }

  /* ------------------------------------------------------------ start */
  Promise.all([load('settings', {}), load('bookmarks', []), load('history', [])]).then(function (r) {
    S.bookmarks = r[1] || []; S.history = r[2] || [];
    applySettings(r[0]);
    Host.call('app.info').then(function (i) { S.version = i.version || ''; }, function () { /* ignore */ });
    Host.send('shell.ready', {});
  });
  window.FbShell = { S: S, resolveInput: resolveInput };
})();
