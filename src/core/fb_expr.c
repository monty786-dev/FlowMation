#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#include "fb_expr.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ================================================================== helpers */

int fbe_parse_number(const char *s, double *out)
{
    while (isspace((unsigned char)*s)) s++;
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    int digits = 0;
    while (isdigit((unsigned char)*p)) { p++; digits++; }
    if (*p == '.') { p++; while (isdigit((unsigned char)*p)) { p++; digits++; } }
    if (!digits) return 0;
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        if (*q == '+' || *q == '-') q++;
        if (isdigit((unsigned char)*q)) { while (isdigit((unsigned char)*q)) q++; p = q; }
    }
    const char *e = p;
    while (isspace((unsigned char)*e)) e++;
    if (*e) return 0;
    if (out) *out = strtod(s, NULL);
    return 1;
}

void fbe_format_number(double d, FbBuf *out)
{
    if (!isfinite(d)) { fb_buf_puts(out, ""); return; }
    if (d == 0) { fb_buf_putc(out, '0'); return; }
    if (fabs(d) < 1e15 && d == floor(d)) { fb_buf_printf(out, "%.0f", d); return; }
    char buf[40];
    snprintf(buf, sizeof buf, "%.15g", d);
    fb_buf_puts(out, buf);
}

int fbe_truthy(const char *s)
{
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return 0;
    if (!fb_stricmp(s, "false") || !fb_stricmp(s, "no") || !fb_stricmp(s, "null") || !strcmp(s, "0") || !fb_stricmp(s, "off")) return 0;
    return 1;
}

static const char *const MONTHS[12] = { "January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December" };
static const char *const DAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

void fbe_format_time(const struct tm *tm, const char *fmt, FbBuf *out)
{
    for (const char *p = fmt; *p;) {
        if (*p == '[') {                      /* [literal text] */
            const char *e = strchr(p, ']');
            if (e) { fb_buf_append(out, p + 1, (size_t)(e - p - 1)); p = e + 1; continue; }
        }
        if (!strncmp(p, "YYYY", 4)) { fb_buf_printf(out, "%04d", tm->tm_year + 1900); p += 4; }
        else if (!strncmp(p, "YY", 2)) { fb_buf_printf(out, "%02d", (tm->tm_year + 1900) % 100); p += 2; }
        else if (!strncmp(p, "MMMM", 4)) { fb_buf_puts(out, MONTHS[tm->tm_mon % 12]); p += 4; }
        else if (!strncmp(p, "MMM", 3)) { fb_buf_append(out, MONTHS[tm->tm_mon % 12], 3); p += 3; }
        else if (!strncmp(p, "MM", 2)) { fb_buf_printf(out, "%02d", tm->tm_mon + 1); p += 2; }
        else if (!strncmp(p, "dddd", 4)) { fb_buf_puts(out, DAYS[tm->tm_wday % 7]); p += 4; }
        else if (!strncmp(p, "ddd", 3)) { fb_buf_append(out, DAYS[tm->tm_wday % 7], 3); p += 3; }
        else if (!strncmp(p, "DD", 2)) { fb_buf_printf(out, "%02d", tm->tm_mday); p += 2; }
        else if (!strncmp(p, "HH", 2)) { fb_buf_printf(out, "%02d", tm->tm_hour); p += 2; }
        else if (!strncmp(p, "hh", 2)) { int h = tm->tm_hour % 12; fb_buf_printf(out, "%02d", h ? h : 12); p += 2; }
        else if (!strncmp(p, "mm", 2)) { fb_buf_printf(out, "%02d", tm->tm_min); p += 2; }
        else if (!strncmp(p, "ss", 2)) { fb_buf_printf(out, "%02d", tm->tm_sec); p += 2; }
        else if (*p == 'A') { fb_buf_puts(out, tm->tm_hour < 12 ? "AM" : "PM"); p++; }
        else fb_buf_putc(out, *p++);
    }
}

static void now_tm(struct tm *tm)
{
    time_t t = time(NULL);
#ifdef _WIN32
    localtime_s(tm, &t);
#else
    localtime_r(&t, tm);
#endif
}

/* parse "YYYY-MM-DD[ T]HH:MM:SS" (time optional) */
static int parse_iso(const char *s, struct tm *tm)
{
    memset(tm, 0, sizeof *tm);
    int Y, M, D, h = 0, m = 0, sec = 0;
    int n = sscanf(s, "%d-%d-%d%*[ T]%d:%d:%d", &Y, &M, &D, &h, &m, &sec);
    if (n < 3) return 0;
    tm->tm_year = Y - 1900; tm->tm_mon = M - 1; tm->tm_mday = D;
    tm->tm_hour = h; tm->tm_min = m; tm->tm_sec = sec;
    tm->tm_isdst = -1;
    struct tm copy = *tm;
    mktime(&copy);                         /* fills tm_wday */
    tm->tm_wday = copy.tm_wday;
    return 1;
}

/* ================================================================ path access */

typedef struct { char kind; char text[256]; long idx; } Seg;   /* kind: 'k' key, 'i' index */

static int parse_path(const char *path, Seg *segs, int maxsegs)
{
    int n = 0;
    const char *p = path;
    while (*p && n < maxsegs) {
        if (*p == '.') { p++; continue; }
        if (*p == '[') {
            p++;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                size_t l = 0;
                while (*p && *p != q && l < sizeof segs[n].text - 1) segs[n].text[l++] = *p++;
                segs[n].text[l] = 0;
                if (*p == q) p++;
                segs[n].kind = 'k';
            } else {
                char tmp[64];
                size_t l = 0;
                while (*p && *p != ']' && l < sizeof tmp - 1) tmp[l++] = *p++;
                tmp[l] = 0;
                char *t = fb_trim(tmp);
                char *e;
                long v = strtol(t, &e, 10);
                if (*t && !*e) { segs[n].kind = 'i'; segs[n].idx = v; }
                else { segs[n].kind = 'v'; snprintf(segs[n].text, sizeof segs[n].text, "%s", t); }   /* [varname] */
            }
            while (*p && *p != ']') p++;
            if (*p == ']') p++;
            n++;
            continue;
        }
        size_t l = 0;
        while (*p && *p != '.' && *p != '[' && l < sizeof segs[n].text - 1) segs[n].text[l++] = *p++;
        segs[n].text[l] = 0;
        while (l && isspace((unsigned char)segs[n].text[l - 1])) segs[n].text[--l] = 0;
        char *start = segs[n].text;
        while (isspace((unsigned char)*start)) start++;
        if (start != segs[n].text) memmove(segs[n].text, start, strlen(start) + 1);
        segs[n].kind = 'k';
        if (segs[n].text[0]) n++;
    }
    return n;
}

const FbJson *fbe_path_get(const FbJson *vars, const char *path)
{
    Seg segs[16];
    int n = parse_path(path, segs, 16);
    const FbJson *cur = vars;
    static FbJson lenval;                      /* synthetic "length" result (single-threaded use) */
    for (int i = 0; i < n && cur; i++) {
        Seg *s = &segs[i];
        if (s->kind == 'v') {
            const FbJson *iv = fbe_path_get(vars, s->text);
            double d;
            if (iv && iv->type == FBJ_NUM) { s->kind = 'i'; s->idx = (long)iv->num; }
            else if (iv && iv->type == FBJ_STR && fbe_parse_number(iv->str, &d)) { s->kind = 'i'; s->idx = (long)d; }
            else if (iv && iv->type == FBJ_STR) { s->kind = 'k'; snprintf(s->text, sizeof s->text, "%s", iv->str); }
            else return NULL;
        }
        if (s->kind == 'i') {
            if (cur->type != FBJ_ARR) return NULL;
            long idx = s->idx < 0 ? (long)cur->count + s->idx : s->idx;
            if (idx < 0 || (size_t)idx >= cur->count) return NULL;
            cur = cur->items[idx];
        } else if (cur->type == FBJ_OBJ) {
            const FbJson *nx = fbj_get(cur, s->text);
            if (!nx && !strcmp(s->text, "length")) { memset(&lenval, 0, sizeof lenval); lenval.type = FBJ_NUM; lenval.num = (double)cur->count; nx = &lenval; }
            cur = nx;
        } else if (cur->type == FBJ_ARR) {
            if (!strcmp(s->text, "length") || !strcmp(s->text, "count")) { memset(&lenval, 0, sizeof lenval); lenval.type = FBJ_NUM; lenval.num = (double)cur->count; cur = &lenval; }
            else if (!strcmp(s->text, "first")) cur = cur->count ? cur->items[0] : NULL;
            else if (!strcmp(s->text, "last")) cur = cur->count ? cur->items[cur->count - 1] : NULL;
            else {
                char *e;
                long v = strtol(s->text, &e, 10);
                cur = (*s->text && !*e && v >= 0 && (size_t)v < cur->count) ? cur->items[v] : NULL;
            }
        } else if (cur->type == FBJ_STR && !strcmp(s->text, "length")) {
            memset(&lenval, 0, sizeof lenval);
            lenval.type = FBJ_NUM;
            size_t chars = 0;
            for (const char *c = cur->str; *c; c++) if (((unsigned char)*c & 0xC0) != 0x80) chars++;
            lenval.num = (double)chars;
            cur = &lenval;
        } else return NULL;
    }
    return cur;
}

int fbe_path_set(FbJson *vars, const char *path, FbJson *value)
{
    if (!vars || vars->type != FBJ_OBJ || !path) { fbj_free(value); return -1; }
    Seg segs[16];
    int n = parse_path(path, segs, 16);
    if (n == 0) { fbj_free(value); return -1; }
    FbJson *cur = vars;
    for (int i = 0; i < n - 1; i++) {
        if (segs[i].kind == 'i') {
            if (cur->type != FBJ_ARR || (size_t)segs[i].idx >= cur->count) { fbj_free(value); return -1; }
            cur = cur->items[segs[i].idx];
            continue;
        }
        FbJson *nx = fbj_get(cur, segs[i].text);
        if (!nx || nx->type != FBJ_OBJ) {
            if (cur->type != FBJ_OBJ) { fbj_free(value); return -1; }
            fbj_obj_set(cur, segs[i].text, fbj_obj());
            nx = fbj_get(cur, segs[i].text);
        }
        cur = nx;
    }
    if (segs[n - 1].kind == 'i') {
        if (cur->type != FBJ_ARR || (size_t)segs[n - 1].idx >= cur->count) { fbj_free(value); return -1; }
        fbj_free(cur->items[segs[n - 1].idx]);
        cur->items[segs[n - 1].idx] = value;
        return 0;
    }
    if (cur->type != FBJ_OBJ) { fbj_free(value); return -1; }
    return fbj_obj_set(cur, segs[n - 1].text, value);
}

/* =============================================================== expressions */

/* find the matching "}}" for a "{{" ending at p, honouring quotes. Returns pointer to "}}" or NULL */
static const char *find_close(const char *p)
{
    char q = 0;
    for (; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (p[0] == '}' && p[1] == '}') return p;
    }
    return NULL;
}

/* split "a | b:c | d" on top-level '|'. Fills starts/lens (max n). */
static int split_pipes(const char *s, size_t len, const char **starts, size_t *lens, int max)
{
    int n = 0;
    char q = 0;
    size_t st = 0;
    for (size_t i = 0; i <= len; i++) {
        char c = i < len ? s[i] : '|';
        if (q) { if (c == q) q = 0; continue; }
        if (c == '"' || c == '\'') { q = c; continue; }
        if (c == '|' && !(i < len && s[i + 1] == '|')) {
            if (n < max) { starts[n] = s + st; lens[n] = i - st; n++; }
            st = i + 1;
        }
    }
    return n;
}

static char *dup_trim(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    return fb_strndup(s, n);
}

static void unquote_inplace(char *s)
{
    size_t n = strlen(s);
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) { memmove(s, s + 1, n - 2); s[n - 2] = 0; }
}

/* parse filter "name:arg1:arg2" with optional quotes around args */
static int parse_filter(const char *f, char *name, size_t nsz, char args[3][256])
{
    size_t i = 0;
    while (f[i] && f[i] != ':' && i < nsz - 1) { name[i] = f[i]; i++; }
    name[i] = 0;
    char *t = fb_trim(name);
    if (t != name) memmove(name, t, strlen(t) + 1);
    int na = 0;
    /* these take the whole remainder as ONE argument, so formats/separators may contain ':' */
    if (f[i] == ':' && (!strcmp(name, "date") || !strcmp(name, "default") || !strcmp(name, "join") || !strcmp(name, "split"))) {
        size_t l = 0;
        for (i++; f[i] && l < 255; i++) args[0][l++] = f[i];
        args[0][l] = 0;
        unquote_inplace(args[0]);
        return 1;
    }
    while (f[i] == ':' && na < 3) {
        i++;
        size_t l = 0;
        char q = 0;
        while (f[i] && (q || f[i] != ':') && l < 255) {
            if (!q && (f[i] == '"' || f[i] == '\'')) { q = f[i]; i++; continue; }
            if (q && f[i] == q) { q = 0; i++; continue; }
            args[na][l++] = f[i++];
        }
        args[na][l] = 0;
        na++;
    }
    return na;
}

static void to_number_text(const char *in, FbBuf *out)
{
    /* keep digits, sign and the decimal separator; decide which of , . is the decimal mark */
    FbBuf keep;
    fb_buf_init(&keep);
    for (const char *p = in; *p; p++)
        if (isdigit((unsigned char)*p) || *p == '.' || *p == ',' || (*p == '-' && keep.len == 0)) fb_buf_putc(&keep, *p);
    const char *k = fb_buf_cstr(&keep);
    const char *lastdot = strrchr(k, '.'), *lastcomma = strrchr(k, ',');
    char decimal = 0;
    if (lastdot && lastcomma) decimal = lastdot > lastcomma ? '.' : ',';
    else if (lastcomma) {
        size_t after = strlen(lastcomma + 1);
        int multiple = strchr(k, ',') != lastcomma;
        decimal = (!multiple && after != 3) ? ',' : 0;
        if (!multiple && after == 3 && !lastdot) decimal = 0;
    } else if (lastdot) {
        int multiple = strchr(k, '.') != lastdot;
        decimal = multiple ? 0 : '.';
    }
    FbBuf clean;
    fb_buf_init(&clean);
    for (const char *p = k; *p; p++) {
        if (*p == ',' || *p == '.') { if (*p == decimal) fb_buf_putc(&clean, '.'); }
        else fb_buf_putc(&clean, *p);
    }
    double d = 0;
    if (clean.len && fbe_parse_number(fb_buf_cstr(&clean), &d)) fbe_format_number(d, out);
    fb_buf_free(&keep);
    fb_buf_free(&clean);
}

static void url_encode(const char *s, FbBuf *out)
{
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') fb_buf_putc(out, (char)c);
        else fb_buf_printf(out, "%%%02X", c);
    }
}

/* apply one filter to value (consumed); returns a new value */
static FbJson *apply_filter(FbJson *v, const char *name, char args[3][256], int na, char *err, size_t errsz)
{
    FbBuf t;
    fb_buf_init(&t);
    if (v) fbj_to_text(v, &t);
    const char *s = fb_buf_cstr(&t);
    FbJson *r = NULL;

    if (!strcmp(name, "trim")) { char *c = fb_strdup(s); r = fbj_str(fb_trim(c)); free(c); }
    else if (!strcmp(name, "upper") || !strcmp(name, "lower") || !strcmp(name, "title")) {
        char *c = fb_strdup(s);
        int up = 1;
        for (char *p = c; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch >= 0x80) { up = 0; continue; }
            if (name[0] == 'u') *p = (char)toupper(ch);
            else if (name[0] == 'l') *p = (char)tolower(ch);
            else { *p = (char)(up ? toupper(ch) : tolower(ch)); up = !isalnum(ch) && ch != '\''; }
        }
        r = fbj_str(c);
        free(c);
    }
    else if (!strcmp(name, "number") || !strcmp(name, "int") || !strcmp(name, "round") || !strcmp(name, "abs")) {
        FbBuf n;
        fb_buf_init(&n);
        if (v && v->type == FBJ_NUM) fbe_format_number(v->num, &n); else to_number_text(s, &n);
        double d = 0;
        int ok = n.len && fbe_parse_number(fb_buf_cstr(&n), &d);
        if (!ok) r = fbj_str("");
        else if (!strcmp(name, "int")) r = fbj_num(d < 0 ? ceil(d) : floor(d));
        else if (!strcmp(name, "abs")) r = fbj_num(fabs(d));
        else if (!strcmp(name, "round")) {
            int dec = na ? atoi(args[0]) : 0;
            if (dec < 0) dec = 0;
            if (dec > 10) dec = 10;
            double m = pow(10, dec);
            r = fbj_num(round(d * m) / m);
        } else r = fbj_num(d);
        fb_buf_free(&n);
    }
    else if (!strcmp(name, "default")) r = (t.len == 0) ? fbj_str(na ? args[0] : "") : fbj_clone(v);
    else if (!strcmp(name, "length") || !strcmp(name, "count")) {
        if (v && (v->type == FBJ_ARR || v->type == FBJ_OBJ)) r = fbj_num((double)v->count);
        else { size_t c = 0; for (const char *p = s; *p; p++) if (((unsigned char)*p & 0xC0) != 0x80) c++; r = fbj_num((double)c); }
    }
    else if (!strcmp(name, "first") || !strcmp(name, "last")) {
        if (v && v->type == FBJ_ARR) { FbJson *e = v->count ? v->items[name[0] == 'f' ? 0 : v->count - 1] : NULL; r = e ? fbj_clone(e) : fbj_str(""); }
        else r = fbj_clone(v);
    }
    else if (!strcmp(name, "join")) {
        FbBuf j;
        fb_buf_init(&j);
        const char *sep = na ? args[0] : ", ";
        if (v && v->type == FBJ_ARR)
            for (size_t i = 0; i < v->count; i++) { if (i) fb_buf_puts(&j, sep); fbj_to_text(v->items[i], &j); }
        else fb_buf_puts(&j, s);
        r = fbj_str(fb_buf_cstr(&j));
        fb_buf_free(&j);
    }
    else if (!strcmp(name, "split")) {
        const char *sep = na && args[0][0] ? args[0] : ",";
        size_t sl = strlen(sep);
        FbJson *arr = fbj_arr();
        const char *p = s;
        for (;;) {
            const char *f = strstr(p, sep);
            if (!f) { fbj_arr_push(arr, fbj_str(p)); break; }
            fbj_arr_push(arr, fbj_strn(p, (size_t)(f - p)));
            p = f + sl;
        }
        r = arr;
    }
    else if (!strcmp(name, "json")) { r = fbj_str(""); free(r->str); r->str = v ? fbj_to_string(v, 0) : fb_strdup("null"); }
    else if (!strcmp(name, "urlencode")) { FbBuf e; fb_buf_init(&e); url_encode(s, &e); r = fbj_str(fb_buf_cstr(&e)); fb_buf_free(&e); }
    else if (!strcmp(name, "replace")) {
        FbBuf o;
        fb_buf_init(&o);
        const char *a = na > 0 ? args[0] : "", *b = na > 1 ? args[1] : "";
        size_t al = strlen(a);
        if (!al) fb_buf_puts(&o, s);
        else for (const char *p = s; *p;) {
            if (!strncmp(p, a, al)) { fb_buf_puts(&o, b); p += al; } else fb_buf_putc(&o, *p++);
        }
        r = fbj_str(fb_buf_cstr(&o));
        fb_buf_free(&o);
    }
    else if (!strcmp(name, "substr") || !strcmp(name, "truncate")) {
        long start = !strcmp(name, "substr") && na > 0 ? atol(args[0]) : 0;
        long cnt = !strcmp(name, "substr") ? (na > 1 ? atol(args[1]) : 1 << 30) : (na > 0 ? atol(args[0]) : 1 << 30);
        FbBuf o;
        fb_buf_init(&o);
        long ci = 0;
        for (const char *p = s; *p;) {
            size_t l = 1;
            while (((unsigned char)p[l] & 0xC0) == 0x80) l++;
            if (ci >= start && ci < start + cnt) fb_buf_append(&o, p, l);
            ci++;
            p += l;
        }
        r = fbj_str(fb_buf_cstr(&o));
        fb_buf_free(&o);
    }
    else if (!strcmp(name, "pad")) {
        int w = na ? atoi(args[0]) : 2;
        FbBuf o;
        fb_buf_init(&o);
        for (int i = (int)strlen(s); i < w; i++) fb_buf_putc(&o, '0');
        fb_buf_puts(&o, s);
        r = fbj_str(fb_buf_cstr(&o));
        fb_buf_free(&o);
    }
    else if (!strcmp(name, "date")) {
        struct tm tm;
        FbBuf o;
        fb_buf_init(&o);
        if (parse_iso(s, &tm)) fbe_format_time(&tm, na ? args[0] : "YYYY-MM-DD", &o);
        else fb_buf_puts(&o, s);
        r = fbj_str(fb_buf_cstr(&o));
        fb_buf_free(&o);
    }
    else {
        fb_set_err(err, errsz, "unknown filter '%s'", name);
    }
    fb_buf_free(&t);
    fbj_free(v);
    return r;
}

/* Evaluate "base | filter | filter" to an owned value. NULL on error. */
static FbJson *eval_expr(const FbJson *vars, const char *expr, size_t len, char *err, size_t errsz)
{
    const char *st[12];
    size_t ln[12];
    int np = split_pipes(expr, len, st, ln, 12);
    if (np == 0) return fbj_str("");
    char *base = dup_trim(st[0], ln[0]);
    FbJson *val = NULL;
    size_t bl = strlen(base);
    if (bl >= 2 && (base[0] == '"' || base[0] == '\'') && base[bl - 1] == base[0]) {
        unquote_inplace(base);
        val = fbj_str(base);
    } else {
        const FbJson *found = fbe_path_get(vars, base);
        struct tm tm;
        if (found) val = fbj_clone(found);
        else if (!strcmp(base, "now")) { now_tm(&tm); FbBuf b; fb_buf_init(&b); fbe_format_time(&tm, "YYYY-MM-DD HH:mm:ss", &b); val = fbj_str(fb_buf_cstr(&b)); fb_buf_free(&b); }
        else if (!strcmp(base, "today")) { now_tm(&tm); FbBuf b; fb_buf_init(&b); fbe_format_time(&tm, "YYYY-MM-DD", &b); val = fbj_str(fb_buf_cstr(&b)); fb_buf_free(&b); }
        else if (!strcmp(base, "timestamp")) val = fbj_num((double)time(NULL));
        else if (!strcmp(base, "random")) val = fbj_num((double)(rand() % 1000000));
        else if (!strcmp(base, "newline")) val = fbj_str("\n");
        else if (!strcmp(base, "tab")) val = fbj_str("\t");
        else val = fbj_str("");
    }
    free(base);
    for (int i = 1; i < np && val; i++) {
        char fname[64], args[3][256] = { "", "", "" };
        char *f = dup_trim(st[i], ln[i]);
        int na = parse_filter(f, fname, sizeof fname, args);
        free(f);
        char localerr[128] = "";
        val = apply_filter(val, fname, args, na, localerr, sizeof localerr);
        if (!val) { fb_set_err(err, errsz, "%s", localerr[0] ? localerr : "filter failed"); return NULL; }
    }
    return val;
}

int fbe_expand(const FbJson *vars, const char *tpl, FbBuf *out, char *err, size_t errsz)
{
    const char *p = tpl;
    while (*p) {
        const char *open = strstr(p, "{{");
        if (!open) { fb_buf_puts(out, p); break; }
        fb_buf_append(out, p, (size_t)(open - p));
        const char *close = find_close(open + 2);
        if (!close) { fb_buf_puts(out, open); break; }
        FbJson *v = eval_expr(vars, open + 2, (size_t)(close - open - 2), err, errsz);
        if (!v) return -1;
        fbj_to_text(v, out);
        fbj_free(v);
        p = close + 2;
    }
    return 0;
}

FbJson *fbe_expand_value(const FbJson *vars, const char *tpl, char *err, size_t errsz)
{
    const char *s = tpl;
    while (isspace((unsigned char)*s)) s++;
    if (s[0] == '{' && s[1] == '{') {
        const char *close = find_close(s + 2);
        if (close) {
            const char *after = close + 2;
            while (isspace((unsigned char)*after)) after++;
            if (!*after) return eval_expr(vars, s + 2, (size_t)(close - s - 2), err, errsz);
        }
    }
    FbBuf b;
    fb_buf_init(&b);
    if (fbe_expand(vars, tpl, &b, err, errsz)) { fb_buf_free(&b); return NULL; }
    FbJson *r = fbj_str(fb_buf_cstr(&b));
    fb_buf_free(&b);
    return r;
}

/* ================================================================ conditions */

enum { OP_EQ = 1, OP_NE, OP_GE, OP_LE, OP_GT, OP_LT, OP_CONTAINS, OP_ICONTAINS, OP_STARTS, OP_ENDS, OP_EMPTY, OP_NOTEMPTY };

typedef struct { const char *tok; int op; int unary; } OpDef;
/* order matters: longer tokens first when they share a first character */
static const OpDef OPS[] = {
    { "==", OP_EQ, 0 }, { "!=", OP_NE, 0 }, { ">=", OP_GE, 0 }, { "<=", OP_LE, 0 }, { ">", OP_GT, 0 }, { "<", OP_LT, 0 },
    { " icontains ", OP_ICONTAINS, 0 }, { " contains ", OP_CONTAINS, 0 },
    { " startswith ", OP_STARTS, 0 }, { " endswith ", OP_ENDS, 0 },
    { " is not empty", OP_NOTEMPTY, 1 }, { " is empty", OP_EMPTY, 1 }, { " not empty", OP_NOTEMPTY, 1 },
};

static int match_ci(const char *p, const char *tok)
{
    for (; *tok; p++, tok++) if (tolower((unsigned char)*p) != tolower((unsigned char)*tok)) return 0;
    return 1;
}

/* first top-level (outside quotes / {{ }}) occurrence in s of any token in `toks`; returns its index or -1 */
static const char *scan_top(const char *s, const char *const *toks, int ntoks, int *which)
{
    char q = 0;
    int depth = 0;
    for (const char *p = s; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (depth == 0 && (*p == '"' || *p == '\'')) { q = *p; continue; }
        if (p[0] == '{' && p[1] == '{') { depth++; p++; continue; }
        if (p[0] == '}' && p[1] == '}' && depth) { depth--; p++; continue; }
        if (depth) {
            if (*p == '"' || *p == '\'') { char qq = *p++; while (*p && *p != qq) p++; if (!*p) break; }
            continue;
        }
        for (int i = 0; i < ntoks; i++)
            if (match_ci(p, toks[i])) { if (which) *which = i; return p; }
    }
    return NULL;
}

static char *operand_text(const FbJson *vars, const char *s, size_t n, char *err, size_t errsz)
{
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    char *raw = fb_strndup(s, n);
    if (!raw) return NULL;
    if (n >= 2 && (raw[0] == '"' || raw[0] == '\'') && raw[n - 1] == raw[0]) unquote_inplace(raw);
    FbBuf b;
    fb_buf_init(&b);
    if (fbe_expand(vars, raw, &b, err, errsz)) { free(raw); fb_buf_free(&b); return NULL; }
    free(raw);
    return fb_buf_detach(&b);
}

static int compare_atom(int op, const char *a, const char *b)
{
    double x, y;
    int numeric = fbe_parse_number(a, &x) && fbe_parse_number(b, &y);
    int c = numeric ? (x < y ? -1 : x > y ? 1 : 0) : strcmp(a, b);
    switch (op) {
    case OP_EQ: return c == 0;
    case OP_NE: return c != 0;
    case OP_GE: return c >= 0;
    case OP_LE: return c <= 0;
    case OP_GT: return c > 0;
    case OP_LT: return c < 0;
    case OP_CONTAINS: return strstr(a, b) != NULL;
    case OP_ICONTAINS: {
        size_t bl = strlen(b);
        if (!bl) return 1;
        for (const char *p = a; *p; p++) {
            size_t i = 0;
            while (i < bl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)b[i])) i++;
            if (i == bl) return 1;
        }
        return 0;
    }
    case OP_STARTS: return strncmp(a, b, strlen(b)) == 0;
    case OP_ENDS: return fb_ends_with(a, b);
    }
    return 0;
}

static int eval_atom(const FbJson *vars, const char *s, char *err, size_t errsz)
{
    while (isspace((unsigned char)*s)) s++;
    int negate = 0;
    if (s[0] == '!' && s[1] != '=') { negate = 1; s++; }
    else if (match_ci(s, "not ") ) { negate = 1; s += 4; }
    const char *toks[sizeof OPS / sizeof OPS[0]];
    for (size_t i = 0; i < sizeof OPS / sizeof OPS[0]; i++) toks[i] = OPS[i].tok;
    int which = -1;
    const char *at = scan_top(s, toks, (int)(sizeof OPS / sizeof OPS[0]), &which);
    int result;
    if (!at) {
        char *t = operand_text(vars, s, strlen(s), err, errsz);
        if (!t) return -1;
        result = fbe_truthy(t);
        free(t);
    } else {
        const OpDef *od = &OPS[which];
        char *left = operand_text(vars, s, (size_t)(at - s), err, errsz);
        if (!left) return -1;
        if (od->unary) {
            char *tl = fb_trim(left);
            result = (od->op == OP_EMPTY) ? (*tl == 0) : (*tl != 0);
        } else {
            const char *rhs = at + strlen(od->tok);
            char *right = operand_text(vars, rhs, strlen(rhs), err, errsz);
            if (!right) { free(left); return -1; }
            result = compare_atom(od->op, left, right);
            free(right);
        }
        free(left);
    }
    return negate ? !result : result;
}

static int eval_cond_rec(const FbJson *vars, const char *s, char *err, size_t errsz)
{
    static const char *const OR[] = { " or ", " || " };
    static const char *const AND[] = { " and ", " && " };
    int which = 0;
    const char *o = scan_top(s, OR, 2, &which);
    if (o) {
        char *left = fb_strndup(s, (size_t)(o - s));
        int r = eval_cond_rec(vars, left, err, errsz);
        free(left);
        if (r != 0) return r;                         /* true or error */
        return eval_cond_rec(vars, o + strlen(OR[which]), err, errsz);
    }
    const char *a = scan_top(s, AND, 2, &which);
    if (a) {
        char *left = fb_strndup(s, (size_t)(a - s));
        int r = eval_cond_rec(vars, left, err, errsz);
        free(left);
        if (r <= 0) return r;
        return eval_cond_rec(vars, a + strlen(AND[which]), err, errsz);
    }
    return eval_atom(vars, s, err, errsz);
}

int fbe_eval_cond(const FbJson *vars, const char *cond, char *err, size_t errsz)
{
    if (!cond || !*cond) return 1;
    return eval_cond_rec(vars, cond, err, errsz);
}

/* ================================================================ arithmetic */

typedef struct { const char *p; int bad; char msg[64]; } Calc;

static double calc_expr(Calc *c);

static void calc_ws(Calc *c) { while (isspace((unsigned char)*c->p)) c->p++; }

static double calc_atom(Calc *c)
{
    calc_ws(c);
    if (*c->p == '(') {
        c->p++;
        double v = calc_expr(c);
        calc_ws(c);
        if (*c->p == ')') c->p++; else { c->bad = 1; snprintf(c->msg, sizeof c->msg, "missing )"); }
        return v;
    }
    if (*c->p == '-') { c->p++; return -calc_atom(c); }
    if (*c->p == '+') { c->p++; return calc_atom(c); }
    char *e;
    double v = strtod(c->p, &e);
    if (e == c->p || (!isdigit((unsigned char)*c->p) && *c->p != '.')) { c->bad = 1; snprintf(c->msg, sizeof c->msg, "number expected"); return 0; }
    c->p = e;
    return v;
}

static double calc_term(Calc *c)
{
    double v = calc_atom(c);
    for (;;) {
        calc_ws(c);
        char op = *c->p;
        if (op != '*' && op != '/' && op != '%') return v;
        c->p++;
        double r = calc_atom(c);
        if (op == '*') v *= r;
        else if (r == 0) { c->bad = 1; snprintf(c->msg, sizeof c->msg, "division by zero"); return 0; }
        else v = (op == '/') ? v / r : fmod(v, r);
    }
}

static double calc_expr(Calc *c)
{
    double v = calc_term(c);
    for (;;) {
        calc_ws(c);
        char op = *c->p;
        if (op != '+' && op != '-') return v;
        c->p++;
        double r = calc_term(c);
        v = (op == '+') ? v + r : v - r;
    }
}

int fbe_calc(const char *expr, double *out, char *err, size_t errsz)
{
    Calc c = { expr, 0, "" };
    double v = calc_expr(&c);
    calc_ws(&c);
    if (!c.bad && *c.p) { c.bad = 1; snprintf(c.msg, sizeof c.msg, "unexpected '%c'", *c.p); }
    if (c.bad) { fb_set_err(err, errsz, "cannot calculate \"%s\": %s", expr, c.msg); return -1; }
    *out = v;
    return 0;
}
