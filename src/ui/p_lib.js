/* Studio panel - Library tab: saved flows, import / export, backup & restore, Windows scheduling. */
(function () {
  'use strict';
  var Lib = window.Lib = {};
  var listBox, searchIn, flows = [], schedBox;

  function load() {
    Host.call('lib.list').then(function (r) { flows = r.flows || []; draw(); }, function (e) { clear(listBox); listBox.appendChild(h('div.muted.small.pad', { text: 'Could not read the library: ' + e.message })); });
    loadSched();
  }
  Lib.refresh = load;

  function draw() {
    clear(listBox);
    var f = searchIn.value.trim().toLowerCase();
    var shown = flows.filter(function (x) { return !f || (x.name + ' ' + (x.title || '') + ' ' + (x.description || '')).toLowerCase().indexOf(f) >= 0; });
    if (!shown.length) listBox.appendChild(h('div.welcome', h('div', { style: { fontSize: '40px' }, text: '📚' }), h('p.muted', { text: flows.length ? 'No flow matches.' : 'Your saved flows appear here. Build one in the Flow tab and press Save.' })));
    shown.forEach(function (x) {
      listBox.appendChild(h('div.libitem' + (x.name === P.flowName ? '.cur' : ''),
        h('div.row', h('b.grow.ell', { text: x.title || x.name }), x.broken ? h('span.chip.err', { text: 'unreadable' }) : h('span.chip', { text: (x.steps || 0) + ' steps' })),
        x.title && x.title !== x.name ? h('div.small.muted', { text: 'file: ' + x.name }) : null,
        x.description ? h('div.small.muted.ell', { text: x.description }) : null, h('div.small.muted', { text: x.updated ? 'Saved ' + x.updated : '' }),
        h('div.row.wrap', { style: { marginTop: '4px' } },
          h('button.btn.sm.primary', { text: 'Open', onclick: function () { Flow.open(x.name); } }),
          h('button.btn.sm', { text: '▶ Run', onclick: function () { Flow.open(x.name).then(function () { Run.start('run'); }); } }),
          h('button.btn.sm', { text: 'Copy', onclick: function () { copy(x); } }), h('button.btn.sm', { text: 'Rename', onclick: function () { rename(x); } }),
          h('button.btn.sm', { text: 'Export…', onclick: function () { exportOne(x); } }), h('button.btn.sm', { text: 'Versions', onclick: function () { versions(x); } }),
          h('button.btn.sm', { text: '⏰ Schedule', onclick: function () { schedule(x); } }), h('button.btn.sm.danger', { text: 'Delete', onclick: function () { del(x); } }))));
    });
  }
  function uniqueName(base) { var n = base, i = 2, names = flows.map(function (f) { return f.name.toLowerCase(); }); while (names.indexOf(n.toLowerCase()) >= 0) n = base + ' (' + i++ + ')'; return n; }
  function copy(x) {
    Host.call('lib.load', { name: x.name }).then(function (r) {
      var n = uniqueName(x.name + ' copy'); r.flow.name = n;
      return Host.call('lib.save', { name: n, flow: r.flow, keep_versions: 0 });
    }).then(function () { toast('Copied', 'ok'); load(); }, function (e) { toast(e.message, 'err'); });
  }
  function rename(x) {
    promptBox('Rename flow', x.name).then(function (n) {
      if (!n || !n.trim() || n === x.name) return;
      Host.call('lib.load', { name: x.name }).then(function (r) { r.flow.name = n.trim(); return Host.call('lib.save', { name: n.trim(), flow: r.flow, keep_versions: 0 }); })
        .then(function (r) { return Host.call('lib.delete', { name: x.name }).then(function () { if (P.flowName === x.name) P.flowName = r.name || n.trim(); }); })
        .then(function () { toast('Renamed', 'ok'); load(); Flow.draw(); }, function (e) { toast(e.message, 'err'); });
    });
  }
  function del(x) {
    confirmBox('Delete "' + x.name + '"? Earlier versions are kept for a while in the library\'s .history folder.', 'Delete', true).then(function (ok) {
      if (!ok) return; Host.call('lib.delete', { name: x.name }).then(function () { if (P.flowName === x.name) P.flowName = null; toast('Deleted', 'ok'); load(); Flow.draw(); }, function (e) { toast(e.message, 'err'); });
    });
  }
  function exportOne(x) {
    Host.call('lib.load', { name: x.name }).then(function (r) { return Host.call('flow.export', { flow: r.flow, name: x.name }, 0); }).then(function (r) { if (r.path) toast('Exported to ' + basename(r.path), 'ok'); }, function (e) { toast(e.message, 'err'); });
  }
  function versions(x) {
    Host.call('lib.versions', { name: x.name }).then(function (r) {
      var vs = r.versions || [];
      if (!vs.length) { toast('No earlier versions yet. A version is kept every time you save over a flow.'); return; }
      var closeFn;
      modal({ title: 'Earlier versions of "' + x.name + '"', setup: function (c) { closeFn = c; }, body: h('div.lst', vs.map(function (v) {
        return h('div.li', { onclick: function () { closeFn(true); Flow.confirmDiscard().then(function (ok) { if (!ok) return; Host.call('lib.load', { name: x.name, version: v.id }).then(function (rr) { P.setFlow(rr.flow, x.name); P.dirty = true; Panel.show('flow'); toast('Opened an earlier version. Press Save to keep it.'); }, function (e) { toast(e.message, 'err'); }); }); } },
          h('span.grow', { text: v.when || v.id }), h('span.muted.small', { text: 'open' }));
      })), buttons: [{ label: 'Close', primary: true, value: true }] });
    }, function (e) { toast(e.message, 'err'); });
  }

  /* ------------------------------------------------------------ import / backup */
  function importFile() {
    Flow.confirmDiscard().then(function (ok) {
      if (!ok) return;
      Host.call('flow.import', {}, 0).then(function (r) { if (!r.flow) return; P.setFlow(r.flow, null); P.dirty = true; Panel.show('flow'); toast('Imported "' + (r.flow.name || 'flow') + '". Press Save to add it to the library.', 'ok'); }, function (e) { toast(e.message, 'err'); });
    });
  }
  function backup() {
    Host.call('backup.create', {}, 0).then(function (r) { if (r.path) toast('Backup saved: ' + basename(r.path) + ' (' + (r.count || 0) + ' flows)', 'ok'); }, function (e) { toast(e.message, 'err'); });
  }
  function restore() {
    var mode = '0';
    function radio(v, label, hint) { var r = h('input', { type: 'radio', name: 'rmode', value: v, checked: v === '0' }); r.onchange = function () { mode = v; }; return h('label.col', { style: { gap: '1px' } }, h('span.chk', r, h('b', { text: label })), h('span.small.muted', { style: { marginLeft: '22px' }, text: hint })); }
    modal({ title: 'Restore from a backup file', body: h('div.col', h('p.muted.small', { text: 'Choose what happens when a flow in the backup has the same name as one you already have:' }),
      radio('0', 'Keep my current flows', 'Only flows that do not exist yet are added.'), radio('1', 'Replace my flows with the backup', 'The previous version is kept in the history folder.'), radio('2', 'Keep both', 'Backup copies get a new name.')),
      buttons: [{ label: 'Cancel', value: false }, { label: 'Choose file…', primary: true, value: true }] }).then(function (ok) {
      if (!ok) return;
      Host.call('backup.restore', { mode: Number(mode) }, 0).then(function (r) {
        if (!r.path) return;
        var rep = r.report || {};
        toast('Restored: ' + (rep.added || 0) + ' added, ' + (rep.replaced || 0) + ' replaced, ' + (rep.skipped || 0) + ' skipped' + (rep.renamed ? ', ' + rep.renamed + ' renamed' : ''), 'ok'); load();
      }, function (e) { toast(e.message, 'err'); });
    });
  }

  /* ------------------------------------------------------------ Windows Task Scheduler */
  function schedule(x) {
    var when = h('select', [['daily', 'Every day'], ['weekly', 'Every week'], ['hourly', 'Every hour'], ['logon', 'When I sign in to Windows'], ['once', 'Once (today)']].map(function (o) { return h('option', { value: o[0], text: o[1] }); }));
    var time = h('input', { type: 'text', value: '09:00', style: { width: '80px' } }), day = h('select', ['MON', 'TUE', 'WED', 'THU', 'FRI', 'SAT', 'SUN'].map(function (d) { return h('option', { value: d, text: d }); }));
    modal({ title: 'Run "' + x.name + '" automatically', body: h('div.col', h('p.muted.small', { text: 'Uses the Windows Task Scheduler. FlowBrowser opens, runs the flow, then closes. You must be signed in (the browser needs a screen).' }),
      h('label.col', h('span.small.muted', { text: 'When' }), when), h('div.row', h('label.col', h('span.small.muted', { text: 'Time (24h)' }), time), h('label.col', h('span.small.muted', { text: 'Day (weekly)' }), day))),
      buttons: [{ label: 'Cancel', value: false }, { label: 'Create', primary: true, value: true }] }).then(function (ok) {
      if (!ok) return;
      Host.call('schedule.create', { name: x.name, when: when.value, time: time.value, day: day.value }, 30000).then(function () { toast('Scheduled', 'ok'); loadSched(); }, function (e) { toast(e.message, 'err'); });
    });
  }
  function loadSched() {
    if (!schedBox) return;
    Host.call('schedule.list', {}, 20000).then(function (r) {
      clear(schedBox);
      var rows = (r.tasks || []);
      if (!rows.length) { schedBox.appendChild(h('div.small.muted', { text: 'No scheduled flows.' })); return; }
      rows.forEach(function (t) {
        schedBox.appendChild(h('div.row', h('span.grow.ell', { text: t.name }), h('span.muted.small', { text: t.next || '' }),
          h('button.btn.sm.flat.danger', { text: '✕', title: 'Remove schedule', onclick: function () { Host.call('schedule.delete', { name: t.name }).then(loadSched, function (e) { toast(e.message, 'err'); }); } })));
      });
    }, function () { clear(schedBox); schedBox.appendChild(h('div.small.muted', { text: 'Scheduling is not available here.' })); });
  }

  Lib.init = function (page) {
    searchIn = h('input', { type: 'search', placeholder: 'Search flows…' }); searchIn.oninput = draw;
    listBox = h('div.liblist'); schedBox = h('div.col');
    page.appendChild(h('div.toolbar', h('div.grow', searchIn), h('button.btn.sm', { text: 'Open file…', title: 'Open a .fbflow file from anywhere', onclick: importFile })));
    page.appendChild(h('div.toolbar', h('button.btn.sm', { text: '💾 Back up all flows…', onclick: backup }), h('button.btn.sm', { text: '♻ Restore…', onclick: restore }), h('span.grow'),
      h('button.btn.sm.flat', { text: 'Open folder', onclick: function () { Host.send('open.folder', { which: 'flows' }); } })));
    page.appendChild(listBox);
    page.appendChild(h('div.pad', h('h4', { text: 'Scheduled runs' }), schedBox));
    P.on('lib:changed', load);
    load();
  };
})();
