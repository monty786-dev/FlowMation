/* Core library tests. Usage: test_core <fixture.xlsx> <out_dir> */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/core/fb_json.h"
#include "../src/core/fb_util.h"
#include "../src/core/fb_xlsx.h"
#include "../src/core/fb_zip.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) g_pass++; else { g_fail++; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { const char *_a = (a), *_b = (b); \
    if (_a && _b && !strcmp(_a, _b)) g_pass++; else { g_fail++; printf("  FAIL %s:%d  got '%s' expected '%s'\n", __FILE__, __LINE__, _a ? _a : "(null)", _b); } } while (0)

static void test_util(void)
{
    printf("util\n");
    CHECK(fb_crc32(0, "123456789", 9) == 0xCBF43926u);
    FbBuf b, d;
    fb_buf_init(&b); fb_buf_init(&d);
    fb_base64_encode("Hello, World!?", 14, &b);
    CHECK_STR(fb_buf_cstr(&b), "SGVsbG8sIFdvcmxkIT8=");
    fb_base64_decode(b.data, b.len, &d);
    CHECK(d.len == 14 && !memcmp(d.data, "Hello, World!?", 14));
    fb_buf_clear(&b);
    fb_xml_escape(&b, "a<b>&\"c\"\x01", 10);
    CHECK_STR(fb_buf_cstr(&b), "a&lt;b&gt;&amp;&quot;c&quot;");
    fb_buf_clear(&d);
    fb_xml_unescape(&d, "x &lt;y&gt; &amp; &#65;&#x42; &unknown;", strlen("x &lt;y&gt; &amp; &#65;&#x42; &unknown;"));
    CHECK_STR(fb_buf_cstr(&d), "x <y> & AB &unknown;");
    fb_buf_free(&b); fb_buf_free(&d);
}

static void test_json(void)
{
    printf("json\n");
    char err[128];
    const char *src = "{\"a\":1,\"b\":[true,false,null,\"x\\n\\u00e9\\ud83d\\ude00\"],\"c\":{\"d\":-2.5e3},}";
    FbJson *j = fbj_parse_str(src, err, sizeof err);
    CHECK(j != NULL);
    if (!j) { printf("  %s\n", err); return; }
    CHECK(fbj_get_num(j, "a", 0) == 1);
    FbJson *b = fbj_get(j, "b");
    CHECK(fbj_len(b) == 4);
    CHECK_STR(fbj_at(b, 3)->str, "x\n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(fbj_get_num(fbj_get(j, "c"), "d", 0) == -2500);
    char *s = fbj_to_string(j, 0);
    CHECK_STR(s, "{\"a\":1,\"b\":[true,false,null,\"x\\n\xC3\xA9\xF0\x9F\x98\x80\"],\"c\":{\"d\":-2500}}");
    FbJson *j2 = fbj_parse_str(s, err, sizeof err);
    CHECK(j2 != NULL);
    char *s2 = fbj_to_string(j2, 1);
    FbJson *j3 = fbj_parse_str(s2, err, sizeof err);
    char *s3 = fbj_to_string(j3, 0);
    CHECK_STR(s3, s);
    free(s); free(s2); free(s3);
    fbj_free(j); fbj_free(j2); fbj_free(j3);
    CHECK(fbj_parse_str("{\"a\":", err, sizeof err) == NULL);
    CHECK(fbj_parse_str("[1,2] x", err, sizeof err) == NULL);
    CHECK(fbj_parse_str("\"abc", err, sizeof err) == NULL);
    FbJson *o = fbj_obj();
    fbj_obj_set(o, "k", fbj_str("v1"));
    fbj_obj_set(o, "k", fbj_str("v2"));
    fbj_obj_set(o, "n", fbj_num(0.1));
    CHECK(fbj_len(o) == 2);
    CHECK_STR(fbj_get_str(o, "k", ""), "v2");
    fbj_obj_del(o, "k");
    CHECK(fbj_len(o) == 1);
    fbj_free(o);
}

static void test_refs(void)
{
    printf("refs\n");
    int r, c, r2, c2;
    CHECK(fbx_parse_ref("B5", &r, &c) == 0 && r == 5 && c == 2);
    CHECK(fbx_parse_ref("$AA$10", &r, &c) == 0 && r == 10 && c == 27);
    CHECK(fbx_parse_ref("XFD1048576", &r, &c) == 0 && c == 16384);
    CHECK(fbx_parse_ref("XFE1", &r, &c) != 0);
    CHECK(fbx_parse_ref("A0", &r, &c) != 0);
    CHECK(fbx_parse_ref("5B", &r, &c) != 0);
    CHECK(fbx_parse_range("A1:D9", &r, &c, &r2, &c2) == 0 && r == 1 && c == 1 && r2 == 9 && c2 == 4);
    CHECK(fbx_parse_range("B:C", &r, &c, &r2, &c2) == 0 && r == 0 && c == 2 && c2 == 3);
    CHECK(fbx_parse_range("C3", &r, &c, &r2, &c2) == 0 && r2 == 3 && c2 == 3);
    char nm[8];
    fbx_col_name(1, nm); CHECK_STR(nm, "A");
    fbx_col_name(26, nm); CHECK_STR(nm, "Z");
    fbx_col_name(27, nm); CHECK_STR(nm, "AA");
    fbx_col_name(702, nm); CHECK_STR(nm, "ZZ");
    fbx_col_name(703, nm); CHECK_STR(nm, "AAA");
}

static void test_csv(void)
{
    printf("csv\n");
    const char *t = "\xEF\xBB\xBFName;City;Note\r\nAna;\"Mü;nchen\";\"say \"\"hi\"\"\"\r\nBob;;\"multi\nline\"\r\n\r\nEve;X;Y";
    FbGrid g;
    CHECK(fbcsv_parse(t, strlen(t), &g) == 0);
    CHECK(g.nrows == 5);
    CHECK_STR(fbgrid_get(&g, 0, 0), "Name");
    CHECK_STR(fbgrid_get(&g, 1, 1), "M\xC3\xBC;nchen");
    CHECK_STR(fbgrid_get(&g, 1, 2), "say \"hi\"");
    CHECK(fbgrid_get(&g, 2, 1) == NULL);
    CHECK_STR(fbgrid_get(&g, 2, 2), "multi\nline");
    CHECK_STR(fbgrid_get(&g, 4, 2), "Y");
    FbJson *tab = fbgrid_to_json(&g, 0, 1, 1, 1);
    CHECK(fbj_len(tab) == 3);                       /* blank row skipped */
    CHECK_STR(fbj_get_str(fbj_at(tab, 0), "City", ""), "M\xC3\xBC;nchen");
    CHECK(fbj_get_num(fbj_at(tab, 2), "_row", 0) == 5);
    fbj_free(tab);
    fbgrid_free(&g);
    FbBuf o;
    fb_buf_init(&o);
    fbcsv_write_field(&o, "a,b", ',');  fb_buf_putc(&o, '|');
    fbcsv_write_field(&o, "say \"x\"", ','); fb_buf_putc(&o, '|');
    fbcsv_write_field(&o, "plain", ',');
    CHECK_STR(fb_buf_cstr(&o), "\"a,b\"|\"say \"\"x\"\"\"|plain");
    fb_buf_free(&o);
}

static void test_xlsx_read(const char *fixture)
{
    printf("xlsx read\n");
    char err[256] = "";
    FbGrid g;
    CHECK(fbx_read_sheet(fixture, "Customers", &g, err, sizeof err) == 0);
    if (g.nrows == 0) { printf("  %s\n", err); return; }
    CHECK(g.nrows == 6);
    CHECK_STR(fbgrid_get(&g, 0, 0), "Name");
    CHECK_STR(fbgrid_get(&g, 1, 0), "Ana M\xC3\xBCller");
    CHECK_STR(fbgrid_get(&g, 1, 2), "1250.5");
    CHECK_STR(fbgrid_get(&g, 1, 3), "2024-03-15");               /* date style -> ISO */
    CHECK_STR(fbgrid_get(&g, 2, 3), "2025-01-02 13:30:00");
    CHECK_STR(fbgrid_get(&g, 1, 4), "\xC3\xBCn\xC3\xAF" "code & <tags> \"quoted\"");
    CHECK_STR(fbgrid_get(&g, 2, 4), "line1\nline2");
    CHECK_STR(fbgrid_get(&g, 3, 0), "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE5\x90\x8D\xE5\x89\x8D");
    CHECK_STR(fbgrid_get(&g, 3, 2), "0.3");                       /* 0.1+0.2 shown like Excel */
    CHECK(fbgrid_get(&g, 3, 3) == NULL);
    CHECK_STR(fbgrid_get(&g, 5, 4), "merged");
    FbJson *t = fbgrid_to_json(&g, 0, 1, 1, 1);
    CHECK(fbj_len(t) == 5);
    CHECK_STR(fbj_get_str(fbj_at(t, 0), "Email", ""), "ana@example.com");
    CHECK(fbj_get_num(fbj_at(t, 0), "_row", 0) == 2);
    fbj_free(t);
    fbgrid_free(&g);

    CHECK(fbx_read_sheet(fixture, "data & notes", &g, err, sizeof err) == 0);   /* case-insensitive */
    CHECK_STR(fbgrid_get(&g, 1, 0), "007");
    CHECK_STR(fbgrid_get(&g, 1, 1), "TRUE");
    fbgrid_free(&g);
    CHECK(fbx_read_sheet(fixture, "Nope", &g, err, sizeof err) != 0);
}

static void test_xlsx_patch(const char *fixture, const char *outdir)
{
    printf("xlsx patch\n");
    char err[256] = "";
    FbXEdit *x = fbxe_open(fixture, 0, err, sizeof err);
    CHECK(x != NULL);
    if (!x) { printf("  %s\n", err); return; }
    CHECK(fbxe_last_row(x, "Customers") == 6);       /* row 6 holds the merged cell text */
    CHECK(fbxe_last_row(x, "Missing") == 0);
    /* overwrite an existing styled cell (keeps its style), append to a new row, write a formula,
       a number-looking text, leading zeros and special characters */
    CHECK(fbxe_set(x, "Customers", 2, 3, "2000", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Customers", 8, 1, "Zoë & <Co>", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Customers", 8, 3, "42.50", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Customers", 8, 4, "00123", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Customers", 8, 5, "=C8*2", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Customers", 9, 1, "Header-bold", "text", 1, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Customers", 3, 5, "", NULL, 0, err, sizeof err) == 0);      /* clear */
    CHECK(fbxe_set(x, "Data & Notes", 5, 2, "new row in sheet 2", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Results: 2026", 1, 1, "Title", NULL, 1, err, sizeof err) == 0);  /* new sheet, bad name char */
    CHECK(fbxe_set(x, "Results: 2026", 2, 2, "x", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Customers", 0, 1, "bad", NULL, 0, err, sizeof err) != 0);
    CHECK(fbxe_dirty(x));
    FbBuf path;
    fb_buf_init(&path);
    fb_buf_printf(&path, "%s/patched.xlsx", outdir);
    CHECK(fbxe_save(x, fb_buf_cstr(&path), err, sizeof err) == 0);
    fbxe_close(x);

    /* round trip through our own reader */
    FbGrid g;
    CHECK(fbx_read_sheet(fb_buf_cstr(&path), "Customers", &g, err, sizeof err) == 0);
    CHECK_STR(fbgrid_get(&g, 1, 2), "2000");
    CHECK_STR(fbgrid_get(&g, 7, 0), "Zo\xC3\xAB & <Co>");
    CHECK_STR(fbgrid_get(&g, 7, 3), "00123");
    CHECK(fbgrid_get(&g, 2, 4) == NULL);
    CHECK_STR(fbgrid_get(&g, 1, 1), "ana@example.com");           /* untouched */
    fbgrid_free(&g);
    fb_buf_free(&path);
}

static void test_xlsx_new(const char *outdir)
{
    printf("xlsx new\n");
    char err[256] = "";
    FbBuf path;
    fb_buf_init(&path);
    fb_buf_printf(&path, "%s/new.xlsx", outdir);
    fb_delete_file(fb_buf_cstr(&path));                 /* start from "file does not exist" */
    FbXEdit *x = fbxe_open(fb_buf_cstr(&path), 1, err, sizeof err);
    CHECK(x != NULL);
    if (!x) return;
    const char *hdr[] = { "Product", "Price", "URL" };
    for (int c = 0; c < 3; c++) CHECK(fbxe_set(x, "Products", 1, c + 1, hdr[c], "text", 1, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Products", 2, 1, "Widget \"Pro\"", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Products", 2, 2, "19.99", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Products", 2, 3, "https://example.com/p?a=1&b=2", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Products", 3, 2, "=B2*3", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_set(x, "Second", 1, 1, "x", NULL, 0, err, sizeof err) == 0);
    CHECK(fbxe_last_row(x, "Products") == 3);
    CHECK(fbxe_save(x, fb_buf_cstr(&path), err, sizeof err) == 0);
    fbxe_close(x);
    /* re-open for append, the way "append row" works across runs */
    x = fbxe_open(fb_buf_cstr(&path), 0, err, sizeof err);
    CHECK(x != NULL);
    if (x) {
        int last = fbxe_last_row(x, "Products");
        CHECK(last == 3);
        CHECK(fbxe_set(x, "Products", last + 1, 1, "Appended", NULL, 0, err, sizeof err) == 0);
        CHECK(fbxe_save(x, fb_buf_cstr(&path), err, sizeof err) == 0);
        fbxe_close(x);
    }
    fb_buf_free(&path);
}

int main(int argc, char **argv)
{
    test_util();
    test_json();
    test_refs();
    test_csv();
    if (argc >= 3) {
        test_xlsx_read(argv[1]);
        test_xlsx_patch(argv[1], argv[2]);
        test_xlsx_new(argv[2]);
    } else printf("(skipping xlsx tests: pass <fixture.xlsx> <out_dir>)\n");
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
