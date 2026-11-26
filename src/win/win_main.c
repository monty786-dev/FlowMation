/* FlowBrowser for Windows - main window, WebView2 views (shell, Studio panel, one per tab), layout, tabs.

   Views
     shell  - the browser chrome (tab strip, address bar, menus); an HTML page from the embedded UI
     panel  - the Studio panel (recorder, flow editor, data, library); docked on the right or in a floating window
     tab(s) - one WebView2 per browser tab, placed where the shell's #content box is

   Native views sit on top of HTML, so a shell menu that must overlap a tab asks the host to hide it (overlay). */
#include "win_app.h"
#include <stdarg.h>

FbwApp G;

#define NEWTAB_URL "https://appassets.example/newtab.html"
static const wchar_t *MAIN_CLASS  = L"FlowBrowserWindow";
static const wchar_t *FLOAT_CLASS = L"FlowBrowserStudio";
static wchar_t *g_tools_w;          /* page runtime + tools as UTF-16 */

typedef struct { int kind; int tab_id; } VCtx;     /* kind: 0 shell, 1 panel, 2 tab */
static VCtx *vctx(int kind, int id) { VCtx *v = (VCtx *)malloc(sizeof *v); if (v) { v->kind = kind; v->tab_id = id; } return v; }
#define HR(p) ((HRESULT)(INT_PTR)(p))

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
    wchar_t *w = fbw_w(json);
    if (!w) return;
    ICoreWebView2_PostWebMessageAsJson(v->wv, w);
    free(w);
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

static void post_zoom(void)
{
    FbwTab *t = fbw_active_tab();
    char *s = fbw_dupf("{\"ev\":\"zoom\",\"factor\":%.3f}", t && t->zoom > 0 ? t->zoom : 1.0);
    if (s) { fbw_post(FBW_SHELL, s); free(s); }
}

/* ------------------------------------------------------------------ layout */
static void put_rect(ICoreWebView2Controller *c, const RECT *r) { RECT x = *r; ICoreWebView2Controller_put_Bounds(c, x); }

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
    if (G.shell.ctl) { put_rect(G.shell.ctl, &cl); ICoreWebView2Controller_put_IsVisible(G.shell.ctl, !G.fullscreen); }

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
        BOOL vis = have && t->id == G.active_id && !G.overlay;
        if (vis) put_rect(t->ctl, &content);
        ICoreWebView2Controller_put_IsVisible(t->ctl, vis);
    }

    if (G.panel.ctl) {
        BOOL show = G.panel_visible && !G.fullscreen;
        if (G.panel_float) {
            if (G.float_hwnd) {
                RECT fc;
                GetClientRect(G.float_hwnd, &fc);
                put_rect(G.panel.ctl, &fc);
                ShowWindow(G.float_hwnd, show ? SW_SHOWNA : SW_HIDE);
            }
            ICoreWebView2Controller_put_IsVisible(G.panel.ctl, show);
        } else {
            RECT p = css_to_px(&G.panel_css);
            BOOL ok = show && !G.overlay && G.have_layout && p.right > p.left && p.bottom > p.top;
            if (ok) {
                if (p.right > cl.right) p.right = cl.right;
                if (p.bottom > cl.bottom) p.bottom = cl.bottom;
                put_rect(G.panel.ctl, &p);
            }
            ICoreWebView2Controller_put_IsVisible(G.panel.ctl, ok);
            if (G.float_hwnd) ShowWindow(G.float_hwnd, SW_HIDE);
        }
    }
}

static LRESULT CALLBACK float_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE: if (w != SIZE_MINIMIZED) fbw_layout(); return 0;
    case WM_MOVE: if (G.panel.ctl && G.panel_float) ICoreWebView2Controller_NotifyParentWindowPositionChanged(G.panel.ctl); break;
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
        if (G.float_hwnd) ICoreWebView2Controller_put_ParentWindow(G.panel.ctl, G.float_hwnd);
    } else {
        ICoreWebView2Controller_put_ParentWindow(G.panel.ctl, G.hwnd);
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

/* ------------------------------------------------------------------ scripts, DevTools protocol */
void fbw_run_script(FbwTab *t, const char *js, FbwHandlerFn done, void *ctx)
{
    if (!t || !t->wv) { if (done) done(ctx, (void *)(INT_PTR)E_FAIL, NULL); return; }
    wchar_t *w = fbw_w(js);
    void *h = done ? fbw_handler(done, ctx) : NULL;
    HRESULT hr = w ? ICoreWebView2_ExecuteScript(t->wv, w, (ICoreWebView2ExecuteScriptCompletedHandler *)h) : E_OUTOFMEMORY;
    free(w);
    fbw_release(h);
    if (FAILED(hr) && done) done(ctx, (void *)(INT_PTR)hr, NULL);
}

void fbw_cdp(FbwTab *t, const char *method, const char *params, FbwHandlerFn done, void *ctx)
{
    if (!t || !t->wv) { if (done) done(ctx, (void *)(INT_PTR)E_FAIL, NULL); return; }
    wchar_t *wm = fbw_w(method), *wp = fbw_w(params && params[0] ? params : "{}");
    void *h = done ? fbw_handler(done, ctx) : NULL;
    HRESULT hr = (wm && wp) ? ICoreWebView2_CallDevToolsProtocolMethod(t->wv, wm, wp, (ICoreWebView2CallDevToolsProtocolMethodCompletedHandler *)h) : E_OUTOFMEMORY;
    free(wm); free(wp);
    fbw_release(h);
    if (FAILED(hr) && done) done(ctx, (void *)(INT_PTR)hr, NULL);
}

void fbw_page_eval(FbwTab *t, const char *expr, FbwHandlerFn done, void *ctx)
{
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "expression", fbj_str(expr));
    fbj_obj_set(p, "awaitPromise", fbj_bool(1));
    fbj_obj_set(p, "returnByValue", fbj_bool(1));
    char *s = fbj_to_string(p, 0);
    fbj_free(p);
    fbw_cdp(t, "Runtime.evaluate", s, done, ctx);
    free(s);
}

void fbw_tools_cmd(FbwTab *t, const char *cmd_json)
{
    FbJson *lit = fbj_str(cmd_json);
    char *q = fbj_to_string(lit, 0);
    fbj_free(lit);
    char *js = fbw_dupf("window.__fb&&window.__fb.tools&&window.__fb.tools.cmd(%s)", q ? q : "\"{}\"");
    fbw_run_script(t, js, NULL, NULL);
    free(q); free(js);
}

void fbw_apply_user_agent(FbwTab *t)
{
    if (!t || !t->wv || !G.user_agent || !G.user_agent[0]) return;
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "userAgent", fbj_str(G.user_agent));
    char *s = fbj_to_string(p, 0);
    fbj_free(p);
    fbw_cdp(t, "Emulation.setUserAgentOverride", s, NULL, NULL);
    free(s);
}

void fbw_apply_dialog_mode(void)
{
    BOOL manual = strcmp(G.dialogs, "manual") == 0;
    for (int i = 0; i < G.ntabs; i++) {
        ICoreWebView2Settings *st = NULL;
        if (G.tabs[i]->wv && SUCCEEDED(ICoreWebView2_get_Settings(G.tabs[i]->wv, &st)) && st) {
            ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(st, manual);
            ICoreWebView2Settings_Release(st);
        }
    }
}

/* ------------------------------------------------------------------ event handlers */
static void h_noop_done(void *ctx, void *a, void *b) { (void)ctx; (void)a; (void)b; }

static void tab_refresh_source(FbwTab *t)
{
    LPWSTR w = NULL;
    if (t->wv && SUCCEEDED(ICoreWebView2_get_Source(t->wv, &w)) && w) { free(t->url); t->url = fbw_u8(w); }
    if (w) CoTaskMemFree(w);
}
static void tab_refresh_title(FbwTab *t)
{
    LPWSTR w = NULL;
    if (t->wv && SUCCEEDED(ICoreWebView2_get_DocumentTitle(t->wv, &w)) && w) { free(t->title); t->title = fbw_u8(w); }
    if (w) CoTaskMemFree(w);
}

static void h_webmsg(void *ctx, void *sender, void *args)
{
    (void)sender;
    VCtx *v = (VCtx *)ctx;
    ICoreWebView2WebMessageReceivedEventArgs *a = (ICoreWebView2WebMessageReceivedEventArgs *)args;
    LPWSTR w = NULL;
    if (FAILED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(a, &w)) || !w) return;
    char *u = fbw_u8(w);
    CoTaskMemFree(w);
    if (!u) return;
    if (v->kind == 2) {
        FbwTab *t = fbw_tab_by_id(v->tab_id);
        if (t) fbw_page_message(t, u);
    } else {
        LPWSTR src = NULL;
        int ok = 0;
        if (SUCCEEDED(ICoreWebView2WebMessageReceivedEventArgs_get_Source(a, &src)) && src)
            ok = wcsncmp(src, FBW_ORIGIN_W, wcslen(FBW_ORIGIN_W)) == 0;
        if (src) CoTaskMemFree(src);
        if (ok) {
            char err[120];
            FbJson *m = fbj_parse_str(u, err, sizeof err);
            if (m) { fbw_command(v->kind, m); fbj_free(m); }
        }
    }
    free(u);
}

static void h_resource(void *ctx, void *sender, void *args)
{
    (void)ctx; (void)sender;
    ICoreWebView2WebResourceRequestedEventArgs *a = (ICoreWebView2WebResourceRequestedEventArgs *)args;
    ICoreWebView2WebResourceRequest *rq = NULL;
    LPWSTR uri = NULL;
    if (FAILED(ICoreWebView2WebResourceRequestedEventArgs_get_Request(a, &rq)) || !rq) return;
    ICoreWebView2WebResourceRequest_get_Uri(rq, &uri);
    char *u = fbw_u8(uri);
    if (uri) CoTaskMemFree(uri);
    ICoreWebView2WebResourceRequest_Release(rq);
    if (!u) return;
    if (strncmp(u, FBW_ORIGIN, strlen(FBW_ORIGIN)) != 0) { free(u); return; }
    const char *path = u + strlen(FBW_ORIGIN);
    size_t pl = strcspn(path, "?#");
    char name[200];
    snprintf(name, sizeof name, "ui/%.*s", (int)(pl < 150 ? pl : 150), pl ? path : "shell.html");
    size_t size = 0;
    const char *mime = "text/plain";
    const unsigned char *data = fbw_resource(name, &size, &mime);
    IStream *st = SHCreateMemStream(data, data ? (UINT)size : 0);
    wchar_t hdr[200];
    swprintf(hdr, sizeof hdr / sizeof hdr[0], L"Content-Type: %hs\r\nCache-Control: no-store\r\n", mime);
    ICoreWebView2WebResourceResponse *resp = NULL;
    if (st && G.env && SUCCEEDED(ICoreWebView2Environment_CreateWebResourceResponse(G.env, st, data ? 200 : 404, data ? L"OK" : L"Not Found", hdr, &resp)) && resp) {
        ICoreWebView2WebResourceRequestedEventArgs_put_Response(a, resp);
        ICoreWebView2WebResourceResponse_Release(resp);
    }
    if (st) IStream_Release(st);
    free(u);
}

static void h_ui_nav_start(void *ctx, void *sender, void *args)
{
    (void)ctx; (void)sender;
    ICoreWebView2NavigationStartingEventArgs *a = (ICoreWebView2NavigationStartingEventArgs *)args;
    LPWSTR uri = NULL;
    ICoreWebView2NavigationStartingEventArgs_get_Uri(a, &uri);
    if (uri && wcsncmp(uri, FBW_ORIGIN_W, wcslen(FBW_ORIGIN_W)) != 0 && wcsncmp(uri, L"about:blank", 11) != 0) {
        /* the UI itself never leaves its own origin: send the link to a real tab instead */
        ICoreWebView2NavigationStartingEventArgs_put_Cancel(a, TRUE);
        char *u = fbw_u8(uri);
        if (u && (fb_starts_with(u, "http://") || fb_starts_with(u, "https://"))) fbw_tab_new(u, 1);
        free(u);
    }
    if (uri) CoTaskMemFree(uri);
}

static void h_newwin_ui(void *ctx, void *sender, void *args)
{
    (void)ctx; (void)sender;
    ICoreWebView2NewWindowRequestedEventArgs *a = (ICoreWebView2NewWindowRequestedEventArgs *)args;
    LPWSTR uri = NULL;
    ICoreWebView2NewWindowRequestedEventArgs_put_Handled(a, TRUE);
    ICoreWebView2NewWindowRequestedEventArgs_get_Uri(a, &uri);
    char *u = fbw_u8(uri);
    if (uri) CoTaskMemFree(uri);
    if (u && (fb_starts_with(u, "http://") || fb_starts_with(u, "https://"))) fbw_tab_new(u, 1);
    free(u);
}

static void h_nav_start(void *ctx, void *sender, void *args)
{
    (void)sender;
    VCtx *v = (VCtx *)ctx;
    FbwTab *t = fbw_tab_by_id(v->tab_id);
    ICoreWebView2NavigationStartingEventArgs *a = (ICoreWebView2NavigationStartingEventArgs *)args;
    LPWSTR uri = NULL;
    if (!t) return;
    ICoreWebView2NavigationStartingEventArgs_get_Uri(a, &uri);
    t->loading = 1;
    if (uri) { free(t->url); t->url = fbw_u8(uri); CoTaskMemFree(uri); }
    fbw_engine_nav_event(t, "Navigation.started", NULL);
    fbw_tabs_changed();
}

static void h_nav_done(void *ctx, void *sender, void *args)
{
    (void)sender;
    VCtx *v = (VCtx *)ctx;
    FbwTab *t = fbw_tab_by_id(v->tab_id);
    ICoreWebView2NavigationCompletedEventArgs *a = (ICoreWebView2NavigationCompletedEventArgs *)args;
    BOOL ok = FALSE;
    COREWEBVIEW2_WEB_ERROR_STATUS es = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
    if (!t) return;
    ICoreWebView2NavigationCompletedEventArgs_get_IsSuccess(a, &ok);
    ICoreWebView2NavigationCompletedEventArgs_get_WebErrorStatus(a, &es);
    t->loading = 0;
    t->nav_ok = ok ? 1 : 0;
    t->nav_err = (int)es;
    BOOL b = FALSE;
    ICoreWebView2_get_CanGoBack(t->wv, &b); t->can_back = b ? 1 : 0;
    b = FALSE;
    ICoreWebView2_get_CanGoForward(t->wv, &b); t->can_forward = b ? 1 : 0;
    tab_refresh_source(t);
    tab_refresh_title(t);
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "ev", fbj_str("nav.done"));
    fbj_obj_set(o, "tab", fbj_num(t->id));
    fbj_obj_set(o, "url", fbj_str(t->url ? t->url : ""));
    fbj_obj_set(o, "title", fbj_str(t->title ? t->title : ""));
    fbj_obj_set(o, "ok", fbj_bool(ok));
    char *s = fbj_to_string(o, 0);
    fbj_free(o);
    if (s) { fbw_post(FBW_SHELL, s); free(s); }
    fbw_engine_nav_event(t, "Navigation.completed", NULL);
    fbw_tabs_changed();
}

static void h_source(void *ctx, void *sender, void *args)
{
    (void)sender; (void)args;
    FbwTab *t = fbw_tab_by_id(((VCtx *)ctx)->tab_id);
    if (!t) return;
    tab_refresh_source(t);
    BOOL b = FALSE;
    ICoreWebView2_get_CanGoBack(t->wv, &b); t->can_back = b ? 1 : 0;
    b = FALSE;
    ICoreWebView2_get_CanGoForward(t->wv, &b); t->can_forward = b ? 1 : 0;
    fbw_tabs_changed();
}

static void h_title(void *ctx, void *sender, void *args)
{
    (void)sender; (void)args;
    FbwTab *t = fbw_tab_by_id(((VCtx *)ctx)->tab_id);
    if (!t) return;
    tab_refresh_title(t);
    fbw_tabs_changed();
}

static void h_newwin(void *ctx, void *sender, void *args)
{
    (void)ctx; (void)sender;
    ICoreWebView2NewWindowRequestedEventArgs *a = (ICoreWebView2NewWindowRequestedEventArgs *)args;
    LPWSTR uri = NULL;
    BOOL user = FALSE;
    ICoreWebView2NewWindowRequestedEventArgs_put_Handled(a, TRUE);
    ICoreWebView2NewWindowRequestedEventArgs_get_IsUserInitiated(a, &user);
    ICoreWebView2NewWindowRequestedEventArgs_get_Uri(a, &uri);
    char *u = fbw_u8(uri);
    if (uri) CoTaskMemFree(uri);
    if (u && u[0] && (user || !G.popups_block)) fbw_tab_new(u, 1);
    free(u);
}

static void h_permission(void *ctx, void *sender, void *args)
{
    (void)ctx; (void)sender;
    if (G.perm_deny) ICoreWebView2PermissionRequestedEventArgs_put_State((ICoreWebView2PermissionRequestedEventArgs *)args, COREWEBVIEW2_PERMISSION_STATE_DENY);
}

static void h_dialog(void *ctx, void *sender, void *args)
{
    (void)ctx; (void)sender;
    ICoreWebView2ScriptDialogOpeningEventArgs *a = (ICoreWebView2ScriptDialogOpeningEventArgs *)args;
    if (strcmp(G.dialogs, "accept") != 0) return;       /* dismiss: do nothing (cancelled); manual: the default dialog is shown */
    COREWEBVIEW2_SCRIPT_DIALOG_KIND kind = COREWEBVIEW2_SCRIPT_DIALOG_KIND_ALERT;
    ICoreWebView2ScriptDialogOpeningEventArgs_get_Kind(a, &kind);
    if (kind == COREWEBVIEW2_SCRIPT_DIALOG_KIND_PROMPT) {
        LPWSTR def = NULL;
        ICoreWebView2ScriptDialogOpeningEventArgs_get_DefaultText(a, &def);
        ICoreWebView2ScriptDialogOpeningEventArgs_put_ResultText(a, def ? def : L"");
        if (def) CoTaskMemFree(def);
    }
    ICoreWebView2ScriptDialogOpeningEventArgs_Accept(a);
}

static void h_fullscreen(void *ctx, void *sender, void *args)
{
    (void)sender; (void)args;
    FbwTab *t = fbw_tab_by_id(((VCtx *)ctx)->tab_id);
    BOOL b = FALSE;
    if (!t) return;
    ICoreWebView2_get_ContainsFullScreenElement(t->wv, &b);
    if ((b ? 1 : 0) != G.fullscreen) fbw_toggle_fullscreen();
}

static void h_close_req(void *ctx, void *sender, void *args)
{
    (void)sender; (void)args;
    int id = ((VCtx *)ctx)->tab_id;
    /* closing a tab destroys the view that is calling us: do it after this handler returned */
    PostMessageW(G.hwnd, WM_APP + 5, (WPARAM)id, 0);
}

static void h_chooser(void *ctx, void *sender, void *args)
{
    (void)sender;
    FbwTab *t = fbw_tab_by_id(((VCtx *)ctx)->tab_id);
    LPWSTR js = NULL;
    if (!t) return;
    ICoreWebView2DevToolsProtocolEventReceivedEventArgs_get_ParameterObjectAsJson((ICoreWebView2DevToolsProtocolEventReceivedEventArgs *)args, &js);
    char *u = fbw_u8(js ? js : L"{}");
    if (js) CoTaskMemFree(js);
    fbw_engine_nav_event(t, "Page.fileChooserOpened", u);
    free(u);
}

static void h_zoom(void *ctx, void *sender, void *args)
{
    (void)sender; (void)args;
    FbwTab *t = fbw_tab_by_id(((VCtx *)ctx)->tab_id);
    double z = 1.0;
    if (!t || !t->ctl) return;
    ICoreWebView2Controller_get_ZoomFactor(t->ctl, &z);
    t->zoom = z;
    if (t->id == G.active_id) post_zoom();
}

/* keys pressed while a web page has the focus are forwarded to the shell, which owns the shortcuts */
static void h_key(void *ctx, void *sender, void *args)
{
    (void)ctx; (void)sender;
    ICoreWebView2AcceleratorKeyPressedEventArgs *a = (ICoreWebView2AcceleratorKeyPressedEventArgs *)args;
    COREWEBVIEW2_KEY_EVENT_KIND kind;
    UINT vk = 0;
    if (FAILED(ICoreWebView2AcceleratorKeyPressedEventArgs_get_KeyEventKind(a, &kind))) return;
    if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN && kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN) return;
    ICoreWebView2AcceleratorKeyPressedEventArgs_get_VirtualKey(a, &vk);
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
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
    if (!name) return;
    ICoreWebView2AcceleratorKeyPressedEventArgs_put_Handled(a, TRUE);
    /* the address bar and find box live in the shell view: give it the keyboard before it focuses them */
    if (G.shell.ctl && (!strcmp(name, "focusurl") || !strcmp(name, "find") || !strcmp(name, "newtab") || !strcmp(name, "bookmark") || !strcmp(name, "history")))
        ICoreWebView2Controller_MoveFocus(G.shell.ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    char *s = fbw_dupf("{\"ev\":\"shortcut\",\"name\":\"%s\"}", name);
    if (s) { fbw_post(FBW_SHELL, s); free(s); }
}

/* ------------------------------------------------------------------ creating views */
static void create_controller(HWND parent, FbwHandlerFn fn, void *ctx)
{
    void *h = fbw_handler(fn, ctx);
    HRESULT hr = G.env ? ICoreWebView2Environment_CreateCoreWebView2Controller(G.env, parent, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *)h) : E_FAIL;
    fbw_release(h);
    if (FAILED(hr)) fn(ctx, (void *)(INT_PTR)hr, NULL);
}

#define ADD_EVENT(wv, Event, fn, ctx) do { \
        void *h_ = fbw_handler((fn), (ctx)); EventRegistrationToken tk_; \
        ICoreWebView2_add_##Event((wv), (ICoreWebView2##Event##EventHandler *)h_, &tk_); fbw_release(h_); } while (0)

static void add_resource_filter(ICoreWebView2 *wv)
{
    void *h = fbw_handler(h_resource, NULL);
    EventRegistrationToken tk;
    ICoreWebView2_AddWebResourceRequestedFilter(wv, L"https://appassets.example/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    ICoreWebView2_add_WebResourceRequested(wv, (ICoreWebView2WebResourceRequestedEventHandler *)h, &tk);
    fbw_release(h);
}

static void setup_ui_view(FbwView *v, int kind, ICoreWebView2Controller *ctl, const wchar_t *url)
{
    ICoreWebView2Controller_AddRef(ctl);
    v->ctl = ctl;
    v->kind = kind;
    ICoreWebView2Controller_get_CoreWebView2(ctl, &v->wv);
    if (!v->wv) return;
    ICoreWebView2Settings *st = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(v->wv, &st)) && st) {
        ICoreWebView2Settings_put_IsStatusBarEnabled(st, FALSE);
        ICoreWebView2Settings_put_AreDevToolsEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsZoomControlEnabled(st, FALSE);
        ICoreWebView2Settings_put_IsBuiltInErrorPageEnabled(st, FALSE);
        ICoreWebView2Settings_Release(st);
    }
    ADD_EVENT(v->wv, WebMessageReceived, h_webmsg, vctx(kind, 0));
    ADD_EVENT(v->wv, NavigationStarting, h_ui_nav_start, NULL);
    ADD_EVENT(v->wv, NewWindowRequested, h_newwin_ui, NULL);
    add_resource_filter(v->wv);
    ICoreWebView2_Navigate(v->wv, url);
}

static void on_shell_ctl(void *ctx, void *hr, void *ctl)
{
    free(ctx);
    if (FAILED(HR(hr)) || !ctl) { MessageBoxW(G.hwnd, L"The browser window could not be created (WebView2 error).", L"FlowBrowser", MB_ICONERROR); PostMessageW(G.hwnd, WM_FBW_EXIT, 0, 0); return; }
    setup_ui_view(&G.shell, FBW_SHELL, (ICoreWebView2Controller *)ctl, L"https://appassets.example/shell.html");
    fbw_layout();
}

static void on_panel_ctl(void *ctx, void *hr, void *ctl)
{
    free(ctx);
    if (FAILED(HR(hr)) || !ctl) return;
    setup_ui_view(&G.panel, FBW_PANEL, (ICoreWebView2Controller *)ctl, L"https://appassets.example/panel.html");
    fbw_apply_panel_mode();
}

static void tab_script_added(void *ctx, void *hr, void *id) { (void)ctx; (void)hr; (void)id; }

static void on_tab_ctl(void *ctx, void *hr, void *ctlp)
{
    VCtx *v = (VCtx *)ctx;
    ICoreWebView2Controller *ctl = (ICoreWebView2Controller *)ctlp;
    FbwTab *t = fbw_tab_by_id(v->tab_id);
    int id = v->tab_id;
    free(v);
    if (!t) { if (ctl && SUCCEEDED(HR(hr))) ICoreWebView2Controller_Close(ctl); return; }
    if (FAILED(HR(hr)) || !ctl) {
        t->loading = 0;
        free(t->title);
        t->title = fb_strdup("Could not start the browser view");
        fbw_tabs_changed();
        if (t->pending) { snprintf(t->pending->err, sizeof t->pending->err, "The new tab could not be created"); t->pending->rc = 1; fbw_call_finish(t->pending); t->pending = NULL; }
        return;
    }
    ICoreWebView2Controller_AddRef(ctl);
    t->ctl = ctl;
    ICoreWebView2Controller_get_CoreWebView2(ctl, &t->wv);
    if (!t->wv) return;
    t->ready = 1;

    ICoreWebView2Settings *st = NULL;
    if (SUCCEEDED(ICoreWebView2_get_Settings(t->wv, &st)) && st) {
        ICoreWebView2Settings_put_IsStatusBarEnabled(st, FALSE);
        ICoreWebView2Settings_put_AreDevToolsEnabled(st, TRUE);
        ICoreWebView2Settings_put_AreDefaultScriptDialogsEnabled(st, strcmp(G.dialogs, "manual") == 0);
        ICoreWebView2Settings_Release(st);
    }
    ADD_EVENT(t->wv, WebMessageReceived, h_webmsg, vctx(2, id));
    ADD_EVENT(t->wv, NavigationStarting, h_nav_start, vctx(2, id));
    ADD_EVENT(t->wv, NavigationCompleted, h_nav_done, vctx(2, id));
    ADD_EVENT(t->wv, SourceChanged, h_source, vctx(2, id));
    ADD_EVENT(t->wv, DocumentTitleChanged, h_title, vctx(2, id));
    ADD_EVENT(t->wv, HistoryChanged, h_source, vctx(2, id));
    ADD_EVENT(t->wv, NewWindowRequested, h_newwin, vctx(2, id));
    ADD_EVENT(t->wv, PermissionRequested, h_permission, vctx(2, id));
    ADD_EVENT(t->wv, ScriptDialogOpening, h_dialog, vctx(2, id));
    ADD_EVENT(t->wv, ContainsFullScreenElementChanged, h_fullscreen, vctx(2, id));
    ADD_EVENT(t->wv, WindowCloseRequested, h_close_req, vctx(2, id));
    {
        void *h = fbw_handler(h_key, NULL);
        EventRegistrationToken tk;
        ICoreWebView2Controller_add_AcceleratorKeyPressed(ctl, (ICoreWebView2AcceleratorKeyPressedEventHandler *)h, &tk);
        fbw_release(h);
        h = fbw_handler(h_zoom, vctx(2, id));
        ICoreWebView2Controller_add_ZoomFactorChanged(ctl, (ICoreWebView2ZoomFactorChangedEventHandler *)h, &tk);
        fbw_release(h);
    }
    add_resource_filter(t->wv);

    /* DevTools events used by the automation engine (file chooser interception) */
    {
        ICoreWebView2DevToolsProtocolEventReceiver *rc = NULL;
        if (SUCCEEDED(ICoreWebView2_GetDevToolsProtocolEventReceiver(t->wv, L"Page.fileChooserOpened", &rc)) && rc) {
            void *h = fbw_handler(h_chooser, vctx(2, id));
            EventRegistrationToken tk;
            ICoreWebView2DevToolsProtocolEventReceiver_add_DevToolsProtocolEventReceived(rc, (ICoreWebView2DevToolsProtocolEventReceivedEventHandler *)h, &tk);
            fbw_release(h);
        }
        fbw_cdp(t, "Page.enable", "{}", h_noop_done, NULL);
    }
    /* the page runtime and the picker / recorder go into every page and every frame */
    if (g_tools_w) {
        void *h = fbw_handler(tab_script_added, NULL);
        ICoreWebView2_AddScriptToExecuteOnDocumentCreated(t->wv, g_tools_w, (ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *)h);
        fbw_release(h);
    }
    if (G.default_zoom > 0.2 && G.default_zoom < 5.0 && G.default_zoom != 1.0) { ICoreWebView2Controller_put_ZoomFactor(ctl, G.default_zoom); t->zoom = G.default_zoom; }
    fbw_apply_user_agent(t);

    wchar_t *wu = fbw_w(t->start_url ? t->start_url : NEWTAB_URL);
    if (!wu || FAILED(ICoreWebView2_Navigate(t->wv, wu))) ICoreWebView2_Navigate(t->wv, L"https://appassets.example/newtab.html");
    free(wu);
    fbw_layout();
    if (t->id == G.active_id) post_zoom();
    if (t->pending) { fbw_call_finish(t->pending); t->pending = NULL; }
    fbw_tabs_changed();
}

int fbw_tab_new(const char *url, int activate)
{
    if (!G.env || G.ntabs >= FBW_MAX_TABS) return 0;
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
    create_controller(G.hwnd, on_tab_ctl, vctx(2, t->id));
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
    post_zoom();
    if (t->ctl) ICoreWebView2Controller_MoveFocus(t->ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
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
    if (t->ctl) { ICoreWebView2Controller_Close(t->ctl); ICoreWebView2Controller_Release(t->ctl); }
    if (t->wv) ICoreWebView2_Release(t->wv);
    free(t->title); free(t->url); free(t->start_url);
    free(t);
    if (!G.ntabs && !G.closing) fbw_tab_new(NULL, 1);
    fbw_layout();
    fbw_tabs_changed();
    post_zoom();
}

/* ------------------------------------------------------------------ startup sequence */
void fbw_on_shell_ready(void);
void fbw_on_panel_ready(void);

void fbw_on_shell_ready(void)
{
    if (G.shell.ready && G.ntabs) { fbw_tabs_changed(); post_zoom(); return; }
    G.shell.ready = 1;
    create_controller(G.hwnd, on_panel_ctl, vctx(1, 0));
    fbw_tab_new(G.cli_url, 1);
    post_zoom();
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

static void on_env(void *ctx, void *hr, void *env)
{
    (void)ctx;
    if (FAILED(HR(hr)) || !env) {
        MessageBoxW(G.hwnd,
            L"FlowBrowser needs the Microsoft Edge WebView2 Runtime, and it could not be started.\n\n"
            L"Install it from https://developer.microsoft.com/microsoft-edge/webview2/ (Evergreen Runtime) and start FlowBrowser again.",
            L"FlowBrowser", MB_ICONERROR);
        G.exit_code = 3;
        PostMessageW(G.hwnd, WM_FBW_EXIT, 0, 0);
        return;
    }
    G.env = (ICoreWebView2Environment *)env;
    ICoreWebView2Environment_AddRef(G.env);
    create_controller(G.hwnd, on_shell_ctl, vctx(0, 0));
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
    for (int i = 0; i < G.ntabs; i++) if (G.tabs[i]->ctl) ICoreWebView2Controller_Close(G.tabs[i]->ctl);
    if (G.panel.ctl) ICoreWebView2Controller_Close(G.panel.ctl);
    if (G.shell.ctl) ICoreWebView2Controller_Close(G.shell.ctl);
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
        for (int i = 0; i < G.ntabs; i++) if (G.tabs[i]->ctl) ICoreWebView2Controller_NotifyParentWindowPositionChanged(G.tabs[i]->ctl);
        if (G.shell.ctl) ICoreWebView2Controller_NotifyParentWindowPositionChanged(G.shell.ctl);
        if (G.panel.ctl && !G.panel_float) ICoreWebView2Controller_NotifyParentWindowPositionChanged(G.panel.ctl);
        break;
    case WM_SETFOCUS: {
        FbwTab *t = fbw_active_tab();
        if (t && t->ctl) ICoreWebView2Controller_MoveFocus(t->ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
        else if (G.shell.ctl) ICoreWebView2Controller_MoveFocus(G.shell.ctl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
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
    g_tools_w = fbw_w(G.tools_js);
    return g_tools_w ? 0 : -1;
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

    wchar_t *prof = fbw_w(G.profile_dir);
    void *hd = fbw_handler(on_env, NULL);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, prof, NULL, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)hd);
    fbw_release(hd);
    free(prof);
    if (FAILED(hr)) on_env(NULL, (void *)(INT_PTR)hr, NULL);

    MSG msg;
    int code = 0;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    code = (int)msg.wParam;
    if (G.runner) { fbr_free(G.runner); G.runner = NULL; }
    OleUninitialize();
    return code ? code : G.exit_code;
}

void fbw_request_close(void) { if (G.hwnd) PostMessageW(G.hwnd, WM_CLOSE, 0, 0); }
