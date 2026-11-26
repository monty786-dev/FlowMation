#include "fb_zip.h"

#include <stdlib.h>
#include <string.h>

/* ================================================================ inflate */

#define MAXBITS 15
#define FIXLCODES 288

typedef struct {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
} Huffman;

typedef struct {
    const unsigned char *in;
    size_t inlen, incnt;
    unsigned long bitbuf;
    int bitcnt;
    int err;
    FbBuf *out;
    size_t max_out;
} State;

static int getbits(State *s, int need)
{
    unsigned long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) { s->err = 1; return 0; }
        val |= (unsigned long)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1UL << need) - 1));
}

static int decode(State *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= getbits(s, 1);
        if (s->err) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->err = 1;
    return -1;
}

static int construct(Huffman *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    int left = 1;
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static const short LENS[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const short LEXT[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const short DISTS[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const short DEXT[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

static int codes(State *s, const Huffman *lencode, const Huffman *distcode)
{
    for (;;) {
        int sym = decode(s, lencode);
        if (s->err || sym < 0) return -1;
        if (sym < 256) {
            if (s->out->len >= s->max_out) return -1;
            char c = (char)sym;
            if (fb_buf_append(s->out, &c, 1)) return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = LENS[sym] + getbits(s, LEXT[sym]);
            int ds = decode(s, distcode);
            if (s->err || ds < 0 || ds >= 30) return -1;
            size_t dist = (size_t)DISTS[ds] + (size_t)getbits(s, DEXT[ds]);
            if (s->err || dist > s->out->len) return -1;
            if (s->out->len + (size_t)len > s->max_out) return -1;
            if (fb_buf_reserve(s->out, (size_t)len)) return -1;
            for (int i = 0; i < len; i++) {
                s->out->data[s->out->len] = s->out->data[s->out->len - dist];
                s->out->len++;
            }
            s->out->data[s->out->len] = 0;
        }
    }
}

static int stored_block(State *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return -1;
    unsigned len = s->in[s->incnt] | (unsigned)s->in[s->incnt + 1] << 8;
    unsigned nlen = s->in[s->incnt + 2] | (unsigned)s->in[s->incnt + 3] << 8;
    s->incnt += 4;
    if (len != (~nlen & 0xFFFF)) return -1;
    if (s->incnt + len > s->inlen) return -1;
    if (s->out->len + len > s->max_out) return -1;
    if (fb_buf_append(s->out, s->in + s->incnt, len)) return -1;
    s->incnt += len;
    return 0;
}

static int fixed_block(State *s)
{
    static int virgin = 1;
    static Huffman lencode, distcode;
    if (virgin) {
        short lengths[FIXLCODES];
        int sym;
        for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < 30; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, 30);
        virgin = 0;
    }
    return codes(s, &lencode, &distcode);
}

static int dynamic_block(State *s)
{
    static const short order[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
    short lengths[320];
    Huffman lencode, distcode;
    int nlen = getbits(s, 5) + 257;
    int ndist = getbits(s, 5) + 1;
    int ncode = getbits(s, 4) + 4;
    if (s->err || nlen > 286 || ndist > 30) return -1;
    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (short)getbits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (s->err) return -1;
    if (construct(&lencode, lengths, 19) != 0) return -1;
    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lencode);
        if (s->err || sym < 0) return -1;
        if (sym < 16) lengths[index++] = (short)sym;
        else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) return -1;
                len = lengths[index - 1];
                rep = 3 + getbits(s, 2);
            } else if (sym == 17) rep = 3 + getbits(s, 3);
            else rep = 11 + getbits(s, 7);
            if (s->err || index + rep > nlen + ndist) return -1;
            while (rep--) lengths[index++] = (short)len;
        }
    }
    if (lengths[256] == 0) return -1;
    int err = construct(&lencode, lengths, nlen);
    if (err && (err < 0 || nlen != lencode.count[0] + lencode.count[1])) return -1;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err && (err < 0 || ndist != distcode.count[0] + distcode.count[1])) return -1;
    return codes(s, &lencode, &distcode);
}

int fb_inflate(const unsigned char *in, size_t inlen, FbBuf *out, size_t max_out)
{
    State s;
    memset(&s, 0, sizeof s);
    s.in = in; s.inlen = inlen; s.out = out; s.max_out = max_out;
    int last;
    do {
        last = getbits(&s, 1);
        int type = getbits(&s, 2);
        if (s.err) return -1;
        int rc;
        if (type == 0) rc = stored_block(&s);
        else if (type == 1) rc = fixed_block(&s);
        else if (type == 2) rc = dynamic_block(&s);
        else rc = -1;
        if (rc) return -1;
    } while (!last);
    return 0;
}

/* ============================================================== zip reader */

static uint16_t rd16(const unsigned char *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

void fbzip_close(FbZip *z)
{
    for (size_t i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    free(z->data);
    memset(z, 0, sizeof *z);
}

int fbzip_open_mem(FbZip *z, unsigned char *data, size_t size, char *err, size_t errsz)
{
    memset(z, 0, sizeof *z);
    z->data = data;
    z->size = size;
    if (size < 22) { fb_set_err(err, errsz, "not a zip/xlsx file (too small)"); goto bad; }
    size_t i = size - 22;
    size_t lo = size > 65557 ? size - 65557 : 0;
    for (;; i--) {
        if (rd32(data + i) == 0x06054b50u) break;
        if (i == lo) { fb_set_err(err, errsz, "not a zip/xlsx file (no end record)"); goto bad; }
    }
    uint16_t total = rd16(data + i + 10);
    uint32_t cdsize = rd32(data + i + 12), cdoff = rd32(data + i + 16);
    if (total == 0xFFFF || cdsize == 0xFFFFFFFFu || cdoff == 0xFFFFFFFFu) {
        fb_set_err(err, errsz, "zip64 archives are not supported"); goto bad;
    }
    if ((size_t)cdoff + cdsize > size) { fb_set_err(err, errsz, "corrupt zip directory"); goto bad; }
    z->e = (FbZipEntry *)calloc(total ? total : 1, sizeof(FbZipEntry));
    if (!z->e) { fb_set_err(err, errsz, "out of memory"); goto bad; }
    size_t p = cdoff;
    for (uint16_t k = 0; k < total; k++) {
        if (p + 46 > size || rd32(data + p) != 0x02014b50u) { fb_set_err(err, errsz, "corrupt zip directory entry"); goto bad; }
        FbZipEntry *e = &z->e[z->n];
        e->flags = rd16(data + p + 8);
        e->method = rd16(data + p + 10);
        e->mtime = rd16(data + p + 12);
        e->mdate = rd16(data + p + 14);
        e->crc = rd32(data + p + 16);
        e->csize = rd32(data + p + 20);
        e->usize = rd32(data + p + 24);
        uint16_t nl = rd16(data + p + 28), xl = rd16(data + p + 30), cl = rd16(data + p + 32);
        e->lho = rd32(data + p + 42);
        if (p + 46 + nl > size) { fb_set_err(err, errsz, "corrupt zip entry name"); goto bad; }
        e->name = fb_strndup((const char *)data + p + 46, nl);
        if (!e->name) { fb_set_err(err, errsz, "out of memory"); goto bad; }
        z->n++;
        p += 46u + nl + xl + cl;
    }
    return 0;
bad:
    fbzip_close(z);
    return -1;
}

int fbzip_open(FbZip *z, const char *path, char *err, size_t errsz)
{
    FbBuf b;
    fb_buf_init(&b);
    if (fb_read_file(path, &b)) {
        fb_buf_free(&b);
        fb_set_err(err, errsz, "cannot read file: %s", path);
        memset(z, 0, sizeof *z);
        return -1;
    }
    size_t n = b.len;
    unsigned char *d = (unsigned char *)fb_buf_detach(&b);
    return fbzip_open_mem(z, d, n, err, errsz);
}

int fbzip_find(const FbZip *z, const char *name)
{
    for (size_t i = 0; i < z->n; i++)
        if (!strcmp(z->e[i].name, name)) return (int)i;
    return -1;
}

int fbzip_raw(const FbZip *z, size_t idx, const unsigned char **p, size_t *len)
{
    const FbZipEntry *e = &z->e[idx];
    size_t lh = e->lho;
    if (lh + 30 > z->size || rd32(z->data + lh) != 0x04034b50u) return -1;
    size_t start = lh + 30u + rd16(z->data + lh + 26) + rd16(z->data + lh + 28);
    if (start + e->csize > z->size) return -1;
    *p = z->data + start;
    *len = e->csize;
    return 0;
}

int fbzip_extract(const FbZip *z, size_t idx, FbBuf *out, char *err, size_t errsz)
{
    const FbZipEntry *e = &z->e[idx];
    const unsigned char *raw;
    size_t rl;
    if (fbzip_raw(z, idx, &raw, &rl)) { fb_set_err(err, errsz, "corrupt zip entry: %s", e->name); return -1; }
    size_t start = out->len;
    if (e->method == 0) {
        if (fb_buf_append(out, raw, rl)) return -1;
    } else if (e->method == 8) {
        size_t lim = (size_t)e->usize + 16;
        if (lim < 1024) lim = 1024;
        FbBuf tmp;
        fb_buf_init(&tmp);
        if (fb_inflate(raw, rl, &tmp, lim)) {
            fb_buf_free(&tmp);
            fb_set_err(err, errsz, "cannot decompress: %s", e->name);
            return -1;
        }
        fb_buf_append(out, tmp.data, tmp.len);
        fb_buf_free(&tmp);
    } else {
        fb_set_err(err, errsz, "unsupported compression method %d in %s", e->method, e->name);
        return -1;
    }
    if (fb_crc32(0, out->data + start, out->len - start) != e->crc) {
        fb_set_err(err, errsz, "checksum mismatch: %s", e->name);
        out->len = start;
        return -1;
    }
    return 0;
}

/* ============================================================== zip writer */

static void wr16(FbBuf *b, unsigned v) { unsigned char c[2] = { (unsigned char)v, (unsigned char)(v >> 8) }; fb_buf_append(b, c, 2); }
static void wr32(FbBuf *b, uint32_t v)
{
    unsigned char c[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fb_buf_append(b, c, 4);
}

void fbzw_init(FbZipWriter *w) { fb_buf_init(&w->out); fb_buf_init(&w->cd); w->count = 0; }
void fbzw_free(FbZipWriter *w) { fb_buf_free(&w->out); fb_buf_free(&w->cd); w->count = 0; }

int fbzw_add_raw(FbZipWriter *w, const char *name, uint16_t method, uint32_t crc,
                 uint32_t csize, uint32_t usize, const void *raw)
{
    size_t nl = strlen(name);
    uint32_t off = (uint32_t)w->out.len;
    const unsigned dostime = 0, dosdate = 0x21; /* 1980-01-01 */
    wr32(&w->out, 0x04034b50u);
    wr16(&w->out, 20); wr16(&w->out, 0x0800); wr16(&w->out, method);
    wr16(&w->out, dostime); wr16(&w->out, dosdate);
    wr32(&w->out, crc); wr32(&w->out, csize); wr32(&w->out, usize);
    wr16(&w->out, (unsigned)nl); wr16(&w->out, 0);
    fb_buf_append(&w->out, name, nl);
    if (csize && fb_buf_append(&w->out, raw, csize)) return -1;

    wr32(&w->cd, 0x02014b50u);
    wr16(&w->cd, 20); wr16(&w->cd, 20); wr16(&w->cd, 0x0800); wr16(&w->cd, method);
    wr16(&w->cd, dostime); wr16(&w->cd, dosdate);
    wr32(&w->cd, crc); wr32(&w->cd, csize); wr32(&w->cd, usize);
    wr16(&w->cd, (unsigned)nl); wr16(&w->cd, 0); wr16(&w->cd, 0);
    wr16(&w->cd, 0); wr16(&w->cd, 0); wr32(&w->cd, 0);
    wr32(&w->cd, off);
    fb_buf_append(&w->cd, name, nl);
    w->count++;
    return 0;
}

int fbzw_add_stored(FbZipWriter *w, const char *name, const void *data, size_t n)
{
    return fbzw_add_raw(w, name, 0, fb_crc32(0, data, n), (uint32_t)n, (uint32_t)n, data);
}

int fbzw_finish(FbZipWriter *w, FbBuf *result)
{
    uint32_t cdoff = (uint32_t)w->out.len;
    if (fb_buf_append(&w->out, w->cd.data, w->cd.len)) return -1;
    wr32(&w->out, 0x06054b50u);
    wr16(&w->out, 0); wr16(&w->out, 0);
    wr16(&w->out, (unsigned)w->count); wr16(&w->out, (unsigned)w->count);
    wr32(&w->out, (uint32_t)w->cd.len);
    wr32(&w->out, cdoff);
    wr16(&w->out, 0);
    *result = w->out;
    fb_buf_init(&w->out);
    fb_buf_free(&w->cd);
    w->count = 0;
    return 0;
}
