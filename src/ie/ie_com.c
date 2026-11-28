/* FlowBrowser for Wine - late-bound COM helpers, ATL hosting and the browser event sink. */
#include "ie_app.h"

static const IID ie_iid_null = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

BSTR ie_bstr(const char *u8)
{
    wchar_t *w = fbw_w(u8 ? u8 : "");
    BSTR b = w ? SysAllocString(w) : NULL;
    free(w);
    return b;
}

char *ie_variant_u8(const VARIANT *v)
{
    while (v && V_VT(v) == (VT_BYREF | VT_VARIANT)) v = V_VARIANTREF(v);
    if (!v) return NULL;
    if (V_VT(v) == VT_BSTR) return V_BSTR(v) ? fbw_u8(V_BSTR(v)) : fb_strdup("");
    if (V_VT(v) == (VT_BYREF | VT_BSTR)) return V_BSTRREF(v) && *V_BSTRREF(v) ? fbw_u8(*V_BSTRREF(v)) : fb_strdup("");
    return NULL;
}

int ie_same_object(IUnknown *a, IUnknown *b)
{
    IUnknown *ua = NULL, *ub = NULL;
    int same = 0;
    if (!a || !b) return 0;
    if (SUCCEEDED(IUnknown_QueryInterface(a, &IID_IUnknown, (void **)&ua)) && SUCCEEDED(IUnknown_QueryInterface(b, &IID_IUnknown, (void **)&ub))) same = ua == ub;
    if (ua) IUnknown_Release(ua);
    if (ub) IUnknown_Release(ub);
    return same;
}

/* ------------------------------------------------------------------ IDispatch by name */
static HRESULT name_id(IDispatch *d, const wchar_t *name, DISPID *id)
{
    LPOLESTR n = (LPOLESTR)name;
    return IDispatch_GetIDsOfNames(d, &ie_iid_null, &n, 1, LOCALE_USER_DEFAULT, id);
}

int ie_has_prop(IDispatch *d, const wchar_t *name)
{
    DISPID id;
    return d && name_id(d, name, &id) == S_OK;
}

static HRESULT invoke(IDispatch *d, DISPID id, WORD flags, VARIANT *args, int nargs, VARIANT *ret)
{
    DISPPARAMS dp;
    VARIANT rev[8];
    DISPID put = DISPID_PROPERTYPUT;
    EXCEPINFO ei;
    UINT argerr = 0;
    VARIANT tmp;
    memset(&dp, 0, sizeof dp);
    memset(&ei, 0, sizeof ei);
    if (nargs > 8) return E_INVALIDARG;
    for (int i = 0; i < nargs; i++) rev[i] = args[nargs - 1 - i];     /* IDispatch wants the last argument first */
    dp.rgvarg = nargs ? rev : NULL;
    dp.cArgs = (UINT)nargs;
    if (flags & DISPATCH_PROPERTYPUT) { dp.cNamedArgs = 1; dp.rgdispidNamedArgs = &put; }
    VariantInit(&tmp);
    if (ret) VariantInit(ret);
    HRESULT hr = IDispatch_Invoke(d, id, &ie_iid_null, LOCALE_USER_DEFAULT, flags, &dp, ret ? ret : &tmp, &ei, &argerr);
    VariantClear(&tmp);
    SysFreeString(ei.bstrSource);
    SysFreeString(ei.bstrDescription);
    SysFreeString(ei.bstrHelpFile);
    return hr;
}

HRESULT ie_get(IDispatch *d, const wchar_t *name, VARIANT *out)
{
    DISPID id;
    HRESULT hr;
    if (!d) return E_POINTER;
    hr = name_id(d, name, &id);
    if (FAILED(hr)) return hr;
    return invoke(d, id, DISPATCH_PROPERTYGET, NULL, 0, out);
}

HRESULT ie_put(IDispatch *d, const wchar_t *name, VARIANT *val)
{
    DISPID id;
    HRESULT hr;
    if (!d) return E_POINTER;
    hr = name_id(d, name, &id);
    if (FAILED(hr)) return hr;
    return invoke(d, id, DISPATCH_PROPERTYPUT, val, 1, NULL);
}

HRESULT ie_call(IDispatch *d, const wchar_t *name, VARIANT *args, int nargs, VARIANT *ret)
{
    DISPID id;
    HRESULT hr;
    if (!d) return E_POINTER;
    hr = name_id(d, name, &id);
    if (FAILED(hr)) return hr;
    return invoke(d, id, DISPATCH_METHOD, args, nargs, ret);
}

IDispatch *ie_window_of(IDispatch *document)
{
    VARIANT v;
    IDispatch *w = NULL;
    if (!document || FAILED(ie_get(document, L"parentWindow", &v))) return NULL;
    if (V_VT(&v) == VT_DISPATCH && V_DISPATCH(&v)) { w = V_DISPATCH(&v); IDispatch_AddRef(w); }
    VariantClear(&v);
    return w;
}

IDispatch *ie_top_window(IWebBrowser2 *br)
{
    IDispatch *doc = NULL, *w;
    if (!br || FAILED(IWebBrowser2_get_Document(br, &doc)) || !doc) return NULL;
    w = ie_window_of(doc);
    IDispatch_Release(doc);
    return w;
}

int ie_exec_script(IDispatch *win, const char *js, char *err, size_t errsz)
{
    VARIANT a[2];
    VariantInit(&a[0]); VariantInit(&a[1]);
    V_VT(&a[0]) = VT_BSTR; V_BSTR(&a[0]) = ie_bstr(js);
    V_VT(&a[1]) = VT_BSTR; V_BSTR(&a[1]) = SysAllocString(L"JScript");
    HRESULT hr = V_BSTR(&a[0]) ? ie_call(win, L"execScript", a, 2, NULL) : E_OUTOFMEMORY;
    SysFreeString(V_BSTR(&a[0]));
    SysFreeString(V_BSTR(&a[1]));
    if (FAILED(hr)) { fb_set_err(err, errsz, "navigating"); return 1; }          /* no document yet, or one that is being replaced */
    return 0;
}

/* Page scripts reach us as text: __fbEval evaluates an expression and returns String(value) ('' for undefined / null). */
static const char EVAL_HELPER[] =
    "window.__fbEval=function(__s){try{var __v=eval(__s);return (__v===undefined||__v===null)?'':String(__v);}"
    "catch(__e){return '!E:'+(__e&&__e.message?__e.message:String(__e));}};";

int ie_eval(IDispatch *win, const char *js, FbBuf *out, char *err, size_t errsz)
{
    VARIANT a, ret;
    if (!win) { fb_set_err(err, errsz, "navigating"); return 1; }
    if (!ie_has_prop(win, L"__fbEval")) {
        if (ie_exec_script(win, EVAL_HELPER, err, errsz)) return 1;
        if (!ie_has_prop(win, L"__fbEval")) { fb_set_err(err, errsz, "navigating"); return 1; }
    }
    VariantInit(&a);
    V_VT(&a) = VT_BSTR;
    V_BSTR(&a) = ie_bstr(js);
    if (!V_BSTR(&a)) { fb_set_err(err, errsz, "Out of memory"); return 1; }
    HRESULT hr = ie_call(win, L"__fbEval", &a, 1, &ret);
    SysFreeString(V_BSTR(&a));
    if (FAILED(hr)) { fb_set_err(err, errsz, "navigating"); VariantClear(&ret); return 1; }
    char *s = ie_variant_u8(&ret);
    VariantClear(&ret);
    if (!s) s = fb_strdup("");
    if (!strncmp(s, "!E:", 3)) { fb_set_err(err, errsz, "%s", s + 3); free(s); return 1; }
    fb_buf_puts(out, s);
    free(s);
    return 0;
}

/* ------------------------------------------------------------------ ATL hosting */
typedef BOOL    (WINAPI *PFN_AtlAxWinInit)(void);
typedef HRESULT (WINAPI *PFN_AtlAxGetControl)(HWND, IUnknown **);
static PFN_AtlAxGetControl g_get_control;

int ie_atl_init(char *err, size_t errsz)
{
    static const wchar_t *libs[] = { L"atl.dll", L"atl110.dll", L"atl100.dll", L"atl90.dll", L"atl80.dll", NULL };
    for (int i = 0; libs[i] && !g_get_control; i++) {
        HMODULE m = LoadLibraryW(libs[i]);
        if (!m) continue;
        PFN_AtlAxWinInit init = (PFN_AtlAxWinInit)(void *)GetProcAddress(m, "AtlAxWinInit");
        PFN_AtlAxGetControl get = (PFN_AtlAxGetControl)(void *)GetProcAddress(m, "AtlAxGetControl");
        if (init && get && init()) g_get_control = get;
    }
    if (!g_get_control) { fb_set_err(err, errsz, "atl.dll is not available, so the browser control cannot be hosted"); return 1; }
    return 0;
}

HWND ie_create_host(HWND parent, IWebBrowser2 **br)
{
    IUnknown *unk = NULL;
    *br = NULL;
    if (!g_get_control) return NULL;
    /* the window text of an AtlAxWin window names the control to create: this is CLSID_WebBrowser (Shell.Explorer) */
    HWND h = CreateWindowExW(0, L"AtlAxWin", L"{8856F961-340A-11D0-A96B-00C04FD705A2}", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                             0, 0, 16, 16, parent, NULL, G.inst, NULL);
    if (!h) return NULL;
    if (FAILED(g_get_control(h, &unk)) || !unk) { DestroyWindow(h); return NULL; }
    if (FAILED(IUnknown_QueryInterface(unk, &IID_IWebBrowser2, (void **)br)) || !*br) { IUnknown_Release(unk); DestroyWindow(h); return NULL; }
    IUnknown_Release(unk);
    IWebBrowser2_put_Silent(*br, VARIANT_TRUE);              /* no script-error pop-ups */
    return h;
}

/* ------------------------------------------------------------------ DWebBrowserEvents2 sink */
struct IeSink {
    const IDispatchVtbl *lpVtbl;
    LONG                 ref;
    IeView              *v;
};

static HRESULT STDMETHODCALLTYPE sink_qi(IDispatch *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDispatch) || IsEqualIID(riid, &DIID_DWebBrowserEvents2)) {
        *ppv = This;
        IDispatch_AddRef(This);
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE sink_addref(IDispatch *This) { return (ULONG)InterlockedIncrement(&((IeSink *)This)->ref); }
static ULONG STDMETHODCALLTYPE sink_release(IDispatch *This)
{
    IeSink *s = (IeSink *)This;
    LONG n = InterlockedDecrement(&s->ref);
    if (n == 0) free(s);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE sink_count(IDispatch *This, UINT *n) { (void)This; if (n) *n = 0; return S_OK; }
static HRESULT STDMETHODCALLTYPE sink_typeinfo(IDispatch *This, UINT i, LCID lcid, ITypeInfo **ti) { (void)This; (void)i; (void)lcid; if (ti) *ti = NULL; return E_NOTIMPL; }
static HRESULT STDMETHODCALLTYPE sink_ids(IDispatch *This, REFIID riid, LPOLESTR *names, UINT n, LCID lcid, DISPID *ids) { (void)This; (void)riid; (void)names; (void)n; (void)lcid; (void)ids; return E_NOTIMPL; }

/* Invoke receives the parameters in reverse order: rgvarg[0] is the last parameter of the event. */
static VARIANT *deref(VARIANT *v)
{
    while (v && V_VT(v) == (VT_BYREF | VT_VARIANT)) v = V_VARIANTREF(v);
    return v;
}
static void set_bool_ref(VARIANT *v, int on) { if (v && V_VT(v) == (VT_BYREF | VT_BOOL) && V_BOOLREF(v)) *V_BOOLREF(v) = on ? VARIANT_TRUE : VARIANT_FALSE; }
static IDispatch *disp_of(VARIANT *v) { v = deref(v); return v && V_VT(v) == VT_DISPATCH ? V_DISPATCH(v) : NULL; }

static HRESULT STDMETHODCALLTYPE sink_invoke(IDispatch *This, DISPID id, REFIID riid, LCID lcid, WORD flags, DISPPARAMS *dp, VARIANT *res, EXCEPINFO *ei, UINT *argerr)
{
    IeSink *s = (IeSink *)This;
    IeView *v = s->v;
    VARIANT *a = dp ? dp->rgvarg : NULL;
    int n = dp ? (int)dp->cArgs : 0;
    (void)riid; (void)lcid; (void)flags; (void)res; (void)ei; (void)argerr;
    if (!v || !a) return S_OK;
    switch (id) {
    case DISPID_BEFORENAVIGATE2:
        if (n >= 7) {
            char *url = ie_variant_u8(&a[5]);
            int cancel = 0;
            ie_on_before_navigate(v, disp_of(&a[6]), url ? url : "", &cancel);
            if (cancel) set_bool_ref(&a[0], 1);
            free(url);
        }
        break;
    case DISPID_NAVIGATECOMPLETE2:
    case DISPID_DOCUMENTCOMPLETE:
        if (n >= 2) {
            char *url = ie_variant_u8(&a[0]);
            if (id == DISPID_DOCUMENTCOMPLETE) ie_on_document_complete(v, disp_of(&a[1]), url ? url : "");
            else ie_on_navigate_complete(v, disp_of(&a[1]), url ? url : "");
            free(url);
        }
        break;
    case DISPID_TITLECHANGE:
        if (n >= 1) { char *t = ie_variant_u8(&a[0]); ie_on_title(v, t ? t : ""); free(t); }
        break;
    case DISPID_NEWWINDOW3:
        if (n >= 5) {
            char *url = ie_variant_u8(&a[0]);
            IDispatch *target = NULL;
            int cancel = 0;
            long flagsv = (V_VT(&a[2]) == VT_I4) ? (long)V_I4(&a[2]) : 0;
            ie_on_new_window(v, url ? url : "", (flagsv & 2) != 0 /* NWMF_USERINITED */, &cancel, &target);
            if (target && V_VT(&a[4]) == (VT_BYREF | VT_DISPATCH) && V_DISPATCHREF(&a[4])) *V_DISPATCHREF(&a[4]) = target;
            else if (target) IDispatch_Release(target);
            set_bool_ref(&a[3], cancel);
            free(url);
        }
        break;
    case DISPID_NEWWINDOW2:
        if (n >= 2) {
            IDispatch *target = NULL;
            int cancel = 0;
            ie_on_new_window(v, "", 1, &cancel, &target);      /* the address arrives later: hand the browser a tab to load it into */
            if (target && V_VT(&a[1]) == (VT_BYREF | VT_DISPATCH) && V_DISPATCHREF(&a[1])) *V_DISPATCHREF(&a[1]) = target;
            else if (target) IDispatch_Release(target);
            set_bool_ref(&a[0], cancel);
        }
        break;
    case DISPID_NAVIGATEERROR:
        if (n >= 5) {
            VARIANT *st = deref(&a[1]);
            char *url = ie_variant_u8(&a[3]);
            ie_on_nav_error(v, disp_of(&a[4]), url ? url : "", st && V_VT(st) == VT_I4 ? (int)V_I4(st) : 1);
            free(url);
        }
        break;
    case DISPID_COMMANDSTATECHANGE:
        if (n >= 2) ie_on_command_state(v, (int)V_I4(&a[1]), V_BOOL(&a[0]) != VARIANT_FALSE);
        break;
    default:
        break;
    }
    return S_OK;
}

static const IDispatchVtbl g_sink_vtbl = { sink_qi, sink_addref, sink_release, sink_count, sink_typeinfo, sink_ids, sink_invoke };

int ie_sink_attach(IeView *v)
{
    IConnectionPointContainer *cpc = NULL;
    HRESULT hr;
    if (FAILED(IWebBrowser2_QueryInterface(v->br, &IID_IConnectionPointContainer, (void **)&cpc)) || !cpc) return 1;
    hr = IConnectionPointContainer_FindConnectionPoint(cpc, &DIID_DWebBrowserEvents2, &v->cp);
    IConnectionPointContainer_Release(cpc);
    if (FAILED(hr) || !v->cp) return 1;
    IeSink *s = (IeSink *)calloc(1, sizeof *s);
    if (!s) return 1;
    s->lpVtbl = &g_sink_vtbl;
    s->ref = 1;
    s->v = v;
    hr = IConnectionPoint_Advise(v->cp, (IUnknown *)s, &v->cookie);
    if (FAILED(hr)) { free(s); return 1; }
    v->sink = s;
    IDispatch_Release((IDispatch *)s);         /* ours: the connection point keeps the object alive */
    return 0;
}

void ie_sink_detach(IeView *v)
{
    if (v->sink) v->sink->v = NULL;      /* events that are still in flight see no view */
    if (v->cp) {
        if (v->cookie) IConnectionPoint_Unadvise(v->cp, v->cookie);
        IConnectionPoint_Release(v->cp);
    }
    v->cp = NULL;
    v->cookie = 0;
    v->sink = NULL;
}
