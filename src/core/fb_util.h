/* FlowBrowser - portable utility layer (buffers, files, base64, time, xml escaping). */
#ifndef FB_UTIL_H
#define FB_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- growable byte/string buffer (always NUL terminated once non-empty) ---- */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} FbBuf;

void        fb_buf_init(FbBuf *b);
void        fb_buf_free(FbBuf *b);
void        fb_buf_clear(FbBuf *b);
int         fb_buf_reserve(FbBuf *b, size_t extra);
int         fb_buf_append(FbBuf *b, const void *d, size_t n);
int         fb_buf_puts(FbBuf *b, const char *s);
int         fb_buf_putc(FbBuf *b, char c);
int         fb_buf_printf(FbBuf *b, const char *fmt, ...);
const char *fb_buf_cstr(FbBuf *b);            /* never NULL */
char       *fb_buf_detach(FbBuf *b);          /* caller frees; buffer reset */

/* ---- strings ---- */
char *fb_strdup(const char *s);
char *fb_strndup(const char *s, size_t n);
int   fb_stricmp(const char *a, const char *b);
int   fb_starts_with(const char *s, const char *prefix);
int   fb_ends_with(const char *s, const char *suffix);
char *fb_trim(char *s);                       /* trims in place, returns pointer into s */
void  fb_set_err(char *err, size_t n, const char *fmt, ...);
int   fb_utf8_encode(unsigned cp, char out[4]);

/* ---- files (paths are UTF-8 on every platform) ---- */
FILE *fb_fopen(const char *path_utf8, const char *mode);
int   fb_read_file(const char *path, FbBuf *out);               /* 0 ok */
int   fb_write_file(const char *path, const void *data, size_t n); /* atomic replace, 0 ok */
int   fb_file_exists(const char *path);
int   fb_copy_file(const char *from, const char *to);
int   fb_delete_file(const char *path);
int   fb_mkdirs(const char *dir);                               /* mkdir -p */
int   fb_ensure_parent_dir(const char *file_path);
typedef int (*FbDirCb)(const char *name, int is_dir, void *ud); /* return non-zero to stop */
int   fb_list_dir(const char *dir, FbDirCb cb, void *ud);
int   fb_is_absolute_path(const char *p);
void  fb_join_path(FbBuf *out, const char *dir, const char *name);

/* ---- encoding ---- */
void   fb_base64_encode(const void *data, size_t n, FbBuf *out);
int    fb_base64_decode(const char *s, size_t n, FbBuf *out);   /* 0 ok */
uint32_t fb_crc32(uint32_t crc, const void *data, size_t n);    /* crc starts at 0 */
void   fb_xml_escape(FbBuf *out, const char *s, size_t n);
void   fb_xml_unescape(FbBuf *out, const char *s, size_t n);

/* ---- time ---- */
uint64_t fb_now_ms(void);                                       /* monotonic */
void     fb_sleep_ms(unsigned ms);

#endif
