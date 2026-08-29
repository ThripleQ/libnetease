#ifndef NE_HTTP_H
#define NE_HTTP_H
#include <stddef.h>

/* Transport-agnostic HTTP wrapper. The default transport is libcurl,
 * installed by default. A replacement transport (e.g. OkHttp-backed on
 * Android) can be installed at runtime with ne_http_set_transport() so the
 * library itself never hard-depends on a given network/TLS stack. Every
 * transport returns the same ne_http_resp shape; gzip/zlib decoding and
 * redirects are the transport's responsibility (curl does them via
 * CURLOPT_ACCEPT_ENCODING / FOLLOWLOCATION), mirroring request.go. */

typedef struct {
    long   status;      /* HTTP status, or 0 on transport error */
    char  *body;        /* malloc'd, NUL-terminated */
    size_t body_len;
    char  *err;         /* malloc'd error string, NULL if none */
    char  *set_cookies; /* owned "name=value\n" lines from Set-Cookie, or NULL */
} ne_http_resp;

void ne_http_resp_free(ne_http_resp *r);

/* A transport implements a single "do a request, get a response" primitive.
 * method is "POST" or "GET" (body/content_type only used for POST);
 * cookie_header may be NULL. Always returns a malloc'd resp, never NULL.
 * Set-Cookie response headers MUST be captured by the transport into
 * resp->set_cookies as "name=value\n" lines — the request layer consumes them
 * directly from the response object (no thread-local back-channel). */
typedef ne_http_resp *(*ne_transport_req_fn)(
    const char *url, const char *method,
    const char *body, const char *content_type,
    const char *cookie_header, const char *user_agent);

typedef struct {
    ne_transport_req_fn request;
} ne_http_transport;

/* Install a custom transport; NULL restores the default (curl). Must be
 * called before any request is made. */
void ne_http_set_transport(const ne_http_transport *t);

const ne_http_transport *ne_http_get_transport(void);

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

/* urlencoded "a=1&b=2" builder over (key,value) pairs (transport-independent) */
char *ne_http_form_encode(const char *const *kv, size_t n_pairs);
#endif