/* FlowBrowser for Wine - views, tabs, navigation events and the polled message transport (the engine interface of win_app.h).

   MSHTML has no web-message channel, so pages and host talk by polling:
     UI pages  (shell, Studio panel)  host -> page: window.__fbDeliver(json)       page -> host: window.__fbPull()
     web pages (tabs)                 page tools -> host: window.__fbPullTools()   (host -> page: script calls)
   The timer in fbh_timer() drives it (about 30 ms). */
#include "ie_app.h"

typedef struct { const char *name; const unsigned char *data; unsigned long size; } FbEmbed;
extern const FbEmbed fb_embedded[];
extern const int     fb_embedded_count;

static char g_ui_base[1200];          /* file:///.../ui/  : where the UI pages are written and loaded from */
static int  g_popup_target;           /* the next tab is the target of a window.open(): do not navigate it */

const char *fbw_engine_name(void) { return "MSHTML (Wine)"; }

/* ------------------------------------------------------------------ the UI files */
static void append_url_path(FbBuf *b, const char *path)
{
    static const char *hex = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (*p == '\\') fb_buf_putc(b, '/');
        else if (isalnum(*p) || strchr("-_.~/:()", *p)) fb_buf_putc(b, (char)*p);
        else { fb_buf_putc(b, '%'); fb_buf_putc(b, hex[*p >> 4]); fb_buf_putc(b, hex[*p & 15]); }
    }
}

/* The UI pages live in the embedded data; MSHTML cannot load pages from memory, so they are written next to the profile. */
static int ui_prepare(char *err, size_t errsz)
{
    char *dir = fbw_dupf("%s\\ui", G.data_dir);
    if (!dir) return 1;
    if (fb_mkdirs(dir)) { fb_set_err(err, errsz, "The folder %s cannot be created", dir); free(dir); return 1; }
    int n = 0;
    for (int i = 0; i < fb_embedded_count; i++) {
        const FbEmbed *e = &fb_embedded[i];
        if (strncmp(e->name, "ui/", 3) != 0) continue;
        char *path = fbw_dupf("%s\\%s", dir, e->name + 3);
        if (!path) continue;
        if (fb_write_file(path, e->data, e->size)) { fb_set_err(err, errsz, "The file %s cannot be written", path); free(path); free(dir); return 1; }
        free(path);
        n++;
    }
    FbBuf b;
    fb_buf_init(&b);
    fb_buf_puts(&b, "file:///");
    append_url_path(&b, dir);
    fb_buf_putc(&b, '/');
    snprintf(g_ui_base, sizeof g_ui_base, "%s", fb_buf_cstr(&b));
    fb_buf_free(&b);
    free(dir);
    fbw_log("UI: %d files written, loaded from %s", n, g_ui_base);
    return n ? 0 : 1;
}

static int is_ui_url(const char *u) { return fb_starts_with(u, g_ui_base) || !_strnicmp(u, "about:blank", 11); }
static int is_http(const char *u) { return fb_starts_with(u, "http://") || fb_starts_with(u, "https://"); }

/* the address the rest of the app uses for the new-tab page is a made-up https address: map it to the real file */
static char *internal_url(const char *url)
{
    if (!strcmp(url, FBW_NEWTAB_URL) || fb_starts_with(url, FBW_NEWTAB_URL "?")) return fbw_dupf("%snewtab.html?host=ie", g_ui_base);
    return fb_strdup(url);
}

static char *public_url(const char *url)
{
    size_t bl = strlen(g_ui_base);
    if (g_ui_base[0] && !strncmp(url, g_ui_base, bl) && !strncmp(url + bl, "newtab.html", 11)) return fb_strdup(FBW_NEWTAB_URL);
    return fb_strdup(url);
}

/* ------------------------------------------------------------------ views */
static IeView *view_new(int kind, int tab_id)
{
    IeView *v = (IeView *)calloc(1, sizeof *v);
    if (v) { v->kind = kind; v->tab_id = tab_id; }
    return v;
}

static int view_open(IeView *v, HWND parent)
{
    v->host = ie_create_host(parent, &v->br);
    if (!v->host) return 1;
    if (ie_sink_attach(v)) fbw_log("view: the browser events could not be connected (kind %d)", v->kind);
    return 0;
}

static void view_close(IeView *v)
{
    if (!v) return;
    ie_sink_detach(v);
    if (v->win) { IDispatch_Release(v->win); v->win = NULL; }
    if (v->br) { IWebBrowser2_Stop(v->br); IWebBrowser2_Release(v->br); v->br = NULL; }
    if (v->host) { DestroyWindow(v->host); v->host = NULL; }
    for (int i = 0; i < v->nq; i++) free(v->queue[i]);
    free(v->queue);
    v->queue = NULL; v->nq = 0;
}

IeView *ie_tab_view(FbwTab *t) { return t ? (IeView *)t->eng : NULL; }
static FbwTab *tab_of(IeView *v) { return v->kind == 2 ? fbw_tab_by_id(v->tab_id) : NULL; }

static IeView *view_by_host(HWND h)
{
    if (G.shell.eng && ((IeView *)G.shell.eng)->host == h) return (IeView *)G.shell.eng;
    if (G.panel.eng && ((IeView *)G.panel.eng)->host == h) return (IeView *)G.panel.eng;
    for (int i = 0; i < G.ntabs; i++) if (G.tabs[i]->eng && ((IeView *)G.tabs[i]->eng)->host == h) return (IeView *)G.tabs[i]->eng;
    return NULL;
}

static IeView *view_for_window(HWND w)
{
    if (!w) return NULL;
    IeView *c[2] = { (IeView *)G.shell.eng, (IeView *)G.panel.eng };
    for (int i = 0; i < 2; i++) if (c[i] && c[i]->host && (w == c[i]->host || IsChild(c[i]->host, w))) return c[i];
    for (int i = 0; i < G.ntabs; i++) {
        IeView *v = (IeView *)G.tabs[i]->eng;
        if (v && v->host && (w == v->host || IsChild(v->host, w))) return v;
    }
    return NULL;
}

static void load_ui(IeView *v, const char *page)
{
    char *u = fbw_dupf("%s%s?host=ie", g_ui_base, page);
    VARIANT url, e[4];
    if (!u) return;
    VariantInit(&url);
    V_VT(&url) = VT_BSTR;
    V_BSTR(&url) = ie_bstr(u);
    for (int i = 0; i < 4; i++) VariantInit(&e[i]);
    v->doc_ready = 0;
    IWebBrowser2_Navigate2(v->br, &url, &e[0], &e[1], &e[2], &e[3]);
    SysFreeString(V_BSTR(&url));
    free(u);
}

static void refresh_window(IeView *v)
{
    if (v->win) { IDispatch_Release(v->win); v->win = NULL; }
    v->win = ie_top_window(v->br);
}

/* ------------------------------------------------------------------ host -> UI pages */
static int deliver(IeView *v, const char *json)
{
    VARIANT a;
    if (!v->win) return 1;
    VariantInit(&a);
    V_VT(&a) = VT_BSTR;
    V_BSTR(&a) = ie_bstr(json);
    if (!V_BSTR(&a)) return 1;
    HRESULT hr = ie_call(v->win, L"__fbDeliver", &a, 1, NULL);
    SysFreeString(V_BSTR(&a));
    return FAILED(hr) ? 1 : 0;
}

static void flush_queue(IeView *v)
{
    int i = 0;
    while (i < v->nq && v->win && v->doc_ready && !deliver(v, v->queue[i])) { free(v->queue[i]); i++; }
    if (i) { memmove(v->queue, v->queue + i, (size_t)(v->nq - i) * sizeof(char *)); v->nq -= i; }
}

void fbh_post_view(FbwView *vw, const char *json)
{
    IeView *v = (IeView *)vw->eng;
    if (!v || !json) return;
    if (v->nq == 0 && v->win && v->doc_ready && !deliver(v, json)) return;
    if (v->nq >= 4000) return;                 /* the page is not taking messages: drop rather than grow without bound */
    char **q = (char **)realloc(v->queue, (size_t)(v->nq + 1) * sizeof(char *));
    if (!q) return;
    v->queue = q;
    v->queue[v->nq++] = fb_strdup(json);
}

/* ------------------------------------------------------------------ polling (page -> host) */
static char *pull(IeView *v, const wchar_t *fn)
{
    VARIANT r;
    if (!v->win || !v->doc_ready) return NULL;
    if (FAILED(ie_call(v->win, fn, NULL, 0, &r))) { VariantClear(&r); return NULL; }
    char *s = ie_variant_u8(&r);
    VariantClear(&r);
    if (s && !s[0]) { free(s); s = NULL; }
    return s;
}

static void poll_ui(IeView *v)
{
    char *s = pull(v, L"__fbPull");
    if (!s) return;
    char err[100];
    FbJson *arr = fbj_parse_str(s, err, sizeof err);
    free(s);
    if (!arr) return;
    if (arr->type == FBJ_ARR) for (size_t i = 0; i < fbj_len(arr); i++) {
        const FbJson *m = fbj_at(arr, i);
        if (m && m->type == FBJ_OBJ) fbw_command(v->kind, (FbJson *)m);
    }
    fbj_free(arr);
}

static void poll_tab(IeView *v)
{
    int id = v->tab_id;
    char *s = pull(v, L"__fbPullTools");
    if (!s) return;
    char err[100];
    FbJson *arr = fbj_parse_str(s, err, sizeof err);
    free(s);
    if (!arr) return;
    if (arr->type == FBJ_ARR) for (size_t i = 0; i < fbj_len(arr); i++) {
        FbwTab *t = fbw_tab_by_id(id);
        if (!t) break;
        char *js = fbj_to_string(fbj_at(arr, i), 0);
        if (js) { fbw_page_message(t, js); free(js); }
    }
    fbj_free(arr);
}

void fbh_timer(void)
{
    static int busy;
    static unsigned tick;
    if (busy) return;                          /* a modal dialog's message loop can deliver the next tick inside this one */
    busy = 1;
    tick++;
    ie_engine_tick();
    IeView *s = (IeView *)G.shell.eng, *p = (IeView *)G.panel.eng;
    if (s) { flush_queue(s); poll_ui(s); }
    if (p) { flush_queue(p); poll_ui(p); }
    if (!ie_dialog_open() && (tick & 1) == 0) {
        for (int i = 0; i < G.ntabs; i++) {
            IeView *v = (IeView *)G.tabs[i]->eng;
            int id = G.tabs[i]->id;
            if (v) poll_tab(v);
            if (!fbw_tab_by_id(id)) break;     /* the tab list changed under us */
        }
    }
    busy = 0;
}

/* ------------------------------------------------------------------ navigation events */
static int is_top(IeView *v, IDispatch *frame) { return !frame || ie_same_object((IUnknown *)frame, (IUnknown *)v->br); }

void ie_on_before_navigate(IeView *v, IDispatch *frame, const char *url, int *cancel)
{
    int top = is_top(v, frame);
    if (v->kind != 2) {
        /* the UI pages never leave their own folder: a link in them opens in a real tab */
        if (top && !is_ui_url(url)) { *cancel = 1; if (is_http(url)) fbw_tab_new(url, 1); }
        return;
    }
    if (!top || !_strnicmp(url, "javascript:", 11)) return;
    FbwTab *t = tab_of(v);
    if (!t) return;
    v->doc_ready = 0;
    v->top_error = 0;
    t->loading = 1;
    t->nav_ok = 1;
    free(t->url);
    t->url = public_url(url);
    fbw_engine_nav_event(t, "Navigation.started", NULL);
    fbw_tabs_changed();
}

void ie_on_navigate_complete(IeView *v, IDispatch *frame, const char *url)
{
    FbwTab *t = tab_of(v);
    if (!t || !is_top(v, frame)) return;
    free(t->url);
    t->url = public_url(url);
    fbw_tabs_changed();
}

static void read_title(IeView *v, FbwTab *t)
{
    IDispatch *doc = NULL;
    VARIANT r;
    if (!v->br || FAILED(IWebBrowser2_get_Document(v->br, &doc)) || !doc) return;
    if (SUCCEEDED(ie_get(doc, L"title", &r))) {
        char *s = ie_variant_u8(&r);
        if (s) { free(t->title); t->title = s; }
    }
    VariantClear(&r);
    IDispatch_Release(doc);
}

void ie_on_document_complete(IeView *v, IDispatch *frame, const char *url)
{
    int top = is_top(v, frame);
    if (v->kind != 2) {
        if (top) { refresh_window(v); v->doc_ready = 1; flush_queue(v); }
        return;
    }
    FbwTab *t = tab_of(v);
    if (!t) return;
    /* the legacy layer, the page runtime and the tools go into every document, the way WebView2 injects them */
    {
        IDispatch *w = NULL;
        if (top) w = ie_top_window(v->br);
        else {
            IWebBrowser2 *fb = NULL;
            if (frame && SUCCEEDED(IDispatch_QueryInterface(frame, &IID_IWebBrowser2, (void **)&fb)) && fb) { w = ie_top_window(fb); IWebBrowser2_Release(fb); }
        }
        if (w) { ie_inject_into(w); IDispatch_Release(w); }
    }
    if (!top) return;
    refresh_window(v);
    v->doc_ready = 1;
    t->loading = 0;
    t->nav_ok = v->top_error ? 0 : 1;
    free(t->url);
    t->url = public_url(url);
    read_title(v, t);

    FbJson *o = fbj_obj();
    fbj_obj_set(o, "ev", fbj_str("nav.done"));
    fbj_obj_set(o, "tab", fbj_num(t->id));
    fbj_obj_set(o, "url", fbj_str(t->url ? t->url : ""));
    fbj_obj_set(o, "title", fbj_str(t->title ? t->title : ""));
    fbj_obj_set(o, "ok", fbj_bool(t->nav_ok));
    char *s = fbj_to_string(o, 0);
    fbj_free(o);
    if (s) { fbw_post(FBW_SHELL, s); free(s); }
    fbw_engine_nav_event(t, "Navigation.completed", NULL);
    fbw_tabs_changed();
}

void ie_on_title(IeView *v, const char *title)
{
    FbwTab *t = tab_of(v);
    if (!t) return;
    free(t->title);
    t->title = fb_strdup(title);
    fbw_tabs_changed();
}

void ie_on_nav_error(IeView *v, IDispatch *frame, const char *url, int status)
{
    FbwTab *t = tab_of(v);
    (void)url;
    if (!t || !is_top(v, frame)) return;
    v->top_error = status ? status : 1;
    t->nav_err = 0;                            /* the engine's text table is WebView2's: the generic message is used */
}

void ie_on_command_state(IeView *v, int command, int enabled)
{
    FbwTab *t = tab_of(v);
    if (!t) return;
    if (command == 1) t->can_forward = enabled;        /* CSC_NAVIGATEFORWARD */
    else if (command == 2) t->can_back = enabled;      /* CSC_NAVIGATEBACK */
    else return;
    fbw_tabs_changed();
}

void ie_on_new_window(IeView *v, const char *url, int user, int *cancel, IDispatch **popup_target)
{
    (void)v;
    *cancel = 0;
    if (G.popups_block && !user) { *cancel = 1; return; }
    if (url[0]) {
        *cancel = 1;
        if (is_http(url) || !_strnicmp(url, "file:", 5)) fbw_tab_new(url, 1);
        return;
    }
    if (!popup_target) { *cancel = 1; return; }
    /* no address yet (window.open('') or NewWindow2): give the page a fresh tab and let it navigate there itself */
    g_popup_target = 1;
    int id = fbw_tab_new(NULL, 1);
    g_popup_target = 0;
    FbwTab *nt = id ? fbw_tab_by_id(id) : NULL;
    if (!nt || !nt->wv) { *cancel = 1; return; }
    IWebBrowser2_QueryInterface((IWebBrowser2 *)nt->wv, &IID_IDispatch, (void **)popup_target);
}

/* ------------------------------------------------------------------ the engine interface */
int fbh_start(void)
{
    char err[300] = "";
    if (ie_build_inject_script()) { MessageBoxW(G.hwnd, L"The page tools are missing from this build.", L"FlowBrowser", MB_ICONERROR); G.exit_code = 2; PostMessageW(G.hwnd, WM_FBW_EXIT, 0, 0); return 1; }
    if (ui_prepare(err, sizeof err) || ie_atl_init(err, sizeof err)) {
        wchar_t *w = fbw_w(err[0] ? err : "The user interface could not be prepared.");
        wchar_t msg[700];
        swprintf(msg, sizeof msg / sizeof msg[0], L"FlowBrowser (Wine build) cannot start.\n\n%ls\n\nIn Winlator, install Wine Gecko and Wine Mono in the container (see docs/WINLATOR.md).", w ? w : L"");
        MessageBoxW(G.hwnd, msg, L"FlowBrowser", MB_ICONERROR);
        free(w);
        G.exit_code = 3;
        PostMessageW(G.hwnd, WM_FBW_EXIT, 0, 0);
        return 1;
    }
    ie_apply_proxy_from_settings();
    G.engine_ready = 1;
    IeView *v = view_new(FBW_SHELL, 0);
    if (!v || view_open(v, G.hwnd)) {
        MessageBoxW(G.hwnd, L"The browser control (Wine Gecko / MSHTML) could not be created.\n\nInstall Wine Gecko in the container: docs/WINLATOR.md",
                    L"FlowBrowser", MB_ICONERROR);
        G.exit_code = 3;
        PostMessageW(G.hwnd, WM_FBW_EXIT, 0, 0);
        return 1;
    }
    G.shell.eng = v; G.shell.ctl = v->host; G.shell.wv = v->br; G.shell.kind = FBW_SHELL;
    load_ui(v, "shell.html");
    SetTimer(G.hwnd, 1, 30, NULL);
    fbw_layout();
    return 0;
}

void fbh_create_panel(void)
{
    IeView *v = view_new(FBW_PANEL, 0);
    if (!v || view_open(v, G.hwnd)) { free(v); return; }
    G.panel.eng = v; G.panel.ctl = v->host; G.panel.wv = v->br; G.panel.kind = FBW_PANEL;
    load_ui(v, "panel.html");
    fbw_apply_panel_mode();
}

void fbh_create_tab(FbwTab *t)
{
    IeView *v = view_new(2, t->id);
    t->eng = v;
    if (!v || view_open(v, G.hwnd)) {
        t->loading = 0;
        free(t->title);
        t->title = fb_strdup("Could not start the browser view");
        if (t->pending) { snprintf(t->pending->err, sizeof t->pending->err, "The new tab could not be created"); t->pending->rc = 1; fbw_call_finish(t->pending); t->pending = NULL; }
        return;
    }
    t->ctl = v->host; t->wv = v->br; t->ready = 1;
    if (G.default_zoom > 0.2 && G.default_zoom < 5.0 && G.default_zoom != 1.0) fbw_tab_set_zoom(t, G.default_zoom);
    fbw_apply_user_agent(t);
    if (!g_popup_target) {
        char *u = internal_url(t->start_url ? t->start_url : FBW_NEWTAB_URL);
        if (!u || fbw_tab_navigate(t, u)) { free(u); u = internal_url(FBW_NEWTAB_URL); fbw_tab_navigate(t, u); }
        free(u);
    }
}

void fbh_destroy_tab(FbwTab *t)
{
    IeView *v = (IeView *)t->eng;
    view_close(v);
    free(v);
    t->eng = NULL; t->ctl = NULL; t->wv = NULL;
}

void fbh_set_bounds(FbwCtl c, const RECT *r)
{
    if (c) SetWindowPos(c, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
}
void fbh_set_visible(FbwCtl c, int visible) { if (c) ShowWindow(c, visible ? SW_SHOWNA : SW_HIDE); }
void fbh_set_parent(FbwCtl c, HWND parent) { if (c) SetParent(c, parent); }
void fbh_parent_moved(FbwCtl c) { (void)c; }
void fbh_focus(FbwCtl c) { if (c) SetFocus(c); }      /* the ATL window activates the control when it gets the focus */

void fbh_close_view(FbwCtl c)
{
    IeView *v = view_by_host(c);
    if (!v) return;
    view_close(v);
}

/* Keys: shortcuts first (tabs only; the UI pages handle their own), then the control's own accelerators (Tab, arrows, ...). */
int fbh_pretranslate(MSG *m)
{
    if (m->message < 0x100 || m->message > 0x109) return 0;       /* WM_KEYFIRST .. WM_KEYLAST */
    IeView *v = view_for_window(m->hwnd);
    if (!v || !v->br) return 0;
    if (v->kind == 2 && (m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN)) {
        int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        const char *name = fbw_shortcut_name((unsigned)m->wParam, ctrl, shift, alt);
        if (name) { fbw_shortcut(name); return 1; }
    }
    IOleInPlaceActiveObject *ao = NULL;
    if (SUCCEEDED(IWebBrowser2_QueryInterface(v->br, &IID_IOleInPlaceActiveObject, (void **)&ao)) && ao) {
        HRESULT hr = IOleInPlaceActiveObject_TranslateAccelerator(ao, m);
        IOleInPlaceActiveObject_Release(ao);
        if (hr == S_OK) return 1;
    }
    return 0;
}

int fbw_tab_navigate(FbwTab *t, const char *url)
{
    IeView *v = ie_tab_view(t);
    VARIANT u, e[4];
    if (!v || !v->br || !url) return 1;
    char *real = internal_url(url);
    if (!real) return 1;
    VariantInit(&u);
    V_VT(&u) = VT_BSTR;
    V_BSTR(&u) = ie_bstr(real);
    free(real);
    for (int i = 0; i < 4; i++) VariantInit(&e[i]);
    HRESULT hr = V_BSTR(&u) ? IWebBrowser2_Navigate2(v->br, &u, &e[0], &e[1], &e[2], &e[3]) : E_OUTOFMEMORY;
    SysFreeString(V_BSTR(&u));
    return FAILED(hr) ? 1 : 0;
}

int fbw_tab_can_go(FbwTab *t, int forward) { return t ? (forward ? t->can_forward : t->can_back) : 0; }

void fbw_tab_nav(FbwTab *t, int op)
{
    IeView *v = ie_tab_view(t);
    if (!v || !v->br) return;
    switch (op) {
    case FBW_NAV_BACK: IWebBrowser2_GoBack(v->br); break;
    case FBW_NAV_FORWARD: IWebBrowser2_GoForward(v->br); break;
    case FBW_NAV_RELOAD: IWebBrowser2_Refresh(v->br); break;
    case FBW_NAV_STOP: IWebBrowser2_Stop(v->br); break;
    }
}

/* Page zoom: OLECMDID_OPTICAL_ZOOM (63) takes a percentage. Wine may ignore it; the factor is remembered either way. */
double fbw_tab_get_zoom(FbwTab *t) { return t && t->zoom > 0 ? t->zoom : 1.0; }

void fbw_tab_set_zoom(FbwTab *t, double z)
{
    IeView *v = ie_tab_view(t);
    if (!v || !v->br) return;
    IOleCommandTarget *ct = NULL;
    if (SUCCEEDED(IWebBrowser2_QueryInterface(v->br, &IID_IOleCommandTarget, (void **)&ct)) && ct) {
        VARIANT in;
        VariantInit(&in);
        V_VT(&in) = VT_I4;
        V_I4(&in) = (LONG)(z * 100.0 + 0.5);
        IOleCommandTarget_Exec(ct, NULL, 63, 2 /* OLECMDEXECOPT_DONTPROMPTUSER */, &in, NULL);
        IOleCommandTarget_Release(ct);
    }
    t->zoom = z;
}

void fbw_tab_devtools(FbwTab *t)
{
    (void)t;
    MessageBoxW(G.hwnd, L"Developer tools are not available in the Wine build (the browser engine is Wine's MSHTML / Gecko).\n\n"
                        L"Use the Studio panel's element picker and the run log instead.", L"FlowBrowser", MB_ICONINFORMATION);
}
