# FlowBrowser protocols

Three boundaries, each small enough to replace one side: UI ⇄ host, page tools ⇄ host, runner ⇄ browser.
The Windows host (`src/win`) implements the host side; `tests/ui_mock.js` and `tests/bridge.js` are test doubles for the
same contracts. There are two engines behind it: WebView2 (`src/win/win_wv2.c`, `FlowBrowser.exe`) and Wine's MSHTML
(`src/ie/`, `FlowBrowserWine.exe`); section 4 describes what changes for the second one. Sections 1-3 are the contract
both of them honour.

## 1. UI ⇄ host (shell and Studio panel)

Transport: JSON **strings** through `chrome.webview.postMessage`; the host answers with `PostWebMessageAsJson`.
The host only accepts messages from pages whose origin is `https://appassets.example/` (the embedded UI).

* UI → host: `{ "id"?: n, "cmd": "name", ...args }`
* host → UI reply (only when `id` was sent): `{ "id": n, "ok": true, ...result }` or `{ "id": n, "ok": false, "error": "text" }`
* host → UI event: `{ "ev": "name", ...data }`

Commands (`src/win/win_cmd.c` is the reference; replies listed where the UI reads them):

| Group | Commands |
|---|---|
| Start-up | `shell.ready`, `panel.ready` |
| Storage | `store.get {key}` → `{value}`; `store.set {key, value}` (key `settings` is also applied and broadcast as event `settings`); `app.info` → `{version, dataDir, portable}` |
| Library | `lib.list` → `{flows[{name,title,description,steps,updated,broken}]}`; `lib.load {name, version?}` → `{flow}`; `lib.save {name, flow, keep_versions?}` → `{name}`; `lib.delete {name}`; `lib.versions {name}` → `{versions[{id,when}]}`; `validate {flow}` → `{issues[{path,level,message}]}` |
| Files | `file.dialog {mode: open|save, title, filters:[[desc,pattern]], multi, name}` → `{path|null, paths}`; `file.save_text`; `flow.import` → `{flow|null, path}`; `flow.export {flow, name}` → `{path|null}`; `clipboard {text}`; `open.folder {which: flows|backups|data}` |
| Data | `data.preview {file, sheet, range, limit}` → `{sheets, sheet, rows, total_rows}`; `data.export {var, file, sheet, start_cell, mode, format: xlsx|csv|json, headers}` (writes the variable from the **last run**, all rows) |
| Backup / schedule | `backup.create` → `{path|null, count}`; `backup.restore {mode 0 keep|1 replace|2 both}` → `{path|null, report{added,replaced,skipped,renamed}}`; `schedule.create {name, when, time, day}`; `schedule.list` → `{tasks[{name,next}]}`; `schedule.delete {name}` |
| Page tools | `page.call {name, arg}` → `{result: "<json string>"}`; `page.exec {op, args}` → `{result: object}`; `pick.start {mode}`, `pick.stop`; `rec.start`, `rec.stop` |
| Running | `run {flow, mode: run|step, breakpoints[paths], vars}`; `run.stop`; `run.pause`; `run.resume {action: step|continue}`; `ask.answer {id, status, answer}` |
| Layout | `layout {content:{x,y,w,h}, panel:{x,y,w,h}|null}` in CSS px (host multiplies by the DPI scale); `overlay {on}` hides native views so HTML menus can cover them; `panel {visible, mode: dock|float}`; `relay {to: shell|panel, msg}` |
| Browsing | `tab.new {url?}`, `tab.close {id}`, `tab.activate {id}`, `nav.go {url, user?}`, `nav.back`, `nav.forward`, `nav.reload`, `nav.stop`, `zoom {delta|reset}`, `find {text, forward, matchCase}`, `find.clear`, `print`, `devtools`, `fullscreen`, `clear.data {cookies, cache}` |
| App | `app.exit {code?}`, `app.stay` |

Events host → UI: `tabs {tabs[{id,title,url,loading,favicon,canBack,canForward}], active}`, `nav.done {tab,url,title,ok}`,
`nav.user {url|action}` (user-driven navigation, for the recorder), `zoom {factor}`, `settings {settings}`,
`shortcut {name}` (keys pressed while a page has focus), `panel.closed`, `page.msg {tab, msg}`, `run.event {e}`,
`run.state {state: running|paused|stopping|idle, path?}`, `ask {id, kind, text, def}`, `ask.close`, `app.closing`, `cli {open, run, exit}`.

## 2. Page tools ⇄ host

`src/js/fb_runtime.js` and `src/js/fb_tools.js` are injected into every frame of every content tab
(`AddScriptToExecuteOnDocumentCreated`). `@@FB_TOKEN@@` in `fb_tools.js` is replaced by a random per-run token.

* Steps run as `window.__fb.exec(op, args)` through DevTools `Runtime.evaluate`.
* Tools are controlled with `window.__fb.tools.cmd('{"name":"picker-start","opts":{"mode":"..."}}')` (also `picker-stop`,
  `recorder-start`, `recorder-stop`) and queried with `window.__fb.tools.call(name, argJson)` (`similar`, `categorize`, `test`, `bestSelector`, `describe`).
* The tools post strings `{ "token": "...", "kind": "pick|pick_cancel|rec|tools_ready", ... }` with `chrome.webview.postMessage`.
  The host drops messages with the wrong token and forwards the rest to the panel as `page.msg`. `tools_ready` fires on every
  document load; while recording / picking the host re-sends the start command.

## 3. Runner ⇄ browser (`src/core/fb_runner.h`, `FbBackend`)

| Callback | Contract |
|---|---|
| `cdp(ud, method, params_json, result_buf, err, errsz)` | One DevTools protocol call on the run tab; `result_buf` receives the CDP `result` object. 0 = ok. |
| `navigate(ud, url, wait, timeout_ms, err, errsz)` | Open a URL; if `wait`, return after the page loaded. |
| `wait_event(ud, name, timeout_ms, params_buf)` | `Navigation.started`, `Navigation.completed`, `Page.fileChooserOpened`. 0 = it happened since last consumed, 1 = timeout. `timeout_ms = 0` just drains. |
| `tab(ud, op, arg, err, errsz)` | `new` \| `switch` \| `close` \| `back` \| `forward` \| `reload` |
| `ask(ud, kind, text, def, timeout_ms, answer_buf)` | `captcha` \| `message` \| `input`. 0 continue/answered, 1 timeout, 2 cancelled. |
| `option(ud, name, value)` | `dialogs` = accept \| dismiss \| manual; `user_agent`. |
| `emit(ud, event_json)` | Progress events (`flow_start`, `step_start`, `step_end`, `step_error`, `log`, `var`, `vars`, `loop`, `captcha`, `flow_end`). |
| `checkpoint(ud, path)` | Called before every step. Non-zero aborts. Used for pause, single-step and breakpoints (paths look like `1.body.2`). |

Threading in the Windows host: the runner runs on its own thread; every browser touch is marshalled to the UI thread with
`PostMessage` + an event (`fbw_ui_call`), and the wait also ends when the run is stopped.

## Files

* `.fbflow` - one flow, JSON, `"format": "flowbrowser-flow"`, versioned and migrated on load.
* `.fbbackup` - JSON with every flow inside, `"format": "flowbrowser-backup"`.
* Library history: `flows/.history/<name>__YYYYMMDD-HHMMSS.fbflow`.

## 4. The Wine / MSHTML engine (`FlowBrowserWine.exe`)

MSHTML under Wine (Wine Gecko) has no `chrome.webview`, no DevTools protocol and no web-message channel, so the same three
contracts are carried differently. Nothing above the host changes: the UI, the runner and the step code are shared.

* **UI ⇄ host.** The shell and Studio pages are loaded as local files with `?host=ie` (materialised from the embedded resources
  into `<data>\ui\`). `src/ui/common.js` then queues what the UI sends; the host polls `window.__fbPull()` (about every 30 ms) and
  delivers messages by calling `window.__fbDeliver(json)`. Message shapes are exactly those of section 1. Page scripts are ES5
  (`src/ui/compat.js` supplies the few missing built-ins); `tests/lint_es5.js` enforces it.
* **Page tools ⇄ host.** `src/js/fb_legacy.js` is injected with the runtime and tools. Tools post with `window.__fbPost(s)`
  into a queue that the host polls with `window.__fbPullTools()` (every second tick). Host → page calls are script calls through
  `window.__fbEval(expression)`, which the host injects (MSHTML can only call a named script function with text arguments).
  The token check of section 2 is unchanged.
* **Runner ⇄ browser.** The runner still issues DevTools calls. `src/core/fb_legacy.c` (`fbl_cdp`) answers the subset the runner
  uses: `Runtime.evaluate`, `Input.dispatchMouseEvent`, `Input.dispatchKeyEvent`, `Input.insertText`, `DOM.setFileInputFiles`,
  `Page.setInterceptFileChooserDialog`, `Page.captureScreenshot`, `Network.clearBrowserCookies` / `clearBrowserCache`,
  `Emulation.setUserAgentOverride`, and no-ops for the `*.enable` calls. Input becomes **synthetic DOM events** built by
  `window.__fbLegacy.*` (so `isTrusted` is false). Clicks are deferred one tick while a file chooser interception is armed.
  The host side of the interface is the small `FbLegacyHost` struct in `fb_legacy.h` (`eval`, `set_files`, `chooser`, `screenshot`, `clear_data`, `user_agent`, `stopped`).
* **File chooser.** There is no `Page.fileChooserOpened` event from the engine. The host watches for the native Open dialog
  (`#32770`) on the UI thread, hides it, and raises `Page.fileChooserOpened {"backendNodeId":1}` for the runner;
  `DOM.setFileInputFiles` then fills the dialog's file name box and presses OK (several files only when they share a folder).
  Setting `<input type=file>` directly through script is tried first where Gecko allows it.
* **Screenshots.** `Page.captureScreenshot` is the visible area only, encoded as PNG by `src/core/fb_png.c`, whatever `format` is asked.
* **Threading.** The UI thread owns every COM object. The engine thread (`fbie_cdp`) marshals each operation with
  `fbw_ui_call`; `Runtime.evaluate` requested from the UI thread itself runs on a worker and completes through a posted
  `WM_FBW_CALL`, so it can never deadlock the message loop.
* **Not provided:** the DevTools window, file downloads, closed shadow roots and cross-origin frames. `Navigation.started` / `Navigation.completed` are raised from the browser control's navigation events.

