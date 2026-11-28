#include "fb_legacy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void fbl_js_string(FbBuf *out, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    fb_buf_putc(out, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned cp = *p;
        int n = 1;
        if (cp >= 0xF0 && p[1] && p[2] && p[3]) { cp = ((cp & 7u) << 18) | ((p[1] & 63u) << 12) | ((p[2] & 63u) << 6) | (p[3] & 63u); n = 4; }
        else if (cp >= 0xE0 && p[1] && p[2]) { cp = ((cp & 15u) << 12) | ((p[1] & 63u) << 6) | (p[2] & 63u); n = 3; }
        else if (cp >= 0xC0 && p[1]) { cp = ((cp & 31u) << 6) | (p[1] & 63u); n = 2; }
        p += n;
        if (cp == '"' || cp == '\\') { fb_buf_putc(out, '\\'); fb_buf_putc(out, (char)cp); }
        else if (cp == '\n') fb_buf_puts(out, "\\n");
        else if (cp == '\r') fb_buf_puts(out, "\\r");
        else if (cp == '\t') fb_buf_puts(out, "\\t");
        else if (cp < 0x20 || cp >= 0x7f) {
            unsigned units[2];
            int nu = 1;
            if (cp >= 0x10000) { cp -= 0x10000; units[0] = 0xD800 + (cp >> 10); units[1] = 0xDC00 + (cp & 0x3FF); nu = 2; }
            else units[0] = cp;
            for (int i = 0; i < nu; i++) {
                char e[7] = { '\\', 'u', hex[(units[i] >> 12) & 15], hex[(units[i] >> 8) & 15], hex[(units[i] >> 4) & 15], hex[units[i] & 15], 0 };
                fb_buf_puts(out, e);
            }
        } else fb_buf_putc(out, (char)cp);
    }
    fb_buf_putc(out, '"');
}


/* runs "window.__fbLegacy.<call>" and returns the string it produced */
static int legacy_call(const FbLegacyHost *h, const char *call, FbBuf *out, char *err, size_t errsz)
{
    FbBuf js;
    fb_buf_init(&js);
    fb_buf_printf(&js, "window.__fbLegacy?window.__fbLegacy.%s:'__NOT_READY__'", call);
    int rc = h->eval(h->ud, fb_buf_cstr(&js), out, err, errsz);
    fb_buf_free(&js);
    if (rc) return rc;
    if (!strcmp(fb_buf_cstr(out), "__NOT_READY__")) { fb_set_err(err, errsz, "%s", "The page is navigating (the script layer is not ready yet)"); return 1; }
    return 0;
}

static void result_with(FbBuf *result, const char *json_obj) { fb_buf_clear(result); fb_buf_puts(result, json_obj); }

static void exception_result(FbBuf *result, const char *msg)
{
    fb_buf_clear(result);
    fb_buf_puts(result, "{\"exceptionDetails\":{\"text\":");
    fbj_write_escaped_string(result, msg, strlen(msg));
    fb_buf_puts(result, ",\"exception\":{\"description\":");
    fbj_write_escaped_string(result, msg, strlen(msg));
    fb_buf_puts(result, "}}}");
}

/* turns the script layer's answer ({ok, value|handle|undef|pending}) into a Runtime.evaluate result */
static int finish_value(const char *answer, FbBuf *result, int *pending, char *err, size_t errsz)
{
    FbJson *a = fbj_parse_str(answer, err, errsz);
    *pending = 0;
    if (!a) { fb_set_err(err, errsz, "The page returned an unreadable answer"); return 1; }
    if (!fbj_get_bool(a, "ok", 0)) { exception_result(result, fbj_get_str(a, "error", "script error")); fbj_free(a); return 0; }
    if (fbj_get(a, "pending")) { *pending = (int)fbj_get_num(a, "pending", 0); fbj_free(a); return 0; }
    FbBuf r;
    fb_buf_init(&r);
    if (fbj_get(a, "handle")) {
        fb_buf_printf(&r, "{\"result\":{\"type\":\"object\",\"objectId\":\"fbh:%d\"}}", (int)fbj_get_num(a, "handle", 0));
    } else if (fbj_get_bool(a, "undef", 0)) {
        fb_buf_puts(&r, "{\"result\":{\"type\":\"undefined\"}}");
    } else {
        const FbJson *v = fbj_get(a, "value");
        const char *type = "object";
        if (v && v->type == FBJ_STR) type = "string";
        else if (v && v->type == FBJ_NUM) type = "number";
        else if (v && v->type == FBJ_BOOL) type = "boolean";
        char *vt = v ? fbj_to_string(v, 0) : fb_strdup("null");
        fb_buf_printf(&r, "{\"result\":{\"type\":\"%s\",%s\"value\":", type, v && v->type == FBJ_NULL ? "\"subtype\":\"null\"," : "");
        fb_buf_puts(&r, vt);
        fb_buf_puts(&r, "}}");
        free(vt);
    }
    result_with(result, fb_buf_cstr(&r));
    fb_buf_free(&r);
    fbj_free(a);
    return 0;
}

static int do_evaluate(const FbLegacyHost *h, const FbJson *p, FbBuf *result, char *err, size_t errsz)
{
    const char *expr = fbj_get_str(p, "expression", "");
    int by_value = fbj_get_bool(p, "returnByValue", 0);
    FbBuf call, answer;
    fb_buf_init(&call);
    fb_buf_init(&answer);
    fb_buf_puts(&call, "run(");
    fbl_js_string(&call, expr);
    fb_buf_puts(&call, by_value ? ",true)" : ",false)");
    int rc = legacy_call(h, fb_buf_cstr(&call), &answer, err, errsz);
    fb_buf_free(&call);
    int pending = 0;
    if (!rc) rc = finish_value(fb_buf_cstr(&answer), result, &pending, err, errsz);
    uint64_t end = fb_now_ms() + 60000;
    while (!rc && pending) {                                                   /* a promise: poll until it settles */
        if ((h->stopped && h->stopped(h->ud)) || fb_now_ms() > end) { exception_result(result, "The script did not finish"); break; }
        fb_sleep_ms(25);
        char c[64];
        snprintf(c, sizeof c, "poll(%d,%s)", pending, by_value ? "true" : "false");
        fb_buf_clear(&answer);
        rc = legacy_call(h, c, &answer, err, errsz);
        if (!rc) rc = finish_value(fb_buf_cstr(&answer), result, &pending, err, errsz);
    }
    fb_buf_free(&answer);
    return rc;
}

static int run_simple(const FbLegacyHost *h, const FbBuf *call, FbBuf *result, char *err, size_t errsz)
{
    FbBuf out;
    fb_buf_init(&out);
    int rc = legacy_call(h, fb_buf_cstr((FbBuf *)call), &out, err, errsz);
    fb_buf_free(&out);
    if (!rc) result_with(result, "{}");
    return rc;
}

static int parse_handle(const char *oid)
{
    if (oid && !strncmp(oid, "fbh:", 4)) return atoi(oid + 4);
    return -1;
}

int fbl_cdp(const FbLegacyHost *h, const char *method, const char *params_json, FbBuf *result, char *err, size_t errsz)
{
    FbJson *p = fbj_parse_str(params_json && *params_json ? params_json : "{}", err, errsz);
    if (!p) return 1;
    int rc = 0;
    fb_buf_clear(result);
    if (!strcmp(method, "Runtime.evaluate")) rc = do_evaluate(h, p, result, err, errsz);
    else if (!strcmp(method, "Input.dispatchMouseEvent")) {
        FbBuf c;
        fb_buf_init(&c);
        fb_buf_printf(&c, "mouse(\"%s\",%.2f,%.2f,\"%s\",%d)", !strcmp(fbj_get_str(p, "type", ""), "mousePressed") ? "mousePressed" : !strcmp(fbj_get_str(p, "type", ""), "mouseReleased") ? "mouseReleased" : "mouseMoved",
                      fbj_get_num(p, "x", 0), fbj_get_num(p, "y", 0), !strcmp(fbj_get_str(p, "button", ""), "right") ? "right" : !strcmp(fbj_get_str(p, "button", ""), "middle") ? "middle" : "left",
                      (int)fbj_get_num(p, "clickCount", 1));
        rc = run_simple(h, &c, result, err, errsz);
        fb_buf_free(&c);
    } else if (!strcmp(method, "Input.dispatchKeyEvent")) {
        FbBuf c;
        fb_buf_init(&c);
        const char *type = fbj_get_str(p, "type", "keyDown");
        fb_buf_printf(&c, "key(\"%s\",", !strcmp(type, "keyUp") ? "keyUp" : "keyDown");
        fbl_js_string(&c, fbj_get_str(p, "key", ""));
        fb_buf_putc(&c, ',');
        fbl_js_string(&c, fbj_get_str(p, "code", ""));
        fb_buf_printf(&c, ",%d,%d,", (int)fbj_get_num(p, "windowsVirtualKeyCode", 0), (int)fbj_get_num(p, "modifiers", 0));
        if (fbj_get(p, "text")) fbl_js_string(&c, fbj_get_str(p, "text", "")); else fb_buf_puts(&c, "null");
        fb_buf_putc(&c, ',');
        const FbJson *cmds = fbj_get(p, "commands");
        if (cmds && cmds->type == FBJ_ARR && cmds->count && cmds->items[0]->type == FBJ_STR) fbl_js_string(&c, cmds->items[0]->str); else fb_buf_puts(&c, "null");
        fb_buf_putc(&c, ')');
        rc = run_simple(h, &c, result, err, errsz);
        fb_buf_free(&c);
    } else if (!strcmp(method, "Input.insertText")) {
        FbBuf c;
        fb_buf_init(&c);
        fb_buf_puts(&c, "insert(");
        fbl_js_string(&c, fbj_get_str(p, "text", ""));
        fb_buf_putc(&c, ')');
        rc = run_simple(h, &c, result, err, errsz);
        fb_buf_free(&c);
    } else if (!strcmp(method, "DOM.setFileInputFiles")) {
        int handle = parse_handle(fbj_get_str(p, "objectId", NULL));
        if (handle < 0 && fbj_get_num(p, "backendNodeId", 0) > 0) handle = 0;
        if (!h->set_files) { fb_set_err(err, errsz, "This browser engine cannot put files into a file field"); rc = 1; }
        else if (handle < 0) { fb_set_err(err, errsz, "No file field was given"); rc = 1; }
        else {
            rc = h->set_files(h->ud, handle, fbj_get_num(p, "backendNodeId", 0), fbj_get(p, "files"), err, errsz);
            if (!rc) result_with(result, "{}");
        }
    } else if (!strcmp(method, "Page.setInterceptFileChooserDialog")) {
        if (!h->chooser) { fb_set_err(err, errsz, "Not supported by this browser engine: %s", method); rc = 1; }
        else { rc = h->chooser(h->ud, fbj_get_bool(p, "enabled", 0), err, errsz); if (!rc) result_with(result, "{}"); }
    } else if (!strcmp(method, "Page.captureScreenshot")) {
        FbBuf b64;
        fb_buf_init(&b64);
        if (!h->screenshot) { fb_set_err(err, errsz, "This browser engine cannot take screenshots"); rc = 1; }
        else {
            rc = h->screenshot(h->ud, p, &b64, err, errsz);
            if (!rc) { fb_buf_puts(result, "{\"data\":\""); fb_buf_puts(result, fb_buf_cstr(&b64)); fb_buf_puts(result, "\"}"); }
        }
        fb_buf_free(&b64);
    } else if (!strcmp(method, "Network.clearBrowserCookies") || !strcmp(method, "Network.clearBrowserCache")) {
        if (!h->clear_data) { fb_set_err(err, errsz, "Not supported by this browser engine: %s", method); rc = 1; }
        else { rc = h->clear_data(h->ud, !strcmp(method, "Network.clearBrowserCookies"), !strcmp(method, "Network.clearBrowserCache"), err, errsz); if (!rc) result_with(result, "{}"); }
    } else if (!strcmp(method, "Emulation.setUserAgentOverride")) {
        if (!h->user_agent) { fb_set_err(err, errsz, "Not supported by this browser engine: %s", method); rc = 1; }
        else { rc = h->user_agent(h->ud, fbj_get_str(p, "userAgent", ""), err, errsz); if (!rc) result_with(result, "{}"); }
    } else if (!strcmp(method, "Page.enable") || !strcmp(method, "Page.disable") || !strcmp(method, "Runtime.enable") || !strcmp(method, "Network.enable")) {
        result_with(result, "{}");
    } else {
        fb_set_err(err, errsz, "Not supported by this browser engine: %s", method);
        rc = 1;
    }
    fbj_free(p);
    return rc;
}
