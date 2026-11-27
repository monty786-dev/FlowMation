/* FlowBrowser for Windows - shared declarations.
   The host is plain C: WebView2 is used through its C-style COM interface (COBJMACROS).
   Threads: the UI thread owns every WebView2 object. The automation engine runs on its own thread and reaches the
   browser only through fbw_ui_call() (see win_engine.c). */
#ifndef FBW_APP_H
#define FBW_APP_H

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#  define UNICODE
#endif
#ifndef _UNICODE
#  define _UNICODE
#endif
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <ole2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FBW_IE
#  include "WebView2.h"            /* the Windows build: Microsoft Edge WebView2 */
#endif                             /* FBW_IE = the Wine / Winlator build: MSHTML (src/ie/), no WebView2 */

#include "fb_json.h"
#include "fb_runner.h"
#include "fb_util.h"

#define FBW_VERSION "1.0.0"
#define FBW_ORIGIN_W L"https://appassets.example/"
#define FBW_ORIGIN    "https://appassets.example/"
#define FBW_NEWTAB_URL "https://appassets.example/newtab.html"   /* the Wine build maps this address to the new-tab page file */
#define FBW_MAX_TABS 64

/* messages posted to the main window */
#define WM_FBW_CALL   (WM_APP + 1)   /* lParam = FbwCall*   : run fn on the UI thread */
#define WM_FBW_POST   (WM_APP + 2)   /* wParam = view (FBW_SHELL / FBW_PANEL), lParam = malloc'ed UTF-8 JSON */
#define WM_FBW_ENV    (WM_APP + 3)   /* environment created */
#define WM_FBW_EXIT   (WM_APP + 4)   /* close the window without asking */

enum { FBW_SHELL = 0, FBW_PANEL = 1 };

/* ------------------------------------------------------------------ views */
#ifdef FBW_IE
typedef HWND  FbwCtl;                /* the window that hosts the browser control */
typedef void *FbwWv;                 /* IWebBrowser2 * (src/ie/) */
#else
typedef ICoreWebView2Controller *FbwCtl;
typedef ICoreWebView2           *FbwWv;
#endif

typedef struct {
    FbwCtl                   ctl;
    FbwWv                    wv;
    int                      ready;
    int                      kind;       /* FBW_SHELL / FBW_PANEL */
    void                    *eng;        /* engine private data (Wine build) */
} FbwView;

typedef struct FbwTab {
    int                      id;
    FbwCtl                   ctl;
    FbwWv                    wv;
    void                    *eng;        /* engine private data (Wine build) */
    int                      ready;      /* controller + web view exist */
    char                    *title;
    char                    *url;
    int                      loading, can_back, can_forward;
    double                   zoom;
    int                      nav_ok;     /* result of the last navigation */
    char                    *start_url;  /* navigated to as soon as the view is ready */
    int                      nav_err;    /* COREWEBVIEW2_WEB_ERROR_STATUS of the last navigation */
    struct FbwCall          *pending;    /* UI-thread call to finish when the view is ready (tab creation from the engine) */
} FbwTab;

/* ------------------------------------------------------------------ cross-thread call into the UI thread */
typedef struct FbwCall {
    void  (*fn)(struct FbwCall *c);
    HANDLE done;                /* signalled when fn (and anything it started) has finished */
    int    rc;                  /* 0 = ok */
    int    i1, i2;
    char  *s1, *s2;             /* owned by the caller */
    char  *out;                 /* malloc'ed result (caller frees) */
    char   err[256];
    void  *p1;
    int    async;               /* fn calls fbw_call_finish() itself when its asynchronous work completes */
    volatile LONG refs;         /* caller + UI thread; the last one frees */
} FbwCall;

/* ------------------------------------------------------------------ run-time state (single instance) */
typedef struct {
    HINSTANCE  inst;
    HWND       hwnd, float_hwnd;
    DWORD      ui_thread;
#ifndef FBW_IE
    ICoreWebView2Environment *env;
#endif
    int        engine_ready;    /* views can be created */
    FbwView    shell, panel;
    FbwTab    *tabs[FBW_MAX_TABS];
    int        ntabs, active_id, next_tab_id;
    int        dpi;
    /* layout in CSS pixels, as reported by the shell */
    RECT       content_css, panel_css;
    int        have_layout, panel_docked, panel_visible, panel_float, overlay;
    int        fullscreen;
    RECT       saved_rect; int saved_style; int saved_maximized;
    char       token[48];       /* secret for messages coming from page views */
    char      *tools_js;        /* fb_runtime.js + fb_tools.js with the token filled in */
    /* folders (UTF-8) */
    char       exe_dir[MAX_PATH * 3], data_dir[MAX_PATH * 3], flows_dir[MAX_PATH * 3], store_dir[MAX_PATH * 3], backup_dir[MAX_PATH * 3], profile_dir[MAX_PATH * 3];
    int        portable;
    /* settings the host acts on */
    char      *user_agent;
    int        popups_block, perm_deny, clear_on_exit, keep_versions, auto_backup;
    double     default_zoom;
    char       dialogs[16];     /* accept | dismiss | manual */
    int        recording;       /* page tools: recorder active */
    int        picking;         /* page tools: picker active */
    char       pick_mode[16];
    /* command line */
    char      *cli_run, *cli_open, *cli_url;
    int        cli_exit;
    int        closing, exit_ok, exit_code;
    char      *home_url;        /* from the settings; the new-tab page when empty */
    HANDLE     stop_evt;        /* manual reset; set while a run is being stopped */
    /* automation engine (win_engine.c) */
    HANDLE     run_thread;
    volatile LONG run_active;
    FbRunner  *runner;          /* the last run's engine (kept for data.export) */
    FbwTab    *run_tab;
} FbwApp;

extern FbwApp G;
extern volatile LONG fbw_pending_posts;      /* messages posted to the UI thread and not yet delivered (win_main.c) */

/* ------------------------------------------------------------------ win_util.c */
wchar_t *fbw_w(const char *utf8);                       /* malloc'ed */
char    *fbw_u8(const wchar_t *w);                      /* malloc'ed */
char    *fbw_dupf(const char *fmt, ...);
typedef void (*FbwHandlerFn)(void *ctx, void *a, void *b);
void    *fbw_handler(FbwHandlerFn fn, void *ctx);       /* COM callback object for any ICoreWebView2*Handler */
const FbJson *fbw_json(const FbJson *o, const char *k); /* shorthand for fbj_get */
void     fbw_random_token(char *out, size_t n);
const unsigned char *fbw_resource(const char *name, size_t *size, const char **mime);
void     fbw_paths_init(void);
void     fbw_rmtree(const char *dir_utf8);
int      fbw_run_capture(const wchar_t *cmdline, FbBuf *out, DWORD timeout_ms);
int      fbw_open_dialog(HWND owner, const char *title, const FbJson *filters, int multi, FbJson **paths);   /* 0 ok, 1 cancelled */
int      fbw_save_dialog(HWND owner, const char *title, const FbJson *filters, const char *name, char **path);
void     fbw_open_folder(const char *dir_utf8);
void     fbw_log(const char *fmt, ...);
char    *fbw_timestamp(const char *fmt);                /* strftime in local time, malloc'ed */

/* ------------------------------------------------------------------ win_main.c */
void     fbw_post(int view, const char *json);          /* UI thread only */
void     fbw_post_async(int view, char *json_malloced); /* any thread; takes ownership */
void     fbw_broadcast(const char *json);               /* UI thread: shell + panel */
FbwTab  *fbw_tab_by_id(int id);
FbwTab  *fbw_active_tab(void);
int      fbw_tab_new(const char *url, int activate);    /* returns the new id */
void     fbw_tab_close(int id);
void     fbw_tab_activate(int id);
void     fbw_tabs_changed(void);                        /* push the tab list to the UI */
void     fbw_layout(void);
void     fbw_apply_panel_mode(void);
void     fbw_apply_dialog_mode(void);
void     fbw_apply_user_agent(FbwTab *t);
void     fbw_run_script(FbwTab *t, const char *js_utf8, FbwHandlerFn done, void *ctx);   /* done(ctx, HRESULT, LPCWSTR json) */
void     fbw_cdp(FbwTab *t, const char *method, const char *params_json, FbwHandlerFn done, void *ctx);   /* done(ctx, HRESULT, LPCWSTR json) */
void     fbw_page_eval(FbwTab *t, const char *expression, FbwHandlerFn done, void *ctx);  /* Runtime.evaluate, awaits promises */
void     fbw_release(void *com_object);
void     fbw_post_zoom(void);
const char *fbw_shortcut_name(unsigned vk, int ctrl, int shift, int alt);   /* NULL when the key is not a browser shortcut */
void     fbw_shortcut(const char *name);                     /* tell the shell */
void     fbw_tools_cmd(FbwTab *t, const char *cmd_json);   /* picker / recorder control */
FbwCall *fbw_call_new(void (*fn)(FbwCall *));            /* refs = 2: the caller's and the UI thread's */
int      fbw_ui_call(FbwCall *c, DWORD timeout_ms);     /* post to the UI thread and wait: 0 done, 1 timeout, 2 the run was stopped */
void     fbw_call_finish(FbwCall *c);                   /* UI thread: result is ready */
void     fbw_call_release(FbwCall *c);                  /* caller: done with the call object */
void     fbw_set_title(void);
void     fbw_request_close(void);
void     fbw_toggle_fullscreen(void);
void     fbw_flash(void);
void     fbw_on_shell_ready(void);
void     fbw_on_panel_ready(void);

/* ------------------------------------------------------------------ browser engine layer
   Implemented by win_wv2.c (WebView2) or by src/ie/ (Wine: MSHTML in an ATL host window). Everything above this line is
   shared by both builds. All of it is called on the UI thread. */
enum { FBW_NAV_BACK, FBW_NAV_FORWARD, FBW_NAV_RELOAD, FBW_NAV_STOP };
const char *fbw_engine_name(void);                          /* shown in About and in the log */
int      fbh_start(void);                                   /* the main window exists: start creating the shell view; 0 = ok */
void     fbh_create_panel(void);                            /* the shell page is ready: create the Studio view */
void     fbh_create_tab(FbwTab *t);                         /* start creating this tab's view */
void     fbh_destroy_tab(FbwTab *t);                        /* close the view and release engine data (t itself is freed by the caller) */
void     fbh_post_view(FbwView *v, const char *json);       /* deliver a JSON message to a shell / panel page */
void     fbh_set_bounds(FbwCtl c, const RECT *r);
void     fbh_set_visible(FbwCtl c, int visible);
void     fbh_set_parent(FbwCtl c, HWND parent);
void     fbh_parent_moved(FbwCtl c);
void     fbh_focus(FbwCtl c);
void     fbh_close_view(FbwCtl c);                          /* shutdown */
int      fbh_pretranslate(MSG *m);                          /* message loop hook (keyboard for embedded controls): 1 = handled */
void     fbh_timer(void);                                   /* WM_TIMER of the main window (the Wine build polls its pages here) */
int      fbw_tab_navigate(FbwTab *t, const char *url);      /* 0 = started */
int      fbw_tab_can_go(FbwTab *t, int forward);
void     fbw_tab_nav(FbwTab *t, int op);                    /* FBW_NAV_* */
double   fbw_tab_get_zoom(FbwTab *t);
void     fbw_tab_set_zoom(FbwTab *t, double z);
void     fbw_tab_devtools(FbwTab *t);
void     fbw_clear_data(FbwTab *t, int cookies, int cache);
#ifdef FBW_IE
/* The runner's DevTools calls, answered by src/core/fb_legacy.c on top of the MSHTML host (engine thread). Same contract as FbBackend.cdp. */
int      fbie_cdp(const char *method, const char *params_json, FbBuf *result_json, char *err, size_t errsz);
#endif
const char *fbw_build_info(void);                           /* version, compiler, date, engine: written to the log and shown in About */

/* ------------------------------------------------------------------ win_cmd.c */
void     fbw_command(int src, FbJson *msg);             /* a message from the shell / panel UI */
void     fbw_page_message(FbwTab *t, const char *json_utf8);
void     fbw_reply(int src, const FbJson *req, FbJson *extra);       /* {id, ok:true, ...extra} */
void     fbw_reply_err(int src, const FbJson *req, const char *msg);
void     fbw_settings_apply(const FbJson *s);
void     fbw_settings_load_early(void);                  /* before the web view exists (proxy) */
void     fbw_auto_backup(void);
char    *fbw_store_read(const char *key);                /* malloc'ed JSON text or NULL */
int      fbw_store_write(const char *key, const char *json_text);

/* ------------------------------------------------------------------ win_engine.c */
int      fbw_engine_start(FbJson *flow, int step_mode, const FbJson *breakpoints, const FbJson *vars, char *err, size_t errsz);   /* takes ownership of flow; 0 started */
void     fbw_engine_stop(void);
void     fbw_engine_pause(void);
void     fbw_engine_resume(int step_only);
void     fbw_engine_answer(int id, int status, const char *answer);
void     fbw_engine_wake_all(void);
void     fbw_engine_nav_event(FbwTab *t, const char *name, const char *params_json);   /* UI thread */
void     fbw_engine_tab_closed(FbwTab *t);
int      fbw_engine_export(const FbJson *req, char *err, size_t errsz);

#endif
