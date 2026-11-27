/* FlowBrowser for Wine / Winlator - the MSHTML engine layer (internal header).
   Wine implements Internet Explorer's browser control (shdocvw / mshtml) on top of Wine Gecko, and that is the only
   embeddable browser that runs under Winlator: WebView2 and Chromium need Windows. The control is hosted with ATL's
   "AtlAxWin" window and driven through IWebBrowser2 plus late-bound IDispatch calls, so no mshtml.h is needed.

   Threads: the UI thread owns every COM object (the process is one apartment). The automation engine thread only uses
   fbl_cdp() (src/core/fb_legacy.c) and reaches the page through fbw_ui_call(). */
#ifndef IE_APP_H
#define IE_APP_H

#include "win_app.h"
#include <oleauto.h>
#include <ocidl.h>
#include <exdisp.h>
#include <exdispid.h>
#include <docobj.h>
#include <urlmon.h>
#include <wininet.h>
#include "fb_legacy.h"
#include "fb_png.h"

typedef struct IeSink IeSink;

typedef struct IeView {
    int            kind;           /* FBW_SHELL, FBW_PANEL or 2 = a browser tab */
    int            tab_id;
    HWND           host;           /* the AtlAxWin window that holds the control */
    IWebBrowser2  *br;
    IConnectionPoint *cp;
    DWORD          cookie;
    IeSink        *sink;
    IDispatch     *win;            /* window of the top document (NULL until a document exists) */
    int            doc_ready;      /* the top document finished loading */
    int            top_error;      /* NavigateError seen for the current navigation */
    char         **queue;          /* messages for a UI page that cannot take them yet */
    int            nq;
    unsigned       ticks;
} IeView;

/* ---- ie_com.c: late-bound COM helpers and the browser event sink */
BSTR    ie_bstr(const char *utf8);
char   *ie_variant_u8(const VARIANT *v);                       /* text of a VT_BSTR (maybe by reference), malloc'ed, else NULL */
int     ie_has_prop(IDispatch *d, const wchar_t *name);
HRESULT ie_get(IDispatch *d, const wchar_t *name, VARIANT *out);
HRESULT ie_put(IDispatch *d, const wchar_t *name, VARIANT *val);
HRESULT ie_call(IDispatch *d, const wchar_t *name, VARIANT *args, int nargs, VARIANT *ret);   /* args in call order */
IDispatch *ie_window_of(IDispatch *document);                  /* document.parentWindow (AddRef'ed) */
IDispatch *ie_top_window(IWebBrowser2 *br);                    /* window of the browser's document, or NULL */
int     ie_exec_script(IDispatch *win, const char *js_utf8, char *err, size_t errsz);        /* window.execScript, 0 = ok */
int     ie_eval(IDispatch *win, const char *js_utf8, FbBuf *out, char *err, size_t errsz);   /* value of an expression as text, 0 = ok */
int     ie_same_object(IUnknown *a, IUnknown *b);
int     ie_sink_attach(IeView *v);
void    ie_sink_detach(IeView *v);
int     ie_atl_init(char *err, size_t errsz);
HWND    ie_create_host(HWND parent, IWebBrowser2 **br);        /* AtlAxWin window + browser control */

/* ---- ie_web.c: events called by the sink (UI thread) */
void ie_on_before_navigate(IeView *v, IDispatch *frame, const char *url, int *cancel);
void ie_on_navigate_complete(IeView *v, IDispatch *frame, const char *url);
void ie_on_document_complete(IeView *v, IDispatch *frame, const char *url);
void ie_on_title(IeView *v, const char *title);
void ie_on_new_window(IeView *v, const char *url, int user_initiated, int *cancel, IDispatch **popup_target);
void ie_on_nav_error(IeView *v, IDispatch *frame, const char *url, int status);
void ie_on_command_state(IeView *v, int command, int enabled);
IeView *ie_tab_view(FbwTab *t);

/* ---- ie_engine.c: the runner's view of the page, file dialogs, screenshots (UI thread unless noted) */
void ie_engine_tick(void);                                     /* file dialog watcher */
int  ie_dialog_open(void);                                     /* a native dialog is blocking the page */
void ie_set_user_agent(const char *ua);
void ie_apply_proxy_from_settings(void);
const char *ie_inject_script(void);                            /* legacy layer + page runtime + tools */
int  ie_build_inject_script(void);
void ie_inject_into(IDispatch *win);                           /* idempotent */
void ie_set_dialog_policy(IDispatch *win);

#endif
