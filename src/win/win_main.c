/* FlowBrowser for Windows - main window, layout, tabs, start-up. Shared by the WebView2 build and the Wine (MSHTML) build;
   everything that touches the browser engine is behind the fbh_* / fbw_tab_* interface (win_app.h).

   Views
     shell  - the browser chrome (tab strip, address bar, menus); an HTML page from the embedded UI
     panel  - the Studio panel (recorder, flow editor, data, library); docked on the right or in a floating window
     tab(s) - one browser view per tab, placed where the shell's #content box is

   Native views sit on top of HTML, so a shell menu that must overlap a tab asks the host to hide it (overlay). */
#include "win_app.h"
#include <stdarg.h>

FbwApp G;

#define NEWTAB_URL FBW_NEWTAB_URL
static const wchar_t *MAIN_CLASS  = L"FlowBrowserWindow";
static const wchar_t *FLOAT_CLASS = L"FlowBrowserStudio";

void fbw_release(void *o) { if (o) IUnknown_Release((IUnknown *)o); }

/* ------------------------------------------------------------------ DPI */
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
static int dpi_of(HWND h)
{
    static PFN_GetDpiForWindow fn;
    static int init;
    if (!init) {
        init = 1;
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) fn = (PFN_GetDpiForWindow)(void *)GetProcAddress(u, "GetDpiForWindow");
    }
    UINT d = fn ? fn(h) : 0;
    if (!d) { HDC dc = GetDC(NULL); d = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); }
    return d ? (int)d : 96;
}

/* ------------------------------------------------------------------ calls from other threads into the UI thread */
FbwCall *fbw_call_new(void (*fn)(FbwCall *))
{
    FbwCall *c = (FbwCall *)calloc(1, sizeof *c);
    if (!c) return NULL;
    c->fn = fn;
    c->refs = 2;
    c->done = CreateEventW(NULL, TRUE, FALSE, NULL);
    return c;
}

void fbw_call_release(FbwCall *c)
{
    if (!c) return;
    if (InterlockedDecrement(&c->refs) == 0) {
        if (c->done) CloseHandle(c->done);
        free(c->s1); free(c->s2); free(c->out);
        free(c);
    }
}

void fbw_call_finish(FbwCall *c)
{
    if (!c) return;
    SetEvent(c->done);
    fbw_call_release(c);
}

int fbw_ui_call(FbwCall *c, DWORD timeout_ms)
{
    if (!G.hwnd || !PostMessageW(G.hwnd, WM_FBW_CALL, 0, (LPARAM)c)) {
        snprintf(c->err, sizeof c->err, "FlowBrowser is closing");
        c->rc = 1;
        fbw_call_release(c);            /* the UI thread's reference: it will never run */
        return 1;
    }
    HANDLE h[2] = { c->done, G.stop_evt };
    DWORD n = G.stop_evt ? 2 : 1;
    DWORD w = WaitForMultipleObjects(n, h, FALSE, timeout_ms);
    if (w == WAIT_OBJECT_0) return 0;
    if (w == WAIT_OBJECT_0 + 1) return 2;
    return 1;
}

/* ------------------------------------------------------------------ messages to the HTML views */
static FbwView *view_of(int k) { return k == FBW_PANEL ? &G.panel : &G.shell; }

void fbw_post(int view, const char *json)
{
    FbwView *v = view_of(view);
    if (!v->wv || !v->ready || !json) return;
    fbh_post_view(v, json);
}

volatile LONG fbw_pending_posts;

void fbw_post_async(int view, char *json)
{
    if (!json) return;
    InterlockedIncrement(&fbw_pending_posts);
    if (!G.hwnd || !PostMessageW(G.hwnd, WM_FBW_POST, (WPARAM)view, (LPARAM)json)) { InterlockedDecrement(&fbw_pending_posts); free(json); }
}

void fbw_broadcast(const char *json) { fbw_post(FBW_SHELL, json); fbw_post(FBW_PANEL, json); }

void fbw_flash(void)
{
    FLASHWINFO fi;
    memset(&fi, 0, sizeof fi);
    fi.cbSize = sizeof fi; fi.hwnd = G.hwnd; fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG; fi.uCount = 6;
    FlashWindowEx(&fi);
}

/* ------------------------------------------------------------------ tabs: lookup + state push */
FbwTab *fbw_tab_by_id(int id)
{
    for (int i = 0; i < G.ntabs; i++) if (G.tabs[i]->id == id) return G.tabs[i];
    return NULL;
}
FbwTab *fbw_active_tab(void) { return fbw_tab_by_id(G.active_id); }

void fbw_set_title(void)
{
    FbwTab *t = fbw_active_tab();
    char *s = fbw_dupf("%s%sFlowBrowser", t && t->title && t->title[0] ? t->title : "", t && t->title && t->title[0] ? " - " : "");
    wchar_t *w = fbw_w(s);
    if (w && G.hwnd) SetWindowTextW(G.hwnd, w);
    free(w); free(s);
}

void fbw_tabs_changed(void)
{
    FbJson *o = fbj_obj(), *arr = fbj_arr();
    fbj_obj_set(o, "ev", fbj_str("tabs"));
    for (int i = 0; i < G.ntabs; i++) {
        FbwTab *t = G.tabs[i];
        FbJson *e = fbj_obj();
        fbj_obj_set(e, "id", fbj_num(t->id));
        fbj_obj_set(e, "title", fbj_str(t->title && t->title[0] ? t->title : (t->loading ? "Loading…" : "New tab")));
        fbj_obj_set(e, "url", fbj_str(t->url ? t->url : ""));
        fbj_obj_set(e, "loading", fbj_bool(t->loading));
        fbj_obj_set(e, "canBack", fbj_bool(t->can_back));
        fbj_obj_set(e, "canForward", fbj_bool(t->can_forward));
        fbj_arr_push(arr, e);
    }
    fbj_obj_set(o, "tabs", arr);
    fbj_obj_set(o, "active", fbj_num(G.active_id));
    char *s = fbj_to_string(o, 0);
    if (s) { fbw_broadcast(s); free(s); }
    fbj_free(o);
    fbw_set_title();
}

void fbw_post_zoom(void)
{
    FbwTab *t = fbw_active_tab();
    char *s = fbw_dupf("{\"ev\":\"zoom\",\"factor\":%.3f}", t && t->zoom > 0 ? t->zoom : 1.0);
    if (s) { fbw_post(FBW_SHELL, s); free(s); }
}

/* ------------------------------------------------------------------ keyboard shortcuts (pressed while a web page has the focus) */
const char *fbw_shortcut_name(unsigned vk, int ctrl, int shift, int alt)
{
    const char *name = NULL;
    if (vk == VK_F5 || (ctrl && !shift && vk == 'R')) name = "reload";
    else if (ctrl && shift && vk == 'R') name = "record";
    else if (ctrl && !shift && vk == 'T') name = "newtab";
    else if (ctrl && vk == 'W') name = "closetab";
    else if ((ctrl && vk == 'L') || (alt && vk == 'D')) name = "focusurl";
    else if (ctrl && vk == 'F') name = "find";
    else if (alt && vk == VK_LEFT) name = "back";
    else if (alt && vk == VK_RIGHT) name = "forward";
    else if (ctrl && vk == 'D') name = "bookmark";
    else if (ctrl && vk == 'H') name = "history";
    else if (ctrl && vk == 'P') name = "print";
    else if (vk == VK_F12) name = "devtools";
    else if (ctrl && (vk == VK_OEM_PLUS || vk == VK_ADD)) name = "zoomin";
    else if (ctrl && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) name = "zoomout";
    else if (ctrl && (vk == '0' || vk == VK_NUMPAD0)) name = "zoomreset";
    else if (vk == VK_F11) name = "fullscreen";
    else if (vk == VK_F9) name = "run";
    else if (vk == VK_F8) name = "panel";
    else if (ctrl && vk == VK_TAB) name = shift ? "prevtab" : "nexttab";
    else if (ctrl && vk == VK_NEXT) name = "nexttab";
    else if (ctrl && vk == VK_PRIOR) name = "prevtab";
    return name;
}

/* the address bar and find box live in the shell view: give it the keyboard before it focuses them */
void fbw_shortcut(const char *name)
{
    if (!name) return;
    if (G.shell.ctl && (!strcmp(name, "focusurl") || !strcmp(name, "find") || !strcmp(name, "newtab") || !strcmp(name, "bookmark") || !strcmp(name, "history")))
        fbh_focus(G.shell.ctl);
    char *s = fbw_dupf("{\"ev\":\"shortcut\",\"name\":\"%s\"}", name);
    if (s) { fbw_post(FBW_SHELL, s); free(s); }
}

/* ------------------------------------------------------------------ layout */
static void put_rect(FbwCtl c, const RECT *r) { fbh_set_bounds(c, r); }

static RECT css_to_px(const RECT *css)
{
    double s = G.dpi / 96.0;
    RECT r;
    r.left = (LONG)(css->left * s + 0.5);
    r.top = (LONG)(css->top * s + 0.5);
    r.right = (LONG)(css->right * s + 0.5);
    r.bottom = (LONG)(css->bottom * s + 0.5);
    return r;
}

void fbw_layout(void)
{
    if (!G.hwnd) return;
    RECT cl;
    GetClientRect(G.hwnd, &cl);
    if (G.shell.ctl) { put_rect(G.shell.ctl, &cl); fbh_set_visible(G.shell.ctl, !G.fullscreen); }

    RECT content = { 0, 0, 0, 0 };
    int have = 0;
    if (G.fullscreen) { content = cl; have = 1; }
    else if (G.have_layout) {
        content = css_to_px(&G.content_css);
        if (content.right > cl.right) content.right = cl.right;
        if (content.bottom > cl.bottom) content.bottom = cl.bottom;
        have = content.right > content.left && content.bottom > content.top;
    }
    for (int i = 0; i < G.ntabs; i++) {
        FbwTab *t = G.tabs[i];
        if (!t->ctl) continue;
        int vis = have && t->id == G.active_id && !G.overlay;
        if (vis) put_rect(t->ctl, &content);
        fbh_set_visible(t->ctl, vis);
    }

    if (G.panel.ctl) {
        int show = G.panel_visible && !G.fullscreen;
        if (G.panel_float) {
            if (G.float_hwnd) {
                RECT fc;
                GetClientRect(G.float_hwnd, &fc);
                put_rect(G.panel.ctl, &fc);
                ShowWindow(G.float_hwnd, show ? SW_SHOWNA : SW_HIDE);
            }
            fbh_set_visible(G.panel.ctl, show);
        } else {
            RECT p = css_to_px(&G.panel_css);
            int ok = show && !G.overlay && G.have_layout && p.right > p.left && p.bottom > p.top;
            if (ok) {
                if (p.right > cl.right) p.right = cl.right;
                if (p.bottom > cl.bottom) p.bottom = cl.bottom;
                put_rect(G.panel.ctl, &p);
            }
            fbh_set_visible(G.panel.ctl, ok);
            if (G.float_hwnd) ShowWindow(G.float_hwnd, SW_HIDE);
        }
    }
}

static LRESULT CALLBACK float_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE: if (w != SIZE_MINIMIZED) fbw_layout(); return 0;
    case WM_MOVE: if (G.panel.ctl && G.panel_float) fbh_parent_moved(G.panel.ctl); break;
    case WM_CLOSE:
        /* hiding the floating Studio is the same as pressing the panel button in the shell */
        G.panel_visible = 0;
        ShowWindow(h, SW_HIDE);
        fbw_post(FBW_SHELL, "{\"ev\":\"panel.closed\"}");
        fbw_layout();
        return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)l; mm->ptMinTrackSize.x = 360; mm->ptMinTrackSize.y = 300; return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}

void fbw_apply_panel_mode(void)
{
    if (!G.panel.ctl) return;
    if (G.panel_float) {
        if (!G.float_hwnd) {
            RECT mr;
            GetWindowRect(G.hwnd, &mr);
            int w = 520, h = mr.bottom - mr.top - 80;
            if (h < 420) h = 420;
            G.float_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, FLOAT_CLASS, L"FlowBrowser Studio", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                           mr.right - w - 40, mr.top + 60, w, h, G.hwnd, NULL, G.inst, NULL);
        }
        if (G.float_hwnd) fbh_set_parent(G.panel.ctl, G.float_hwnd);
    } else {
        fbh_set_parent(G.panel.ctl, G.hwnd);
        if (G.float_hwnd) ShowWindow(G.float_hwnd, SW_HIDE);
    }
    fbw_layout();
}

void fbw_toggle_fullscreen(void)
{
    if (!G.hwnd) return;
    if (!G.fullscreen) {
        MONITORINFO mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        G.saved_maximized = IsZoomed(G.hwnd) ? 1 : 0;
        if (G.saved_maximized) ShowWindow(G.hwnd, SW_RESTORE);
        G.saved_style = (int)GetWindowLongW(G.hwnd, GWL_STYLE);
        GetWindowRect(G.hwnd, &G.saved_rect);
        GetMonitorInfoW(MonitorFromWindow(G.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(G.hwnd, GWL_STYLE, G.saved_style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(G.hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        G.fullscreen = 1;
    } else {
        G.fullscreen = 0;
        SetWindowLongW(G.hwnd, GWL_STYLE, G.saved_style);
        SetWindowPos(G.hwnd, NULL, G.saved_rect.left, G.saved_rect.top, G.saved_rect.right - G.saved_rect.left,
                     G.saved_rect.bottom - G.saved_rect.top, SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        if (G.saved_maximized) ShowWindow(G.hwnd, SW_MAXIMIZE);
    }
    fbw_layout();
}


int fbw_tab_new(const char *url, int activate)
{
    if (!G.engine_ready || G.ntabs >= FBW_MAX_TABS) return 0;
    FbwTab *t = (FbwTab *)calloc(1, sizeof *t);
    if (!t) return 0;
    t->id = ++G.next_tab_id;
    t->zoom = 1.0;
    t->loading = 1;
    t->title = fb_strdup("");
    t->url = fb_strdup(url ? url : "");
    t->start_url = fb_strdup(url && url[0] ? url : (G.home_url && G.home_url[0] ? G.home_url : NEWTAB_URL));
    G.tabs[G.ntabs++] = t;
    if (activate || !G.active_id) G.active_id = t->id;
    fbh_create_tab(t);
    fbw_tabs_changed();
    fbw_layout();
    return t->id;
}

void fbw_tab_activate(int id)
{
    FbwTab *t = fbw_tab_by_id(id);
    if (!t) return;
    FbwTab *old = fbw_active_tab();
    G.active_id = id;
    if (G.recording && old && old != t) { fbw_tools_cmd(old, "{\"name\":\"recorder-stop\"}"); fbw_tools_cmd(t, "{\"name\":\"recorder-start\"}"); }
    fbw_layout();
    fbw_tabs_changed();
    fbw_post_zoom();
    if (t->ctl) fbh_focus(t->ctl);
}

void fbw_tab_close(int id)
{
    int idx = -1;
    for (int i = 0; i < G.ntabs; i++) if (G.tabs[i]->id == id) idx = i;
    if (idx < 0) return;
    FbwTab *t = G.tabs[idx];
    fbw_engine_tab_closed(t);
    for (int i = idx; i < G.ntabs - 1; i++) G.tabs[i] = G.tabs[i + 1];
    G.ntabs--;
    if (G.active_id == id) G.active_id = G.ntabs ? G.tabs[idx < G.ntabs ? idx : G.ntabs - 1]->id : 0;
    if (t->pending) { snprintf(t->pending->err, sizeof t->pending->err, "The tab was closed"); t->pending->rc = 1; fbw_call_finish(t->pending); t->pending = NULL; }
    fbh_destroy_tab(t);
    free(t->title); free(t->url); free(t->start_url);
    free(t);
    if (!G.ntabs && !G.closing) fbw_tab_new(NULL, 1);
    fbw_layout();
    fbw_tabs_changed();
    fbw_post_zoom();
}

/* ------------------------------------------------------------------ startup sequence */
void fbw_on_shell_ready(void);
void fbw_on_panel_ready(void);

void fbw_on_shell_ready(void)
{
    if (G.shell.ready && G.ntabs) { fbw_tabs_changed(); fbw_post_zoom(); return; }
    G.shell.ready = 1;
    fbh_create_panel();
    fbw_tab_new(G.cli_url, 1);
    fbw_post_zoom();
}

void fbw_on_panel_ready(void)
{
    G.panel.ready = 1;
    fbw_tabs_changed();
    if (G.cli_open || G.cli_run) {
        FbJson *o = fbj_obj();
        fbj_obj_set(o, "ev", fbj_str("cli"));
        fbj_obj_set(o, "open", fbj_str(G.cli_open ? G.cli_open : G.cli_run));
        fbj_obj_set(o, "run", fbj_bool(G.cli_run != NULL));
        fbj_obj_set(o, "exit", fbj_bool(G.cli_exit));
        char *s = fbj_to_string(o, 0);
        fbj_free(o);
        if (s) { fbw_post(FBW_PANEL, s); free(s); }
    }
}


/* ------------------------------------------------------------------ window placement */
static void load_placement(int *x, int *y, int *w, int *h, int *max)
{
    *x = CW_USEDEFAULT; *y = CW_USEDEFAULT; *w = 1360; *h = 860; *max = 0;
    char *txt = fbw_store_read("window");
    if (!txt) return;
    FbJson *j = fbj_parse_str(txt, NULL, 0);
    free(txt);
    if (!j) return;
    int px = (int)fbj_get_num(j, "x", -32000), py = (int)fbj_get_num(j, "y", -32000);
    int pw = (int)fbj_get_num(j, "w", 0), ph = (int)fbj_get_num(j, "h", 0);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (pw >= 640 && ph >= 420 && px > vx - 100 && py > vy - 20 && px < vx + vw - 120 && py < vy + vh - 80) { *x = px; *y = py; *w = pw; *h = ph; }
    *max = fbj_get_bool(j, "max", 0);
    fbj_free(j);
}

static void save_placement(void)
{
    if (!G.hwnd || G.fullscreen) return;
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof wp);
    wp.length = sizeof wp;
    if (!GetWindowPlacement(G.hwnd, &wp)) return;
    char *s = fbw_dupf("{\"x\":%ld,\"y\":%ld,\"w\":%ld,\"h\":%ld,\"max\":%d}", wp.rcNormalPosition.left, wp.rcNormalPosition.top,
                       wp.rcNormalPosition.right - wp.rcNormalPosition.left, wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, wp.showCmd == SW_SHOWMAXIMIZED);
    if (s) { fbw_store_write("window", s); free(s); }
}

/* ------------------------------------------------------------------ main window */
static void shutdown_app(void)
{
    save_placement();
    fbw_engine_stop();
    if (G.run_thread) { WaitForSingleObject(G.run_thread, 4000); CloseHandle(G.run_thread); G.run_thread = NULL; }
    fbw_auto_backup();
    if (G.clear_on_exit) {
        char *m = fbw_dupf("%s\\clear-profile.flag", G.data_dir);
        if (m) { fb_write_file(m, "1", 1); free(m); }
    }
    G.closing = 1;
    for (int i = 0; i < G.ntabs; i++) if (G.tabs[i]->ctl) fbh_close_view(G.tabs[i]->ctl);
    if (G.panel.ctl) fbh_close_view(G.panel.ctl);
    if (G.shell.ctl) fbh_close_view(G.shell.ctl);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE: if (w != SIZE_MINIMIZED) fbw_layout(); return 0;
    case WM_DPICHANGED: {
        G.dpi = HIWORD(w);
        const RECT *r = (const RECT *)l;
        SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        fbw_layout();
        return 0;
    }
    case WM_MOVE:
    case WM_MOVING:
        for (int i = 0; i < G.ntabs; i++) if (G.tabs[i]->ctl) fbh_parent_moved(G.tabs[i]->ctl);
        if (G.shell.ctl) fbh_parent_moved(G.shell.ctl);
        if (G.panel.ctl && !G.panel_float) fbh_parent_moved(G.panel.ctl);
        break;
    case WM_SETFOCUS: {
        FbwTab *t = fbw_active_tab();
        if (t && t->ctl) fbh_focus(t->ctl);
        else if (G.shell.ctl) fbh_focus(G.shell.ctl);
        return 0;
    }
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)l; mm->ptMinTrackSize.x = 700; mm->ptMinTrackSize.y = 460; return 0; }
    case WM_CLOSE:
        if (G.exit_ok || !G.panel.ready || !G.panel.wv) { G.exit_ok = 1; DestroyWindow(h); }
        else { G.closing = 1; fbw_post(FBW_PANEL, "{\"ev\":\"app.closing\"}"); }     /* the panel asks about unsaved work, then answers app.exit / app.stay */
        return 0;
    case WM_FBW_EXIT: G.exit_ok = 1; DestroyWindow(h); return 0;
    case WM_FBW_CALL: { FbwCall *c = (FbwCall *)l; c->fn(c); if (!c->async) fbw_call_finish(c); return 0; }
    case WM_FBW_POST: InterlockedDecrement(&fbw_pending_posts); fbw_post((int)w, (const char *)l); free((void *)l); return 0;
    case WM_APP + 5: fbw_tab_close((int)w); return 0;
    case WM_TIMER: fbh_timer(); return 0;
    case WM_DESTROY:
        shutdown_app();
        if (G.float_hwnd) DestroyWindow(G.float_hwnd);
        PostQuitMessage(G.exit_code);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------ command line */
static void parse_cmdline(void)
{
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--run") && i + 1 < argc) G.cli_run = fbw_u8(argv[++i]);
        else if (!wcscmp(argv[i], L"--open") && i + 1 < argc) G.cli_open = fbw_u8(argv[++i]);
        else if (!wcscmp(argv[i], L"--exit")) G.cli_exit = 1;
        else if (argv[i][0] != L'-') {
            char *a = fbw_u8(argv[i]);
            if (a && fb_ends_with(a, ".fbflow") && fb_file_exists(a)) {
                /* a flow file given on the command line is added to the library and opened */
                FbBuf b;
                fb_buf_init(&b);
                if (fb_read_file(a, &b) == 0) {
                    FbJson *f = fbj_parse(fb_buf_cstr(&b), b.len, NULL, 0);
                    if (f) {
                        char err[200];
                        const char *base = strrchr(a, '\\');
                        char name[200];
                        snprintf(name, sizeof name, "%s", base ? base + 1 : a);
                        name[strlen(name) - 7] = 0;
                        fbf_migrate(f);
                        if (fbf_lib_save(G.flows_dir, name, f, G.keep_versions, err, sizeof err) == 0) { free(G.cli_open); G.cli_open = fb_strdup(name); }
                        fbj_free(f);
                    }
                }
                fb_buf_free(&b);
                free(a);
            } else if (a) { free(G.cli_url); G.cli_url = a; }
        }
    }
    LocalFree(argv);
}

static int build_tools_script(void)
{
    size_t n1 = 0, n2 = 0;
    const unsigned char *rt = fbw_resource("js/fb_runtime.js", &n1, NULL), *tl = fbw_resource("js/fb_tools.js", &n2, NULL);
    if (!rt || !tl) return -1;
    FbBuf b;
    fb_buf_init(&b);
    fb_buf_append(&b, rt, n1);
    fb_buf_putc(&b, '\n');
    const char *mark = "@@FB_TOKEN@@";
    const char *p = (const char *)tl, *end = p + n2;
    while (p < end) {
        const char *q = strstr(p, mark);
        if (!q || q >= end) { fb_buf_append(&b, p, (size_t)(end - p)); break; }
        fb_buf_append(&b, p, (size_t)(q - p));
        fb_buf_puts(&b, G.token);
        p = q + strlen(mark);
    }
    G.tools_js = fb_buf_detach(&b);
    return G.tools_js ? 0 : -1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)cmd;
    memset(&G, 0, sizeof G);
    G.inst = inst;
    G.ui_thread = GetCurrentThreadId();
    snprintf(G.dialogs, sizeof G.dialogs, "manual");
    G.keep_versions = 10;
    G.auto_backup = 1;
    G.default_zoom = 1.0;
    G.stop_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (FAILED(OleInitialize(NULL))) return 2;

    fbw_paths_init();
    fbw_random_token(G.token, sizeof G.token);
    {
        size_t n = 0;
        const unsigned char *cat = fbw_resource("ui/catalog.json", &n, NULL);
        char err[200] = "";
        if (!cat || fbf_set_catalog((const char *)cat, err, sizeof err)) {
            wchar_t *w = fbw_w(err[0] ? err : "The step catalog is missing");
            MessageBoxW(NULL, w, L"FlowBrowser", MB_ICONERROR);
            free(w);
            return 2;
        }
    }
    if (build_tools_script()) { MessageBoxW(NULL, L"The page tools are missing from this build.", L"FlowBrowser", MB_ICONERROR); return 2; }
    fbw_settings_load_early();
    parse_cmdline();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(44, 44, 44));
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = float_proc;
    wc.lpszClassName = FLOAT_CLASS;
    RegisterClassExW(&wc);

    int x, y, w, h, maxi;
    load_placement(&x, &y, &w, &h, &maxi);
    G.hwnd = CreateWindowExW(0, MAIN_CLASS, L"FlowBrowser", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, NULL, NULL, inst, NULL);
    if (!G.hwnd) return 2;
    G.dpi = dpi_of(G.hwnd);
    ShowWindow(G.hwnd, maxi ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(G.hwnd);

    fbw_log("FlowBrowser %s (%s) starting, build: %s", FBW_VERSION, fbw_engine_name(), fbw_build_info());
    fbh_start();

    MSG msg;
    int code = 0;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (fbh_pretranslate(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    code = (int)msg.wParam;
    if (G.runner) { fbr_free(G.runner); G.runner = NULL; }
    OleUninitialize();
    return code ? code : G.exit_code;
}
