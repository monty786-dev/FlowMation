#include <stdio.h>
#include <string.h>
#include "../src/core/fb_regex.h"

static int fails, total;
#define CHECK(c) do { total++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

/* returns match text in out (or "" / NULL) ; grp selects capture group */
static int m(const char *pat, const char *flags, const char *s, int grp, char *out, size_t osz)
{
    char err[160];
    FbRegex *re = fbre_compile(pat, fbre_flags_from_string(flags), err, sizeof err);
    if (!re) { snprintf(out, osz, "ERR:%s", err); return -2; }
    int caps[64];
    int r = fbre_search(re, s, strlen(s), 0, caps);
    out[0] = 0;
    if (r == 1 && caps[2 * grp] >= 0) snprintf(out, osz, "%.*s", caps[2 * grp + 1] - caps[2 * grp], s + caps[2 * grp]);
    fbre_free(re);
    return r;
}

#define T(pat, fl, s, grp, expect) do { char o[256]; int r = m(pat, fl, s, grp, o, sizeof o); \
    total++; if (r != 1 || strcmp(o, expect)) { fails++; printf("FAIL line %d: /%s/%s on '%s' -> r=%d '%s' want '%s'\n", __LINE__, pat, fl, s, r, o, expect); } } while (0)
#define NOMATCH(pat, fl, s) do { char o[256]; int r = m(pat, fl, s, 0, o, sizeof o); \
    total++; if (r != 0) { fails++; printf("FAIL line %d: /%s/ should not match '%s' (r=%d %s)\n", __LINE__, pat, s, r, o); } } while (0)
#define BAD(pat) do { char o[256]; int r = m(pat, "", "x", 0, o, sizeof o); \
    total++; if (r != -2) { fails++; printf("FAIL line %d: /%s/ should be rejected\n", __LINE__, pat); } } while (0)

int main(void)
{
    T("abc", "", "xxabcxx", 0, "abc");
    T("a.c", "", "a-c", 0, "a-c");
    NOMATCH("a.c", "", "a\nc");
    T("a.c", "s", "a\nc", 0, "a\nc");
    T("colou?r", "", "my color", 0, "color");
    T("a+", "", "baaad", 0, "aaa");
    T("a+?", "", "baaad", 0, "a");
    T("<.*>", "", "<a><b>", 0, "<a><b>");
    T("<.*?>", "", "<a><b>", 0, "<a>");
    T("(\\d+)-(\\d+)", "", "tel 555-1234 x", 1, "555");
    T("(\\d+)-(\\d+)", "", "tel 555-1234 x", 2, "1234");
    T("\\d{3}", "", "ab12345", 0, "123");
    T("\\d{2,3}", "", "ab12345", 0, "123");
    T("\\d{2,}", "", "a1 22222", 0, "22222");
    T("x{0}y", "", "xy", 0, "y");
    T("^foo", "", "foobar", 0, "foo");
    NOMATCH("^bar", "", "foobar");
    T("bar$", "", "foobar", 0, "bar");
    T("^b", "m", "a\nb", 0, "b");
    T("a$", "m", "a\nb", 0, "a");
    T("cat|dog", "", "hotdog", 0, "dog");
    T("(?:ab)+", "", "xababab", 0, "ababab");
    T("(a|b)*c", "", "abbac", 0, "abbac");
    T("[a-c]+", "", "xxabcabcxx", 0, "abcabc");
    T("[^a-c]+", "", "abcxyzabc", 0, "xyz");
    T("[\\d.]+", "", "v 3.14 ok", 0, "3.14");
    T("[a-]+", "", "x-a-", 0, "-a-");
    T("[]a]+", "", "x]a]", 0, "]a]");
    T("\\bword\\b", "", "a word here", 0, "word");
    NOMATCH("\\bword\\b", "", "swordfish");
    T("\\Boo\\B", "", "foot", 0, "oo");
    T("HELLO", "i", "say hello", 0, "hello");
    T("[A-Z]+", "i", "abcDEF", 0, "abcDEF");
    NOMATCH("HELLO", "", "hello");
    T("\\s+", "", "a \t\nb", 0, " \t\n");
    T("\\w+", "", "  foo_bar9 ", 0, "foo_bar9");
    T("\\S+@\\S+", "", "mail me@x.org now", 0, "me@x.org");
    T("\\$(\\d+\\.\\d\\d)", "", "cost $12.50!", 1, "12.50");
    T("\\u20ac\\s*(\\d+)", "", "price € 42", 1, "42");
    T("caf.", "", "un café", 0, "café");            /* '.' must consume the whole 2-byte char */
    T("é+", "", "xéééx", 0, "ééé");                 /* quantifier applies to the whole UTF-8 char */
    T("[éà]+", "", "xxéàéxx", 0, "éàé");
    T("\\w+", "", "naïve", 0, "naïve");
    T("a{", "", "a{", 0, "a{");
    T("a{x}", "", "a{x}", 0, "a{x}");
    T("", "", "abc", 0, "");
    T("(a*)*b", "", "aaab", 0, "aaab");
    T("(|a)+", "", "aaa", 0, "aaa");
    T("\\.", "", "a.b", 0, ".");
    T("\\x41", "", "xA", 0, "A");
    T("(\\w+)\\s(\\w+)", "", "hello big world", 2, "big");
    T("^(.*?),(.*)$", "", "a,b,c", 2, "b,c");
    BAD("(abc");
    BAD("abc)");
    BAD("[abc");
    BAD("*a");
    BAD("a**");           /* second quantifier has nothing to repeat? lenient engines allow it, we allow lazy only */
    BAD("(?=a)");
    BAD("\\1");
    BAD("a{3,1}");
    BAD("a{5000}");
    BAD("[z-a]");

    /* start offset + iteration, as text_transform's "extract all" uses it */
    {
        char err[100];
        FbRegex *re = fbre_compile("\\d+", 0, err, sizeof err);
        const char *s = "a1 bb22 ccc333";
        int caps[4], pos = 0, n = 0;
        char got[64] = "";
        while (fbre_search(re, s, strlen(s), (size_t)pos, caps) == 1) {
            strncat(got, s + caps[0], (size_t)(caps[1] - caps[0]));
            strcat(got, ",");
            pos = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
            n++;
        }
        CHECK(n == 3 && !strcmp(got, "1,22,333,"));
        fbre_free(re);
    }
    /* catastrophic patterns must give up instead of hanging */
    {
        char err[100];
        FbRegex *re = fbre_compile("(a+)+$", 0, err, sizeof err);
        char s[40];
        memset(s, 'a', 36); s[36] = 'b'; s[37] = 0;
        int caps[4];
        int r = fbre_search(re, s, strlen(s), 0, caps);
        CHECK(r == -1 || r == 0);
        fbre_free(re);
    }
    /* long input with .* must not blow the stack */
    {
        static char big[300000];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = 0;
        char err[100];
        FbRegex *re = fbre_compile("x.*y", 0, err, sizeof err);
        int caps[2];
        int r = fbre_search(re, big, strlen(big), 0, caps);
        CHECK(r == 0 || r == -1);
        fbre_free(re);
    }
    printf("%d checks, %d failed\n", total, fails);
    return fails != 0;
}
