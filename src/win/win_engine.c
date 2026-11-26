/* FlowBrowser for Windows - the automation engine host.
   The portable runner (src/core) runs on its own thread and talks to the browser through FbBackend. Every call that
   touches a WebView2 object is handed to the UI thread with fbw_ui_call(); the engine thread then waits for the
   answer (or for the stop event, so "Stop" always works). */
#include "win_app.h"
#include <stdarg.h>

/* ------------------------------------------------------------------ shared state */
static CRITICAL_SECTION g_cs;
static int              g_cs_ready;
static struct { int started, completed, chooser; char *chooser_params; } g_ev;
static volatile LONG    g_stop, g_pause_req, g_step_mode;
static HANDLE           g_resume_evt, g_ask_evt;
static struct { int id, next_id, waiting, status; char *answer; } g_ask;
static FbJson          *g_bps;                      /* breakpoint paths of the current run */

typedef struct { FbRunner *r; FbJson *flow; } RunCtx;

static void engine_init(void)
{
    if (g_cs_ready) return;
    InitializeCriticalSection(&g_cs);
    g_resume_evt = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_ask_evt = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_cs_ready = 1;
}

static void post_panel(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fbw_post_async(FBW_PANEL, fb_strdup(buf));
}

static void post_state(const char *state, const char *path)
{
    if (path) {
        FbJson *o = fbj_obj();
        fbj_obj_set(o, "ev", fbj_str("run.state"));
        fbj_obj_set(o, "state", fbj_str(state));
        fbj_obj_set(o, "path", fbj_str(path));
        char *s = fbj_to_string(o, 0);
        fbj_free(o);
        fbw_post_async(FBW_PANEL, s);
    } else post_panel("{\"ev\":\"run.state\",\"state\":\"%s\"}", state);
}

/* ------------------------------------------------------------------ events from the UI thread */
void fbw_engine_nav_event(FbwTab *t, const char *name, const char *params)
{
    if (!g_cs_ready || t != G.run_tab) return;
    EnterCriticalSection(&g_cs);
    if (!strcmp(name, "Navigation.started")) g_ev.started = 1;
    else if (!strcmp(name, "Navigation.completed")) g_ev.completed = 1;
    else if (!strcmp(name, "Page.fileChooserOpened")) { g_ev.chooser = 1; free(g_ev.chooser_params); g_ev.chooser_params = fb_strdup(params ? params : "{}"); }
    LeaveCriticalSection(&g_cs);
}

void fbw_engine_tab_closed(FbwTab *t) { if (t == G.run_tab) G.run_tab = NULL; }

static void events_clear(void)
{
    EnterCriticalSection(&g_cs);
    g_ev.started = g_ev.completed = g_ev.chooser = 0;
    LeaveCriticalSection(&g_cs);
}

void fbw_engine_answer(int id, int status, const char *answer)
{
    if (!g_cs_ready) return;
    EnterCriticalSection(&g_cs);
    if (g_ask.waiting && id == g_ask.id) {
        g_ask.status = status;
        free(g_ask.answer);
        g_ask.answer = fb_strdup(answer ? answer : "");
        g_ask.waiting = 0;
        SetEvent(g_ask_evt);
    }
    LeaveCriticalSection(&g_cs);
}

void fbw_engine_stop(void)
{
    if (!g_cs_ready || !G.run_active) return;
    InterlockedExchange(&g_stop, 1);
    SetEvent(G.stop_evt);
    if (G.runner) fbr_stop(G.runner);
    post_state("stopping", NULL);
}

void fbw_engine_pause(void) { if (G.run_active) InterlockedExchange(&g_pause_req, 1); }

void fbw_engine_resume(int step_only)
{
    if (!g_cs_ready) return;
    InterlockedExchange(&g_step_mode, step_only ? 1 : 0);
    SetEvent(g_resume_evt);
}

/* ------------------------------------------------------------------ backend: DevTools protocol */
static void cdp_done(void *ctx, void *hr, void *json)
{
    FbwCall *c = (FbwCall *)ctx;
    HRESULT h = (HRESULT)(INT_PTR)hr;
    if (FAILED(h)) { c->rc = 1; snprintf(c->err, sizeof c->err, "The browser rejected the command (0x%08lX)", (unsigned long)h); }
    else c->out = json ? fbw_u8((const wchar_t *)json) : fb_strdup("{}");
    fbw_call_finish(c);
}

static void ui_cdp(FbwCall *c)
{
    FbwTab *t = G.run_tab;
    if (!t || !t->wv) { c->rc = 1; snprintf(c->err, sizeof c->err, "The tab used by this flow was closed"); fbw_call_finish(c); return; }
    fbw_cdp(t, c->s1, c->s2, cdp_done, c);
}

static int be_cdp(void *ud, const char *method, const char *params, FbBuf *res, char *err, size_t errsz)
{
    (void)ud;
    FbwCall *c = fbw_call_new(ui_cdp);
    if (!c) { fb_set_err(err, errsz, "Out of memory"); return -1; }
    c->s1 = fb_strdup(method);
    c->s2 = fb_strdup(params ? params : "{}");
    c->async = 1;
    int w = fbw_ui_call(c, 120000), rc = -1;
    if (w == 0) {
        if (c->rc == 0) { if (res) fb_buf_puts(res, c->out ? c->out : "{}"); rc = 0; }
        else fb_set_err(err, errsz, "%s", c->err);
    } else if (w == 2) fb_set_err(err, errsz, "Stopped");
    else fb_set_err(err, errsz, "The page did not answer in time");
    fbw_call_release(c);
    return rc;
}

/* ------------------------------------------------------------------ backend: events */
static int be_wait_event(void *ud, const char *name, int timeout_ms, FbBuf *params)
{
    (void)ud;
    uint64_t end = fb_now_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        int got = 0;
        EnterCriticalSection(&g_cs);
        int *flag = !strcmp(name, "Navigation.started") ? &g_ev.started : !strcmp(name, "Navigation.completed") ? &g_ev.completed
                  : !strcmp(name, "Page.fileChooserOpened") ? &g_ev.chooser : NULL;
        if (flag && *flag) {
            *flag = 0;
            got = 1;
            if (params && flag == &g_ev.chooser && g_ev.chooser_params) fb_buf_puts(params, g_ev.chooser_params);
        }
        LeaveCriticalSection(&g_cs);
        if (got) return 0;
        if (!flag || timeout_ms <= 0 || fb_now_ms() >= end) return 1;
        if (WaitForSingleObject(G.stop_evt, 8) == WAIT_OBJECT_0) return 1;
    }
}

static const char *nav_error_text(int st)
{
    switch (st) {
    case 1: case 2: case 3: case 4: case 5: return "the security certificate of the site is not valid";
    case 6: case 12: return "the server could not be reached";
    case 7: return "the server did not answer in time";
    case 9: case 10: case 11: return "the connection was lost";
    case 13: return "the address could not be found";
    case 14: return "the navigation was cancelled";
    default: return "the page could not be loaded";
    }
}

static void ui_navigate(FbwCall *c)
{
    FbwTab *t = G.run_tab;
    if (!t || !t->wv) { c->rc = 1; snprintf(c->err, sizeof c->err, "The tab used by this flow was closed"); return; }
    wchar_t *w = fbw_w(c->s1);
    t->nav_ok = 1;
    if (!w || FAILED(ICoreWebView2_Navigate(t->wv, w))) { c->rc = 1; snprintf(c->err, sizeof c->err, "'%.100s' is not a valid web address", c->s1); }
    free(w);
}

static int be_navigate(void *ud, const char *url, int wait, int timeout_ms, char *err, size_t errsz)
{
    (void)ud;
    while (*url == ' ') url++;
    if (_strnicmp(url, "javascript:", 11) == 0) { fb_set_err(err, errsz, "javascript: addresses are not allowed"); return -1; }
    events_clear();
    FbwCall *c = fbw_call_new(ui_navigate);
    if (!c) return -1;
    c->s1 = fb_strdup(url);
    int w = fbw_ui_call(c, 20000), rc = -1;
    if (w != 0) fb_set_err(err, errsz, w == 2 ? "Stopped" : "The browser did not accept the address");
    else if (c->rc) fb_set_err(err, errsz, "%s", c->err);
    else rc = 0;
    fbw_call_release(c);
    if (rc || !wait) return rc;
    int rr = be_wait_event(NULL, "Navigation.completed", timeout_ms > 0 ? timeout_ms : 30000, NULL);
    if (rr) { fb_set_err(err, errsz, "The page did not finish loading in time"); return -1; }
    FbwTab *t = G.run_tab;
    if (t && !t->nav_ok) { fb_set_err(err, errsz, "%s (%s)", "The page could not be opened", nav_error_text(t->nav_err)); return -1; }
    return 0;
}

/* ------------------------------------------------------------------ backend: tabs */
static void ui_tab(FbwCall *c)
{
    const char *op = c->s1, *arg = c->s2 ? c->s2 : "";
    FbwTab *t = G.run_tab;
    if (!strcmp(op, "new")) {
        int id = fbw_tab_new(arg[0] ? arg : NULL, 1);
        FbwTab *nt = id ? fbw_tab_by_id(id) : NULL;
        if (!nt) { c->rc = 1; snprintf(c->err, sizeof c->err, "A new tab could not be opened"); fbw_call_finish(c); return; }
        G.run_tab = nt;
        nt->pending = c;                    /* finished when the view exists */
        return;
    }
    if (!strcmp(op, "switch")) {
        FbwTab *found = NULL;
        char *low = fb_strdup(arg);
        for (char *p = low; p && *p; p++) *p = (char)tolower((unsigned char)*p);
        int digits = arg[0] != 0;
        for (const char *p = arg; *p; p++) if (!isdigit((unsigned char)*p)) digits = 0;
        if (digits) { int n = atoi(arg); if (n >= 1 && n <= G.ntabs) found = G.tabs[n - 1]; }
        else for (int i = 0; i < G.ntabs && !found; i++) {
            char *ti = fb_strdup(G.tabs[i]->title ? G.tabs[i]->title : ""), *ur = fb_strdup(G.tabs[i]->url ? G.tabs[i]->url : "");
            for (char *p = ti; p && *p; p++) *p = (char)tolower((unsigned char)*p);
            for (char *p = ur; p && *p; p++) *p = (char)tolower((unsigned char)*p);
            if (low && low[0] && (strstr(ti, low) || strstr(ur, low))) found = G.tabs[i];
            free(ti); free(ur);
        }
        free(low);
        if (!found) { c->rc = 1; snprintf(c->err, sizeof c->err, "No tab matches \"%.80s\"", arg); return; }
        G.run_tab = found;
        fbw_tab_activate(found->id);
        return;
    }
    if (!t || !t->wv) { c->rc = 1; snprintf(c->err, sizeof c->err, "The tab used by this flow was closed"); return; }
    if (!strcmp(op, "close")) {
        fbw_tab_close(t->id);
        G.run_tab = fbw_active_tab();
        return;
    }
    if (!strcmp(op, "back")) { BOOL b = FALSE; ICoreWebView2_get_CanGoBack(t->wv, &b); if (!b) { c->rc = 1; snprintf(c->err, sizeof c->err, "There is no earlier page to go back to"); return; } ICoreWebView2_GoBack(t->wv); return; }
    if (!strcmp(op, "forward")) { BOOL b = FALSE; ICoreWebView2_get_CanGoForward(t->wv, &b); if (!b) { c->rc = 1; snprintf(c->err, sizeof c->err, "There is no next page to go forward to"); return; } ICoreWebView2_GoForward(t->wv); return; }
    if (!strcmp(op, "reload")) { ICoreWebView2_Reload(t->wv); return; }
    c->rc = 1;
    snprintf(c->err, sizeof c->err, "Unknown tab command");
}

static int be_tab(void *ud, const char *op, const char *arg, char *err, size_t errsz)
{
    (void)ud;
    int navigates = !strcmp(op, "back") || !strcmp(op, "forward") || !strcmp(op, "reload") || (!strcmp(op, "new") && arg && arg[0]);
    events_clear();
    FbwCall *c = fbw_call_new(ui_tab);
    if (!c) return -1;
    c->s1 = fb_strdup(op);
    c->s2 = fb_strdup(arg ? arg : "");
    c->async = !strcmp(op, "new");
    int w = fbw_ui_call(c, 30000), rc = -1;
    if (w != 0) fb_set_err(err, errsz, w == 2 ? "Stopped" : "The browser did not answer");
    else if (c->rc) fb_set_err(err, errsz, "%s", c->err);
    else rc = 0;
    fbw_call_release(c);
    if (rc == 0 && navigates) be_wait_event(NULL, "Navigation.completed", 30000, NULL);
    return rc;
}

/* ------------------------------------------------------------------ backend: asking the person */
static int be_ask(void *ud, const char *kind, const char *text, const char *def, int timeout_ms, FbBuf *answer)
{
    (void)ud;
    EnterCriticalSection(&g_cs);
    g_ask.id = ++g_ask.next_id;
    g_ask.waiting = 1;
    g_ask.status = 2;
    free(g_ask.answer);
    g_ask.answer = NULL;
    ResetEvent(g_ask_evt);
    int id = g_ask.id;
    LeaveCriticalSection(&g_cs);

    FbJson *o = fbj_obj();
    fbj_obj_set(o, "ev", fbj_str("ask"));
    fbj_obj_set(o, "id", fbj_num(id));
    fbj_obj_set(o, "kind", fbj_str(kind));
    fbj_obj_set(o, "text", fbj_str(text));
    fbj_obj_set(o, "def", fbj_str(def ? def : ""));
    fbw_post_async(FBW_SHELL, fbj_to_string(o, 0));
    fbj_free(o);
    fbw_flash();
    MessageBeep(MB_ICONEXCLAMATION);

    HANDLE h[2] = { g_ask_evt, G.stop_evt };
    DWORD w = WaitForMultipleObjects(2, h, FALSE, timeout_ms > 0 ? (DWORD)timeout_ms : INFINITE);
    int rc;
    EnterCriticalSection(&g_cs);
    if (w == WAIT_OBJECT_0) { rc = g_ask.status; if (answer && g_ask.answer) fb_buf_puts(answer, g_ask.answer); }
    else rc = w == WAIT_TIMEOUT ? 1 : 2;
    g_ask.waiting = 0;
    LeaveCriticalSection(&g_cs);
    if (w != WAIT_OBJECT_0) fbw_post_async(FBW_SHELL, fb_strdup("{\"ev\":\"ask.close\"}"));
    return rc;
}

/* ------------------------------------------------------------------ backend: options, events, checkpoint */
static void ui_option(FbwCall *c)
{
    if (!strcmp(c->s1, "dialogs")) {
        snprintf(G.dialogs, sizeof G.dialogs, "%s", !strcmp(c->s2, "accept") ? "accept" : !strcmp(c->s2, "dismiss") ? "dismiss" : "manual");
        fbw_apply_dialog_mode();
    } else if (!strcmp(c->s1, "user_agent") && c->s2[0]) {
        free(G.user_agent);
        G.user_agent = fb_strdup(c->s2);
        fbw_apply_user_agent(G.run_tab);
    }
}

static int be_option(void *ud, const char *name, const char *value)
{
    (void)ud;
    FbwCall *c = fbw_call_new(ui_option);
    if (!c) return -1;
    c->s1 = fb_strdup(name);
    c->s2 = fb_strdup(value ? value : "");
    fbw_ui_call(c, 10000);
    fbw_call_release(c);
    return 0;
}

static int event_is_critical(const char *ev)
{
    return strstr(ev, "\"flow_end\"") || strstr(ev, "\"flow_start\"") || strstr(ev, "\"step_error\"") || strstr(ev, "\"captcha\"") || strstr(ev, "\"vars\"");
}

static void be_emit(void *ud, const char *ev)
{
    (void)ud;
    /* a loop of thousands of steps must not drown the window's message queue: keep errors and the end, drop chatter */
    if (fbw_pending_posts > 1500 && !event_is_critical(ev)) return;
    fbw_post_async(FBW_PANEL, fbw_dupf("{\"ev\":\"run.event\",\"e\":%s}", ev));
}

static int be_checkpoint(void *ud, const char *path)
{
    (void)ud;
    if (g_stop) return 1;
    int hit = 0;
    for (size_t i = 0; g_bps && i < fbj_len(g_bps); i++) {
        const FbJson *b = fbj_at(g_bps, i);
        if (fbj_is(b, FBJ_STR) && !strcmp(b->str, path)) { hit = 1; break; }
    }
    if (!g_step_mode && !g_pause_req && !hit) return 0;
    InterlockedExchange(&g_pause_req, 0);
    ResetEvent(g_resume_evt);
    post_state("paused", path);
    HANDLE h[2] = { g_resume_evt, G.stop_evt };
    DWORD w = WaitForMultipleObjects(2, h, FALSE, INFINITE);
    if (w != WAIT_OBJECT_0 || g_stop) return 1;
    post_state("running", NULL);
    return 0;
}

/* ------------------------------------------------------------------ running a flow */
static void ui_run_finished(FbwCall *c)
{
    (void)c;
    snprintf(G.dialogs, sizeof G.dialogs, "manual");         /* the flow's dialog policy only applies while it runs */
    fbw_apply_dialog_mode();
}

static DWORD WINAPI run_main(void *p)
{
    RunCtx *c = (RunCtx *)p;
    char err[512] = "";
    fbr_run(c->r, c->flow, err, sizeof err);
    fbj_free(c->flow);
    free(c);
    FbwCall *fin = fbw_call_new(ui_run_finished);
    if (fin) { ResetEvent(G.stop_evt); fbw_ui_call(fin, 5000); fbw_call_release(fin); }
    InterlockedExchange(&G.run_active, 0);
    post_state("idle", NULL);
    return 0;
}

int fbw_engine_start(FbJson *flow, int step_mode, const FbJson *bps, const FbJson *vars, char *err, size_t errsz)
{
    engine_init();
    if (InterlockedCompareExchange(&G.run_active, 1, 0) != 0) { fb_set_err(err, errsz, "A flow is already running. Stop it first."); fbj_free(flow); return 1; }
    FbwTab *t = fbw_active_tab();
    if (!t || !t->wv) { InterlockedExchange(&G.run_active, 0); fb_set_err(err, errsz, "Open a tab first: the flow runs in the tab you are looking at."); fbj_free(flow); return 1; }
    fbf_migrate(flow);

    G.run_tab = t;
    events_clear();
    InterlockedExchange(&g_stop, 0);
    InterlockedExchange(&g_pause_req, 0);
    InterlockedExchange(&g_step_mode, step_mode ? 1 : 0);
    ResetEvent(G.stop_evt);
    ResetEvent(g_resume_evt);
    fbj_free(g_bps);
    g_bps = bps && bps->type == FBJ_ARR ? fbj_clone(bps) : NULL;

    if (G.runner) { fbr_free(G.runner); G.runner = NULL; }
    FbBackend be;
    memset(&be, 0, sizeof be);
    be.cdp = be_cdp; be.navigate = be_navigate; be.wait_event = be_wait_event; be.tab = be_tab;
    be.ask = be_ask; be.option = be_option; be.emit = be_emit; be.checkpoint = be_checkpoint;
    G.runner = fbr_new(&be);
    if (!G.runner) { InterlockedExchange(&G.run_active, 0); fb_set_err(err, errsz, "Out of memory"); fbj_free(flow); return 1; }
    char *base = fbw_dupf("%s\\data", G.data_dir);
    if (base) { fb_mkdirs(base); fbr_set_dirs(G.runner, base, G.flows_dir); free(base); }
    for (size_t i = 0; vars && vars->type == FBJ_OBJ && i < vars->count; i++) fbr_set_var(G.runner, vars->keys[i], fbj_clone(vars->items[i]));

    RunCtx *c = (RunCtx *)calloc(1, sizeof *c);
    if (!c) { InterlockedExchange(&G.run_active, 0); fb_set_err(err, errsz, "Out of memory"); fbj_free(flow); return 1; }
    c->r = G.runner;
    c->flow = flow;
    if (G.run_thread) { CloseHandle(G.run_thread); G.run_thread = NULL; }
    G.run_thread = CreateThread(NULL, 0, run_main, c, 0, NULL);
    if (!G.run_thread) { InterlockedExchange(&G.run_active, 0); fb_set_err(err, errsz, "The run could not be started"); fbj_free(flow); free(c); return 1; }
    post_state("running", NULL);
    return 0;
}

/* ------------------------------------------------------------------ saving a result variable (Variables tab) */
static void noop_emit(void *ud, const char *ev) { (void)ud; (void)ev; }

int fbw_engine_export(const FbJson *req, char *err, size_t errsz)
{
    if (G.run_active) { fb_set_err(err, errsz, "Wait until the flow has finished."); return 1; }
    if (!G.runner) { fb_set_err(err, errsz, "Run a flow first; its results are kept here until the next run."); return 1; }
    const char *var = fbj_get_str(req, "var", ""), *file = fbj_get_str(req, "file", ""), *format = fbj_get_str(req, "format", "xlsx");
    if (!var[0] || !file[0]) { fb_set_err(err, errsz, "Choose a variable and a file."); return 1; }
    const FbJson *value = fbj_get(fbr_vars(G.runner), var);
    if (!value) { fb_set_err(err, errsz, "The variable \"%s\" does not exist in the last run.", var); return 1; }

    FbJson *flow = fbf_new("export");
    FbJson *step = fbj_obj();
    if (!strcmp(format, "csv")) {
        fbj_obj_set(step, "type", fbj_str("csv_write"));
        fbj_obj_set(step, "headers", fbj_bool(fbj_get_bool(req, "headers", 1)));
    } else if (!strcmp(format, "json")) {
        fbj_obj_set(step, "type", fbj_str("json_write"));
    } else {
        fbj_obj_set(step, "type", fbj_str("excel_write_table"));
        fbj_obj_set(step, "sheet", fbj_str(fbj_get_str(req, "sheet", "")));
        fbj_obj_set(step, "start_cell", fbj_str(fbj_get_str(req, "start_cell", "A1")));
        fbj_obj_set(step, "headers", fbj_bool(fbj_get_bool(req, "headers", 1)));
        fbj_obj_set(step, "mode", fbj_str(!strcmp(fbj_get_str(req, "mode", "overwrite"), "append") ? "append" : "overwrite"));
    }
    fbj_obj_set(step, "file", fbj_str(file));
    fbj_obj_set(step, "source", fbj_str("data"));
    FbJson *steps = fbj_get(flow, "steps");
    if (!steps) { steps = fbj_arr(); fbj_obj_set(flow, "steps", steps); steps = fbj_get(flow, "steps"); }
    fbj_arr_push(steps, step);

    FbBackend be;
    memset(&be, 0, sizeof be);
    be.emit = noop_emit;
    FbRunner *r = fbr_new(&be);
    if (!r) { fbj_free(flow); fb_set_err(err, errsz, "Out of memory"); return 1; }
    char *base = fbw_dupf("%s\\data", G.data_dir);
    if (base) { fb_mkdirs(base); fbr_set_dirs(r, base, G.flows_dir); free(base); }
    fbr_set_var(r, "data", fbj_clone(value));
    int rc = fbr_run(r, flow, err, errsz);
    fbr_free(r);
    fbj_free(flow);
    return rc == 0 ? 0 : 1;
}
