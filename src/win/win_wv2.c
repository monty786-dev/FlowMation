/* FlowBrowser for Windows - the WebView2 engine layer.
   Implements the engine interface declared in win_app.h (fbh_*, fbw_tab_*, scripts and DevTools protocol calls) with
   Microsoft Edge WebView2. The Wine / Winlator build replaces this file with src/ie/. */
#include "win_app.h"
#include <stdarg.h>

typedef struct { int kind; int tab_id; } VCtx;     /* kind: 0 shell, 1 panel, 2 tab */
static VCtx *vctx(int kind, int id) { VCtx *v = (VCtx *)malloc(sizeof *v); if (v) { v->kind = kind; v->tab_id = id; } return v; }
#define HR(p) ((HRESULT)(INT_PTR)(p))
#define NEWTAB_URL FBW_NEWTAB_URL
static wchar_t *g_tools_w;          /* page runtime + tools as UTF-16 */

const char *fbw_engine_name(void) { return "WebView2"; }

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
    if (t->id == G.active_id) fbw_post_zoom();
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
    const char *name = fbw_shortcut_name(vk, ctrl, shift, alt);
    if (!name) return;
    ICoreWebView2AcceleratorKeyPressedEventArgs_put_Handled(a, TRUE);
    fbw_shortcut(name);
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
    if (t->id == G.active_id) fbw_post_zoom();
    if (t->pending) { fbw_call_finish(t->pending); t->pending = NULL; }
    fbw_tabs_changed();
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
    G.engine_ready = 1;
    create_controller(G.hwnd, on_shell_ctl, vctx(0, 0));
}

/* ------------------------------------------------------------------ the engine interface (win_app.h) */
int fbh_start(void)
{
    g_tools_w = fbw_w(G.tools_js);
    wchar_t *prof = fbw_w(G.profile_dir);
    void *hd = fbw_handler(on_env, NULL);
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(NULL, prof, NULL, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *)hd);
    fbw_release(hd);
    free(prof);
    if (FAILED(hr)) on_env(NULL, (void *)(INT_PTR)hr, NULL);
    return FAILED(hr) ? 1 : 0;
}

void fbh_create_panel(void) { create_controller(G.hwnd, on_panel_ctl, vctx(1, 0)); }
void fbh_create_tab(FbwTab *t) { create_controller(G.hwnd, on_tab_ctl, vctx(2, t->id)); }

void fbh_destroy_tab(FbwTab *t)
{
    if (t->ctl) { ICoreWebView2Controller_Close(t->ctl); ICoreWebView2Controller_Release(t->ctl); }
    if (t->wv) ICoreWebView2_Release(t->wv);
}

void fbh_post_view(FbwView *v, const char *json)
{
    wchar_t *w = fbw_w(json);
    if (!w) return;
    ICoreWebView2_PostWebMessageAsJson(v->wv, w);
    free(w);
}

void fbh_set_bounds(FbwCtl c, const RECT *r) { RECT x = *r; ICoreWebView2Controller_put_Bounds(c, x); }
void fbh_set_visible(FbwCtl c, int visible) { ICoreWebView2Controller_put_IsVisible(c, visible ? TRUE : FALSE); }
void fbh_set_parent(FbwCtl c, HWND parent) { ICoreWebView2Controller_put_ParentWindow(c, parent); }
void fbh_parent_moved(FbwCtl c) { ICoreWebView2Controller_NotifyParentWindowPositionChanged(c); }
void fbh_focus(FbwCtl c) { ICoreWebView2Controller_MoveFocus(c, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); }
void fbh_close_view(FbwCtl c) { ICoreWebView2Controller_Close(c); }
int  fbh_pretranslate(MSG *m) { (void)m; return 0; }
void fbh_timer(void) { }

int fbw_tab_navigate(FbwTab *t, const char *url)
{
    wchar_t *w = fbw_w(url);
    int bad = !t || !t->wv || !w || FAILED(ICoreWebView2_Navigate(t->wv, w));
    free(w);
    return bad ? 1 : 0;
}

int fbw_tab_can_go(FbwTab *t, int forward)
{
    BOOL b = FALSE;
    if (!t || !t->wv) return 0;
    if (forward) ICoreWebView2_get_CanGoForward(t->wv, &b); else ICoreWebView2_get_CanGoBack(t->wv, &b);
    return b ? 1 : 0;
}

void fbw_tab_nav(FbwTab *t, int op)
{
    if (!t || !t->wv) return;
    switch (op) {
    case FBW_NAV_BACK: ICoreWebView2_GoBack(t->wv); break;
    case FBW_NAV_FORWARD: ICoreWebView2_GoForward(t->wv); break;
    case FBW_NAV_RELOAD: ICoreWebView2_Reload(t->wv); break;
    case FBW_NAV_STOP: ICoreWebView2_Stop(t->wv); break;
    }
}

double fbw_tab_get_zoom(FbwTab *t)
{
    double z = 1.0;
    if (t && t->ctl) ICoreWebView2Controller_get_ZoomFactor(t->ctl, &z);
    return z;
}

void fbw_tab_set_zoom(FbwTab *t, double z) { if (t && t->ctl) ICoreWebView2Controller_put_ZoomFactor(t->ctl, z); }
void fbw_tab_devtools(FbwTab *t) { if (t && t->wv) ICoreWebView2_OpenDevToolsWindow(t->wv); }

void fbw_clear_data(FbwTab *t, int cookies, int cache)
{
    if (!t) return;
    if (cookies) fbw_cdp(t, "Network.clearBrowserCookies", "{}", h_noop_done, NULL);
    if (cache) fbw_cdp(t, "Network.clearBrowserCache", "{}", h_noop_done, NULL);
}
