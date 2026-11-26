/* Studio panel - Recorder (macro) tab: records what the person does in the page, lists it as editable steps. */
(function () {
  'use strict';
  var Rec = window.Rec = {};
  var meta = new WeakMap(), list, startBtn, stopBtn, info, expanded = null, startWithUrl;

  function now() { return P.rec; }
  function setRecording(on) {
    P.rec.on = on;
    startBtn.classList.toggle('hide', on); stopBtn.classList.toggle('hide', !on);
    info.textContent = on ? '● Recording: do the task in the browser. Clicks, typing, choices, uploads and drag & drop are recorded.' : (P.rec.steps.length ? P.rec.steps.length + ' recorded step(s). Edit them below, then add them to your flow.' : 'Press Record, then work on the page as you normally would.');
    info.className = 'rec-info' + (on ? ' live' : '');
    Run.notify();
  }

  Rec.start = function () {
    if (P.rec.on) return;
    Host.call('rec.start', {}).then(function (r) {
      if (r.warn) toast(r.warn, 'err');
      setRecording(true);
      if (startWithUrl.checked && !P.rec.steps.length) {
        var u = P.pageUrl();
        if (u && /^(https?|file):/i.test(u)) { P.rec.steps.push({ type: 'navigate', name: 'Open ' + u.replace(/^https?:\/\//, '').slice(0, 40), url: u, wait: true }); }
      }
      draw();
    }, function (e) { toast('Could not start recording: ' + e.message, 'err'); });
  };
  Rec.stop = function () { if (!P.rec.on) return; Host.send('rec.stop'); setRecording(false); draw(); if (P.rec.steps.length) toast(P.rec.steps.length + ' steps recorded', 'ok'); };
  Rec.toggle = function () { Panel.show('recorder'); if (P.rec.on) Rec.stop(); else Rec.start(); };

  /* a message from the page tools */
  Rec.onMsg = function (m) {
    if (m.kind !== 'rec' || !P.rec.on || !m.step) return;
    var step = m.step, steps = P.rec.steps;
    if (m.secret_var) {
      var v = m.secret_var;
      var have = steps.some(function (s) { return s.type === 'ask_input' && s.result_var === v; }) || (P.flow && JSON.stringify(P.flow.steps).indexOf('"result_var":"' + v + '"') >= 0);
      if (!have) { steps.push({ type: 'ask_input', name: 'Ask for ' + v, prompt: 'Enter the ' + v + ' for this run', default: '', result_var: v }); toast('The password is never recorded: the flow asks you for it when it runs.'); }
    }
    var last = steps[steps.length - 1];
    if (m.merge_key && last && meta.get(last) && meta.get(last).key === m.merge_key) {      /* typing: keep only the final value */
      steps[steps.length - 1] = step; meta.set(step, { key: m.merge_key });
    } else { steps.push(step); if (m.merge_key) meta.set(step, { key: m.merge_key }); }
    if (m.needs_file_path) {
      meta.set(step, Object.assign(meta.get(step) || {}, { needsFile: true }));
      Host.call('file.dialog', { mode: 'open', multi: true, title: 'Which file did you pick? Browsers do not reveal the path', filters: [['All files', '*.*']] }, 0).then(function (r) {
        var paths = r.paths || (r.path ? [r.path] : []);
        if (paths.length) { step.files = paths; var mm = meta.get(step); if (mm) mm.needsFile = false; draw(); }
      }, function () { /* leave flagged */ });
    }
    draw();
  };
  Host.on('nav.user', function (m) {
    if (!P.rec.on) return;
    if (m.action === 'back') P.rec.steps.push({ type: 'back', name: 'Go back' });
    else if (m.action === 'forward') P.rec.steps.push({ type: 'forward', name: 'Go forward' });
    else if (m.action === 'reload') P.rec.steps.push({ type: 'reload', name: 'Reload page' });
    else if (m.url) P.rec.steps.push({ type: 'navigate', name: 'Open ' + m.url.replace(/^https?:\/\//, '').slice(0, 40), url: m.url, wait: true });
    draw();
  });
  P.on('rec:changed', function () { draw(); });

  function move(i, d) { var a = P.rec.steps, j = i + d; if (j < 0 || j >= a.length) return; var t = a[i]; a[i] = a[j]; a[j] = t; draw(); }

  function draw() {
    if (!list) return;
    clear(list);
    var steps = P.rec.steps;
    if (!steps.length) list.appendChild(h('div.welcome', h('div', { style: { fontSize: '40px' }, text: '⏺' }), h('p.muted', { text: P.rec.on ? 'Waiting for your first action…' : 'No recorded steps yet.' })));
    steps.forEach(function (s, i) {
      var sm = P.summary(s), mm = meta.get(s) || {};
      var row = h('div.step.rec' + (expanded === s ? '.sel' : ''), { onclick: function (e) { if (e.target.closest('.act')) return; expanded = expanded === s ? null : s; draw(); } },
        h('span.grip.muted', { text: String(i + 1) }), h('span.ico', { text: sm.icon }),
        h('div.txt.grow', h('b', { text: sm.label }), sm.detail ? h('span.det', { text: ' ' + sm.detail }) : null,
          mm.needsFile ? h('div.iss.e', { text: 'Choose the real path of the file (click to edit).' }) : null),
        h('span.act', h('button.btn.sm.flat.icon', { text: '↑', title: 'Move up', onclick: function () { move(i, -1); } }), h('button.btn.sm.flat.icon', { text: '↓', title: 'Move down', onclick: function () { move(i, 1); } }),
          h('button.btn.sm.flat.icon', { text: '⎘', title: 'Duplicate', onclick: function () { steps.splice(i + 1, 0, cloneJ(s)); draw(); } }),
          h('button.btn.sm.flat.icon.danger', { text: '✕', title: 'Delete', onclick: function () { steps.splice(i, 1); if (expanded === s) expanded = null; draw(); } })));
      list.appendChild(row);
      if (expanded === s) list.appendChild(h('div.ibody.inline', Form.render(s, { key: 'rec' + i, snap: function () {}, onChange: function () { /* in place */ } })));
    });
    if (steps.length) list.appendChild(h('div.row', { style: { padding: '8px' } }, h('button.btn.sm', { text: '＋ Add a step here', onclick: function () { Flow.palette(function (t) { steps.push(P.newStep(t)); expanded = steps[steps.length - 1]; draw(); }); } })));
    var any = steps.length > 0;
    $$('.needs-steps', list.parentNode).forEach(function (b) { b.disabled = !any; });
  }

  function useSteps(mode) {
    var steps = cloneJ(P.rec.steps);
    if (!steps.length) return;
    if (mode === 'append') { P.addSteps(steps, 'flow'); done(); Panel.show('flow'); }
    else if (mode === 'replace') { confirmBox('Replace all steps of the current flow with the recording?', 'Replace', true).then(function (ok) { if (!ok) return; P.snap(); P.flow.steps = steps; P.sel = null; P.changed(); done(); Panel.show('flow'); }); }
    else if (mode === 'new') { Flow.confirmDiscard().then(function (ok) { if (!ok) return; var f = P.newFlow('Recorded flow'); f.steps = steps; P.setFlow(f, null); P.dirty = true; done(); Panel.show('flow'); Flow.save(true); }); }
  }
  function done() { P.rec.steps = []; expanded = null; setRecording(P.rec.on); draw(); }

  Rec.init = function (page) {
    startBtn = h('button.btn.primary', { text: '⏺ Start recording', onclick: Rec.start });
    stopBtn = h('button.btn.danger.hide', { text: '⏹ Stop recording', onclick: Rec.stop });
    info = h('div.rec-info');
    startWithUrl = h('input', { type: 'checkbox', checked: true });
    list = h('div.tree.rec');
    page.appendChild(h('div.pad.col', h('div.row', startBtn, stopBtn, h('span.grow'),
      h('button.btn.sm.needs-steps', { text: '▷ Replay', title: 'Run the recorded steps now to check them', onclick: function () { Run.runSteps(cloneJ(P.rec.steps)); } }),
      h('button.btn.sm.danger.needs-steps', { text: 'Discard', onclick: function () { confirmBox('Throw away the recording?', 'Discard', true).then(function (ok) { if (ok) done(); }); } })),
      info, h('label.chk.small', startWithUrl, 'Start with "Go to" the current page address')));
    page.appendChild(list);
    page.appendChild(h('div.pad.footer', h('div.small.muted', { text: 'Add the recording to your flow:' }), h('div.row.wrap',
      h('button.btn.sm.primary.needs-steps', { text: 'Add to current flow', onclick: function () { useSteps('append'); } }),
      h('button.btn.sm.needs-steps', { text: 'Replace flow steps', onclick: function () { useSteps('replace'); } }),
      h('button.btn.sm.needs-steps', { text: 'Save as a new flow', onclick: function () { useSteps('new'); } }))));
    setRecording(false); draw();
  };
})();
