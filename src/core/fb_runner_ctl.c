/* Runner steps: control flow, variables, text processing, user interaction. No browser needed. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "fb_regex.h"
#include "fb_runner_int.h"

/* ============================================================ helpers */

#define NEED(var, call) const char *var = (call); if (!var) return R_ERR

static void loop_event(FbRunner *r, const char *path, size_t i, size_t n)
{
    FbJson *e = fbj_obj();
    fbj_obj_set(e, "ev", fbj_str("loop"));
    fbj_obj_set(e, "path", fbj_str(path));
    fbj_obj_set(e, "i", fbj_num((double)i));
    fbj_obj_set(e, "n", fbj_num((double)n));
    r_emit(r, e);
}

/* result of one body run inside a loop: 0 keep looping, 1 leave loop normally, else propagate rc */
static int loop_body(FbRunner *r, const FbJson *step, const char *path, int *leave)
{
    int rc = r_nested(r, step, "body", path);
    *leave = 0;
    if (rc == R_BREAK) { *leave = 1; return R_OK; }
    if (rc == R_CONT) return R_OK;
    return rc;
}

/* ============================================================ flow control */

static int s_if(FbRunner *r, const FbJson *s, const char *path)
{
    const char *cond = fbj_get_str(s, "condition", "");
    int v = fbe_eval_cond(r->vars, cond, r->err, sizeof r->err);
    if (v < 0) return r_fail(r, "Condition error: %s", r->err);
    return r_nested(r, s, v ? "then" : "else", path);
}

static int s_foreach(FbRunner *r, const FbJson *s, const char *path)
{
    NEED(src, r_str(r, s, "source", "rows"));
    NEED(as, r_str(r, s, "as", "row"));
    NEED(idx, r_str(r, s, "index_as", "index"));
    /* the loop works on a snapshot: the body may append to the very list it iterates */
    FbJson *owned;
    const FbJson *v0 = r_lookup(r, src, &owned);
    if (!v0 && r->err[0]) return R_ERR;
    FbJson *snap = v0 ? (owned ? owned : fbj_clone(v0)) : NULL;
    if (!snap) return r_fail(r, "'%s' is not a variable", src);
    if (snap->type == FBJ_OBJ) {                    /* iterate the values of an object */
        FbJson *a = fbj_arr();
        for (size_t i = 0; i < snap->count; i++) fbj_arr_push(a, fbj_clone(snap->items[i]));
        fbj_free(snap);
        snap = a;
    } else if (snap->type != FBJ_ARR) {
        FbJson *a = fbj_arr();
        if (snap->type != FBJ_NULL && !(snap->type == FBJ_STR && !snap->str[0])) fbj_arr_push(a, snap); else fbj_free(snap);
        snap = a;
    }
    long start = r_num(r, s, "start", 1), limit = r_num(r, s, "limit", 0);
    if (start < 1) start = 1;
    size_t n = snap->count;
    int rc = R_OK;
    long done = 0;
    for (size_t i = (size_t)start - 1; i < n; i++) {
        if (r->stop) { rc = R_STOP; break; }
        if (limit > 0 && done >= limit) break;
        done++;
        fbj_obj_set(r->vars, as, fbj_clone(snap->items[i]));
        fbj_obj_set(r->vars, idx, fbj_num((double)(i + 1)));
        loop_event(r, path, i + 1, n);
        int leave;
        rc = loop_body(r, s, path, &leave);
        if (rc != R_OK || leave) break;
    }
    fbj_free(snap);
    return rc;
}

static int s_repeat(FbRunner *r, const FbJson *s, const char *path)
{
    NEED(idx, r_str(r, s, "index_as", "i"));
    long n = r_num(r, s, "times", 1);
    if (n < 0) n = 0;
    for (long i = 1; i <= n; i++) {
        if (r->stop) return R_STOP;
        fbj_obj_set(r->vars, idx, fbj_num((double)i));
        loop_event(r, path, (size_t)i, (size_t)n);
        int leave;
        int rc = loop_body(r, s, path, &leave);
        if (rc != R_OK) return rc;
        if (leave) break;
    }
    return R_OK;
}

static int s_while(FbRunner *r, const FbJson *s, const char *path)
{
    const char *cond = fbj_get_str(s, "condition", "");
    long max = r_num(r, s, "max_iterations", 1000);
    for (long i = 1;; i++) {
        if (r->stop) return R_STOP;
        int v = fbe_eval_cond(r->vars, cond, r->err, sizeof r->err);
        if (v < 0) return r_fail(r, "Condition error: %s", r->err);
        if (!v) return R_OK;
        if (i > max) return r_fail(r, "The 'while' loop ran more than %ld times (safety limit). Raise the limit or fix the condition", max);
        loop_event(r, path, (size_t)i, 0);
        int leave;
        int rc = loop_body(r, s, path, &leave);
        if (rc != R_OK) return rc;
        if (leave) return R_OK;
    }
}

static int s_break(FbRunner *r, const FbJson *s, const char *path) { (void)r; (void)s; (void)path; return R_BREAK; }
static int s_continue(FbRunner *r, const FbJson *s, const char *path) { (void)r; (void)s; (void)path; return R_CONT; }

static int s_stop(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(m, r_str(r, s, "message", ""));
    r_log(r, "info", "Flow stopped%s%s", *m ? ": " : "", m);
    return R_END;
}

static int s_try(FbRunner *r, const FbJson *s, const char *path)
{
    NEED(evar, r_str(r, s, "error_var", "error"));
    r->in_try++;
    int rc = r_nested(r, s, "body", path);
    r->in_try--;
    if (rc != R_ERR) return rc;
    char msg[1024];
    snprintf(msg, sizeof msg, "%s", r->err);
    r->err[0] = 0;
    r->err_path[0] = 0;
    r_log(r, "warn", "Caught error: %s", msg);
    fbj_obj_set(r->vars, evar, fbj_str(msg));
    return r_nested(r, s, "catch", path);
}

static int s_run_flow(FbRunner *r, const FbJson *s, const char *path)
{
    NEED(file, r_str(r, s, "file", ""));
    if (!*file) return r_fail(r, "No flow selected");
    if (r->depth >= 8) return r_fail(r, "Flows are nested too deeply (limit 8). Does a flow call itself?");
    FbJson *flow = NULL;
    char err[300] = "";
    int is_path = strchr(file, '/') || strchr(file, '\\') || fb_ends_with(file, ".fbflow") || fb_ends_with(file, ".json");
    if (is_path) {
        FbBuf b;
        fb_buf_init(&b);
        const char *abs = r_abs_path(r, file);
        if (fb_read_file(abs, &b)) { fb_buf_free(&b); return r_fail(r, "Cannot read flow file '%s'", abs); }
        flow = fbj_parse(fb_buf_cstr(&b), b.len, err, sizeof err);
        fb_buf_free(&b);
        if (!flow) return r_fail(r, "Flow file '%s' is not valid: %s", abs, err);
        fbf_migrate(flow);
    } else if (fbf_lib_load(r->flows_dir, file, &flow, err, sizeof err)) {
        return r_fail(r, "Cannot load flow '%s': %s", file, err);
    }
    /* parameters: typed values */
    const FbJson *pv = fbj_get(s, "vars");
    for (size_t i = 0; pv && pv->type == FBJ_OBJ && i < pv->count; i++) {
        FbJson *val = pv->items[i]->type == FBJ_STR
                          ? fbe_expand_value(r->vars, pv->items[i]->str, r->err, sizeof r->err)
                          : fbj_clone(pv->items[i]);
        if (!val) { fbj_free(flow); return r_fail(r, "Variable '%s': %s", pv->keys[i], r->err); }
        fbr_set_var(r, pv->keys[i], val);
    }
    const FbJson *defs = fbj_get(flow, "variables");
    for (size_t i = 0; defs && defs->type == FBJ_OBJ && i < defs->count; i++)
        if (!fbj_get(r->vars, defs->keys[i])) fbj_obj_set(r->vars, defs->keys[i], fbj_clone(defs->items[i]));
    char sub[200];
    snprintf(sub, sizeof sub, "%s.flow", path);
    r->depth++;
    int rc = r_run_steps(r, fbj_get(flow, "steps"), sub);
    r->depth--;
    fbj_free(flow);
    return rc == R_END || rc == R_BREAK || rc == R_CONT ? R_OK : rc;
}

/* ============================================================ variables */

static int s_set_var(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(name, r_str(r, s, "var", ""));
    NEED(type, r_str(r, s, "value_type", "auto"));
    FbJson *v = r_value_param(r, s, "value", type);
    if (!v) return R_ERR;
    return r_set_var(r, name, v);
}

static int s_calc(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(name, r_str(r, s, "var", ""));
    NEED(expr, r_str(r, s, "expression", ""));
    double d;
    char err[200];
    if (fbe_calc(expr, &d, err, sizeof err)) return r_fail(r, "Cannot calculate '%s': %s", expr, err);
    return r_set_var(r, name, fbj_num(d));
}

static int s_list_add(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(name, r_str(r, s, "var", ""));
    NEED(type, r_str(r, s, "value_type", "auto"));
    FbJson *v = r_value_param(r, s, "value", type);
    if (!v) return R_ERR;
    FbJson *list = (FbJson *)fbe_path_get(r->vars, name);
    if (!list || list->type == FBJ_NULL || (list->type == FBJ_STR && !list->str[0])) {
        FbJson *a = fbj_arr();
        fbj_arr_push(a, v);
        return r_set_var(r, name, a);
    }
    if (list->type != FBJ_ARR) { fbj_free(v); return r_fail(r, "Variable '%s' is not a list (use a different name or clear it first)", name); }
    fbj_arr_push(list, v);
    if (list->count <= 20 || fb_now_ms() - r->last_var_emit > 250) r_emit_var(r, name);
    return R_OK;
}

/* ============================================================ text_transform */

typedef struct { FbJson *arr; } Collect;

static void put_group(FbBuf *b, const char *s, const int *caps, int g)
{
    if (caps[2 * g] >= 0) fb_buf_append(b, s + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]));
}

static FbJson *group_str(const char *s, const int *caps, int g)
{
    if (caps[2 * g] < 0) return fbj_str("");
    return fbj_strn(s + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]));
}

static int next_pos(const char *s, size_t len, const int *caps)
{
    if (caps[1] > caps[0]) return caps[1];
    int p = caps[1] + 1;                                         /* empty match: step over one whole UTF-8 char */
    while ((size_t)p < len && ((unsigned char)s[p] & 0xC0) == 0x80) p++;
    return p;
}

static int s_text_transform(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(text, r_str(r, s, "value", ""));
    NEED(op, r_str(r, s, "op", "regex_extract"));
    NEED(pat, r_str(r, s, "pattern", ""));
    NEED(flags, r_str(r, s, "flags", "i"));
    NEED(rvar, r_str(r, s, "result_var", ""));
    NEED(repl, r_str(r, s, "replacement", ""));
    long group = r_num(r, s, "group", 0);
    char err[160];
    FbRegex *re = fbre_compile(pat, fbre_flags_from_string(flags), err, sizeof err);
    if (!re) return r_fail(r, "Bad pattern '%s': %s", pat, err);
    int ng = fbre_ngroups(re);
    if (group < 0 || group > ng) { fbre_free(re); return r_fail(r, "The pattern has %d capture group(s); group %ld does not exist", ng, group); }
    size_t len = strlen(text);
    int *caps = (int *)malloc(sizeof(int) * (size_t)(2 * (ng + 1)));
    if (!caps) { fbre_free(re); return r_fail(r, "Out of memory"); }
    FbJson *out = NULL;
    int rc = R_OK, m;

    if (!strcmp(op, "regex_test")) {
        m = fbre_search(re, text, len, 0, caps);
        if (m < 0) rc = r_fail(r, "The pattern is too complex for this text");
        else out = fbj_bool(m == 1);
    } else if (!strcmp(op, "regex_extract")) {
        m = fbre_search(re, text, len, 0, caps);
        if (m < 0) rc = r_fail(r, "The pattern is too complex for this text");
        else out = m == 1 ? group_str(text, caps, (int)group) : fbj_str("");
    } else if (!strcmp(op, "regex_extract_all")) {
        out = fbj_arr();
        for (int pos = 0; (size_t)pos <= len;) {
            m = fbre_search(re, text, len, (size_t)pos, caps);
            if (m < 0) { rc = r_fail(r, "The pattern is too complex for this text"); break; }
            if (m == 0) break;
            fbj_arr_push(out, group_str(text, caps, (int)group));
            pos = next_pos(text, len, caps);
        }
    } else if (!strcmp(op, "split")) {
        out = fbj_arr();
        size_t last = 0;
        for (int pos = 0; (size_t)pos <= len;) {
            m = fbre_search(re, text, len, (size_t)pos, caps);
            if (m < 0) { rc = r_fail(r, "The pattern is too complex for this text"); break; }
            if (m == 0) break;
            if (caps[1] == caps[0]) {                           /* an empty separator never splits */
                pos = next_pos(text, len, caps);
                continue;
            }
            fbj_arr_push(out, fbj_strn(text + last, (size_t)caps[0] - last));
            last = (size_t)caps[1];
            pos = caps[1];
        }
        if (rc == R_OK) fbj_arr_push(out, fbj_strn(text + last, len - last));
    } else if (!strcmp(op, "regex_replace")) {
        FbBuf b;
        fb_buf_init(&b);
        size_t last = 0;
        for (int pos = 0; (size_t)pos <= len;) {
            m = fbre_search(re, text, len, (size_t)pos, caps);
            if (m < 0) { rc = r_fail(r, "The pattern is too complex for this text"); break; }
            if (m == 0) break;
            fb_buf_append(&b, text + last, (size_t)caps[0] - last);
            for (const char *q = repl; *q; q++) {                /* $0 $& $1..$9 $$ */
                if (*q == '$' && q[1] == '$') { fb_buf_putc(&b, '$'); q++; }
                else if (*q == '$' && q[1] == '&') { put_group(&b, text, caps, 0); q++; }
                else if (*q == '$' && isdigit((unsigned char)q[1]) && q[1] - '0' <= ng) { put_group(&b, text, caps, q[1] - '0'); q++; }
                else fb_buf_putc(&b, *q);
            }
            last = (size_t)caps[1];
            int np = next_pos(text, len, caps);
            if (caps[1] == caps[0] && (size_t)np <= len && np > caps[1]) {   /* copy the char we skip over */
                fb_buf_append(&b, text + caps[1], (size_t)np - (size_t)caps[1]);
                last = (size_t)np;
            }
            pos = np;
        }
        if (rc == R_OK) {
            if (last < len) fb_buf_append(&b, text + last, len - last);
            out = fbj_str(fb_buf_cstr(&b));
        }
        fb_buf_free(&b);
    } else rc = r_fail(r, "Unknown operation '%s'", op);

    free(caps);
    fbre_free(re);
    if (rc != R_OK) { fbj_free(out); return rc; }
    return r_set_var(r, rvar, out);
}

/* ============================================================ messages / input / waiting */

static int s_comment(FbRunner *r, const FbJson *s, const char *path) { (void)r; (void)s; (void)path; return R_OK; }

static int s_log(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(msg, r_str(r, s, "message", ""));
    NEED(lvl, r_str(r, s, "level", "info"));
    r_log(r, !strcmp(lvl, "warn") || !strcmp(lvl, "error") ? lvl : "info", "%s", msg);
    return R_OK;
}

static int s_message_box(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(msg, r_str(r, s, "message", ""));
    if (!r->be.ask) { r_log(r, "info", "%s", msg); return R_OK; }
    FbBuf ans;
    fb_buf_init(&ans);
    int rc = r->be.ask(r->be.ud, "message", msg, "", (int)r_num(r, s, "timeout_ms", 0), &ans);
    fb_buf_free(&ans);
    return rc == 2 ? R_STOP : R_OK;
}

static int s_ask_input(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(prompt, r_str(r, s, "prompt", ""));
    NEED(def, r_str(r, s, "default", ""));
    NEED(rvar, r_str(r, s, "result_var", ""));
    if (!r->be.ask) return r_set_var(r, rvar, fbj_str(def));
    FbBuf ans;
    fb_buf_init(&ans);
    int rc = r->be.ask(r->be.ud, "input", prompt, def, (int)r_num(r, s, "timeout_ms", 0), &ans);
    if (rc == 0) rc = r_set_var(r, rvar, fbj_str(fb_buf_cstr(&ans)));
    else if (rc == 1) rc = r_fail(r, "Nobody answered the question in time");
    else rc = R_STOP;
    fb_buf_free(&ans);
    return rc;
}

static int s_wait_seconds(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(t, r_str(r, s, "seconds", "1"));
    double d = 1;
    if (!fbe_parse_number(t, &d) || d < 0) return r_fail(r, "'%s' is not a valid number of seconds", t);
    if (d > 86400) d = 86400;
    return r_sleep(r, (int)(d * 1000)) ? R_STOP : R_OK;
}

const StepDef CORE_STEPS[] = {
    { "comment", s_comment, SF_QUICK },
    { "log", s_log, SF_QUICK },
    { "message_box", s_message_box, SF_QUICK },
    { "ask_input", s_ask_input, SF_QUICK },
    { "set_var", s_set_var, SF_QUICK },
    { "calc", s_calc, SF_QUICK },
    { "list_add", s_list_add, SF_QUICK },
    { "text_transform", s_text_transform, SF_QUICK },
    { "if", s_if, SF_QUICK },
    { "foreach", s_foreach, SF_QUICK },
    { "repeat", s_repeat, SF_QUICK },
    { "while", s_while, SF_QUICK },
    { "break", s_break, SF_QUICK },
    { "continue", s_continue, SF_QUICK },
    { "stop", s_stop, SF_QUICK },
    { "try", s_try, SF_QUICK },
    { "run_flow", s_run_flow, SF_QUICK },
    { "wait_seconds", s_wait_seconds, SF_QUICK },
    { NULL, NULL, 0 }
};
