/* FlowBrowser - variable paths, {{templates}} with filters, conditions and arithmetic. */
#ifndef FB_EXPR_H
#define FB_EXPR_H

#include <time.h>

#include "fb_json.h"

/* "row.First Name", "items[0].title", rows[2]["Full Name"], list.length. Borrowed result or NULL. */
const FbJson *fbe_path_get(const FbJson *vars, const char *path);
/* Set (creating intermediate objects) a dotted path like "a.b.c". Takes ownership of value. */
int           fbe_path_set(FbJson *vars, const char *path, FbJson *value);

/* Expand every {{ expr | filter:arg }} in tpl. Unknown variables expand to "". 0 ok, -1 error (err). */
int     fbe_expand(const FbJson *vars, const char *tpl, FbBuf *out, char *err, size_t errsz);
/* Like fbe_expand but when tpl is exactly one {{...}} the typed value (array/object/number) is returned. */
FbJson *fbe_expand_value(const FbJson *vars, const char *tpl, char *err, size_t errsz);

/* Conditions: "{{a}} == 5 and {{b}} contains \"x\" or {{c}} is empty". 1 true, 0 false, -1 error. */
int     fbe_eval_cond(const FbJson *vars, const char *cond, char *err, size_t errsz);

/* Arithmetic: + - * / % ( ) on plain numbers (expand templates first). */
int     fbe_calc(const char *expr, double *out, char *err, size_t errsz);

int     fbe_parse_number(const char *s, double *out);          /* strict: whole string must be a number */
void    fbe_format_number(double d, FbBuf *out);
void    fbe_format_time(const struct tm *tm, const char *fmt, FbBuf *out);   /* YYYY MM DD HH mm ss MMM ddd ... */
int     fbe_truthy(const char *s);

#endif
