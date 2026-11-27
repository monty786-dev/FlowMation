/* FlowBrowser for Wine - the automation side of the MSHTML engine:
     * the runner's DevTools calls (fbie_cdp -> src/core/fb_legacy.c -> scripts in the page, see src/js/fb_legacy.js),
     * page script injection, native file-dialog automation for uploads, screenshots, user agent, proxy, cookies. */
#include "ie_app.h"

/* ------------------------------------------------------------------ injection */
static char *g_inject_legacy;

int ie_build_inject_script(void)
{
    size_t n = 0;
    const unsigned char *lg = fbw_resource("js/fb_legacy.js", &n, NULL);
    if (!lg || !G.tools_js) return 1;
    g_inject_legacy = (char *)malloc(n + 1);
    if (!g_inject_legacy) return 1;
    memcpy(g_inject_legacy, lg, n);
    g_inject_legacy[n] = 0;
    return 0;
}

void ie_set_dialog_policy(IDispatch *win)
{
    char js[160], err[100];
    snprintf(js, sizeof js, "if(window.__fbLegacy)window.__fbLegacy.dialogs='%s';", !strcmp(G.dialogs, "accept") ? "accept" : !strcmp(G.dialogs, "dismiss") ? "dismiss" : "manual");
    ie_exec_script(win, js, err, sizeof err);
}

/* Three separate scripts: a failure in one must not stop the next. Each one is skipped when the document already has it. */
void ie_inject_into(IDispatch *win)
{
    char err[100] = "";
    if (!win) return;
    if (!ie_has_prop(win, L"__fbLegacy") && g_inject_legacy && ie_exec_script(win, g_inject_legacy, err, sizeof err)) fbw_log("inject: legacy layer was refused (%s)", err);
    if (!ie_has_prop(win, L"__fb") && ie_exec_script(win, G.tools_js, err, sizeof err)) fbw_log("inject: page tools were refused (%s)", err);
    ie_set_dialog_policy(win);
}

void fbw_apply_dialog_mode(void)
{
    for (int i = 0; i < G.ntabs; i++) {
        IeView *v = ie_tab_view(G.tabs[i]);
        IDispatch *w = v && v->br && v->doc_ready ? ie_top_window(v->br) : NULL;
        if (w) { ie_set_dialog_policy(w); IDispatch_Release(w); }
    }
}

/* ------------------------------------------------------------------ small helpers */
static void fail(FbwCall *c, const char *msg) { c->rc = 1; snprintf(c->err, sizeof c->err, "%s", msg); }
static FbwTab *call_tab(FbwCall *c) { return c->i1 ? fbw_tab_by_id(c->i1) : G.run_tab; }

/* Runs a call on the UI thread and waits. Called from the UI thread itself it just runs. */
static int run_on_ui(FbwCall *c, DWORD timeout)
{
    if (GetCurrentThreadId() == G.ui_thread) {
        c->fn(c);
        fbw_call_release(c);                    /* the reference the UI thread would have dropped */
        return 0;
    }
    return fbw_ui_call(c, timeout);
}

static int roundtrip(FbwCall *c, DWORD timeout, FbBuf *out, char *err, size_t errsz)
{
    int rc = 1;
    if (!c) { fb_set_err(err, errsz, "Out of memory"); return 1; }
    int w = run_on_ui(c, timeout);
    if (w == 0) {
        if (c->rc == 0) { if (out && c->out) fb_buf_puts(out, c->out); rc = 0; }
        else fb_set_err(err, errsz, "%s", c->err);
    } else if (w == 2) fb_set_err(err, errsz, "Stopped");
    else fb_set_err(err, errsz, "The page did not answer in time");
    fbw_call_release(c);
    return rc;
}

/* ------------------------------------------------------------------ native dialogs (file chooser) */
typedef struct { HWND any, file; } Found;
static int    g_intercept;          /* Page.setInterceptFileChooserDialog is on */
static HWND   g_dlg;                /* an intercepted file dialog, waiting for set_files */
static DWORD  g_dlg_since;

static int is_file_dialog(HWND h)
{
    /* the classic Open dialog: file-name combo box 0x47C or edit 0x480 */
    return GetDlgItem(h, 0x47C) != NULL || GetDlgItem(h, 0x480) != NULL;
}

static BOOL CALLBACK enum_dialogs(HWND h, LPARAM lp)
{
    Found *f = (Found *)lp;
    wchar_t cls[40];
    if (h == G.hwnd || h == G.float_hwnd) return TRUE;
    if (!GetClassNameW(h, cls, (int)(sizeof cls / sizeof cls[0])) || wcscmp(cls, L"#32770") != 0) return TRUE;
    if (!f->any) f->any = h;
    if (!f->file && is_file_dialog(h)) f->file = h;
    return TRUE;
}

static void find_dialogs(Found *f)
{
    f->any = f->file = NULL;
    EnumThreadWindows(G.ui_thread, enum_dialogs, (LPARAM)f);
}

int ie_dialog_open(void)
{
    Found f;
    find_dialogs(&f);
    return f.any != NULL;
}

void ie_engine_tick(void)
{
    Found f;
    find_dialogs(&f);
    if (g_dlg && !IsWindow(g_dlg)) g_dlg = NULL;
    if (f.file && !g_dlg && g_intercept) {
        g_dlg = f.file;
        g_dlg_since = GetTickCount();
        ShowWindow(g_dlg, SW_HIDE);                      /* the runner answers it; the person need not see it */
        fbw_engine_nav_event(G.run_tab, "Page.fileChooserOpened", "{\"backendNodeId\":1}");
    }
    if (g_dlg && GetTickCount() - g_dlg_since > 25000) {      /* nobody answered: do not leave the page blocked */
        PostMessageW(g_dlg, WM_COMMAND, MAKEWPARAM(2 /* IDCANCEL */, 0), 0);
        g_dlg = NULL;
    }
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG m;
    while ((LONG)(end - GetTickCount()) > 0) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
}

static void dialog_set_name_and_ok(HWND dlg, const wchar_t *text)
{
    HWND combo = GetDlgItem(dlg, 0x47C), edit = GetDlgItem(dlg, 0x480);
    HWND ed = combo ? FindWindowExW(combo, NULL, L"Edit", NULL) : NULL;
    if (!ed && combo) {                                   /* ComboBoxEx32 > ComboBox > Edit */
        HWND inner = FindWindowExW(combo, NULL, L"ComboBox", NULL);
        if (inner) ed = FindWindowExW(inner, NULL, L"Edit", NULL);
    }
    if (ed) SendMessageW(ed, WM_SETTEXT, 0, (LPARAM)text);
    else if (combo) SendMessageW(combo, WM_SETTEXT, 0, (LPARAM)text);
    else if (edit) SendMessageW(edit, WM_SETTEXT, 0, (LPARAM)text);
    SendMessageW(dlg, WM_COMMAND, MAKEWPARAM(1 /* IDOK */, 0 /* BN_CLICKED */), (LPARAM)GetDlgItem(dlg, 1));
}

static int fill_dialog(HWND dlg, const FbJson *paths, char *err, size_t errsz)
{
    size_t n = paths ? fbj_len(paths) : 0;
    if (!n) { fb_set_err(err, errsz, "No file was given"); return 1; }
    if (n == 1) {
        wchar_t *w = fbw_w(fbj_at(paths, 0)->str ? fbj_at(paths, 0)->str : "");
        if (!w) { fb_set_err(err, errsz, "Out of memory"); return 1; }
        dialog_set_name_and_ok(dlg, w);
        free(w);
    } else {
        /* several files: the dialog takes "a" "b" from one folder */
        wchar_t **w = (wchar_t **)calloc(n, sizeof *w);
        size_t dirlen = 0;
        int ok = w != NULL;
        for (size_t i = 0; ok && i < n; i++) {
            w[i] = fbw_w(fbj_at(paths, i)->str ? fbj_at(paths, i)->str : "");
            wchar_t *sl = w[i] ? wcsrchr(w[i], L'\\') : NULL;
            if (!sl) sl = w[i] ? wcsrchr(w[i], L'/') : NULL;
            if (!sl) { ok = 0; break; }
            size_t dl = (size_t)(sl - w[i]);
            if (i == 0) dirlen = dl;
            else if (dl != dirlen || _wcsnicmp(w[i], w[0], dl) != 0) ok = 0;
        }
        if (!ok) {
            for (size_t i = 0; w && i < n; i++) free(w[i]);
            free(w);
            fb_set_err(err, errsz, "This browser engine can only upload several files at once when they are in the same folder");
            return 1;
        }
        wchar_t *dir = (wchar_t *)calloc(dirlen + 1, sizeof(wchar_t));
        size_t cap = 8;
        for (size_t i = 0; i < n; i++) cap += wcslen(w[i]) + 4;
        wchar_t *names = (wchar_t *)calloc(cap, sizeof(wchar_t));
        if (dir && names) {
            memcpy(dir, w[0], dirlen * sizeof(wchar_t));
            for (size_t i = 0; i < n; i++) {
                wcscat(names, L"\"");
                wcscat(names, w[i] + dirlen + 1);
                wcscat(names, L"\" ");
            }
            dialog_set_name_and_ok(dlg, dir);            /* go to the folder first */
            pump(300);
            if (IsWindow(dlg)) dialog_set_name_and_ok(dlg, names);
        }
        for (size_t i = 0; i < n; i++) free(w[i]);
        free(w); free(dir); free(names);
    }
    for (int i = 0; i < 60 && IsWindow(dlg); i++) pump(25);       /* the dialog closes when the file was accepted */
    if (IsWindow(dlg)) {
        PostMessageW(dlg, WM_COMMAND, MAKEWPARAM(2 /* IDCANCEL */, 0), 0);
        fb_set_err(err, errsz, "The file chooser did not accept the file (does it exist, and is the path a Windows path such as Z:\\...?)");
        return 1;
    }
    return 0;
}

/* Putting a file into <input type=file> by script: Gecko under Wine may allow it for the host (it is not a page script). */
static int set_input_value(FbwTab *t, int handle, const FbJson *paths, char *err, size_t errsz)
{
    IeView *v = ie_tab_view(t);
    IDispatch *win = v && v->br ? ie_top_window(v->br) : NULL;
    int rc = 1;
    if (!win) { fb_set_err(err, errsz, "The page is not ready"); return 1; }
    if (fbj_len(paths) != 1) { IDispatch_Release(win); fb_set_err(err, errsz, "This engine can fill a file field with one file only; use the upload button (mode: dialog) for several"); return 1; }
    VARIANT lg, num, el, val;
    VariantInit(&lg); VariantInit(&num); VariantInit(&el); VariantInit(&val);
    if (SUCCEEDED(ie_get(win, L"__fbLegacy", &lg)) && V_VT(&lg) == VT_DISPATCH && V_DISPATCH(&lg)) {
        V_VT(&num) = VT_I4;
        V_I4(&num) = handle;
        if (SUCCEEDED(ie_call(V_DISPATCH(&lg), L"handle", &num, 1, &el)) && V_VT(&el) == VT_DISPATCH && V_DISPATCH(&el)) {
            V_VT(&val) = VT_BSTR;
            V_BSTR(&val) = ie_bstr(fbj_at(paths, 0)->str);
            if (V_BSTR(&val) && SUCCEEDED(ie_put(V_DISPATCH(&el), L"value", &val))) {
                /* did it stick? */
                char js[120];
                FbBuf out;
                char e2[100];
                fb_buf_init(&out);
                snprintf(js, sizeof js, "window.__fbLegacy.fileInfo(%d)", handle);
                if (!ie_eval(win, js, &out, e2, sizeof e2)) {
                    FbJson *j = fbj_parse_str(fb_buf_cstr(&out), NULL, 0);
                    if (j && fbj_get_str(j, "value", "")[0]) rc = 0;
                    fbj_free(j);
                }
                fb_buf_free(&out);
            }
            SysFreeString(V_BSTR(&val));
        }
    }
    VariantClear(&el);
    VariantClear(&lg);
    IDispatch_Release(win);
    if (rc) fb_set_err(err, errsz, "This engine refused to fill the file field directly");
    return rc;
}

/* ------------------------------------------------------------------ calls from the runner (UI-thread halves) */
static void ui_eval(FbwCall *c)
{
    FbwTab *t = call_tab(c);
    IeView *v = ie_tab_view(t);
    FbBuf out;
    if (!v || !v->br) { fail(c, "The tab used by this flow was closed"); return; }
    if (g_dlg || ie_dialog_open()) { fail(c, "navigating"); return; }   /* a native dialog blocks the page's scripts: the runner retries */
    IDispatch *w = ie_top_window(v->br);
    fb_buf_init(&out);
    c->rc = ie_eval(w, c->s1, &out, c->err, sizeof c->err);
    if (w) IDispatch_Release(w);
    if (!c->rc) c->out = fb_buf_detach(&out); else fb_buf_free(&out);
}

static void ui_set_files(FbwCall *c)
{
    FbwTab *t = call_tab(c);
    FbJson *paths = fbj_parse_str(c->s1 ? c->s1 : "[]", NULL, 0);
    if (!paths || paths->type != FBJ_ARR) { fail(c, "No file was given"); fbj_free(paths); return; }
    if (c->i2 > 0) {
        if (set_input_value(t, c->i2, paths, c->err, sizeof c->err)) c->rc = 1;
    } else {
        if (!g_dlg || !IsWindow(g_dlg)) fail(c, "No file chooser is open");
        else {
            HWND d = g_dlg;
            if (fill_dialog(d, paths, c->err, sizeof c->err)) c->rc = 1;
            g_dlg = NULL;
        }
    }
    fbj_free(paths);
}

static void ui_chooser(FbwCall *c)
{
    FbwTab *t = call_tab(c);
    IeView *v = ie_tab_view(t);
    g_intercept = c->i2;
    if (!v || !v->br || g_dlg) return;
    /* while intercepting, clicks run from a timer so the call that asked for the click can return before a dialog opens */
    IDispatch *w = ie_top_window(v->br);
    if (w) {
        char err[100];
        ie_exec_script(w, g_intercept ? "if(window.__fbLegacy)window.__fbLegacy.setDefer(true);" : "if(window.__fbLegacy)window.__fbLegacy.setDefer(false);", err, sizeof err);
        IDispatch_Release(w);
    }
}

static HWND find_server(HWND parent)
{
    HWND found = FindWindowExW(parent, NULL, L"Internet Explorer_Server", NULL);
    if (found) return found;
    for (HWND ch = GetWindow(parent, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT)) {
        found = find_server(ch);
        if (found) return found;
    }
    return NULL;
}

static int looks_blank(const unsigned char *px, int w, int h)
{
    unsigned first = *(const unsigned *)px & 0xFFFFFFu;
    for (int i = 0; i < 64; i++) {
        int x = (i * 37) % w, y = (i * 53) % h;
        if ((*(const unsigned *)(px + ((size_t)y * w + x) * 4) & 0xFFFFFFu) != first) return 0;
    }
    return 1;
}

static void ui_shot(FbwCall *c)
{
    FbwTab *t = call_tab(c);
    IeView *v = ie_tab_view(t);
    if (!v || !v->host) { fail(c, "The tab used by this flow was closed"); return; }
    HWND target = find_server(v->host);
    if (!target) target = v->host;
    RECT rc;
    GetClientRect(target, &rc);
    int w = (int)rc.right, h = (int)rc.bottom;
    if (w <= 0 || h <= 0) { fail(c, "The page is not visible: bring its tab to the front before taking a screenshot"); return; }
    HDC sdc = GetDC(target);
    HDC mdc = sdc ? CreateCompatibleDC(sdc) : NULL;
    BITMAPINFO bi;
    void *bits = NULL;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;                     /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HBITMAP bmp = mdc ? CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        if (mdc) DeleteDC(mdc);
        if (sdc) ReleaseDC(target, sdc);
        fail(c, "A screenshot could not be taken (no bitmap)");
        return;
    }
    HGDIOBJ old = SelectObject(mdc, bmp);
    memset(bits, 0xFF, (size_t)w * h * 4);
    int ok = PrintWindow(target, mdc, 0);
    if (!ok || looks_blank((const unsigned char *)bits, w, h)) BitBlt(mdc, 0, 0, w, h, sdc, 0, 0, SRCCOPY);   /* copy what is on screen instead */
    FbBuf png, b64;
    fb_buf_init(&png);
    fb_buf_init(&b64);
    if (fbpng_encode_bgra((const unsigned char *)bits, w, h, w * 4, &png)) fail(c, "A screenshot could not be encoded");
    else { fb_base64_encode(png.data, png.len, &b64); c->out = fb_buf_detach(&b64); }
    fb_buf_free(&png);
    fb_buf_free(&b64);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(target, sdc);
}

/* ------------------------------------------------------------------ user agent, proxy, cookies and cache */
void ie_set_user_agent(const char *ua)
{
    if (ua && ua[0]) UrlMkSetSessionOption(URLMON_OPTION_USERAGENT, (LPVOID)ua, (DWORD)strlen(ua), 0);
    else UrlMkSetSessionOption(URLMON_OPTION_USERAGENT_REFRESH, NULL, 0, 0);
}

void fbw_apply_user_agent(FbwTab *t)
{
    (void)t;
    if (G.user_agent && G.user_agent[0]) ie_set_user_agent(G.user_agent);
}

void ie_apply_proxy_from_settings(void)
{
    char *txt = fbw_store_read("settings");
    if (!txt) return;
    FbJson *j = fbj_parse_str(txt, NULL, 0);
    free(txt);
    if (!j) return;
    const char *px = fbj_get_str(j, "proxy", "");
    int ok = px[0] && strlen(px) < 200;
    for (const char *p = px; ok && *p; p++) if (!(isalnum((unsigned char)*p) || strchr(".:/-_@[]", *p))) ok = 0;
    if (ok) {
        wchar_t *w = fbw_w(px);
        INTERNET_PROXY_INFOW pi;
        memset(&pi, 0, sizeof pi);
        pi.dwAccessType = INTERNET_OPEN_TYPE_PROXY;
        pi.lpszProxy = w;
        pi.lpszProxyBypass = L"<local>";
        if (w) InternetSetOptionW(NULL, INTERNET_OPTION_PROXY, &pi, sizeof pi);
        free(w);
        fbw_log("proxy: %s", px);
    }
    fbj_free(j);
}

static void purge_cache(int cookies, int files)
{
    DWORD size = 0;
    HANDLE h = FindFirstUrlCacheEntryW(NULL, NULL, &size);
    if (!h && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return;
    INTERNET_CACHE_ENTRY_INFOW *e = (INTERNET_CACHE_ENTRY_INFOW *)malloc(size ? size : 1);
    if (!e) return;
    h = FindFirstUrlCacheEntryW(NULL, e, &size);
    while (h) {
        if (e->lpszSourceUrlName) {
            int is_cookie = wcsncmp(e->lpszSourceUrlName, L"Cookie:", 7) == 0;
            if ((is_cookie && cookies) || (!is_cookie && files)) DeleteUrlCacheEntryW(e->lpszSourceUrlName);
        }
        DWORD need = size;
        if (!FindNextUrlCacheEntryW(h, e, &need)) {
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
            INTERNET_CACHE_ENTRY_INFOW *bigger = (INTERNET_CACHE_ENTRY_INFOW *)realloc(e, need);
            if (!bigger) break;
            e = bigger;
            size = need;
            if (!FindNextUrlCacheEntryW(h, e, &size)) break;
        }
    }
    if (h) FindCloseUrlCache(h);
    free(e);
}

static void clear_now(int cookies, int cache)
{
    if (cookies) InternetSetOptionW(NULL, INTERNET_OPTION_END_BROWSER_SESSION, NULL, 0);
    purge_cache(cookies, cache);
}

void fbw_clear_data(FbwTab *t, int cookies, int cache) { (void)t; clear_now(cookies, cache); }

/* ------------------------------------------------------------------ FbLegacyHost for the runner */
typedef struct { int tab_id; } LgCtx;      /* 0 = the tab the flow runs in */

static int lg_eval(void *ud, const char *js, FbBuf *out, char *err, size_t errsz)
{
    FbwCall *c = fbw_call_new(ui_eval);
    if (c) { c->i1 = ((LgCtx *)ud)->tab_id; c->s1 = fb_strdup(js); }
    return roundtrip(c, 120000, out, err, errsz);
}

static int lg_set_files(void *ud, int handle, double node, const FbJson *paths, char *err, size_t errsz)
{
    FbwCall *c = fbw_call_new(ui_set_files);
    (void)node;
    if (c) { c->i1 = ((LgCtx *)ud)->tab_id; c->i2 = handle; c->s1 = fbj_to_string(paths, 0); }
    return roundtrip(c, 60000, NULL, err, errsz);
}

static int lg_chooser(void *ud, int on, char *err, size_t errsz)
{
    FbwCall *c = fbw_call_new(ui_chooser);
    if (c) { c->i1 = ((LgCtx *)ud)->tab_id; c->i2 = on ? 1 : 0; }
    return roundtrip(c, 20000, NULL, err, errsz);
}

static int lg_screenshot(void *ud, const FbJson *params, FbBuf *b64, char *err, size_t errsz)
{
    FbwCall *c = fbw_call_new(ui_shot);
    (void)params;                                   /* the visible page only: no clip, no full-page capture, always PNG */
    if (c) c->i1 = ((LgCtx *)ud)->tab_id;
    return roundtrip(c, 30000, b64, err, errsz);
}

static void ui_clear(FbwCall *c) { clear_now(c->i2 & 1, c->i2 & 2); }
static int lg_clear(void *ud, int cookies, int cache, char *err, size_t errsz)
{
    FbwCall *c = fbw_call_new(ui_clear);
    (void)ud;
    if (c) c->i2 = (cookies ? 1 : 0) | (cache ? 2 : 0);
    return roundtrip(c, 30000, NULL, err, errsz);
}

static void ui_ua(FbwCall *c) { ie_set_user_agent(c->s1); }
static int lg_ua(void *ud, const char *ua, char *err, size_t errsz)
{
    FbwCall *c = fbw_call_new(ui_ua);
    (void)ud;
    if (c) c->s1 = fb_strdup(ua ? ua : "");
    return roundtrip(c, 10000, NULL, err, errsz);
}

static int lg_stopped(void *ud) { (void)ud; return G.stop_evt && WaitForSingleObject(G.stop_evt, 0) == WAIT_OBJECT_0; }

static void make_host(FbLegacyHost *h, LgCtx *ctx)
{
    memset(h, 0, sizeof *h);
    h->ud = ctx;
    h->eval = lg_eval; h->set_files = lg_set_files; h->chooser = lg_chooser; h->screenshot = lg_screenshot;
    h->clear_data = lg_clear; h->user_agent = lg_ua; h->stopped = lg_stopped;
}

/* engine thread (the runner) */
int fbie_cdp(const char *method, const char *params, FbBuf *res, char *err, size_t errsz)
{
    LgCtx ctx = { 0 };
    FbLegacyHost h;
    make_host(&h, &ctx);
    return fbl_cdp(&h, method, params && params[0] ? params : "{}", res, err, errsz);
}

/* ------------------------------------------------------------------ fbw_cdp / fbw_page_eval for the UI thread's own needs */
typedef struct {
    int           tab_id;
    char         *method, *params, *result;
    int           rc;
    FbwHandlerFn  done;
    void         *ctx;
} Job;

static void job_finish(FbwCall *c)
{
    Job *j = (Job *)c->p1;
    wchar_t *w = j->rc == 0 && j->result ? fbw_w(j->result) : NULL;
    if (j->done) j->done(j->ctx, (void *)(INT_PTR)(j->rc ? E_FAIL : S_OK), w);
    free(w);
    free(j->method); free(j->params); free(j->result);
    free(j);
}

static DWORD WINAPI job_main(void *p)
{
    Job *j = (Job *)p;
    LgCtx ctx;
    FbLegacyHost h;
    FbBuf res;
    char err[256] = "";
    ctx.tab_id = j->tab_id;
    make_host(&h, &ctx);
    fb_buf_init(&res);
    j->rc = fbl_cdp(&h, j->method, j->params, &res, err, sizeof err) ? 1 : 0;
    if (!j->rc) j->result = fb_buf_detach(&res); else fb_buf_free(&res);
    FbwCall *fin = fbw_call_new(job_finish);
    if (fin) {
        fin->p1 = j;
        if (!G.hwnd || !PostMessageW(G.hwnd, WM_FBW_CALL, 0, (LPARAM)fin)) { fbw_call_release(fin); fbw_call_release(fin); }   /* closing: drop both references */
        else fbw_call_release(fin);                                                                                        /* ours; the UI thread drops the other */
    }
    return 0;
}

void fbw_cdp(FbwTab *t, const char *method, const char *params, FbwHandlerFn done, void *ctx)
{
    if (!t || !ie_tab_view(t)) { if (done) done(ctx, (void *)(INT_PTR)E_FAIL, NULL); return; }
    const char *p = params && params[0] ? params : "{}";
    if (strcmp(method, "Runtime.evaluate") != 0) {
        /* everything but evaluate is quick and does not wait for the page: answer right here */
        LgCtx lc;
        FbLegacyHost h;
        FbBuf res;
        char err[256] = "";
        lc.tab_id = t->id;
        make_host(&h, &lc);
        fb_buf_init(&res);
        int rc = fbl_cdp(&h, method, p, &res, err, sizeof err);
        wchar_t *w = rc == 0 ? fbw_w(fb_buf_cstr(&res)) : NULL;
        if (done) done(ctx, (void *)(INT_PTR)(rc ? E_FAIL : S_OK), w);
        free(w);
        fb_buf_free(&res);
        return;
    }
    /* evaluate may wait for a promise: do it on a worker thread so the window keeps pumping messages */
    Job *j = (Job *)calloc(1, sizeof *j);
    if (!j) { if (done) done(ctx, (void *)(INT_PTR)E_OUTOFMEMORY, NULL); return; }
    j->tab_id = t->id;
    j->method = fb_strdup(method);
    j->params = fb_strdup(p);
    j->done = done;
    j->ctx = ctx;
    if (!QueueUserWorkItem(job_main, j, 0)) {
        free(j->method); free(j->params); free(j);
        if (done) done(ctx, (void *)(INT_PTR)E_FAIL, NULL);
    }
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

void fbw_run_script(FbwTab *t, const char *js, FbwHandlerFn done, void *ctx)
{
    IeView *v = ie_tab_view(t);
    IDispatch *w = v && v->br ? ie_top_window(v->br) : NULL;
    char err[100];
    int rc = w ? ie_exec_script(w, js, err, sizeof err) : 1;
    if (w) IDispatch_Release(w);
    if (done) done(ctx, (void *)(INT_PTR)(rc ? E_FAIL : S_OK), rc ? NULL : (void *)L"null");
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
