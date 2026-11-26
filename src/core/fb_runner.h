/* FlowBrowser - automation runner. Platform independent: talks to a browser through FbBackend. */
#ifndef FB_RUNNER_H
#define FB_RUNNER_H

#include "fb_json.h"
#include "fb_util.h"

/* Everything the runner needs from a host (WebView2 on Windows, a CDP bridge in tests/CLI).
   All callbacks are invoked from the runner thread. Return 0 for success unless noted. */
typedef struct {
    void *ud;
    /* Chrome DevTools Protocol pass-through. result_json receives the "result" object as JSON. */
    int  (*cdp)(void *ud, const char *method, const char *params_json, FbBuf *result_json, char *err, size_t errsz);
    /* Navigate the current tab; wait!=0 blocks until the load finished. */
    int  (*navigate)(void *ud, const char *url, int wait, int timeout_ms, char *err, size_t errsz);
    /* Wait for "Navigation.started" | "Navigation.completed" | "Page.fileChooserOpened" | ...
       Returns 0 if the event occurred since the last time it was consumed, 1 on timeout, <0 on error.
       timeout_ms == 0 just consumes a stale event (drain). params_json may be NULL. */
    int  (*wait_event)(void *ud, const char *name, int timeout_ms, FbBuf *params_json);
    /* op: "new" (arg=url) | "switch" (arg=index or title) | "close" | "back" | "forward" | "reload" */
    int  (*tab)(void *ud, const char *op, const char *arg, char *err, size_t errsz);
    /* Ask the human. kind: "captcha" | "message" | "input". Returns 0 = continue/answered (answer filled),
       1 = timed out, 2 = cancelled. */
    int  (*ask)(void *ud, const char *kind, const char *text, const char *def, int timeout_ms, FbBuf *answer);
    /* Host options: "dialogs" = accept|dismiss|manual, "user_agent" = ... */
    int  (*option)(void *ud, const char *name, const char *value);
    /* Progress events as JSON text (flow_start, step_start, step_end, step_error, log, var, flow_end). */
    void (*emit)(void *ud, const char *event_json);
    /* Called before every step (debugging: pause / single-step / breakpoints). Non-zero aborts the run. */
    int  (*checkpoint)(void *ud, const char *path);
} FbBackend;

typedef struct FbRunner FbRunner;

FbRunner     *fbr_new(const FbBackend *be);
void          fbr_free(FbRunner *r);
/* base_dir: relative file paths resolve against it. flows_dir: where "run_flow" finds library flows. */
void          fbr_set_dirs(FbRunner *r, const char *base_dir, const char *flows_dir);
void          fbr_set_var(FbRunner *r, const char *name, FbJson *value);   /* takes ownership */
const FbJson *fbr_vars(const FbRunner *r);
void          fbr_stop(FbRunner *r);                                       /* thread-safe */
/* 0 = finished ok, 1 = failed (err has the message), 2 = stopped by the user */
int           fbr_run(FbRunner *r, const FbJson *flow, char *err, size_t errsz);
/* "1.2.then.0"-style path of the step that failed last */
const char   *fbr_error_path(const FbRunner *r);

/* ---- flow files / catalog / library (fb_flow.c) ---- */
int     fbf_set_catalog(const char *catalog_json, char *err, size_t errsz);
const FbJson *fbf_catalog(void);
FbJson *fbf_new(const char *name);
void    fbf_migrate(FbJson *flow);
/* Returns an array of {"path","level":"error"|"warn","message"} (empty array = valid). */
FbJson *fbf_validate(const FbJson *flow);

int     fbf_lib_list(const char *dir, FbJson **out);
int     fbf_lib_save(const char *dir, const char *name, const FbJson *flow, int keep_versions, char *err, size_t errsz);
int     fbf_lib_load(const char *dir, const char *name, FbJson **flow, char *err, size_t errsz);
int     fbf_lib_delete(const char *dir, const char *name);
void    fbf_safe_name(const char *in, FbBuf *out);
FbJson *fbf_backup_create(const char *dir, const FbJson *extra, char *err, size_t errsz);
/* mode: 0 skip existing, 1 overwrite existing, 2 keep both (renames incoming) */
int     fbf_backup_restore(const FbJson *backup, const char *dir, int mode, FbJson **report, char *err, size_t errsz);

#endif
