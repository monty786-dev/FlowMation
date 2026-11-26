/* Internal declarations shared by the runner's source files. Not part of the public API. */
#ifndef FB_RUNNER_INT_H
#define FB_RUNNER_INT_H

#include "fb_expr.h"
#include "fb_runner.h"
#include "fb_xlsx.h"

/* step results */
enum { R_OK = 0, R_ERR = 1, R_STOP = 2, R_BREAK = 3, R_CONT = 4, R_END = 5 };

/* StepDef.flags */
#define SF_CAPTCHA 1   /* look for a CAPTCHA after this step succeeds */
#define SF_QUICK   2   /* no "slow down" delay after this step (control flow, variables) */

typedef struct {
    char   *path;           /* absolute workbook path */
    FbXEdit *x;
    int     pending;        /* unsaved edits */
    uint64_t last_flush;
} XlBook;

struct FbRunner {
    FbBackend be;
    FbJson   *vars;
    volatile int stop;
    char      err[1024];
    char      err_path[160];
    char    **scr;                   /* per-step scratch strings */
    size_t    nscr, cscr;
    char      base_dir[512], flows_dir[512];
    int       timeout_ms, step_delay_ms;
    char      on_error[16];          /* stop | continue */
    char      captcha[16];           /* off | pause | stop */
    int       depth;                 /* run_flow nesting */
    int       in_try;                /* >0 while inside a try body */
    uint64_t  last_var_emit;         /* throttles var events from loops */
    XlBook    books[16];
    int       nbooks;
};

typedef int (*StepFn)(FbRunner *r, const FbJson *step, const char *path);
typedef struct { const char *type; StepFn fn; int flags; } StepDef;

extern const StepDef CORE_STEPS[];
extern const StepDef WEB_STEPS[];
extern const StepDef DATA_STEPS[];

/* ---- errors / parameters (fb_runner.c) ----
   Parameter getters expand {{templates}}. r_str returns NULL ONLY when expansion failed (r->err is set),
   so callers must always pass a non-NULL default and treat NULL as failure. */
int         r_fail(FbRunner *r, const char *fmt, ...);
const char *r_keep(FbRunner *r, char *owned);
const char *r_str(FbRunner *r, const FbJson *s, const char *key, const char *def);
long        r_num(FbRunner *r, const FbJson *s, const char *key, long def);
int         r_bool(FbRunner *r, const FbJson *s, const char *key, int def);
int         r_timeout(FbRunner *r, const FbJson *s);
const char *r_abs_path(FbRunner *r, const char *p);

/* ---- variables / events / flow ---- */
int  r_set_var(FbRunner *r, const char *name, FbJson *value);      /* takes ownership; emits a "var" event */
void r_emit_var(FbRunner *r, const char *name);                    /* emit the current value of a variable */
void r_emit_vars(FbRunner *r);                                     /* emit the whole variable table */
/* A variable by name/path, or a {{template}} yielding a value. Borrowed result; *owned is set when the result must be freed. */
const FbJson *r_lookup(FbRunner *r, const char *src, FbJson **owned);
FbJson *r_value_param(FbRunner *r, const FbJson *s, const char *key, const char *type);   /* typed, owned; NULL on error */
void r_log(FbRunner *r, const char *level, const char *fmt, ...);
void r_emit(FbRunner *r, FbJson *event);                           /* takes ownership */
int  r_sleep(FbRunner *r, int ms);                                 /* non-zero when stopped meanwhile */
int  r_run_steps(FbRunner *r, const FbJson *steps, const char *prefix);
int  r_nested(FbRunner *r, const FbJson *step, const char *key, const char *path);

/* ---- web (fb_runner_web.c) ---- */
int  r_captcha_guard(FbRunner *r, const char *action_override);

/* ---- data (fb_runner_data.c) ---- */
int  r_flush_books(FbRunner *r, int force);
void r_close_books(FbRunner *r);

#endif
