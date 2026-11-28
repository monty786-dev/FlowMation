/* FlowBrowser - "legacy engine" bridge (portable).
   The runner speaks the Chrome DevTools Protocol. Browsers without it (Wine's MSHTML / Gecko, as used under
   Winlator on Android) can still host the runner: fbl_cdp() answers the handful of DevTools calls the runner
   makes by running small scripts in the page (src/js/fb_legacy.js) or by asking the host for the things a page
   cannot do itself (file fields, screenshots, cookies, user agent). */
#ifndef FB_LEGACY_H
#define FB_LEGACY_H

#include "fb_json.h"
#include "fb_util.h"

typedef struct FbLegacyHost {
    void *ud;
    /* Runs a script in the top document of the run tab and returns the text it evaluated to (UTF-8). 0 = ok.
       A failure because the page is loading must say "navigating" in err so the runner retries. */
    int (*eval)(void *ud, const char *js, FbBuf *out, char *err, size_t errsz);
    /* Puts files into a file field. handle = element handle of the <input type=file>, or 0 for the file chooser
       that is currently open (backend_node is the id the host reported with Page.fileChooserOpened, if any).
       paths = array of absolute paths. NULL = not supported. */
    int (*set_files)(void *ud, int handle, double backend_node, const FbJson *paths, char *err, size_t errsz);
    /* Page.setInterceptFileChooserDialog on / off. NULL or non-zero = not supported. */
    int (*chooser)(void *ud, int on, char *err, size_t errsz);
    /* PNG of the visible page as base64. params as for Page.captureScreenshot. NULL = not supported. */
    int (*screenshot)(void *ud, const FbJson *params, FbBuf *base64, char *err, size_t errsz);
    int (*clear_data)(void *ud, int cookies, int cache, char *err, size_t errsz);
    int (*user_agent)(void *ud, const char *ua, char *err, size_t errsz);       /* "" = back to the default */
    /* Optional: returns non-zero when the run was stopped, to end long waits early. */
    int (*stopped)(void *ud);
} FbLegacyHost;

/* Same contract as FbBackend.cdp. Unsupported methods fail with "Not supported by this browser engine: <method>". */
int fbl_cdp(const FbLegacyHost *h, const char *method, const char *params_json, FbBuf *result_json, char *err, size_t errsz);

/* Appends s as a JavaScript string literal that every engine can parse (ASCII only, \uXXXX escapes). */
void fbl_js_string(FbBuf *out, const char *s);

#endif
