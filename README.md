# FlowBrowser

A Windows web browser built for **web automation**: record or build a flow, point it at Excel data, extract tables and
lists back into Excel, run it now or on a schedule. Think "Power Automate for web", as one small `.exe`
written in C, with an Ubuntu-style (Yaru) interface.

The browser engine is Microsoft Edge WebView2 (the same Chromium that Edge uses). Everything else - the flow engine,
Excel reader/writer, expression language, library and backups - is C in this repository.

> **Read this first**
>
> * **CAPTCHAs are not solved automatically.** FlowBrowser detects them and either *pauses* so you can solve them
>   (then the flow carries on), *stops*, or ignores the check. Tools that defeat CAPTCHAs bypass a protection the site
>   owner put there on purpose, so this project does not include one. Settings → Automation → *When a flow reaches a
>   CAPTCHA* is the on/off switch.
> * **How much of this has been run.** The portable core (flow engine, Excel/CSV/JSON, expressions, library/backup),
>   the in-page tools (picker, recorder, similar-item detection, selector tester), the runner against a real Chromium
>   and the whole Studio UI (against a mock host) have automated tests that pass (see [Testing](#testing)).
>   The **Windows host** (`src/win/`: window, tabs, WebView2 hookup, file dialogs, scheduling) was written without
>   access to a Windows compiler. It was only syntax-checked against stand-in headers. **Expect to fix a few build errors
>   or small bugs the first time you build it on Windows**; the CI job below shows them immediately.

---

## What it does

| You asked for | Where it is |
|---|---|
| Click any element, extract all data / tables / headings, with similar items grouped while you build | **Elements** tab: *Pick* any element, *Scan page* groups tables, repeated items (cards, rows, list entries), headings, links, images, text blocks. A picked element shows "N similar items" and turns into an *Extract list / table* step in one click. Selectors are tested live (matches are flashed on the page). |
| Fill every kind of input, especially **file upload** | Steps: `fill`, `select_option` (dropdowns, combo boxes, multi-select), `check` (checkboxes, radios, toggles), `set_slider` (range / adjustment bars), `type_text`, `press_key`, `autofill_form` (matches column names to form labels), `drag_drop`, and `upload_file` with four modes: file input, the native file chooser (intercepted, no dialog is shown), drag-and-drop onto a drop zone, or *auto*. |
| Use Excel to fill inputs / click buttons / follow links and write results back to a file, sheet or cell range | **Data** tab: preview any `.xlsx` / `.csv`; the *Fill a form from every row* wizard builds the whole loop (read rows, fill, upload, click, write a status back into the row). Steps `excel_read`, `excel_write_cell`, `excel_append_row`, `excel_write_table` (start at any cell, overwrite or append), `csv_*`, `json_write`. Results of the last run can be saved to Excel from the **Variables** tab (all rows, not just the preview). |
| Solve CAPTCHAs, with an on/off setting | See the note above: detect, then pause / stop / off. |
| Ubuntu theme | Yaru-style light, dark and aubergine themes, orange accent, Ubuntu font if installed. |
| Browser settings and features | Tabs, address/search bar, bookmarks and bar, history, find in page, zoom, full screen, print, DevTools, downloads, pop-up policy, permission policy (ask / deny), custom user agent, proxy, home page, clear cookies / cache, clear-on-exit. |
| **Recorder (macro)** in a window or panel, editable | **Record** tab: press ⏺, use the page normally; clicks, typing, selections, uploads and navigation become steps. Passwords are never recorded (an *Ask for input* step is inserted instead). Reorder, edit, delete, merge into the current flow, or start a new one. The Studio panel docks on the right or floats as its own window. |
| Save, back up and restore automations by file | **Library** tab: save, copy, rename, delete, open earlier versions (kept automatically), export / open single `.fbflow` files, back up everything to one `.fbbackup` file, restore it (keep mine / replace / keep both). Automatic backups on exit (last 7 kept). |
| Power Automate-style features, plus extras for precision | Variables and `{{ }}` expressions with filters, `if` / `foreach` / `while` / `repeat` / `try` / `break` / `continue` / sub-flows (`run_flow`), retries and timeouts per step, alternative selectors (self-healing), step-through debugging (run only one step, pause, step, **breakpoints**), per-step run log with timings, flow validation before running, input prompts and message boxes, JavaScript step, screenshots, Windows Task Scheduler integration, command line. |

### Precision features worth knowing

* **Selectors** accept `css:`, `xpath:`, `text:`, `label:`, `placeholder:`, `id:`, `name:`, `testid:`, `aria:`, `alt:`,
  `title:`, `nth=` and chains with ` >> ` that pass through iframes and open shadow DOM. Each step can list `alts`
  (fallback selectors) so a changed page does not break the flow.
* **Real input events** (mouse, keyboard) go through the browser's DevTools protocol, so sites that ignore synthetic
  events still work. Waits are condition-based (element, text, page load, or a condition you write), not only fixed sleeps.
* **Extraction** keeps column names, handles `colspan` / `rowspan`, repeated cards, "Next page" buttons and infinite scroll, and writes
  values as text - scraped data can never become an Excel formula (cells are escaped; CSV has *protect formulas* on by default).
* **Dialogs** (`alert` / `confirm` / `prompt` / `beforeunload`) are accepted, dismissed or left for you, per flow.
* **Templates**: `{{ row.Name | trim | upper }}`, `{{ price | number | round }}`, `{{ items | length }}`, dates, default values.

---

## Build the .exe from GitHub

You need nothing installed locally. GitHub builds it for you.

1. **Create a repository.** On github.com click *New repository*, name it (for example `flowbrowser`), leave it empty.
2. **Upload this project.** Either drag-and-drop the contents of this folder into the repository in the browser, or
   from a terminal in this folder:
   ```
   git init
   git add .
   git commit -m "FlowBrowser"
   git branch -M main
   git remote add origin https://github.com/<you>/flowbrowser.git
   git push -u origin main
   ```
   Make sure the `.github/workflows/build.yml` file is included (it is a hidden folder).
3. **Watch the build.** Open the repository → **Actions** → *Build FlowBrowser*. It runs automatically on every push
   (or press *Run workflow*). The Windows job fetches the WebView2 SDK, configures with CMake and compiles with
   Visual Studio's C compiler.
4. **Download the .exe.** Open the finished run → **Artifacts** → `FlowBrowser-windows-x64` (and `-Win32` for 32-bit).
   Unzip and run `FlowBrowser.exe`.
5. **Publish a release (optional).** Push a tag such as `v1.0.0` (`git tag v1.0.0 && git push --tags`); the workflow attaches
   the zips to a GitHub Release.

**If the Windows job fails** (likely the first time, see the note at the top): open the failed step, copy the compiler
error, and fix that line - or send it to whoever helps you with the code. The `portable-tests` job tells you
whether the engine itself is healthy.

### Build locally instead

Requirements: Windows 10/11, *Visual Studio 2019/2022* or *Build Tools* with the "Desktop development with C++" workload,
CMake 3.21+, PowerShell.

```
powershell -ExecutionPolicy Bypass -File tools\fetch-webview2.ps1
cmake -S . -B build -A x64
cmake --build build --config Release
build\Release\FlowBrowser.exe
```

The result is a single self-contained `FlowBrowser.exe` (the UI is compiled into it; the C runtime is linked statically).

### Running it

* Needs the **Microsoft Edge WebView2 Runtime**, which is already part of Windows 11 and current Windows 10. If it is missing,
  FlowBrowser tells you where to get it (the *Evergreen* runtime).
* The exe is unsigned, so Windows SmartScreen may warn ("More info" → "Run anyway"). Code signing needs a certificate
  that this project does not ship.
* **Where data lives:** `%APPDATA%\FlowBrowser\` (`flows`, `backups`, `store`, `logs`, `data`, `profile`).
  Create an empty file named `portable.txt` next to the exe to keep everything in a `data` folder beside it instead.
  Relative file names in flows (for example `results.xlsx`) are resolved in the `data` folder (*Library → Open folder* / ⋮ menu).
* **Command line:** `FlowBrowser.exe --run "My flow" [--exit]`, `--open "My flow"`, `FlowBrowser.exe file.fbflow`, `FlowBrowser.exe https://example.com`.
  The Library's *Schedule* button creates a Windows Task Scheduler entry that runs `--run "name" --exit`.

---

## Quick start

1. Open the page you want to automate. Press **⏺ Record**, do the task once, press stop. You now have steps.
2. In the **Flow** tab adjust them: rename, change a value to `{{ row.Email }}`, add *wait* steps, delete mistakes.
3. **Data** tab → choose your Excel file → *Fill a form from every row*: pick which column goes into which field. The wizard
   wraps your steps in a loop and writes "Done" / the error back into the sheet.
4. **▶ Run** (F9). *Step* runs one step at a time; click the dot beside a step to set a breakpoint.
5. **Library** → Save. Back up from the same tab.

Tips: keep the automated tab visible and the window un-minimised while a flow runs (hidden pages are throttled by the
browser); scheduled runs need you to be signed in to Windows.

---

## Limits you should know about

* **Cross-origin iframes** and **closed shadow roots** cannot be reached from page scripts; elements inside them can't be picked
  or recorded. (Same-origin frames and open shadow DOM work.)
* Pop-ups that depend on `window.opener` (some OAuth flows) open as normal tabs without the opener link.
* Downloads use WebView2's default download experience; flows can't yet choose the download folder or wait for a file.
* No private/incognito window. *Clear browsing data* and *clear on exit* are available.
* The *Once* schedule runs "today"; for a specific date use Task Scheduler directly.
* The recorder stores the file path you chose for an upload; move the file and the step needs the new path.
* Sites change. Prefer selectors by label / text / test id and add alternatives on important steps.
* Use FlowBrowser on sites and data you are allowed to automate; respect each site's terms and rate limits.

---

## Project layout

```
src/core/   portable C: JSON, regex, expressions, flow model + validation, library/backup, ZIP + Excel (xlsx) reader/writer,
            the flow runner (fb_runner*.c) and its backend interface (fb_runner.h)
src/js/     fb_runtime.js (page runtime used by every step), fb_tools.js (picker, recorder, similar items, selector tester)
src/ui/     the interface: shell (tabs, address bar), Studio panel, Yaru theme - plain HTML/CSS/JS, embedded into the exe
src/win/    Windows host: WebView2 window and tabs, DevTools bridge for the runner, file dialogs, scheduling, settings
tools/      embed.c (turns the UI into C data), fbrun.c (console runner used by the tests), fetch-webview2.ps1
tests/      automated tests (see below) and the stand-in headers for syntax-checking the Windows code on Linux
docs/       PROTOCOL.md - messages between the UI, the host and the runner
```

### Testing

```
tests/run_all.sh            C tests, flow engine, Excel/CSV/JSON through the runner      (gcc/clang + python3)
tests/run_all.sh --web      + in-page tools, web flows in a real Chromium, whole UI        (node + playwright)
tests/winstub/check.sh      syntax-check src/win against stand-in Windows headers          (clang, Linux)
```

The web tests drive the runner through `tests/bridge.js`, which plays the same role as the Windows host does with WebView2
(a DevTools-protocol bridge), so the flow steps are exercised against real Chromium. The Studio UI is tested against
`tests/ui_mock.js`, an in-page imitation of the host protocol. What no test covers is the real Windows host itself.

## License

MIT. WebView2 and its loader are Microsoft's and are downloaded at build time under their own license.
