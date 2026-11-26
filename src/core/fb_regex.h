/* FlowBrowser - small portable regular-expression engine (backtracking VM, UTF-8 aware, bounded work).
   Syntax: literals . [] [^] \d \w \s \D \W \S \b \B ^ $ | () (?:) * + ? {n} {n,} {n,m} (+ lazy "?" forms),
   escapes \n \t \r \xHH \uHHHH. Not supported (reported as errors): back-references, look-around. */
#ifndef FB_REGEX_H
#define FB_REGEX_H

#include <stddef.h>

#define FBRE_ICASE     1
#define FBRE_MULTILINE 2
#define FBRE_DOTALL    4

typedef struct FbRegex FbRegex;

FbRegex *fbre_compile(const char *pattern, int flags, char *err, size_t errsz);
void     fbre_free(FbRegex *re);
int      fbre_ngroups(const FbRegex *re);
/* "i" "m" "s" (any order; "g" ignored) -> FBRE_* flags */
int      fbre_flags_from_string(const char *f);
/* Search from byte offset `from`. caps has 2*(ngroups+1) ints: [start,end) byte offsets, -1 = group did not take part.
   Returns 1 = match, 0 = no match, -1 = gave up (pattern too expensive for this input). */
int      fbre_search(const FbRegex *re, const char *s, size_t len, size_t from, int *caps);

#endif
