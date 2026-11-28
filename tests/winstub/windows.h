/* Minimal stand-in for the Windows SDK headers: just enough declarations for clang to parse FlowBrowser's Windows
   sources on Linux. NOT a replacement for the SDK. Names and shapes follow the real ones. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <wchar.h>
#include <time.h>

#define WINAPI
#define CALLBACK
#define STDMETHODCALLTYPE
#define APIENTRY
#define MAX_PATH 260
#define TRUE 1
#define FALSE 0
#define INFINITE 0xFFFFFFFFu
typedef int BOOL; typedef unsigned long DWORD; typedef unsigned int UINT; typedef long LONG; typedef unsigned long ULONG;
typedef unsigned short WORD; typedef unsigned char BYTE; typedef long long LONGLONG; typedef unsigned long long ULONGLONG;
typedef long long INT_PTR; typedef unsigned long long UINT_PTR; typedef long long LONG_PTR; typedef long long INT64;
typedef UINT_PTR WPARAM; typedef LONG_PTR LPARAM; typedef LONG_PTR LRESULT; typedef long HRESULT;
typedef void *HANDLE, *HWND, *HINSTANCE, *HMODULE, *HGLOBAL, *HDC, *HBRUSH, *HICON, *HCURSOR, *HMONITOR, *HMENU;
typedef wchar_t WCHAR; typedef WCHAR *LPWSTR, *PWSTR; typedef const WCHAR *LPCWSTR, *PCWSTR; typedef char *LPSTR; typedef const char *LPCSTR;
typedef void *LPVOID; typedef const void *LPCVOID; typedef DWORD *LPDWORD;
typedef struct _GUID { unsigned long Data1; unsigned short Data2, Data3; unsigned char Data4[8]; } GUID, IID;
typedef const IID *REFIID;
typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef union _LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; } u; LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { void *opaque[6]; } CRITICAL_SECTION;
typedef struct tagPOINT { LONG x, y; } POINT;
typedef struct tagRECT { LONG left, top, right, bottom; } RECT;
typedef struct tagMSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCWSTR lpszMenuName, lpszClassName; HICON hIconSm; } WNDCLASSEXW;
typedef struct { POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize; } MINMAXINFO;
typedef struct { UINT length, flags, showCmd; POINT ptMinPosition, ptMaxPosition; RECT rcNormalPosition; } WINDOWPLACEMENT;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct { UINT cbSize; HWND hwnd; DWORD dwFlags; UINT uCount; DWORD dwTimeout; } FLASHWINFO;
typedef struct { DWORD nLength; void *lpSecurityDescriptor; BOOL bInheritHandle; } SECURITY_ATTRIBUTES;
typedef struct { DWORD cb; LPWSTR lpReserved, lpDesktop, lpTitle; DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags; WORD wShowWindow, cbReserved2; BYTE *lpReserved2; HANDLE hStdInput, hStdOutput, hStdError; } STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;
typedef struct { DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCWSTR lpstrFilter; LPWSTR lpstrCustomFilter; DWORD nMaxCustFilter, nFilterIndex; LPWSTR lpstrFile; DWORD nMaxFile; LPWSTR lpstrFileTitle; DWORD nMaxFileTitle; LPCWSTR lpstrInitialDir, lpstrTitle; DWORD Flags; WORD nFileOffset, nFileExtension; LPCWSTR lpstrDefExt; LPARAM lCustData; void *lpfnHook; LPCWSTR lpTemplateName; } OPENFILENAMEW;
typedef struct { HWND hwnd; UINT wFunc; const WCHAR *pFrom, *pTo; WORD fFlags; BOOL fAnyOperationsAborted; void *hNameMappings; LPCWSTR lpszProgressTitle; } SHFILEOPSTRUCTW;
typedef struct { DWORD dwFileAttributes; WCHAR cFileName[260]; WCHAR cAlternateFileName[14]; } WIN32_FIND_DATAW;
typedef struct IUnknown IUnknown; typedef struct IStream IStream;
typedef void *HBITMAP, *HGDIOBJ; typedef wchar_t OLECHAR; typedef OLECHAR *BSTR;
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter; DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
typedef struct { BITMAPINFOHEADER bmiHeader; DWORD bmiColors[1]; } BITMAPINFO;

extern const IID IID_IMarshal, IID_INoMarshal, IID_IStdMarshalInfo, IID_IExternalConnection;
int IsEqualIID();
#define IsEqualGUID IsEqualIID
#define MAKEINTRESOURCEW(i) ((LPWSTR)(UINT_PTR)(i))
#define LOWORD(l) ((WORD)(((UINT_PTR)(l)) & 0xffff))
#define HIWORD(l) ((WORD)((((UINT_PTR)(l)) >> 16) & 0xffff))
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define S_OK ((HRESULT)0)
#define E_FAIL ((HRESULT)0x80004005L)
#define E_POINTER ((HRESULT)0x80004003L)
#define E_NOINTERFACE ((HRESULT)0x80004002L)
#define E_OUTOFMEMORY ((HRESULT)0x8007000EL)
#define E_INVALIDARG ((HRESULT)0x80070057L)
#define E_NOTIMPL ((HRESULT)0x80004001L)
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define CW_USEDEFAULT ((int)0x80000000)
#define HWND_TOP ((HWND)0)
#define IDC_ARROW MAKEINTRESOURCEW(32512)
#define MAKEWPARAM(l, h) ((WPARAM)(((WORD)(l)) | ((DWORD)((WORD)(h))) << 16))
#define RGB(r, g, b) ((DWORD)(((BYTE)(r)) | ((WORD)((BYTE)(g)) << 8) | (((DWORD)(BYTE)(b)) << 16)))
#define VK_F5 0x74
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F11 0x7A
#define VK_F12 0x7B
#define InterlockedIncrement(p) (++*(p))
#define InterlockedDecrement(p) (--*(p))
#define InterlockedExchange(p, v) (*(p) = (v))
#define InterlockedCompareExchange(p, e, c) (*(p) == (c) ? (*(p) = (e), (c)) : *(p))

enum {
  CP_UTF8 = 65001, CP_OEMCP = 1, MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8, FILE_ATTRIBUTE_DIRECTORY = 16,
  GENERIC_READ = 1, FILE_APPEND_DATA = 4, FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, OPEN_ALWAYS = 4, FILE_ATTRIBUTE_NORMAL = 128,
  STARTF_USESTDHANDLES = 0x100, STARTF_USESHOWWINDOW = 1, CREATE_NO_WINDOW = 0x08000000, HANDLE_FLAG_INHERIT = 1, STD_INPUT_HANDLE = -10,
  WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOWMAXIMIZED = 3, SW_MAXIMIZE = 3, SW_RESTORE = 9, SW_SHOWNA = 8,
  CSIDL_APPDATA = 0x1a, FO_DELETE = 3, FOF_SILENT = 4, FOF_NOCONFIRMATION = 0x10, FOF_NOERRORUI = 0x400,
  OFN_ALLOWMULTISELECT = 0x200, OFN_EXPLORER = 0x80000, OFN_FILEMUSTEXIST = 0x1000, OFN_PATHMUSTEXIST = 0x800, OFN_HIDEREADONLY = 4, OFN_OVERWRITEPROMPT = 2,
  CF_UNICODETEXT = 13, GMEM_MOVEABLE = 2, MB_ICONERROR = 0x10, MB_ICONEXCLAMATION = 0x30,
  FLASHW_ALL = 3, FLASHW_TIMERNOFG = 12,
  WM_DESTROY = 2, WM_SIZE = 5, WM_SETFOCUS = 7, WM_CLOSE = 0x10, WM_GETMINMAXINFO = 0x24, WM_MOVE = 3, WM_MOVING = 0x216, WM_DPICHANGED = 0x2E0, WM_APP = 0x8000, WM_TIMER = 0x113,
  SIZE_MINIMIZED = 1, WS_OVERLAPPEDWINDOW = 0xcf0000, WS_CLIPCHILDREN = 0x2000000, WS_EX_TOOLWINDOW = 0x80, GWL_STYLE = -16,
  SWP_NOZORDER = 4, SWP_NOACTIVATE = 0x10, SWP_NOOWNERZORDER = 0x200, SWP_FRAMECHANGED = 0x20,
  MONITOR_DEFAULTTONEAREST = 2, SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77, SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79, LOGPIXELSX = 88,
  VK_CONTROL = 0x11, VK_SHIFT = 0x10, VK_MENU = 0x12, VK_LEFT = 0x25, VK_RIGHT = 0x27, VK_TAB = 9, VK_NEXT = 0x22, VK_PRIOR = 0x21,
  VK_OEM_PLUS = 0xBB, VK_OEM_MINUS = 0xBD, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_NUMPAD0 = 0x60,
  WS_CHILD = 0x40000000, WS_CLIPSIBLINGS = 0x4000000, WM_KEYDOWN = 0x100, WM_SYSKEYDOWN = 0x104, WM_COMMAND = 0x111, WM_SETTEXT = 0xC, PM_REMOVE = 1, GW_CHILD = 5, GW_HWNDNEXT = 2,
  BI_RGB = 0, DIB_RGB_COLORS = 0, SRCCOPY = 0x00CC0020, MB_ICONINFORMATION = 0x40, ERROR_INSUFFICIENT_BUFFER = 122,
  BCRYPT_USE_SYSTEM_PREFERRED_RNG = 2, SE_DUMMY = 0
};
#include "gen_decls.h"
#include <strings.h>
#define _strnicmp strncasecmp
#define localtime_s(tm, t) (localtime_r((t), (tm)) ? 0 : 1)
wchar_t *_wcsdup(const wchar_t *);
/* registry + message box constants used by the Wine host */
typedef void *HKEY;
#define HKEY_CURRENT_USER ((HKEY)(unsigned long)0x80000001)
#define KEY_SET_VALUE 0x0002
#define REG_DWORD 4
#define ERROR_SUCCESS 0L
#define MB_ICONWARNING 0x00000030L

