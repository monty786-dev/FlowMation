/* FlowBrowser - Excel (.xlsx) / CSV reading, in-place patching and writing. */
#ifndef FB_XLSX_H
#define FB_XLSX_H

#include "fb_json.h"
#include "fb_util.h"

/* ---- cell reference helpers (rows/cols are 1-based) ---- */
int  fbx_parse_ref(const char *ref, int *row, int *col);              /* "B5" -> row 5 col 2, 0 ok */
void fbx_col_name(int col, char out[8]);                              /* 1 -> "A" */
int  fbx_col_index(const char *letters);                              /* "AB" -> 28, 0 if invalid */
/* "A1:D9", "B5", "A:C" -> 1-based bounds; unspecified end = 0 (meaning "to the end"). 0 ok */
int  fbx_parse_range(const char *s, int *r1, int *c1, int *r2, int *c2);

/* ---- grid of strings (what reading produces) ---- */
typedef struct { char **c; size_t n; } FbRow;
typedef struct { FbRow *rows; size_t nrows; } FbGrid;

void        fbgrid_init(FbGrid *g);
void        fbgrid_free(FbGrid *g);
const char *fbgrid_get(const FbGrid *g, size_t row0, size_t col0);    /* NULL when empty/out of range */
size_t      fbgrid_ncols(const FbGrid *g);
/* Convert to an array of objects (header=1) or array of arrays (header=0).
   Object rows get "_row" (the 1-based source row number) when include_row_no. */
FbJson     *fbgrid_to_json(const FbGrid *g, size_t first_row0, int header, int include_row_no, int skip_empty_rows);

/* ---- CSV ---- */
int   fbcsv_parse(const char *text, size_t len, FbGrid *out);         /* auto-detects , ; tab | */
void  fbcsv_write_field(FbBuf *out, const char *s, char delim);

/* ---- workbook editing session ---- */
typedef struct FbXEdit FbXEdit;

FbXEdit *fbxe_open(const char *path, int create_if_missing, char *err, size_t errsz);
void     fbxe_close(FbXEdit *x);
/* type: NULL/"auto" | "text" | "number" | "formula". Rows/cols 1-based. bold only if the book allows it. */
int      fbxe_set(FbXEdit *x, const char *sheet, int row, int col, const char *value,
                  const char *type, int bold, char *err, size_t errsz);
int      fbxe_last_row(FbXEdit *x, const char *sheet);                /* 0 if empty or sheet missing */
int      fbxe_get_grid(FbXEdit *x, const char *sheet, FbGrid *out, char *err, size_t errsz);
FbJson  *fbxe_sheet_names(FbXEdit *x);                                /* JSON array of strings */
int      fbxe_dirty(const FbXEdit *x);
int      fbxe_save(FbXEdit *x, const char *path, char *err, size_t errsz);

/* convenience */
int      fbx_read_sheet(const char *path, const char *sheet, FbGrid *out, char *err, size_t errsz);

#endif
