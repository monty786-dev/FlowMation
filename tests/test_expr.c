#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/fb_expr.h"
#include "../src/core/fb_json.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("  FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

static FbJson *V;
static char err[256];

static void X(const char *tpl, const char *expect)
{
    FbBuf b;
    fb_buf_init(&b);
    err[0] = 0;
    int rc = fbe_expand(V, tpl, &b, err, sizeof err);
    if (rc == 0 && !strcmp(fb_buf_cstr(&b), expect)) g_pass++;
    else { g_fail++; printf("  FAIL expand '%s' -> '%s' (rc=%d %s), expected '%s'\n", tpl, fb_buf_cstr(&b), rc, err, expect); }
    fb_buf_free(&b);
}

static void C(const char *cond, int expect)
{
    err[0] = 0;
    int r = fbe_eval_cond(V, cond, err, sizeof err);
    if (r == expect) g_pass++;
    else { g_fail++; printf("  FAIL cond '%s' -> %d (%s), expected %d\n", cond, r, err, expect); }
}

static void K(const char *e, double expect, int ok)
{
    double d = 0;
    err[0] = 0;
    int rc = fbe_calc(e, &d, err, sizeof err);
    if (ok ? (rc == 0 && fabs(d - expect) < 1e-9) : rc != 0) g_pass++;
    else { g_fail++; printf("  FAIL calc '%s' -> %g (rc=%d %s) expected %g/%s\n", e, d, rc, err, expect, ok ? "ok" : "error"); }
}

int main(void)
{
    const char *src =
        "{\"name\":\"  ana müller \",\"price\":\"$1,234.50\",\"eu\":\"1.234,56\",\"n\":5,"
        "\"row\":{\"First Name\":\"Ana\",\"Amount\":\"1250.5\",\"_row\":7,\"Empty\":\"\"},"
        "\"items\":[{\"title\":\"A\"},{\"title\":\"B\"},{\"title\":\"C\"}],"
        "\"i\":1,\"list\":[\"x\",\"y\",\"z\"],\"when\":\"2025-01-02 13:30:00\",\"flag\":\"false\",\"zero\":0,"
        "\"rows\":[{\"Full Name\":\"R0\"},{\"Full Name\":\"R1\"},{\"Full Name\":\"R2\"}]}";
    V = fbj_parse_str(src, err, sizeof err);
    if (!V) { printf("setup failed: %s\n", err); return 1; }

    printf("paths + templates\n");
    X("Hello {{row.First Name}}!", "Hello Ana!");
    X("{{ row.Amount }}", "1250.5");
    X("{{row._row}}", "7");
    X("{{items[0].title}}{{items[2].title}}{{items[-1].title}}", "ACC");
    X("{{items[i].title}}", "B");
    X("{{rows[2][\"Full Name\"]}}", "R2");
    X("{{items.length}} / {{list.length}} / {{name.length}}", "3 / 3 / 13");
    X("{{missing}}|{{row.nope}}|{{items[9]}}", "||");
    X("no templates here", "no templates here");
    X("unclosed {{ row", "unclosed {{ row");
    X("{{\"literal | pipe\" | upper}}", "LITERAL | PIPE");

    printf("filters\n");
    X("[{{name | trim}}]", "[ana müller]");
    X("{{name | trim | upper}}", "ANA MüLLER");           /* non-ASCII bytes are left alone */
    X("{{name | trim | title}}", "Ana M\xC3\xBCller");
    X("{{price | number}}", "1234.5");
    X("{{eu | number}}", "1234.56");
    X("{{price | number | int}}", "1234");
    X("{{price | number | round:1}}", "1234.5");
    X("{{row.Empty | default:n/a}}", "n/a");
    X("{{row.Amount | default:n/a}}", "1250.5");
    X("{{list | join:-}}", "x-y-z");
    X("{{list | join}}", "x, y, z");
    X("{{\"a,b,c\" | split:, | last}}", "c");
    X("{{name | trim | replace:ana:Bob}}", "Bob müller");
    X("{{name | trim | substr:0:3}}", "ana");
    X("{{n | pad:3}}", "005");
    X("{{when | date:DD/MM/YYYY}}", "02/01/2025");
    X("{{when | date:ddd, MMM DD [at] HH:mm}}", "Thu, Jan 02 at 13:30");
    X("{{\"a b&c\" | urlencode}}", "a%20b%26c");
    X("{{list | json}}", "[\"x\",\"y\",\"z\"]");
    {   /* unknown filters are reported, not silently ignored */
        FbBuf b; fb_buf_init(&b);
        CHECK(fbe_expand(V, "{{n | nosuchfilter}}", &b, err, sizeof err) == -1 && strstr(err, "unknown filter"));
        fb_buf_free(&b);
    }
    X("{{when | date:YYYY-MM-DD HH:mm:ss}}", "2025-01-02 13:30:00");
    X("{{list | join:\" : \"}}", "x : y : z");     /* quote separators that contain spaces */

    printf("typed values\n");
    FbJson *tv = fbe_expand_value(V, "{{items}}", err, sizeof err);
    CHECK(tv && tv->type == FBJ_ARR && tv->count == 3);
    fbj_free(tv);
    tv = fbe_expand_value(V, "  {{row}}  ", err, sizeof err);
    CHECK(tv && tv->type == FBJ_OBJ);
    fbj_free(tv);
    tv = fbe_expand_value(V, "{{n}}", err, sizeof err);
    CHECK(tv && tv->type == FBJ_NUM && tv->num == 5);
    fbj_free(tv);
    tv = fbe_expand_value(V, "x{{n}}", err, sizeof err);
    CHECK(tv && tv->type == FBJ_STR && !strcmp(tv->str, "x5"));
    fbj_free(tv);

    printf("conditions\n");
    C("10 > 9", 1);                                   /* numeric, not lexicographic */
    C("{{row.Amount}} >= 1250.5", 1);
    C("{{row.Amount}} < 1000", 0);
    C("{{row.First Name}} == Ana", 1);
    C("{{row.First Name}} == \"Ana\"", 1);
    C("{{row.First Name}} != \"Bob\"", 1);
    C("{{row.First Name}} contains na", 1);
    C("{{row.First Name}} contains NA", 0);
    C("{{row.First Name}} icontains NA", 1);
    C("{{row.First Name}} startswith An", 1);
    C("{{row.First Name}} endswith na", 1);
    C("{{row.Empty}} is empty", 1);
    C("{{row.First Name}} is not empty", 1);
    C("{{missing}} is empty", 1);
    C("{{n}} == 5 and {{row.First Name}} == Ana", 1);
    C("{{n}} == 4 or {{row.First Name}} == Ana", 1);
    C("{{n}} == 4 or {{n}} == 3", 0);
    C("{{n}} == 5 and {{n}} == 4 or {{n}} == 5", 1);
    C("{{flag}}", 0);                                 /* "false" is falsy */
    C("{{zero}}", 0);
    C("{{n}}", 1);
    C("not {{flag}}", 1);
    C("!{{n}}", 0);
    C("\"a and b\" == \"a and b\"", 1);               /* operators inside quotes are data */
    C("{{row.First Name}} == \"Ana or Bob\"", 0);
    C("\"{{row.First Name}} Smith\" == \"Ana Smith\"", 1);
    C("", 1);

    printf("calc\n");
    K("1 + 2 * 3", 7, 1);
    K("(1 + 2) * 3", 9, 1);
    K("-4 + 10 / 4", -1.5, 1);
    K("10 % 4", 2, 1);
    K("2 * -3", -6, 1);
    K("1 / 0", 0, 0);
    K("1 +", 0, 0);
    K("2 3", 0, 0);
    K("(1 + 2", 0, 0);
    K("abc", 0, 0);

    printf("path_set\n");
    FbJson *o = fbj_obj();
    CHECK(fbe_path_set(o, "a", fbj_num(1)) == 0);
    CHECK(fbe_path_set(o, "b.c.d", fbj_str("deep")) == 0);
    CHECK(fbe_path_set(o, "a", fbj_num(2)) == 0);
    char *s = fbj_to_string(o, 0);
    CHECK(!strcmp(s, "{\"a\":2,\"b\":{\"c\":{\"d\":\"deep\"}}}"));
    free(s);
    fbj_free(o);

    fbj_free(V);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
