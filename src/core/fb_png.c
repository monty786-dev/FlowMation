#include <stdlib.h>
#include "fb_png.h"

static void be32(FbBuf *b, uint32_t v)
{
    unsigned char c[4] = { (unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v };
    fb_buf_append(b, c, 4);
}

static void chunk(FbBuf *out, const char *type, const unsigned char *data, size_t n)
{
    be32(out, (uint32_t)n);
    size_t start = out->len;
    fb_buf_append(out, type, 4);
    if (n) fb_buf_append(out, data, n);
    be32(out, fb_crc32(0, out->data + start, out->len - start));
}

int fbpng_encode_bgra(const unsigned char *px, int w, int h, int stride, FbBuf *out)
{
    if (!px || w <= 0 || h <= 0 || w > 32000 || h > 32000 || stride < w * 4) return 1;
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    size_t row = (size_t)w * 3 + 1;                      /* filter byte + RGB */
    size_t raw = row * (size_t)h;
    unsigned char *img = (unsigned char *)malloc(raw);
    if (!img) return 1;
    for (int y = 0; y < h; y++) {
        unsigned char *d = img + row * (size_t)y;
        const unsigned char *s = px + (size_t)stride * (size_t)y;
        *d++ = 0;
        for (int x = 0; x < w; x++, s += 4) { *d++ = s[2]; *d++ = s[1]; *d++ = s[0]; }
    }
    /* zlib stream: header, stored blocks of at most 65535 bytes, Adler-32 */
    FbBuf z;
    fb_buf_init(&z);
    fb_buf_putc(&z, 0x78); fb_buf_putc(&z, 0x01);
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw; ) {
        size_t n = raw - i > 65535 ? 65535 : raw - i;
        unsigned char hd[5] = { (unsigned char)(i + n >= raw ? 1 : 0), (unsigned char)(n & 255), (unsigned char)(n >> 8), (unsigned char)(~n & 255), (unsigned char)((~n >> 8) & 255) };
        fb_buf_append(&z, hd, 5);
        fb_buf_append(&z, img + i, n);
        for (size_t k = 0; k < n; k++) { a = (a + img[i + k]) % 65521; b = (b + a) % 65521; }
        i += n;
    }
    free(img);
    be32(&z, (b << 16) | a);

    unsigned char ihdr[13] = { 0 };
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16); ihdr[2] = (unsigned char)(w >> 8); ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16); ihdr[6] = (unsigned char)(h >> 8); ihdr[7] = (unsigned char)h;
    ihdr[8] = 8; ihdr[9] = 2;                            /* 8 bits, RGB */
    fb_buf_append(out, sig, 8);
    chunk(out, "IHDR", ihdr, 13);
    chunk(out, "IDAT", (const unsigned char *)z.data, z.len);
    chunk(out, "IEND", NULL, 0);
    fb_buf_free(&z);
    return 0;
}
