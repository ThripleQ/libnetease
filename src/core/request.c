/* request.go kernel port (v1.6.0). */
#include "netease/request.h"
#include "netease/cookiejar.h"
#include "netease/crypto.h"
#include "netease/deviceids.h"
#include "netease/encoding.h"
#include "netease/http.h"
#include "netease/jval.h"
#include "netease/rand.h"
#include "netease/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* chooseUserAgent("pc") from request.go */
static const char *UA_PC =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36 Edg/124.0.0.0";

static ne_jar *g_jar = NULL;
static char   *g_cookie_path = NULL;

/* API base override — test hook only; unset in production the value is the
 * hard-coded https://music.163.com exactly like the Go build. */
static const char *api_base(void) {
    const char *b = getenv("NE_API_BASE");
    if (b && *b) {
        static char base[512];
        snprintf(base, sizeof base, "%s", b);
        /* strip trailing slash */
        size_t l = strlen(base);
        while (l > 0 && base[l - 1] == '/') base[--l] = '\0';
        return base;
    }
    return "https://music.163.com";
}

const char *ne_api_base(void) { return api_base(); }

void ne_set_cookie_file(const char *path) {
    free(g_cookie_path);
    g_cookie_path = ne_xstrdup(path);
}
const char *ne_cookie_file(void) { return g_cookie_path; }

void ne_jar_reload(void) {
    if (g_jar) ne_jar_free(g_jar);
    g_jar = ne_jar_new();
    if (g_cookie_path) ne_jar_load_file(g_jar, g_cookie_path);
    /* GetGlobalCookieJar: ensure sDeviceId exists (v1.6.0 behaviour) */
    if (!ne_jar_get(g_jar, "sDeviceId")) {
        static const char hexchars[] = "0123456789ABCDEF";
        char id[53];
        for (int i = 0; i < 52; i++) id[i] = hexchars[ne_rand_below(16)];
        id[52] = '\0';
        ne_jar_set(g_jar, "sDeviceId", id);
    }
}

static void jar_sync_from_response(void) {
    const char *sc = ne_http_last_setcookies();
    if (!sc || !*sc) return;
    char *copy = ne_xstrdup(sc);
    char *save = NULL;
    for (char *line = strtok_r(copy, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save))
        ne_jar_merge_cookie_str(g_jar, line);
    free(copy);
}

void ne_resp_free(ne_resp *r) {
    if (!r) return;
    free(r->body);
    free(r);
}

/* parse "code" as a double from a JSON body (jsonparser.GetFloat semantics:
 * missing field => 200). Extremely small scanner — the code field sits at
 * the top level; we find '"code"' followed by ':' and a number. */
static double parse_code(const char *body) {
    if (!body) return 200;
    const char *p = body;
    while ((p = strstr(p, "\"code\"")) != NULL) {
        /* ensure it is at top level: preceded by '{' or ',' with only
         * whitespace/quotes between would be complex; netease always has
         * top-level code — accept first occurrence, matching jsonparser. */
        p += 6;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ':') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            char *end;
            double v = strtod(p, &end);
            if (end != p) return v;
        }
        p += 1;
    }
    return 200;
}

static ne_resp *finish(ne_http_resp *h) {
    ne_resp *r = ne_xmalloc(sizeof(ne_resp));
    memset(r, 0, sizeof *r);
    if (!h || h->status == 0 || h->err) {
        r->code = 520;
        r->body = ne_xstrdup(h && h->err ? h->err : "transport error");
        r->err = 1;
    } else {
        jar_sync_from_response();
        r->body = ne_xstrdup(h->body ? h->body : "");
        r->body_len = h->body_len;
        r->code = parse_code(r->body);
        r->err = 0;
    }
    ne_http_resp_free(h);
    return r;
}

ne_resp *ne_call_weapi(const char *api, const jmap *data) {
    if (!g_jar) ne_jar_reload();

    ne_weapi_result enc;
    if (ne_weapi(data, &enc) != 0) {
        ne_resp *r = ne_xmalloc(sizeof(ne_resp));
        r->code = 520; r->body = ne_xstrdup("encode failed");
        r->body_len = strlen(r->body); r->err = 1;
        return r;
    }
    const char *kv[4] = { "params", enc.params, "encSecKey", enc.enc_sec_key };
    char *form = ne_http_form_encode(kv, 2);
    char *cookies = ne_jar_cookie_header(g_jar);

    ne_http_resp *h = ne_http_post(api, form, cookies, UA_PC, NULL);
    free(form); free(cookies);
    ne_weapi_free(&enc);
    ne_resp *r = finish(h);

    /* CallWeapi (request.go:375): unlike CreateRequest it VALIDATES the
     * body — must unmarshal into map[string]interface{} with a numeric
     * top-level "code", else (0, body, err). Transport error is also
     * (0, err). The song-url shell keys its fallback off this err. */
    if (r->err == 1) {
        r->code = 0;
        return r;
    }
    ne_jval *root = ne_jval_parse(r->body);
    ne_jval *code = root && ne_jval_type(root) == NE_JV_OBJ
                  ? ne_jval_get(root, "code") : NULL;
    if (code && ne_jval_type(code) == NE_JV_NUM) {
        r->code = ne_jval_num(code);
    } else {
        r->code = 0;
        r->err = 2;
    }
    ne_jval_free(root);
    return r;
}

/* RandStringRunes(16) then hex — request.go's _ntes_nuid / NMTID generator */
static char *rand_hex_of_16(void) {
    static const char letters[] =
        "1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    char raw[17];
    for (int i = 0; i < 16; i++) raw[i] = letters[ne_rand_below(62)];
    raw[16] = '\0';
    return ne_hex_lower((const uint8_t *)raw, 16);
}

/* request.go regex `/\w*api/` → "/weapi/" (or "/api/" for linuxapi). The
 * Go pattern consumes BOTH slashes (segment plus its trailing '/'), the
 * replacement restores them — so "/api/x" → "/weapi/x", exactly one slash.
 * A segment NOT followed by '/' is no match (regex needs the tail slash);
 * idempotent for URLs already carrying /weapi/. Returns malloc'd URL. */
char *ne_rewrite_api_segment(const char *url, const char *replacement) {
    const char *path = strstr(url, "://");
    path = path ? strchr(path + 3, '/') : strchr(url, '/');
    if (!path) return ne_xstrdup(url);

    /* scan path segments */
    const char *seg = path + 1;
    for (;;) {
        const char *end = seg;
        while (*end && *end != '/' && *end != '?' && *end != '#') end++;
        size_t slen = (size_t)(end - seg);
        if (slen >= 3 && *end == '/') {
            int is_word = 1;
            for (size_t i = 0; i < slen; i++) {
                char c = seg[i];
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_')) { is_word = 0; break; }
            }
            /* \w*api: segment ends with "api" */
            if (is_word && slen >= 3 &&
                strncmp(seg + slen - 3, "api", 3) == 0) {
                /* Go: match "/<seg>/" (both slashes) → replace with the
                 * replacement verbatim. Keep everything BEFORE the leading
                 * slash, then splice replacement + rest-after-consumed-slash. */
                size_t cut = (size_t)(seg - url) - 1;         /* excl. leading '/' */
                const char *rest = end + 1;                   /* consumed slash */
                size_t repl_len = strlen(replacement);        /* "/weapi/" */
                char *out = ne_xmalloc(cut + repl_len + strlen(rest) + 1);
                memcpy(out, url, cut);                        /* ...host */
                memcpy(out + cut, replacement, repl_len);     /* /weapi/ */
                strcpy(out + cut + repl_len, rest);
                return out;
            }
        }
        if (!*end) break;
        seg = end + 1;
    }
    return ne_xstrdup(url);
}

/* shared CreateRequest transport for weapi/linuxapi: cookie assembly
 * (jar + extras + __remember_me/os/appver [+ _ntes_nuid] [+ NMTID on login
 * URLs], checked against the ORIGINAL url like request.go).
 * With clean=1 the anti-fraud cookie injection is skipped and the request
 * carries only the jar cookies verbatim — this is the CallWeapi / NewRequest
 * path (request.go:375/298) used by login flows, which must NOT advertise a
 * mobile os/appver alongside a web chainId (that mismatch is what netease
 * risk-control flags on QR login). */
static ne_resp *post_common(const char *orig_url, const char *post_url,
                            const char *form, const char *ua,
                            const char *const *extra_cookies, int clean) {
    if (!g_jar) ne_jar_reload();

    const char *os = "ios";
    const char *appver_extra = NULL;
    for (size_t i = 0; extra_cookies && extra_cookies[2 * i]; i++) {
        if (strcmp(extra_cookies[2 * i], "os") == 0 && extra_cookies[2 * i + 1])
            os = extra_cookies[2 * i + 1];
        if (strcmp(extra_cookies[2 * i], "appver") == 0 && extra_cookies[2 * i + 1])
            appver_extra = extra_cookies[2 * i + 1];
    }
    /* request.go: appver = Ternary(os != "pc", "9.0.65", "") unless a cookie
     * provides one */
    const char *appver = appver_extra ? appver_extra
                      : (strcmp(os, "pc") != 0 ? "9.0.65" : "");

    ne_jar *scratch = ne_jar_new();
    char *ch = ne_jar_cookie_header(g_jar);
    ne_jar_merge_cookie_str(scratch, ch);
    free(ch);
    for (size_t i = 0; extra_cookies && extra_cookies[2 * i]; i++)
        if (extra_cookies[2 * i + 1])
            ne_jar_set(scratch, extra_cookies[2 * i], extra_cookies[2 * i + 1]);
    if (!clean) {
        ne_jar_set(scratch, "__remember_me", "true");
        ne_jar_set(scratch, "os", os);
        ne_jar_set(scratch, "appver", appver);
        if (ne_jar_get(scratch, "MUSIC_U")) {
            char *nuid = rand_hex_of_16();
            ne_jar_set(scratch, "_ntes_nuid", nuid);
            free(nuid);
        }
        if (strstr(orig_url, "login")) {
            char *nmtid = rand_hex_of_16();
            ne_jar_set(scratch, "NMTID", nmtid);   /* request-level random */
            free(nmtid);
        }
    }

    char *cookies = ne_jar_cookie_header(scratch);
    ne_http_resp *h = ne_http_post(post_url, form, cookies, ua, NULL);
    free(cookies);
    ne_jar_free(scratch);
    return finish(h);
}

ne_resp *ne_create_weapi(const char *url, jmap *data,
                         const char *const *extra_cookies) {
    if (!g_jar) ne_jar_reload();

    /* csrf_token from jar __csrf, injected pre-encryption (weapi branch) */
    const char *csrf = ne_jar_get(g_jar, "__csrf");
    jmap_put(data, "csrf_token", csrf ? csrf : "");

    ne_weapi_result enc;
    if (ne_weapi(data, &enc) != 0) {
        ne_resp *r = ne_xmalloc(sizeof(ne_resp));
        r->code = 520; r->body = ne_xstrdup("encode failed");
        r->body_len = strlen(r->body); r->err = 1;
        return r;
    }
    const char *kv[4] = { "params", enc.params, "encSecKey", enc.enc_sec_key };
    char *form = ne_http_form_encode(kv, 2);
    char *final_url = ne_rewrite_api_segment(url, "/weapi/");

    ne_resp *r = post_common(url, final_url, form, UA_PC, extra_cookies, 0);
    free(form); free(final_url);
    ne_weapi_free(&enc);
    return r;
}

/* CallWeapi-equivalent transport for login flows: no anti-fraud cookie
 * injection (no os/appver/NMTID), clean web request like request.go's
 * NewRequest path. */
ne_resp *ne_create_weapi_clean(const char *url, jmap *data) {
    if (!g_jar) ne_jar_reload();

    const char *csrf = ne_jar_get(g_jar, "__csrf");
    jmap_put(data, "csrf_token", csrf ? csrf : "");

    ne_weapi_result enc;
    if (ne_weapi(data, &enc) != 0) {
        ne_resp *r = ne_xmalloc(sizeof(ne_resp));
        r->code = 520; r->body = ne_xstrdup("encode failed");
        r->body_len = strlen(r->body); r->err = 1;
        return r;
    }
    const char *kv[4] = { "params", enc.params, "encSecKey", enc.enc_sec_key };
    char *form = ne_http_form_encode(kv, 2);
    char *final_url = ne_rewrite_api_segment(url, "/weapi/");

    ne_resp *r = post_common(url, final_url, form, UA_PC, NULL, 1);
    free(form); free(final_url);
    ne_weapi_free(&enc);
    return r;
}

/* linuxapi branch: payload {method, url(/api/-rewritten), params} encrypted
 * with the linuxapi key, POSTed to /api/linux/forward with a Linux UA. */
static const char *UA_LINUX =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/60.0.3112.90 Safari/537.36";

ne_resp *ne_call_linuxapi(const char *url, jmap *data,
                          const char *const *extra_cookies) {
    if (!g_jar) ne_jar_reload();

    char *api_url = ne_rewrite_api_segment(url, "/api/");

    jmap *outer = jmap_new();
    jmap_put(outer, "method", "POST");
    jmap_put(outer, "url", api_url);
    jmap_put_map(outer, "params", data);
    char *eparams = ne_linuxapi(outer);
    jmap_free(outer);
    free(api_url);
    if (!eparams) {
        ne_resp *r = ne_xmalloc(sizeof(ne_resp));
        r->code = 520; r->body = ne_xstrdup("encode failed");
        r->body_len = strlen(r->body); r->err = 1;
        return r;
    }

    char fwd[640];
    snprintf(fwd, sizeof fwd, "%s/api/linux/forward", api_base());
    const char *kv[2] = { "eparams", eparams };
    char *form = ne_http_form_encode(kv, 1);

    ne_resp *r = post_common(url, fwd, form, UA_LINUX, extra_cookies, 0);
    free(form); free(eparams);
    return r;
}

/* eapi branch (request.go:180-214): the mobile anti-fraud header object is
 * built from jar cookies + defaults, embedded as data["header"] AND sent as
 * request cookies; payload = AES-ECB(url path + json); URL /→/eapi/. */
static const char *jar_or(const ne_jar *j, const char *name,
                          const char *fallback) {
    const char *v = ne_jar_get(j, name);
    return (v && *v) ? v : fallback;
}

ne_resp *ne_call_eapi(const char *url, const char *eapi_path, jmap *data) {
    if (!g_jar) ne_jar_reload();

    /* CookieValueByName(options.Cookies, name, fallback) — options.Cookies
     * for eapi is just the jar (no extras in any current service) */
    const char *os = jar_or(g_jar, "os", "ios");
    const char *appver = jar_or(g_jar, "appver",
                                strcmp(os, "pc") != 0 ? "9.0.65" : "");

    char buildver[32];
    snprintf(buildver, sizeof buildver, "%s", jar_or(g_jar, "buildver", ""));
    if (!buildver[0])
        snprintf(buildver, sizeof buildver, "%lld",
                 (long long)(ne_now_ms() / 1000));
    /* requestId = Unix()*1000 + rand(0..999) — seconds*1000, not real ms */
    char request_id[40];
    snprintf(request_id, sizeof request_id, "%lld%u",
             (long long)(ne_now_ms() / 1000) * 1000, ne_rand_below(1000));

    char *device_id = NULL;
    const char *dv = ne_jar_get(g_jar, "deviceId");
    if (!dv || !*dv) device_id = ne_random_device_id();
    const char *device_id_v = (dv && *dv) ? dv : device_id;

    const char *mu = ne_jar_get(g_jar, "MUSIC_U");
    const char *ma = ne_jar_get(g_jar, "MUSIC_A");

    jmap *header = jmap_new();
    jmap_put(header, "osver", jar_or(g_jar, "osver", "17.4.1"));
    jmap_put(header, "deviceId", device_id_v);
    jmap_put(header, "appver", appver);
    jmap_put(header, "versioncode", jar_or(g_jar, "versioncode", "140"));
    jmap_put(header, "mobilename", jar_or(g_jar, "mobilename", ""));
    jmap_put(header, "buildver", buildver);
    jmap_put(header, "resolution", jar_or(g_jar, "resolution", "1920x1080"));
    jmap_put(header, "__csrf", jar_or(g_jar, "__csrf", ""));
    jmap_put(header, "os", os);
    jmap_put(header, "channel", jar_or(g_jar, "channel", ""));
    jmap_put(header, "requestId", request_id);
    if (mu && *mu) jmap_put(header, "MUSIC_U", mu);
    if (ma && *ma) jmap_put(header, "MUSIC_A", ma);
    jmap_put_map(data, "header", header);

    char *params = ne_eapi(eapi_path, data);
    if (!params) {
        free(device_id);
        ne_resp *r = ne_xmalloc(sizeof(ne_resp));
        r->code = 520; r->body = ne_xstrdup("encode failed");
        r->body_len = strlen(r->body); r->err = 1;
        return r;
    }

    const char *kv[2] = { "params", params };
    char *form = ne_http_form_encode(kv, 1);
    char *final_url = ne_rewrite_api_segment(url, "/eapi/");

    /* the header entries are ALSO sent as request cookies (request.go
     * SetCookies them after the options.Cookies loop) */
    const char *extras[32];
    int n = 0;
    extras[n++] = "osver";      extras[n++] = jar_or(g_jar, "osver", "17.4.1");
    extras[n++] = "deviceId";   extras[n++] = device_id_v;
    extras[n++] = "appver";     extras[n++] = appver;
    extras[n++] = "versioncode";extras[n++] = jar_or(g_jar, "versioncode", "140");
    extras[n++] = "mobilename"; extras[n++] = jar_or(g_jar, "mobilename", "");
    extras[n++] = "buildver";   extras[n++] = buildver;
    extras[n++] = "resolution"; extras[n++] = jar_or(g_jar, "resolution", "1920x1080");
    extras[n++] = "__csrf";     extras[n++] = jar_or(g_jar, "__csrf", "");
    extras[n++] = "os";         extras[n++] = os;
    extras[n++] = "channel";    extras[n++] = jar_or(g_jar, "channel", "");
    extras[n++] = "requestId";  extras[n++] = request_id;
    if (mu && *mu) { extras[n++] = "MUSIC_U"; extras[n++] = mu; }
    if (ma && *ma) { extras[n++] = "MUSIC_A"; extras[n++] = ma; }
    extras[n] = NULL;

    ne_resp *r = post_common(url, final_url, form, UA_PC, extras, 0);
    free(form); free(final_url); free(params); free(device_id);
    return r;
}

void ne_apply_request_strategy(void) {
    if (!g_jar) ne_jar_reload();
    /* os=pc + fixed fake NMTID — filterJar keeps the fake value off disk but
     * the jar-in-memory carries it, exactly like the Go process */
    ne_jar_set(g_jar, "os", "pc");
    ne_jar_set(g_jar, "NMTID", "some_random_id_from_strategy");
}

char *ne_generate_chain_id(void) {
    if (!g_jar) ne_jar_reload();
    const char *sd = ne_jar_get(g_jar, "sDeviceId");
    char *id;
    if (sd) {
        id = ne_xstrdup(sd);
    } else {
        static const char hexchars[] = "0123456789ABCDEF";
        id = ne_xmalloc(53);
        for (int i = 0; i < 52; i++) id[i] = hexchars[ne_rand_below(16)];
        id[52] = '\0';
    }
    char *out = ne_xmalloc(64 + strlen(id));
    snprintf(out, 64 + strlen(id), "v1_%s_web_login_%lld", id,
             (long long)ne_now_ms());
    free(id);
    return out;
}

/* global jar accessor for the CLI (persist on exit) */
ne_jar *ne_global_jar(void) {
    if (!g_jar) ne_jar_reload();
    return g_jar;
}
