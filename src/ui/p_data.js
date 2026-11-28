/* Studio panel - Data tab: Excel / CSV input preview, "fill a web form from every row" wizard, save extracted data to Excel. */
(function () {
  'use strict';
  var Data = window.Data = {};
  var D = { file: '', sheet: '', range: '', header: true, sheets: [], rows: [], map: {}, loaded: false };
  var previewBox, mapBox, outBox, fileIn, sheetSel, rangeIn, headerChk;

  function isCsv(f) { return /\.(csv|txt|tsv)$/i.test(f || ''); }

  /* ------------------------------------------------------------ preview */
  function load() {
    var f = fileIn.value.trim(); if (!f) { toast('Choose a file first.', 'err'); return; }
    D.file = f; D.range = rangeIn.value.trim(); D.header = headerChk.checked;
    clear(previewBox); previewBox.appendChild(h('div.muted.small', { text: 'Reading…' }));
    Host.call('data.preview', { file: f, sheet: sheetSel.value || '', range: D.range, limit: 200 }, 30000).then(function (r) {
      D.sheets = r.sheets || []; D.rows = r.rows || []; D.loaded = true;
      clear(sheetSel); sheetSel.appendChild(h('option', { value: '', text: D.sheets.length ? '(first sheet)' : '(n/a)' }));
      D.sheets.forEach(function (s) { sheetSel.appendChild(h('option', { value: s, text: s })); });
      sheetSel.value = r.sheet && D.sheets.indexOf(r.sheet) >= 0 && sheetSel.getAttribute('data-chosen') ? r.sheet : (sheetSel.getAttribute('data-chosen') || '');
      sheetSel.disabled = isCsv(f);
      draw(r);
    }, function (e) { clear(previewBox); previewBox.appendChild(h('div.err', { text: e.message })); D.loaded = false; drawMap(); });
  }
  function columns() {
    if (!D.rows.length) return [];
    if (D.header) return D.rows[0].map(function (c, i) { return String(c == null || c === '' ? 'Column ' + (i + 1) : c); });
    return D.rows[0].map(function (_, i) { return 'Column ' + (i + 1); });
  }
  function draw(r) {
    clear(previewBox);
    var rows = D.rows, cols = columns();
    if (!rows.length) { previewBox.appendChild(h('div.muted.small', { text: 'The sheet is empty.' })); drawMap(); return; }
    var body = rows.slice(D.header ? 1 : 0).map(function (row) { var o = {}; cols.forEach(function (c, i) { o[c] = row[i] == null ? '' : row[i]; }); return o; });
    previewBox.appendChild(h('div.small.muted', { text: (r.total_rows != null ? r.total_rows : rows.length) + ' row(s)' + (D.sheets.length ? ' · sheet "' + (r.sheet || D.sheets[0]) + '"' : '') + ' · columns: ' + cols.join(', ') }));
    previewBox.appendChild(GridView(body, 25));
    previewBox.appendChild(h('div.row.wrap', { style: { marginTop: '8px' } },
      h('button.btn.sm', { text: '＋ Read this data', title: 'Adds "Read Excel" which stores the table in a variable', onclick: function () { P.addSteps(readStep()); } }),
      h('button.btn.sm', { text: '＋ Repeat for every row', title: 'Adds Read Excel + For each row', onclick: function () { var r1 = readStep(), fe = P.newStep('foreach'); fe.source = r1.result_var; fe.as = 'row'; fe.index_as = 'index'; fe.name = 'For each row'; P.addSteps([r1, fe]); } })));
    drawMap();
  }
  function readStep() {
    var s = P.newStep(isCsv(D.file) ? 'csv_read' : 'excel_read');
    s.file = D.file; s.result_var = 'rows'; s.header = D.header; s.name = 'Read ' + basename(D.file);
    if (!isCsv(D.file)) { if (sheetSel.value) s.sheet = sheetSel.value; if (D.range) s.range = D.range; }
    return s;
  }

  /* ------------------------------------------------------------ wizard: fill a web form from every row */
  var KINDS = [['fill', 'Fill (text, date, anything)'], ['select_option', 'Dropdown choice'], ['check', 'Checkbox / radio (yes, true, 1, x = tick)'], ['upload_file', 'Upload file (cell = path)'], ['type_text', 'Type like a person'], ['skip', 'Skip']];
  var submitIn, statusChk, tryChk, statusCol, waitIn;
  function drawMap() {
    clear(mapBox);
    var cols = columns();
    if (!cols.length) { mapBox.appendChild(h('div.muted.small', { text: 'Load an Excel / CSV file above, then match each column to a field on the web page.' })); return; }
    mapBox.appendChild(h('div.muted.small', { text: 'For each column choose how to put it on the page and which field it goes to. Press 🎯 and click the field in the browser.' }));
    cols.forEach(function (c) {
      var m = D.map[c] = D.map[c] || { kind: 'fill', selector: '' };
      var sel = h('input.mono', { type: 'text', value: m.selector, placeholder: 'field selector', spellcheck: 'false' }), kind = h('select', KINDS.map(function (k) { return h('option', { value: k[0], text: k[1] }); }));
      kind.value = m.kind; sel.oninput = function () { m.selector = sel.value; }; kind.onchange = function () { m.kind = kind.value; };
      mapBox.appendChild(h('div.maprow', h('div.mapcol', h('b.ell', { title: c, text: c })), h('div.grow.col', { style: { gap: '3px' } }, h('div.row', h('div.grow', sel), h('button.btn.sm', { text: '🎯', title: 'Pick the field on the page', onclick: function () {
        Elem.pickOnce(function (d) { sel.value = d.selector; m.selector = d.selector; m.pickedSel = d.selector; m.alts = d.alts; if (d.kind === 'select') { kind.value = m.kind = 'select_option'; } else if (d.kind === 'checkbox' || d.kind === 'radio') { kind.value = m.kind = 'check'; } else if (d.kind === 'file') { kind.value = m.kind = 'upload_file'; } });
      } })), kind)));
    });
    submitIn = h('input.mono', { type: 'text', placeholder: 'e.g. text:"Submit"  (optional)', value: D.submit || '', spellcheck: 'false' });
    submitIn.oninput = function () { D.submit = submitIn.value; };
    waitIn = h('input', { type: 'text', value: D.wait != null ? D.wait : '1' }); waitIn.oninput = function () { D.wait = waitIn.value; };
    statusChk = h('input', { type: 'checkbox', checked: !!D.statusOn }); statusChk.onchange = function () { D.statusOn = statusChk.checked; };
    statusCol = h('input', { type: 'text', value: D.statusCol || 'Status', style: { width: '90px' } }); statusCol.oninput = function () { D.statusCol = statusCol.value; };
    tryChk = h('input', { type: 'checkbox', checked: D.tryOn !== false }); tryChk.onchange = function () { D.tryOn = tryChk.checked; };
    mapBox.appendChild(h('div.col', { style: { marginTop: '8px' } },
      h('div.small.muted', { text: 'Button to click after filling each row' }), h('div.row', h('div.grow', submitIn), h('button.btn.sm', { text: '🎯', onclick: function () { Elem.pickOnce(function (d) { submitIn.value = d.selector; D.submit = d.selector; }); } })),
      h('div.row', h('span.small.muted', { text: 'Wait after each row (s)' }), h('div', { style: { width: '60px' } }, waitIn)),
      h('label.chk.small', tryChk, 'If a row fails, note it and continue with the next row'),
      isCsv(D.file) ? null : h('label.chk.small', statusChk, 'Write the result ("Done" / error) into column ', statusCol, ' of the same row'),
      h('div.row', h('button.btn.primary', { text: '✨ Generate the steps', onclick: generate }))));
  }
  function generate() {
    var cols = columns(), body = [];
    cols.forEach(function (c) {
      var m = D.map[c]; if (!m || m.kind === 'skip' || !m.selector.trim()) return;
      var v = P.tpl('row', c), s;
      switch (m.kind) {
        case 'select_option': s = P.newStep('select_option'); s.value = v; break;
        case 'check': s = P.newStep('fill'); s.value = v; break;     /* fill ticks / unticks from yes/true/1/x */
        case 'upload_file': s = P.newStep('upload_file'); s.files = [v]; break;
        case 'type_text': s = P.newStep('type_text'); s.text = v; break;
        default: s = P.newStep('fill'); s.value = v;
      }
      s.selector = m.selector.trim(); if (m.alts && m.alts.length && m.pickedSel === m.selector) s.alts = m.alts;
      s.name = 'Enter ' + c;
      body.push(s);
    });
    if (!body.length) { toast('Give at least one column a field selector.', 'err'); return; }
    if (D.submit && D.submit.trim()) { var cl = P.newStep('click'); cl.selector = D.submit.trim(); cl.name = 'Submit'; body.push(cl); }
    var wt = Number(D.wait != null ? D.wait : 1); if (wt > 0) { var w = P.newStep('wait_seconds'); w.seconds = wt; body.push(w); }
    var inner = body;
    var statusOk = null, statusErr = null;
    if (D.statusOn && !isCsv(D.file)) {
      var col = (D.statusCol || 'Status').trim() || 'Status';
      var colLetter = /^[A-Za-z]{1,3}$/.test(col) ? col.toUpperCase() : null;
      /* column letter if given, else the header position + 1 */
      var idx = colLetter ? null : cols.indexOf(col);
      var letter = colLetter || (idx >= 0 ? colName(idx + 1) : colName(cols.length + 1));
      statusOk = P.newStep('excel_write_cell'); statusOk.file = D.file; statusOk.cell = letter + '{{row._row}}'; statusOk.value = 'Done'; statusOk.value_type = 'text'; statusOk.name = 'Mark row done';
      if (sheetSel.value) statusOk.sheet = sheetSel.value;
      statusErr = P.newStep('excel_write_cell'); statusErr.file = D.file; statusErr.cell = letter + '{{row._row}}'; statusErr.value = 'Failed: {{error}}'; statusErr.value_type = 'text'; statusErr.name = 'Mark row failed';
      if (sheetSel.value) statusErr.sheet = sheetSel.value;
    }
    var loopBody;
    if (D.tryOn !== false) {
      var t = P.newStep('try'); t.name = 'Fill one row'; t.body = inner.concat(statusOk ? [statusOk] : []); t['catch'] = [];
      var lg = P.newStep('log'); lg.message = 'Row {{index}} failed: {{error}}'; lg.level = 'warn'; t['catch'].push(lg);
      if (statusErr) t['catch'].push(statusErr);
      t.error_var = 'error';
      loopBody = [t];
    } else loopBody = inner.concat(statusOk ? [statusOk] : []);
    var rd = readStep(), fe = P.newStep('foreach'); fe.source = rd.result_var; fe.as = 'row'; fe.index_as = 'index'; fe.name = 'For each row'; fe.body = loopBody;
    P.addSteps([rd, fe]);
    Panel.show('flow');
  }
  function colName(n) { var s = ''; while (n > 0) { var m = (n - 1) % 26; s = String.fromCharCode(65 + m) + s; n = Math.floor((n - 1) / 26); } return s; }

  /* ------------------------------------------------------------ save extracted data */
  function tableVars() {
    var out = []; if (P.run.vars) Object.keys(P.run.vars).forEach(function (k) { var v = P.run.vars[k]; if (Array.isArray(v) && v.length && typeof v[0] === 'object') out.push(k); });
    return out;
  }
  function drawOut() {
    clear(outBox);
    var known = P.knownVars(), tv = tableVars();
    var src = h('input', { type: 'text', value: tv[0] || known.filter(function (k) { return /items|table|rows|data|result/.test(k); })[0] || 'items', list: 'dvars' }), dl = h('datalist', { id: 'dvars' }, known.map(function (k) { return h('option', { value: k }); }));
    var file = h('input', { type: 'text', placeholder: 'C:\\Data\\results.xlsx' }), sheet = h('input', { type: 'text', value: 'Sheet1' }), start = h('input', { type: 'text', value: 'A1', style: { width: '70px' } });
    var mode = h('select', [h('option', { value: 'overwrite', text: 'Replace what is there' }), h('option', { value: 'append', text: 'Add below the existing rows' })]), heads = h('input', { type: 'checkbox', checked: true });
    function fld(l, c) { return h('label.col', { style: { gap: '3px' } }, h('span.small.muted', { text: l }), c); }
    outBox.appendChild(h('div.col', dl, fld('Table / list variable to save', src),
      fld('Excel file (.xlsx) — created if missing, other sheets are left untouched', h('div.row', h('div.grow', file), h('button.btn.sm', { text: 'Browse…', onclick: function () { Host.call('file.dialog', { mode: 'save', title: 'Excel file', filters: [['Excel workbook', '*.xlsx']], name: 'results.xlsx' }, 0).then(function (r) { if (r.path) file.value = r.path; }); } }))),
      h('div.row', h('div.grow', fld('Sheet', sheet)), fld('Start at cell', start)), fld('If data is already there', mode), h('label.chk', heads, 'Write column names in the first row (bold)'),
      h('div.row.wrap',
        h('button.btn.primary.sm', { text: '＋ Write table to Excel', onclick: function () { var s = P.newStep('excel_write_table'); s.file = file.value; s.sheet = sheet.value; s.source = src.value; s.start_cell = start.value || 'A1'; s.headers = heads.checked; s.mode = mode.value; s.name = 'Save ' + src.value + ' to Excel'; P.addSteps(s); } }),
        h('button.btn.sm', { text: '＋ Add one row (append)', title: 'Appends a single row, e.g. inside a loop', onclick: function () { var s = P.newStep('excel_append_row'); s.file = file.value; s.sheet = sheet.value; s.source = src.value; s.name = 'Add row to Excel'; P.addSteps(s); } }),
        h('button.btn.sm', { text: '＋ Write one cell', onclick: function () { var s = P.newStep('excel_write_cell'); s.file = file.value; s.sheet = sheet.value; s.cell = start.value || 'A1'; s.value = '{{' + src.value + '}}'; s.name = 'Write ' + (start.value || 'A1'); P.addSteps(s); } })),
      h('div.row.wrap',
        h('button.btn.sm', { text: '＋ Save as CSV', onclick: function () { var s = P.newStep('csv_write'); s.file = (file.value || '').replace(/\.xlsx$/i, '') + '.csv'; s.source = src.value; P.addSteps(s); } }),
        h('button.btn.sm', { text: '＋ Save as JSON', onclick: function () { var s = P.newStep('json_write'); s.file = (file.value || '').replace(/\.xlsx$/i, '') + '.json'; s.source = src.value; P.addSteps(s); } })),
      tv.length ? h('div.small.muted', { text: 'From the last run you can save right now: ' }) : null,
      tv.length ? h('div.row.wrap', tv.map(function (k) { return h('button.btn.sm', { text: '📊 ' + k, onclick: function () { Data.exportVar(k); } }); })) : null));
  }
  Data.exportVar = function (name) {
    var file = h('input', { type: 'text', placeholder: 'C:\\Data\\results.xlsx' }), sheet = h('input', { type: 'text', value: 'Sheet1' }), start = h('input', { type: 'text', value: 'A1' });
    var mode = h('select', [h('option', { value: 'overwrite', text: 'Replace what is there' }), h('option', { value: 'append', text: 'Add below the existing rows' })]);
    var fmt = h('select', [h('option', { value: 'xlsx', text: 'Excel (.xlsx)' }), h('option', { value: 'csv', text: 'CSV' }), h('option', { value: 'json', text: 'JSON' })]);
    function fld(l, c) { return h('label.col', { style: { gap: '3px' } }, h('span.small.muted', { text: l }), c); }
    modal({ title: 'Save "' + name + '" now', body: h('div.col', fld('Format', fmt), fld('File', h('div.row', h('div.grow', file), h('button.btn.sm', { text: 'Browse…', onclick: function () { Host.call('file.dialog', { mode: 'save', title: 'Save as', filters: fmt.value === 'xlsx' ? [['Excel workbook', '*.xlsx']] : fmt.value === 'csv' ? [['CSV', '*.csv']] : [['JSON', '*.json']], name: name + '.' + fmt.value }, 0).then(function (r) { if (r.path) file.value = r.path; }); } }))),
      fld('Sheet (Excel only)', sheet), fld('Start at cell (Excel only)', start), fld('If data is already there (Excel only)', mode)),
      buttons: [{ label: 'Cancel', value: false }, { label: 'Save', primary: true, value: true }] }).then(function (ok) {
      if (!ok) return;
      if (!file.value.trim()) { toast('Enter a file name.', 'err'); return; }
      Host.call('data.export', { 'var': name, file: file.value.trim(), sheet: sheet.value || 'Sheet1', start_cell: start.value || 'A1', mode: mode.value, format: fmt.value, headers: true }, 60000).then(function () { toast('Saved ' + basename(file.value), 'ok'); }, function (e) { toast(e.message, 'err'); });
    });
  };

  /* ------------------------------------------------------------ build */
  Data.init = function (page) {
    fileIn = h('input', { type: 'text', placeholder: 'C:\\Data\\customers.xlsx  or  .csv', spellcheck: 'false' });
    sheetSel = h('select', h('option', { value: '', text: '(first sheet)' })); sheetSel.addEventListener('change', function () { sheetSel.setAttribute('data-chosen', sheetSel.value); load(); });
    rangeIn = h('input', { type: 'text', placeholder: 'A1:F100 (optional)', style: { width: '120px' } });
    headerChk = h('input', { type: 'checkbox', checked: true }); headerChk.onchange = function () { if (D.loaded) { D.header = headerChk.checked; D.map = {}; draw({ total_rows: D.rows.length }); } };
    previewBox = h('div'); mapBox = h('div.col'); outBox = h('div');
    page.appendChild(h('div.pad.col',
      h('h4', { text: '1 · Excel / CSV data to use' }),
      h('div.row', h('div.grow', fileIn), h('button.btn.sm', { text: 'Browse…', onclick: function () { Host.call('file.dialog', { mode: 'open', title: 'Excel or CSV file', filters: [['Excel / CSV', '*.xlsx;*.csv;*.txt'], ['All files', '*.*']] }, 0).then(function (r) { if (r.path) { fileIn.value = r.path; sheetSel.dataset.chosen = ''; D.map = {}; load(); } }); } }), h('button.btn.sm.primary', { text: 'Load', onclick: load })),
      h('div.row', sheetSel, rangeIn, h('label.chk.small', headerChk, 'First row = column names')), previewBox,
      h('div.sep'), h('h4', { text: '2 · Fill a web form from every row' }), mapBox,
      h('div.sep'), h('h4', { text: '3 · Save extracted data to Excel' }), outBox));
    drawMap(); drawOut();
    P.on('run:vars', drawOut); P.on('flow:loaded', drawOut);
  };
  Data.refresh = drawOut;
})();
