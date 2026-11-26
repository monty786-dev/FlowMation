/* fbrun - runs a .fbflow file headless. The browser is reached over stdio with one JSON object per line, so any
   program that can drive a browser (tests/bridge.js does, with Chrome DevTools) can host the runner:

     runner -> host   {"id":1,"op":"cdp","method":"Runtime.evaluate","params":{...}}      host -> runner {"id":1,"ok":true,"result":{...}}
                      {"id":2,"op":"navigate"|"wait_event"|"tab"|"ask"|"option", ...}      host -> runner {"id":2,"ok":true,...}
                      {"op":"emit","event":{...}}                                           (no answer)

   usage: fbrun flow.fbflow [--catalog catalog.json] [--base DIR] [--flows DIR] [--var name=value]... [--check] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/fb_runner.h"

static int g_next_id = 1;

static int read_line(FbBuf *line)
{
    char chunk[65536];
    fb_buf_clear(line);
    for (;;) {
        if (!fgets(chunk, sizeof chunk, stdin)) return line->len ? 0 : -1;
        size_t n = strlen(chunk);
        fb_buf_append(line, chunk, n);
        if (n && chunk[n - 1] == '\n') return 0;
    }
}

/* sends a request object (id added) and waits for the matching answer */
static FbJson *call(FbJson *req)
{
    int id = g_next_id++;
    fbj_obj_set(req, "id", fbj_num(id));
    char *txt = fbj_to_string(req, 0);
    fbj_free(req);
    fputs(txt, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(txt);
    FbBuf line;
    fb_buf_init(&line);
    for (;;) {
        if (read_line(&line)) { fb_buf_free(&line); return NULL; }
        char err[100];
        FbJson *a = fbj_parse(fb_buf_cstr(&line), line.len, err, sizeof err);
        if (a && (int)fbj_get_num(a, "id", -1) == id) { fb_buf_free(&line); return a; }
        fbj_free(a);
    }
}

static int fill_err(FbJson *a, char *err, size_t n)
{
    if (!a) { fb_set_err(err, n, "The browser host disconnected"); return -1; }
    int ok = fbj_get_bool(a, "ok", 0);
    if (!ok) fb_set_err(err, n, "%s", fbj_get_str(a, "error", "browser command failed"));
    return ok ? 0 : -1;
}

static int h_cdp(void *ud, const char *method, const char *params, FbBuf *result, char *err, size_t errsz)
{
    (void)ud;
    FbJson *q = fbj_obj();
    fbj_obj_set(q, "op", fbj_str("cdp"));
    fbj_obj_set(q, "method", fbj_str(method));
    FbJson *p = fbj_parse_str(params, err, errsz);
    fbj_obj_set(q, "params", p ? p : fbj_obj());
    FbJson *a = call(q);
    int rc = fill_err(a, err, errsz);
    if (!rc) {
        const FbJson *r = fbj_get(a, "result");
        if (r) fbj_write(r, result, 0);
    }
    fbj_free(a);
    return rc;
}

static int h_navigate(void *ud, const char *url, int wait, int timeout_ms, char *err, size_t errsz)
{
    (void)ud;
    FbJson *q = fbj_obj();
    fbj_obj_set(q, "op", fbj_str("navigate"));
    fbj_obj_set(q, "url", fbj_str(url));
    fbj_obj_set(q, "wait", fbj_bool(wait));
    fbj_obj_set(q, "timeout", fbj_num(timeout_ms));
    FbJson *a = call(q);
    int rc = fill_err(a, err, errsz);
    fbj_free(a);
    return rc;
}

static int h_wait_event(void *ud, const char *name, int timeout_ms, FbBuf *params)
{
    (void)ud;
    FbJson *q = fbj_obj();
    fbj_obj_set(q, "op", fbj_str("wait_event"));
    fbj_obj_set(q, "name", fbj_str(name));
    fbj_obj_set(q, "timeout", fbj_num(timeout_ms));
    FbJson *a = call(q);
    int st = a ? (int)fbj_get_num(a, "status", 1) : -1;
    if (a && params && st == 0 && fbj_get(a, "params")) fbj_write(fbj_get(a, "params"), params, 0);
    fbj_free(a);
    return st;
}

static int h_tab(void *ud, const char *op, const char *arg, char *err, size_t errsz)
{
    (void)ud;
    FbJson *q = fbj_obj();
    fbj_obj_set(q, "op", fbj_str("tab"));
    fbj_obj_set(q, "action", fbj_str(op));
    fbj_obj_set(q, "arg", fbj_str(arg ? arg : ""));
    FbJson *a = call(q);
    int rc = fill_err(a, err, errsz);
    fbj_free(a);
    return rc;
}

static int h_ask(void *ud, const char *kind, const char *text, const char *def, int timeout_ms, FbBuf *answer)
{
    (void)ud;
    FbJson *q = fbj_obj();
    fbj_obj_set(q, "op", fbj_str("ask"));
    fbj_obj_set(q, "kind", fbj_str(kind));
    fbj_obj_set(q, "text", fbj_str(text));
    fbj_obj_set(q, "def", fbj_str(def ? def : ""));
    fbj_obj_set(q, "timeout", fbj_num(timeout_ms));
    FbJson *a = call(q);
    int st = a ? (int)fbj_get_num(a, "status", 2) : 2;
    if (a && answer) fb_buf_puts(answer, fbj_get_str(a, "answer", ""));
    fbj_free(a);
    return st;
}

static int h_option(void *ud, const char *name, const char *value)
{
    (void)ud;
    FbJson *q = fbj_obj();
    fbj_obj_set(q, "op", fbj_str("option"));
    fbj_obj_set(q, "name", fbj_str(name));
    fbj_obj_set(q, "value", fbj_str(value));
    fbj_free(call(q));
    return 0;
}

static void h_emit(void *ud, const char *ev)
{
    (void)ud;
    printf("{\"op\":\"emit\",\"event\":%s}\n", ev);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    const char *flow_path = NULL, *catalog = NULL, *base = NULL, *flows = NULL;
    int check_only = 0;
    FbJson *vars = fbj_obj();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--catalog") && i + 1 < argc) catalog = argv[++i];
        else if (!strcmp(argv[i], "--base") && i + 1 < argc) base = argv[++i];
        else if (!strcmp(argv[i], "--flows") && i + 1 < argc) flows = argv[++i];
        else if (!strcmp(argv[i], "--check")) check_only = 1;
        else if (!strcmp(argv[i], "--var") && i + 1 < argc) {
            char *kv = fb_strdup(argv[++i]);
            char *eq = strchr(kv, '=');
            if (eq) { *eq = 0; fbj_obj_set(vars, kv, fbj_str(eq + 1)); }
            free(kv);
        } else if (argv[i][0] != '-') flow_path = argv[i];
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 64; }
    }
    if (!flow_path) { fprintf(stderr, "usage: fbrun flow.fbflow [--catalog catalog.json] [--base DIR] [--flows DIR] [--var n=v] [--check]\n"); return 64; }

    char err[512];
    FbBuf b;
    fb_buf_init(&b);
    if (catalog) {
        if (fb_read_file(catalog, &b) || fbf_set_catalog(fb_buf_cstr(&b), err, sizeof err)) { fprintf(stderr, "catalog: %s\n", err); return 65; }
        fb_buf_clear(&b);
    }
    if (fb_read_file(flow_path, &b)) { fprintf(stderr, "cannot read %s\n", flow_path); return 66; }
    FbJson *flow = fbj_parse(fb_buf_cstr(&b), b.len, err, sizeof err);
    fb_buf_free(&b);
    if (!flow) { fprintf(stderr, "invalid flow file: %s\n", err); return 65; }
    fbf_migrate(flow);
    if (catalog) {
        FbJson *issues = fbf_validate(flow);
        int bad = 0;
        for (size_t i = 0; i < fbj_len(issues); i++) {
            const FbJson *it = issues->items[i];
            fprintf(stderr, "%s at step %s: %s\n", fbj_get_str(it, "level", "?"), fbj_get_str(it, "path", "?"), fbj_get_str(it, "message", ""));
            if (!strcmp(fbj_get_str(it, "level", ""), "error")) bad = 1;
        }
        fbj_free(issues);
        if (bad) { fbj_free(flow); fbj_free(vars); return 65; }
    }
    if (check_only) { fbj_free(flow); fbj_free(vars); return 0; }

    FbBackend be = { 0 };
    be.cdp = h_cdp; be.navigate = h_navigate; be.wait_event = h_wait_event; be.tab = h_tab;
    be.ask = h_ask; be.option = h_option; be.emit = h_emit;
    FbRunner *r = fbr_new(&be);
    char cwd_dir[512];
    snprintf(cwd_dir, sizeof cwd_dir, "%s", base ? base : ".");
    fbr_set_dirs(r, cwd_dir, flows ? flows : cwd_dir);
    for (size_t i = 0; i < vars->count; i++) fbr_set_var(r, vars->keys[i], fbj_clone(vars->items[i]));
    fbj_free(vars);
    int rc = fbr_run(r, flow, err, sizeof err);
    if (rc == 1) fprintf(stderr, "FAILED at step %s: %s\n", fbr_error_path(r), err);
    fbr_free(r);
    fbj_free(flow);
    return rc;
}
