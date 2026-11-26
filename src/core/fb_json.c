#include "fb_json.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- creation */

static FbJson *node_new(FbJType t)
{
    FbJson *j = (FbJson *)calloc(1, sizeof *j);
    if (j) j->type = t;
    return j;
}
FbJson *fbj_null(void) { return node_new(FBJ_NULL); }
FbJson *fbj_bool(int b) { FbJson *j = node_new(FBJ_BOOL); if (j) j->b = b ? 1 : 0; return j; }
FbJson *fbj_num(double d) { FbJson *j = node_new(FBJ_NUM); if (j) j->num = d; return j; }
FbJson *fbj_strn(const char *s, size_t n)
{
    FbJson *j = node_new(FBJ_STR);
    if (!j) return NULL;
    j->str = fb_strndup(s, n);
    if (!j->str) { free(j); return NULL; }
    return j;
}
FbJson *fbj_str(const char *s) { return fbj_strn(s ? s : "", s ? strlen(s) : 0); }
FbJson *fbj_arr(void) { return node_new(FBJ_ARR); }
FbJson *fbj_obj(void) { return node_new(FBJ_OBJ); }

void fbj_free(FbJson *j)
{
    if (!j) return;
    for (size_t i = 0; i < j->count; i++) {
        fbj_free(j->items[i]);
        if (j->keys) free(j->keys[i]);
    }
    free(j->items);
    free(j->keys);
    free(j->str);
    free(j);
}

FbJson *fbj_clone(const FbJson *j)
{
    if (!j) return NULL;
    FbJson *c = NULL;
    switch (j->type) {
    case FBJ_NULL: return fbj_null();
    case FBJ_BOOL: return fbj_bool(j->b);
    case FBJ_NUM:  return fbj_num(j->num);
    case FBJ_STR:  return fbj_str(j->str);
    case FBJ_ARR:
        c = fbj_arr();
        for (size_t i = 0; c && i < j->count; i++) fbj_arr_push(c, fbj_clone(j->items[i]));
        return c;
    case FBJ_OBJ:
        c = fbj_obj();
        for (size_t i = 0; c && i < j->count; i++) fbj_obj_set(c, j->keys[i], fbj_clone(j->items[i]));
        return c;
    }
    return NULL;
}

static int grow(FbJson *c)
{
    if (c->count < c->cap) return 0;
    size_t nc = c->cap ? c->cap * 2 : 4;
    FbJson **ni = (FbJson **)realloc(c->items, nc * sizeof *ni);
    if (!ni) return -1;
    c->items = ni;
    if (c->type == FBJ_OBJ) {
        char **nk = (char **)realloc(c->keys, nc * sizeof *nk);
        if (!nk) return -1;
        c->keys = nk;
    }
    c->cap = nc;
    return 0;
}

int fbj_arr_push(FbJson *arr, FbJson *val)
{
    if (!arr || arr->type != FBJ_ARR || !val) { fbj_free(val); return -1; }
    if (grow(arr)) { fbj_free(val); return -1; }
    arr->items[arr->count++] = val;
    return 0;
}

int fbj_obj_set(FbJson *obj, const char *key, FbJson *val)
{
    if (!obj || obj->type != FBJ_OBJ || !val) { fbj_free(val); return -1; }
    for (size_t i = 0; i < obj->count; i++) {
        if (!strcmp(obj->keys[i], key)) {
            fbj_free(obj->items[i]);
            obj->items[i] = val;
            return 0;
        }
    }
    if (grow(obj)) { fbj_free(val); return -1; }
    char *k = fb_strdup(key);
    if (!k) { fbj_free(val); return -1; }
    obj->keys[obj->count] = k;
    obj->items[obj->count] = val;
    obj->count++;
    return 0;
}

int fbj_obj_del(FbJson *obj, const char *key)
{
    if (!obj || obj->type != FBJ_OBJ) return -1;
    for (size_t i = 0; i < obj->count; i++) {
        if (!strcmp(obj->keys[i], key)) {
            fbj_free(obj->items[i]);
            free(obj->keys[i]);
            memmove(obj->items + i, obj->items + i + 1, (obj->count - i - 1) * sizeof *obj->items);
            memmove(obj->keys + i, obj->keys + i + 1, (obj->count - i - 1) * sizeof *obj->keys);
            obj->count--;
            return 0;
        }
    }
    return -1;
}

int fbj_arr_del(FbJson *arr, size_t idx)
{
    if (!arr || arr->type != FBJ_ARR || idx >= arr->count) return -1;
    fbj_free(arr->items[idx]);
    memmove(arr->items + idx, arr->items + idx + 1, (arr->count - idx - 1) * sizeof *arr->items);
    arr->count--;
    return 0;
}

FbJson *fbj_get(const FbJson *obj, const char *key)
{
    if (!obj || obj->type != FBJ_OBJ) return NULL;
    for (size_t i = 0; i < obj->count; i++)
        if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}
FbJson *fbj_at(const FbJson *arr, size_t i)
{
    if (!arr || arr->type != FBJ_ARR || i >= arr->count) return NULL;
    return arr->items[i];
}
size_t fbj_len(const FbJson *j) { return (j && (j->type == FBJ_ARR || j->type == FBJ_OBJ)) ? j->count : 0; }
int fbj_is(const FbJson *j, FbJType t) { return j && j->type == t; }

const char *fbj_get_str(const FbJson *obj, const char *key, const char *def)
{
    FbJson *v = fbj_get(obj, key);
    return (v && v->type == FBJ_STR) ? v->str : def;
}
double fbj_get_num(const FbJson *obj, const char *key, double def)
{
    FbJson *v = fbj_get(obj, key);
    if (!v) return def;
    if (v->type == FBJ_NUM) return v->num;
    if (v->type == FBJ_STR && v->str[0]) { char *e; double d = strtod(v->str, &e); if (*e == 0) return d; }
    return def;
}
int fbj_get_bool(const FbJson *obj, const char *key, int def)
{
    FbJson *v = fbj_get(obj, key);
    if (!v) return def;
    if (v->type == FBJ_BOOL) return v->b;
    if (v->type == FBJ_NUM) return v->num != 0;
    if (v->type == FBJ_STR) {
        if (!fb_stricmp(v->str, "true") || !fb_stricmp(v->str, "yes") || !strcmp(v->str, "1")) return 1;
        if (!fb_stricmp(v->str, "false") || !fb_stricmp(v->str, "no") || !strcmp(v->str, "0")) return 0;
    }
    return def;
}

/* ----------------------------------------------------------------- parsing */

typedef struct {
    const char *p, *end;
    char *err; size_t errsz;
    int depth;
    int failed;
} Parser;

static FbJson *fail(Parser *ps, const char *msg)
{
    if (!ps->failed) {
        ps->failed = 1;
        fb_set_err(ps->err, ps->errsz, "JSON error: %s", msg);
    }
    return NULL;
}

static void skip_ws(Parser *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static int hex4(const char *s, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

static char *parse_string_raw(Parser *ps, size_t *outlen)
{
    /* assumes *p == '"' */
    ps->p++;
    FbBuf b;
    fb_buf_init(&b);
    while (ps->p < ps->end && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p++;
        if (c != '\\') { fb_buf_putc(&b, (char)c); continue; }
        if (ps->p >= ps->end) break;
        char e = *ps->p++;
        switch (e) {
        case '"': fb_buf_putc(&b, '"'); break;
        case '\\': fb_buf_putc(&b, '\\'); break;
        case '/': fb_buf_putc(&b, '/'); break;
        case 'b': fb_buf_putc(&b, '\b'); break;
        case 'f': fb_buf_putc(&b, '\f'); break;
        case 'n': fb_buf_putc(&b, '\n'); break;
        case 'r': fb_buf_putc(&b, '\r'); break;
        case 't': fb_buf_putc(&b, '\t'); break;
        case 'u': {
            unsigned cp;
            if (ps->end - ps->p < 4 || hex4(ps->p, &cp)) { fb_buf_free(&b); fail(ps, "bad \\u escape"); return NULL; }
            ps->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
                unsigned lo;
                if (!hex4(ps->p + 2, &lo) && lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    ps->p += 6;
                }
            }
            char u[4];
            int k = fb_utf8_encode(cp, u);
            fb_buf_append(&b, u, (size_t)k);
            break;
        }
        default: fb_buf_free(&b); fail(ps, "bad escape"); return NULL;
        }
    }
    if (ps->p >= ps->end) { fb_buf_free(&b); fail(ps, "unterminated string"); return NULL; }
    ps->p++; /* closing quote */
    if (outlen) *outlen = b.len;
    return fb_buf_detach(&b);
}

static FbJson *parse_value(Parser *ps);

static FbJson *parse_array(Parser *ps)
{
    ps->p++;
    FbJson *a = fbj_arr();
    if (!a) return fail(ps, "out of memory");
    skip_ws(ps);
    while (ps->p < ps->end && *ps->p != ']') {
        FbJson *v = parse_value(ps);
        if (!v) { fbj_free(a); return NULL; }
        fbj_arr_push(a, v);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; skip_ws(ps); }
        else if (ps->p < ps->end && *ps->p != ']') { fbj_free(a); return fail(ps, "expected , or ] in array"); }
    }
    if (ps->p >= ps->end) { fbj_free(a); return fail(ps, "unterminated array"); }
    ps->p++;
    return a;
}

static FbJson *parse_object(Parser *ps)
{
    ps->p++;
    FbJson *o = fbj_obj();
    if (!o) return fail(ps, "out of memory");
    skip_ws(ps);
    while (ps->p < ps->end && *ps->p != '}') {
        if (*ps->p != '"') { fbj_free(o); return fail(ps, "expected string key"); }
        char *k = parse_string_raw(ps, NULL);
        if (!k) { fbj_free(o); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(k); fbj_free(o); return fail(ps, "expected :"); }
        ps->p++;
        skip_ws(ps);
        FbJson *v = parse_value(ps);
        if (!v) { free(k); fbj_free(o); return NULL; }
        fbj_obj_set(o, k, v);
        free(k);
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; skip_ws(ps); }
        else if (ps->p < ps->end && *ps->p != '}') { fbj_free(o); return fail(ps, "expected , or } in object"); }
    }
    if (ps->p >= ps->end) { fbj_free(o); return fail(ps, "unterminated object"); }
    ps->p++;
    return o;
}

static FbJson *parse_value(Parser *ps)
{
    skip_ws(ps);
    if (ps->p >= ps->end) return fail(ps, "unexpected end");
    if (++ps->depth > 200) return fail(ps, "nesting too deep");
    FbJson *r = NULL;
    char c = *ps->p;
    if (c == '{') r = parse_object(ps);
    else if (c == '[') r = parse_array(ps);
    else if (c == '"') {
        size_t n;
        char *s = parse_string_raw(ps, &n);
        if (s) {
            r = fbj_strn(s, n);
            free(s);
            if (!r) fail(ps, "out of memory");
        }
    } else if (!strncmp(ps->p, "true", 4)) { ps->p += 4; r = fbj_bool(1); }
    else if (!strncmp(ps->p, "false", 5)) { ps->p += 5; r = fbj_bool(0); }
    else if (!strncmp(ps->p, "null", 4)) { ps->p += 4; r = fbj_null(); }
    else if (c == '-' || (c >= '0' && c <= '9')) {
        char *e;
        double d = strtod(ps->p, &e);
        if (e == ps->p) r = fail(ps, "bad number");
        else { ps->p = e; r = fbj_num(d); }
    } else r = fail(ps, "unexpected character");
    ps->depth--;
    return r;
}

FbJson *fbj_parse(const char *text, size_t len, char *err, size_t errsz)
{
    char *copy = NULL;
    if (text[len] != 0) {
        copy = fb_strndup(text, len);
        if (!copy) { fb_set_err(err, errsz, "out of memory"); return NULL; }
        text = copy;
    }
    Parser ps = { text, text + len, err, errsz, 0, 0 };
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) ps.p += 3;
    FbJson *j = parse_value(&ps);
    if (j) {
        skip_ws(&ps);
        if (ps.p < ps.end) { fbj_free(j); j = NULL; fail(&ps, "trailing characters"); }
    }
    free(copy);
    return j;
}

FbJson *fbj_parse_str(const char *text, char *err, size_t errsz)
{
    return fbj_parse(text, strlen(text), err, errsz);
}

/* ------------------------------------------------------------------ output */

void fbj_write_escaped_string(FbBuf *out, const char *s, size_t n)
{
    fb_buf_putc(out, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': fb_buf_puts(out, "\\\""); break;
        case '\\': fb_buf_puts(out, "\\\\"); break;
        case '\n': fb_buf_puts(out, "\\n"); break;
        case '\r': fb_buf_puts(out, "\\r"); break;
        case '\t': fb_buf_puts(out, "\\t"); break;
        case '\b': fb_buf_puts(out, "\\b"); break;
        case '\f': fb_buf_puts(out, "\\f"); break;
        default:
            if (c < 0x20) fb_buf_printf(out, "\\u%04x", c);
            else fb_buf_putc(out, (char)c);
        }
    }
    fb_buf_putc(out, '"');
}

static void write_num(FbBuf *out, double d)
{
    if (!isfinite(d)) { fb_buf_puts(out, "null"); return; }
    if (d == 0) { fb_buf_putc(out, '0'); return; }
    if (fabs(d) < 1e15 && d == floor(d)) { fb_buf_printf(out, "%.0f", d); return; }
    char buf[40];
    snprintf(buf, sizeof buf, "%.15g", d);
    if (strtod(buf, NULL) != d) snprintf(buf, sizeof buf, "%.17g", d);
    fb_buf_puts(out, buf);
}

static void indent(FbBuf *out, int n) { for (int i = 0; i < n; i++) fb_buf_puts(out, "  "); }

static void write_rec(const FbJson *j, FbBuf *out, int pretty, int level)
{
    if (!j) { fb_buf_puts(out, "null"); return; }
    switch (j->type) {
    case FBJ_NULL: fb_buf_puts(out, "null"); break;
    case FBJ_BOOL: fb_buf_puts(out, j->b ? "true" : "false"); break;
    case FBJ_NUM: write_num(out, j->num); break;
    case FBJ_STR: fbj_write_escaped_string(out, j->str, strlen(j->str)); break;
    case FBJ_ARR:
        if (!j->count) { fb_buf_puts(out, "[]"); break; }
        fb_buf_putc(out, '[');
        for (size_t i = 0; i < j->count; i++) {
            if (i) fb_buf_putc(out, ',');
            if (pretty) { fb_buf_putc(out, '\n'); indent(out, level + 1); }
            write_rec(j->items[i], out, pretty, level + 1);
        }
        if (pretty) { fb_buf_putc(out, '\n'); indent(out, level); }
        fb_buf_putc(out, ']');
        break;
    case FBJ_OBJ:
        if (!j->count) { fb_buf_puts(out, "{}"); break; }
        fb_buf_putc(out, '{');
        for (size_t i = 0; i < j->count; i++) {
            if (i) fb_buf_putc(out, ',');
            if (pretty) { fb_buf_putc(out, '\n'); indent(out, level + 1); }
            fbj_write_escaped_string(out, j->keys[i], strlen(j->keys[i]));
            fb_buf_puts(out, pretty ? ": " : ":");
            write_rec(j->items[i], out, pretty, level + 1);
        }
        if (pretty) { fb_buf_putc(out, '\n'); indent(out, level); }
        fb_buf_putc(out, '}');
        break;
    }
}

void fbj_write(const FbJson *j, FbBuf *out, int pretty) { write_rec(j, out, pretty, 0); }

char *fbj_to_string(const FbJson *j, int pretty)
{
    FbBuf b;
    fb_buf_init(&b);
    fbj_write(j, &b, pretty);
    return fb_buf_detach(&b);
}

void fbj_to_text(const FbJson *j, FbBuf *out)
{
    if (!j) return;
    switch (j->type) {
    case FBJ_NULL: break;
    case FBJ_BOOL: fb_buf_puts(out, j->b ? "true" : "false"); break;
    case FBJ_NUM: write_num(out, j->num); break;
    case FBJ_STR: fb_buf_puts(out, j->str); break;
    default: fbj_write(j, out, 0);
    }
}
