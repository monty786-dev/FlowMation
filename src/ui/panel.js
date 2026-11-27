/* Studio panel bootstrap: tabs, routing of host events, startup. */
(function () {
  'use strict';
  var TABS = [['flow', '🧩', 'Flow'], ['recorder', '⏺', 'Record'], ['elements', '🎯', 'Elements'], ['data', '📊', 'Data'], ['vars', '𝑥', 'Variables'], ['log', '📜', 'Run log'], ['library', '📚', 'Library'], ['settings', '⚙', 'Settings']];
  var pages = {}, tabBtns = {}, cur = 'flow';

  window.Panel = {
    show: function (name) {
      if (!pages[name]) return;
      cur = name;
      Object.keys(pages).forEach(function (k) { pages[k].classList.toggle('on', k === name); tabBtns[k].classList.toggle('on', k === name); });
      if (name === 'library') Lib.refresh();
      if (name === 'data') Data.refresh();
      if (name === 'flow') Flow.draw();
    },
    current: function () { return cur; }
  };

  function build() {
    var nav = $('#tabs'), main = $('#pages');
    TABS.forEach(function (t) {
      tabBtns[t[0]] = h('button.tabbtn', { title: t[2], onclick: function () { Panel.show(t[0]); } }, h('span.i', { text: t[1] }), h('span.l', { text: t[2] }));
      nav.appendChild(tabBtns[t[0]]);
      pages[t[0]] = h('section.page#pg-' + t[0]); main.appendChild(pages[t[0]]);
    });
    Run.initBar($('#runhost'));
    Flow.init(pages.flow); Rec.init(pages.recorder); Elem.init(pages.elements); Data.init(pages.data);
    Run.initVars(pages.vars); Run.initLog(pages.log); Lib.init(pages.library); Settings.init(pages.settings);
  }

  /* ------------------------------------------------------------ host events */
  Host.on('page.msg', function (m) {
    var d = m.msg || {};
    if (d.kind === 'rec') Rec.onMsg(d); else if (d.kind === 'pick' || d.kind === 'pick_cancel') Elem.onMsg(d);
  });
  Host.on('tabs', function (m) { P.tabs = m.tabs || []; P.activeTab = m.active; });
  Host.on('settings', function (m) { P.settings = Settings.merge(m.settings); applyTheme(P.settings.theme); P.emit('settings:changed'); });
  Host.on('ui.action', function (m) {
    switch (m.action) {
      case 'record.toggle': Rec.toggle(); break;
      case 'pick': Panel.show('elements'); $('#pg-elements .btn.primary').click(); break;
      case 'run': Panel.show('flow'); Run.start('run'); break;
      case 'stop': Run.stop(); break;
      case 'tab': Panel.show(m.tab); break;
    }
  });
  Host.on('app.closing', function () {
    var dirty = (P.dirty && P.flow && P.flow.steps.length) || P.rec.steps.length;
    if (!dirty) { Host.send('app.exit', {}); return; }
    confirmBox('You have unsaved work (' + (P.rec.steps.length && !(P.dirty && P.flow.steps.length) ? 'a recording' : 'a flow') + '). Close FlowBrowser and discard it?', 'Discard and close', true).then(function (ok) { if (ok) Host.send('app.exit', {}); else Host.send('app.stay', {}); });
  });
  Host.on('cli', function (m) {
    if (m.open) Flow.open(m.open).then(function () { if (m.run) { P.cliExit = !!m.exit; Run.start('run'); } });
  });
  P.on('run:state', function (st) { if (st === 'idle' && P.cliExit) { P.cliExit = false; setTimeout(function () { Host.send('app.exit', { code: P.run.log.some(function (l) { return l.level === 'error'; }) ? 1 : 0 }); }, 800); } });

  document.addEventListener('keydown', function (e) {
    if (e.key === 'F9') { e.preventDefault(); Run.start('run'); }
    else if (e.key === 'F10' && P.run.state === 'paused') { e.preventDefault(); Run.resume('step'); }
    else if (e.key === 'Escape' && P.run.state === 'running' && !document.querySelector('.modal-bg')) { /* never stop on Escape: too easy to hit */ }
  });

  /* ------------------------------------------------------------ start */
  Promise.all([P.loadCatalog(), Host.call('store.get', { key: 'settings' }).then(function (r) { return r.value; }, function () { return {}; })]).then(function (r) {
    P.settings = Settings.merge(r[1]); applyTheme(P.settings.theme);
    P.flow = P.newFlow('New flow');
    build();
    P.setFlow(P.newFlow('New flow'), null);
    Panel.show('flow');
    Host.send('panel.ready', {});
  }).then(null, function (e) { document.body.appendChild(h('pre', { style: { padding: '12px', color: 'crimson' }, text: 'FlowBrowser Studio could not start: ' + (e && e.message) })); });
})();
