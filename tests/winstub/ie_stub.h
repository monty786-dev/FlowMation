/* stand-in for oleauto.h / ocidl.h / exdisp.h / exdispid.h / docobj.h / urlmon.h / wininet.h - syntax checking only */
#pragma once
#include "windows.h"
typedef unsigned short VARTYPE; typedef short VARIANT_BOOL; typedef long DISPID; typedef unsigned long LCID; typedef OLECHAR *LPOLESTR; typedef long SCODE;
enum { VT_EMPTY = 0, VT_I4 = 3, VT_BSTR = 8, VT_DISPATCH = 9, VT_BOOL = 11, VT_VARIANT = 12, VT_BYREF = 0x4000 };
#define VARIANT_TRUE ((VARIANT_BOOL)-1)
#define VARIANT_FALSE ((VARIANT_BOOL)0)
typedef struct IDispatch IDispatch;
typedef struct tagVARIANT VARIANT;
struct tagVARIANT { VARTYPE vt; union { long lVal; VARIANT_BOOL boolVal; BSTR bstrVal; IDispatch *pdispVal; VARIANT *pvarVal; BSTR *pbstrVal; VARIANT_BOOL *pboolVal; IDispatch **ppdispVal; } u; };
#define V_VT(v) ((v)->vt)
#define V_I4(v) ((v)->u.lVal)
#define V_BOOL(v) ((v)->u.boolVal)
#define V_BSTR(v) ((v)->u.bstrVal)
#define V_DISPATCH(v) ((v)->u.pdispVal)
#define V_VARIANTREF(v) ((v)->u.pvarVal)
#define V_BSTRREF(v) ((v)->u.pbstrVal)
#define V_BOOLREF(v) ((v)->u.pboolVal)
#define V_DISPATCHREF(v) ((v)->u.ppdispVal)
typedef struct ITypeInfo ITypeInfo;
typedef struct tagDISPPARAMS { VARIANT *rgvarg; DISPID *rgdispidNamedArgs; UINT cArgs; UINT cNamedArgs; } DISPPARAMS;
typedef struct tagEXCEPINFO { WORD wCode, wReserved; BSTR bstrSource, bstrDescription, bstrHelpFile; DWORD dwHelpContext; void *pvReserved, *pfnDeferredFillIn; SCODE scode; } EXCEPINFO;
typedef struct IDispatchVtbl {
  HRESULT (*QueryInterface)(IDispatch *, REFIID, void **); ULONG (*AddRef)(IDispatch *); ULONG (*Release)(IDispatch *);
  HRESULT (*GetTypeInfoCount)(IDispatch *, UINT *); HRESULT (*GetTypeInfo)(IDispatch *, UINT, LCID, ITypeInfo **);
  HRESULT (*GetIDsOfNames)(IDispatch *, REFIID, LPOLESTR *, UINT, LCID, DISPID *);
  HRESULT (*Invoke)(IDispatch *, DISPID, REFIID, LCID, WORD, DISPPARAMS *, VARIANT *, EXCEPINFO *, UINT *);
} IDispatchVtbl;
struct IDispatch { const IDispatchVtbl *lpVtbl; };
typedef struct IWebBrowser2 IWebBrowser2; typedef struct IConnectionPoint IConnectionPoint; typedef struct IConnectionPointContainer IConnectionPointContainer;
typedef struct IOleCommandTarget IOleCommandTarget; typedef struct IOleInPlaceActiveObject IOleInPlaceActiveObject;
extern const IID IID_IUnknown, IID_IDispatch, IID_IWebBrowser2, IID_IConnectionPointContainer, IID_IOleCommandTarget, IID_IOleInPlaceActiveObject, DIID_DWebBrowserEvents2;
enum { LOCALE_USER_DEFAULT = 0x0400, DISPATCH_METHOD = 1, DISPATCH_PROPERTYGET = 2, DISPATCH_PROPERTYPUT = 4, DISPID_PROPERTYPUT = -3,
  DISPID_BEFORENAVIGATE2 = 250, DISPID_NEWWINDOW2 = 251, DISPID_NAVIGATECOMPLETE2 = 252, DISPID_DOCUMENTCOMPLETE = 259, DISPID_TITLECHANGE = 113,
  DISPID_NEWWINDOW3 = 273, DISPID_NAVIGATEERROR = 271, DISPID_COMMANDSTATECHANGE = 105,
  URLMON_OPTION_USERAGENT = 0x10000001, URLMON_OPTION_USERAGENT_REFRESH = 0x10000002,
  INTERNET_OPEN_TYPE_PROXY = 3, INTERNET_OPTION_PROXY = 38, INTERNET_OPTION_END_BROWSER_SESSION = 42 };
typedef struct { DWORD dwStructSize; LPWSTR lpszSourceUrlName; } INTERNET_CACHE_ENTRY_INFOW;
typedef struct { DWORD dummy; } *PIE_UNUSED;
