#include "fb_xlsx.h"
#include "fb_zip.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================== cell reference helpers */

int fbx_col_index(const char *s)
{
    int c = 0;
    if (!*s) return 0;
    for (; *s; s++) {
        if (!isalpha((unsigned char)*s)) return 0;
        c = c * 26 + (toupper((unsigned char)*s) - 'A' + 1);
        if (c > 16384) return 0;
    }
    return c;
}

void fbx_col_name(int col, char out[8])
{
    char t[8];
    int n = 0;
    while (col > 0 && n < 7) {
        t[n++] = (char)('A' + (col - 1) % 26);
        col = (col - 1) / 26;
    }
    for (int i = 0; i < n; i++) out[i] = t[n - 1 - i];
    out[n] = 0;
}

/* letters and digits are both optional here; returns 0 if the text is a valid fragment */
static int parse_part(const char *s, size_t n, int *row, int *col)
{
    size_t i = 0;
    *row = 0; *col = 0;
    if (i < n && s[i] == '$') i++;
    size_t ls = i;
    while (i < n && isalpha((unsigned char)s[i])) i++;
    if (i > ls) {
        char tmp[8];
        if (i - ls > 3) return -1;
        memcpy(tmp, s + ls, i - ls);
        tmp[i - ls] = 0;
        *col = fbx_col_index(tmp);
        if (!*col) return -1;
    }
    if (i < n && s[i] == '$') i++;
    size_t ds = i;
    long r = 0;
    while (i < n && isdigit((unsigned char)s[i])) { r = r * 10 + (s[i] - '0'); i++; if (r > 1048576) return -1; }
    if (i > ds) *row = (int)r;
    if (i != n || (!*row && !*col)) return -1;
    return 0;
}

int fbx_parse_ref(const char *ref, int *row, int *col)
{
    if (parse_part(ref, strlen(ref), row, col)) return -1;
    return (*row >= 1 && *col >= 1) ? 0 : -1;
}

int fbx_parse_range(const char *s, int *r1, int *c1, int *r2, int *c2)
{
    const char *colon = strchr(s, ':');
    *r1 = *c1 = *r2 = *c2 = 0;
    if (!colon) {
        if (parse_part(s, strlen(s), r1, c1)) return -1;
        *r2 = *r1; *c2 = *c1;
        return 0;
    }
    if (parse_part(s, (size_t)(colon - s), r1, c1)) return -1;
    if (parse_part(colon + 1, strlen(colon + 1), r2, c2)) return -1;
    return 0;
}

/* ============================================================== grid + CSV */

void fbgrid_init(FbGrid *g) { g->rows = NULL; g->nrows = 0; }

void fbgrid_free(FbGrid *g)
{
    for (size_t i = 0; i < g->nrows; i++) {
        for (size_t k = 0; k < g->rows[i].n; k++) free(g->rows[i].c[k]);
        free(g->rows[i].c);
    }
    free(g->rows);
    fbgrid_init(g);
}

const char *fbgrid_get(const FbGrid *g, size_t r, size_t c)
{
    if (r >= g->nrows || c >= g->rows[r].n) return NULL;
    return g->rows[r].c[c];
}

size_t fbgrid_ncols(const FbGrid *g)
{
    size_t m = 0;
    for (size_t i = 0; i < g->nrows; i++) if (g->rows[i].n > m) m = g->rows[i].n;
    return m;
}

static int grid_row_add(FbGrid *g)
{
    FbRow *nr = (FbRow *)realloc(g->rows, (g->nrows + 1) * sizeof(FbRow));
    if (!nr) return -1;
    g->rows = nr;
    g->rows[g->nrows].c = NULL;
    g->rows[g->nrows].n = 0;
    g->nrows++;
    return 0;
}

static int row_push(FbRow *r, char *val /* ownership; may be NULL */)
{
    char **nc = (char **)realloc(r->c, (r->n + 1) * sizeof(char *));
    if (!nc) { free(val); return -1; }
    r->c = nc;
    r->c[r->n++] = val;
    return 0;
}

static int row_is_empty(const FbRow *r)
{
    for (size_t i = 0; i < r->n; i++) if (r->c[i] && r->c[i][0]) return 0;
    return 1;
}

FbJson *fbgrid_to_json(const FbGrid *g, size_t first_row0, int header, int include_row_no, int skip_empty_rows)
{
    FbJson *arr = fbj_arr();
    if (!arr) return NULL;
    size_t r = first_row0;
    if (!header) {
        for (; r < g->nrows; r++) {
            if (skip_empty_rows && row_is_empty(&g->rows[r])) continue;
            FbJson *row = fbj_arr();
            for (size_t c = 0; c < g->rows[r].n; c++) fbj_arr_push(row, fbj_str(g->rows[r].c[c] ? g->rows[r].c[c] : ""));
            fbj_arr_push(arr, row);
        }
        return arr;
    }
    while (r < g->nrows && row_is_empty(&g->rows[r])) r++;
    if (r >= g->nrows) return arr;
    size_t ncols = g->rows[r].n;
    char **keys = (char **)calloc(ncols ? ncols : 1, sizeof(char *));
    for (size_t c = 0; c < ncols; c++) {
        FbBuf k;
        fb_buf_init(&k);
        const char *h = g->rows[r].c[c];
        if (h) { char *t = fb_strdup(h); fb_buf_puts(&k, fb_trim(t)); free(t); }
        if (!k.len) fb_buf_printf(&k, "Column %zu", c + 1);
        for (int dup = 2;; dup++) {           /* make keys unique */
            int clash = 0;
            for (size_t p = 0; p < c; p++) if (!strcmp(keys[p], fb_buf_cstr(&k))) { clash = 1; break; }
            if (!clash) break;
            size_t base = k.len;
            char *t = fb_strdup(fb_buf_cstr(&k));
            char *us = strrchr(t, '_');
            if (us && dup > 2) base = (size_t)(us - t);
            k.len = base; k.data[base] = 0;
            fb_buf_printf(&k, "_%d", dup);
            free(t);
        }
        keys[c] = fb_buf_detach(&k);
    }
    for (r = r + 1; r < g->nrows; r++) {
        if (skip_empty_rows && row_is_empty(&g->rows[r])) continue;
        FbJson *o = fbj_obj();
        for (size_t c = 0; c < ncols; c++) {
            const char *v = c < g->rows[r].n ? g->rows[r].c[c] : NULL;
            fbj_obj_set(o, keys[c], fbj_str(v ? v : ""));
        }
        if (include_row_no) fbj_obj_set(o, "_row", fbj_num((double)(r + 1)));
        fbj_arr_push(arr, o);
    }
    for (size_t c = 0; c < ncols; c++) free(keys[c]);
    free(keys);
    return arr;
}

int fbcsv_parse(const char *text, size_t len, FbGrid *out)
{
    fbgrid_init(out);
    const char *p = text, *end = text + len;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    /* detect delimiter on first line (outside quotes) */
    int cnt[4] = { 0, 0, 0, 0 };
    const char delims[4] = { ',', ';', '\t', '|' };
    int inq = 0;
    for (const char *q = p; q < end && (inq || (*q != '\n' && *q != '\r')); q++) {
        if (*q == '"') inq = !inq;
        else if (!inq) for (int d = 0; d < 4; d++) if (*q == delims[d]) cnt[d]++;
    }
    int best = 0;
    for (int d = 1; d < 4; d++) if (cnt[d] > cnt[best]) best = d;
    char delim = delims[best];

    FbBuf f;
    fb_buf_init(&f);
    if (grid_row_add(out)) return -1;
    int quoted = 0, have_field = 0;
    while (p < end) {
        char c = *p;
        if (quoted) {
            if (c == '"') {
                if (p + 1 < end && p[1] == '"') { fb_buf_putc(&f, '"'); p += 2; continue; }
                quoted = 0; p++; continue;
            }
            fb_buf_putc(&f, c); p++; continue;
        }
        if (c == '"' && f.len == 0) { quoted = 1; have_field = 1; p++; continue; }
        if (c == delim || c == '\n' || c == '\r') {
            FbRow *row = &out->rows[out->nrows - 1];
            row_push(row, (f.len || have_field) ? (have_field && !f.len ? fb_strdup("") : fb_buf_detach(&f)) : NULL);
            fb_buf_free(&f); fb_buf_init(&f);
            have_field = 0;
            if (c == delim) { p++; continue; }
            if (c == '\r' && p + 1 < end && p[1] == '\n') p++;
            p++;
            if (p < end) { if (grid_row_add(out)) { fb_buf_free(&f); return -1; } }
            continue;
        }
        fb_buf_putc(&f, c);
        p++;
    }
    if (p == end && (out->nrows > 0)) {
        FbRow *row = &out->rows[out->nrows - 1];
        if (f.len || have_field || row->n) row_push(row, (f.len || have_field) ? (have_field && !f.len ? fb_strdup("") : fb_buf_detach(&f)) : NULL);
        fb_buf_free(&f);
        if (row->n == 0) { free(row->c); out->nrows--; }
    }
    return 0;
}

void fbcsv_write_field(FbBuf *out, const char *s, char delim)
{
    int need = 0;
    for (const char *p = s; *p; p++)
        if (*p == delim || *p == '"' || *p == '\n' || *p == '\r') { need = 1; break; }
    if (!need && (s[0] == ' ' || (s[0] && s[strlen(s) - 1] == ' '))) need = 1;
    if (!need) { fb_buf_puts(out, s); return; }
    fb_buf_putc(out, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '"') fb_buf_putc(out, '"');
        fb_buf_putc(out, *p);
    }
    fb_buf_putc(out, '"');
}

/* ================================================================ XML helpers */

static const char *tag_end(const char *p, const char *end)
{
    char q = 0;
    for (p++; p < end; p++) {
        if (q) { if (*p == q) q = 0; }
        else if (*p == '"' || *p == '\'') q = *p;
        else if (*p == '>') return p + 1;
    }
    return NULL;
}

static int tag_selfclosing(const char *te) { return te[-2] == '/'; }

static int attr_get(const char *tag, const char *te, const char *name, FbBuf *out)
{
    const char *p = tag + 1;
    size_t nl = strlen(name);
    while (p < te && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
    while (p < te) {
        while (p < te && isspace((unsigned char)*p)) p++;
        if (p >= te || *p == '>' || *p == '/') break;
        const char *ns = p;
        while (p < te && *p != '=' && !isspace((unsigned char)*p) && *p != '>' && *p != '/') p++;
        size_t l = (size_t)(p - ns);
        while (p < te && isspace((unsigned char)*p)) p++;
        if (p >= te || *p != '=') continue;
        p++;
        while (p < te && isspace((unsigned char)*p)) p++;
        if (p >= te || (*p != '"' && *p != '\'')) break;
        char q = *p++;
        const char *vs = p;
        while (p < te && *p != q) p++;
        const char *ve = p;
        if (p < te) p++;
        if (l == nl && !memcmp(ns, name, nl)) {
            if (out) { fb_buf_clear(out); fb_xml_unescape(out, vs, (size_t)(ve - vs)); }
            return 1;
        }
    }
    return 0;
}

static int tag_named(const char *p, const char *name)
{ /* p at '<' : is this an opening tag called `name`? */
    size_t n = strlen(name);
    if (strncmp(p + 1, name, n)) return 0;
    char c = p[1 + n];
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* find next opening tag called name in [p,end) */
static const char *find_tag(const char *p, const char *end, const char *name)
{
    while (p < end && (p = (const char *)memchr(p, '<', (size_t)(end - p))) != NULL) {
        if (tag_named(p, name)) return p;
        p++;
    }
    return NULL;
}

static const char *find_str(const char *p, const char *end, const char *needle)
{
    const char *r = strstr(p, needle);
    return (r && r < end) ? r : NULL;
}

/* gather text of all <t> elements under [s,e) (skipping phonetic runs) */
static void collect_text(const char *s, const char *e, FbBuf *out)
{
    const char *p = s;
    while (p < e) {
        const char *lt = (const char *)memchr(p, '<', (size_t)(e - p));
        if (!lt) break;
        if (tag_named(lt, "rPh")) {
            const char *cl = find_str(lt, e, "</rPh>");
            p = cl ? cl + 6 : e;
        } else if (tag_named(lt, "t")) {
            const char *te = tag_end(lt, e);
            if (!te) break;
            if (tag_selfclosing(te)) { p = te; continue; }
            const char *cl = find_str(te, e, "</t>");
            if (!cl) break;
            fb_xml_unescape(out, te, (size_t)(cl - te));
            p = cl + 4;
        } else p = lt + 1;
    }
}

/* ================================================================ data model */

typedef struct { int col; char *xml; int w; } XCell;
typedef struct {
    int r;
    char *attrs;          /* attribute text after "<row" */
    int dirty;
    XCell *cells;
    size_t n, cap;
} XRow;
typedef struct {
    char *name, *part;
    int sheet_id;
    char *rid;
    int loaded, is_new, dirty, in_workbook;
    char *prefix, *suffix;
    XRow *rows;
    size_t nrows, caprows;
} XSheet;

struct FbXEdit {
    FbZip zip;
    int has_zip;
    char *wb_part, *rels_part, *styles_part, *sst_part;
    FbBuf wb_xml, rels_xml, ct_xml, styles_xml;
    int have_wb, have_rels, have_ct, have_styles;
    int wb_mod, rels_mod, ct_mod, styles_mod;
    XSheet *sheets;
    size_t n, cap;
    char **sst;
    size_t nsst;
    unsigned char *xf_date;
    size_t nxf;
    int date1904;
    int is_new_book;
    int bold_xf;          /* >=0 usable style id, -1 not tried yet, -2 unavailable */
    int formulas_written;
    int dirty;
};

static void sheet_free(XSheet *s)
{
    free(s->name); free(s->part); free(s->rid); free(s->prefix); free(s->suffix);
    for (size_t i = 0; i < s->nrows; i++) {
        for (size_t k = 0; k < s->rows[i].n; k++) free(s->rows[i].cells[k].xml);
        free(s->rows[i].cells);
        free(s->rows[i].attrs);
    }
    free(s->rows);
}

void fbxe_close(FbXEdit *x)
{
    if (!x) return;
    for (size_t i = 0; i < x->n; i++) sheet_free(&x->sheets[i]);
    free(x->sheets);
    for (size_t i = 0; i < x->nsst; i++) free(x->sst[i]);
    free(x->sst);
    free(x->xf_date);
    free(x->wb_part); free(x->rels_part); free(x->styles_part); free(x->sst_part);
    fb_buf_free(&x->wb_xml); fb_buf_free(&x->rels_xml); fb_buf_free(&x->ct_xml); fb_buf_free(&x->styles_xml);
    if (x->has_zip) fbzip_close(&x->zip);
    free(x);
}

int fbxe_dirty(const FbXEdit *x) { return x->dirty; }

/* ---------------------------------------------------------- shared strings */

static void parse_sst(FbXEdit *x, const char *xml, size_t len)
{
    const char *p = xml, *end = xml + len;
    size_t cap = 0;
    while (p < end && (p = find_tag(p, end, "si")) != NULL) {
        const char *te = tag_end(p, end);
        if (!te) break;
        FbBuf b;
        fb_buf_init(&b);
        if (tag_selfclosing(te)) p = te;
        else {
            const char *cl = find_str(te, end, "</si>");
            if (!cl) { fb_buf_free(&b); break; }
            collect_text(te, cl, &b);
            p = cl + 5;
        }
        if (x->nsst == cap) {
            cap = cap ? cap * 2 : 256;
            char **ns = (char **)realloc(x->sst, cap * sizeof(char *));
            if (!ns) { fb_buf_free(&b); break; }
            x->sst = ns;
        }
        x->sst[x->nsst++] = fb_buf_detach(&b);
    }
}

/* ------------------------------------------------------------------ styles */

static int builtin_date_fmt(int id)
{
    return (id >= 14 && id <= 22) || (id >= 27 && id <= 36) || (id >= 45 && id <= 47) || (id >= 50 && id <= 58);
}

static int fmt_is_date(const char *code)
{
    for (const char *p = code; *p; p++) {
        if (*p == '"') { p++; while (*p && *p != '"') p++; if (!*p) break; }
        else if (*p == '[') {
            char c = (char)tolower((unsigned char)p[1]);
            if (c == 'h' || c == 'm' || c == 's') return 1;
            while (*p && *p != ']') p++;
            if (!*p) break;
        } else if (*p == '\\' || *p == '_' || *p == '*') { if (p[1]) p++; }
        else if (strchr("ymdhsYMDHS", *p)) return 1;
    }
    return 0;
}

static void parse_styles(FbXEdit *x, const char *xml, size_t len)
{
    const char *end = xml + len;
    int cid[512], cdate[512], nc = 0;
    const char *nf = find_tag(xml, end, "numFmts");
    if (nf) {
        const char *nfe = find_str(nf, end, "</numFmts>");
        const char *p = nf;
        while (nfe && (p = find_tag(p + 1, nfe, "numFmt")) != NULL && nc < 512) {
            const char *te = tag_end(p, end);
            if (!te) break;
            FbBuf id, code;
            fb_buf_init(&id); fb_buf_init(&code);
            if (attr_get(p, te, "numFmtId", &id) && attr_get(p, te, "formatCode", &code)) {
                cid[nc] = atoi(fb_buf_cstr(&id));
                cdate[nc] = fmt_is_date(fb_buf_cstr(&code));
                nc++;
            }
            fb_buf_free(&id); fb_buf_free(&code);
            p = te - 1;      /* loop does find_tag(p + 1): don't skip an adjacent tag */
        }
    }
    const char *cx = find_tag(xml, end, "cellXfs");
    if (!cx) return;
    const char *cxe = find_str(cx, end, "</cellXfs>");
    if (!cxe) return;
    size_t cap = 64;
    x->xf_date = (unsigned char *)calloc(cap, 1);
    const char *p = cx;
    while ((p = find_tag(p + 1, cxe, "xf")) != NULL) {
        const char *te = tag_end(p, end);
        if (!te) break;
        FbBuf id;
        fb_buf_init(&id);
        int isdate = 0;
        if (attr_get(p, te, "numFmtId", &id)) {
            int v = atoi(fb_buf_cstr(&id));
            isdate = builtin_date_fmt(v);
            for (int i = 0; i < nc; i++) if (cid[i] == v) isdate = cdate[i];
        }
        fb_buf_free(&id);
        if (x->nxf == cap) {
            cap *= 2;
            unsigned char *nd = (unsigned char *)realloc(x->xf_date, cap);
            if (!nd) break;
            memset(nd + x->nxf, 0, cap - x->nxf);
            x->xf_date = nd;
        }
        x->xf_date[x->nxf++] = (unsigned char)isdate;
        p = te - 1;
    }
}

static int xf_is_date(const FbXEdit *x, const char *s)
{
    if (!s || !x->xf_date) return 0;
    size_t i = (size_t)atoi(s);
    return i < x->nxf && x->xf_date[i];
}

/* --------------------------------------------------------------- load book */

static char *dir_of(const char *part)
{
    const char *s = strrchr(part, '/');
    return s ? fb_strndup(part, (size_t)(s - part) + 1) : fb_strdup("");
}

static char *resolve_target(const char *dir, const char *target)
{
    FbBuf b;
    fb_buf_init(&b);
    if (target[0] == '/') fb_buf_puts(&b, target + 1);
    else {
        fb_buf_puts(&b, dir);
        while (!strncmp(target, "./", 2)) target += 2;
        fb_buf_puts(&b, target);
    }
    return fb_buf_detach(&b);
}

static int load_part(FbXEdit *x, const char *name, FbBuf *out)
{
    int i = fbzip_find(&x->zip, name);
    if (i < 0) return -1;
    return fbzip_extract(&x->zip, (size_t)i, out, NULL, 0);
}

static XSheet *sheet_push(FbXEdit *x)
{
    if (x->n == x->cap) {
        size_t nc = x->cap ? x->cap * 2 : 8;
        XSheet *ns = (XSheet *)realloc(x->sheets, nc * sizeof(XSheet));
        if (!ns) return NULL;
        x->sheets = ns;
        x->cap = nc;
    }
    XSheet *s = &x->sheets[x->n++];
    memset(s, 0, sizeof *s);
    return s;
}

static int load_book(FbXEdit *x, char *err, size_t errsz)
{
    fb_buf_init(&x->wb_xml); fb_buf_init(&x->rels_xml); fb_buf_init(&x->ct_xml); fb_buf_init(&x->styles_xml);
    x->bold_xf = -1;
    x->wb_part = fb_strdup("xl/workbook.xml");
    FbBuf root;
    fb_buf_init(&root);
    if (!load_part(x, "_rels/.rels", &root)) {
        const char *p = root.data, *end = root.data + root.len;
        while ((p = find_tag(p, end, "Relationship")) != NULL) {
            const char *te = tag_end(p, end);
            if (!te) break;
            FbBuf ty, tg;
            fb_buf_init(&ty); fb_buf_init(&tg);
            if (attr_get(p, te, "Type", &ty) && attr_get(p, te, "Target", &tg) && fb_ends_with(fb_buf_cstr(&ty), "/officeDocument")) {
                free(x->wb_part);
                x->wb_part = resolve_target("", fb_buf_cstr(&tg));
            }
            fb_buf_free(&ty); fb_buf_free(&tg);
            p = te;
        }
    }
    fb_buf_free(&root);
    if (load_part(x, x->wb_part, &x->wb_xml)) { fb_set_err(err, errsz, "not a valid .xlsx: workbook part missing"); return -1; }
    x->have_wb = 1;
    char *wdir = dir_of(x->wb_part);
    const char *slash = strrchr(x->wb_part, '/');
    FbBuf rp;
    fb_buf_init(&rp);
    fb_buf_printf(&rp, "%s_rels/%s.rels", wdir, slash ? slash + 1 : x->wb_part);
    x->rels_part = fb_buf_detach(&rp);
    if (!load_part(x, x->rels_part, &x->rels_xml)) x->have_rels = 1;
    if (!load_part(x, "[Content_Types].xml", &x->ct_xml)) x->have_ct = 1;

    /* relationships: id -> (type,target) */
    const char *wend = x->wb_xml.data + x->wb_xml.len;
    const char *wp = find_tag(x->wb_xml.data, wend, "workbookPr");
    if (wp) {
        const char *te = tag_end(wp, wend);
        FbBuf v;
        fb_buf_init(&v);
        if (te && attr_get(wp, te, "date1904", &v)) x->date1904 = (!strcmp(fb_buf_cstr(&v), "1") || !fb_stricmp(fb_buf_cstr(&v), "true"));
        fb_buf_free(&v);
    }
    const char *sp = find_tag(x->wb_xml.data, wend, "sheets");
    const char *sp_end = sp ? find_str(sp, wend, "</sheets>") : NULL;
    int maxid = 0;
    if (sp && sp_end) {
        const char *p = sp;
        while ((p = find_tag(p + 1, sp_end, "sheet")) != NULL) {
            const char *te = tag_end(p, wend);
            if (!te) break;
            FbBuf nm, rid, sid;
            fb_buf_init(&nm); fb_buf_init(&rid); fb_buf_init(&sid);
            attr_get(p, te, "name", &nm);
            int has_rid = attr_get(p, te, "r:id", &rid);
            attr_get(p, te, "sheetId", &sid);
            if (has_rid) {
                /* look the target up in the rels */
                const char *rb = x->rels_xml.data ? x->rels_xml.data : "";
                const char *rend = rb + x->rels_xml.len;
                const char *q = rb;
                while ((q = find_tag(q, rend, "Relationship")) != NULL) {
                    const char *qe = tag_end(q, rend);
                    if (!qe) break;
                    FbBuf id, tg, ty;
                    fb_buf_init(&id); fb_buf_init(&tg); fb_buf_init(&ty);
                    if (attr_get(q, qe, "Id", &id) && !strcmp(fb_buf_cstr(&id), fb_buf_cstr(&rid)) &&
                        attr_get(q, qe, "Target", &tg) && attr_get(q, qe, "Type", &ty) && fb_ends_with(fb_buf_cstr(&ty), "/worksheet")) {
                        XSheet *s = sheet_push(x);
                        if (s) {
                            s->name = fb_strdup(fb_buf_cstr(&nm));
                            s->part = resolve_target(wdir, fb_buf_cstr(&tg));
                            s->rid = fb_strdup(fb_buf_cstr(&rid));
                            s->sheet_id = atoi(fb_buf_cstr(&sid));
                            s->in_workbook = 1;
                            if (s->sheet_id > maxid) maxid = s->sheet_id;
                        }
                    }
                    fb_buf_free(&id); fb_buf_free(&tg); fb_buf_free(&ty);
                    q = qe;
                }
            }
            fb_buf_free(&nm); fb_buf_free(&rid); fb_buf_free(&sid);
            p = te - 1;
        }
    }
    /* shared strings + styles */
    {
        const char *rb = x->rels_xml.data ? x->rels_xml.data : "";
        const char *rend = rb + x->rels_xml.len;
        const char *q = rb;
        while ((q = find_tag(q, rend, "Relationship")) != NULL) {
            const char *qe = tag_end(q, rend);
            if (!qe) break;
            FbBuf tg, ty;
            fb_buf_init(&tg); fb_buf_init(&ty);
            if (attr_get(q, qe, "Target", &tg) && attr_get(q, qe, "Type", &ty)) {
                if (fb_ends_with(fb_buf_cstr(&ty), "/sharedStrings")) x->sst_part = resolve_target(wdir, fb_buf_cstr(&tg));
                else if (fb_ends_with(fb_buf_cstr(&ty), "/styles")) x->styles_part = resolve_target(wdir, fb_buf_cstr(&tg));
            }
            fb_buf_free(&tg); fb_buf_free(&ty);
            q = qe;
        }
    }
    free(wdir);
    if (x->sst_part) {
        FbBuf s;
        fb_buf_init(&s);
        if (!load_part(x, x->sst_part, &s)) parse_sst(x, fb_buf_cstr(&s), s.len);
        fb_buf_free(&s);
    }
    if (x->styles_part && !load_part(x, x->styles_part, &x->styles_xml)) {
        x->have_styles = 1;
        parse_styles(x, x->styles_xml.data, x->styles_xml.len);
    }
    return 0;
}

/* ------------------------------------------------------------ sheet parse */

static XRow *row_insert_at(XSheet *sh, size_t i)
{
    if (sh->nrows == sh->caprows) {
        size_t nc = sh->caprows ? sh->caprows * 2 : 32;
        XRow *nr = (XRow *)realloc(sh->rows, nc * sizeof(XRow));
        if (!nr) return NULL;
        sh->rows = nr;
        sh->caprows = nc;
    }
    memmove(sh->rows + i + 1, sh->rows + i, (sh->nrows - i) * sizeof(XRow));
    sh->nrows++;
    memset(&sh->rows[i], 0, sizeof(XRow));
    return &sh->rows[i];
}

static int row_add_cell_end(XRow *row, int col, char *xml)
{
    if (row->n == row->cap) {
        size_t nc = row->cap ? row->cap * 2 : 8;
        XCell *c = (XCell *)realloc(row->cells, nc * sizeof(XCell));
        if (!c) { free(xml); return -1; }
        row->cells = c;
        row->cap = nc;
    }
    row->cells[row->n].col = col;
    row->cells[row->n].xml = xml;
    row->cells[row->n].w = 0;
    row->n++;
    return 0;
}

static int parse_row_cells(XRow *row, const char *s, const char *e)
{
    int prevcol = 0;
    const char *p = s;
    while (p < e) {
        const char *lt = (const char *)memchr(p, '<', (size_t)(e - p));
        if (!lt) break;
        if (!tag_named(lt, "c")) { p = lt + 1; continue; }
        const char *te = tag_end(lt, e + 1);
        if (!te) return -1;
        FbBuf rb;
        fb_buf_init(&rb);
        int col = prevcol + 1, rr;
        if (attr_get(lt, te, "r", &rb)) {
            int c2;
            if (!fbx_parse_ref(fb_buf_cstr(&rb), &rr, &c2)) col = c2;
        }
        fb_buf_free(&rb);
        const char *cend;
        if (tag_selfclosing(te)) cend = te;
        else {
            const char *cl = find_str(te, e + 4, "</c>");
            if (!cl) return -1;
            cend = cl + 4;
        }
        char *xml = fb_strndup(lt, (size_t)(cend - lt));
        if (!xml || row_add_cell_end(row, col, xml)) return -1;
        prevcol = col;
        p = cend;
    }
    return 0;
}

static int cmp_rows(const void *a, const void *b) { return ((const XRow *)a)->r - ((const XRow *)b)->r; }

static int sheet_parse(XSheet *sh, const char *xml, size_t len, char *err, size_t errsz)
{
    const char *end = xml + len;
    const char *sd = find_tag(xml, end, "sheetData");
    if (!sd) { fb_set_err(err, errsz, "worksheet has no sheetData: %s", sh->part); return -1; }
    const char *te = tag_end(sd, end);
    if (!te) { fb_set_err(err, errsz, "corrupt worksheet: %s", sh->part); return -1; }
    FbBuf pre, suf;
    fb_buf_init(&pre); fb_buf_init(&suf);
    fb_buf_append(&pre, xml, (size_t)(sd - xml));
    fb_buf_puts(&pre, "<sheetData>");
    const char *rs, *re;
    if (tag_selfclosing(te)) {
        fb_buf_puts(&suf, "</sheetData>");
        fb_buf_append(&suf, te, (size_t)(end - te));
        rs = re = te;
    } else {
        const char *cl = find_str(te, end, "</sheetData>");
        if (!cl) { fb_buf_free(&pre); fb_buf_free(&suf); fb_set_err(err, errsz, "corrupt worksheet: %s", sh->part); return -1; }
        fb_buf_append(&suf, cl, (size_t)(end - cl));
        rs = te; re = cl;
    }
    sh->prefix = fb_buf_detach(&pre);
    sh->suffix = fb_buf_detach(&suf);
    int prev = 0, sorted = 1;
    const char *p = rs;
    while (p < re) {
        const char *lt = (const char *)memchr(p, '<', (size_t)(re - p));
        if (!lt) break;
        if (!tag_named(lt, "row")) { p = lt + 1; continue; }
        const char *rte = tag_end(lt, re + 1);
        if (!rte) break;
        FbBuf rb;
        fb_buf_init(&rb);
        int rn = prev + 1;
        if (attr_get(lt, rte, "r", &rb)) rn = atoi(fb_buf_cstr(&rb));
        fb_buf_free(&rb);
        if (rn <= prev) sorted = 0;
        XRow *row = row_insert_at(sh, sh->nrows);
        if (!row) return -1;
        row->r = rn;
        int self = tag_selfclosing(rte);
        row->attrs = fb_strndup(lt + 4, (size_t)(rte - (self ? 2 : 1)) - (size_t)(lt + 4));
        prev = rn;
        if (self) { p = rte; continue; }
        const char *cl = find_str(rte, re + 6, "</row>");
        if (!cl) { fb_set_err(err, errsz, "corrupt row in %s", sh->part); return -1; }
        if (parse_row_cells(row, rte, cl)) { fb_set_err(err, errsz, "corrupt cell in %s", sh->part); return -1; }
        p = cl + 6;
    }
    if (!sorted) qsort(sh->rows, sh->nrows, sizeof(XRow), cmp_rows);
    return 0;
}

static int sheet_load(FbXEdit *x, XSheet *sh, char *err, size_t errsz)
{
    if (sh->loaded) return 0;
    sh->loaded = 1;
    if (sh->is_new) return 0;
    int i = fbzip_find(&x->zip, sh->part);
    if (i < 0) { fb_set_err(err, errsz, "worksheet part missing: %s", sh->part); return -1; }
    FbBuf b;
    fb_buf_init(&b);
    if (fbzip_extract(&x->zip, (size_t)i, &b, err, errsz)) { fb_buf_free(&b); return -1; }
    int rc = sheet_parse(sh, fb_buf_cstr(&b), b.len, err, errsz);
    fb_buf_free(&b);
    return rc;
}

/* ---------------------------------------------------------- sheet editing */

static XRow *sheet_row(XSheet *sh, int r, int create)
{
    size_t i = sh->nrows;
    while (i > 0 && sh->rows[i - 1].r > r) i--;
    if (i > 0 && sh->rows[i - 1].r == r) return &sh->rows[i - 1];
    if (!create) return NULL;
    XRow *row = row_insert_at(sh, i);
    if (!row) return NULL;
    row->r = r;
    FbBuf a;
    fb_buf_init(&a);
    fb_buf_printf(&a, " r=\"%d\"", r);
    row->attrs = fb_buf_detach(&a);
    return row;
}

static XCell *row_cell(XRow *row, int col, int create)
{
    size_t i = row->n;
    while (i > 0 && row->cells[i - 1].col > col) i--;
    if (i > 0 && row->cells[i - 1].col == col) return &row->cells[i - 1];
    if (!create) return NULL;
    if (row->n == row->cap) {
        size_t nc = row->cap ? row->cap * 2 : 8;
        XCell *c = (XCell *)realloc(row->cells, nc * sizeof(XCell));
        if (!c) return NULL;
        row->cells = c;
        row->cap = nc;
    }
    memmove(row->cells + i + 1, row->cells + i, (row->n - i) * sizeof(XCell));
    row->n++;
    row->cells[i].col = col;
    row->cells[i].xml = NULL;
    row->cells[i].w = 0;
    return &row->cells[i];
}

static void strip_attr(char *attrs, const char *name)
{
    size_t nl = strlen(name);
    for (char *p = attrs; *p; p++) {
        if ((p == attrs || isspace((unsigned char)p[-1])) && !strncmp(p, name, nl) && p[nl] == '=') {
            char q = p[nl + 1];
            char *e = p + nl + 2;
            while (*e && *e != q) e++;
            if (*e) e++;
            while (*e && isspace((unsigned char)*e)) e++;
            memmove(p, e, strlen(e) + 1);
            return;
        }
    }
}

static int cell_is_nonempty(const char *xml)
{
    const char *te = tag_end(xml, xml + strlen(xml));
    if (!te || tag_selfclosing(te)) return 0;
    return strstr(te, "<v>") || strstr(te, "<v ") || strstr(te, "<is>") || strstr(te, "<is ") || strstr(te, "<f");
}

static char *sanitize_sheet_name(const char *in);

static XSheet *find_sheet(FbXEdit *x, const char *name)
{
    if (!x->n) return NULL;
    if (!name || !*name) return &x->sheets[0];
    for (size_t i = 0; i < x->n; i++)
        if (!fb_stricmp(x->sheets[i].name, name)) return &x->sheets[i];
    /* sheets we created are stored under their sanitised name: look that up too */
    char *clean = sanitize_sheet_name(name);
    XSheet *found = NULL;
    if (clean) {
        for (size_t i = 0; i < x->n && !found; i++)
            if (!fb_stricmp(x->sheets[i].name, clean)) found = &x->sheets[i];
        free(clean);
    }
    return found;
}

static char *sanitize_sheet_name(const char *in)
{
    FbBuf b;
    fb_buf_init(&b);
    size_t chars = 0;
    for (const char *p = in; *p && chars < 31; p++) {
        unsigned char c = (unsigned char)*p;
        if (strchr(":\\/?*[]", c)) c = '_';
        fb_buf_putc(&b, (char)c);
        if ((c & 0xC0) != 0x80) chars++;
    }
    /* do not cut a UTF-8 sequence in half */
    if (*in && b.len == 0) fb_buf_puts(&b, "Sheet");
    return fb_buf_detach(&b);
}

static XSheet *add_sheet(FbXEdit *x, const char *name)
{
    XSheet *s = sheet_push(x);
    if (!s) return NULL;
    s->name = sanitize_sheet_name(name && *name ? name : "Sheet1");
    int maxn = 0, maxid = 0;
    for (size_t i = 0; i + 1 < x->n; i++) {
        int n;
        if (sscanf(x->sheets[i].part, "xl/worksheets/sheet%d.xml", &n) == 1 && n > maxn) maxn = n;
        if (x->sheets[i].sheet_id > maxid) maxid = x->sheets[i].sheet_id;
    }
    if (x->has_zip)
        for (size_t i = 0; i < x->zip.n; i++) {
            int n;
            if (sscanf(x->zip.e[i].name, "xl/worksheets/sheet%d.xml", &n) == 1 && n > maxn) maxn = n;
        }
    FbBuf pt;
    fb_buf_init(&pt);
    fb_buf_printf(&pt, "xl/worksheets/sheet%d.xml", maxn + 1);
    s->part = fb_buf_detach(&pt);
    s->sheet_id = maxid + 1;
    s->is_new = 1;
    s->loaded = 1;
    s->dirty = 1;
    s->prefix = fb_strdup("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
                          "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
                          "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
                          "<dimension ref=\"A1\"/><sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
                          "<sheetFormatPr defaultRowHeight=\"15\"/>");
    s->suffix = fb_strdup("</sheetData><pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/></worksheet>");
    x->dirty = 1;
    return s;
}

/* ------------------------------------------------------------ bold style */

static int count_children(const char *p, const char *end, const char *name)
{
    int n = 0;
    while ((p = find_tag(p, end, name)) != NULL) { n++; p++; }
    return n;
}

static int set_count_attr(FbBuf *b, size_t tag_off, int newcount)
{
    const char *base = b->data;
    const char *te = tag_end(base + tag_off, base + b->len);
    if (!te) return -1;
    const char *c = strstr(base + tag_off, "count=\"");
    if (!c || c > te) return -1;
    size_t vs = (size_t)(c - base) + 7;
    size_t ve = vs;
    while (b->data[ve] && b->data[ve] != '"') ve++;
    char num[16];
    snprintf(num, sizeof num, "%d", newcount);
    FbBuf nb;
    fb_buf_init(&nb);
    fb_buf_append(&nb, b->data, vs);
    fb_buf_puts(&nb, num);
    fb_buf_append(&nb, b->data + ve, b->len - ve);
    fb_buf_free(b);
    *b = nb;
    return 0;
}

static int ensure_bold(FbXEdit *x)
{
    if (x->bold_xf >= 0 || x->bold_xf == -2) return x->bold_xf;
    x->bold_xf = -2;
    if (!x->have_styles) return -2;
    FbBuf *s = &x->styles_xml;
    const char *fonts = find_tag(s->data, s->data + s->len, "fonts");
    const char *cxfs = find_tag(s->data, s->data + s->len, "cellXfs");
    if (!fonts || !cxfs) return -2;
    const char *fonts_end = find_str(fonts, s->data + s->len, "</fonts>");
    const char *cxfs_end = find_str(cxfs, s->data + s->len, "</cellXfs>");
    if (!fonts_end || !cxfs_end) return -2;
    const char *fte = tag_end(fonts, s->data + s->len);
    if (!fte || tag_selfclosing(fte)) return -2;
    int nfonts = count_children(fte, fonts_end, "font");
    const char *f0 = find_tag(fte, fonts_end, "font");
    if (!f0) return -2;
    const char *f0te = tag_end(f0, fonts_end);
    const char *f0end = find_str(f0, fonts_end, "</font>");
    if (!f0te || !f0end || tag_selfclosing(f0te)) return -2;
    FbBuf nf;
    fb_buf_init(&nf);
    fb_buf_puts(&nf, "<font><b/>");
    fb_buf_append(&nf, f0te, (size_t)(f0end - f0te));
    fb_buf_puts(&nf, "</font>");
    int nxfs = count_children(tag_end(cxfs, s->data + s->len), cxfs_end, "xf");
    FbBuf xf;
    fb_buf_init(&xf);
    fb_buf_printf(&xf, "<xf numFmtId=\"0\" fontId=\"%d\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>", nfonts);

    /* rebuild the styles xml: new font before </fonts>, new xf before </cellXfs> */
    size_t xf_off = (size_t)(cxfs_end - s->data);
    size_t font_off = (size_t)(fonts_end - s->data);
    if (xf_off < font_off) { fb_buf_free(&nf); fb_buf_free(&xf); return -2; }   /* unusual part order */
    FbBuf out;
    fb_buf_init(&out);
    fb_buf_append(&out, s->data, font_off);
    fb_buf_append(&out, nf.data, nf.len);
    fb_buf_append(&out, s->data + font_off, xf_off - font_off);
    fb_buf_append(&out, xf.data, xf.len);
    fb_buf_append(&out, s->data + xf_off, s->len - xf_off);
    fb_buf_free(s);
    *s = out;
    fb_buf_free(&nf); fb_buf_free(&xf);
    /* update the count="" attributes (re-locate the tags: offsets moved after the insert) */
    const char *cx2 = find_tag(s->data, s->data + s->len, "cellXfs");
    if (!cx2 || set_count_attr(s, (size_t)(cx2 - s->data), nxfs + 1)) return -2;
    const char *fo2 = find_tag(s->data, s->data + s->len, "fonts");
    if (!fo2 || set_count_attr(s, (size_t)(fo2 - s->data), nfonts + 1)) return -2;
    x->styles_mod = 1;
    x->bold_xf = nxfs;
    return x->bold_xf;
}

/* -------------------------------------------------------------- cell write */

static int looks_numeric(const char *s)
{
    const char *p = s;
    if (*p == '-') p++;
    if (!isdigit((unsigned char)*p)) return 0;
    if (p[0] == '0' && isdigit((unsigned char)p[1])) return 0;     /* leading zeros stay text */
    int digits = 0;
    while (isdigit((unsigned char)*p)) { p++; digits++; }
    if (*p == '.') {
        p++;
        if (!isdigit((unsigned char)*p)) return 0;
        while (isdigit((unsigned char)*p)) { p++; digits++; }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!isdigit((unsigned char)*p)) return 0;
        while (isdigit((unsigned char)*p)) p++;
    }
    return *p == 0 && digits <= 15;
}

static int utf8_chars(const char *s)
{
    int n = 0;
    for (; *s; s++) if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

int fbxe_set(FbXEdit *x, const char *sheet, int row, int col, const char *value,
             const char *type, int bold, char *err, size_t errsz)
{
    if (row < 1 || row > 1048576 || col < 1 || col > 16384) {
        fb_set_err(err, errsz, "cell position out of range (row %d, column %d)", row, col);
        return -1;
    }
    XSheet *sh = find_sheet(x, sheet);
    if (!sh) sh = add_sheet(x, sheet && *sheet ? sheet : "Sheet1");
    if (!sh) { fb_set_err(err, errsz, "out of memory"); return -1; }
    if (sheet_load(x, sh, err, errsz)) return -1;
    if (!value) value = "";

    int kind; /* 0 text, 1 number, 2 formula, 3 blank */
    if (!*value) kind = 3;
    else if (!type || !*type || !strcmp(type, "auto")) kind = (value[0] == '=' && value[1]) ? 2 : looks_numeric(value) ? 1 : 0;
    else if (!strcmp(type, "number")) kind = looks_numeric(value) ? 1 : 0;
    else if (!strcmp(type, "formula")) kind = (value[0] == '=') ? 2 : 0;
    else kind = 0;

    XRow *r = sheet_row(sh, row, 1);
    if (!r) { fb_set_err(err, errsz, "out of memory"); return -1; }
    XCell *c = row_cell(r, col, 1);
    if (!c) { fb_set_err(err, errsz, "out of memory"); return -1; }

    FbBuf style;
    fb_buf_init(&style);
    if (c->xml) {
        const char *te = tag_end(c->xml, c->xml + strlen(c->xml));
        if (te) attr_get(c->xml, te, "s", &style);
    }
    if (bold) {
        int b = x->is_new_book ? 1 : ensure_bold(x);
        if (b >= 0) { fb_buf_clear(&style); fb_buf_printf(&style, "%d", b); }
    }
    char ref[16];
    fbx_col_name(col, ref);
    FbBuf xml;
    fb_buf_init(&xml);
    fb_buf_printf(&xml, "<c r=\"%s%d\"", ref, row);
    if (style.len) fb_buf_printf(&xml, " s=\"%s\"", fb_buf_cstr(&style));
    int width = 0;
    if (kind == 3) fb_buf_puts(&xml, "/>");
    else if (kind == 1) { fb_buf_printf(&xml, "><v>%s</v></c>", value); width = (int)strlen(value); }
    else if (kind == 2) {
        fb_buf_puts(&xml, "><f>");
        fb_xml_escape(&xml, value + 1, strlen(value + 1));
        fb_buf_puts(&xml, "</f></c>");
        x->formulas_written = 1;
        width = 10;
    } else {
        size_t n = strlen(value);
        if (n > 32767) { n = 32767; while (n && ((unsigned char)value[n] & 0xC0) == 0x80) n--; }
        fb_buf_puts(&xml, " t=\"inlineStr\"><is><t xml:space=\"preserve\">");
        fb_xml_escape(&xml, value, n);
        fb_buf_puts(&xml, "</t></is></c>");
        width = utf8_chars(value);
    }
    free(c->xml);
    c->xml = fb_buf_detach(&xml);
    c->w = width;
    fb_buf_free(&style);
    r->dirty = 1;
    sh->dirty = 1;
    x->dirty = 1;
    return 0;
}

int fbxe_last_row(FbXEdit *x, const char *sheet)
{
    XSheet *sh = find_sheet(x, sheet);
    if (!sh || sheet_load(x, sh, NULL, 0)) return 0;
    for (size_t i = sh->nrows; i > 0; i--) {
        XRow *r = &sh->rows[i - 1];
        for (size_t k = 0; k < r->n; k++)
            if (r->cells[k].xml && cell_is_nonempty(r->cells[k].xml)) return r->r;
    }
    return 0;
}

FbJson *fbxe_sheet_names(FbXEdit *x)
{
    FbJson *a = fbj_arr();
    for (size_t i = 0; i < x->n; i++) fbj_arr_push(a, fbj_str(x->sheets[i].name));
    return a;
}

/* ---------------------------------------------------------------- reading */

static void serial_to_text(double v, int date1904, FbBuf *out)
{
    double days = floor(v), frac = v - days;
    long secs = (long)llround(frac * 86400.0);
    if (secs >= 86400) { secs -= 86400; days += 1; }
    if (date1904) days += 1462;
    if (days < 1) { fb_buf_printf(out, "%02ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60); return; }
    long z;
    if (days == 60) { fb_buf_puts(out, "1900-02-29"); goto time_part; }
    z = (long)days - (days < 60 ? 25568 : 25569);
    {
        z += 719468;
        long era = (z >= 0 ? z : z - 146096) / 146097;
        unsigned doe = (unsigned)(z - era * 146097);
        unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        long y = (long)yoe + era * 400;
        unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        unsigned mp = (5 * doy + 2) / 153;
        unsigned d = doy - (153 * mp + 2) / 5 + 1;
        unsigned m = mp < 10 ? mp + 3 : mp - 9;
        if (m <= 2) y++;
        fb_buf_printf(out, "%04ld-%02u-%02u", y, m, d);
    }
time_part:
    if (secs) fb_buf_printf(out, " %02ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60);
}

/* returns 1 when the cell holds a value */
static int cell_value(const FbXEdit *x, const char *xml, FbBuf *out)
{
    const char *end = xml + strlen(xml);
    const char *te = tag_end(xml, end);
    if (!te || tag_selfclosing(te)) return 0;
    FbBuf t, s;
    fb_buf_init(&t); fb_buf_init(&s);
    attr_get(xml, te, "t", &t);
    attr_get(xml, te, "s", &s);
    const char *ty = fb_buf_cstr(&t);
    int has = 0;
    if (!strcmp(ty, "inlineStr")) {
        const char *is = find_tag(te, end, "is");
        const char *ie = is ? find_str(is, end, "</is>") : NULL;
        if (is && ie) { collect_text(is, ie, out); has = out->len > 0; }
    } else {
        const char *v = find_tag(te, end, "v");
        const char *vte = v ? tag_end(v, end) : NULL;
        const char *ve = vte ? find_str(vte, end, "</v>") : NULL;
        if (vte && ve && ve > vte) {
            FbBuf raw;
            fb_buf_init(&raw);
            fb_xml_unescape(&raw, vte, (size_t)(ve - vte));
            const char *r = fb_buf_cstr(&raw);
            if (!strcmp(ty, "s")) {
                size_t i = (size_t)atol(r);
                if (i < x->nsst) { fb_buf_puts(out, x->sst[i]); has = 1; }
            } else if (!strcmp(ty, "b")) { fb_buf_puts(out, r[0] == '1' ? "TRUE" : "FALSE"); has = 1; }
            else if (!strcmp(ty, "str") || !strcmp(ty, "e") || !strcmp(ty, "d")) { fb_buf_puts(out, r); has = raw.len > 0; }
            else {
                char *e;
                double d = strtod(r, &e);
                if (e == r) { fb_buf_puts(out, r); }
                else if (xf_is_date(x, s.len ? fb_buf_cstr(&s) : NULL)) serial_to_text(d, x->date1904, out);
                else if (d == floor(d) && fabs(d) < 1e15) fb_buf_printf(out, "%.0f", d);
                else {
                    char nb[40];
                    snprintf(nb, sizeof nb, "%.15g", d);
                    fb_buf_puts(out, nb);
                }
                has = 1;
            }
            fb_buf_free(&raw);
        }
    }
    fb_buf_free(&t); fb_buf_free(&s);
    return has;
}

int fbxe_get_grid(FbXEdit *x, const char *sheet, FbGrid *out, char *err, size_t errsz)
{
    fbgrid_init(out);
    XSheet *sh = find_sheet(x, sheet);
    if (!sh) { fb_set_err(err, errsz, "sheet not found: %s", sheet ? sheet : "(first)"); return -1; }
    if (sheet_load(x, sh, err, errsz)) return -1;
    int maxr = 0;
    for (size_t i = 0; i < sh->nrows; i++) {
        for (size_t k = 0; k < sh->rows[i].n; k++)
            if (sh->rows[i].cells[k].xml && cell_is_nonempty(sh->rows[i].cells[k].xml)) { maxr = sh->rows[i].r; break; }
    }
    if (maxr > 5000000) { fb_set_err(err, errsz, "sheet too large"); return -1; }
    out->rows = (FbRow *)calloc((size_t)maxr ? (size_t)maxr : 1, sizeof(FbRow));
    if (!out->rows) { fb_set_err(err, errsz, "out of memory"); return -1; }
    out->nrows = (size_t)maxr;
    for (size_t i = 0; i < sh->nrows; i++) {
        XRow *r = &sh->rows[i];
        if (r->r > maxr || r->r < 1) continue;
        int maxc = 0;
        for (size_t k = 0; k < r->n; k++) if (r->cells[k].col > maxc) maxc = r->cells[k].col;
        if (!maxc) continue;
        FbRow *dst = &out->rows[r->r - 1];
        dst->c = (char **)calloc((size_t)maxc, sizeof(char *));
        if (!dst->c) { fbgrid_free(out); fb_set_err(err, errsz, "out of memory"); return -1; }
        dst->n = (size_t)maxc;
        for (size_t k = 0; k < r->n; k++) {
            if (!r->cells[k].xml) continue;
            FbBuf v;
            fb_buf_init(&v);
            if (cell_value(x, r->cells[k].xml, &v) && v.len) dst->c[r->cells[k].col - 1] = fb_buf_detach(&v);
            else fb_buf_free(&v);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ saving */

static void serialize_sheet(const XSheet *sh, FbBuf *out)
{
    int minc = 0, maxc = 0, minr = 0, maxr = 0;
    int *width = NULL;
    for (size_t i = 0; i < sh->nrows; i++) {
        const XRow *r = &sh->rows[i];
        for (size_t k = 0; k < r->n; k++) {
            if (!minr || r->r < minr) minr = r->r;
            if (r->r > maxr) maxr = r->r;
            if (!minc || r->cells[k].col < minc) minc = r->cells[k].col;
            if (r->cells[k].col > maxc) maxc = r->cells[k].col;
        }
    }
    if (sh->is_new && maxc > 0) {
        width = (int *)calloc((size_t)maxc + 1, sizeof(int));
        for (size_t i = 0; width && i < sh->nrows; i++)
            for (size_t k = 0; k < sh->rows[i].n; k++) {
                int w = sh->rows[i].cells[k].w, c = sh->rows[i].cells[k].col;
                if (w > width[c]) width[c] = w;
            }
    }
    /* prefix, with refreshed <dimension> */
    const char *pre = sh->prefix;
    const char *dim = strstr(pre, "<dimension");
    const char *ref = dim ? strstr(dim, "ref=\"") : NULL;
    if (dim && ref && minr) {
        const char *vs = ref + 5, *ve = strchr(vs, '"');
        char a[8], b[8];
        fbx_col_name(minc, a); fbx_col_name(maxc, b);
        if (ve) {
            fb_buf_append(out, pre, (size_t)(vs - pre));
            fb_buf_printf(out, "%s%d:%s%d", a, minr, b, maxr);
            fb_buf_puts(out, ve);
        } else fb_buf_puts(out, pre);
    } else fb_buf_puts(out, pre);
    if (sh->is_new) {
        if (width) {
            fb_buf_puts(out, "<cols>");
            for (int c = 1; c <= maxc; c++) {
                int w = width[c] + 2;
                if (w < 9) w = 9;
                if (w > 60) w = 60;
                fb_buf_printf(out, "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>", c, c, w);
            }
            fb_buf_puts(out, "</cols>");
        }
        fb_buf_puts(out, "<sheetData>");
    }
    free(width);
    for (size_t i = 0; i < sh->nrows; i++) {
        const XRow *r = &sh->rows[i];
        char *attrs = fb_strdup(r->attrs ? r->attrs : "");
        if (r->dirty) strip_attr(attrs, "spans");
        fb_buf_puts(out, "<row");
        fb_buf_puts(out, attrs);
        free(attrs);
        if (!r->n) { fb_buf_puts(out, "/>"); continue; }
        fb_buf_putc(out, '>');
        for (size_t k = 0; k < r->n; k++) if (r->cells[k].xml) fb_buf_puts(out, r->cells[k].xml);
        fb_buf_puts(out, "</row>");
    }
    fb_buf_puts(out, sh->suffix);
}

static void remove_tags_containing(FbBuf *b, const char *open_tag, const char *needle)
{
    char *p = b->data;
    while (p && (p = strstr(p, open_tag)) != NULL) {
        const char *te = tag_end(p, b->data + b->len);
        if (!te) break;
        size_t tl = (size_t)(te - p);
        int hit = 0;
        for (size_t i = 0; i + strlen(needle) <= tl; i++) if (!memcmp(p + i, needle, strlen(needle))) { hit = 1; break; }
        if (hit) {
            size_t off = (size_t)(p - b->data);
            memmove(p, te, b->len - (size_t)(te - b->data) + 1);
            b->len -= tl;
            p = b->data + off;
        } else p = (char *)te;
    }
}

static int insert_before(FbBuf *b, const char *marker, const char *text)
{
    char *p = strstr(b->data, marker);
    if (!p) return -1;
    size_t off = (size_t)(p - b->data);
    FbBuf nb;
    fb_buf_init(&nb);
    fb_buf_append(&nb, b->data, off);
    fb_buf_puts(&nb, text);
    fb_buf_append(&nb, b->data + off, b->len - off);
    fb_buf_free(b);
    *b = nb;
    return 0;
}

static void ensure_calc_on_load(FbBuf *wb)
{
    char *cp = strstr(wb->data, "<calcPr");
    if (cp) {
        const char *te = tag_end(cp, wb->data + wb->len);
        if (!te) return;
        for (const char *q = cp; q < te; q++) if (!strncmp(q, "fullCalcOnLoad", 14)) return;
        size_t off = (size_t)(cp - wb->data) + 7;
        FbBuf nb;
        fb_buf_init(&nb);
        fb_buf_append(&nb, wb->data, off);
        fb_buf_puts(&nb, " fullCalcOnLoad=\"1\"");
        fb_buf_append(&nb, wb->data + off, wb->len - off);
        fb_buf_free(wb);
        *wb = nb;
        return;
    }
    const char *marks[] = { "</definedNames>", "</externalReferences>", "</functionGroups>", "</sheets>" };
    const char *best = NULL;
    size_t bl = 0;
    for (int i = 0; i < 4; i++) {
        const char *m = strstr(wb->data, marks[i]);
        if (m && (!best || m > best)) { best = m; bl = strlen(marks[i]); }
    }
    if (!best) return;
    size_t off = (size_t)(best - wb->data) + bl;
    FbBuf nb;
    fb_buf_init(&nb);
    fb_buf_append(&nb, wb->data, off);
    fb_buf_puts(&nb, "<calcPr fullCalcOnLoad=\"1\"/>");
    fb_buf_append(&nb, wb->data + off, wb->len - off);
    fb_buf_free(wb);
    *wb = nb;
}

static int next_rid(const FbBuf *rels)
{
    int maxn = 0;
    const char *p = rels->data ? rels->data : "";
    while ((p = strstr(p, "Id=\"rId")) != NULL) {
        int n = atoi(p + 7);
        if (n > maxn) maxn = n;
        p += 7;
    }
    return maxn + 1;
}

static const char *NEW_STYLES =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
    "<fonts count=\"2\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font><font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
    "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill><fill><patternFill patternType=\"gray125\"/></fill></fills>"
    "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
    "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
    "<cellXfs count=\"2\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
    "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/></cellXfs>"
    "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles></styleSheet>";

#define WS_CT "application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"
#define REL_NS "http://schemas.openxmlformats.org/officeDocument/2006/relationships"

static int save_new_book(FbXEdit *x, FbZipWriter *w)
{
    if (!x->n) add_sheet(x, "Sheet1");
    FbBuf ct, wb, rels;
    fb_buf_init(&ct); fb_buf_init(&wb); fb_buf_init(&rels);
    fb_buf_puts(&ct, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>");
    fb_buf_puts(&wb, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"" REL_NS "\">"
        "<bookViews><workbookView/></bookViews><sheets>");
    fb_buf_puts(&rels, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">");
    for (size_t i = 0; i < x->n; i++) {
        XSheet *s = &x->sheets[i];
        FbBuf part;
        fb_buf_init(&part);
        fb_buf_printf(&part, "xl/worksheets/sheet%zu.xml", i + 1);
        free(s->part);
        s->part = fb_buf_detach(&part);
        s->sheet_id = (int)i + 1;
        fb_buf_printf(&ct, "<Override PartName=\"/%s\" ContentType=\"" WS_CT "\"/>", s->part);
        fb_buf_puts(&wb, "<sheet name=\"");
        fb_xml_escape(&wb, s->name, strlen(s->name));
        fb_buf_printf(&wb, "\" sheetId=\"%d\" r:id=\"rId%zu\"/>", s->sheet_id, i + 1);
        fb_buf_printf(&rels, "<Relationship Id=\"rId%zu\" Type=\"" REL_NS "/worksheet\" Target=\"worksheets/sheet%zu.xml\"/>", i + 1, i + 1);
    }
    fb_buf_puts(&ct, "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/></Types>");
    fb_buf_puts(&wb, "</sheets><calcPr fullCalcOnLoad=\"1\"/></workbook>");
    fb_buf_printf(&rels, "<Relationship Id=\"rId%zu\" Type=\"" REL_NS "/styles\" Target=\"styles.xml\"/></Relationships>", x->n + 1);
    static const char root_rels[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"" REL_NS "/officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>";
    int rc = 0;
    rc |= fbzw_add_stored(w, "[Content_Types].xml", ct.data, ct.len);
    rc |= fbzw_add_stored(w, "_rels/.rels", root_rels, strlen(root_rels));
    rc |= fbzw_add_stored(w, "xl/workbook.xml", wb.data, wb.len);
    rc |= fbzw_add_stored(w, "xl/_rels/workbook.xml.rels", rels.data, rels.len);
    rc |= fbzw_add_stored(w, "xl/styles.xml", NEW_STYLES, strlen(NEW_STYLES));
    for (size_t i = 0; i < x->n; i++) {
        FbBuf sx;
        fb_buf_init(&sx);
        serialize_sheet(&x->sheets[i], &sx);
        rc |= fbzw_add_stored(w, x->sheets[i].part, sx.data, sx.len);
        fb_buf_free(&sx);
    }
    fb_buf_free(&ct); fb_buf_free(&wb); fb_buf_free(&rels);
    return rc;
}

static int save_existing_book(FbXEdit *x, FbZipWriter *w)
{
    /* register brand-new sheets in the workbook parts */
    for (size_t i = 0; i < x->n; i++) {
        XSheet *s = &x->sheets[i];
        if (!s->is_new || s->in_workbook) continue;
        if (!x->have_wb || !x->have_rels || !x->have_ct) return -1;
        int rid = next_rid(&x->rels_xml);
        FbBuf t;
        fb_buf_init(&t);
        /* declare the r: prefix on the element itself: some writers (openpyxl) declare it per <sheet>
           instead of on the root, so we cannot rely on the root declaration. Re-declaring is always legal. */
        fb_buf_puts(&t, "<sheet xmlns:r=\"" REL_NS "\" name=\"");
        fb_xml_escape(&t, s->name, strlen(s->name));
        fb_buf_printf(&t, "\" sheetId=\"%d\" r:id=\"rId%d\"/>", s->sheet_id, rid);
        if (insert_before(&x->wb_xml, "</sheets>", fb_buf_cstr(&t))) { fb_buf_free(&t); return -1; }
        fb_buf_clear(&t);
        char *wdir = dir_of(x->wb_part);
        const char *rel_target = s->part + strlen(wdir);
        fb_buf_printf(&t, "<Relationship Id=\"rId%d\" Type=\"" REL_NS "/worksheet\" Target=\"%s\"/>", rid, rel_target);
        free(wdir);
        if (insert_before(&x->rels_xml, "</Relationships>", fb_buf_cstr(&t))) { fb_buf_free(&t); return -1; }
        fb_buf_clear(&t);
        fb_buf_printf(&t, "<Override PartName=\"/%s\" ContentType=\"" WS_CT "\"/>", s->part);
        if (insert_before(&x->ct_xml, "</Types>", fb_buf_cstr(&t))) { fb_buf_free(&t); return -1; }
        fb_buf_free(&t);
        x->wb_mod = x->rels_mod = x->ct_mod = 1;
        s->in_workbook = 1;
    }
    if (x->formulas_written && x->have_wb) { ensure_calc_on_load(&x->wb_xml); x->wb_mod = 1; }
    /* a stale calcChain makes Excel "repair" the file once cells change */
    int drop_calc = fbzip_find(&x->zip, "xl/calcChain.xml") >= 0;
    if (drop_calc) {
        if (x->have_rels) { remove_tags_containing(&x->rels_xml, "<Relationship ", "calcChain"); x->rels_mod = 1; }
        if (x->have_ct) { remove_tags_containing(&x->ct_xml, "<Override ", "calcChain"); x->ct_mod = 1; }
    }
    int rc = 0;
    for (size_t i = 0; i < x->zip.n; i++) {
        const FbZipEntry *e = &x->zip.e[i];
        if (drop_calc && !strcmp(e->name, "xl/calcChain.xml")) continue;
        const FbBuf *repl = NULL;
        FbBuf sx;
        fb_buf_init(&sx);
        if (x->wb_mod && !strcmp(e->name, x->wb_part)) repl = &x->wb_xml;
        else if (x->rels_mod && x->rels_part && !strcmp(e->name, x->rels_part)) repl = &x->rels_xml;
        else if (x->ct_mod && !strcmp(e->name, "[Content_Types].xml")) repl = &x->ct_xml;
        else if (x->styles_mod && x->styles_part && !strcmp(e->name, x->styles_part)) repl = &x->styles_xml;
        else {
            for (size_t k = 0; k < x->n; k++)
                if (!x->sheets[k].is_new && x->sheets[k].dirty && !strcmp(x->sheets[k].part, e->name)) {
                    serialize_sheet(&x->sheets[k], &sx);
                    repl = &sx;
                    break;
                }
        }
        if (repl) rc |= fbzw_add_stored(w, e->name, repl->data ? repl->data : "", repl->len);
        else {
            const unsigned char *raw;
            size_t rl;
            if (fbzip_raw(&x->zip, i, &raw, &rl)) { fb_buf_free(&sx); return -1; }
            rc |= fbzw_add_raw(w, e->name, e->method, e->crc, e->csize, e->usize, raw);
        }
        fb_buf_free(&sx);
    }
    for (size_t k = 0; k < x->n; k++) {
        if (!x->sheets[k].is_new) continue;
        FbBuf sx;
        fb_buf_init(&sx);
        serialize_sheet(&x->sheets[k], &sx);
        rc |= fbzw_add_stored(w, x->sheets[k].part, sx.data, sx.len);
        fb_buf_free(&sx);
    }
    return rc;
}

int fbxe_save(FbXEdit *x, const char *path, char *err, size_t errsz)
{
    FbZipWriter w;
    fbzw_init(&w);
    int rc = x->is_new_book ? save_new_book(x, &w) : save_existing_book(x, &w);
    if (rc) { fbzw_free(&w); fb_set_err(err, errsz, "could not assemble the workbook"); return -1; }
    FbBuf out;
    fb_buf_init(&out);
    if (fbzw_finish(&w, &out)) { fb_buf_free(&out); fbzw_free(&w); fb_set_err(err, errsz, "out of memory"); return -1; }
    fb_ensure_parent_dir(path);
    if (fb_write_file(path, out.data, out.len)) {
        fb_set_err(err, errsz, "cannot write '%s' (is it open in Excel?)", path);
        fb_buf_free(&out);
        return -1;
    }
    fb_buf_free(&out);
    x->dirty = 0;
    return 0;
}

FbXEdit *fbxe_open(const char *path, int create_if_missing, char *err, size_t errsz)
{
    FbXEdit *x = (FbXEdit *)calloc(1, sizeof *x);
    if (!x) { fb_set_err(err, errsz, "out of memory"); return NULL; }
    fb_buf_init(&x->wb_xml); fb_buf_init(&x->rels_xml); fb_buf_init(&x->ct_xml); fb_buf_init(&x->styles_xml);
    x->bold_xf = -1;
    if (fb_file_exists(path)) {
        if (fbzip_open(&x->zip, path, err, errsz)) { free(x); return NULL; }
        x->has_zip = 1;
        if (load_book(x, err, errsz)) { fbxe_close(x); return NULL; }
    } else if (create_if_missing) {
        x->is_new_book = 1;
        x->bold_xf = 1;
    } else {
        fb_set_err(err, errsz, "file not found: %s", path);
        free(x);
        return NULL;
    }
    return x;
}

int fbx_read_sheet(const char *path, const char *sheet, FbGrid *out, char *err, size_t errsz)
{
    FbXEdit *x = fbxe_open(path, 0, err, errsz);
    if (!x) { fbgrid_init(out); return -1; }
    int rc = fbxe_get_grid(x, sheet, out, err, errsz);
    fbxe_close(x);
    return rc;
}
