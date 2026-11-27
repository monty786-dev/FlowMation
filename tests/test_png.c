/* writes a gradient PNG to argv[1] (checked by tests/test_png.py) */
#include <stdio.h>
#include <stdlib.h>
#include "fb_png.h"
int main(int argc, char **argv)
{
    int w = 300, h = 220;             /* big enough for several stored blocks */
    if (argc < 2) return 2;
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned char *p = px + ((size_t)y * w + x) * 4;
        p[0] = (unsigned char)(x);        /* B */
        p[1] = (unsigned char)(y);        /* G */
        p[2] = (unsigned char)(x + y);    /* R */
        p[3] = 255;
    }
    FbBuf out; fb_buf_init(&out);
    if (fbpng_encode_bgra(px, w, h, w * 4, &out)) { puts("encode failed"); return 1; }
    FILE *f = fopen(argv[1], "wb"); fwrite(out.data, 1, out.len, f); fclose(f);
    printf("png ok %zu bytes\n", out.len);
    free(px); fb_buf_free(&out);
    return 0;
}
