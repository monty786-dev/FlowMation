/* Runner core: step loop, parameters, variables, control flow and the variable/utility steps. */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "fb_runner_int.h"

/* ============================================================ lifecycle */

FbRunner *fbr_new(const FbBackend *be)
{
    FbRunner *r = (FbRunner *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->be = *be;
    r->vars = fbj_obj();
    r->timeout_ms = 15000;
    snprintf(r->on_error, sizeof r->on_error, "stop");
    snprintf(r->captcha, sizeof r->captcha, "pause");
    return r;
}

static void scr_release(FbRunner *r, size_t mark)
{
    while (r->nscr > mark) free(r->scr[--r->nscr]);
}

void fbr_free(FbRunner *r)
{
    if (!r) return;
    r_close_books(r);
    scr_release(r, 0);
    free(r->scr);
    fbj_free(r->vars);
    free(r);
}

void fbr_set_dirs(FbRunner *r, const char *base_dir, const char *flows_dir)
{
    snprintf(r->base_dir, sizeof r->base_dir, "%s", base_dir ? base_dir : "");
    snprintf(r->flows_dir, sizeof r->flows_dir, "%s", flows_dir ? flows_dir : "");
}

void fbr_set_var(FbRunner *r, const char *name, FbJson *value)
{
    if (strpbrk(name, ".[")) fbe_path_set(r->vars, name, value);
    else fbj_obj_set(r->vars, name, value);
}

const FbJson *fbr_vars(const FbRunner *r) { return r->vars; }
void fbr_stop(FbRunner *r) { r->stop = 1; }
const char *fbr_error_path(const FbRunner *r) { return r->err_path; }

/* ======================================================= errors / params */

int r_fail(FbRunner *r, const char *fmt, ...)
{
    char tmp[sizeof r->err];            /* format aside first: callers often pass r->err itself as an argument */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    memcpy(r->err, tmp, sizeof tmp);
    return R_ERR;
}

const char *r_keep(FbRunner *r, char *owned)
{
    if (!owned) return NULL;
    if (r->nscr == r->cscr) {
        size_t nc = r->cscr ? r->cscr * 2 : 32;
        char **ns = (char **)realloc(r->scr, nc * sizeof(char *));
        if (!ns) { free(owned); return NULL; }
        r->scr = ns;
        r->cscr = nc;
    }
    r->scr[r->nscr++] = owned;
    return owned;
}

const char *r_str(FbRunner *r, const FbJson *s, const char *key, const char *def)
{
    const FbJson *v = fbj_get(s, key);
    if (!v || v->type == FBJ_NULL) return def;
    FbBuf b;
    fb_buf_init(&b);
    if (v->type == FBJ_STR) {
        if (fbe_expand(r->vars, v->str, &b, r->err, sizeof r->err)) { fb_buf_free(&b); return NULL; }
    } else fbj_to_text(v, &b);
    return r_keep(r, fb_buf_detach(&b));
}

long r_num(FbRunner *r, const FbJson *s, const char *key, long def)
{
    const FbJson *v = fbj_get(s, key);
    if (!v) return def;
    if (v->type == FBJ_NUM) return (long)v->num;
    const char *t = r_str(r, s, key, "");
    double d;
    if (t && fbe_parse_number(t, &d)) return (long)d;
    return def;
}

int r_bool(FbRunner *r, const FbJson *s, const char *key, int def)
{
    const FbJson *v = fbj_get(s, key);
    if (!v) return def;
    if (v->type == FBJ_BOOL) return v->b;
    if (v->type == FBJ_NUM) return v->num != 0;
    const char *t = r_str(r, s, key, "");
    return (t && *t) ? fbe_truthy(t) : def;
}

int r_timeout(FbRunner *r, const FbJson *s)
{
    long t = r_num(r, s, "timeout_ms", r->timeout_ms);
    return (int)(t < 0 ? 0 : t > 600000 ? 600000 : t);
}

/* %NAME% environment expansion, then resolve relative paths against the flow's base folder */
const char *r_abs_path(FbRunner *r, const char *p)
{
    if (!p || !*p) return p;
    FbBuf e;
    fb_buf_init(&e);
    for (const char *c = p; *c;) {
        if (*c == '%') {
            const char *end = strchr(c + 1, '%');
            if (end && end - c > 1 && end - c < 64) {
                char nm[64];
                memcpy(nm, c + 1, (size_t)(end - c - 1));
                nm[end - c - 1] = 0;
                const char *val = getenv(nm);
                if (val) { fb_buf_puts(&e, val); c = end + 1; continue; }
            }
        }
        fb_buf_putc(&e, *c++);
    }
    const char *full = fb_buf_cstr(&e);
    if (fb_is_absolute_path(full) || !r->base_dir[0]) return r_keep(r, fb_buf_detach(&e));
    FbBuf j;
    fb_buf_init(&j);
    fb_join_path(&j, r->base_dir, full);
    fb_buf_free(&e);
    return r_keep(r, fb_buf_detach(&j));
}

/* ========================================================== events / vars */

void r_emit(FbRunner *r, FbJson *ev)
{
    if (r->be.emit) {
        char *s = fbj_to_string(ev, 0);
        if (s) r->be.emit(r->be.ud, s);
        free(s);
    }
    fbj_free(ev);
}

void r_log(FbRunner *r, const char *level, const char *fmt, ...)
{
    char msg[1500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    FbJson *e = fbj_obj();
    fbj_obj_set(e, "ev", fbj_str("log"));
    fbj_obj_set(e, "level", fbj_str(level));
    fbj_obj_set(e, "msg", fbj_str(msg));
    r_emit(r, e);
}

int r_sleep(FbRunner *r, int ms)
{
    while (ms > 0 && !r->stop) {
        int slice = ms > 100 ? 100 : ms;
        fb_sleep_ms((unsigned)slice);
        ms -= slice;
    }
    return r->stop;
}

/* copy with limits so "var" events stay small even for big tables */
static FbJson *clone_limited(const FbJson *j, int max_items, size_t max_str, int depth)
{
    if (!j) return fbj_null();
    switch (j->type) {
    case FBJ_STR:
        if (strlen(j->str) > max_str) {
            size_t n = max_str;
            while (n && ((unsigned char)j->str[n] & 0xC0) == 0x80) n--;
            FbBuf b;
            fb_buf_init(&b);
            fb_buf_append(&b, j->str, n);
            fb_buf_puts(&b, "…");
            FbJson *s = fbj_str(fb_buf_cstr(&b));
            fb_buf_free(&b);
            return s;
        }
        return fbj_str(j->str);
    case FBJ_ARR: {
        FbJson *a = fbj_arr();
        for (size_t i = 0; i < j->count && (int)i < max_items; i++)
            fbj_arr_push(a, depth > 4 ? fbj_str("…") : clone_limited(j->items[i], max_items, max_str, depth + 1));
        return a;
    }
    case FBJ_OBJ: {
        FbJson *o = fbj_obj();
        for (size_t i = 0; i < j->count && (int)i < 200; i++)
            fbj_obj_set(o, j->keys[i], depth > 4 ? fbj_str("…") : clone_limited(j->items[i], max_items, max_str, depth + 1));
        return o;
    }
    default:
        return fbj_clone(j);
    }
}

static FbJson *var_event(const char *name, const FbJson *value)
{
    FbJson *e = fbj_obj();
    fbj_obj_set(e, "ev", fbj_str("var"));
    fbj_obj_set(e, "name", fbj_str(name));
    fbj_obj_set(e, "rows", fbj_num((double)fbj_len(value)));
    fbj_obj_set(e, "value", clone_limited(value, 200, 400, 0));
    return e;
}

int r_set_var(FbRunner *r, const char *name, FbJson *value)
{
    if (!name || !*name) { fbj_free(value); return r_fail(r, "A variable name is missing"); }
    FbJson *e = var_event(name, value);
    int rc;
    if (strpbrk(name, ".[")) rc = fbe_path_set(r->vars, name, value);
    else rc = fbj_obj_set(r->vars, name, value);
    if (rc) { fbj_free(e); return r_fail(r, "Cannot set variable '%s'", name); }
    r_emit(r, e);
    r->last_var_emit = fb_now_ms();
    return R_OK;
}

void r_emit_var(FbRunner *r, const char *name)
{
    const FbJson *v = fbe_path_get(r->vars, name);
    if (v) r_emit(r, var_event(name, v));
    r->last_var_emit = fb_now_ms();
}

void r_emit_vars(FbRunner *r)
{
    FbJson *e = fbj_obj();
    fbj_obj_set(e, "ev", fbj_str("vars"));
    fbj_obj_set(e, "vars", clone_limited(r->vars, 200, 400, 0));
    r_emit(r, e);
}

const FbJson *r_lookup(FbRunner *r, const char *src, FbJson **owned)
{
    *owned = NULL;
    if (strstr(src, "{{")) {
        FbJson *v = fbe_expand_value(r->vars, src, r->err, sizeof r->err);
        *owned = v;
        return v;
    }
    return fbe_path_get(r->vars, src);
}

/* A step parameter as a typed value. type: "auto" keeps the native type of a lone {{expr}} and of non-string JSON,
   "text" forces a string, "number" / "bool" convert, "json" parses the expanded text. Caller owns the result. */
FbJson *r_value_param(FbRunner *r, const FbJson *s, const char *key, const char *type)
{
    const FbJson *v = fbj_get(s, key);
    if (!v) return fbj_str("");
    int is_text = !strcmp(type, "text");
    if (v->type != FBJ_STR) {
        if (is_text) { FbBuf b; fb_buf_init(&b); fbj_to_text(v, &b); FbJson *o = fbj_str(fb_buf_cstr(&b)); fb_buf_free(&b); return o; }
        return fbj_clone(v);
    }
    if (!strcmp(type, "auto")) {
        FbJson *o = fbe_expand_value(r->vars, v->str, r->err, sizeof r->err);
        if (!o) r_fail(r, "%s", r->err[0] ? r->err : "Could not expand the value");
        return o;
    }
    const char *t = r_str(r, s, key, "");
    if (!t) return NULL;
    if (is_text) return fbj_str(t);
    if (!strcmp(type, "number")) {
        double d;
        if (!fbe_parse_number(t, &d)) { r_fail(r, "'%s' is not a number", t); return NULL; }
        return fbj_num(d);
    }
    if (!strcmp(type, "bool")) return fbj_bool(fbe_truthy(t));
    if (!strcmp(type, "json")) {
        char perr[200];
        FbJson *o = fbj_parse_str(t, perr, sizeof perr);
        if (!o) r_fail(r, "Invalid JSON: %s", perr);
        return o;
    }
    return fbj_str(t);
}

/* ============================================================== step loop */

static const StepDef *find_in(const StepDef *tab, const char *type)
{
    for (; tab->type; tab++) if (!strcmp(tab->type, type)) return tab;
    return NULL;
}

static const StepDef *find_step(const char *type)
{
    const StepDef *d = find_in(CORE_STEPS, type);
    if (!d) d = find_in(WEB_STEPS, type);
    if (!d) d = find_in(DATA_STEPS, type);
    return d;
}

static FbJson *step_event(const char *ev, const char *path, const FbJson *step)
{
    FbJson *e = fbj_obj();
    fbj_obj_set(e, "ev", fbj_str(ev));
    fbj_obj_set(e, "path", fbj_str(path));
    fbj_obj_set(e, "type", fbj_str(fbj_get_str(step, "type", "")));
    return e;
}

static int run_step(FbRunner *r, const FbJson *step, const char *path)
{
    if (r->stop) return R_STOP;
    if (!step || step->type != FBJ_OBJ) { snprintf(r->err_path, sizeof r->err_path, "%s", path); return r_fail(r, "Step %s is not valid", path); }
    if (!fbj_get_bool(step, "enabled", 1)) return R_OK;
    const char *type = fbj_get_str(step, "type", "");
    const StepDef *def = find_step(type);
    if (!def) {
        snprintf(r->err_path, sizeof r->err_path, "%s", path);
        r_fail(r, "Unknown step type '%s'", type);
        FbJson *ev = step_event("step_error", path, step);
        fbj_obj_set(ev, "error", fbj_str(r->err));
        r_emit(r, ev);
        return R_ERR;
    }
    if (r->be.checkpoint && r->be.checkpoint(r->be.ud, path)) return R_STOP;

    FbJson *ev = step_event("step_start", path, step);
    fbj_obj_set(ev, "name", fbj_str(fbj_get_str(step, "name", "")));
    r_emit(r, ev);

    size_t mark = r->nscr;
    uint64_t t0 = fb_now_ms();
    const char *policy = fbj_get_str(step, "on_error", "inherit");
    if (!strcmp(policy, "inherit") || !*policy) policy = r->in_try ? "stop" : r->on_error;
    int attempts = 1;
    if (!strcmp(policy, "retry")) {
        long n = (long)fbj_get_num(step, "retries", 2);
        attempts = 1 + (int)(n < 0 ? 0 : n > 10 ? 10 : n);
    }
    int rc;
    for (int a = 1;; a++) {
        r->err[0] = 0;
        rc = def->fn(r, step, path);
        if (rc == R_ERR && a < attempts && !r->stop) {
            FbJson *re = step_event("step_error", path, step);
            fbj_obj_set(re, "error", fbj_str(r->err));
            fbj_obj_set(re, "retry", fbj_num(a));
            r_emit(r, re);
            scr_release(r, mark);
            if (r_sleep(r, (int)fbj_get_num(step, "retry_delay_ms", 1000))) { rc = R_STOP; break; }
            continue;
        }
        break;
    }
    if (rc == R_OK && (def->flags & SF_CAPTCHA) && strcmp(r->captcha, "off") != 0) rc = r_captcha_guard(r, NULL);

    if (rc == R_ERR) {
        snprintf(r->err_path, sizeof r->err_path, "%s", path);
        FbJson *ee = step_event("step_error", path, step);
        fbj_obj_set(ee, "error", fbj_str(r->err));
        r_emit(r, ee);
        if (!strcmp(policy, "continue")) {
            r_log(r, "warn", "Step %s failed, continuing: %s", path, r->err);
            fbj_obj_set(r->vars, "error", fbj_str(r->err));
            rc = R_OK;
        }
    }
    FbJson *end = step_event("step_end", path, step);
    fbj_obj_set(end, "ok", fbj_bool(rc != R_ERR));
    fbj_obj_set(end, "ms", fbj_num((double)(fb_now_ms() - t0)));
    r_emit(r, end);
    scr_release(r, mark);
    if (rc == R_OK && r->step_delay_ms > 0 && !(def->flags & SF_QUICK)) { if (r_sleep(r, r->step_delay_ms)) rc = R_STOP; }
    return rc;
}

int r_run_steps(FbRunner *r, const FbJson *steps, const char *prefix)
{
    for (size_t i = 0; steps && i < steps->count; i++) {
        char path[200];
        if (prefix && *prefix) snprintf(path, sizeof path, "%s.%zu", prefix, i);
        else snprintf(path, sizeof path, "%zu", i);
        int rc = run_step(r, steps->items[i], path);
        if (rc != R_OK) return rc;
    }
    return R_OK;
}

int r_nested(FbRunner *r, const FbJson *step, const char *key, const char *path)
{
    const FbJson *a = fbj_get(step, key);
    if (!a || a->type != FBJ_ARR || !a->count) return R_OK;
    char sub[200];
    snprintf(sub, sizeof sub, "%s.%s", path, key);
    return r_run_steps(r, a, sub);
}

/* =================================================================== run */

static int count_all(const FbJson *steps)
{
    static const char *const NESTED[] = { "then", "else", "body", "catch", NULL };
    int n = 0;
    for (size_t i = 0; steps && steps->type == FBJ_ARR && i < steps->count; i++) {
        n++;
        const FbJson *s = steps->items[i];
        for (int k = 0; s->type == FBJ_OBJ && NESTED[k]; k++) n += count_all(fbj_get(s, NESTED[k]));
    }
    return n;
}

int fbr_run(FbRunner *r, const FbJson *flow, char *err, size_t errsz)
{
    r->stop = 0;
    r->err[0] = 0;
    r->err_path[0] = 0;
    r->depth = 0;
    r->in_try = 0;
    const FbJson *st = fbj_get(flow, "settings");
    r->timeout_ms = (int)fbj_get_num(st, "timeout_ms", 15000);
    r->step_delay_ms = (int)fbj_get_num(st, "step_delay_ms", 0);
    snprintf(r->on_error, sizeof r->on_error, "%s", !strcmp(fbj_get_str(st, "on_error", "stop"), "continue") ? "continue" : "stop");
    snprintf(r->captcha, sizeof r->captcha, "%s", fbj_get_str(st, "captcha", "pause"));
    if (r->be.option) {
        r->be.option(r->be.ud, "dialogs", fbj_get_str(st, "dialogs", "accept"));
        const char *ua = fbj_get_str(st, "user_agent", "");
        if (*ua) r->be.option(r->be.ud, "user_agent", ua);
    }
    const FbJson *vars = fbj_get(flow, "variables");
    for (size_t i = 0; vars && vars->type == FBJ_OBJ && i < vars->count; i++)
        if (!fbj_get(r->vars, vars->keys[i])) fbj_obj_set(r->vars, vars->keys[i], fbj_clone(vars->items[i]));

    const FbJson *steps = fbj_get(flow, "steps");
    FbJson *ev = fbj_obj();
    fbj_obj_set(ev, "ev", fbj_str("flow_start"));
    fbj_obj_set(ev, "name", fbj_str(fbj_get_str(flow, "name", "")));
    fbj_obj_set(ev, "steps", fbj_num(count_all(steps)));
    r_emit(r, ev);

    uint64_t t0 = fb_now_ms();
    int rc = steps && steps->type == FBJ_ARR ? r_run_steps(r, steps, "") : r_fail(r, "The flow has no steps");
    r_flush_books(r, 1);                    /* never leave edits unsaved when a flow ends, however it ends */
    int result;
    switch (rc) {
    case R_OK: case R_END: case R_BREAK: case R_CONT: result = 0; break;   /* stray break/continue outside a loop = end */
    case R_STOP: result = 2; break;
    default: result = 1; break;
    }
    if (result == 1 && !r->err[0]) r_fail(r, "The flow failed");

    FbJson *fe = fbj_obj();
    fbj_obj_set(fe, "ev", fbj_str("flow_end"));
    fbj_obj_set(fe, "result", fbj_str(result == 0 ? "ok" : result == 2 ? "stopped" : "error"));
    fbj_obj_set(fe, "ms", fbj_num((double)(fb_now_ms() - t0)));
    if (result == 1) {
        fbj_obj_set(fe, "error", fbj_str(r->err));
        fbj_obj_set(fe, "path", fbj_str(r->err_path));
    }
    r_emit_vars(r);
    r_emit(r, fe);
    if (err && errsz) { if (result == 1) fb_set_err(err, errsz, "%s", r->err); else err[0] = 0; }
    r_close_books(r);
    return result;
}
