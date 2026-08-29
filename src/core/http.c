/* Transport-agnostic HTTP layer. The default transport is libcurl, compiled
 * in only when NE_HAVE_CURL is set (the desktop builds). On a platform
 * without curl (e.g. Android NDK) no default transport is present; the host
 * app installs one via ne_http_set_transport() — typically an OkHttp-backed
 * transport reached over JNI. Either way the request layer is untouched.
 *
 * Set-Cookie handling: each transport captures Set-Cookie response headers
 * into resp->set_cookies; the request layer reads them straight off the
 * response object, so there is no shared/global cookie back-channel here. */
#include "netease/http.h"
#include "netease/util.h"
#include <stdio.h>
#include <string.h>

#ifdef NE_HAVE_CURL
#include <curl/curl.h>
#endif

/* ── default libcurl transport ────────────────────────── */
#ifdef NE_HAVE_CURL

struct body_buf { char *p; size_t len, cap; };

static void body_append(struct body_buf *b, const char *data, size_t n) {
    while (b->len + n + 1 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 4096;
        b->p = ne_xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    body_append((struct body_buf *)ud, ptr, size * nmemb);
    return size * nmemb;
}

/* Collect Set-Cookie headers into concatenated "name=value; ...\n" lines so
 * the transport can hand them back via resp->set_cookies. */
static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    struct body_buf *setcookies = (struct body_buf *)ud;
    if (n > 12 && strncasecmp(ptr, "Set-Cookie:", 11) == 0) {
        const char *v = ptr + 11;
        size_t vn = n - 11;
        while (vn > 0 && (*v == ' ' || *v == '\t')) { v++; vn--; }
        /* strip trailing \r\n */
        while (vn > 0 && (v[vn - 1] == '\n' || v[vn - 1] == '\r')) vn--;
        body_append(setcookies, v, vn);
        body_append(setcookies, "\n", 1);
    }
    return n;
}

static ne_http_resp *curl_request(const char *url, const char *method,
                                  const char *body, const char *content_type,
                                  const char *cookie_header,
                                  const char *user_agent) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct body_buf body_b = {0}, setcookies = {0};
    ne_http_resp *r = ne_xmalloc(sizeof(ne_http_resp));
    memset(r, 0, sizeof *r);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (getenv("NE_DEBUG_HTTP")) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body_b);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &setcookies);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");   /* zlib/gzip auto */
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist *hdrs = NULL;
    if (user_agent) {
        char ua_hdr[512];
        snprintf(ua_hdr, sizeof ua_hdr, "User-Agent: %s", user_agent);
        hdrs = curl_slist_append(hdrs, ua_hdr);
    }
    if (body && strcmp(method, "POST") == 0) {
        char ct_hdr[256];
        snprintf(ct_hdr, sizeof ct_hdr, "Content-Type: %s",
                 content_type ? content_type : "application/x-www-form-urlencoded");
        hdrs = curl_slist_append(hdrs, ct_hdr);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    }
    /* Go requests.NewRequest sets Referer unconditionally (CallWeapi path);
     * CreateRequest sets it for music.163.com — netease URLs always are,
     * so unconditional matches both. */
    hdrs = curl_slist_append(hdrs, "Referer: https://music.163.com");
    if (cookie_header && *cookie_header) {
        char ck_hdr[8192];
        snprintf(ck_hdr, sizeof ck_hdr, "Cookie: %s", cookie_header);
        hdrs = curl_slist_append(hdrs, ck_hdr);
    }
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        r->err = ne_xstrdup(curl_easy_strerror(rc));
        r->status = 0;
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        r->status = code;
    }
    r->body = body_b.p ? body_b.p : ne_xstrdup("");
    r->body_len = body_b.len;
    r->set_cookies = setcookies.p ? setcookies.p : NULL;

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

static const ne_http_transport g_curl_transport = { curl_request };
#endif /* NE_HAVE_CURL */

/* ── installed-transport dispatch + thread-local set-cookie sink ── */

static const ne_http_transport *g_transport = NULL;   /* NULL → default */

static const ne_http_transport *g_default_transport(void) {
#ifdef NE_HAVE_CURL
    return &g_curl_transport;
#else
    return NULL;
#endif
}

void ne_http_set_transport(const ne_http_transport *t) { g_transport = t; }
const ne_http_transport *ne_http_get_transport(void) {
    return g_transport ? g_transport : g_default_transport();
}

void ne_http_resp_free(ne_http_resp *r) {
    if (!r) return;
    free(r->body);
    free(r->err);
    free(r->set_cookies);
    free(r);
}

static ne_http_resp *do_request(const char *url, const char *method,
                                const char *body, const char *content_type,
                                const char *cookie_header,
                                const char *user_agent) {
    const ne_http_transport *t = g_transport ? g_transport : g_default_transport();
    if (!t || !t->request) {
        /* no transport (built without curl and none injected) — fail cleanly
         * instead of crashing */
        ne_http_resp *r = ne_xmalloc(sizeof(ne_http_resp));
        memset(r, 0, sizeof *r);
        r->status = 0;
        r->body = ne_xstrdup("");
        r->err = ne_xstrdup("no http transport installed");
        return r;
    }
    return t->request(url, method, body, content_type, cookie_header, user_agent);
}

ne_http_resp *ne_http_post(const char *url, const char *form_body,
                           const char *cookie_header, const char *user_agent,
                           const char *content_type) {
    return do_request(url, "POST", form_body, content_type,
                      cookie_header, user_agent);
}

ne_http_resp *ne_http_get(const char *url, const char *cookie_header,
                          const char *user_agent) {
    return do_request(url, "GET", NULL, NULL, cookie_header, user_agent);
}

/* ── urlencoding (transport-independent; was curl_easy_escape) ── */

static int url_unreserved(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
           (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
           ch == '.' || ch == '~';
}

static void url_append_escaped(char *dst, size_t *len,
                               const unsigned char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *s; s++) {
        if (url_unreserved(*s)) {
            dst[(*len)++] = (char)*s;
        } else {
            dst[(*len)++] = '%';
            dst[(*len)++] = hex[*s >> 4];
            dst[(*len)++] = hex[*s & 0xF];
        }
    }
}

char *ne_http_form_encode(const char *const *kv, size_t n_pairs) {
    size_t cap = 256, len = 0;
    char *out = ne_xmalloc(cap);
    out[0] = '\0';
    for (size_t i = 0; i < n_pairs; i++) {
        const char *k = kv[2 * i], *v = kv[2 * i + 1];
        /* worst case: 3x each byte of key/value, '=', and '&' or '\0' */
        size_t need = (strlen(k) + strlen(v)) * 3 + 2 + (len ? 1 : 0);
        while (len + need + 1 > cap) { cap *= 2; out = ne_xrealloc(out, cap); }
        if (len) out[len++] = '&';
        url_append_escaped(out, &len, (const unsigned char *)k);
        out[len++] = '=';
        url_append_escaped(out, &len, (const unsigned char *)v);
        out[len] = '\0';
    }
    return out;
}