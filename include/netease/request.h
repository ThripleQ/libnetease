#ifndef NE_REQUEST_H
#define NE_REQUEST_H
#include <stddef.h>
#include "netease/jmap.h"

/* Request kernel — mirrors util.CallWeapi / util.CreateRequest (v1.6.0).
 * One global jar per process, persisted by the CLI on exit. */

typedef struct {
    double code;     /* API business code ("code" field), 200 fallback,
                        520 on transport error (Go convention) */
    char  *body;     /* malloc'd response body */
    size_t body_len;
    int    err;      /* 0 ok; 1 transport error; 2 no code field */
} ne_resp;

void ne_resp_free(ne_resp *r);

/* global jar */
void ne_set_cookie_file(const char *path);
const char *ne_cookie_file(void);
/* (re)create the global jar from the cookie file; missing file = empty jar */
void ne_jar_reload(void);
/* opaque global jar handle (for ne_jar_save_file on exit) */
struct ne_jar;
struct ne_jar *ne_global_jar(void);

/* API base URL — "https://music.163.com" or the NE_API_BASE env override
 * (test hook; unset in production). */
const char *ne_api_base(void);

/* util.CallWeapi — POST <api> with ApiParamsEncode(data).
 * UA = pc, Referer set, cookies from global jar. */
ne_resp *ne_call_weapi(const char *api, const jmap *data);

/* util.CreateRequest("POST", url, data, {Crypto:"weapi", Cookies: extra})
 * — the legacy path used by most read services and the `like` command.
 * Applies the /\w*api/ → /weapi/ URL rewrite, injects the request.go cookie
 * set (os/appver/__remember_me, _ntes_nuid, NMTID on login URLs) and
 * csrf_token before encryption. extra_cookies = {name,value} NULL-terminated
 * pairs. NOTE: `data` is modified (csrf_token added). */
ne_resp *ne_create_weapi(const char *url, jmap *data,
                         const char *const *extra_cookies);
/* CallWeapi-equivalent transport for login flows: no anti-fraud cookie
 * injection (no os/appver/NMTID), clean web request like request.go's
 * NewRequest path. */
ne_resp *ne_create_weapi_clean(const char *url, jmap *data);

/* util.CreateRequest(..., {Crypto:"linuxapi"}) — payload
 * {method, url(/api/), params} AES-ECB encrypted, POSTed to
 * /api/linux/forward with a Linux UA. TAKES OWNERSHIP of `data`
 * (embedded as the nested params map — do not free it afterwards). */
ne_resp *ne_call_linuxapi(const char *url, jmap *data,
                          const char *const *extra_cookies);

/* util.CreateRequest(..., {Crypto:"eapi", Url: eapi_path}) — the mobile-API
 * branch (playlist-rename): builds the anti-fraud "header" object (osver/
 * deviceId/appver/versioncode/mobilename/buildver/resolution/__csrf/os/
 * channel/requestId [+MUSIC_U/MUSIC_A]), embeds it as data["header"],
 * AES-ECB-encrypts url+json, rewrites /\w*api/ -> /eapi/ and ALSO sends the
 * header entries as request cookies (request.go does both). The header map
 * is merged INTO `data` (ownership taken by the call). */
ne_resp *ne_call_eapi(const char *url, const char *eapi_path, jmap *data);

/* util.ApplyRequestStrategy — inject os=pc + FIXED fake NMTID into the jar.
 * The fake NMTID is dropped AT THE JAR BOUNDARY (filterJar.SetCookies), so it
 * never reaches memory, disk, or the wire — only os=pc actually takes effect.
 * Kept as an explicit function because the Go call site still exists. */
void ne_apply_request_strategy(void);

/* util.GenerateChainID — "v1_<sDeviceId>_web_login_<ms>"; sDeviceId from
 * jar or freshly generated 52-hex (upper). malloc'd. */
char *ne_generate_chain_id(void);

/* request.go `/\w*api/` rewrite (weapi branch + linuxapi inner url).
 * Replacement is "/weapi/" or "/api/"; a segment not followed by '/' is
 * no match. malloc'd result. Public for the unit tests. */
char *ne_rewrite_api_segment(const char *url, const char *replacement);
#endif
