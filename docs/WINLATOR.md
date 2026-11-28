# FlowBrowser on Winlator (Wine on Android)

Winlator runs Windows programs on Android through Wine (plus Box86 / Box64 to run x86 / x64 code on an ARM phone).
Microsoft's WebView2 cannot run there, so the release zip contains **two programs built from the same code**:

| File | Browser engine | Use it on |
|---|---|---|
| `FlowBrowser.exe` | Microsoft Edge WebView2 (Chromium) | Windows 10 / 11 |
| `FlowBrowserWine.exe` | **Wine's MSHTML, which uses Wine Gecko** (the Firefox-based engine Wine ships) | **Winlator / Wine** (on a normal Windows PC it would use Internet Explorer's MSHTML instead; that is untested, and `FlowBrowser.exe` is the right choice there) |

Everything outside the page engine is identical: the window, tabs, Studio panel, flow engine, Excel / CSV / JSON, library,
backups. FlowBrowser is plain C, so it does **not** need .NET. Wine Mono is only installed because Wine asks for it, and
**Wine Gecko is the one that matters**: it *is* the browser.

> **Honest status.** `FlowBrowserWine.exe` has not been run on Wine or Winlator by the person who wrote it - there was no
> Wine available. The page-automation layer it uses was tested in Chromium with the modern features switched off (to
> imitate an old engine), and the C code was syntax-checked, but the real MSHTML/Gecko behaviour is untested. Expect to
> report a few problems. The CI job `wine-smoke` runs it under real Wine on Linux; its logs (artifact `wine-smoke-logs`) are the best thing to send back.

## 1. Set up the container

1. In Winlator create (or open) a container. Wine 9.x or newer is a good choice. Set **Screen size** to at least
   1280x720 (the default is smaller than the Studio panel needs). 1920x1080 is more comfortable on a tablet.
2. **Make sure Wine Gecko is installed** in that container. Many Winlator builds already include it. To check: start the
   container's *Control Panel* / `wine control` → *Add/Remove Programs*, or run `wine uninstaller`; you should see
   **Wine Gecko** (and *Wine Mono*). If they are missing, either
   * start any program that uses the browser engine and accept Wine's offer to download the add-ons, or
   * download the `.msi` files that match your Wine version from the WineHQ *wine-gecko* and *wine-mono* download pages
     (the Wine version is shown by `winecfg`), copy them to the phone's Download folder and install them with
     `wine msiexec /i <file>.msi` in the container (Winlator's *Run* menu, or the container's command prompt).
     Install both the 64-bit and 32-bit Gecko if the container runs both.
3. Recommended container settings: audio and graphics at their defaults are fine; FlowBrowser draws nothing special.
   Increase **Box64 / Box86 stability presets** if the app crashes at start (Winlator's *Stable* / *Compatibility* options).

## 2. Get the files onto the phone

Download the artifact zip from GitHub Actions (or the release) on the phone, extract it into a folder inside your
**Download** folder, for example `Download/FlowBrowser/`. Winlator shows Android folders as drive letters in the
container (the Download folder is normally `D:`; see the container's *Drives* setting).

Which exe? Newer Winlator builds run both 64-bit and 32-bit Windows programs. If you have an older container that only runs
32-bit programs, use the **Win32** zip. If a 64-bit exe does not start, try the Win32 one.

**Tip: portable mode.** Create an empty file named `portable.txt` next to `FlowBrowserWine.exe`. FlowBrowser then keeps all
its data in a `data` folder beside the exe (`flows`, `backups`, `logs`, `profile`, `ui`) instead of the hidden Wine profile,
so you can reach your flows, Excel results and logs from any Android file manager.
Without it the data lives at `C:\users\<name>\AppData\Roaming\FlowBrowser` inside the container.

## 3. Run it

In Winlator open the container → **Run** (or add a *Shortcut*) → pick `FlowBrowserWine.exe`. The first start is slow
(the emulated CPU has to start Wine's browser engine); later starts are faster. Settings → About shows the engine
(`Wine MSHTML / Gecko`) and the exact build.

Command line (shortcut *Exec arguments*): `FlowBrowserWine.exe --run "My flow" --exit`,
`FlowBrowserWine.exe file.fbflow`, `FlowBrowserWine.exe https://example.com`.

Using the app is the same as on Windows (see the README): record, edit, run, Excel, library. Some practical points on a phone:

* **Give the page room.** Hide the Studio panel with the panel button while you browse; show it when you edit. Use a
  keyboard and mouse (Bluetooth or USB) if you can, because the Studio is a desktop interface; Winlator's on-screen
  controls map touches to mouse clicks.
* **Use longer waits.** The page engine runs on an emulated CPU. Settings → Automation → *Wait up to (ms)* of 30000 or more
  avoids false time-outs.
* **File upload** works through the native *Open* dialog of the page (FlowBrowser finds it, fills in the file name and confirms it).
  Choose files that are in drives Wine can see (`D:` / `Z:`). Several files can be uploaded at once only if they are in the same folder.
* **Excel files** are read and written by FlowBrowser itself (no Excel needed). Put them in the same shared folder, or use the portable `data` folder.

## 4. How the Wine edition differs from the Windows one

The page engine is old compared with Chromium, so some things are limited. FlowBrowser works around many of them (listed
first); the rest are limits you should know.

Handled by FlowBrowser itself:
* The runner talks to the page without Chromium's DevTools: clicking, typing, selecting, sliders, drag-and-drop, scrolling,
  waiting, `Run JavaScript`, tables, lists, the picker, the recorder and the selector tester all work through scripts that
  are written in old JavaScript (ES5) so Wine's JScript accepts them.
* Web-page text, Excel, CSV, JSON, library, backups, and CAPTCHA detection are the same code as on Windows.

Limits:
* **Input events are synthetic** (`isTrusted` is false). Sites that insist on real mouse/keyboard events may ignore them.
* **No clipboard shortcuts** (Ctrl+C / Ctrl+V) in `Press key`, and HTML5 drag-and-drop is emulated by dispatching events.
* **Screenshots** capture the visible part of the page only and are always PNG.
* **Cross-origin iframes** cannot be reached (same as Windows), and **Find in page** may not work (the engine may lack `window.find`).
* **XPath selectors** may be unavailable on some Gecko versions; prefer `css:`, `label:`, `text:`, `id:`, `name:`.
* **Modern pages may look wrong or fail**: Wine Gecko is a much older Firefox. Sites that need current JavaScript or CSS
  (some single-page apps, `position: sticky`, CSS variables, `gap` in flexbox) can misbehave. For those, run `FlowBrowser.exe` on a real Windows PC.
* **No DevTools window.** Downloads are not handled by flows. Zoom may be ignored by Wine.
* **Proxy, user-agent and clearing cookies/cache** are best effort through Windows networking APIs that Wine implements only partly.
* **Windows Task Scheduler** (`schtasks`) is little more than a stub in Wine, so the Library's *Schedule* button will not create working schedules there.
  Use Android automation (for example a Tasker / MacroDroid task that starts a Winlator shortcut with `--run "name" --exit`).
* **CAPTCHAs are not solved.** They are detected, and the flow pauses for you, stops, or ignores them, as chosen in Settings → Automation. This is deliberate.

## 5. Build logs and run logs (what to send back when something fails)

* **`build.log`** - written by `tools/build.ps1` on every build. It lists the date, Windows version, git commit, CMake and
  Visual Studio versions, the complete compiler output, every step's exit code, a list of the errors found, and a final
  `BUILD SUCCESS` / `BUILD FAILED` line. GitHub Actions uploads it as the artifact `build-log-x64` / `build-log-Win32`
  **even when the build fails**, and it is copied into the release zip as `build.log`.
* **Which build is running?** Settings → About, and the start-up line of `logs\host.log`, show the build info compiled into
  the exe (target, compiler, architecture, git commit, build time) so a log can always be matched to its build.
* **`logs\host.log`** (in the data folder) - what the app did at start-up and why something failed (engine start, page
  load, file dialog handling, run errors).
* **`wine-smoke-logs`** (CI artifact) - the same logs from a real Wine run on Linux.

## 6. Troubleshooting

| Symptom | What to do |
|---|---|
| A message says the browser control (Wine Gecko / MSHTML) could not be created | Install Wine Gecko in the container (section 1), restart the container. |
| The window is white / blank | See *A white or blank window* below. |
| The window is black or tiny | Raise the container *Screen size*; try the other exe (Win32 vs x64); try the *Stable* / *Compatibility* Box presets; send `logs\host.log`. |
| The Studio panel or toolbar looks broken | The interface needs Gecko's JavaScript engine; update to the Wine / Gecko version Winlator offers; send a screenshot with `logs\host.log`. |
| A page loads but a step cannot find an element | Check the selector in the *Elements* tab's tester; try `label:` / `text:` / `css:`; add waits; if the site is modern, see *Limits*. |
| A click does nothing | The site may require trusted events; try `Press key`, `Run JavaScript`, or a different selector for the same control. |
| File upload does nothing | Use the *Upload file* step in `auto` mode; use a path on `D:` or `Z:`; send `logs\host.log`. |
| Very slow | Expected on emulation. Close other apps, use longer waits, keep flows short, prefer the *Elements* extraction steps to long scripted loops. |
| Build fails in Actions | Download the `build-log-*` artifact (or open the failed step) and send the lines under *Summary*; they list the compiler errors. |

### A white or blank window

The Wine edition watches its own start-up and reports what it finds instead of leaving a white window:

* **An orange box at the top of the window** means a page script failed or the page did not finish starting. The box lists the
  error and what the browser engine supports (document mode, JSON, Promise ...). Read it out or take a screenshot.
* **After 30 seconds without a start-up message from the interface** a message box says so and names the log file.
* **`logs\host.log`** (see section 2 for where it is; with `portable.txt` it is `data\logs\host.log` next to the exe) always
  contains, in this order: `environment:` (Wine version, 32/64-bit), `gecko:` lines (Gecko folders found in the prefix; a Gecko
  that is installed system-wide for Wine is not listed there and can still be used), `atl:` and `browser control created`,
  `emulation:` (IE11 document mode requested), `load UI page`, `navigate ...`, `document complete ...`, `page shell#0 (loaded): ...`
  (document mode, whether JSON / Promise exist, user agent), and `ui shell:` lines for every script error. If the window stays
  white, **send that file**: the last of these lines shows how far the start got.
* The most common causes, in order: Wine Gecko is not installed (no `document complete` line, a `watchdog:` line instead);
  the Gecko version does not match the Wine version; a Box64/Box86 preset that makes Gecko crash (try *Stable* or *Compatibility*);
  a screen size that is too small.

