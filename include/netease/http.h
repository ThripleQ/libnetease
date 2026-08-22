#ifndef NE_HTTP_H
#define NE_HTTP_H
#include <stddef.h>

/* libcurl wrapper: one-shot POST (form-urlencoded) / GET with manual cookie
 * management. zlib/gzip response decoding is handled by libcurl
 * (CURLOPT_ACCEPT_ENCODING), mirroring request.go's zlib fallback. */

typedef struct {
    long   status;      /* HTTP status, or 0 on transport error */
    char  *body;        /* malloc'd, NUL-terminated */
    size_t body_len;
    char  *err;         /* malloc'd curl error string, NULL if none */
} ne_http_resp;

void ne_http_resp_free(ne_http_resp *r);

/* POST form: body is "k=v&k2=v2" ALREADY url-encoded by the caller
 * (netease params are base64/hex — we still encode defensively in
 * ne_http_form_encode()). cookie_header may be NULL. */
ne_http_resp *ne_http_post(const char *url,
                           const char *form_body,
                           const char *cookie_header,
                           const char *user_agent,
                           const char *content_type);

ne_http_resp *ne_http_get(const char *url,
                          const char *cookie_header,
                          const char *user_agent);

/* concatenated "name=value\n" lines from Set-Cookie response headers of the
 * last ne_http_* call (single-threaded CLI usage) */
const char *ne_http_last_setcookies(void);

/* urlencoded "a=1&b=2" builder over (key,value) pairs */
char *ne_http_form_encode(const char *const *kv, size_t n_pairs);
#endif
