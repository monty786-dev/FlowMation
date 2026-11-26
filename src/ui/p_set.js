/* Studio panel - Settings tab. Everything is saved immediately in the "settings" store; the host and the shell react to it. */
(function () {
  'use strict';
  var Settings = window.Settings = {};
  var DEF = { theme: 'light', home: '', search: 'https://duckduckgo.com/?q=%s', bookmarksBar: true, defaultZoom: 100, userAgent: '', proxy: '', popups: 'tab', permissions: 'ask',
    clearOnExit: false, keepVersions: 10, autoBackup: true, automation: { timeout_ms: 15000, step_delay_ms: 0, on_error: 'stop', captcha: 'pause', dialogs: 'accept' } };
  var ENGINES = [['https://duckduckgo.com/?q=%s', 'DuckDuckGo'], ['https://www.google.com/search?q=%s', 'Google'], ['https://www.bing.com/search?q=%s', 'Bing'], ['https://search.brave.com/search?q=%s', 'Brave'], ['https://www.ecosia.org/search?q=%s', 'Ecosia']];

  Settings.merge = function (s) { var o = Object.assign({}, DEF, s || {}); o.automation = Object.assign({}, DEF.automation, (s || {}).automation || {}); return o; };
  function save(msg) { Host.send('store.set', { key: 'settings', value: P.settings }); applyTheme(P.settings.theme); if (msg) toast(msg, 'ok', 1500); }

  function sect(title, body, open) { return h('details.sect', { open: open }, h('summary', { text: title }), h('div.col.sbody', body)); }
  function fld(l, c, hint) { return h('label.col', { style: { gap: '3px' } }, h('span.small.muted', { text: l }), c, hint ? h('span.small.muted', { text: hint }) : null); }
  function bindText(obj, key, ph, after) { var i = h('input', { type: 'text', value: obj[key] == null ? '' : obj[key], placeholder: ph || '', spellcheck: 'false' }); i.addEventListener('change', function () { obj[key] = i.value.trim(); save(); if (after) after(); }); return i; }
  function bindNum(obj, key) { var i = h('input', { type: 'text', value: obj[key] == null ? '' : obj[key] }); i.addEventListener('change', function () { var n = Number(i.value); if (isFinite(n)) obj[key] = n; save(); }); return i; }
  function bindSel(obj, key, opts, after) { var s = h('select', opts.map(function (o) { return h('option', { value: o[0], text: o[1] }); })); s.value = obj[key]; s.addEventListener('change', function () { obj[key] = s.value; save(); if (after) after(); }); return s; }
  function bindChk(obj, key, label) { var c = h('input', { type: 'checkbox', checked: !!obj[key] }); c.addEventListener('change', function () { obj[key] = c.checked; save(); }); return h('label.chk', c, label); }

  Settings.draw = function (page) {
    clear(page);
    var s = P.settings, a = s.automation;
    var themes = [['light', 'Light', '#F6F5F4', '#E95420'], ['dark', 'Dark', '#2B2B2B', '#E95420'], ['aubergine', 'Aubergine', '#3B1030', '#E95420']];
    var tcards = h('div.row.wrap', themes.map(function (t) {
      return h('div.themecard' + (s.theme === t[0] ? '.on' : ''), { onclick: function () { s.theme = t[0]; save(); Settings.draw(page); } }, h('div.sw', { style: { background: t[2] } }, h('span', { style: { background: t[3] } })), h('div.small', { text: t[1] }));
    }));
    var engine = h('select', ENGINES.map(function (e) { return h('option', { value: e[0], text: e[1] }); }).concat([h('option', { value: '__c', text: 'Custom…' })]));
    engine.value = ENGINES.some(function (e) { return e[0] === s.search; }) ? s.search : '__c';
    var custom = h('input', { type: 'text', value: s.search, placeholder: 'https://example.com/?q=%s', spellcheck: 'false' }); custom.classList.toggle('hide', engine.value !== '__c');
    engine.onchange = function () { if (engine.value === '__c') { custom.classList.remove('hide'); custom.focus(); } else { custom.classList.add('hide'); s.search = engine.value; save(); } };
    custom.onchange = function () { if (/%s/.test(custom.value)) { s.search = custom.value.trim(); save(); } else toast('The address must contain %s where the search words go.', 'err'); };

    page.appendChild(h('div.pad.col',
      sect('Appearance', [tcards, bindChk(s, 'bookmarksBar', 'Show the bookmarks bar'), fld('Page zoom for new tabs (%)', bindNum(s, 'defaultZoom'))], true),
      sect('CAPTCHA and "I am not a robot"', [
        h('p.small', { html: '<b>FlowBrowser does not solve CAPTCHAs.</b> They exist to stop automated access, so bypassing them is not something this app does. Instead it <b>detects</b> them and lets <b>you</b> solve them, then carries on automatically.' }),
        fld('When a flow reaches a CAPTCHA (default for new flows)', bindSel(a, 'captcha', [['pause', 'Pause and ask me to solve it, then continue'], ['stop', 'Stop the flow'], ['off', 'Off: do not check for CAPTCHAs']]), 'Each flow can override this in its ⚙ Flow settings. The step "Check for CAPTCHA" can be placed anywhere in a flow.'),
        h('p.small.muted', { text: 'Tip: sites with an official API or an "automation friendly" login (API keys, app passwords) are more reliable than a flow that has to pass a CAPTCHA each time.' })], true),
      sect('Browser', [fld('Home page (empty = start page)', bindText(s, 'home', 'https://')), fld('Search engine', h('div.col', engine, custom)),
        fld('User agent (empty = default)', bindText(s, 'userAgent', 'Mozilla/5.0 …')), fld('Proxy server (restart needed)', bindText(s, 'proxy', 'host:port  or  socks5://host:port')),
        fld('Pop-up windows', bindSel(s, 'popups', [['tab', 'Open in a new tab'], ['block', 'Block']])), fld('Camera, microphone, location, notifications', bindSel(s, 'permissions', [['ask', 'Ask each time'], ['deny', 'Always block']])),
        bindChk(s, 'clearOnExit', 'Delete cookies and cache when FlowBrowser closes')]),
      sect('Automation defaults for new flows', [fld('Wait up to (ms) for elements and pages', bindNum(a, 'timeout_ms')), fld('Pause between steps (ms)', bindNum(a, 'step_delay_ms')),
        fld('When a step fails', bindSel(a, 'on_error', [['stop', 'Stop the flow'], ['continue', 'Skip the step and continue']])), fld('JavaScript alert / confirm dialogs', bindSel(a, 'dialogs', [['accept', 'Accept'], ['dismiss', 'Dismiss'], ['manual', 'Let me answer']]))]),
      sect('Library and backup', [fld('Earlier versions kept per flow', bindNum(s, 'keepVersions')), bindChk(s, 'autoBackup', 'Make a backup of all flows each time FlowBrowser closes (last 7 kept)'),
        h('div.row.wrap', h('button.btn.sm', { text: 'Open flows folder', onclick: function () { Host.send('open.folder', { which: 'flows' }); } }), h('button.btn.sm', { text: 'Open backups folder', onclick: function () { Host.send('open.folder', { which: 'backups' }); } }), h('button.btn.sm', { text: 'Open data folder', onclick: function () { Host.send('open.folder', { which: 'data' }); } }))]),
      sect('Keyboard shortcuts', [h('div.small', { html: '<span class="kbd">Ctrl+T</span> new tab · <span class="kbd">Ctrl+W</span> close · <span class="kbd">Ctrl+L</span> address · <span class="kbd">Ctrl+F</span> find · <span class="kbd">Ctrl+D</span> bookmark · <span class="kbd">F5</span> reload · <span class="kbd">F12</span> developer tools · <span class="kbd">Ctrl+Shift+R</span> record · <span class="kbd">F9</span> run · <span class="kbd">F8</span> panel · <span class="kbd">Ctrl+S</span> save flow · <span class="kbd">Ctrl+Z</span> undo' })])));
  };
  Settings.init = function (page) { Settings.draw(page); P.on('settings:changed', function () { Settings.draw(page); }); };
})();
