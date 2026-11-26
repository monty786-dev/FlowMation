/* FlowBrowser - small JSON DOM (objects keep insertion order). */
#ifndef FB_JSON_H
#define FB_JSON_H

#include "fb_util.h"

typedef enum { FBJ_NULL, FBJ_BOOL, FBJ_NUM, FBJ_STR, FBJ_ARR, FBJ_OBJ } FbJType;

typedef struct FbJson FbJson;
struct FbJson {
    FbJType  type;
    double   num;
    int      b;
    char    *str;
    FbJson **items;   /* array elements / object values */
    char   **keys;    /* object keys (parallel to items) */
    size_t   count, cap;
};

/* Parsing: text must be NUL terminated at text[len] (FbBuf is). Lenient: BOM, trailing commas. */
FbJson *fbj_parse(const char *text, size_t len, char *err, size_t errsz);
FbJson *fbj_parse_str(const char *text, char *err, size_t errsz);
void    fbj_free(FbJson *j);
FbJson *fbj_clone(const FbJson *j);

FbJson *fbj_null(void);
FbJson *fbj_bool(int b);
FbJson *fbj_num(double d);
FbJson *fbj_str(const char *s);
FbJson *fbj_strn(const char *s, size_t n);
FbJson *fbj_arr(void);
FbJson *fbj_obj(void);

/* Containers take ownership of val. Return 0 on success. */
int     fbj_arr_push(FbJson *arr, FbJson *val);
int     fbj_obj_set(FbJson *obj, const char *key, FbJson *val);   /* replaces existing key */
int     fbj_obj_del(FbJson *obj, const char *key);
int     fbj_arr_del(FbJson *arr, size_t idx);

FbJson *fbj_get(const FbJson *obj, const char *key);              /* NULL if absent */
FbJson *fbj_at(const FbJson *arr, size_t i);
size_t  fbj_len(const FbJson *j);                                  /* array/object count, else 0 */
int     fbj_is(const FbJson *j, FbJType t);

const char *fbj_get_str(const FbJson *obj, const char *key, const char *def);
double      fbj_get_num(const FbJson *obj, const char *key, double def);
int         fbj_get_bool(const FbJson *obj, const char *key, int def);

/* Output */
void    fbj_write(const FbJson *j, FbBuf *out, int pretty);
char   *fbj_to_string(const FbJson *j, int pretty);               /* caller frees */
/* "text" form used by templates: strings raw, numbers plain, null empty, containers as JSON */
void    fbj_to_text(const FbJson *j, FbBuf *out);
void    fbj_write_escaped_string(FbBuf *out, const char *s, size_t n);

#endif
