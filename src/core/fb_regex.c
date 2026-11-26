#include "fb_regex.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ char classes */

enum { P_D = 1, P_ND = 2, P_W = 4, P_NW = 8, P_S = 16, P_NS = 32 };

typedef struct {
    uint8_t   bits[16];      /* code points 0..127 */
    uint32_t *r;             /* flat lo,hi pairs for code points >= 128 */
    int       nr, cr;
    int       neg;
    unsigned  preds;
} Class;

static int is_word_cp(uint32_t c)
{
    if (c < 128) return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (c < 0xC0 || c == 0xD7 || c == 0xF7) return 0;
    if (c >= 0x2000 && c <= 0x206F) return 0;        /* general punctuation */
    if (c >= 0x20A0 && c <= 0x20CF) return 0;        /* currency symbols */
    if (c >= 0x2190 && c <= 0x2BFF) return 0;        /* arrows, math, symbols, dingbats */
    if (c >= 0x3000 && c <= 0x303F) return 0;        /* CJK punctuation */
    if (c >= 0xFF00 && c <= 0xFF0F) return 0;
    return 1;
}

static int is_space_cp(uint32_t c)
{
    return c == ' ' || (c >= 9 && c <= 13) || c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200A) ||
           c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF;
}

static int class_has(const Class *k, uint32_t c)
{
    int in = 0;
    if (c < 128) in = (k->bits[c >> 3] >> (c & 7)) & 1;
    else for (int i = 0; i < k->nr && !in; i += 2) if (c >= k->r[i] && c <= k->r[i + 1]) in = 1;
    if (!in && k->preds) {
        int dig = (c >= '0' && c <= '9');
        if ((k->preds & P_D) && dig) in = 1;
        else if ((k->preds & P_ND) && !dig) in = 1;
        else if ((k->preds & P_W) && is_word_cp(c)) in = 1;
        else if ((k->preds & P_NW) && !is_word_cp(c)) in = 1;
        else if ((k->preds & P_S) && is_space_cp(c)) in = 1;
        else if ((k->preds & P_NS) && !is_space_cp(c)) in = 1;
    }
    return in != k->neg;
}

/* ------------------------------------------------------------------ AST */

enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REP, N_GROUP, N_BOL, N_EOL, N_WB, N_NWB };

typedef struct Node {
    int type, c, cls, min, max, greedy, group;
    struct Node *a, *b, *chain;
} Node;

typedef enum { I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_BOL, I_EOL, I_WB, I_NWB, I_MATCH, I_PROGRESS } Op;
typedef struct { uint8_t op; int x, y; } Inst;

struct FbRegex {
    Inst  *prog;
    int    n, cap;
    Class *cls;
    int    ncls, ccls;
    int    ngroups;
    int    nloops;           /* extra work slots used by the empty-loop guard */
    int    flags;
};

typedef struct {
    const char *p, *end;
    int    flags;
    Node  *chain;
    FbRegex *re;
    char   err[120];
} Parser;

static void perr(Parser *P, const char *m) { if (!P->err[0]) snprintf(P->err, sizeof P->err, "%s", m); }

static Node *mk(Parser *P, int type)
{
    Node *n = (Node *)calloc(1, sizeof *n);
    if (!n) { perr(P, "out of memory"); return NULL; }
    n->type = type;
    n->chain = P->chain;
    P->chain = n;
    return n;
}

static uint32_t decode(const char *s, const char *end, int *len)
{
    unsigned char c = (unsigned char)s[0];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (s + n > end) n = 1;
    if (n == 1) { *len = 1; return c; }
    uint32_t cp = c & (0xFF >> (n + 1));
    for (int i = 1; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) { *len = 1; return c; }
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
    }
    *len = n;
    return cp;
}

static int encode(uint32_t cp, unsigned char out[4])
{
    if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { out[0] = (unsigned char)(0xC0 | (cp >> 6)); out[1] = (unsigned char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12)); out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18)); out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

static Node *cat(Parser *P, Node *a, Node *b)
{
    if (!a || a->type == N_EMPTY) return b;
    if (!b || b->type == N_EMPTY) return a;
    Node *n = mk(P, N_CAT);
    if (n) { n->a = a; n->b = b; }
    return n;
}

static Node *lit(Parser *P, uint32_t cp)
{
    unsigned char b[4];
    int n = encode(cp, b);
    Node *r = NULL;
    for (int i = 0; i < n; i++) {
        Node *c = mk(P, N_CHAR);
        if (!c) return NULL;
        c->c = b[i];
        r = cat(P, r, c);
    }
    return r;
}

static int new_class(Parser *P)
{
    FbRegex *re = P->re;
    if (re->ncls == re->ccls) {
        int nc = re->ccls ? re->ccls * 2 : 8;
        Class *k = (Class *)realloc(re->cls, (size_t)nc * sizeof(Class));
        if (!k) { perr(P, "out of memory"); return -1; }
        re->cls = k;
        re->ccls = nc;
    }
    memset(&re->cls[re->ncls], 0, sizeof(Class));
    return re->ncls++;
}

static void cls_range(Parser *P, Class *k, uint32_t lo, uint32_t hi)
{
    for (uint32_t c = lo; c <= hi && c < 128; c++) {
        k->bits[c >> 3] |= (uint8_t)(1u << (c & 7));
        if (P->flags & FBRE_ICASE) {
            uint32_t o = (c >= 'a' && c <= 'z') ? c - 32 : (c >= 'A' && c <= 'Z') ? c + 32 : c;
            k->bits[o >> 3] |= (uint8_t)(1u << (o & 7));
        }
    }
    if (hi >= 128) {
        if (k->nr + 2 > k->cr) {
            int nc = k->cr ? k->cr * 2 : 8;
            uint32_t *r = (uint32_t *)realloc(k->r, (size_t)nc * sizeof(uint32_t));
            if (!r) { perr(P, "out of memory"); return; }
            k->r = r;
            k->cr = nc;
        }
        k->r[k->nr++] = lo < 128 ? 128 : lo;
        k->r[k->nr++] = hi;
    }
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* Parses the escape after a backslash. Returns 1 and sets *cp for a literal, 2 and sets *pred for \d \w \s family,
   3 for \b, 4 for \B, 0 on error. */
static int parse_escape(Parser *P, uint32_t *cp, unsigned *pred)
{
    if (P->p >= P->end) { perr(P, "pattern ends with a backslash"); return 0; }
    char c = *P->p++;
    switch (c) {
    case 'd': *pred = P_D; return 2;
    case 'D': *pred = P_ND; return 2;
    case 'w': *pred = P_W; return 2;
    case 'W': *pred = P_NW; return 2;
    case 's': *pred = P_S; return 2;
    case 'S': *pred = P_NS; return 2;
    case 'b': return 3;
    case 'B': return 4;
    case 'n': *cp = '\n'; return 1;
    case 't': *cp = '\t'; return 1;
    case 'r': *cp = '\r'; return 1;
    case 'f': *cp = '\f'; return 1;
    case 'v': *cp = '\v'; return 1;
    case '0': *cp = 0; return 1;
    case 'x': case 'u': {
        int want = c == 'x' ? 2 : 4;
        uint32_t v = 0;
        for (int i = 0; i < want; i++) {
            int h = P->p < P->end ? hexv(*P->p) : -1;
            if (h < 0) { perr(P, "bad \\x or \\u escape"); return 0; }
            v = (v << 4) | (uint32_t)h;
            P->p++;
        }
        *cp = v;
        return 1;
    }
    default:
        if (c >= '1' && c <= '9') { perr(P, "back-references are not supported"); return 0; }
        *cp = (unsigned char)c;
        if ((unsigned char)c >= 0x80) { int l; P->p--; *cp = decode(P->p, P->end, &l); P->p += l; }
        return 1;
    }
}

static Node *parse_class(Parser *P)
{
    int idx = new_class(P);
    if (idx < 0) return NULL;
    Class *k = &P->re->cls[idx];
    if (P->p < P->end && *P->p == '^') { k->neg = 1; P->p++; }
    int first = 1;
    for (;;) {
        if (P->p >= P->end) { perr(P, "missing ]"); return NULL; }
        if (*P->p == ']' && !first) { P->p++; break; }
        first = 0;
        uint32_t lo;
        unsigned pred = 0;
        if (*P->p == '\\') {
            P->p++;
            int e = parse_escape(P, &lo, &pred);
            if (!e) return NULL;
            if (e == 2) { k->preds |= pred; continue; }
            if (e == 3) lo = 8;          /* [\b] = backspace */
            else if (e == 4) lo = 'B';
        } else {
            int l;
            lo = decode(P->p, P->end, &l);
            P->p += l;
        }
        uint32_t hi = lo;
        if (P->p + 1 < P->end && *P->p == '-' && P->p[1] != ']') {
            const char *save = P->p;
            P->p++;
            if (*P->p == '\\') {
                P->p++;
                unsigned pr = 0;
                int e = parse_escape(P, &hi, &pr);
                if (!e) return NULL;
                if (e != 1) { P->p = save; hi = lo; }          /* "a-\d": '-' is literal */
            } else {
                int l;
                hi = decode(P->p, P->end, &l);
                P->p += l;
            }
            if (hi < lo) { perr(P, "range out of order in []"); return NULL; }
        }
        /* P->re->cls may have been reallocated by nothing here, but keep the pointer fresh anyway */
        k = &P->re->cls[idx];
        cls_range(P, k, lo, hi);
    }
    Node *n = mk(P, N_CLASS);
    if (n) n->cls = idx;
    return n;
}

static Node *parse_alt(Parser *P, int depth);

static Node *parse_atom(Parser *P, int depth)
{
    char c = *P->p;
    if (c == '(') {
        P->p++;
        int gi = -1;
        if (P->p + 1 < P->end && P->p[0] == '?') {
            if (P->p[1] == ':') P->p += 2;
            else { perr(P, "look-around and flags groups are not supported"); return NULL; }
        } else gi = ++P->re->ngroups;
        Node *in = parse_alt(P, depth + 1);
        if (P->err[0]) return NULL;
        if (P->p >= P->end || *P->p != ')') { perr(P, "missing )"); return NULL; }
        P->p++;
        Node *g = mk(P, N_GROUP);
        if (g) { g->a = in; g->group = gi; }
        return g;
    }
    if (c == '[') { P->p++; return parse_class(P); }
    if (c == '.') { P->p++; return mk(P, N_ANY); }
    if (c == '^') { P->p++; return mk(P, N_BOL); }
    if (c == '$') { P->p++; return mk(P, N_EOL); }
    if (c == '*' || c == '+' || c == '?') { perr(P, "nothing to repeat"); return NULL; }
    if (c == '\\') {
        P->p++;
        uint32_t cp;
        unsigned pred = 0;
        int e = parse_escape(P, &cp, &pred);
        if (!e) return NULL;
        if (e == 1) return lit(P, cp);
        if (e == 3) return mk(P, N_WB);
        if (e == 4) return mk(P, N_NWB);
        int idx = new_class(P);
        if (idx < 0) return NULL;
        P->re->cls[idx].preds = pred;
        Node *n = mk(P, N_CLASS);
        if (n) n->cls = idx;
        return n;
    }
    int l;
    uint32_t cp = decode(P->p, P->end, &l);
    P->p += l;
    return lit(P, cp);
}

static int parse_int(const char **p, const char *end)
{
    int v = 0, any = 0;
    while (*p < end && **p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); if (v > 100000) v = 100000; (*p)++; any = 1; }
    return any ? v : -1;
}

static Node *parse_rep(Parser *P, int depth)
{
    Node *a = parse_atom(P, depth);
    if (!a) return NULL;
    if (P->p >= P->end) return a;
    int mn, mx;
    char c = *P->p;
    if (c == '*') { mn = 0; mx = -1; P->p++; }
    else if (c == '+') { mn = 1; mx = -1; P->p++; }
    else if (c == '?') { mn = 0; mx = 1; P->p++; }
    else if (c == '{') {
        const char *q = P->p + 1;
        int lo = parse_int(&q, P->end), hi;
        if (lo < 0) return a;                                  /* literal '{' is handled by parse_cat */
        if (q < P->end && *q == ',') {
            q++;
            hi = parse_int(&q, P->end);
            if (hi < 0) hi = -1;
        } else hi = lo;
        if (q >= P->end || *q != '}') return a;
        if (lo > 1000 || hi > 1000) { perr(P, "repeat count too large (max 1000)"); return NULL; }
        if (hi >= 0 && hi < lo) { perr(P, "bad repeat range"); return NULL; }
        mn = lo; mx = hi;
        P->p = q + 1;
    } else return a;
    Node *r = mk(P, N_REP);
    if (!r) return NULL;
    r->a = a; r->min = mn; r->max = mx; r->greedy = 1;
    if (P->p < P->end && *P->p == '?') { r->greedy = 0; P->p++; }
    if (P->p < P->end && (*P->p == '*' || *P->p == '+' || *P->p == '?')) { perr(P, "nothing to repeat"); return NULL; }
    return r;
}

static Node *parse_cat(Parser *P, int depth)
{
    Node *r = NULL;
    while (P->p < P->end && *P->p != '|' && *P->p != ')') {
        if (*P->p == '{') {                                    /* a '{' that did not form a quantifier is a literal */
            P->p++;
            r = cat(P, r, lit(P, '{'));
            continue;
        }
        Node *x = parse_rep(P, depth);
        if (!x) return NULL;
        r = cat(P, r, x);
        if (P->err[0]) return NULL;
    }
    return r ? r : mk(P, N_EMPTY);
}

static Node *parse_alt(Parser *P, int depth)
{
    if (depth > 100) { perr(P, "pattern nested too deeply"); return NULL; }
    Node *l = parse_cat(P, depth);
    while (l && P->p < P->end && *P->p == '|') {
        P->p++;
        Node *r = parse_cat(P, depth);
        if (!r) return NULL;
        Node *a = mk(P, N_ALT);
        if (!a) return NULL;
        a->a = l; a->b = r;
        l = a;
    }
    return l;
}

/* ------------------------------------------------------------------ code generation */

#define MAX_PROG 60000

static int emit(Parser *P, int op, int x, int y)
{
    FbRegex *re = P->re;
    if (re->n >= MAX_PROG) { perr(P, "pattern too large"); return 0; }
    if (re->n == re->cap) {
        int nc = re->cap ? re->cap * 2 : 64;
        Inst *p = (Inst *)realloc(re->prog, (size_t)nc * sizeof(Inst));
        if (!p) { perr(P, "out of memory"); return 0; }
        re->prog = p;
        re->cap = nc;
    }
    re->prog[re->n].op = (uint8_t)op;
    re->prog[re->n].x = x;
    re->prog[re->n].y = y;
    return re->n++;
}

static int nullable(const Node *n)
{
    if (!n) return 1;
    switch (n->type) {
    case N_CHAR: case N_ANY: case N_CLASS: return 0;
    case N_CAT: return nullable(n->a) && nullable(n->b);
    case N_ALT: return nullable(n->a) || nullable(n->b);
    case N_GROUP: return nullable(n->a);
    case N_REP: return n->min == 0 || nullable(n->a);
    default: return 1;
    }
}

static void gen(Parser *P, const Node *n)
{
    FbRegex *re = P->re;
    if (!n || P->err[0]) return;
    switch (n->type) {
    case N_EMPTY: break;
    case N_CHAR: emit(P, I_CHAR, n->c, (P->flags & FBRE_ICASE) ? 1 : 0); break;
    case N_ANY: emit(P, I_ANY, (P->flags & FBRE_DOTALL) ? 1 : 0, 0); break;
    case N_CLASS: emit(P, I_CLASS, n->cls, 0); break;
    case N_BOL: emit(P, I_BOL, (P->flags & FBRE_MULTILINE) ? 1 : 0, 0); break;
    case N_EOL: emit(P, I_EOL, (P->flags & FBRE_MULTILINE) ? 1 : 0, 0); break;
    case N_WB: emit(P, I_WB, 0, 0); break;
    case N_NWB: emit(P, I_NWB, 0, 0); break;
    case N_CAT: gen(P, n->a); gen(P, n->b); break;
    case N_ALT: {
        int s = emit(P, I_SPLIT, 0, 0);
        re->prog[s].x = s + 1;
        gen(P, n->a);
        int j = emit(P, I_JMP, 0, 0);
        if (P->err[0]) return;
        re->prog[s].y = re->n;
        gen(P, n->b);
        if (P->err[0]) return;
        re->prog[j].x = re->n;
        break;
    }
    case N_GROUP:
        if (n->group >= 0) emit(P, I_SAVE, 2 * n->group, 0);
        gen(P, n->a);
        if (n->group >= 0) emit(P, I_SAVE, 2 * n->group + 1, 0);
        break;
    case N_REP: {
        for (int i = 0; i < n->min && !P->err[0]; i++) gen(P, n->a);
        if (n->max < 0) {
            int s = emit(P, I_SPLIT, 0, 0);
            if (P->err[0]) return;
            int slot = -1;
            if (nullable(n->a)) {                              /* an iteration that matched nothing must not loop forever */
                slot = 2 * (re->ngroups + 1) + re->nloops++;
                emit(P, I_SAVE, slot, 0);
            }
            gen(P, n->a);
            if (slot >= 0) emit(P, I_PROGRESS, slot, 0);
            emit(P, I_JMP, s, 0);
            if (P->err[0]) return;
            int end = re->n;
            re->prog[s].x = n->greedy ? s + 1 : end;
            re->prog[s].y = n->greedy ? end : s + 1;
        } else {
            int cnt = n->max - n->min;
            if (cnt <= 0) break;
            int *sp = (int *)malloc((size_t)cnt * sizeof(int));
            if (!sp) { perr(P, "out of memory"); return; }
            int made = 0;
            for (int i = 0; i < cnt && !P->err[0]; i++) {
                sp[made++] = emit(P, I_SPLIT, 0, 0);
                gen(P, n->a);
            }
            int end = re->n;
            for (int i = 0; i < made && !P->err[0]; i++) {
                re->prog[sp[i]].x = n->greedy ? sp[i] + 1 : end;
                re->prog[sp[i]].y = n->greedy ? end : sp[i] + 1;
            }
            free(sp);
        }
        break;
    }
    }
}

/* ------------------------------------------------------------------ public */

int fbre_flags_from_string(const char *f)
{
    int r = 0;
    for (; f && *f; f++) {
        if (*f == 'i') r |= FBRE_ICASE;
        else if (*f == 'm') r |= FBRE_MULTILINE;
        else if (*f == 's') r |= FBRE_DOTALL;
    }
    return r;
}

void fbre_free(FbRegex *re)
{
    if (!re) return;
    for (int i = 0; i < re->ncls; i++) free(re->cls[i].r);
    free(re->cls);
    free(re->prog);
    free(re);
}

int fbre_ngroups(const FbRegex *re) { return re->ngroups; }

FbRegex *fbre_compile(const char *pat, int flags, char *err, size_t errsz)
{
    FbRegex *re = (FbRegex *)calloc(1, sizeof *re);
    if (!re) return NULL;
    Parser P;
    memset(&P, 0, sizeof P);
    P.p = pat;
    P.end = pat + strlen(pat);
    P.flags = flags;
    P.re = re;
    re->flags = flags;
    Node *root = parse_alt(&P, 0);
    if (!P.err[0] && P.p < P.end) perr(&P, "unmatched )");
    if (!P.err[0]) {
        gen(&P, root);
        emit(&P, I_MATCH, 0, 0);
    }
    for (Node *n = P.chain; n;) { Node *nx = n->chain; free(n); n = nx; }
    if (P.err[0]) {
        if (err && errsz) snprintf(err, errsz, "%s", P.err);
        fbre_free(re);
        return NULL;
    }
    return re;
}

/* ------------------------------------------------------------------ matcher */

typedef struct { int pc, pos, slot, old; } Ent;       /* pc < 0: restore caps[slot] = old */

#define MAX_STACK 400000
#define MAX_STEPS 8000000L

static int word_byte(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80; }

static int run(const FbRegex *re, const char *s, int len, int start, int *out_caps, long *steps)
{
    int ncap = 2 * (re->ngroups + 1);
    int nwork = ncap + re->nloops;
    int *caps = (int *)malloc((size_t)nwork * sizeof(int));
    if (!caps) return -1;
    for (int i = 0; i < nwork; i++) caps[i] = -1;
    caps[0] = start;
    int cap = 256, sp = 0;
    Ent *st = (Ent *)malloc((size_t)cap * sizeof(Ent));
    if (!st) return -1;
    int result = 0;
    st[sp].pc = 0; st[sp].pos = start; sp++;
    while (sp > 0) {
        Ent e = st[--sp];
        if (e.pc < 0) { caps[e.slot] = e.old; continue; }
        int pc = e.pc, pos = e.pos;
        for (;;) {
            if (++*steps > MAX_STEPS) { result = -1; goto done; }
            const Inst *in = &re->prog[pc];
            int ok = 1;
            switch (in->op) {
            case I_CHAR:
                if (pos < len && ((unsigned char)s[pos] == in->x ||
                                  (in->y && (unsigned char)s[pos] < 128 && in->x < 128 &&
                                   ((s[pos] | 32) == (in->x | 32)) && ((in->x | 32) >= 'a' && (in->x | 32) <= 'z')))) { pos++; pc++; }
                else ok = 0;
                break;
            case I_ANY: {
                if (pos >= len) { ok = 0; break; }
                int l;
                uint32_t c = decode(s + pos, s + len, &l);
                if (!in->x && c == '\n') { ok = 0; break; }
                pos += l; pc++;
                break;
            }
            case I_CLASS: {
                if (pos >= len) { ok = 0; break; }
                int l;
                uint32_t c = decode(s + pos, s + len, &l);
                if (!class_has(&re->cls[in->x], c)) { ok = 0; break; }
                pos += l; pc++;
                break;
            }
            case I_SPLIT:
                if (sp + 2 >= cap) {
                    if (cap >= MAX_STACK) { result = -1; goto done; }
                    cap *= 2;
                    Ent *ns = (Ent *)realloc(st, (size_t)cap * sizeof(Ent));
                    if (!ns) { result = -1; goto done; }
                    st = ns;
                }
                st[sp].pc = in->y; st[sp].pos = pos; sp++;
                pc = in->x;
                break;
            case I_JMP: pc = in->x; break;
            case I_SAVE:
                if (sp + 2 >= cap) {
                    if (cap >= MAX_STACK) { result = -1; goto done; }
                    cap *= 2;
                    Ent *ns = (Ent *)realloc(st, (size_t)cap * sizeof(Ent));
                    if (!ns) { result = -1; goto done; }
                    st = ns;
                }
                st[sp].pc = -1; st[sp].slot = in->x; st[sp].old = caps[in->x]; sp++;
                caps[in->x] = pos;
                pc++;
                break;
            case I_BOL:
                if (pos == 0 || (in->x && s[pos - 1] == '\n')) pc++; else ok = 0;
                break;
            case I_EOL:
                if (pos == len || (in->x && s[pos] == '\n')) pc++; else ok = 0;
                break;
            case I_WB: case I_NWB: {
                int a = pos > 0 && word_byte((unsigned char)s[pos - 1]);
                int b = pos < len && word_byte((unsigned char)s[pos]);
                if ((a != b) == (in->op == I_WB)) pc++; else ok = 0;
                break;
            }
            case I_PROGRESS:
                if (caps[in->x] == pos) ok = 0; else pc++;
                break;
            case I_MATCH:
                caps[1] = pos;
                memcpy(out_caps, caps, (size_t)ncap * sizeof(int));
                result = 1;
                goto done;
            }
            if (!ok) break;
        }
    }
done:
    free(st);
    free(caps);
    return result;
}

int fbre_search(const FbRegex *re, const char *s, size_t len, size_t from, int *caps)
{
    if (len > 0x7ffffff0u) return -1;
    long steps = 0;
    int n = (int)len;
    for (int st = (int)from; st <= n;) {
        int r = run(re, s, n, st, caps, &steps);
        if (r != 0) return r;
        if (st >= n) break;
        int l;
        decode(s + st, s + n, &l);
        st += l;
    }
    return 0;
}
