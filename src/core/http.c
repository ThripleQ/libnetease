/* libcurl wrapper — request.go's transport equivalent. */
#include "netease/http.h"
#include "netease/util.h"
#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

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

/* parse Set-Cookie headers from the raw header block into jar-form lines;
 * the request layer feeds these to ne_jar via ne_jar_merge_cookie_str on
 * each "Set-Cookie:" header line. Handled by the caller through the
 * header callback below (kept simple: full header text returned in
 * resp->body only; cookies are extracted in http_post_common). */
struct resp_meta {
    struct body_buf body;
    struct body_buf setcookies;   /* concatenated "name=value; ...\n" */
};

static size_t header_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
    size_t n = size * nmemb;
    struct resp_meta *m = ud;
    if (n > 12 && strncasecmp(ptr, "Set-Cookie:", 11) == 0) {
        const char *v = ptr + 11;
        size_t vn = n - 11;
        while (vn > 0 && (*v == ' ' || *v == '\t')) { v++; vn--; }
        /* strip trailing \r\n */
        while (vn > 0 && (v[vn - 1] == '\n' || v[vn - 1] == '\r')) vn--;
        body_append(&m->setcookies, v, vn);
        body_append(&m->setcookies, "\n", 1);
    }
    return n;
}

void ne_http_resp_free(ne_http_resp *r) {
    if (!r) return;
    free(r->body);
    free(r->err);
    free(r);
}

static ne_http_resp *do_request(const char *url, const char *post_body,
                                const char *cookie_header,
                                const char *user_agent,
                                const char *content_type) {
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    struct resp_meta m = {{0}, {0}};
    ne_http_resp *r = ne_xmalloc(sizeof(ne_http_resp));
    memset(r, 0, sizeof *r);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (getenv("NE_DEBUG_HTTP")) curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &m.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &m);
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
    if (post_body) {
        char ct_hdr[256];
        snprintf(ct_hdr, sizeof ct_hdr, "Content-Type: %s",
                 content_type ? content_type : "application/x-www-form-urlencoded");
        hdrs = curl_slist_append(hdrs, ct_hdr);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
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
    r->body = m.body.p ? m.body.p : ne_xstrdup("");
    r->body_len = m.body.len;

    /* expose set-cookies through a hidden channel: caller re-fetches via
     * ne_http_last_setcookies (single-threaded CLI, acceptable) */
    {
        extern __thread char *g_last_setcookies;
        free(g_last_setcookies);
        g_last_setcookies = m.setcookies.p ? m.setcookies.p : ne_xstrdup("");
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return r;
}

/* thread-local set-cookie sink (single-threaded CLI usage) */
__thread char *g_last_setcookies;

const char *ne_http_last_setcookies(void) { return g_last_setcookies; }

ne_http_resp *ne_http_post(const char *url, const char *form_body,
                           const char *cookie_header, const char *user_agent,
                           const char *content_type) {
    return do_request(url, form_body, cookie_header, user_agent, content_type);
}

ne_http_resp *ne_http_get(const char *url, const char *cookie_header,
                          const char *user_agent) {
    return do_request(url, NULL, cookie_header, user_agent, NULL);
}

char *ne_http_form_encode(const char *const *kv, size_t n_pairs) {
    size_t cap = 256, len = 0;
    char *out = ne_xmalloc(cap);
    out[0] = '\0';
    for (size_t i = 0; i < n_pairs; i++) {
        char *ek = curl_easy_escape(NULL, kv[2 * i], 0);
        char *ev = curl_easy_escape(NULL, kv[2 * i + 1], 0);
        if (!ek || !ev) { free(ek); free(ev); free(out); return NULL; }
        size_t add = strlen(ek) + strlen(ev) + 2 + (len ? 1 : 0);
        while (len + add + 1 > cap) { cap *= 2; out = ne_xrealloc(out, cap); }
        len += (size_t)snprintf(out + len, cap - len, "%s%s=%s", len ? "&" : "", ek, ev);
        curl_free(ek); curl_free(ev);
    }
    return out;
}
