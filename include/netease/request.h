#ifndef NE_REQUEST_H
#define NE_REQUEST_H
#include <stddef.h>
#include "netease/jmap.h"

/* Request kernel -- mirrors util.CallWeapi / util.CreateRequest (v1.6.0).
 * One global jar per process, persisted by the CLI on exit.
 *
 * THREAD CONTRACT: this layer is process-global, single-threaded by design
 * (one active caller at a time): the global jar (ne_global_jar and the
 * ne_jar_ accessors), the optional cookie file, the installed API base and
 * the request transport are all shared, unsynchronized state. A multi-threaded
 * host (e.g. Android via JNI) MUST NOT issue concurrent request-kernel calls
 * against the same jar; serialize them (single dispatcher thread/queue, or an
 * external mutex guarding every call). This matches the Go process's
 * single-threaded operation. */

typedef struct {
    double code;     /* API business code ("code" field), 200 fallback,
                        520 on transport error (Go convention) */
    long   http_status; /* raw HTTP status from the transport, 0 on
                           transport error (added by the risk-control
                           hardening; existing callers ignore it) */
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

/* API base URL. Default "https://music.163.com". Priority: explicit
 * ne_set_api_base() (highest) -> NE_API_BASE env (CLI/test hook) -> default.
 * Embedded hosts (Android, GUI) that cannot rely on the environment should
 * call ne_set_api_base() at startup. */
const char *ne_api_base(void);
void ne_set_api_base(const char *base);

/* util.CallWeapi -- POST <api> with ApiParamsEncode(data).
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

/* 风控应对(可选, 默认关闭): 对 -460/高频限流/传输错误做指数退避自动重试,
 * 最多 max_attempts 次; <=0 关闭. 环境变量 NE_RETRY_RISK=<n> 同效.
 * 仅重试"瞬时可恢复"的分类(见 ne_risk_is_transient), -462 行为验证不重试.
 * 注意: 写操作(like/subscribe/track-add 等)若服务端已受理但响应丢失,
 * 重试可能重复生效 —— 写操作场景请保持关闭或自行保证幂等. */
void ne_set_risk_retry(int max_attempts);
#endif
