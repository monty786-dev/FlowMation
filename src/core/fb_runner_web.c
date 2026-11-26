/* Runner steps that drive the browser. Every page action is a call into the page runtime (window.__fb.exec);
   real mouse / keyboard input, file choosers and screenshots go through CDP. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "fb_runner_int.h"

#define NEED(var, call) const char *var = (call); if (!var) return R_ERR

/* ============================================================ CDP + runtime plumbing */

static int cdp(FbRunner *r, const char *method, const FbJson *params, FbJson **out)
{
    if (!r->be.cdp) return r_fail(r, "This host cannot control a browser");
    char *p = params ? fbj_to_string(params, 0) : fb_strdup("{}");
    FbBuf res;
    fb_buf_init(&res);
    char err[400] = "";
    int rc = r->be.cdp(r->be.ud, method, p, &res, err, sizeof err);
    free(p);
    if (rc) { fb_buf_free(&res); return r_fail(r, "%s", err[0] ? err : "Browser command failed"); }
    if (out) {
        *out = res.len ? fbj_parse(fb_buf_cstr(&res), res.len, err, sizeof err) : fbj_obj();
        if (!*out) { fb_buf_free(&res); return r_fail(r, "Bad answer from the browser: %s", err); }
    }
    fb_buf_free(&res);
    return R_OK;
}

static int transient(const char *msg)
{
    return strstr(msg, "context was destroyed") || strstr(msg, "Cannot find context") || strstr(msg, "navigat") ||
           strstr(msg, "Target closed") || strstr(msg, "Inspected target");
}

/* Calls window.__fb.exec(op, args). Returns R_OK with *out = the runtime's answer object ({ok:..}), owned by the caller.
   Returns R_ERR when the call itself failed (r->err is set; *transient_out says whether retrying makes sense). */
static int call_rt(FbRunner *r, const char *op, const FbJson *args, FbJson **out, int *transient_out)
{
    FbBuf e;
    fb_buf_init(&e);
    fb_buf_puts(&e, "window.__fb?window.__fb.exec(");
    fbj_write_escaped_string(&e, op, strlen(op));
    fb_buf_putc(&e, ',');
    char *a = args ? fbj_to_string(args, 0) : fb_strdup("{}");
    fb_buf_puts(&e, a);
    free(a);
    fb_buf_puts(&e, "):{ok:false,code:\"no_runtime\",error:\"The page runtime is not available on this page\"}");
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "expression", fbj_str(fb_buf_cstr(&e)));
    fbj_obj_set(p, "returnByValue", fbj_bool(1));
    fbj_obj_set(p, "awaitPromise", fbj_bool(1));
    fbj_obj_set(p, "userGesture", fbj_bool(1));
    fb_buf_free(&e);
    FbJson *res = NULL;
    int rc = cdp(r, "Runtime.evaluate", p, &res);
    fbj_free(p);
    if (transient_out) *transient_out = 0;
    if (rc) { if (transient_out) *transient_out = transient(r->err); return R_ERR; }
    const FbJson *ex = fbj_get(res, "exceptionDetails");
    if (ex) {
        const char *d = fbj_get_str(fbj_get(ex, "exception"), "description", fbj_get_str(ex, "text", "script error"));
        char m[600];
        snprintf(m, sizeof m, "%s", d);
        if (transient_out) *transient_out = transient(m);
        fbj_free(res);
        return r_fail(r, "Page script error: %s", m);
    }
    FbJson *val = fbj_get(fbj_get(res, "result"), "value");
    if (!val || val->type != FBJ_OBJ) {
        fbj_free(res);
        if (transient_out) *transient_out = 1;
        return r_fail(r, "The page returned no answer (it may be loading)");
    }
    FbJson *copy = fbj_clone(val);
    fbj_free(res);
    *out = copy;
    return R_OK;
}

/* One runtime call whose {ok:false} answer is an error. */
static int rt(FbRunner *r, const char *op, const FbJson *args, FbJson **out)
{
    FbJson *res = NULL;
    int tr = 0, tries = 0;
    for (;;) {
        int rc = call_rt(r, op, args, &res, &tr);
        if (rc == R_OK) break;
        if (!tr || ++tries > 20 || r->stop) return R_ERR;     /* the page was navigating: give it a moment and retry */
        if (r_sleep(r, 150)) return R_STOP;
    }
    if (!fbj_get_bool(res, "ok", 0)) {
        r_fail(r, "%s", fbj_get_str(res, "error", "The page reported an error"));
        fbj_free(res);
        return R_ERR;
    }
    if (out) *out = res; else fbj_free(res);
    return R_OK;
}

typedef int (*Pred)(const FbJson *res, void *ud);

/* Re-runs an op until pred(result) is true. R_ERR on timeout with the last problem in r->err. */
static int poll_rt(FbRunner *r, const char *op, const FbJson *args, int timeout_ms, Pred pred, void *ud, FbJson **out)
{
    uint64_t end = fb_now_ms() + (uint64_t)timeout_ms;
    char last[1100] = "";
    int wait = 40;
    for (;;) {
        FbJson *res = NULL;
        int tr = 0;
        r->err[0] = 0;
        if (call_rt(r, op, args, &res, &tr) == R_OK) {
            if (fbj_get_bool(res, "ok", 0) && (!pred || pred(res, ud))) { if (out) *out = res; else fbj_free(res); return R_OK; }
            snprintf(last, sizeof last, "%s", fbj_get_str(res, "error", fbj_get_bool(res, "ok", 0) ? "" : "The page reported an error"));
            const char *code = fbj_get_str(res, "code", "");
            if (!strcmp(code, "bad_selector") || !strcmp(code, "no_runtime")) { r_fail(r, "%s", last); fbj_free(res); return R_ERR; }
            fbj_free(res);
        } else snprintf(last, sizeof last, "%s", r->err);
        if (r->stop) return R_STOP;
        if (fb_now_ms() >= end) break;
        if (r_sleep(r, wait)) return R_STOP;
        if (wait < 250) wait += 30;
    }
    return r_fail(r, "%s", last[0] ? last : "Timed out");
}

/* args object with the (expanded) selector and its alternatives */
static FbJson *sel_args(FbRunner *r, const FbJson *s, const char *key)
{
    const char *sel = r_str(r, s, key, "");
    if (!sel) return NULL;
    if (!*sel) { r_fail(r, "No element selected (%s is empty)", key); return NULL; }
    FbJson *a = fbj_obj();
    fbj_obj_set(a, "selector", fbj_str(sel));
    const FbJson *alts = strcmp(key, "selector") == 0 ? fbj_get(s, "alts") : NULL;
    if (alts && alts->type == FBJ_ARR) {
        FbJson *al = fbj_arr();
        for (size_t i = 0; i < alts->count; i++) {
            if (alts->items[i]->type != FBJ_STR) continue;
            FbBuf b;
            fb_buf_init(&b);
            if (!fbe_expand(r->vars, alts->items[i]->str, &b, r->err, sizeof r->err)) fbj_arr_push(al, fbj_str(fb_buf_cstr(&b)));
            fb_buf_free(&b);
        }
        fbj_obj_set(a, "alts", al);
    }
    return a;
}

static const char *sel_name(const FbJson *args) { return fbj_get_str(args, "selector", ""); }

static int notfound(FbRunner *r, const FbJson *args, const char *why)
{
    return r_fail(r, "%s: %s", why && *why ? why : "Element not found", sel_name(args));
}

/* ============================================================ input: mouse / keyboard */

static int mouse_ev(FbRunner *r, const char *type, double x, double y, const char *button, int clicks)
{
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "type", fbj_str(type));
    fbj_obj_set(p, "x", fbj_num(x));
    fbj_obj_set(p, "y", fbj_num(y));
    fbj_obj_set(p, "button", fbj_str(!strcmp(type, "mouseMoved") ? "none" : button));
    fbj_obj_set(p, "buttons", fbj_num(!strcmp(type, "mousePressed") ? (!strcmp(button, "right") ? 2 : !strcmp(button, "middle") ? 4 : 1) : 0));
    fbj_obj_set(p, "clickCount", fbj_num(clicks));
    int rc = cdp(r, "Input.dispatchMouseEvent", p, NULL);
    fbj_free(p);
    return rc;
}

typedef struct { int force; } ActPred;

/* element is present, visible, enabled and (unless force) not covered by something else */
static int actionable(const FbJson *res, void *ud)
{
    const ActPred *ap = (const ActPred *)ud;
    if (!fbj_get_bool(res, "visible", 0)) return 0;
    if (ap && ap->force) return 1;
    if (fbj_get_bool(res, "disabled", 0)) return 0;
    if (fbj_get(res, "covered") && fbj_get(res, "covered")->type == FBJ_OBJ) return 0;
    return 1;
}

static int visible_only(const FbJson *res, void *ud) { (void)ud; return fbj_get_bool(res, "visible", 0); }

/* waits until the element can be used; fills pt with its centre */
static int wait_rect(FbRunner *r, const FbJson *s, const FbJson *args, int force, double *x, double *y, FbJson **info)
{
    ActPred ap = { force };
    FbJson *res = NULL;
    int rc = poll_rt(r, "rect", args, r_timeout(r, s), actionable, &ap, &res);
    if (rc != R_OK) {
        if (rc == R_ERR) {
            char m[1100];
            snprintf(m, sizeof m, "%s", r->err);
            if (strstr(m, "not found") || strstr(m, "No element")) r_fail(r, "Element not found: %s", sel_name(args));
            else r_fail(r, "%s (%s)", m, sel_name(args));
        }
        return rc;
    }
    *x = fbj_get_num(res, "x", 0);
    *y = fbj_get_num(res, "y", 0);
    if (info) *info = res; else fbj_free(res);
    return R_OK;
}

/* after an action: if the page started navigating, wait until it finished */
static int settle(FbRunner *r, const FbJson *s, int started_ms)
{
    if (!r->be.wait_event) return R_OK;
    const char *after = fbj_get_str(s, "after", "auto");
    if (!strcmp(after, "none")) return R_OK;
    int rc = r->be.wait_event(r->be.ud, "Navigation.started", !strcmp(after, "navigation") ? r_timeout(r, s) : started_ms, NULL);
    if (rc == 0) {
        rc = r->be.wait_event(r->be.ud, "Navigation.completed", r_timeout(r, s), NULL);
        if (rc == 1) return r_fail(r, "The page did not finish loading in time");
    } else if (rc == 1 && !strcmp(after, "navigation")) return r_fail(r, "No page navigation happened after the click");
    return R_OK;
}

static void drain_nav(FbRunner *r)
{
    if (!r->be.wait_event) return;
    r->be.wait_event(r->be.ud, "Navigation.started", 0, NULL);
    r->be.wait_event(r->be.ud, "Navigation.completed", 0, NULL);
}

static int click_at(FbRunner *r, double x, double y, const char *button, int count)
{
    int rc = mouse_ev(r, "mouseMoved", x, y, button, 0);
    if (rc) return rc;
    for (int i = 1; i <= count; i++) {
        if ((rc = mouse_ev(r, "mousePressed", x, y, button, i))) return rc;
        if ((rc = mouse_ev(r, "mouseReleased", x, y, button, i))) return rc;
    }
    return R_OK;
}

static int do_click(FbRunner *r, const FbJson *s, const FbJson *args)
{
    const char *button = r_str(r, s, "button", "left");
    const char *method = r_str(r, s, "method", "mouse");
    if (!button || !method) return R_ERR;
    int count = (int)r_num(r, s, "count", 1);
    if (count < 1) count = 1;
    if (count > 3) count = 3;
    int force = r_bool(r, s, "force", 0);
    drain_nav(r);
    double x, y;
    int rc;
    if (!strcmp(method, "js")) {
        FbJson *a = fbj_clone(args);
        fbj_obj_set(a, "button", fbj_str(button));
        fbj_obj_set(a, "count", fbj_num(count));
        FbJson *res = NULL;
        poll_rt(r, "rect", args, r_timeout(r, s), visible_only, NULL, NULL);       /* wait for it to exist */
        rc = rt(r, "click_js", a, &res);
        fbj_free(a);
        fbj_free(res);
        if (rc != R_OK) return rc == R_ERR ? notfound(r, args, r->err) : rc;
    } else {
        if ((rc = wait_rect(r, s, args, force, &x, &y, NULL)) != R_OK) return rc;
        if ((rc = click_at(r, x, y, button, count)) != R_OK) return rc;
    }
    return settle(r, s, 350);
}

static int s_click(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    int rc = do_click(r, s, args);
    fbj_free(args);
    return rc;
}

static int s_hover(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    double x, y;
    int rc = wait_rect(r, s, args, 1, &x, &y, NULL);
    if (rc == R_OK) rc = mouse_ev(r, "mouseMoved", x, y, "none", 0);
    fbj_free(args);
    return rc;
}

/* ---- key names -> CDP ---- */

typedef struct { const char *name; const char *key; const char *code; int vk; const char *text; } KeyDef;
static const KeyDef KEYS[] = {
    { "enter", "Enter", "Enter", 13, "\r" }, { "return", "Enter", "Enter", 13, "\r" }, { "tab", "Tab", "Tab", 9, NULL },
    { "escape", "Escape", "Escape", 27, NULL }, { "esc", "Escape", "Escape", 27, NULL }, { "backspace", "Backspace", "Backspace", 8, NULL },
    { "delete", "Delete", "Delete", 46, NULL }, { "del", "Delete", "Delete", 46, NULL }, { "space", " ", "Space", 32, " " },
    { "arrowup", "ArrowUp", "ArrowUp", 38, NULL }, { "up", "ArrowUp", "ArrowUp", 38, NULL },
    { "arrowdown", "ArrowDown", "ArrowDown", 40, NULL }, { "down", "ArrowDown", "ArrowDown", 40, NULL },
    { "arrowleft", "ArrowLeft", "ArrowLeft", 37, NULL }, { "left", "ArrowLeft", "ArrowLeft", 37, NULL },
    { "arrowright", "ArrowRight", "ArrowRight", 39, NULL }, { "right", "ArrowRight", "ArrowRight", 39, NULL },
    { "home", "Home", "Home", 36, NULL }, { "end", "End", "End", 35, NULL },
    { "pageup", "PageUp", "PageUp", 33, NULL }, { "pagedown", "PageDown", "PageDown", 34, NULL },
    { "insert", "Insert", "Insert", 45, NULL },
    { "f1", "F1", "F1", 112, NULL }, { "f2", "F2", "F2", 113, NULL }, { "f3", "F3", "F3", 114, NULL }, { "f4", "F4", "F4", 115, NULL },
    { "f5", "F5", "F5", 116, NULL }, { "f6", "F6", "F6", 117, NULL }, { "f7", "F7", "F7", 118, NULL }, { "f8", "F8", "F8", 119, NULL },
    { "f9", "F9", "F9", 120, NULL }, { "f10", "F10", "F10", 121, NULL }, { "f11", "F11", "F11", 122, NULL }, { "f12", "F12", "F12", 123, NULL },
    { NULL, NULL, NULL, 0, NULL }
};

static int key_event(FbRunner *r, const char *type, const char *key, const char *code, int vk, const char *text, int mods, const char *cmd)
{
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "type", fbj_str(type));
    fbj_obj_set(p, "key", fbj_str(key));
    fbj_obj_set(p, "code", fbj_str(code));
    fbj_obj_set(p, "windowsVirtualKeyCode", fbj_num(vk));
    fbj_obj_set(p, "nativeVirtualKeyCode", fbj_num(vk));
    fbj_obj_set(p, "modifiers", fbj_num(mods));
    if (text && !strcmp(type, "keyDown")) { fbj_obj_set(p, "text", fbj_str(text)); fbj_obj_set(p, "unmodifiedText", fbj_str(text)); }
    if (cmd) { FbJson *c = fbj_arr(); fbj_arr_push(c, fbj_str(cmd)); fbj_obj_set(p, "commands", c); }
    int rc = cdp(r, "Input.dispatchKeyEvent", p, NULL);
    fbj_free(p);
    return rc;
}

/* "Ctrl+Shift+A", "Enter", "a" */
static int press_combo(FbRunner *r, const char *combo)
{
    char buf[80];
    snprintf(buf, sizeof buf, "%s", combo);
    int mods = 0;
    char *last = buf;
    for (char *p = buf; *p; p++) {
        if (*p == '+' && p > buf && p[1]) {
            *p = 0;
            if (!fb_stricmp(last, "ctrl") || !fb_stricmp(last, "control")) mods |= 2;
            else if (!fb_stricmp(last, "alt")) mods |= 1;
            else if (!fb_stricmp(last, "shift")) mods |= 8;
            else if (!fb_stricmp(last, "meta") || !fb_stricmp(last, "cmd") || !fb_stricmp(last, "win")) mods |= 4;
            else return r_fail(r, "Unknown modifier '%s' in key '%s'", last, combo);
            last = p + 1;
        }
    }
    const KeyDef *kd = NULL;
    for (const KeyDef *k = KEYS; k->name; k++) if (!fb_stricmp(k->name, last)) { kd = k; break; }
    char kc[2] = { 0, 0 }, cc[16];
    const char *key, *code, *text = NULL, *cmd = NULL;
    int vk;
    if (kd) { key = kd->key; code = kd->code; vk = kd->vk; text = kd->text; }
    else if (strlen(last) == 1 && isalnum((unsigned char)last[0])) {
        char ch = last[0];
        kc[0] = (mods & 8) ? (char)toupper((unsigned char)ch) : (char)tolower((unsigned char)ch);
        key = kc;
        snprintf(cc, sizeof cc, isdigit((unsigned char)ch) ? "Digit%c" : "Key%c", toupper((unsigned char)ch));
        code = cc;
        vk = toupper((unsigned char)ch);
        text = (mods & ~8) ? NULL : kc;                               /* Ctrl/Alt combos produce no text */
        if (mods & 2) {
            char l = (char)tolower((unsigned char)ch);
            cmd = l == 'a' ? "selectAll" : l == 'c' ? "copy" : l == 'v' ? "paste" : l == 'x' ? "cut" : l == 'z' ? "undo" : l == 'y' ? "redo" : NULL;
        }
    } else return r_fail(r, "Unknown key '%s'. Examples: Enter, Tab, Escape, ArrowDown, PageDown, Ctrl+A, Shift+Tab, F5", last);
    int rc = key_event(r, text ? "keyDown" : "rawKeyDown", key, code, vk, text, mods, cmd);
    if (rc) return rc;
    return key_event(r, "keyUp", key, code, vk, NULL, mods, NULL);
}

static int s_press_key(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(key, r_str(r, s, "key", ""));
    long times = r_num(r, s, "times", 1);
    if (times < 1) times = 1;
    if (times > 500) times = 500;
    if (fbj_get(s, "selector") && *fbj_get_str(s, "selector", "")) {
        FbJson *args = sel_args(r, s, "selector");
        if (!args) return R_ERR;
        poll_rt(r, "rect", args, r_timeout(r, s), visible_only, NULL, NULL);
        int rc = rt(r, "focus", args, NULL);
        fbj_free(args);
        if (rc != R_OK) return rc;
    }
    drain_nav(r);
    for (long i = 0; i < times; i++) {
        int rc = press_combo(r, key);
        if (rc) return rc;
        if (times > 1 && r_sleep(r, 30)) return R_STOP;
    }
    return settle(r, s, 300);
}

/* ============================================================ filling */

static int insert_text(FbRunner *r, const char *text)
{
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "text", fbj_str(text));
    int rc = cdp(r, "Input.insertText", p, NULL);
    fbj_free(p);
    return rc;
}

static int type_chars(FbRunner *r, const char *text, int delay_ms)
{
    if (delay_ms <= 0) return insert_text(r, text);
    for (const char *p = text; *p;) {
        int l = 1;
        unsigned char c = (unsigned char)*p;
        l = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        char ch[5] = { 0 };
        memcpy(ch, p, (size_t)l);
        if (*p == '\n') { if (press_combo(r, "Enter")) return R_ERR; }
        else {
            FbJson *e = fbj_obj();
            fbj_obj_set(e, "type", fbj_str("keyDown"));
            fbj_obj_set(e, "text", fbj_str(ch));
            fbj_obj_set(e, "key", fbj_str(ch));
            int rc = cdp(r, "Input.dispatchKeyEvent", e, NULL);
            fbj_obj_set(e, "type", fbj_str("keyUp"));
            fbj_obj_del(e, "text");
            if (!rc) rc = cdp(r, "Input.dispatchKeyEvent", e, NULL);
            fbj_free(e);
            if (rc) return rc;
        }
        p += l;
        if (r_sleep(r, delay_ms)) return R_STOP;
    }
    return R_OK;
}

static int s_fill(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(mode, r_str(r, s, "mode", "auto"));
    FbJson *val = r_value_param(r, s, "value", "text");
    if (!val) { fbj_free(args); return R_ERR; }
    int rc;
    if (!strcmp(mode, "type")) {
        FbJson *res = NULL;
        poll_rt(r, "rect", args, r_timeout(r, s), visible_only, NULL, NULL);
        FbJson *a = fbj_clone(args);
        fbj_obj_set(a, "clear", fbj_bool(1));
        rc = rt(r, "prepare_type", a, &res);
        fbj_free(a);
        fbj_free(res);
        if (rc == R_OK) rc = type_chars(r, val->str, 25);
    } else {
        FbJson *a = fbj_clone(args);
        fbj_obj_set(a, "value", fbj_clone(val));
        FbJson *res = NULL;
        FbJson *w = NULL;
        rc = poll_rt(r, "exists", args, r_timeout(r, s), NULL, NULL, &w);                 /* wait for the field to exist */
        fbj_free(w);
        if (rc == R_OK) rc = poll_rt(r, "fill", a, 0, NULL, NULL, &res);
        fbj_free(a);
        fbj_free(res);
    }
    if (rc == R_ERR && strstr(r->err, "Timed out")) rc = notfound(r, args, "Field not found");
    fbj_free(val);
    fbj_free(args);
    return rc;
}

static int s_type_text(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(text, r_str(r, s, "text", ""));
    FbJson *res = NULL;
    poll_rt(r, "rect", args, r_timeout(r, s), visible_only, NULL, NULL);
    FbJson *a = fbj_clone(args);
    fbj_obj_set(a, "clear", fbj_bool(r_bool(r, s, "clear", 1)));
    int rc = rt(r, "prepare_type", a, &res);
    fbj_free(a);
    fbj_free(res);
    if (rc == R_OK) rc = type_chars(r, text, (int)r_num(r, s, "delay_ms", 0));
    fbj_free(args);
    return rc;
}

/* generic "call op with selector + a few extra params" step */
static int simple_op(FbRunner *r, const FbJson *s, const char *op, const char *const *extra)
{
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    for (int i = 0; extra && extra[i]; i++) {
        const char *v = r_str(r, s, extra[i], "");
        if (!v) { fbj_free(args); return R_ERR; }
        fbj_obj_set(args, extra[i], fbj_str(v));
    }
    FbJson *w = NULL;
    int rc = poll_rt(r, "exists", args, r_timeout(r, s), NULL, NULL, &w);
    fbj_free(w);
    if (rc == R_ERR && strstr(r->err, "not found")) rc = notfound(r, args, "Element not found");
    if (rc == R_OK) rc = rt(r, op, args, NULL);
    fbj_free(args);
    return rc;
}

static int s_select_option(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    static const char *const ex[] = { "value", "by", NULL };
    return simple_op(r, s, "select_option", ex);
}

static int s_check(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    static const char *const ex[] = { "state", NULL };
    return simple_op(r, s, "check", ex);
}

static int s_set_slider(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    static const char *const ex[] = { "value", NULL };
    return simple_op(r, s, "set_slider", ex);
}

static int s_focus(FbRunner *r, const FbJson *s, const char *path) { (void)path; return simple_op(r, s, "focus", NULL); }

static int s_submit_form(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    drain_nav(r);
    int rc = simple_op(r, s, "submit_form", NULL);
    return rc == R_OK ? settle(r, s, 600) : rc;
}

/* ============================================================ drag & drop, upload */

static int s_drag_drop(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *from = sel_args(r, s, "selector"), *to = sel_args(r, s, "target");
    if (!from || !to) { fbj_free(from); fbj_free(to); return R_ERR; }
    NEED(method, r_str(r, s, "method", "mouse"));
    int rc;
    if (!strcmp(method, "html5")) {
        FbJson *a = fbj_obj();
        fbj_obj_set(a, "selector", fbj_clone(fbj_get(from, "selector")));
        fbj_obj_set(a, "target", fbj_clone(fbj_get(to, "selector")));
        FbJson *w = NULL;
        rc = poll_rt(r, "exists", from, r_timeout(r, s), NULL, NULL, &w);
        fbj_free(w);
        if (rc == R_OK) rc = rt(r, "html5_drag", a, NULL);
        fbj_free(a);
    } else {
        double x1, y1, x2, y2;
        rc = wait_rect(r, s, from, 1, &x1, &y1, NULL);
        if (rc == R_OK) rc = wait_rect(r, s, to, 1, &x2, &y2, NULL);
        /* the target may have moved the page: re-read the source after scrolling to the target is not needed for
           typical pages; keep the straightforward press - move - release sequence */
        if (rc == R_OK) rc = mouse_ev(r, "mouseMoved", x1, y1, "none", 0);
        if (rc == R_OK) rc = mouse_ev(r, "mousePressed", x1, y1, "left", 1);
        for (int i = 1; rc == R_OK && i <= 8; i++) {
            rc = mouse_ev(r, "mouseMoved", x1 + (x2 - x1) * i / 8.0, y1 + (y2 - y1) * i / 8.0, "left", 0);
            if (r_sleep(r, 15)) { rc = R_STOP; break; }
        }
        if (rc == R_OK) rc = mouse_ev(r, "mouseReleased", x2, y2, "left", 1);
    }
    fbj_free(from);
    fbj_free(to);
    return rc;
}

/* file list from a param: array, or text split on newlines / '|' ; every entry made absolute and checked */
static int collect_files(FbRunner *r, const FbJson *s, FbJson *out_paths)
{
    const FbJson *v = fbj_get(s, "files");
    if (!v) return r_fail(r, "No file given");
    FbBuf all;
    fb_buf_init(&all);
    if (v->type == FBJ_ARR) {
        for (size_t i = 0; i < v->count; i++) {
            FbBuf b;
            fb_buf_init(&b);
            const FbJson *it = v->items[i];
            if (it->type == FBJ_STR) { if (fbe_expand(r->vars, it->str, &b, r->err, sizeof r->err)) { fb_buf_free(&b); fb_buf_free(&all); return R_ERR; } }
            else fbj_to_text(it, &b);
            fb_buf_puts(&all, fb_buf_cstr(&b));
            fb_buf_putc(&all, '\n');
            fb_buf_free(&b);
        }
    } else {
        const char *t = r_str(r, s, "files", "");
        if (!t) { fb_buf_free(&all); return R_ERR; }
        fb_buf_puts(&all, t);
    }
    /* a template may also have produced a JSON-ish list: ["a","b"] -> treat quotes/brackets as noise only at the ends */
    char *txt = fb_buf_detach(&all);
    int rc = R_OK;
    for (char *p = txt; p && *p && rc == R_OK;) {
        char *e = p;
        while (*e && *e != '\n' && *e != '|') e++;
        char sv = *e;
        *e = 0;
        char *t = fb_trim(p);
        size_t n = strlen(t);
        if (n >= 2 && t[0] == '"' && t[n - 1] == '"') { t[n - 1] = 0; t++; }
        if (*t) {
            const char *abs = r_abs_path(r, t);
            if (!fb_file_exists(abs)) rc = r_fail(r, "File not found: %s", abs);
            else fbj_arr_push(out_paths, fbj_str(abs));
        }
        if (!sv) break;
        p = e + 1;
    }
    free(txt);
    if (rc == R_OK && !out_paths->count) rc = r_fail(r, "No file given");
    return rc;
}

/* evaluates window.__fb.handle(kind, args) and returns the objectId of the element (NULL = none) */
static char *handle_object(FbRunner *r, const char *kind, const FbJson *args)
{
    FbBuf e;
    fb_buf_init(&e);
    fb_buf_puts(&e, "window.__fb?window.__fb.handle(");
    fbj_write_escaped_string(&e, kind, strlen(kind));
    fb_buf_putc(&e, ',');
    char *a = fbj_to_string(args, 0);
    fb_buf_puts(&e, a);
    free(a);
    fb_buf_puts(&e, "):null");
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "expression", fbj_str(fb_buf_cstr(&e)));
    fbj_obj_set(p, "returnByValue", fbj_bool(0));
    fbj_obj_set(p, "userGesture", fbj_bool(1));
    fb_buf_free(&e);
    FbJson *res = NULL;
    int rc = cdp(r, "Runtime.evaluate", p, &res);
    fbj_free(p);
    if (rc) return NULL;
    const char *oid = fbj_get_str(fbj_get(res, "result"), "objectId", NULL);
    char *out = oid ? fb_strdup(oid) : NULL;
    fbj_free(res);
    return out;
}

static int set_files(FbRunner *r, const FbJson *files, const char *object_id, double backend_node)
{
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "files", fbj_clone(files));
    if (object_id) fbj_obj_set(p, "objectId", fbj_str(object_id));
    else fbj_obj_set(p, "backendNodeId", fbj_num(backend_node));
    int rc = cdp(r, "DOM.setFileInputFiles", p, NULL);
    fbj_free(p);
    return rc;
}

static const char *mime_of(const char *name)
{
    static const struct { const char *ext, *mime; } M[] = {
        { ".png", "image/png" }, { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".gif", "image/gif" }, { ".webp", "image/webp" },
        { ".pdf", "application/pdf" }, { ".txt", "text/plain" }, { ".csv", "text/csv" }, { ".json", "application/json" },
        { ".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
        { ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
        { ".zip", "application/zip" }, { ".mp4", "video/mp4" }, { ".mp3", "audio/mpeg" }, { NULL, NULL } };
    for (int i = 0; M[i].ext; i++) if (fb_ends_with(name, M[i].ext) || fb_stricmp(name + (strlen(name) > strlen(M[i].ext) ? strlen(name) - strlen(M[i].ext) : 0), M[i].ext) == 0) return M[i].mime;
    return "application/octet-stream";
}

static int upload_drop(FbRunner *r, const FbJson *args, const FbJson *paths)
{
    FbJson *list = fbj_arr();
    size_t total = 0;
    for (size_t i = 0; i < paths->count; i++) {
        const char *p = paths->items[i]->str;
        FbBuf data;
        fb_buf_init(&data);
        if (fb_read_file(p, &data)) { fb_buf_free(&data); fbj_free(list); return r_fail(r, "Cannot read %s", p); }
        total += data.len;
        if (total > 25u * 1024 * 1024) { fb_buf_free(&data); fbj_free(list); return r_fail(r, "Files are too big for drag-and-drop upload (25 MB limit). Use a normal file input or the file-chooser mode"); }
        FbBuf b64;
        fb_buf_init(&b64);
        fb_base64_encode(data.data, data.len, &b64);
        const char *name = p;
        for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') name = q + 1;
        FbJson *f = fbj_obj();
        fbj_obj_set(f, "name", fbj_str(name));
        fbj_obj_set(f, "type", fbj_str(mime_of(name)));
        fbj_obj_set(f, "b64", fbj_str(fb_buf_cstr(&b64)));
        fbj_arr_push(list, f);
        fb_buf_free(&b64);
        fb_buf_free(&data);
    }
    FbJson *a = fbj_clone(args);
    fbj_obj_set(a, "files", list);
    int rc = rt(r, "drop_files", a, NULL);
    fbj_free(a);
    return rc;
}

static int s_upload_file(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(mode, r_str(r, s, "mode", "auto"));
    FbJson *paths = fbj_arr();
    int rc = collect_files(r, s, paths);
    if (rc != R_OK) { fbj_free(args); fbj_free(paths); return rc; }

    FbJson *w = NULL;
    rc = poll_rt(r, "exists", args, r_timeout(r, s), NULL, NULL, &w);
    fbj_free(w);
    if (rc != R_OK) { rc = notfound(r, args, "Upload field not found"); goto out; }
    FbJson *kind = NULL;
    if ((rc = rt(r, "target_kind", args, &kind)) != R_OK) goto out;
    char kbuf[40];
    snprintf(kbuf, sizeof kbuf, "%s", fbj_get_str(kind, "kind", "other"));      /* the answer is freed below */
    const char *k = kbuf;
    int multiple = fbj_get_bool(kind, "multiple", 0);
    if (paths->count > 1 && !multiple && (!strcmp(k, "file_input") || !strcmp(k, "has_file_input")) && strcmp(mode, "drop") != 0)
        r_log(r, "warn", "This upload field accepts only one file; the browser will use the first of %zu", paths->count);
    fbj_free(kind);

    int try_input = !strcmp(mode, "auto") || !strcmp(mode, "input");
    int try_dialog = !strcmp(mode, "auto") || !strcmp(mode, "dialog");
    int try_drop = !strcmp(mode, "auto") || !strcmp(mode, "drop");
    rc = R_ERR;
    r->err[0] = 0;

    if (try_input) {
        char *oid = handle_object(r, "file_input", args);                          /* the element's own input */
        if (!oid && !strcmp(mode, "input")) oid = handle_object(r, "file_input_near", args);
        if (oid) {
            rc = set_files(r, paths, oid, 0);
            free(oid);
            if (rc == R_OK) { r_log(r, "info", "Uploaded %zu file(s) into the file field", paths->count); goto out; }
        }
        if (!strcmp(mode, "input") && rc != R_OK) { if (!r->err[0]) r_fail(r, "No file input found at %s", sel_name(args)); goto out; }
    }
    if (try_dialog) {
        FbJson *on = fbj_obj();
        fbj_obj_set(on, "enabled", fbj_bool(1));
        r->err[0] = 0;
        int ic = cdp(r, "Page.setInterceptFileChooserDialog", on, NULL);
        fbj_free(on);
        if (ic == R_OK && r->be.wait_event) {
            r->be.wait_event(r->be.ud, "Page.fileChooserOpened", 0, NULL);                  /* forget older choosers */
            double x, y;
            int cr = wait_rect(r, s, args, 1, &x, &y, NULL);
            if (cr == R_OK) cr = click_at(r, x, y, "left", 1);
            FbBuf ev;
            fb_buf_init(&ev);
            int is_button = !strcmp(k, "button");
            int got = cr == R_OK ? r->be.wait_event(r->be.ud, "Page.fileChooserOpened", is_button || !strcmp(mode, "dialog") ? 4000 : 1500, &ev) : 1;
            FbJson *off = fbj_obj();
            fbj_obj_set(off, "enabled", fbj_bool(0));
            cdp(r, "Page.setInterceptFileChooserDialog", off, NULL);
            fbj_free(off);
            if (got == 0) {
                FbJson *evj = fbj_parse(fb_buf_cstr(&ev), ev.len, r->err, sizeof r->err);
                double node = fbj_get_num(evj, "backendNodeId", 0);
                fbj_free(evj);
                fb_buf_free(&ev);
                if (node > 0) {
                    rc = set_files(r, paths, NULL, node);
                    if (rc == R_OK) { r_log(r, "info", "Uploaded %zu file(s) through the file chooser", paths->count); goto out; }
                }
            } else fb_buf_free(&ev);
            if (cr != R_OK && !strcmp(mode, "dialog")) { rc = cr; goto out; }
        }
        if (!strcmp(mode, "dialog")) { rc = r_fail(r, "The element did not open a file chooser"); goto out; }
    }
    if (!strcmp(mode, "auto")) {                                                      /* nothing opened a chooser: a nearby input is the best guess */
        char *oid = handle_object(r, "file_input_near", args);
        if (oid) {
            r->err[0] = 0;
            rc = set_files(r, paths, oid, 0);
            free(oid);
            if (rc == R_OK) { r_log(r, "info", "Uploaded %zu file(s) into the file field next to the element", paths->count); goto out; }
        }
    }
    if (try_drop) {
        r->err[0] = 0;
        rc = upload_drop(r, args, paths);
        if (rc == R_OK) r_log(r, "info", "Dropped %zu file(s) on the element", paths->count);
        else if (!strcmp(mode, "auto")) r_fail(r, "Could not upload: this element has no file field, opens no file chooser and did not accept a drop (%s)", r->err);
    }
out:
    fbj_free(args);
    fbj_free(paths);
    return rc;
}

/* ============================================================ navigation & tabs */

static int s_navigate(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(url, r_str(r, s, "url", ""));
    if (!*url) return r_fail(r, "No address given");
    char full[2048];
    if (fb_stricmp(url, "javascript:") == 0 || !strncmp(url, "javascript:", 11)) return r_fail(r, "javascript: addresses are not allowed; use the 'Run JavaScript' step");
    if (strstr(url, "://") || !strncmp(url, "about:", 6) || !strncmp(url, "data:", 5) || !strncmp(url, "file:", 5)) snprintf(full, sizeof full, "%s", url);
    else snprintf(full, sizeof full, "https://%s", url);
    if (!r->be.navigate) return r_fail(r, "This host cannot navigate");
    char err[400] = "";
    if (r->be.navigate(r->be.ud, full, r_bool(r, s, "wait", 1), r_timeout(r, s), err, sizeof err)) return r_fail(r, "%s", err[0] ? err : "Navigation failed");
    return R_OK;
}

static int tab_op(FbRunner *r, const char *op, const char *arg)
{
    if (!r->be.tab) return r_fail(r, "This host has no tabs");
    char err[400] = "";
    if (r->be.tab(r->be.ud, op, arg, err, sizeof err)) return r_fail(r, "%s", err[0] ? err : "Tab command failed");
    return R_OK;
}

static int s_new_tab(FbRunner *r, const FbJson *s, const char *path) { (void)path; NEED(u, r_str(r, s, "url", "")); return tab_op(r, "new", u); }
static int s_switch_tab(FbRunner *r, const FbJson *s, const char *path) { (void)path; NEED(t, r_str(r, s, "tab", "")); return tab_op(r, "switch", t); }
static int s_close_tab(FbRunner *r, const FbJson *s, const char *path) { (void)s; (void)path; return tab_op(r, "close", ""); }
static int s_back(FbRunner *r, const FbJson *s, const char *path) { (void)s; (void)path; drain_nav(r); int rc = tab_op(r, "back", ""); return rc ? rc : settle(r, s, 1200); }
static int s_forward(FbRunner *r, const FbJson *s, const char *path) { (void)s; (void)path; drain_nav(r); int rc = tab_op(r, "forward", ""); return rc ? rc : settle(r, s, 1200); }
static int s_reload(FbRunner *r, const FbJson *s, const char *path) { (void)s; (void)path; drain_nav(r); int rc = tab_op(r, "reload", ""); return rc ? rc : settle(r, s, 1200); }

static int s_handle_dialog(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(a, r_str(r, s, "action", "accept"));
    if (r->be.option) r->be.option(r->be.ud, "dialogs", a);
    return R_OK;
}

static int s_set_user_agent(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(ua, r_str(r, s, "user_agent", ""));
    if (r->be.option) r->be.option(r->be.ud, "user_agent", ua);
    return R_OK;
}

static int s_clear_browsing_data(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(what, r_str(r, s, "what", "both"));
    int rc = R_OK;
    if (strcmp(what, "cache")) rc = cdp(r, "Network.clearBrowserCookies", NULL, NULL);
    if (rc == R_OK && strcmp(what, "cookies")) rc = cdp(r, "Network.clearBrowserCache", NULL, NULL);
    return rc;
}

static int s_wait_navigation(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    if (!r->be.wait_event) return R_OK;
    int rc = r->be.wait_event(r->be.ud, "Navigation.completed", r_timeout(r, s), NULL);
    return rc == 0 ? R_OK : r_fail(r, "No page finished loading within %d ms", r_timeout(r, s));
}

/* ============================================================ scrolling & running JS */

static int s_scroll(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(to, r_str(r, s, "to", "bottom"));
    FbJson *a = fbj_obj();
    fbj_obj_set(a, "to", fbj_str(to));
    fbj_obj_set(a, "amount", fbj_num((double)r_num(r, s, "amount", 500)));
    if (!strcmp(to, "element")) {
        FbJson *sa = sel_args(r, s, "selector");
        if (!sa) { fbj_free(a); return R_ERR; }
        fbj_obj_set(a, "selector", fbj_clone(fbj_get(sa, "selector")));
        fbj_free(sa);
    }
    int rc = rt(r, "scroll", a, NULL);
    fbj_free(a);
    return rc;
}

static int run_js_eval(FbRunner *r, const char *code, FbJson **value)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        FbBuf e;
        fb_buf_init(&e);
        if (attempt == 0) fb_buf_puts(&e, code);
        else { fb_buf_puts(&e, "(async function(){\n"); fb_buf_puts(&e, code); fb_buf_puts(&e, "\n})()"); }
        FbJson *p = fbj_obj();
        fbj_obj_set(p, "expression", fbj_str(fb_buf_cstr(&e)));
        fbj_obj_set(p, "returnByValue", fbj_bool(1));
        fbj_obj_set(p, "awaitPromise", fbj_bool(1));
        fbj_obj_set(p, "userGesture", fbj_bool(1));
        fb_buf_free(&e);
        FbJson *res = NULL;
        int rc = cdp(r, "Runtime.evaluate", p, &res);
        fbj_free(p);
        if (rc) return rc;
        const FbJson *ex = fbj_get(res, "exceptionDetails");
        if (ex) {
            const char *d = fbj_get_str(fbj_get(ex, "exception"), "description", fbj_get_str(ex, "text", "script error"));
            if (attempt == 0 && (strstr(d, "Illegal return") || strstr(d, "await is only valid"))) { fbj_free(res); continue; }
            char m[800];
            snprintf(m, sizeof m, "%s", d);
            fbj_free(res);
            return r_fail(r, "JavaScript error: %s", m);
        }
        FbJson *v = fbj_get(fbj_get(res, "result"), "value");
        *value = v ? fbj_clone(v) : fbj_null();
        fbj_free(res);
        return R_OK;
    }
    return r_fail(r, "JavaScript could not be run");
}

static int s_run_js(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    /* the code is raw: {{ }} in it belongs to JavaScript, only {{= }} style expansion is NOT applied */
    const char *code = fbj_get_str(s, "code", "");
    if (!*code) return r_fail(r, "No JavaScript to run");
    FbJson *v = NULL;
    int rc = run_js_eval(r, code, &v);
    if (rc != R_OK) return rc;
    const char *rv = fbj_get_str(s, "result_var", "");
    if (*rv) return r_set_var(r, rv, v);
    fbj_free(v);
    return R_OK;
}

/* ============================================================ reading data */

static int s_get_text(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(what, r_str(r, s, "what", "text"));
    NEED(rvar, r_str(r, s, "result_var", ""));
    fbj_obj_set(args, "what", fbj_str(what));
    FbJson *res = NULL;
    int rc = poll_rt(r, "get", args, r_timeout(r, s), NULL, NULL, &res);
    if (rc == R_ERR && strstr(r->err, "not found")) rc = notfound(r, args, "Element not found");
    if (rc == R_OK) { FbJson *v = fbj_get(res, "value"); rc = r_set_var(r, rvar, v ? fbj_clone(v) : fbj_str("")); }
    fbj_free(res);
    fbj_free(args);
    return rc;
}

static int s_get_attribute(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(attr, r_str(r, s, "attribute", ""));
    NEED(rvar, r_str(r, s, "result_var", ""));
    fbj_obj_set(args, "attribute", fbj_str(attr));
    FbJson *res = NULL;
    int rc = poll_rt(r, "attr", args, r_timeout(r, s), NULL, NULL, &res);
    if (rc == R_ERR && strstr(r->err, "not found")) rc = notfound(r, args, "Element not found");
    if (rc == R_OK) { FbJson *v = fbj_get(res, "value"); rc = r_set_var(r, rvar, v && v->type != FBJ_NULL ? fbj_clone(v) : fbj_str("")); }
    fbj_free(res);
    fbj_free(args);
    return rc;
}

static int s_element_exists(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(rvar, r_str(r, s, "result_var", ""));
    fbj_obj_set(args, "visible_only", fbj_bool(r_bool(r, s, "visible_only", 0)));
    fbj_obj_set(args, "optional", fbj_bool(1));
    FbJson *res = NULL;
    int rc = rt(r, "exists", args, &res);
    if (rc == R_OK) rc = r_set_var(r, rvar, fbj_bool(fbj_get_bool(res, "exists", 0)));
    fbj_free(res);
    fbj_free(args);
    return rc;
}

static int s_count_elements(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(rvar, r_str(r, s, "result_var", ""));
    FbJson *res = NULL;
    int rc = rt(r, "count", args, &res);
    if (rc == R_OK) rc = r_set_var(r, rvar, fbj_num(fbj_get_num(res, "count", 0)));
    fbj_free(res);
    fbj_free(args);
    return rc;
}

static int s_extract_table(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(headers, r_str(r, s, "headers", "auto"));
    NEED(rvar, r_str(r, s, "result_var", ""));
    fbj_obj_set(args, "headers", fbj_str(headers));
    FbJson *res = NULL;
    int rc = poll_rt(r, "extract_table", args, r_timeout(r, s), NULL, NULL, &res);
    if (rc == R_ERR && strstr(r->err, "not found")) rc = notfound(r, args, "Table not found");
    if (rc == R_OK) {
        FbJson *rows = fbj_get(res, "rows");
        r_log(r, "info", "Read %zu table row(s)", fbj_len(rows));
        rc = r_set_var(r, rvar, rows ? fbj_clone(rows) : fbj_arr());
    }
    fbj_free(res);
    fbj_free(args);
    return rc;
}

static int s_extract_page(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(rvar, r_str(r, s, "result_var", ""));
    FbJson *a = fbj_obj();
    const FbJson *k = fbj_get(s, "kinds");
    if (k && k->type == FBJ_ARR) fbj_obj_set(a, "kinds", fbj_clone(k));
    else if (k && k->type == FBJ_STR && k->str[0]) {                         /* "headings, links" */
        FbJson *arr = fbj_arr();
        char *t = fb_strdup(k->str);
        for (char *p = strtok(t, ", ;"); p; p = strtok(NULL, ", ;")) fbj_arr_push(arr, fbj_str(p));
        free(t);
        fbj_obj_set(a, "kinds", arr);
    }
    FbJson *res = NULL;
    int rc = rt(r, "extract_page", a, &res);
    fbj_free(a);
    if (rc == R_OK) { FbJson *d = fbj_get(res, "data"); rc = r_set_var(r, rvar, d ? fbj_clone(d) : fbj_obj()); }
    fbj_free(res);
    return rc;
}

static int s_extract_list(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "item_selector");
    if (!args) return R_ERR;
    NEED(rvar, r_str(r, s, "result_var", ""));
    const FbJson *fields = fbj_get(s, "fields");
    if (!fields || fields->type != FBJ_ARR || !fields->count) { fbj_free(args); return r_fail(r, "Add at least one field to read from each item"); }
    fbj_obj_set(args, "fields", fbj_clone(fields));
    long max_pages = r_num(r, s, "max_pages", 1);
    if (max_pages < 1) max_pages = 1;
    if (max_pages > 5000) max_pages = 5000;
    int wait_ms = (int)r_num(r, s, "page_wait_ms", 800), scroll = r_bool(r, s, "scroll_load", 0);
    int append = r_bool(r, s, "append", 0);
    const char *next = fbj_get_str(s, "next_selector", "");
    NEED(next_x, r_str(r, s, "next_selector", ""));
    (void)next;

    FbJson *all = NULL;
    if (append) {
        const FbJson *old = fbe_path_get(r->vars, rvar);
        if (old && old->type == FBJ_ARR) all = fbj_clone(old);
    }
    if (!all) all = fbj_arr();
    size_t before_total = all->count;
    int rc = R_OK;
    FbJson *w = NULL;
    rc = poll_rt(r, "exists", args, r_timeout(r, s), NULL, NULL, &w);
    fbj_free(w);
    if (rc != R_OK) { fbj_free(all); fbj_free(args); return notfound(r, args, "No items found for"); }

    size_t last_count = 0;
    for (long page = 1; page <= max_pages && rc == R_OK; page++) {
        FbJson *res = NULL;
        rc = rt(r, "extract_list", args, &res);
        if (rc != R_OK) break;
        FbJson *rows = fbj_get(res, "rows");
        size_t n = fbj_len(rows);
        if (scroll) {                                       /* infinite scroll: every pass returns everything loaded so far */
            if (page > 1 && n <= last_count) { fbj_free(res); break; }
            last_count = n;
            while (all->count > before_total) fbj_arr_del(all, all->count - 1);
            for (size_t i = 0; i < n; i++) fbj_arr_push(all, fbj_clone(rows->items[i]));
            fbj_free(res);
            FbJson *sc = fbj_obj();
            fbj_obj_set(sc, "to", fbj_str("bottom"));
            rc = rt(r, "scroll", sc, NULL);
            fbj_free(sc);
            if (rc == R_OK && r_sleep(r, wait_ms)) rc = R_STOP;
            continue;
        }
        for (size_t i = 0; i < n; i++) fbj_arr_push(all, fbj_clone(rows->items[i]));
        fbj_free(res);
        r_log(r, "info", "Page %ld: %zu item(s), %zu in total", page, n, all->count - before_total);
        if (!*next_x || page == max_pages) break;
        /* next page */
        FbJson *na = fbj_obj();
        fbj_obj_set(na, "selector", fbj_str(next_x));
        FbJson *info = NULL;
        FbJson *probe = NULL;
        if (call_rt(r, "rect", na, &probe, NULL) != R_OK || !fbj_get_bool(probe, "ok", 0) || !fbj_get_bool(probe, "visible", 0) || fbj_get_bool(probe, "disabled", 0)) {
            fbj_free(probe); fbj_free(na);
            r->err[0] = 0;
            break;                                          /* no (enabled) next button: that was the last page */
        }
        fbj_free(probe);
        FbJson *fp = fbj_obj();                             /* remember the first item so a changed list can be detected */
        fbj_obj_set(fp, "item_selector", fbj_clone(fbj_get(args, "selector")));
        FbJson *sig = NULL;
        rt(r, "signature", fp, &sig);
        char before[300] = "";
        snprintf(before, sizeof before, "%s", fbj_get_str(sig, "sig", ""));
        fbj_free(sig);
        fbj_free(fp);
        double x, y;
        drain_nav(r);
        rc = wait_rect(r, s, na, 0, &x, &y, &info);
        fbj_free(info);
        if (rc == R_OK) rc = click_at(r, x, y, "left", 1);
        fbj_free(na);
        if (rc != R_OK) break;
        if (r->be.wait_event && r->be.wait_event(r->be.ud, "Navigation.started", 250, NULL) == 0)
            r->be.wait_event(r->be.ud, "Navigation.completed", r_timeout(r, s), NULL);
        /* wait until the list really changed (single-page apps replace items without navigating) */
        uint64_t end = fb_now_ms() + (uint64_t)(wait_ms > 0 ? wait_ms : 800) + 6000;
        int changed = 0;
        while (fb_now_ms() < end && !r->stop) {
            FbJson *fp2 = fbj_obj();
            fbj_obj_set(fp2, "item_selector", fbj_clone(fbj_get(args, "selector")));
            FbJson *sg = NULL;
            int ok = call_rt(r, "signature", fp2, &sg, NULL) == R_OK && fbj_get_bool(sg, "ok", 0);
            int diff = ok && strcmp(before, fbj_get_str(sg, "sig", "")) != 0;
            fbj_free(sg);
            fbj_free(fp2);
            if (diff) { changed = 1; break; }
            if (r_sleep(r, 120)) break;
        }
        r->err[0] = 0;
        if (r->stop) { rc = R_STOP; break; }
        if (!changed) { r_log(r, "warn", "The page did not change after 'next'; stopping at page %ld", page); break; }
        if (wait_ms > 0 && r_sleep(r, wait_ms > 300 ? 300 : wait_ms)) rc = R_STOP;
    }
    if (rc != R_OK) { fbj_free(all); fbj_free(args); return rc; }
    fbj_free(args);
    r_log(r, "info", "Extracted %zu item(s)", all->count - before_total);
    return r_set_var(r, rvar, all);
}

static int s_autofill_form(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(dname, r_str(r, s, "data", "row"));
    FbJson *owned;
    const FbJson *data = r_lookup(r, dname, &owned);
    FbJson *d = fbj_obj();
    for (size_t i = 0; data && data->type == FBJ_OBJ && i < data->count; i++) {
        if (data->keys[i][0] == '_') continue;
        FbBuf b;
        fb_buf_init(&b);
        fbj_to_text(data->items[i], &b);
        fbj_obj_set(d, data->keys[i], fbj_str(fb_buf_cstr(&b)));
        fb_buf_free(&b);
    }
    fbj_free(owned);
    const FbJson *map = fbj_get(s, "mapping");
    for (size_t i = 0; map && map->type == FBJ_OBJ && i < map->count; i++) {
        FbBuf b;
        fb_buf_init(&b);
        if (map->items[i]->type == FBJ_STR) {
            if (fbe_expand(r->vars, map->items[i]->str, &b, r->err, sizeof r->err)) { fb_buf_free(&b); fbj_free(d); return R_ERR; }
        } else fbj_to_text(map->items[i], &b);
        fbj_obj_set(d, map->keys[i], fbj_str(fb_buf_cstr(&b)));
        fb_buf_free(&b);
    }
    if (!d->count) { fbj_free(d); return r_fail(r, "'%s' holds no data to fill the form with", dname); }
    FbJson *a = fbj_obj();
    fbj_obj_set(a, "data", d);
    NEED(root, r_str(r, s, "root", ""));
    if (*root) fbj_obj_set(a, "root", fbj_str(root));
    fbj_obj_set(a, "submit", fbj_bool(r_bool(r, s, "submit", 0)));
    drain_nav(r);
    FbJson *res = NULL;
    int rc = rt(r, "autofill_form", a, &res);
    fbj_free(a);
    if (rc != R_OK) return rc;
    const FbJson *rep = fbj_get(res, "report");
    size_t ok = 0, total = fbj_len(rep);
    FbBuf miss;
    fb_buf_init(&miss);
    for (size_t i = 0; i < total; i++) {
        if (fbj_get_bool(rep->items[i], "matched", 0)) ok++;
        else { if (miss.len) fb_buf_puts(&miss, ", "); fb_buf_puts(&miss, fbj_get_str(rep->items[i], "field", "?")); }
    }
    r_log(r, ok == total ? "info" : "warn", "Filled %zu of %zu field(s)%s%s", ok, total, miss.len ? "; no field found for: " : "", fb_buf_cstr(&miss));
    fb_buf_free(&miss);
    const char *rv = fbj_get_str(s, "result_var", "");
    if (*rv) rc = r_set_var(r, rv, rep ? fbj_clone(rep) : fbj_arr());
    fbj_free(res);
    if (rc == R_OK && r_bool(r, s, "submit", 0)) rc = settle(r, s, 700);
    return rc;
}

static int s_screenshot(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "path", ""));
    if (!*file) return r_fail(r, "No file name given for the screenshot");
    FbJson *p = fbj_obj();
    fbj_obj_set(p, "format", fbj_str(fb_ends_with(file, ".jpg") || fb_ends_with(file, ".jpeg") ? "jpeg" : "png"));
    const char *sel = fbj_get_str(s, "selector", "");
    if (*sel) {
        FbJson *args = sel_args(r, s, "selector");
        if (!args) { fbj_free(p); return R_ERR; }
        FbJson *info = NULL;
        double x, y;
        int rc = wait_rect(r, s, args, 1, &x, &y, &info);
        fbj_free(args);
        if (rc != R_OK) { fbj_free(p); return rc; }
        FbJson *clip = fbj_obj();
        fbj_obj_set(clip, "x", fbj_num(fbj_get_num(info, "px", 0)));
        fbj_obj_set(clip, "y", fbj_num(fbj_get_num(info, "py", 0)));
        fbj_obj_set(clip, "width", fbj_num(fbj_get_num(info, "w", 1)));
        fbj_obj_set(clip, "height", fbj_num(fbj_get_num(info, "h", 1)));
        fbj_obj_set(clip, "scale", fbj_num(1));
        fbj_obj_set(p, "clip", clip);
        fbj_obj_set(p, "captureBeyondViewport", fbj_bool(1));
        fbj_free(info);
    } else if (r_bool(r, s, "full_page", 0)) fbj_obj_set(p, "captureBeyondViewport", fbj_bool(1));
    FbJson *res = NULL;
    int rc = cdp(r, "Page.captureScreenshot", p, &res);
    fbj_free(p);
    if (rc != R_OK) return rc;
    FbBuf img;
    fb_buf_init(&img);
    const char *b64 = fbj_get_str(res, "data", "");
    if (fb_base64_decode(b64, strlen(b64), &img) || !img.len) { fbj_free(res); fb_buf_free(&img); return r_fail(r, "The browser returned no image"); }
    fbj_free(res);
    const char *abs = r_abs_path(r, file);
    fb_ensure_parent_dir(abs);
    rc = fb_write_file(abs, img.data, img.len) ? r_fail(r, "Cannot write '%s'", abs) : R_OK;
    fb_buf_free(&img);
    return rc;
}

/* ============================================================ waiting */

typedef struct { const char *state; } WaitState;

static int state_ok(const FbJson *res, void *ud)
{
    const char *st = ((WaitState *)ud)->state;
    int exists = fbj_get_bool(res, "exists", 0), vis = fbj_get_bool(res, "visible", 0);
    if (!strcmp(st, "attached")) return exists;
    if (!strcmp(st, "detached")) return !exists;
    if (!strcmp(st, "hidden")) return !exists || !vis;
    return exists && vis;
}

static int s_wait_element(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    FbJson *args = sel_args(r, s, "selector");
    if (!args) return R_ERR;
    NEED(state, r_str(r, s, "state", "visible"));
    WaitState ws = { state };
    fbj_obj_set(args, "optional", fbj_bool(1));
    int rc = poll_rt(r, "exists", args, r_timeout(r, s), state_ok, &ws, NULL);
    if (rc == R_ERR) r_fail(r, "Waited %d ms but the element did not become %s: %s", r_timeout(r, s), state, sel_name(args));
    fbj_free(args);
    return rc;
}

static int text_found(const FbJson *res, void *ud) { return fbj_get_bool(res, "found", 0) == *(int *)ud; }

static int s_wait_text(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(text, r_str(r, s, "text", ""));
    int gone = r_bool(r, s, "gone", 0), want = !gone;
    FbJson *a = fbj_obj();
    fbj_obj_set(a, "text", fbj_str(text));
    int rc = poll_rt(r, "has_text", a, r_timeout(r, s), text_found, &want, NULL);
    if (rc == R_ERR) r_fail(r, "Waited %d ms but the text %s: %s", r_timeout(r, s), gone ? "did not disappear" : "did not appear", text);
    fbj_free(a);
    return rc;
}

static int s_wait_until(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    const char *cond = fbj_get_str(s, "condition", "");
    long interval = r_num(r, s, "interval_ms", 1000);
    if (interval < 50) interval = 50;
    uint64_t end = fb_now_ms() + (uint64_t)r_timeout(r, s);
    for (;;) {
        FbJson *info = NULL;
        if (call_rt(r, "page_info", NULL, &info, NULL) == R_OK) {            /* page.url / page.title / page.text are always current here */
            FbJson *pg = fbj_obj();
            fbj_obj_set(pg, "url", fbj_str(fbj_get_str(info, "url", "")));
            fbj_obj_set(pg, "title", fbj_str(fbj_get_str(info, "title", "")));
            fbj_obj_set(pg, "text", fbj_str(fbj_get_str(info, "text", "")));
            fbj_obj_set(r->vars, "page", pg);
            fbj_free(info);
        }
        r->err[0] = 0;
        int v = fbe_eval_cond(r->vars, cond, r->err, sizeof r->err);
        if (v < 0) return r_fail(r, "Condition error: %s", r->err);
        if (v) return R_OK;
        if (fb_now_ms() >= end) return r_fail(r, "Waited %d ms but this never became true: %s", r_timeout(r, s), cond);
        if (r_sleep(r, (int)interval)) return R_STOP;
    }
}

/* ============================================================ CAPTCHA (detect only, never solve) */

int r_captcha_guard(FbRunner *r, const char *action_override)
{
    const char *action = action_override && strcmp(action_override, "inherit") ? action_override : r->captcha;
    if (!strcmp(action, "off") || !strcmp(action, "ignore")) return R_OK;
    for (int round = 0; round < 20; round++) {
        FbJson *res = NULL;
        int tr = 0;
        char keep[1024];
        snprintf(keep, sizeof keep, "%s", r->err);
        if (call_rt(r, "captcha", NULL, &res, &tr) != R_OK) { snprintf(r->err, sizeof r->err, "%s", keep); return R_OK; }   /* detection is best effort */
        int found = fbj_get_bool(res, "found", 0);
        char kind[100];
        snprintf(kind, sizeof kind, "%s", fbj_get_str(res, "kind", "CAPTCHA"));
        fbj_free(res);
        if (!found) return R_OK;
        FbJson *ev = fbj_obj();
        fbj_obj_set(ev, "ev", fbj_str("captcha"));
        fbj_obj_set(ev, "kind", fbj_str(kind));
        r_emit(r, ev);
        if (!strcmp(action, "stop"))
            return r_fail(r, "A CAPTCHA (%s) was found. FlowBrowser does not solve CAPTCHAs; the flow was stopped because 'on CAPTCHA' is set to stop", kind);
        char msg[300];
        snprintf(msg, sizeof msg, "A CAPTCHA (%s) is on the page. Solve it in the browser yourself, then press Continue.", kind);
        r_log(r, "warn", "%s", msg);
        if (!r->be.ask) return r_fail(r, "A CAPTCHA (%s) was found and nobody can solve it here", kind);
        FbBuf ans;
        fb_buf_init(&ans);
        int rc = r->be.ask(r->be.ud, "captcha", msg, "", 0, &ans);
        fb_buf_free(&ans);
        if (rc == 2) return R_STOP;
        if (rc == 1) return r_fail(r, "The CAPTCHA was not solved in time");
        if (r_sleep(r, 400)) return R_STOP;
    }
    return r_fail(r, "The CAPTCHA is still on the page");
}

static int s_captcha_check(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(a, r_str(r, s, "action", "inherit"));
    const char *eff = !strcmp(a, "inherit") ? (strcmp(r->captcha, "off") ? r->captcha : "pause") : a;   /* an explicit check step works even when auto-detection is off */
    return r_captcha_guard(r, eff);
}

const StepDef WEB_STEPS[] = {
    { "navigate", s_navigate, SF_CAPTCHA },
    { "new_tab", s_new_tab, SF_CAPTCHA },
    { "switch_tab", s_switch_tab, 0 },
    { "close_tab", s_close_tab, 0 },
    { "back", s_back, SF_CAPTCHA },
    { "forward", s_forward, SF_CAPTCHA },
    { "reload", s_reload, SF_CAPTCHA },
    { "handle_dialog", s_handle_dialog, SF_QUICK },
    { "clear_browsing_data", s_clear_browsing_data, 0 },
    { "set_user_agent", s_set_user_agent, SF_QUICK },
    { "click", s_click, SF_CAPTCHA },
    { "hover", s_hover, 0 },
    { "fill", s_fill, 0 },
    { "type_text", s_type_text, 0 },
    { "press_key", s_press_key, SF_CAPTCHA },
    { "select_option", s_select_option, 0 },
    { "check", s_check, 0 },
    { "set_slider", s_set_slider, 0 },
    { "upload_file", s_upload_file, 0 },
    { "drag_drop", s_drag_drop, 0 },
    { "scroll", s_scroll, 0 },
    { "focus", s_focus, 0 },
    { "submit_form", s_submit_form, SF_CAPTCHA },
    { "autofill_form", s_autofill_form, SF_CAPTCHA },
    { "run_js", s_run_js, 0 },
    { "get_text", s_get_text, 0 },
    { "get_attribute", s_get_attribute, 0 },
    { "element_exists", s_element_exists, 0 },
    { "count_elements", s_count_elements, 0 },
    { "extract_table", s_extract_table, 0 },
    { "extract_list", s_extract_list, 0 },
    { "extract_page", s_extract_page, 0 },
    { "screenshot", s_screenshot, 0 },
    { "wait_element", s_wait_element, 0 },
    { "wait_text", s_wait_text, 0 },
    { "wait_navigation", s_wait_navigation, SF_CAPTCHA },
    { "wait_until", s_wait_until, SF_QUICK },
    { "captcha_check", s_captcha_check, SF_QUICK },
    { NULL, NULL, 0 }
};
