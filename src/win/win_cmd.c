/* FlowBrowser for Windows - commands from the HTML UI (shell + Studio panel): storage, library, backup, files,
   Excel preview, scheduling, page tools, browsing. The protocol is documented in docs/PROTOCOL.md. */
#include "win_app.h"
#include "fb_xlsx.h"
#include <stdarg.h>

/* ------------------------------------------------------------------ replies */
void fbw_reply(int src, const FbJson *req, FbJson *extra)
{
    FbJson *o = extra && extra->type == FBJ_OBJ ? extra : fbj_obj();
    if (extra && extra != o) fbj_free(extra);
    const FbJson *id = fbj_get(req, "id");
    if (!id) { fbj_free(o); return; }
    fbj_obj_set(o, "id", fbj_clone(id));
    fbj_obj_set(o, "ok", fbj_bool(1));
    char *s = fbj_to_string(o, 0);
    fbj_free(o);
    if (s) { fbw_post(src, s); free(s); }
}

void fbw_reply_err(int src, const FbJson *req, const char *msg)
{
    const FbJson *id = fbj_get(req, "id");
    if (!id) return;
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "id", fbj_clone(id));
    fbj_obj_set(o, "ok", fbj_bool(0));
    fbj_obj_set(o, "error", fbj_str(msg && msg[0] ? msg : "failed"));
    char *s = fbj_to_string(o, 0);
    fbj_free(o);
    if (s) { fbw_post(src, s); free(s); }
}

/* an answer that arrives later (page scripts, DevTools calls) */
typedef struct { int src; int has_id; FbJson *id; int mode; } Async;
static Async *async_new(int src, const FbJson *req, int mode)
{
    Async *a = (Async *)calloc(1, sizeof *a);
    if (!a) return NULL;
    a->src = src; a->mode = mode;
    const FbJson *id = fbj_get(req, "id");
    if (id) { a->has_id = 1; a->id = fbj_clone(id); }
    return a;
}
static void async_done(Async *a, FbJson *extra, const char *err)
{
    if (a->has_id) {
        FbJson *o = extra && extra->type == FBJ_OBJ ? extra : fbj_obj();
        fbj_obj_set(o, "id", fbj_clone(a->id));
        fbj_obj_set(o, "ok", fbj_bool(err == NULL));
        if (err) fbj_obj_set(o, "error", fbj_str(err));
        char *s = fbj_to_string(o, 0);
        if (s) { fbw_post(a->src, s); free(s); }
        fbj_free(o);
    } else if (extra) fbj_free(extra);
    fbj_free(a->id);
    free(a);
}

#define STR(j, k, d) fbj_get_str((j), (k), (d))

/* ------------------------------------------------------------------ key / value store (one JSON file per key) */
static char *store_path(const char *key)
{
    char safe[96];
    size_t n = 0;
    for (const char *p = key; *p && n < sizeof safe - 1; p++)
        safe[n++] = (isalnum((unsigned char)*p) || *p == '-' || *p == '_') ? *p : '_';
    safe[n] = 0;
    if (!n) return NULL;
    return fbw_dupf("%s\\%s.json", G.store_dir, safe);
}

char *fbw_store_read(const char *key)
{
    char *p = store_path(key);
    if (!p) return NULL;
    FbBuf b;
    fb_buf_init(&b);
    char *out = NULL;
    if (fb_read_file(p, &b) == 0 && b.len) out = fb_buf_detach(&b);
    fb_buf_free(&b);
    free(p);
    return out;
}

int fbw_store_write(const char *key, const char *text)
{
    char *p = store_path(key);
    if (!p) return -1;
    int rc = fb_write_file(p, text, strlen(text));
    free(p);
    return rc;
}

/* ------------------------------------------------------------------ settings the host acts on */
void fbw_settings_apply(const FbJson *s)
{
    if (!s || s->type != FBJ_OBJ) return;
    const char *ua = STR(s, "userAgent", "");
    int ua_changed = strcmp(ua, G.user_agent ? G.user_agent : "") != 0;
    free(G.user_agent);
    G.user_agent = fb_strdup(ua);
    G.popups_block = strcmp(STR(s, "popups", "tab"), "block") == 0;
    G.perm_deny = strcmp(STR(s, "permissions", "ask"), "deny") == 0;
    G.clear_on_exit = fbj_get_bool(s, "clearOnExit", 0);
    G.auto_backup = fbj_get_bool(s, "autoBackup", 1);
    G.keep_versions = (int)fbj_get_num(s, "keepVersions", 10);
    if (G.keep_versions < 0) G.keep_versions = 0;
    double z = fbj_get_num(s, "defaultZoom", 100);
    G.default_zoom = z >= 25 && z <= 500 ? z / 100.0 : 1.0;
    free(G.home_url);
    G.home_url = fb_strdup(STR(s, "home", ""));
    if (G.home_url[0] && !fb_starts_with(G.home_url, "http://") && !fb_starts_with(G.home_url, "https://") && !fb_starts_with(G.home_url, "file:")) { free(G.home_url); G.home_url = fb_strdup(""); }
    if (ua_changed) {
        for (int i = 0; i < G.ntabs; i++) {
            if (G.user_agent[0]) fbw_apply_user_agent(G.tabs[i]);
            else fbw_cdp(G.tabs[i], "Emulation.setUserAgentOverride", "{\"userAgent\":\"\"}", NULL, NULL);
        }
    }
}

void fbw_settings_load_early(void)
{
    char *txt = fbw_store_read("settings");
    if (!txt) return;
    FbJson *j = fbj_parse_str(txt, NULL, 0);
    free(txt);
    if (!j) return;
    fbw_settings_apply(j);
    /* the proxy is a start-up option of the browser engine; only plain host:port / scheme://host:port is accepted */
    const char *px = STR(j, "proxy", "");
    if (px[0]) {
        int ok = strlen(px) < 200;
        for (const char *p = px; ok && *p; p++) if (!(isalnum((unsigned char)*p) || strchr(".:/-_@[]", *p))) ok = 0;
        if (ok) {
            char *arg = fbw_dupf("--proxy-server=%s", px);
            wchar_t *w = fbw_w(arg);
            if (w) { SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", w); free(w); }
            free(arg);
        }
    }
    fbj_free(j);
}

/* ------------------------------------------------------------------ helpers: file lists */
typedef struct { char **names; size_t n, cap; const char *prefix, *suffix; } NameList;

static int name_cb(const char *name, int is_dir, void *ud)
{
    NameList *l = (NameList *)ud;
    if (is_dir) return 0;
    if (l->prefix && !fb_starts_with(name, l->prefix)) return 0;
    if (l->suffix && !fb_ends_with(name, l->suffix)) return 0;
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 32;
        char **nn = (char **)realloc(l->names, nc * sizeof(char *));
        if (!nn) return 1;
        l->names = nn; l->cap = nc;
    }
    l->names[l->n++] = fb_strdup(name);
    return 0;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void names_free(NameList *l) { for (size_t i = 0; i < l->n; i++) free(l->names[i]); free(l->names); }

static int write_json_file(const char *path, const FbJson *j)
{
    char *s = fbj_to_string(j, 1);
    if (!s) return -1;
    int rc = fb_write_file(path, s, strlen(s));
    free(s);
    return rc;
}
static FbJson *read_json_file(const char *path, char *err, size_t errsz)
{
    FbBuf b;
    fb_buf_init(&b);
    if (fb_read_file(path, &b)) { fb_set_err(err, errsz, "Could not read the file"); fb_buf_free(&b); return NULL; }
    FbJson *j = fbj_parse(fb_buf_cstr(&b), b.len, err, errsz);
    fb_buf_free(&b);
    return j;
}

/* ------------------------------------------------------------------ backups */
void fbw_auto_backup(void)
{
    if (!G.auto_backup || G.cli_run || !G.flows_dir[0]) return;
    char err[200];
    FbJson *b = fbf_backup_create(G.flows_dir, NULL, err, sizeof err);
    if (!b) return;
    const FbJson *flows = fbj_get(b, "flows");
    if (fbj_len(flows) > 0) {
        char *ts = fbw_timestamp("%Y%m%d-%H%M%S");
        char *path = fbw_dupf("%s\\auto-%s.fbbackup", G.backup_dir, ts ? ts : "x");
        if (path) { write_json_file(path, b); free(path); }
        free(ts);
    }
    fbj_free(b);
    NameList l = { NULL, 0, 0, "auto-", ".fbbackup" };
    fb_list_dir(G.backup_dir, name_cb, &l);
    qsort(l.names, l.n, sizeof(char *), cmp_str);
    for (size_t i = 0; i + 7 < l.n; i++) {
        char *p = fbw_dupf("%s\\%s", G.backup_dir, l.names[i]);
        if (p) { fb_delete_file(p); free(p); }
    }
    names_free(&l);
}

/* ------------------------------------------------------------------ OEM console output -> UTF-8 */
static char *oem_to_u8(const char *s, size_t n)
{
    int wn = MultiByteToWideChar(CP_OEMCP, 0, s, (int)n, NULL, 0);
    wchar_t *w = (wchar_t *)calloc((size_t)wn + 1, sizeof(wchar_t));
    if (!w) return NULL;
    if (wn > 0) MultiByteToWideChar(CP_OEMCP, 0, s, (int)n, w, wn);
    char *u = fbw_u8(w);
    free(w);
    return u;
}

/* ------------------------------------------------------------------ scheduling (Windows Task Scheduler through schtasks.exe) */
static char *task_name_for(const char *flow) { FbBuf b; fb_buf_init(&b); fbf_safe_name(flow, &b); char *r = fb_buf_detach(&b); return r; }

static int valid_time(const char *t)
{
    return strlen(t) == 5 && isdigit((unsigned char)t[0]) && isdigit((unsigned char)t[1]) && t[2] == ':' && isdigit((unsigned char)t[3]) && isdigit((unsigned char)t[4]) &&
           ((t[0] - '0') * 10 + (t[1] - '0')) < 24 && ((t[3] - '0') * 10 + (t[4] - '0')) < 60;
}

static void cmd_schedule_create(int src, const FbJson *m)
{
    const char *when = STR(m, "when", "daily"), *time = STR(m, "time", "09:00"), *day = STR(m, "day", "MON");
    char *safe = task_name_for(STR(m, "name", ""));
    if (!safe || !safe[0]) { free(safe); fbw_reply_err(src, m, "Which flow should run?"); return; }
    if (!valid_time(time)) { free(safe); fbw_reply_err(src, m, "Time must look like 09:30 (24 hour clock)."); return; }
    static const char *days[] = { "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN" };
    int dok = 0;
    for (int i = 0; i < 7; i++) if (!strcmp(day, days[i])) dok = 1;
    const char *sc = !strcmp(when, "daily") ? "DAILY" : !strcmp(when, "weekly") ? "WEEKLY" : !strcmp(when, "hourly") ? "HOURLY" : !strcmp(when, "logon") ? "ONLOGON" : !strcmp(when, "once") ? "ONCE" : NULL;
    if (!sc || (!strcmp(sc, "WEEKLY") && !dok)) { free(safe); fbw_reply_err(src, m, "Unknown schedule."); return; }

    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(NULL, exe, (DWORD)(sizeof exe / sizeof exe[0]));
    char *exe8 = fbw_u8(exe);
    char *line = fbw_dupf("schtasks.exe /Create /F /TN \"FlowBrowser\\%s\" /TR \"\\\"%s\\\" --run \\\"%s\\\" --exit\" /SC %s%s%s%s%s", safe, exe8, safe, sc,
                          !strcmp(sc, "WEEKLY") ? " /D " : "", !strcmp(sc, "WEEKLY") ? day : "", strcmp(sc, "ONLOGON") ? " /ST " : "", strcmp(sc, "ONLOGON") ? time : "");
    wchar_t *wl = fbw_w(line);
    FbBuf out;
    fb_buf_init(&out);
    int rc = wl ? fbw_run_capture(wl, &out, 20000) : -1;
    if (rc == 0) fbw_reply(src, m, NULL);
    else {
        char *msg = oem_to_u8(out.data ? out.data : "", out.len);
        char *t = msg ? fb_trim(msg) : NULL;
        fbw_reply_err(src, m, t && t[0] ? t : "Windows Task Scheduler refused the task.");
        free(msg);
    }
    fb_buf_free(&out); free(wl); free(line); free(exe8); free(safe);
}

/* first fields of one CSV line: "a","b","c" */
static int csv_fields(char *line, char **f, int max)
{
    int n = 0;
    char *p = line;
    while (*p && n < max) {
        if (*p == '"') {
            char *w = ++p;
            f[n++] = w;
            while (*p) {
                if (*p == '"' && p[1] == '"') { *w++ = '"'; p += 2; }
                else if (*p == '"') { p++; break; }
                else *w++ = *p++;
            }
            *w = 0;
            if (*p == ',') p++;
        } else {
            f[n++] = p;
            while (*p && *p != ',') p++;
            if (*p) *p++ = 0;
        }
    }
    return n;
}

static void cmd_schedule_list(int src, const FbJson *m)
{
    FbBuf out;
    fb_buf_init(&out);
    int rc = fbw_run_capture(L"schtasks.exe /Query /FO CSV /NH", &out, 20000);
    FbJson *tasks = fbj_arr();
    if (rc == 0 || out.len) {
        char *txt = oem_to_u8(out.data ? out.data : "", out.len);
        char *line = txt;
        while (line && *line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            size_t ll = strlen(line);
            if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
            char *f[4] = { 0 };
            int n = csv_fields(line, f, 4);
            if (n >= 1 && f[0] && fb_starts_with(f[0], "\\FlowBrowser\\")) {
                int dup = 0;
                for (size_t i = 0; i < fbj_len(tasks); i++) if (!strcmp(STR(tasks->items[i], "name", ""), f[0] + 13)) dup = 1;
                if (!dup) {
                    FbJson *t = fbj_obj();
                    fbj_obj_set(t, "name", fbj_str(f[0] + 13));
                    fbj_obj_set(t, "next", fbj_str(n >= 2 && f[1] ? f[1] : ""));
                    fbj_arr_push(tasks, t);
                }
            }
            line = nl ? nl + 1 : NULL;
        }
        free(txt);
    }
    fb_buf_free(&out);
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "tasks", tasks);
    fbw_reply(src, m, o);
}

static void cmd_schedule_delete(int src, const FbJson *m)
{
    char *safe = task_name_for(STR(m, "name", ""));
    char *line = fbw_dupf("schtasks.exe /Delete /F /TN \"FlowBrowser\\%s\"", safe ? safe : "");
    wchar_t *wl = fbw_w(line);
    FbBuf out;
    fb_buf_init(&out);
    int rc = wl ? fbw_run_capture(wl, &out, 15000) : -1;
    if (rc == 0) fbw_reply(src, m, NULL); else fbw_reply_err(src, m, "The scheduled task could not be removed.");
    fb_buf_free(&out); free(wl); free(line); free(safe);
}

/* ------------------------------------------------------------------ Excel / CSV preview */
static void cmd_data_preview(int src, const FbJson *m)
{
    const char *file = STR(m, "file", ""), *sheet = STR(m, "sheet", ""), *range = STR(m, "range", "");
    int limit = (int)fbj_get_num(m, "limit", 200);
    if (limit < 1) limit = 200;
    if (!file[0]) { fbw_reply_err(src, m, "Choose a file first."); return; }
    char err[300] = "";
    FbGrid g;
    fbgrid_init(&g);
    FbJson *sheets = fbj_arr();
    const char *used_sheet = "";
    char *first_sheet = NULL;
    int is_csv = fb_ends_with(file, ".csv") || fb_ends_with(file, ".CSV") || fb_ends_with(file, ".txt") || fb_ends_with(file, ".tsv");
    if (is_csv) {
        FbBuf b;
        fb_buf_init(&b);
        if (fb_read_file(file, &b)) { fbj_free(sheets); fb_buf_free(&b); fbw_reply_err(src, m, "That file could not be opened."); return; }
        fbcsv_parse(fb_buf_cstr(&b), b.len, &g);
        fb_buf_free(&b);
    } else {
        FbXEdit *x = fbxe_open(file, 0, err, sizeof err);
        if (!x) { fbj_free(sheets); fbw_reply_err(src, m, err[0] ? err : "That file could not be opened."); return; }
        FbJson *names = fbxe_sheet_names(x);
        if (names) {
            for (size_t i = 0; i < names->count; i++) fbj_arr_push(sheets, fbj_str(names->items[i]->str));
            if (names->count) first_sheet = fb_strdup(names->items[0]->str);
            fbj_free(names);
        }
        used_sheet = sheet[0] ? sheet : (first_sheet ? first_sheet : "");
        int rc = fbxe_get_grid(x, used_sheet[0] ? used_sheet : NULL, &g, err, sizeof err);
        fbxe_close(x);
        if (rc) { fbj_free(sheets); free(first_sheet); fbw_reply_err(src, m, err[0] ? err : "That sheet could not be read."); return; }
    }
    int r1 = 1, c1 = 1, r2 = 0, c2 = 0;
    if (range[0] && fbx_parse_range(range, &r1, &c1, &r2, &c2)) {
        fbgrid_free(&g); fbj_free(sheets); free(first_sheet);
        fbw_reply_err(src, m, "That is not a valid range. Example: A1:D20");
        return;
    }
    size_t ncols = fbgrid_ncols(&g);
    size_t last_r = r2 > 0 && (size_t)r2 < g.nrows ? (size_t)r2 : g.nrows;
    size_t last_c = c2 > 0 && (size_t)c2 < ncols ? (size_t)c2 : ncols;
    FbJson *rows = fbj_arr();
    size_t total = 0;
    for (size_t r = (size_t)(r1 > 0 ? r1 - 1 : 0); r < last_r; r++) {
        total++;
        if ((int)fbj_len(rows) >= limit) continue;
        FbJson *row = fbj_arr();
        for (size_t c = (size_t)(c1 > 0 ? c1 - 1 : 0); c < last_c; c++) {
            const char *v = fbgrid_get(&g, r, c);
            fbj_arr_push(row, fbj_str(v ? v : ""));
        }
        fbj_arr_push(rows, row);
    }
    fbgrid_free(&g);
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "sheets", sheets);
    fbj_obj_set(o, "sheet", fbj_str(used_sheet));
    fbj_obj_set(o, "rows", rows);
    fbj_obj_set(o, "total_rows", fbj_num((double)total));
    free(first_sheet);
    fbw_reply(src, m, o);
}

/* ------------------------------------------------------------------ library */
static int valid_version_id(const char *s)
{
    if (strlen(s) != 15 || s[8] != '-') return 0;
    for (int i = 0; i < 15; i++) if (i != 8 && !isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static void cmd_lib_versions(int src, const FbJson *m)
{
    FbBuf safe;
    fb_buf_init(&safe);
    fbf_safe_name(STR(m, "name", ""), &safe);
    char *hist = fbw_dupf("%s\\.history", G.flows_dir);
    char *prefix = fbw_dupf("%s__", fb_buf_cstr(&safe));
    NameList l = { NULL, 0, 0, prefix, ".fbflow" };
    fb_list_dir(hist, name_cb, &l);
    qsort(l.names, l.n, sizeof(char *), cmp_str);
    FbJson *arr = fbj_arr();
    size_t pl = strlen(prefix);
    for (size_t i = l.n; i-- > 0;) {
        const char *n = l.names[i];
        size_t len = strlen(n);
        if (len < pl + 7 + 15) continue;
        char id[16];
        memcpy(id, n + pl, 15);
        id[15] = 0;
        if (!valid_version_id(id)) continue;
        FbJson *e = fbj_obj();
        char when[32];
        snprintf(when, sizeof when, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s", id, id + 4, id + 6, id + 9, id + 11, id + 13);
        fbj_obj_set(e, "id", fbj_str(id));
        fbj_obj_set(e, "when", fbj_str(when));
        fbj_arr_push(arr, e);
    }
    names_free(&l);
    free(prefix); free(hist);
    fb_buf_free(&safe);
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "versions", arr);
    fbw_reply(src, m, o);
}

static void cmd_lib_load(int src, const FbJson *m)
{
    char err[300] = "";
    FbJson *flow = NULL;
    const char *ver = STR(m, "version", "");
    if (ver[0]) {
        if (!valid_version_id(ver)) { fbw_reply_err(src, m, "Unknown version."); return; }
        FbBuf safe;
        fb_buf_init(&safe);
        fbf_safe_name(STR(m, "name", ""), &safe);
        char *p = fbw_dupf("%s\\.history\\%s__%s.fbflow", G.flows_dir, fb_buf_cstr(&safe), ver);
        fb_buf_free(&safe);
        flow = p ? read_json_file(p, err, sizeof err) : NULL;
        free(p);
        if (flow) fbf_migrate(flow);
    } else if (fbf_lib_load(G.flows_dir, STR(m, "name", ""), &flow, err, sizeof err)) flow = NULL;
    if (!flow) { fbw_reply_err(src, m, err[0] ? err : "That flow could not be opened."); return; }
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "flow", flow);
    fbw_reply(src, m, o);
}

static void cmd_lib_save(int src, const FbJson *m)
{
    const FbJson *flow = fbj_get(m, "flow");
    const char *name = STR(m, "name", "");
    char err[300] = "";
    if (!flow || !name[0]) { fbw_reply_err(src, m, "Give the flow a name before saving."); return; }
    int keep = fbj_get(m, "keep_versions") ? (int)fbj_get_num(m, "keep_versions", 0) : G.keep_versions;
    if (fbf_lib_save(G.flows_dir, name, flow, keep, err, sizeof err)) { fbw_reply_err(src, m, err[0] ? err : "The flow could not be saved."); return; }
    FbBuf safe;
    fb_buf_init(&safe);
    fbf_safe_name(name, &safe);
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "name", fbj_str(fb_buf_cstr(&safe)));
    fb_buf_free(&safe);
    fbw_reply(src, m, o);
}

/* ------------------------------------------------------------------ files */
static void cmd_file_dialog(int src, const FbJson *m)
{
    const char *mode = STR(m, "mode", "open");
    FbJson *o = fbj_obj();
    if (!strcmp(mode, "save")) {
        char *path = NULL;
        if (fbw_save_dialog(G.hwnd, STR(m, "title", "Save"), fbj_get(m, "filters"), STR(m, "name", ""), &path) == 0 && path) fbj_obj_set(o, "path", fbj_str(path));
        else fbj_obj_set(o, "path", fbj_null());
        free(path);
    } else {
        FbJson *paths = NULL;
        if (fbw_open_dialog(G.hwnd, STR(m, "title", "Open"), fbj_get(m, "filters"), fbj_get_bool(m, "multi", 0), &paths) == 0 && paths && fbj_len(paths)) {
            fbj_obj_set(o, "path", fbj_str(fbj_at(paths, 0)->str));
            fbj_obj_set(o, "paths", paths);
        } else {
            fbj_free(paths);
            fbj_obj_set(o, "path", fbj_null());
            fbj_obj_set(o, "paths", fbj_arr());
        }
    }
    fbw_reply(src, m, o);
}

static void cmd_flow_export(int src, const FbJson *m)
{
    const FbJson *flow = fbj_get(m, "flow");
    if (!flow) { fbw_reply_err(src, m, "Nothing to export."); return; }
    FbBuf safe;
    fb_buf_init(&safe);
    fbf_safe_name(STR(m, "name", STR(flow, "name", "flow")), &safe);
    fb_buf_puts(&safe, ".fbflow");
    FbJson *filters = fbj_parse_str("[[\"FlowBrowser flow\",\"*.fbflow\"],[\"All files\",\"*.*\"]]", NULL, 0);
    char *path = NULL;
    FbJson *o = fbj_obj();
    if (fbw_save_dialog(G.hwnd, "Export flow", filters, fb_buf_cstr(&safe), &path) == 0 && path) {
        if (write_json_file(path, flow)) { fbj_free(o); fbj_free(filters); free(path); fb_buf_free(&safe); fbw_reply_err(src, m, "The file could not be written."); return; }
        fbj_obj_set(o, "path", fbj_str(path));
    } else fbj_obj_set(o, "path", fbj_null());
    free(path); fbj_free(filters); fb_buf_free(&safe);
    fbw_reply(src, m, o);
}

static void cmd_flow_import(int src, const FbJson *m)
{
    FbJson *filters = fbj_parse_str("[[\"FlowBrowser flow\",\"*.fbflow;*.json\"],[\"All files\",\"*.*\"]]", NULL, 0);
    FbJson *paths = NULL;
    FbJson *o = fbj_obj();
    if (fbw_open_dialog(G.hwnd, "Open a flow file", filters, 0, &paths) == 0 && paths && fbj_len(paths)) {
        char err[300];
        FbJson *flow = read_json_file(fbj_at(paths, 0)->str, err, sizeof err);
        if (!flow || flow->type != FBJ_OBJ) { fbj_free(flow); fbj_free(o); fbj_free(paths); fbj_free(filters); fbw_reply_err(src, m, "That file is not a FlowBrowser flow."); return; }
        if (!strcmp(STR(flow, "format", ""), "flowbrowser-backup")) { fbj_free(flow); fbj_free(o); fbj_free(paths); fbj_free(filters); fbw_reply_err(src, m, "That is a backup file. Use Restore in the Library tab."); return; }
        if (!fbj_get(flow, "steps")) { fbj_free(flow); fbj_free(o); fbj_free(paths); fbj_free(filters); fbw_reply_err(src, m, "That file is not a FlowBrowser flow."); return; }
        fbf_migrate(flow);
        fbj_obj_set(o, "flow", flow);
        fbj_obj_set(o, "path", fbj_str(fbj_at(paths, 0)->str));
    } else fbj_obj_set(o, "flow", fbj_null());
    fbj_free(paths); fbj_free(filters);
    fbw_reply(src, m, o);
}

static void cmd_backup_create(int src, const FbJson *m)
{
    char err[300] = "";
    FbJson *b = fbf_backup_create(G.flows_dir, NULL, err, sizeof err);
    if (!b) { fbw_reply_err(src, m, err[0] ? err : "The backup could not be made."); return; }
    char *ts = fbw_timestamp("%Y%m%d-%H%M%S");
    char *name = fbw_dupf("flowbrowser-backup-%s.fbbackup", ts ? ts : "now");
    FbJson *filters = fbj_parse_str("[[\"FlowBrowser backup\",\"*.fbbackup\"],[\"All files\",\"*.*\"]]", NULL, 0);
    char *path = NULL;
    FbJson *o = fbj_obj();
    if (fbw_save_dialog(G.hwnd, "Save a backup of all flows", filters, name, &path) == 0 && path) {
        if (write_json_file(path, b)) { fbj_free(o); fbj_free(b); fbj_free(filters); free(path); free(name); free(ts); fbw_reply_err(src, m, "The backup file could not be written."); return; }
        fbj_obj_set(o, "path", fbj_str(path));
        fbj_obj_set(o, "count", fbj_num((double)fbj_len(fbj_get(b, "flows"))));
    } else fbj_obj_set(o, "path", fbj_null());
    free(path); free(name); free(ts); fbj_free(filters); fbj_free(b);
    fbw_reply(src, m, o);
}

static void cmd_backup_restore(int src, const FbJson *m)
{
    FbJson *filters = fbj_parse_str("[[\"FlowBrowser backup\",\"*.fbbackup;*.json\"],[\"All files\",\"*.*\"]]", NULL, 0);
    FbJson *paths = NULL;
    FbJson *o = fbj_obj();
    int mode = (int)fbj_get_num(m, "mode", 0);
    if (mode < 0 || mode > 2) mode = 0;
    if (fbw_open_dialog(G.hwnd, "Choose a backup file", filters, 0, &paths) == 0 && paths && fbj_len(paths)) {
        char err[300] = "";
        FbJson *b = read_json_file(fbj_at(paths, 0)->str, err, sizeof err);
        FbJson *rep = NULL;
        if (!b || strcmp(STR(b, "format", ""), "flowbrowser-backup") != 0) {
            fbj_free(b); fbj_free(o); fbj_free(paths); fbj_free(filters);
            fbw_reply_err(src, m, "That file is not a FlowBrowser backup.");
            return;
        }
        if (fbf_backup_restore(b, G.flows_dir, mode, &rep, err, sizeof err)) {
            fbj_free(b); fbj_free(rep); fbj_free(o); fbj_free(paths); fbj_free(filters);
            fbw_reply_err(src, m, err[0] ? err : "The backup could not be restored.");
            return;
        }
        int added = 0, replaced = 0, skipped = 0, renamed = 0;
        for (size_t i = 0; rep && i < fbj_len(rep); i++) {
            const char *a = STR(rep->items[i], "action", "");
            if (!strcmp(a, "added")) added++; else if (!strcmp(a, "overwritten")) replaced++; else if (!strcmp(a, "skipped")) skipped++; else if (!strcmp(a, "renamed")) renamed++;
        }
        FbJson *r = fbj_obj();
        fbj_obj_set(r, "added", fbj_num(added));
        fbj_obj_set(r, "replaced", fbj_num(replaced));
        fbj_obj_set(r, "skipped", fbj_num(skipped));
        fbj_obj_set(r, "renamed", fbj_num(renamed));
        fbj_obj_set(o, "report", r);
        fbj_obj_set(o, "path", fbj_str(fbj_at(paths, 0)->str));
        fbj_free(b); fbj_free(rep);
    } else fbj_obj_set(o, "path", fbj_null());
    fbj_free(paths); fbj_free(filters);
    fbw_reply(src, m, o);
}

static void cmd_save_text(int src, const FbJson *m)
{
    FbJson *filters = fbj_parse_str("[[\"Text file\",\"*.txt\"],[\"All files\",\"*.*\"]]", NULL, 0);
    char *path = NULL;
    FbJson *o = fbj_obj();
    if (fbw_save_dialog(G.hwnd, STR(m, "title", "Save"), filters, STR(m, "name", "log.txt"), &path) == 0 && path) {
        const char *t = STR(m, "text", "");
        if (fb_write_file(path, t, strlen(t))) { fbj_free(o); fbj_free(filters); free(path); fbw_reply_err(src, m, "The file could not be written."); return; }
        fbj_obj_set(o, "path", fbj_str(path));
    } else fbj_obj_set(o, "path", fbj_null());
    free(path); fbj_free(filters);
    fbw_reply(src, m, o);
}

static void set_clipboard(const char *text)
{
    wchar_t *w = fbw_w(text);
    if (!w) return;
    size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (g) {
        void *p = GlobalLock(g);
        if (p) { memcpy(p, w, bytes); GlobalUnlock(g); }
        if (OpenClipboard(G.hwnd)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
            CloseClipboard();
        } else GlobalFree(g);
    }
    free(w);
}

/* ------------------------------------------------------------------ page scripts (panel -> active tab) */
static void on_page_eval(void *ctx, void *hr, void *jsonw)
{
    Async *a = (Async *)ctx;
    if (FAILED((HRESULT)(INT_PTR)hr) || !jsonw) { async_done(a, NULL, "The page did not answer. Reload the page and try again."); return; }
    char *u = fbw_u8((const wchar_t *)jsonw);
    FbJson *res = u ? fbj_parse_str(u, NULL, 0) : NULL;
    free(u);
    const FbJson *ex = fbj_get(res, "exceptionDetails");
    if (ex) {
        const FbJson *exc = fbj_get(ex, "exception");
        char *msg = fbw_dupf("%s", STR(exc, "description", STR(ex, "text", "The page script failed")));
        async_done(a, NULL, msg);
        free(msg);
        fbj_free(res);
        return;
    }
    const FbJson *val = fbj_get(fbj_get(res, "result"), "value");
    FbJson *o = fbj_obj();
    if (a->mode == 0) {      /* page.call: a JSON string */
        if (val && val->type == FBJ_STR) fbj_obj_set(o, "result", fbj_str(val->str));
        else if (val) { char *s = fbj_to_string(val, 0); fbj_obj_set(o, "result", fbj_str(s ? s : "{}")); free(s); }
        else fbj_obj_set(o, "result", fbj_str("{}"));
    } else {                 /* page.exec: the object */
        fbj_obj_set(o, "result", val ? fbj_clone(val) : fbj_null());
    }
    fbj_free(res);
    async_done(a, o, NULL);
}

static void cmd_page(int src, const FbJson *m, int is_call)
{
    FbwTab *t = fbw_active_tab();
    if (!t || !t->wv) { fbw_reply_err(src, m, "There is no web page open in the active tab."); return; }
    Async *a = async_new(src, m, is_call ? 0 : 1);
    if (!a) return;
    char *expr;
    if (is_call) {
        FbJson *nm = fbj_str(STR(m, "name", ""));
        char *name = fbj_to_string(nm, 0);
        fbj_free(nm);
        char *argtxt = fbj_get(m, "arg") ? fbj_to_string(fbj_get(m, "arg"), 0) : fb_strdup("{}");
        FbJson *al = fbj_str(argtxt ? argtxt : "{}");
        char *arg = fbj_to_string(al, 0);
        fbj_free(al);
        expr = fbw_dupf("window.__fb&&window.__fb.tools?window.__fb.tools.call(%s,%s):JSON.stringify({ok:false,error:'The page tools are not loaded in this tab. Reload the page.'})", name, arg);
        free(name); free(argtxt); free(arg);
    } else {
        FbJson *op = fbj_str(STR(m, "op", ""));
        char *ops = fbj_to_string(op, 0);
        fbj_free(op);
        char *args = fbj_get(m, "args") ? fbj_to_string(fbj_get(m, "args"), 0) : fb_strdup("{}");
        expr = fbw_dupf("window.__fb?window.__fb.exec(%s,%s):({ok:false,code:'no_runtime',error:'The page tools are not loaded in this tab. Reload the page.'})", ops, args);
        free(ops); free(args);
    }
    fbw_page_eval(t, expr ? expr : "null", on_page_eval, a);
    free(expr);
}

void fbw_page_message(FbwTab *t, const char *json)
{
    char err[100];
    FbJson *m = fbj_parse_str(json, err, sizeof err);
    if (!m) return;
    if (m->type != FBJ_OBJ || strcmp(STR(m, "token", ""), G.token) != 0) { fbj_free(m); return; }     /* only our own page tools know the token */
    fbj_obj_del(m, "token");
    const char *kind = STR(m, "kind", "");
    if (!strcmp(kind, "tools_ready")) {
        /* every document load announces itself: pick the recorder / picker back up in the new page */
        if (G.recording) fbw_tools_cmd(t, "{\"name\":\"recorder-start\"}");
        if (G.picking) { char *c = fbw_dupf("{\"name\":\"picker-start\",\"opts\":{\"mode\":\"%s\"}}", G.pick_mode); if (c) { fbw_tools_cmd(t, c); free(c); } }
        fbj_free(m);
        return;
    }
    if (!strcmp(kind, "pick") || !strcmp(kind, "pick_cancel")) G.picking = 0;
    else if (strcmp(kind, "rec") != 0) { fbj_free(m); return; }
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "ev", fbj_str("page.msg"));
    fbj_obj_set(o, "tab", fbj_num(t->id));
    fbj_obj_set(o, "msg", m);
    char *s = fbj_to_string(o, 0);
    fbj_free(o);
    if (s) { fbw_post(FBW_PANEL, s); free(s); }
}

/* ------------------------------------------------------------------ browsing */
static int safe_url(const char *u)
{
    while (*u == ' ') u++;
    return _strnicmp(u, "javascript:", 11) != 0;
}

static void notify_user_nav(const char *action, const char *url)
{
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "ev", fbj_str("nav.user"));
    if (action) fbj_obj_set(o, "action", fbj_str(action));
    if (url) fbj_obj_set(o, "url", fbj_str(url));
    char *s = fbj_to_string(o, 0);
    fbj_free(o);
    if (s) { fbw_post(FBW_PANEL, s); free(s); }
}

static void on_nothing(void *c, void *a, void *b) { (void)c; (void)a; (void)b; }

static void do_zoom(const FbJson *m)
{
    FbwTab *t = fbw_active_tab();
    if (!t || !t->ctl) return;
    double z = 1.0;
    ICoreWebView2Controller_get_ZoomFactor(t->ctl, &z);
    if (fbj_get_bool(m, "reset", 0)) z = G.default_zoom > 0 ? G.default_zoom : 1.0;
    else z += fbj_get_num(m, "delta", 0);
    if (z < 0.25) z = 0.25;
    if (z > 5.0) z = 5.0;
    ICoreWebView2Controller_put_ZoomFactor(t->ctl, z);
    t->zoom = z;
    char *s = fbw_dupf("{\"ev\":\"zoom\",\"factor\":%.3f}", z);
    if (s) { fbw_post(FBW_SHELL, s); free(s); }
}

static void do_find(const FbJson *m)
{
    FbwTab *t = fbw_active_tab();
    if (!t) return;
    FbJson *lit = fbj_str(STR(m, "text", ""));
    char *q = fbj_to_string(lit, 0);
    fbj_free(lit);
    char *js = fbw_dupf("window.find(%s,%s,%s,true,false,false,false)", q, fbj_get_bool(m, "matchCase", 0) ? "true" : "false", fbj_get_bool(m, "forward", 1) ? "false" : "true");
    fbw_run_script(t, js, on_nothing, NULL);
    free(q); free(js);
}

/* ------------------------------------------------------------------ dispatcher */
void fbw_command(int src, FbJson *m)
{
    const char *cmd = STR(m, "cmd", "");
    if (!cmd[0]) return;

    /* ---- start-up handshake */
    if (!strcmp(cmd, "shell.ready")) { fbw_on_shell_ready(); return; }
    if (!strcmp(cmd, "panel.ready")) { fbw_on_panel_ready(); return; }

    /* ---- storage and info */
    if (!strcmp(cmd, "store.get")) {
        char *txt = fbw_store_read(STR(m, "key", ""));
        FbJson *v = txt ? fbj_parse_str(txt, NULL, 0) : NULL;
        free(txt);
        FbJson *o = fbj_obj();
        fbj_obj_set(o, "value", v ? v : fbj_null());
        fbw_reply(src, m, o);
    } else if (!strcmp(cmd, "store.set")) {
        const FbJson *v = fbj_get(m, "value");
        const char *key = STR(m, "key", "");
        if (v && key[0]) {
            char *s = fbj_to_string(v, 0);
            if (s) { fbw_store_write(key, s); free(s); }
            if (!strcmp(key, "settings")) {
                fbw_settings_apply(v);
                FbJson *o = fbj_obj();
                fbj_obj_set(o, "ev", fbj_str("settings"));
                fbj_obj_set(o, "settings", fbj_clone(v));
                char *b = fbj_to_string(o, 0);
                fbj_free(o);
                if (b) { fbw_broadcast(b); free(b); }
            }
        }
        fbw_reply(src, m, NULL);
    } else if (!strcmp(cmd, "app.info")) {
        FbJson *o = fbj_obj();
        fbj_obj_set(o, "version", fbj_str(FBW_VERSION));
        fbj_obj_set(o, "dataDir", fbj_str(G.data_dir));
        fbj_obj_set(o, "portable", fbj_bool(G.portable));
        fbw_reply(src, m, o);

    /* ---- library, files, backup */
    } else if (!strcmp(cmd, "lib.list")) {
        FbJson *arr = NULL;
        fbf_lib_list(G.flows_dir, &arr);
        FbJson *o = fbj_obj();
        fbj_obj_set(o, "flows", arr ? arr : fbj_arr());
        fbw_reply(src, m, o);
    } else if (!strcmp(cmd, "lib.load")) cmd_lib_load(src, m);
    else if (!strcmp(cmd, "lib.save")) cmd_lib_save(src, m);
    else if (!strcmp(cmd, "lib.delete")) { fbf_lib_delete(G.flows_dir, STR(m, "name", "")); fbw_reply(src, m, NULL); }
    else if (!strcmp(cmd, "lib.versions")) cmd_lib_versions(src, m);
    else if (!strcmp(cmd, "validate")) {
        FbJson *copy = fbj_clone(fbj_get(m, "flow"));
        FbJson *o = fbj_obj();
        if (copy) { fbf_migrate(copy); fbj_obj_set(o, "issues", fbf_validate(copy)); fbj_free(copy); }
        else fbj_obj_set(o, "issues", fbj_arr());
        fbw_reply(src, m, o);
    } else if (!strcmp(cmd, "file.dialog")) cmd_file_dialog(src, m);
    else if (!strcmp(cmd, "file.save_text")) cmd_save_text(src, m);
    else if (!strcmp(cmd, "flow.import")) cmd_flow_import(src, m);
    else if (!strcmp(cmd, "flow.export")) cmd_flow_export(src, m);
    else if (!strcmp(cmd, "backup.create")) cmd_backup_create(src, m);
    else if (!strcmp(cmd, "backup.restore")) cmd_backup_restore(src, m);
    else if (!strcmp(cmd, "clipboard")) set_clipboard(STR(m, "text", ""));
    else if (!strcmp(cmd, "open.folder")) {
        const char *w = STR(m, "which", "flows");
        char *d = !strcmp(w, "backups") ? fb_strdup(G.backup_dir) : !strcmp(w, "data") ? fbw_dupf("%s\\data", G.data_dir) : fb_strdup(G.flows_dir);
        if (d) { fb_mkdirs(d); fbw_open_folder(d); free(d); }
    }

    /* ---- Excel / CSV */
    else if (!strcmp(cmd, "data.preview")) cmd_data_preview(src, m);
    else if (!strcmp(cmd, "data.export")) {
        char err[300] = "";
        if (fbw_engine_export(m, err, sizeof err) == 0) fbw_reply(src, m, NULL); else fbw_reply_err(src, m, err);
    }

    /* ---- scheduling */
    else if (!strcmp(cmd, "schedule.create")) cmd_schedule_create(src, m);
    else if (!strcmp(cmd, "schedule.list")) cmd_schedule_list(src, m);
    else if (!strcmp(cmd, "schedule.delete")) cmd_schedule_delete(src, m);

    /* ---- page tools */
    else if (!strcmp(cmd, "page.call")) cmd_page(src, m, 1);
    else if (!strcmp(cmd, "page.exec")) cmd_page(src, m, 0);
    else if (!strcmp(cmd, "pick.start")) {
        FbwTab *t = fbw_active_tab();
        snprintf(G.pick_mode, sizeof G.pick_mode, "%s", STR(m, "mode", "element"));
        for (char *p = G.pick_mode; *p; p++) if (!isalnum((unsigned char)*p)) *p = '_';
        G.picking = 1;
        if (t) { char *c = fbw_dupf("{\"name\":\"picker-start\",\"opts\":{\"mode\":\"%s\"}}", G.pick_mode); if (c) { fbw_tools_cmd(t, c); free(c); } }
        fbw_reply(src, m, NULL);
    } else if (!strcmp(cmd, "pick.stop")) {
        G.picking = 0;
        FbwTab *t = fbw_active_tab();
        if (t) fbw_tools_cmd(t, "{\"name\":\"picker-stop\"}");
    } else if (!strcmp(cmd, "rec.start")) {
        FbwTab *t = fbw_active_tab();
        G.recording = 1;
        if (t) fbw_tools_cmd(t, "{\"name\":\"recorder-start\"}");
        fbw_reply(src, m, NULL);
    } else if (!strcmp(cmd, "rec.stop")) {
        G.recording = 0;
        for (int i = 0; i < G.ntabs; i++) fbw_tools_cmd(G.tabs[i], "{\"name\":\"recorder-stop\"}");
    }

    /* ---- running flows */
    else if (!strcmp(cmd, "run")) {
        char err[300] = "";
        FbJson *flow = fbj_clone(fbj_get(m, "flow"));
        if (!flow) { fbw_reply_err(src, m, "There is no flow to run."); return; }
        if (fbw_engine_start(flow, !strcmp(STR(m, "mode", "run"), "step"), fbj_get(m, "breakpoints"), fbj_get(m, "vars"), err, sizeof err)) fbw_reply_err(src, m, err);
        else fbw_reply(src, m, NULL);
    } else if (!strcmp(cmd, "run.stop")) fbw_engine_stop();
    else if (!strcmp(cmd, "run.pause")) fbw_engine_pause();
    else if (!strcmp(cmd, "run.resume")) fbw_engine_resume(!strcmp(STR(m, "action", "continue"), "step"));
    else if (!strcmp(cmd, "ask.answer")) fbw_engine_answer((int)fbj_get_num(m, "id", 0), (int)fbj_get_num(m, "status", 0), STR(m, "answer", ""));

    /* ---- layout */
    else if (!strcmp(cmd, "layout")) {
        const FbJson *c = fbj_get(m, "content"), *p = fbj_get(m, "panel");
        if (c && c->type == FBJ_OBJ) {
            G.content_css.left = (LONG)fbj_get_num(c, "x", 0); G.content_css.top = (LONG)fbj_get_num(c, "y", 0);
            G.content_css.right = G.content_css.left + (LONG)fbj_get_num(c, "w", 0); G.content_css.bottom = G.content_css.top + (LONG)fbj_get_num(c, "h", 0);
            G.have_layout = 1;
        }
        memset(&G.panel_css, 0, sizeof G.panel_css);
        if (p && p->type == FBJ_OBJ) {
            G.panel_css.left = (LONG)fbj_get_num(p, "x", 0); G.panel_css.top = (LONG)fbj_get_num(p, "y", 0);
            G.panel_css.right = G.panel_css.left + (LONG)fbj_get_num(p, "w", 0); G.panel_css.bottom = G.panel_css.top + (LONG)fbj_get_num(p, "h", 0);
        }
        fbw_layout();
    } else if (!strcmp(cmd, "overlay")) { G.overlay = fbj_get_bool(m, "on", 0); fbw_layout(); }
    else if (!strcmp(cmd, "panel")) {
        G.panel_visible = fbj_get_bool(m, "visible", 0);
        G.panel_float = strcmp(STR(m, "mode", "dock"), "float") == 0;
        fbw_apply_panel_mode();
    } else if (!strcmp(cmd, "relay")) {
        const FbJson *msg = fbj_get(m, "msg");
        if (msg) { char *s = fbj_to_string(msg, 0); if (s) { fbw_post(!strcmp(STR(m, "to", "shell"), "panel") ? FBW_PANEL : FBW_SHELL, s); free(s); } }
    }

    /* ---- app */
    else if (!strcmp(cmd, "app.exit")) { G.exit_code = (int)fbj_get_num(m, "code", 0); PostMessageW(G.hwnd, WM_FBW_EXIT, 0, 0); }
    else if (!strcmp(cmd, "app.stay")) G.closing = 0;

    /* ---- tabs and navigation */
    else if (!strcmp(cmd, "tab.new")) { const char *u = STR(m, "url", ""); fbw_tab_new(u[0] && safe_url(u) ? u : NULL, 1); }
    else if (!strcmp(cmd, "tab.close")) fbw_tab_close((int)fbj_get_num(m, "id", 0));
    else if (!strcmp(cmd, "tab.activate")) fbw_tab_activate((int)fbj_get_num(m, "id", 0));
    else if (!strcmp(cmd, "nav.go")) {
        const char *u = STR(m, "url", "");
        FbwTab *t = fbw_active_tab();
        if (!u[0] || !safe_url(u)) return;
        if (!t || !t->wv) { fbw_tab_new(u, 1); return; }
        wchar_t *w = fbw_w(u);
        if (w) { if (FAILED(ICoreWebView2_Navigate(t->wv, w))) fbw_tab_new(NULL, 1); free(w); }
        if (fbj_get_bool(m, "user", 0)) notify_user_nav(NULL, u);
    } else if (!strcmp(cmd, "nav.back")) { FbwTab *t = fbw_active_tab(); if (t && t->wv) { ICoreWebView2_GoBack(t->wv); notify_user_nav("back", NULL); } }
    else if (!strcmp(cmd, "nav.forward")) { FbwTab *t = fbw_active_tab(); if (t && t->wv) { ICoreWebView2_GoForward(t->wv); notify_user_nav("forward", NULL); } }
    else if (!strcmp(cmd, "nav.reload")) { FbwTab *t = fbw_active_tab(); if (t && t->wv) { ICoreWebView2_Reload(t->wv); notify_user_nav("reload", NULL); } }
    else if (!strcmp(cmd, "nav.stop")) { FbwTab *t = fbw_active_tab(); if (t && t->wv) ICoreWebView2_Stop(t->wv); }
    else if (!strcmp(cmd, "zoom")) do_zoom(m);
    else if (!strcmp(cmd, "find")) do_find(m);
    else if (!strcmp(cmd, "find.clear")) fbw_run_script(fbw_active_tab(), "getSelection&&getSelection().removeAllRanges()", on_nothing, NULL);
    else if (!strcmp(cmd, "print")) fbw_run_script(fbw_active_tab(), "window.print()", on_nothing, NULL);
    else if (!strcmp(cmd, "devtools")) { FbwTab *t = fbw_active_tab(); if (t && t->wv) ICoreWebView2_OpenDevToolsWindow(t->wv); }
    else if (!strcmp(cmd, "fullscreen")) fbw_toggle_fullscreen();
    else if (!strcmp(cmd, "clear.data")) {
        FbwTab *t = fbw_active_tab();
        if (t && fbj_get_bool(m, "cookies", 0)) fbw_cdp(t, "Network.clearBrowserCookies", "{}", on_nothing, NULL);
        if (t && fbj_get_bool(m, "cache", 0)) fbw_cdp(t, "Network.clearBrowserCache", "{}", on_nothing, NULL);
    } else if (m->count && fbj_get(m, "id")) fbw_reply_err(src, m, "Unknown command");
}
