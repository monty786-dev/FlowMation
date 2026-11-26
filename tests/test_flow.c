#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/core/fb_runner.h"

static int fails, total;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int count_files(const char *dir, const char *suffix)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "ls '%s' 2>/dev/null | grep -c '%s$'", dir, suffix);
    FILE *p = popen(cmd, "r");
    int n = 0;
    if (p) { if (fscanf(p, "%d", &n) != 1) n = 0; pclose(p); }
    return n;
}

static int has_issue(const FbJson *issues, const char *path, const char *level, const char *needle)
{
    for (size_t i = 0; i < fbj_len(issues); i++) {
        const FbJson *it = issues->items[i];
        if (!strcmp(fbj_get_str(it, "path", ""), path) && !strcmp(fbj_get_str(it, "level", ""), level) && strstr(fbj_get_str(it, "message", ""), needle)) return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *catalog = argc > 1 ? argv[1] : "../src/core/catalog.json";
    char err[300];
    FbBuf b;
    fb_buf_init(&b);
    CHECK(fb_read_file(catalog, &b) == 0);
    CHECK(fbf_set_catalog(fb_buf_cstr(&b), err, sizeof err) == 0);
    fb_buf_free(&b);

    /* ---- new + validate ---- */
    FbJson *flow = fbf_new("My flow");
    CHECK(!strcmp(fbj_get_str(flow, "name", ""), "My flow"));
    CHECK(!strcmp(fbj_get_str(flow, "format", ""), "flowbrowser-flow"));
    FbJson *steps = fbj_get(flow, "steps");
    CHECK(steps && steps->type == FBJ_ARR);
    FbJson *v = fbf_validate(flow);
    CHECK(fbj_len(v) == 0);
    fbj_free(v);

    const char *bad = "{\"format\":\"flowbrowser-flow\",\"version\":1,\"name\":\"bad\",\"steps\":["
        "{\"type\":\"navigate\"},"                                                          /* 0: url missing */
        "{\"type\":\"warp_drive\"},"                                                        /* 1: unknown type */
        "{\"type\":\"if\",\"condition\":\"1 == 1\",\"then\":[{\"type\":\"click\"}]},"        /* 2.then.0: selector missing */
        "{\"type\":\"click\",\"selector\":\"css:a\",\"button\":\"purple\"},"                 /* 3: enum warning */
        "{\"type\":\"foreach\",\"source\":\"rows\",\"body\":[{\"type\":\"try\",\"body\":[{\"type\":\"fill\",\"selector\":\"x\"}]}]}" /* 4.body.0.body.0: value missing */
        "]}";
    FbJson *bf = fbj_parse_str(bad, err, sizeof err);
    CHECK(bf != NULL);
    v = fbf_validate(bf);
    CHECK(has_issue(v, "0", "error", "URL"));
    CHECK(has_issue(v, "1", "error", "warp_drive"));
    CHECK(has_issue(v, "2.then.0", "error", "Element"));
    CHECK(has_issue(v, "3", "warn", "purple"));
    CHECK(has_issue(v, "4.body.0.body.0", "error", "Value"));
    CHECK(!has_issue(v, "3", "error", ""));
    fbj_free(v);
    fbj_free(bf);

    /* a template instead of a required value counts as filled */
    FbJson *ok = fbj_parse_str("{\"format\":\"flowbrowser-flow\",\"version\":1,\"name\":\"ok\",\"steps\":[{\"type\":\"navigate\",\"url\":\"{{base}}/x\"},{\"type\":\"if\",\"condition\":\"{{a}} == 1\"}]}", err, sizeof err);
    v = fbf_validate(ok);
    int errs = 0;
    for (size_t i = 0; i < fbj_len(v); i++) if (!strcmp(fbj_get_str(v->items[i], "level", ""), "error")) errs++;
    CHECK(errs == 0);
    fbj_free(v);
    fbj_free(ok);

    /* ---- library ---- */
    char dir[] = "/tmp/fbflowlib_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char lib[400];
    snprintf(lib, sizeof lib, "%s/flows", dir);
    FbJson *list = NULL;
    CHECK(fbf_lib_list(lib, &list) == 0 && fbj_len(list) == 0);   /* a missing folder is an empty library */
    fbj_free(list);

    fbj_obj_set(flow, "description", fbj_str("hello"));
    fbj_arr_push(steps, fbj_parse_str("{\"type\":\"navigate\",\"url\":\"https://example.org\"}", err, sizeof err));
    for (int i = 0; i < 6; i++) {
        char nm[40];
        snprintf(nm, sizeof nm, "v%d", i);
        fbj_obj_set(flow, "name", fbj_str(nm));
        CHECK(fbf_lib_save(lib, "Weekly report: 日本/2025?", flow, 3, err, sizeof err) == 0);
        fb_sleep_ms(1100);                                        /* history stamps have 1 s resolution */
    }
    FbJson *other = fbf_new("Second");
    CHECK(fbf_lib_save(lib, "second", other, 3, err, sizeof err) == 0);
    fbj_free(other);
    CHECK(fbf_lib_list(lib, &list) == 0 && fbj_len(list) == 2);
    CHECK(list && !strcmp(fbj_get_str(list->items[0], "title", ""), "Second") && !strcmp(fbj_get_str(list->items[1], "title", ""), "v5") && fbj_get_num(list->items[1], "steps", 0) == 1);   /* sorted by file name */
    fbj_free(list);
    char hist[500];
    snprintf(hist, sizeof hist, "%s/.history", lib);
    CHECK(count_files(hist, ".fbflow") == 3);                      /* keep_versions = 3 prunes the older ones */

    FbJson *loaded = NULL;
    CHECK(fbf_lib_load(lib, "Weekly report: 日本/2025?", &loaded, err, sizeof err) == 0);
    CHECK(loaded && !strcmp(fbj_get_str(loaded, "name", ""), "v5") && !strcmp(fbj_get_str(loaded, "description", ""), "hello"));
    fbj_free(loaded);
    CHECK(fbf_lib_load(lib, "nothing", &loaded, err, sizeof err) != 0 && err[0]);

    /* ---- backup / restore ---- */
    FbJson *extra = fbj_obj();
    fbj_obj_set(extra, "settings", fbj_parse_str("{\"theme\":\"dark\"}", err, sizeof err));
    FbJson *bk = fbf_backup_create(lib, extra, err, sizeof err);
    fbj_free(extra);
    CHECK(bk && !strcmp(fbj_get_str(bk, "format", ""), "flowbrowser-backup") && fbj_len(fbj_get(bk, "flows")) == 2);
    CHECK(!strcmp(fbj_get_str(fbj_get(bk, "settings"), "theme", ""), "dark"));
    /* hostile names inside a backup must never escape the library folder */
    FbJson *flows = fbj_get(bk, "flows");
    fbj_obj_set(flows, "../../evil", fbf_new("evil1"));
    fbj_obj_set(flows, "..\\..\\evil2", fbf_new("evil2"));
    fbj_obj_set(flows, "/tmp/abs_evil", fbf_new("evil3"));
    char *bk_text = fbj_to_string(bk, 1);                          /* a backup travels as a file */
    fbj_free(bk);
    FbJson *bk2 = fbj_parse_str(bk_text, err, sizeof err);
    free(bk_text);
    CHECK(bk2 != NULL);

    char lib2[400];
    snprintf(lib2, sizeof lib2, "%s/restored/flows", dir);
    FbJson *rep = NULL;
    CHECK(fbf_backup_restore(bk2, lib2, 0, &rep, err, sizeof err) == 0 && fbj_len(rep) == 5);
    fbj_free(rep);
    CHECK(count_files(lib2, ".fbflow") == 5);
    CHECK(access("/tmp/evil.fbflow", F_OK) != 0 && access("/tmp/abs_evil.fbflow", F_OK) != 0);
    char up[500];
    snprintf(up, sizeof up, "%s/restored/evil.fbflow", dir);
    CHECK(access(up, F_OK) != 0);
    snprintf(up, sizeof up, "%s/evil.fbflow", dir);
    CHECK(access(up, F_OK) != 0);

    {   /* file-name rules */
        const char *in[] = { "../../evil", "  .hidden. ", "con", "NUL.txt", "COM3", "com10", "a/b\\c:d", "", "...", NULL };
        const char *want[] = { "____evil", "hidden", "_con", "_NUL.txt", "_COM3", "com10", "a_b_c_d", "flow", "_" };
        for (int i = 0; in[i]; i++) {
            FbBuf o;
            fb_buf_init(&o);
            fbf_safe_name(in[i], &o);
            if (strcmp(fb_buf_cstr(&o), want[i])) printf("safe_name('%s') = '%s', want '%s'\n", in[i], fb_buf_cstr(&o), want[i]);
            CHECK(!strcmp(fb_buf_cstr(&o), want[i]));
            fb_buf_free(&o);
        }
        FbBuf o;
        fb_buf_init(&o);
        char longname[400];
        memset(longname, 0, sizeof longname);
        for (int i = 0; i < 60; i++) strcat(longname, "日本");      /* 360 bytes of 3-byte characters */
        fbf_safe_name(longname, &o);
        CHECK(o.len <= 120 && o.len % 3 == 0);                         /* cut on a character boundary */
        fb_buf_free(&o);
    }

    /* second restore into the same folder: skip / overwrite / keep both */
    CHECK(fbf_backup_restore(bk2, lib2, 0, &rep, err, sizeof err) == 0);
    int skipped = 0;
    for (size_t i = 0; i < fbj_len(rep); i++) if (!strcmp(fbj_get_str(rep->items[i], "action", ""), "skipped")) skipped++;
    CHECK(skipped == 5);
    fbj_free(rep);
    CHECK(fbf_backup_restore(bk2, lib2, 1, &rep, err, sizeof err) == 0);
    int over = 0;
    for (size_t i = 0; i < fbj_len(rep); i++) if (!strcmp(fbj_get_str(rep->items[i], "action", ""), "overwritten")) over++;
    CHECK(over == 5);
    fbj_free(rep);
    CHECK(fbf_backup_restore(bk2, lib2, 2, &rep, err, sizeof err) == 0);
    int renamed = 0;
    for (size_t i = 0; i < fbj_len(rep); i++) if (!strcmp(fbj_get_str(rep->items[i], "action", ""), "renamed") && fbj_get(rep->items[i], "as")) renamed++;
    CHECK(renamed == 5);
    fbj_free(rep);
    CHECK(count_files(lib2, ".fbflow") == 10);

    FbJson *notbackup = fbj_parse_str("{\"format\":\"something\"}", err, sizeof err);
    CHECK(fbf_backup_restore(notbackup, lib2, 0, &rep, err, sizeof err) != 0 && strstr(err, "not a FlowBrowser backup"));
    fbj_free(rep);
    fbj_free(notbackup);

    /* delete */
    CHECK(fbf_lib_delete(lib, "second") == 0);
    CHECK(fbf_lib_list(lib, &list) == 0 && fbj_len(list) == 1);
    fbj_free(list);

    /* old files are upgraded */
    FbJson *old = fbj_parse_str("{\"name\":\"legacy\",\"steps\":[]}", err, sizeof err);
    fbf_migrate(old);
    CHECK(!strcmp(fbj_get_str(old, "format", ""), "flowbrowser-flow") && fbj_get_num(old, "version", 0) >= 1);
    fbj_free(old);

    fbj_free(bk2);
    fbj_free(flow);
    char cmd[600];
    if (getenv("KEEP")) printf("kept %s\n", dir);
    else { snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir); if (system(cmd)) {} }
    printf("%d checks, %d failed\n", total, fails);
    return fails != 0;
}
