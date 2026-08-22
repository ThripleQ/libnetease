#ifndef NE_UTIL_H
#define NE_UTIL_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* checked malloc — aborts on OOM (CLI tool, fail-loud is fine) */
static inline void *ne_xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}
static inline void *ne_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) abort();
    return q;
}
static inline char *ne_xstrdup(const char *s) {
    char *d = ne_xmalloc(strlen(s) + 1);
    strcpy(d, s);
    return d;
}

/* millisecond / second unix timestamps */
int64_t ne_now_ms(void);
int64_t ne_now_unix(void);
#endif
