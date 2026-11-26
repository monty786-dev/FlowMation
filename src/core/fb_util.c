#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L   /* clock_gettime, nanosleep, fileno ... under -std=c99 */
#endif
#include "fb_util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <time.h>
#  include <unistd.h>
#endif

/* ------------------------------------------------------------------ buffer */

void fb_buf_init(FbBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
void fb_buf_free(FbBuf *b) { free(b->data); fb_buf_init(b); }
void fb_buf_clear(FbBuf *b) { b->len = 0; if (b->data) b->data[0] = 0; }

int fb_buf_reserve(FbBuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return 0;
    size_t nc = b->cap ? b->cap : 64;
    while (nc < need) {
        if (nc > ((size_t)-1) / 2) return -1;
        nc *= 2;
    }
    char *p = (char *)realloc(b->data, nc);
    if (!p) return -1;
    b->data = p;
    b->cap = nc;
    return 0;
}

int fb_buf_append(FbBuf *b, const void *d, size_t n)
{
    if (fb_buf_reserve(b, n)) return -1;
    if (n) memcpy(b->data + b->len, d, n);
    b->len += n;
    b->data[b->len] = 0;
    return 0;
}

int fb_buf_puts(FbBuf *b, const char *s) { return fb_buf_append(b, s, strlen(s)); }
int fb_buf_putc(FbBuf *b, char c) { return fb_buf_append(b, &c, 1); }

int fb_buf_printf(FbBuf *b, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return -1; }
    if (fb_buf_reserve(b, (size_t)n)) { va_end(ap2); return -1; }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 0;
}

const char *fb_buf_cstr(FbBuf *b)
{
    if (!b->data) { fb_buf_reserve(b, 0); if (b->data) b->data[0] = 0; }
    return b->data ? b->data : "";
}

char *fb_buf_detach(FbBuf *b)
{
    char *p = b->data;
    if (!p) { p = (char *)malloc(1); if (p) p[0] = 0; }
    fb_buf_init(b);
    return p;
}

/* ----------------------------------------------------------------- strings */

char *fb_strndup(const char *s, size_t n)
{
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}
char *fb_strdup(const char *s) { return fb_strndup(s ? s : "", s ? strlen(s) : 0); }

int fb_stricmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        if (!ca) return 0;
    }
}
int fb_starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
int fb_ends_with(const char *s, const char *x)
{
    size_t a = strlen(s), b = strlen(x);
    return a >= b && memcmp(s + a - b, x, b) == 0;
}

char *fb_trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

void fb_set_err(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

int fb_utf8_encode(unsigned cp, char out[4])
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F)); return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* ------------------------------------------------------------------- files */

#ifdef _WIN32
static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
static char *from_wide(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc((size_t)n);
    if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}
#endif

FILE *fb_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wp = to_wide(path), *wm = to_wide(mode);
    FILE *f = NULL;
    if (wp && wm) f = _wfopen(wp, wm);
    free(wp); free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

int fb_read_file(const char *path, FbBuf *out)
{
    FILE *f = fb_fopen(path, "rb");
    if (!f) return -1;
    char tmp[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) {
        if (fb_buf_append(out, tmp, n)) { rc = -1; break; }
    }
    if (ferror(f)) rc = -1;
    fclose(f);
    if (!out->data && fb_buf_reserve(out, 0) == 0) out->data[0] = 0;
    return rc;
}

static int replace_file(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t *a = to_wide(from), *b = to_wide(to);
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    free(a); free(b);
    return ok ? 0 : -1;
#else
    return rename(from, to);
#endif
}

int fb_delete_file(const char *path)
{
#ifdef _WIN32
    wchar_t *w = to_wide(path);
    int ok = w && DeleteFileW(w);
    free(w);
    return ok ? 0 : -1;
#else
    return remove(path);
#endif
}

int fb_write_file(const char *path, const void *data, size_t n)
{
    size_t pl = strlen(path);
    char *tmp = (char *)malloc(pl + 8);
    if (!tmp) return -1;
    memcpy(tmp, path, pl);
    memcpy(tmp + pl, ".fbtmp", 7);
    FILE *f = fb_fopen(tmp, "wb");
    if (!f) { free(tmp); return -1; }
    int rc = 0;
    if (n && fwrite(data, 1, n, f) != n) rc = -1;
    if (fflush(f) != 0) rc = -1;
    if (fclose(f) != 0) rc = -1;
    if (rc == 0) rc = replace_file(tmp, path);
    if (rc != 0) fb_delete_file(tmp);
    free(tmp);
    return rc;
}

int fb_file_exists(const char *path)
{
    FILE *f = fb_fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int fb_copy_file(const char *from, const char *to)
{
    FbBuf b;
    fb_buf_init(&b);
    int rc = fb_read_file(from, &b);
    if (rc == 0) rc = fb_write_file(to, b.data ? b.data : "", b.len);
    fb_buf_free(&b);
    return rc;
}

static int make_one_dir(const char *p)
{
#ifdef _WIN32
    wchar_t *w = to_wide(p);
    int r = w ? _wmkdir(w) : -1;
    free(w);
#else
    int r = mkdir(p, 0777);
#endif
    return (r == 0 || errno == EEXIST) ? 0 : -1;
}

int fb_mkdirs(const char *dir)
{
    if (!dir || !*dir) return 0;
    char *p = fb_strdup(dir);
    if (!p) return -1;
    size_t n = strlen(p);
    while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = 0;
    for (size_t i = 1; i < n; i++) {
        if ((p[i] == '/' || p[i] == '\\') && p[i - 1] != ':') {
            char c = p[i];
            p[i] = 0;
            make_one_dir(p);
            p[i] = c;
        }
    }
    int rc = make_one_dir(p);
    free(p);
    return rc;
}

int fb_ensure_parent_dir(const char *file_path)
{
    char *p = fb_strdup(file_path);
    if (!p) return -1;
    char *s1 = strrchr(p, '/'), *s2 = strrchr(p, '\\');
    char *s = s1 > s2 ? s1 : s2;
    int rc = 0;
    if (s && s != p) { *s = 0; rc = fb_mkdirs(p); }
    free(p);
    return rc;
}

int fb_list_dir(const char *dir, FbDirCb cb, void *ud)
{
#ifdef _WIN32
    FbBuf pat;
    fb_buf_init(&pat);
    fb_buf_puts(&pat, dir);
    fb_buf_puts(&pat, "\\*");
    wchar_t *w = to_wide(fb_buf_cstr(&pat));
    fb_buf_free(&pat);
    if (!w) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char *u = from_wide(fd.cFileName);
        if (!u) continue;
        int stop = cb(u, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, ud);
        free(u);
        if (stop) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        FbBuf full;
        fb_buf_init(&full);
        fb_join_path(&full, dir, e->d_name);
        struct stat st;
        int isdir = (stat(fb_buf_cstr(&full), &st) == 0) && S_ISDIR(st.st_mode);
        fb_buf_free(&full);
        if (cb(e->d_name, isdir, ud)) break;
    }
    closedir(d);
    return 0;
#endif
}

int fb_is_absolute_path(const char *p)
{
    if (!p || !*p) return 0;
    if (p[0] == '/' || p[0] == '\\') return 1;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') return 1;
    return 0;
}

void fb_join_path(FbBuf *out, const char *dir, const char *name)
{
    size_t n = dir ? strlen(dir) : 0;
    if (n) {
        fb_buf_append(out, dir, n);
        if (dir[n - 1] != '/' && dir[n - 1] != '\\') fb_buf_putc(out, '/');
    }
    fb_buf_puts(out, name);
}

/* ---------------------------------------------------------------- encoding */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void fb_base64_encode(const void *data, size_t n, FbBuf *out)
{
    const unsigned char *s = (const unsigned char *)data;
    fb_buf_reserve(out, (n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        unsigned v = (unsigned)s[i] << 16 | (unsigned)s[i + 1] << 8 | s[i + 2];
        char q[4] = { B64[v >> 18], B64[(v >> 12) & 63], B64[(v >> 6) & 63], B64[v & 63] };
        fb_buf_append(out, q, 4);
    }
    if (i + 1 == n) {
        unsigned v = (unsigned)s[i] << 16;
        char q[4] = { B64[v >> 18], B64[(v >> 12) & 63], '=', '=' };
        fb_buf_append(out, q, 4);
    } else if (i + 2 == n) {
        unsigned v = (unsigned)s[i] << 16 | (unsigned)s[i + 1] << 8;
        char q[4] = { B64[v >> 18], B64[(v >> 12) & 63], B64[(v >> 6) & 63], '=' };
        fb_buf_append(out, q, 4);
    }
}

int fb_base64_decode(const char *s, size_t n, FbBuf *out)
{
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=') break;
        else if (isspace((unsigned char)c)) continue;
        else return -1;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            char byte = (char)((acc >> bits) & 0xFF);
            if (fb_buf_append(out, &byte, 1)) return -1;
        }
    }
    return 0;
}

uint32_t fb_crc32(uint32_t crc, const void *data, size_t n)
{
    static uint32_t table[256];
    static int init = 0;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = 1;
    }
    const unsigned char *p = (const unsigned char *)data;
    crc = ~crc;
    while (n--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void fb_xml_escape(FbBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '&': fb_buf_puts(out, "&amp;"); break;
        case '<': fb_buf_puts(out, "&lt;"); break;
        case '>': fb_buf_puts(out, "&gt;"); break;
        case '"': fb_buf_puts(out, "&quot;"); break;
        default:
            /* drop characters that are illegal in XML 1.0 */
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') break;
            fb_buf_putc(out, (char)c);
        }
    }
}

void fb_xml_unescape(FbBuf *out, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') { fb_buf_putc(out, s[i]); continue; }
        const char *semi = memchr(s + i, ';', n - i > 12 ? 12 : n - i);
        if (!semi) { fb_buf_putc(out, s[i]); continue; }
        size_t l = (size_t)(semi - (s + i)) - 1;
        const char *e = s + i + 1;
        if (l == 3 && !memcmp(e, "amp", 3)) fb_buf_putc(out, '&');
        else if (l == 2 && !memcmp(e, "lt", 2)) fb_buf_putc(out, '<');
        else if (l == 2 && !memcmp(e, "gt", 2)) fb_buf_putc(out, '>');
        else if (l == 4 && !memcmp(e, "quot", 4)) fb_buf_putc(out, '"');
        else if (l == 4 && !memcmp(e, "apos", 4)) fb_buf_putc(out, '\'');
        else if (l >= 2 && e[0] == '#') {
            unsigned long cp = (e[1] == 'x' || e[1] == 'X') ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
            char u[4];
            int k = fb_utf8_encode((unsigned)cp, u);
            fb_buf_append(out, u, (size_t)k);
        } else { fb_buf_putc(out, '&'); continue; }
        i += l + 1;
    }
}

/* -------------------------------------------------------------------- time */

uint64_t fb_now_ms(void)
{
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

void fb_sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}
