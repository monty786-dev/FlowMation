/* fb_legacy.c without a browser: a scripted host answers the page-script calls. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "fb_legacy.h"

static int total, failed;
#define CHECK(c, ...) do { total++; if (!(c)) { failed++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char last_js[4096];
static const char *answers[8];
static int n_answers, answer_at, eval_calls, files_called, files_handle;
static double files_node;

static int h_eval(void *ud, const char *js, FbBuf *out, char *err, size_t errsz)
{
    (void)ud;
    snprintf(last_js, sizeof last_js, "%s", js);
    eval_calls++;
    if (answer_at >= n_answers) { fb_set_err(err, errsz, "no answer"); return 1; }
    const char *a = answers[answer_at++];
    if (!strcmp(a, "FAIL")) { fb_set_err(err, errsz, "The page is navigating"); return 1; }
    fb_buf_puts(out, a);
    return 0;
}
static int h_files(void *ud, int handle, double node, const FbJson *paths, char *err, size_t errsz)
{
    (void)ud; (void)err; (void)errsz;
    files_called++; files_handle = handle; files_node = node;
    return paths && paths->type == FBJ_ARR && paths->count == 2 ? 0 : 1;
}
static void set_answers(const char *a, const char *b, const char *c)
{
    n_answers = 0; answer_at = 0; eval_calls = 0;
    if (a) answers[n_answers++] = a;
    if (b) answers[n_answers++] = b;
    if (c) answers[n_answers++] = c;
}

static char *call(const FbLegacyHost *h, const char *m, const char *p, int *rc, char *err)
{
    FbBuf r;
    fb_buf_init(&r);
    err[0] = 0;
    *rc = fbl_cdp(h, m, p, &r, err, 256);
    return fb_buf_detach(&r);
}

int main(void)
{
    FbLegacyHost h = { 0 };
    h.eval = h_eval;
    char err[256], *r;
    int rc;

    /* string literals are ASCII-only and survive every engine */
    FbBuf b; fb_buf_init(&b);
    fbl_js_string(&b, "a\"b\\c\n\xE2\x80\xA8 \xC3\xA9 \xF0\x9F\x98\x80");
    CHECK(!strcmp(fb_buf_cstr(&b), "\"a\\\"b\\\\c\\n\\u2028 \\u00e9 \\ud83d\\ude00\""), "js string: %s", fb_buf_cstr(&b));
    fb_buf_free(&b);

    /* value */
    set_answers("{\"ok\":true,\"value\":{\"a\":[1,2]}}", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"1+1\",\"returnByValue\":true}", &rc, err);
    CHECK(rc == 0 && strstr(r, "\"value\":{\"a\":[1,2]}") && strstr(r, "\"type\":\"object\""), "value: %s", r);
    CHECK(strstr(last_js, "window.__fbLegacy.run(\"1+1\",true)") != NULL, "script: %s", last_js);
    free(r);
    /* string / number / bool / null / undefined */
    set_answers("{\"ok\":true,\"value\":\"x\"}", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"'x'\",\"returnByValue\":true}", &rc, err);
    CHECK(strstr(r, "\"type\":\"string\"") && strstr(r, "\"value\":\"x\""), "string: %s", r); free(r);
    set_answers("{\"ok\":true,\"value\":null}", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"null\",\"returnByValue\":true}", &rc, err);
    CHECK(strstr(r, "\"subtype\":\"null\""), "null: %s", r); free(r);
    set_answers("{\"ok\":true,\"undef\":true}", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"void 0\",\"returnByValue\":true}", &rc, err);
    CHECK(strstr(r, "\"type\":\"undefined\""), "undefined: %s", r); free(r);
    /* handle */
    set_answers("{\"ok\":true,\"handle\":7}", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"document.body\",\"returnByValue\":false}", &rc, err);
    CHECK(strstr(r, "\"objectId\":\"fbh:7\""), "handle: %s", r); free(r);
    /* script error becomes exceptionDetails, like the protocol */
    set_answers("{\"ok\":false,\"error\":\"SyntaxError: Illegal return (x)\"}", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"return 1\"}", &rc, err);
    CHECK(rc == 0 && strstr(r, "exceptionDetails") && strstr(r, "Illegal return"), "exception: %s", r); free(r);
    /* a promise is polled until it settles */
    set_answers("{\"ok\":true,\"pending\":3}", "{\"ok\":true,\"pending\":3}", "{\"ok\":true,\"value\":42}");
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"p\",\"returnByValue\":true,\"awaitPromise\":true}", &rc, err);
    CHECK(rc == 0 && strstr(r, "\"value\":42") && eval_calls == 3, "promise: %s calls=%d", r, eval_calls);
    CHECK(strstr(last_js, "poll(3,true)") != NULL, "poll call: %s", last_js); free(r);
    /* page not ready */
    set_answers("__NOT_READY__", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"1\"}", &rc, err);
    CHECK(rc != 0 && strstr(err, "navigating"), "not ready: rc=%d err=%s", rc, err); free(r);
    set_answers("FAIL", NULL, NULL);
    r = call(&h, "Runtime.evaluate", "{\"expression\":\"1\"}", &rc, err);
    CHECK(rc != 0 && strstr(err, "navigating"), "eval failure passes through: %s", err); free(r);

    /* input */
    set_answers("", NULL, NULL);
    r = call(&h, "Input.dispatchMouseEvent", "{\"type\":\"mousePressed\",\"x\":10.5,\"y\":20,\"button\":\"left\",\"clickCount\":2}", &rc, err);
    CHECK(rc == 0 && !strcmp(r, "{}") && strstr(last_js, "mouse(\"mousePressed\",10.50,20.00,\"left\",2)"), "mouse: %s | %s", r, last_js); free(r);
    set_answers("", NULL, NULL);
    r = call(&h, "Input.dispatchKeyEvent", "{\"type\":\"rawKeyDown\",\"key\":\"Enter\",\"code\":\"Enter\",\"windowsVirtualKeyCode\":13,\"modifiers\":8,\"text\":\"\\r\"}", &rc, err);
    CHECK(rc == 0 && strstr(last_js, "key(\"keyDown\",\"Enter\",\"Enter\",13,8,\"\\r\",null)"), "key: %s", last_js); free(r);
    set_answers("", NULL, NULL);
    r = call(&h, "Input.dispatchKeyEvent", "{\"type\":\"keyDown\",\"key\":\"a\",\"modifiers\":2,\"commands\":[\"selectAll\"]}", &rc, err);
    CHECK(strstr(last_js, "null,\"selectAll\")") != NULL, "key command: %s", last_js); free(r);
    set_answers("", NULL, NULL);
    r = call(&h, "Input.insertText", "{\"text\":\"h\\u00e9llo\"}", &rc, err);
    CHECK(rc == 0 && strstr(last_js, "insert(\"h\\u00e9llo\")"), "insert: %s", last_js); free(r);

    /* things only the host can do */
    r = call(&h, "DOM.setFileInputFiles", "{\"files\":[\"a\",\"b\"],\"objectId\":\"fbh:5\"}", &rc, err);
    CHECK(rc != 0 && strstr(err, "cannot put files"), "no set_files: %s", err); free(r);
    h.set_files = h_files;
    r = call(&h, "DOM.setFileInputFiles", "{\"files\":[\"a\",\"b\"],\"objectId\":\"fbh:5\"}", &rc, err);
    CHECK(rc == 0 && files_handle == 5, "set_files by handle: rc=%d handle=%d", rc, files_handle); free(r);
    r = call(&h, "DOM.setFileInputFiles", "{\"files\":[\"a\",\"b\"],\"backendNodeId\":12}", &rc, err);
    CHECK(rc == 0 && files_handle == 0 && files_node == 12, "set_files for the open chooser: handle=%d node=%.0f", files_handle, files_node); free(r);
    r = call(&h, "DOM.setFileInputFiles", "{\"files\":[\"a\"],\"objectId\":\"junk\"}", &rc, err);
    CHECK(rc != 0, "bad object id is refused"); free(r);
    r = call(&h, "Page.setInterceptFileChooserDialog", "{\"enabled\":true}", &rc, err);
    CHECK(rc != 0 && strstr(err, "Not supported"), "chooser unsupported: %s", err); free(r);
    r = call(&h, "Page.captureScreenshot", "{}", &rc, err);
    CHECK(rc != 0 && strstr(err, "screenshots"), "screenshot unsupported: %s", err); free(r);
    r = call(&h, "Network.clearBrowserCache", "{}", &rc, err);
    CHECK(rc != 0, "cache unsupported"); free(r);
    r = call(&h, "Page.enable", "{}", &rc, err);
    CHECK(rc == 0 && !strcmp(r, "{}"), "enable calls are accepted"); free(r);
    r = call(&h, "Target.createTarget", "{}", &rc, err);
    CHECK(rc != 0 && strstr(err, "Not supported by this browser engine: Target.createTarget"), "unknown method: %s", err); free(r);

    printf("%d checks, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
