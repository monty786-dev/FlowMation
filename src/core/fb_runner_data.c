/* Runner steps: Excel / CSV / JSON / text files. Workbooks stay open for the whole run and are patched in place. */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "fb_runner_int.h"

#define NEED(var, call) const char *var = (call); if (!var) return R_ERR
#define FLUSH_EVERY_MS 1500

/* ============================================================ open workbooks */

static XlBook *find_book(FbRunner *r, const char *path)
{
    for (int i = 0; i < r->nbooks; i++) if (!fb_stricmp(r->books[i].path, path)) return &r->books[i];
    return NULL;
}

static int book_save(FbRunner *r, XlBook *b)
{
    char err[300];
    if (!b->pending) return R_OK;
    if (fbxe_save(b->x, b->path, err, sizeof err))
        return r_fail(r, "Cannot save '%s': %s. If the workbook is open in Excel, close it and run again", b->path, err);
    b->pending = 0;
    b->last_flush = fb_now_ms();
    return R_OK;
}

int r_flush_books(FbRunner *r, int force)
{
    int rc = R_OK;
    uint64_t now = fb_now_ms();
    for (int i = 0; i < r->nbooks; i++) {
        XlBook *b = &r->books[i];
        if (!b->pending || (!force && now - b->last_flush < FLUSH_EVERY_MS)) continue;
        int e = book_save(r, b);
        if (e) rc = e;
    }
    return rc;
}

void r_close_books(FbRunner *r)
{
    for (int i = 0; i < r->nbooks; i++) { fbxe_close(r->books[i].x); free(r->books[i].path); }
    r->nbooks = 0;
}

static XlBook *get_book(FbRunner *r, const char *path, int create)
{
    XlBook *b = find_book(r, path);
    if (b) return b;
    if (r->nbooks >= 16) { r_fail(r, "Too many Excel files are open in one flow (limit 16)"); return NULL; }
    if (!create && !fb_file_exists(path)) { r_fail(r, "Excel file not found: %s", path); return NULL; }
    char err[300];
    if (create) fb_ensure_parent_dir(path);
    FbXEdit *x = fbxe_open(path, create, err, sizeof err);
    if (!x) { r_fail(r, "Cannot open '%s': %s", path, err); return NULL; }
    b = &r->books[r->nbooks++];
    b->path = fb_strdup(path);
    b->x = x;
    b->pending = 0;
    b->last_flush = fb_now_ms();
    return b;
}

static int touched(FbRunner *r, XlBook *b)
{
    b->pending = 1;
    return r_flush_books(r, 0);
}

/* ============================================================ values -> cells */

/* text of a JSON value as written to a cell / CSV field; *is_num set for numbers */
static void cell_text(const FbJson *v, FbBuf *out, int *is_num)
{
    *is_num = 0;
    if (!v || v->type == FBJ_NULL) return;
    if (v->type == FBJ_NUM) { fbe_format_number(v->num, out); *is_num = 1; return; }
    if (v->type == FBJ_BOOL) { fb_buf_puts(out, v->b ? "TRUE" : "FALSE"); return; }
    if (v->type == FBJ_STR) { fb_buf_puts(out, v->str); return; }
    char *j = fbj_to_string(v, 0);                                  /* nested values become JSON text */
    if (j) { fb_buf_puts(out, j); free(j); }
}

/* column names of a table: union of object keys in order of first appearance, "_row" style keys hidden */
static void table_columns(const FbJson *t, FbJson *cols)
{
    for (size_t i = 0; i < t->count; i++) {
        const FbJson *row = t->items[i];
        if (row->type != FBJ_OBJ) continue;
        for (size_t k = 0; k < row->count; k++) {
            if (row->keys[k][0] == '_') continue;
            int seen = 0;
            for (size_t c = 0; c < cols->count && !seen; c++) seen = !strcmp(cols->items[c]->str, row->keys[k]);
            if (!seen) fbj_arr_push(cols, fbj_str(row->keys[k]));
        }
    }
}

/* normalises any "table-ish" value to an array of rows; *owned must be freed by the caller */
static const FbJson *as_table(const FbJson *v, FbJson **owned)
{
    *owned = NULL;
    if (!v) return NULL;
    if (v->type == FBJ_ARR) return v;
    FbJson *a = fbj_arr();
    fbj_arr_push(a, fbj_clone(v));                                  /* a single row (object) or value */
    *owned = a;
    return a;
}

/* ============================================================ excel */

static void crop(const FbGrid *g, int r1, int c1, int r2, int c2, FbGrid *out)
{
    fbgrid_init(out);
    if (r1 < 1) r1 = 1;
    if (c1 < 1) c1 = 1;
    size_t ncols = fbgrid_ncols(g);
    if (r2 <= 0 || (size_t)r2 > g->nrows) r2 = (int)g->nrows;
    if (c2 <= 0 || (size_t)c2 > ncols) c2 = (int)ncols;
    if (r2 < r1 || c2 < c1) return;
    out->rows = (FbRow *)calloc((size_t)(r2 - r1 + 1), sizeof(FbRow));
    if (!out->rows) return;
    out->nrows = (size_t)(r2 - r1 + 1);
    for (int r = r1; r <= r2; r++) {
        FbRow *d = &out->rows[r - r1];
        d->c = (char **)calloc((size_t)(c2 - c1 + 1), sizeof(char *));
        if (!d->c) { out->nrows = (size_t)(r - r1); return; }
        d->n = (size_t)(c2 - c1 + 1);
        for (int c = c1; c <= c2; c++) {
            const char *v = fbgrid_get(g, (size_t)r - 1, (size_t)c - 1);
            d->c[c - c1] = v ? fb_strdup(v) : NULL;
        }
    }
}

static int s_excel_read(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(sheet, r_str(r, s, "sheet", ""));
    NEED(range, r_str(r, s, "range", ""));
    NEED(rvar, r_str(r, s, "result_var", "rows"));
    int header = r_bool(r, s, "header", 1), skip = r_bool(r, s, "skip_empty", 1);
    if (!*file) return r_fail(r, "No Excel file selected");
    XlBook *b = get_book(r, r_abs_path(r, file), 0);
    if (!b) return R_ERR;
    FbGrid g;
    char err[300];
    if (fbxe_get_grid(b->x, *sheet ? sheet : NULL, &g, err, sizeof err)) {
        FbJson *names = fbxe_sheet_names(b->x);
        FbBuf list;
        fb_buf_init(&list);
        for (size_t i = 0; names && i < names->count; i++) { if (i) fb_buf_puts(&list, ", "); fb_buf_puts(&list, names->items[i]->str); }
        r_fail(r, "%s (sheets in this file: %s)", err, fb_buf_cstr(&list));
        fb_buf_free(&list);
        fbj_free(names);
        return R_ERR;
    }
    int r1 = 1, c1 = 1, r2 = 0, c2 = 0;
    FbGrid view;
    if (*range) {
        if (fbx_parse_range(range, &r1, &c1, &r2, &c2)) { fbgrid_free(&g); return r_fail(r, "'%s' is not a valid range (example: A1:D20)", range); }
        crop(&g, r1, c1, r2, c2, &view);
    } else view = g;
    FbJson *rows = fbgrid_to_json(&view, 0, header, 1, skip);
    if (*range) {
        fbgrid_free(&view);
        if (header) for (size_t i = 0; rows && i < rows->count; i++) {       /* keep _row = real sheet row */
            FbJson *rn = fbj_get(rows->items[i], "_row");
            if (rn) rn->num += (r1 < 1 ? 1 : r1) - 1;
        }
    }
    fbgrid_free(&g);
    if (!rows) return r_fail(r, "Out of memory");
    return r_set_var(r, rvar, rows);
}

static int s_excel_write_cell(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(sheet, r_str(r, s, "sheet", ""));
    NEED(cell, r_str(r, s, "cell", ""));
    NEED(value, r_str(r, s, "value", ""));
    NEED(type, r_str(r, s, "value_type", "auto"));
    int row, col;
    if (!*file) return r_fail(r, "No Excel file selected");
    if (fbx_parse_ref(cell, &row, &col)) return r_fail(r, "'%s' is not a cell address (example: B5)", cell);
    /* a value that came from a variable is data, never a formula; only literal text typed in the step may be one */
    const char *raw = fbj_get_str(s, "value", "");
    if (!strcmp(type, "auto") && strstr(raw, "{{") && value[0] == '=') type = "text";
    XlBook *b = get_book(r, r_abs_path(r, file), 1);
    if (!b) return R_ERR;
    char err[300];
    if (fbxe_set(b->x, *sheet ? sheet : NULL, row, col, value, type, 0, err, sizeof err)) return r_fail(r, "%s", err);
    return touched(r, b);
}

static int s_excel_append_row(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(sheet, r_str(r, s, "sheet", ""));
    NEED(scol, r_str(r, s, "start_col", "A"));
    if (!*file) return r_fail(r, "No Excel file selected");
    int c0 = fbx_col_index(scol);
    if (!c0) return r_fail(r, "'%s' is not a column letter", scol);

    /* the row: from a variable, or from the list of values typed in the step */
    FbJson *row = fbj_arr();
    const char *src = fbj_get_str(s, "source", "");
    if (*src) {
        FbJson *owned;
        const FbJson *v = r_lookup(r, src, &owned);
        if (!v) { fbj_free(row); return r_fail(r, "'%s' is not a variable", src); }
        if (v->type == FBJ_OBJ) { for (size_t i = 0; i < v->count; i++) if (v->keys[i][0] != '_') fbj_arr_push(row, fbj_clone(v->items[i])); }
        else if (v->type == FBJ_ARR) { for (size_t i = 0; i < v->count; i++) fbj_arr_push(row, fbj_clone(v->items[i])); }
        else fbj_arr_push(row, fbj_clone(v));
        fbj_free(owned);
    } else {
        const FbJson *vals = fbj_get(s, "values");
        for (size_t i = 0; vals && vals->type == FBJ_ARR && i < vals->count; i++) {
            FbJson *e = vals->items[i]->type == FBJ_STR ? fbe_expand_value(r->vars, vals->items[i]->str, r->err, sizeof r->err)
                                                        : fbj_clone(vals->items[i]);
            if (!e) { fbj_free(row); return r_fail(r, "Value %zu: %s", i + 1, r->err); }
            fbj_arr_push(row, e);
        }
    }
    if (!row->count) { fbj_free(row); return r_fail(r, "Nothing to write: give values or a source variable"); }

    XlBook *b = get_book(r, r_abs_path(r, file), 1);
    if (!b) { fbj_free(row); return R_ERR; }
    int rn = fbxe_last_row(b->x, *sheet ? sheet : NULL) + 1;
    char err[300];
    for (size_t i = 0; i < row->count; i++) {
        FbBuf t;
        int isnum;
        fb_buf_init(&t);
        cell_text(row->items[i], &t, &isnum);
        int e = fbxe_set(b->x, *sheet ? sheet : NULL, rn, c0 + (int)i, fb_buf_cstr(&t), "number", 0, err, sizeof err);
        fb_buf_free(&t);
        if (e) { fbj_free(row); return r_fail(r, "%s", err); }
    }
    fbj_free(row);
    return touched(r, b);
}

static int s_excel_write_table(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(sheet, r_str(r, s, "sheet", ""));
    NEED(src, r_str(r, s, "source", ""));
    NEED(start, r_str(r, s, "start_cell", "A1"));
    NEED(mode, r_str(r, s, "mode", "overwrite"));
    int headers = r_bool(r, s, "headers", 1);
    if (!*file) return r_fail(r, "No Excel file selected");
    int row0, col0;
    if (fbx_parse_ref(start, &row0, &col0)) return r_fail(r, "'%s' is not a cell address (example: A1)", start);
    FbJson *owned0;
    const FbJson *v = r_lookup(r, src, &owned0);
    if (!v) return r_fail(r, "'%s' is not a variable", src);
    FbJson *owned;
    const FbJson *t = as_table(v, &owned);
    FbJson *cols = fbj_arr();
    table_columns(t, cols);

    XlBook *b = get_book(r, r_abs_path(r, file), 1);
    if (!b) { fbj_free(cols); fbj_free(owned); fbj_free(owned0); return R_ERR; }
    const char *sh = *sheet ? sheet : NULL;
    char err[300];
    int rc = R_OK;
    int append = !strcmp(mode, "append");
    int last = append ? fbxe_last_row(b->x, sh) : 0;
    int rn = append && last >= row0 ? last + 1 : row0;
    int write_header = headers && cols->count && (!append || last < row0);

    if (write_header) {
        for (size_t c = 0; c < cols->count; c++)
            if (fbxe_set(b->x, sh, rn, col0 + (int)c, cols->items[c]->str, "text", 1, err, sizeof err)) { rc = r_fail(r, "%s", err); goto done; }
        rn++;
    }
    for (size_t i = 0; i < t->count && rc == R_OK; i++, rn++) {
        const FbJson *row = t->items[i];
        if (cols->count) {                                          /* table of objects: align by column name */
            for (size_t c = 0; c < cols->count; c++) {
                const FbJson *cv = row->type == FBJ_OBJ ? fbj_get(row, cols->items[c]->str) : NULL;
                FbBuf tx;
                int isnum;
                fb_buf_init(&tx);
                cell_text(cv, &tx, &isnum);
                int e = fbxe_set(b->x, sh, rn, col0 + (int)c, fb_buf_cstr(&tx), "number", 0, err, sizeof err);
                fb_buf_free(&tx);
                if (e) { rc = r_fail(r, "%s", err); break; }
            }
        } else if (row->type == FBJ_ARR) {                          /* table of arrays */
            for (size_t c = 0; c < row->count; c++) {
                FbBuf tx;
                int isnum;
                fb_buf_init(&tx);
                cell_text(row->items[c], &tx, &isnum);
                int e = fbxe_set(b->x, sh, rn, col0 + (int)c, fb_buf_cstr(&tx), "number", 0, err, sizeof err);
                fb_buf_free(&tx);
                if (e) { rc = r_fail(r, "%s", err); break; }
            }
        } else {                                                    /* plain list: one value per row */
            FbBuf tx;
            int isnum;
            fb_buf_init(&tx);
            cell_text(row, &tx, &isnum);
            int e = fbxe_set(b->x, sh, rn, col0, fb_buf_cstr(&tx), "number", 0, err, sizeof err);
            fb_buf_free(&tx);
            if (e) rc = r_fail(r, "%s", err);
        }
    }
done:
    fbj_free(cols);
    fbj_free(owned);
    fbj_free(owned0);
    if (rc != R_OK) return rc;
    return touched(r, b);
}

static int s_excel_save(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    if (*file) {
        XlBook *b = find_book(r, r_abs_path(r, file));
        return b ? book_save(r, b) : R_OK;
    }
    return r_flush_books(r, 1);
}

/* ============================================================ csv / json / text */

static const char *delim_of(FbRunner *r, const FbJson *s, char *out)
{
    const char *d = r_str(r, s, "delimiter", ",");
    if (!d) return NULL;
    if (!strcmp(d, "\\t") || !fb_stricmp(d, "tab")) *out = '\t';
    else if (!fb_stricmp(d, "semicolon")) *out = ';';
    else if (!fb_stricmp(d, "pipe")) *out = '|';
    else *out = *d ? *d : ',';
    return d;
}

/* CSV injection guard: a field that a spreadsheet would run as a formula gets a leading apostrophe */
static void csv_field(FbBuf *out, const char *v, char delim, int protect)
{
    FbBuf tmp;
    fb_buf_init(&tmp);
    double d;
    int risky = v[0] == '=' || v[0] == '@' || v[0] == '\t' || v[0] == '\r' ||
                ((v[0] == '+' || v[0] == '-') && v[1] && !fbe_parse_number(v, &d));
    if (protect && risky) fb_buf_putc(&tmp, '\'');
    fb_buf_puts(&tmp, v);
    fbcsv_write_field(out, fb_buf_cstr(&tmp), delim);
    fb_buf_free(&tmp);
}

static int s_csv_read(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(rvar, r_str(r, s, "result_var", "rows"));
    const char *abs = r_abs_path(r, file);
    FbBuf b;
    fb_buf_init(&b);
    if (fb_read_file(abs, &b)) { fb_buf_free(&b); return r_fail(r, "File not found or not readable: %s", abs); }
    FbGrid g;
    if (fbcsv_parse(fb_buf_cstr(&b), b.len, &g)) { fb_buf_free(&b); return r_fail(r, "Cannot parse CSV '%s'", abs); }
    fb_buf_free(&b);
    FbJson *rows = fbgrid_to_json(&g, 0, r_bool(r, s, "header", 1), 1, 1);
    fbgrid_free(&g);
    return r_set_var(r, rvar, rows);
}

static int s_csv_write(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(src, r_str(r, s, "source", ""));
    char delim;
    if (!delim_of(r, s, &delim)) return R_ERR;
    int headers = r_bool(r, s, "headers", 1), append = r_bool(r, s, "append", 0), protect = r_bool(r, s, "protect_formulas", 1);
    const char *abs = r_abs_path(r, file);
    FbJson *o0;
    const FbJson *v = r_lookup(r, src, &o0);
    if (!v) return r_fail(r, "'%s' is not a variable", src);
    FbJson *owned;
    const FbJson *t = as_table(v, &owned);
    FbJson *cols = fbj_arr();
    table_columns(t, cols);
    FbBuf out;
    fb_buf_init(&out);
    int exists = fb_file_exists(abs);
    if (!append || !exists) fb_buf_puts(&out, "\xEF\xBB\xBF");               /* BOM: Excel then reads UTF-8 correctly */
    if (headers && cols->count && (!append || !exists)) {
        for (size_t c = 0; c < cols->count; c++) { if (c) fb_buf_putc(&out, delim); csv_field(&out, cols->items[c]->str, delim, protect); }
        fb_buf_puts(&out, "\r\n");
    }
    for (size_t i = 0; i < t->count; i++) {
        const FbJson *row = t->items[i];
        size_t n = cols->count ? cols->count : (row->type == FBJ_ARR ? row->count : 1);
        for (size_t c = 0; c < n; c++) {
            const FbJson *cv = cols->count ? (row->type == FBJ_OBJ ? fbj_get(row, cols->items[c]->str) : NULL)
                                           : (row->type == FBJ_ARR ? row->items[c] : row);
            FbBuf tx;
            int isnum;
            fb_buf_init(&tx);
            cell_text(cv, &tx, &isnum);
            if (c) fb_buf_putc(&out, delim);
            csv_field(&out, fb_buf_cstr(&tx), delim, protect && !isnum);
            fb_buf_free(&tx);
        }
        fb_buf_puts(&out, "\r\n");
    }
    int rc = R_OK;
    fb_ensure_parent_dir(abs);
    if (append && exists) {
        FILE *f = fb_fopen(abs, "ab");
        if (!f || fwrite(out.data, 1, out.len, f) != out.len) rc = r_fail(r, "Cannot write '%s'", abs);
        if (f) fclose(f);
    } else if (fb_write_file(abs, out.data, out.len)) rc = r_fail(r, "Cannot write '%s' (is it open in another program?)", abs);
    fb_buf_free(&out);
    fbj_free(cols);
    fbj_free(owned);
    fbj_free(o0);
    return rc;
}

static int s_json_write(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(src, r_str(r, s, "source", ""));
    FbJson *owned;
    const FbJson *v = r_lookup(r, src, &owned);
    if (!v) return r_fail(r, "'%s' is not a variable", src);
    char *txt = fbj_to_string(v, 1);
    const char *abs = r_abs_path(r, file);
    fb_ensure_parent_dir(abs);
    int rc = (!txt || fb_write_file(abs, txt, strlen(txt))) ? r_fail(r, "Cannot write '%s'", abs) : R_OK;
    free(txt);
    fbj_free(owned);
    return rc;
}

static int s_text_write(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(text, r_str(r, s, "text", ""));
    const char *abs = r_abs_path(r, file);
    fb_ensure_parent_dir(abs);
    size_t n = strlen(text);
    int nl = n == 0 || text[n - 1] != '\n';
    FILE *f = fb_fopen(abs, r_bool(r, s, "append", 1) ? "ab" : "wb");
    if (!f) return r_fail(r, "Cannot write '%s'", abs);
    int bad = fwrite(text, 1, n, f) != n || (nl && fputc('\n', f) == EOF);
    fclose(f);
    return bad ? r_fail(r, "Cannot write '%s'", abs) : R_OK;
}

static int s_text_read(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(rvar, r_str(r, s, "result_var", ""));
    const char *abs = r_abs_path(r, file);
    FbBuf b;
    fb_buf_init(&b);
    if (fb_read_file(abs, &b)) { fb_buf_free(&b); return r_fail(r, "File not found or not readable: %s", abs); }
    const char *p = fb_buf_cstr(&b);
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    FbJson *v = fbj_str(p);
    fb_buf_free(&b);
    return r_set_var(r, rvar, v);
}

static int s_file_exists(FbRunner *r, const FbJson *s, const char *path)
{
    (void)path;
    NEED(file, r_str(r, s, "file", ""));
    NEED(rvar, r_str(r, s, "result_var", ""));
    return r_set_var(r, rvar, fbj_bool(fb_file_exists(r_abs_path(r, file))));
}

const StepDef DATA_STEPS[] = {
    { "excel_read", s_excel_read, SF_QUICK },
    { "excel_write_cell", s_excel_write_cell, SF_QUICK },
    { "excel_append_row", s_excel_append_row, SF_QUICK },
    { "excel_write_table", s_excel_write_table, SF_QUICK },
    { "excel_save", s_excel_save, SF_QUICK },
    { "csv_read", s_csv_read, SF_QUICK },
    { "csv_write", s_csv_write, SF_QUICK },
    { "json_write", s_json_write, SF_QUICK },
    { "text_write", s_text_write, SF_QUICK },
    { "text_read", s_text_read, SF_QUICK },
    { "file_exists", s_file_exists, SF_QUICK },
    { NULL, NULL, 0 }
};
