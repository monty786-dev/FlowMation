/* FlowBrowser - minimal ZIP reader/writer (+ inflate) used for .xlsx files. */
#ifndef FB_ZIP_H
#define FB_ZIP_H

#include "fb_util.h"

typedef struct {
    char    *name;
    uint16_t method, flags;
    uint32_t crc, csize, usize, lho;
    uint16_t mtime, mdate;
} FbZipEntry;

typedef struct {
    unsigned char *data;      /* whole archive in memory (owned) */
    size_t         size;
    FbZipEntry    *e;
    size_t         n;
} FbZip;

int  fbzip_open(FbZip *z, const char *path, char *err, size_t errsz);
int  fbzip_open_mem(FbZip *z, unsigned char *data, size_t size, char *err, size_t errsz); /* takes ownership */
void fbzip_close(FbZip *z);
int  fbzip_find(const FbZip *z, const char *name);                       /* index or -1 */
int  fbzip_extract(const FbZip *z, size_t idx, FbBuf *out, char *err, size_t errsz);
int  fbzip_raw(const FbZip *z, size_t idx, const unsigned char **p, size_t *len);

int  fb_inflate(const unsigned char *in, size_t inlen, FbBuf *out, size_t max_out);   /* raw deflate, 0 ok */

typedef struct {
    FbBuf  out;
    FbBuf  cd;
    size_t count;
} FbZipWriter;

void fbzw_init(FbZipWriter *w);
void fbzw_free(FbZipWriter *w);
int  fbzw_add_stored(FbZipWriter *w, const char *name, const void *data, size_t n);
int  fbzw_add_raw(FbZipWriter *w, const char *name, uint16_t method, uint32_t crc,
                  uint32_t csize, uint32_t usize, const void *raw);
int  fbzw_finish(FbZipWriter *w, FbBuf *result);   /* result receives the archive bytes */

#endif
