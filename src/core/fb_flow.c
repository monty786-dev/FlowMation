#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
/* Flow files: catalog, validation, library (save/load/versions) and backup/restore. */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fb_runner.h"

/* ================================================================== catalog */

static FbJson *g_catalog = NULL;

int fbf_set_catalog(const char *json, char *err, size_t errsz)
{
    FbJson *c = fbj_parse_str(json, err, errsz);
    if (!c) return -1;
    if (!fbj_get(c, "steps") || fbj_get(c, "steps")->type != FBJ_ARR) {
        fbj_free(c);
        fb_set_err(err, errsz, "catalog has no \"steps\" array");
        return -1;
    }
    fbj_free(g_catalog);
    g_catalog = c;
    return 0;
}

const FbJson *fbf_catalog(void) { return g_catalog; }

static const FbJson *find_def(const char *type)
{
    const FbJson *steps = g_catalog ? fbj_get(g_catalog, "steps") : NULL;
    for (size_t i = 0; steps && i < steps->count; i++)
        if (!strcmp(fbj_get_str(steps->items[i], "type", ""), type)) return steps->items[i];
    return NULL;
}

static const FbJson *find_param(const FbJson *def, const char *name)
{
    const FbJson *ps = fbj_get(def, "params");
    for (size_t i = 0; ps && i < ps->count; i++)
        if (!strcmp(fbj_get_str(ps->items[i], "name", ""), name)) return ps->items[i];
    const FbJson *common = g_catalog ? fbj_get(g_catalog, "common") : NULL;
    for (size_t i = 0; common && i < common->count; i++)
        if (!strcmp(fbj_get_str(common->items[i], "name", ""), name)) return common->items[i];
    return NULL;
}

/* ================================================================ new flow */

static void timestamp(char *out, size_t n, const char *fmt)
{
    time_t t = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(out, n, fmt, &tm);
}

FbJson *fbf_new(const char *name)
{
    FbJson *f = fbj_obj();
    char ts[32];
    timestamp(ts, sizeof ts, "%Y-%m-%d %H:%M:%S");
    fbj_obj_set(f, "format", fbj_str("flowbrowser-flow"));
    fbj_obj_set(f, "version", fbj_num(1));
    fbj_obj_set(f, "name", fbj_str(name && *name ? name : "New flow"));
    fbj_obj_set(f, "description", fbj_str(""));
    fbj_obj_set(f, "created", fbj_str(ts));
    FbJson *s = fbj_obj();
    fbj_obj_set(s, "timeout_ms", fbj_num(15000));
    fbj_obj_set(s, "step_delay_ms", fbj_num(0));
    fbj_obj_set(s, "on_error", fbj_str("stop"));
    fbj_obj_set(s, "captcha", fbj_str("pause"));
    fbj_obj_set(s, "dialogs", fbj_str("accept"));
    fbj_obj_set(f, "settings", s);
    fbj_obj_set(f, "variables", fbj_obj());
    fbj_obj_set(f, "steps", fbj_arr());
    return f;
}

void fbf_migrate(FbJson *flow)
{
    if (!flow || flow->type != FBJ_OBJ) return;
    if (!fbj_get(flow, "format")) fbj_obj_set(flow, "format", fbj_str("flowbrowser-flow"));
    if (!fbj_get(flow, "version")) fbj_obj_set(flow, "version", fbj_num(1));
    if (!fbj_get(flow, "settings")) fbj_obj_set(flow, "settings", fbj_obj());
    if (!fbj_get(flow, "variables")) fbj_obj_set(flow, "variables", fbj_obj());
    if (!fbj_get(flow, "steps")) fbj_obj_set(flow, "steps", fbj_arr());
}

/* ================================================================ validation */

static void issue(FbJson *arr, const char *path, const char *level, const char *fmt, ...)
{
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "path", fbj_str(path));
    fbj_obj_set(o, "level", fbj_str(level));
    fbj_obj_set(o, "message", fbj_str(msg));
    fbj_arr_push(arr, o);
}

static void validate_steps(const FbJson *steps, const char *prefix, FbJson *issues, int depth);

static int has_template(const char *s) { return strstr(s, "{{") != NULL; }

static void validate_step(const FbJson *step, const char *path, FbJson *issues, int depth)
{
    if (!step || step->type != FBJ_OBJ) { issue(issues, path, "error", "Step is not an object"); return; }
    const char *type = fbj_get_str(step, "type", "");
    if (!*type) { issue(issues, path, "error", "Step has no \"type\""); return; }
    const FbJson *def = find_def(type);
    if (!def) { issue(issues, path, "error", "Unknown step type '%s'", type); return; }
    const char *label = fbj_get_str(def, "label", type);

    /* unknown keys */
    for (size_t i = 0; i < step->count; i++) {
        const char *k = step->keys[i];
        if (!strcmp(k, "type") || !strcmp(k, "id") || !strcmp(k, "alts") || !strcmp(k, "index") || !strcmp(k, "frame")) continue;
        if (!find_param(def, k)) issue(issues, path, "warn", "%s: unknown setting '%s' (ignored)", label, k);
    }
    /* required + typed checks */
    const FbJson *ps = fbj_get(def, "params");
    for (size_t i = 0; ps && i < ps->count; i++) {
        const FbJson *p = ps->items[i];
        const char *pn = fbj_get_str(p, "name", "");
        const char *kind = fbj_get_str(p, "kind", "text");
        const FbJson *v = fbj_get(step, pn);
        int required = fbj_get_bool(p, "required", 0);
        int missing = !v || (v->type == FBJ_STR && !v->str[0]) || (v->type == FBJ_NULL) ||
                      (v->type == FBJ_ARR && v->count == 0 && strcmp(kind, "steps") != 0);
        if (required && missing && strcmp(kind, "steps") != 0 && !fbj_get(p, "default")) {
            issue(issues, path, "error", "%s: \"%s\" is required", label, fbj_get_str(p, "label", pn));
            continue;
        }
        if (required && missing && fbj_get(p, "default") && !v) continue;   /* a default applies */
        if (!v) continue;
        if (!strcmp(kind, "steps")) {
            if (v->type != FBJ_ARR) { issue(issues, path, "error", "%s: \"%s\" must be a list of steps", label, pn); continue; }
            if (depth > 30) { issue(issues, path, "error", "Steps are nested too deeply"); continue; }
            char sub[200];
            snprintf(sub, sizeof sub, "%s.%s", path, pn);
            validate_steps(v, sub, issues, depth + 1);
        } else if (!strcmp(kind, "enum") && v->type == FBJ_STR && !has_template(v->str) && v->str[0]) {
            const FbJson *opts = fbj_get(p, "options");
            int ok = 0;
            for (size_t k = 0; opts && k < opts->count; k++) if (!strcmp(opts->items[k]->str, v->str)) ok = 1;
            if (!ok) issue(issues, path, "warn", "%s: \"%s\" has unexpected value '%s'", label, pn, v->str);
        } else if (!strcmp(kind, "fields")) {
            if (v->type != FBJ_ARR) { issue(issues, path, "error", "%s: fields must be a list", label); continue; }
            for (size_t k = 0; k < v->count; k++) {
                const FbJson *f = v->items[k];
                if (f->type != FBJ_OBJ || !*fbj_get_str(f, "name", "") || !*fbj_get_str(f, "selector", "")) {
                    issue(issues, path, "error", "%s: field #%zu needs a name and a selector", label, k + 1);
                    break;
                }
            }
        } else if (!strcmp(kind, "kv") && v->type != FBJ_OBJ) {
            issue(issues, path, "error", "%s: \"%s\" must be a name/value map", label, pn);
        } else if ((!strcmp(kind, "list")) && v->type != FBJ_ARR && v->type != FBJ_STR) {
            issue(issues, path, "error", "%s: \"%s\" must be a list", label, pn);
        }
    }
    if (!strcmp(type, "excel_append_row") && !fbj_get(step, "values") && !*fbj_get_str(step, "source", ""))
        issue(issues, path, "error", "%s: give either values or a variable (source)", label);
    if (!strcmp(type, "switch_tab") && !*fbj_get_str(step, "tab", "")) issue(issues, path, "error", "%s: \"tab\" is required", label);
}

static void validate_steps(const FbJson *steps, const char *prefix, FbJson *issues, int depth)
{
    for (size_t i = 0; i < steps->count; i++) {
        char path[220];
        if (prefix && *prefix) snprintf(path, sizeof path, "%s.%zu", prefix, i);
        else snprintf(path, sizeof path, "%zu", i);
        validate_step(steps->items[i], path, issues, depth);
    }
}

FbJson *fbf_validate(const FbJson *flow)
{
    FbJson *issues = fbj_arr();
    if (!flow || flow->type != FBJ_OBJ) { issue(issues, "", "error", "A flow must be a JSON object"); return issues; }
    const FbJson *steps = fbj_get(flow, "steps");
    if (!steps || steps->type != FBJ_ARR) { issue(issues, "", "error", "Flow has no \"steps\" list"); return issues; }
    if (!g_catalog) { issue(issues, "", "warn", "Step catalog not loaded; skipping detailed checks"); return issues; }
    validate_steps(steps, "", issues, 0);
    return issues;
}

/* ================================================================== library */

static int fb_stricmp_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return x - y;
    }
    return 0;
}

void fbf_safe_name(const char *in, FbBuf *out)
{
    size_t start = out->len;
    for (const char *p = in; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) fb_buf_putc(out, '_');
        else if (c == '.' && p[1] == '.') { fb_buf_putc(out, '_'); p++; }       /* never ".." (path traversal) */
        else fb_buf_putc(out, (char)c);
    }
    /* no leading dots / spaces (hidden files), no trailing dots / spaces (illegal on Windows) */
    while (out->len > start && (out->data[out->len - 1] == ' ' || out->data[out->len - 1] == '.')) out->data[--out->len] = 0;
    size_t lead = 0;
    while (start + lead < out->len && (out->data[start + lead] == ' ' || out->data[start + lead] == '.')) lead++;
    if (lead) { memmove(out->data + start, out->data + start + lead, out->len - start - lead + 1); out->len -= lead; }
    if (out->len == start) fb_buf_puts(out, "flow");
    if (out->len - start > 120) {
        size_t n = 120;
        while (n > 0 && ((unsigned char)out->data[start + n] & 0xC0) == 0x80) n--;      /* do not cut a UTF-8 character in half */
        out->len = start + n;
        out->data[out->len] = 0;
    }
    /* Windows device names (CON, NUL, COM1 ...) cannot be file names, with or without an extension */
    {
        static const char *const RES[] = { "CON", "PRN", "AUX", "NUL", NULL };
        const char *nm = out->data + start;
        size_t stem = strcspn(nm, ".");
        int bad = 0;
        for (int i = 0; RES[i]; i++) if (stem == 3 && fb_stricmp_n(nm, RES[i], 3) == 0) bad = 1;
        if (stem == 4 && (fb_stricmp_n(nm, "COM", 3) == 0 || fb_stricmp_n(nm, "LPT", 3) == 0) && nm[3] >= '1' && nm[3] <= '9') bad = 1;
        if (bad) {
            fb_buf_putc(out, 0);
            out->len--;
            fb_buf_reserve(out, 2);
            memmove(out->data + start + 1, out->data + start, out->len - start + 1);
            out->data[start] = '_';
            out->len++;
        }
    }
}

static void lib_path(FbBuf *out, const char *dir, const char *name)
{
    FbBuf safe;
    fb_buf_init(&safe);
    fbf_safe_name(name, &safe);
    fb_join_path(out, dir, fb_buf_cstr(&safe));
    fb_buf_puts(out, ".fbflow");
    fb_buf_free(&safe);
}

typedef struct { char **names; size_t n, cap; const char *suffix; } NameList;

static int collect_cb(const char *name, int is_dir, void *ud)
{
    NameList *l = (NameList *)ud;
    if (is_dir || !fb_ends_with(name, l->suffix)) return 0;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 16;
        char **nn = (char **)realloc(l->names, nc * sizeof(char *));
        if (!nn) return 1;
        l->names = nn;
        l->cap = nc;
    }
    l->names[l->n++] = fb_strdup(name);
    return 0;
}

static int cmp_ci(const void *a, const void *b) { return fb_stricmp(*(char *const *)a, *(char *const *)b); }

static void names_free(NameList *l) { for (size_t i = 0; i < l->n; i++) free(l->names[i]); free(l->names); }

static int count_steps(const FbJson *steps)
{
    int n = 0;
    for (size_t i = 0; steps && steps->type == FBJ_ARR && i < steps->count; i++) {
        n++;
        const FbJson *s = steps->items[i];
        for (size_t k = 0; s->type == FBJ_OBJ && k < s->count; k++)
            if (s->items[k]->type == FBJ_ARR && (!strcmp(s->keys[k], "body") || !strcmp(s->keys[k], "then") || !strcmp(s->keys[k], "else") || !strcmp(s->keys[k], "catch")))
                n += count_steps(s->items[k]);
    }
    return n;
}

int fbf_lib_list(const char *dir, FbJson **out)
{
    *out = fbj_arr();
    NameList l = { NULL, 0, 0, ".fbflow" };
    if (fb_list_dir(dir, collect_cb, &l) != 0) { names_free(&l); return 0; }   /* no folder yet = empty library */
    qsort(l.names, l.n, sizeof(char *), cmp_ci);
    for (size_t i = 0; i < l.n; i++) {
        FbBuf path, data;
        fb_buf_init(&path); fb_buf_init(&data);
        fb_join_path(&path, dir, l.names[i]);
        char base[256];
        snprintf(base, sizeof base, "%.*s", (int)(strlen(l.names[i]) - 7), l.names[i]);
        FbJson *e = fbj_obj();
        fbj_obj_set(e, "name", fbj_str(base));
        if (fb_read_file(fb_buf_cstr(&path), &data) == 0) {
            FbJson *f = fbj_parse(fb_buf_cstr(&data), data.len, NULL, 0);
            if (f) {
                fbj_obj_set(e, "title", fbj_str(fbj_get_str(f, "name", base)));
                fbj_obj_set(e, "description", fbj_str(fbj_get_str(f, "description", "")));
                fbj_obj_set(e, "steps", fbj_num(count_steps(fbj_get(f, "steps"))));
                fbj_obj_set(e, "updated", fbj_str(fbj_get_str(f, "updated", fbj_get_str(f, "created", ""))));
                fbj_free(f);
            } else fbj_obj_set(e, "broken", fbj_bool(1));
        }
        fbj_arr_push(*out, e);
        fb_buf_free(&path); fb_buf_free(&data);
    }
    names_free(&l);
    return 0;
}

static void prune_history(const char *hist_dir, const char *safe, int keep)
{
    NameList l = { NULL, 0, 0, ".fbflow" };
    if (fb_list_dir(hist_dir, collect_cb, &l) != 0) return;
    char prefix[200];
    snprintf(prefix, sizeof prefix, "%s__", safe);
    char **mine = (char **)calloc(l.n ? l.n : 1, sizeof(char *));
    size_t nm = 0;
    for (size_t i = 0; i < l.n; i++) if (fb_starts_with(l.names[i], prefix)) mine[nm++] = l.names[i];
    qsort(mine, nm, sizeof(char *), cmp_ci);            /* timestamp in the name => oldest first */
    for (size_t i = 0; nm > (size_t)keep && i < nm - (size_t)keep; i++) {
        FbBuf p;
        fb_buf_init(&p);
        fb_join_path(&p, hist_dir, mine[i]);
        fb_delete_file(fb_buf_cstr(&p));
        fb_buf_free(&p);
    }
    free(mine);
    names_free(&l);
}

int fbf_lib_save(const char *dir, const char *name, const FbJson *flow, int keep_versions, char *err, size_t errsz)
{
    if (fb_mkdirs(dir) != 0 && !fb_file_exists(dir)) { /* mkdir -p reports EEXIST as ok; real failures surface on write */ }
    FbBuf path, safe;
    fb_buf_init(&path); fb_buf_init(&safe);
    lib_path(&path, dir, name);
    fbf_safe_name(name, &safe);
    FbJson *copy = fbj_clone(flow);
    fbf_migrate(copy);
    char ts[32];
    timestamp(ts, sizeof ts, "%Y-%m-%d %H:%M:%S");
    fbj_obj_set(copy, "updated", fbj_str(ts));
    int rc = 0;
    if (keep_versions > 0 && fb_file_exists(fb_buf_cstr(&path))) {
        FbBuf hist, hf;
        fb_buf_init(&hist); fb_buf_init(&hf);
        fb_join_path(&hist, dir, ".history");
        fb_mkdirs(fb_buf_cstr(&hist));
        char stamp[32];
        timestamp(stamp, sizeof stamp, "%Y%m%d-%H%M%S");
        FbBuf leaf;
        fb_buf_init(&leaf);
        fb_buf_printf(&leaf, "%s__%s.fbflow", fb_buf_cstr(&safe), stamp);
        fb_join_path(&hf, fb_buf_cstr(&hist), fb_buf_cstr(&leaf));
        fb_copy_file(fb_buf_cstr(&path), fb_buf_cstr(&hf));
        prune_history(fb_buf_cstr(&hist), fb_buf_cstr(&safe), keep_versions);
        fb_buf_free(&hist); fb_buf_free(&hf); fb_buf_free(&leaf);
    }
    char *text = fbj_to_string(copy, 1);
    if (fb_write_file(fb_buf_cstr(&path), text, strlen(text)) != 0) {
        fb_set_err(err, errsz, "Cannot write %s", fb_buf_cstr(&path));
        rc = -1;
    }
    free(text);
    fbj_free(copy);
    fb_buf_free(&path); fb_buf_free(&safe);
    return rc;
}

int fbf_lib_load(const char *dir, const char *name, FbJson **flow, char *err, size_t errsz)
{
    FbBuf path, data;
    fb_buf_init(&path); fb_buf_init(&data);
    lib_path(&path, dir, name);
    *flow = NULL;
    int rc = -1;
    if (fb_read_file(fb_buf_cstr(&path), &data) != 0) fb_set_err(err, errsz, "Flow '%s' was not found", name);
    else {
        *flow = fbj_parse(fb_buf_cstr(&data), data.len, err, errsz);
        if (*flow) { fbf_migrate(*flow); rc = 0; }
    }
    fb_buf_free(&path); fb_buf_free(&data);
    return rc;
}

int fbf_lib_delete(const char *dir, const char *name)
{
    FbBuf path;
    fb_buf_init(&path);
    lib_path(&path, dir, name);
    int rc = fb_delete_file(fb_buf_cstr(&path));
    fb_buf_free(&path);
    return rc;
}

/* ================================================================== backup */

FbJson *fbf_backup_create(const char *dir, const FbJson *extra, char *err, size_t errsz)
{
    FbJson *b = fbj_obj();
    char ts[32];
    timestamp(ts, sizeof ts, "%Y-%m-%d %H:%M:%S");
    fbj_obj_set(b, "format", fbj_str("flowbrowser-backup"));
    fbj_obj_set(b, "version", fbj_num(1));
    fbj_obj_set(b, "created", fbj_str(ts));
    FbJson *flows = fbj_obj();
    NameList l = { NULL, 0, 0, ".fbflow" };
    fb_list_dir(dir, collect_cb, &l);
    qsort(l.names, l.n, sizeof(char *), cmp_ci);
    for (size_t i = 0; i < l.n; i++) {
        FbBuf path, data;
        fb_buf_init(&path); fb_buf_init(&data);
        fb_join_path(&path, dir, l.names[i]);
        if (fb_read_file(fb_buf_cstr(&path), &data) == 0) {
            FbJson *f = fbj_parse(fb_buf_cstr(&data), data.len, NULL, 0);
            if (f) {
                char base[256];
                snprintf(base, sizeof base, "%.*s", (int)(strlen(l.names[i]) - 7), l.names[i]);
                fbj_obj_set(flows, base, f);
            }
        }
        fb_buf_free(&path); fb_buf_free(&data);
    }
    names_free(&l);
    fbj_obj_set(b, "flows", flows);
    if (extra && extra->type == FBJ_OBJ)
        for (size_t i = 0; i < extra->count; i++) fbj_obj_set(b, extra->keys[i], fbj_clone(extra->items[i]));
    (void)err; (void)errsz;
    return b;
}

int fbf_backup_restore(const FbJson *backup, const char *dir, int mode, FbJson **report, char *err, size_t errsz)
{
    *report = fbj_arr();
    if (!backup || backup->type != FBJ_OBJ || strcmp(fbj_get_str(backup, "format", ""), "flowbrowser-backup") != 0) {
        fb_set_err(err, errsz, "This file is not a FlowBrowser backup");
        return -1;
    }
    const FbJson *flows = fbj_get(backup, "flows");
    if (!flows || flows->type != FBJ_OBJ) { fb_set_err(err, errsz, "The backup contains no flows"); return -1; }
    fb_mkdirs(dir);
    for (size_t i = 0; i < flows->count; i++) {
        const char *name = flows->keys[i];
        FbBuf path;
        fb_buf_init(&path);
        lib_path(&path, dir, name);
        int exists = fb_file_exists(fb_buf_cstr(&path));
        const char *action = exists ? "overwritten" : "added";
        char target[300];
        snprintf(target, sizeof target, "%s", name);
        if (exists && mode == 0) action = "skipped";
        else if (exists && mode == 2) {
            for (int k = 2; k < 1000; k++) {
                snprintf(target, sizeof target, "%s (restored %d)", name, k);
                fb_buf_clear(&path);
                lib_path(&path, dir, target);
                if (!fb_file_exists(fb_buf_cstr(&path))) break;
            }
            action = "renamed";
        }
        if (strcmp(action, "skipped") != 0) {
            char *text = fbj_to_string(flows->items[i], 1);
            if (fb_write_file(fb_buf_cstr(&path), text, strlen(text)) != 0) action = "failed";
            free(text);
        }
        FbJson *r = fbj_obj();
        fbj_obj_set(r, "name", fbj_str(name));
        fbj_obj_set(r, "action", fbj_str(action));
        if (!strcmp(action, "renamed")) fbj_obj_set(r, "as", fbj_str(target));
        fbj_arr_push(*report, r);
        fb_buf_free(&path);
    }
    return 0;
}
