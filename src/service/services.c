/* Read-service family — direct ports of the service .go sources (v1.6.0). */
#include "netease/services.h"
#include "netease/cookiejar.h"
#include "netease/encoding.h"
#include "netease/md5.h"
#include "netease/util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* options.Cookies sets the Go services attach (os overrides the request.go
 * default; appver then follows Ternary(os != "pc", ...)) */
static const char *COOKIES_OS_PC[]  = { "os", "pc",  NULL };
static const char *COOKIES_OS_IOS[] = { "os", "ios", NULL };

/* string builder for song_detail's `c` field */
typedef struct { char *s; size_t len, cap; } sb;
static void sb_add(sb *b, const char *frag, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->s = ne_xrealloc(b->s, b->cap);
    }
    memcpy(b->s + b->len, frag, n);
    b->len += n;
    b->s[b->len] = '\0';
}

/* ── search_service.go ─────────────────────────────────── */
ne_resp *ne_search(const char *s, const char *type,
                   const char *limit, const char *offset) {
    if (!type || !*type) type = "1";
    if (!limit || !*limit) limit = "30";
    if (!offset || !*offset) offset = "0";

    jmap *data = jmap_new();
    jmap_put(data, "limit", limit);
    jmap_put(data, "offset", offset);
    ne_resp *r;

    if (strcmp(type, "2000") == 0) {
        jmap_put(data, "keyword", s);
        jmap_put(data, "scene", "normal");
        char url[640];
        snprintf(url, sizeof url, "%s/api/search/voice/get", ne_api_base());
        r = ne_create_weapi(url, data, NULL);
    } else {
        jmap_put(data, "type", type);
        jmap_put(data, "s", s);
        char url[640];
        snprintf(url, sizeof url, "%s/api/cloudsearch/pc", ne_api_base());
        r = ne_create_weapi(url, data, NULL);
    }
    jmap_free(data);
    return r;
}

/* ── check_music_service.go ────────────────────────────── */
ne_resp *ne_check_music(const char *id, const char *br) {
    if (!br || !*br) br = "999000";
    char ids[256];
    snprintf(ids, sizeof ids, "[%s]", id);

    jmap *data = jmap_new();
    jmap_put(data, "ids", ids);
    jmap_put(data, "br", br);
    char url[640];
    snprintf(url, sizeof url, "%s/api/song/enhance/player/url", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── record_recent_songs_service.go (CallWeapi — no URL rewrite,
 * strict code validation happens in the kernel) ─────────── */
ne_resp *ne_record_recent(const char *limit) {
    jmap *data = jmap_new();
    jmap_put(data, "limit", limit && *limit ? limit : "100");
    char url[640];
    snprintf(url, sizeof url, "%s/api/play-record/song/list", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── recommend_resource_service.go ─────────────────────── */
ne_resp *ne_recommend_resource(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/v1/discovery/recommend/resource",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── song_url_v1_service.go (CallWeapi) ────────────────── */
ne_resp *ne_song_url_v1(const char *id, const char *level) {
    if (!level || !*level) level = "higher";
    char ids[256];
    snprintf(ids, sizeof ids, "[%s]", id);

    jmap *data = jmap_new();
    jmap_put(data, "ids", ids);
    jmap_put(data, "level", level);
    if (strcmp(level, "sky") == 0)
        jmap_put(data, "immerseType", "c51");
    jmap_put(data, "encodeType", "flac");
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/song/enhance/player/url/v1",
             ne_api_base());
    ne_resp *r = ne_call_weapi(url, data);
    jmap_free(data);
    return r;
}

/* ── song_url_service.go (linuxapi) ────────────────────── */
ne_resp *ne_song_url_old(const char *id, const char *br) {
    if (!br || !*br) br = "320000";
    char ids[256];
    snprintf(ids, sizeof ids, "[%s]", id);

    jmap *data = jmap_new();
    jmap_put(data, "ids", ids);
    jmap_put(data, "br", br);
    char url[640];
    snprintf(url, sizeof url, "%s/api/song/enhance/player/url", ne_api_base());
    /* ne_call_linuxapi takes ownership of data */
    return ne_call_linuxapi(url, data, COOKIES_OS_PC);
}

/* ── song_download_url (original apiservice — no Go service) ──
 * Endpoint pinned from chaunsin/netease-cloud-music SongDownloadUrlV1:
 * POST /weapi/song/enhance/download/url/v1 with body {id, level}
 * (immerseType only for sky). The path is also intercepted by
 * UnblockNeteaseMusic/server and go-musicfox's vendored copy. Unlike
 * player/url/v1, free tracks (fee==0) can get up to Hi-Res here, so the
 * download channel is kept separate; response data is a single object. */
ne_resp *ne_song_download_url(const char *id, const char *level) {
    if (!level || !*level) level = "standard";

    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    jmap_put(data, "level", level);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/song/enhance/download/url/v1",
             ne_api_base());
    ne_resp *r = ne_call_weapi(url, data);
    jmap_free(data);
    return r;
}

/* ── song_detail_service.go ────────────────────────────── */
ne_resp *ne_song_detail(const char *ids_csv) {
    /* c: [{"id":".."},...] — one entry per comma piece, input order;
     * json.Marshal of []IDS keeps single-field order, no HTML escaping of
     * digits anyway (jmap marshal is byte-identical here) */
    sb c = {0};
    sb_add(&c, "[", 1);
    const char *p = ids_csv;
    int first = 1;
    for (;;) {
        const char *end = strchr(p, ',');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        /* {"id":"<piece>"} — piece escaped like Go json.Marshal */
        jmap *one = jmap_new();
        char *piece = ne_xmalloc(n + 1);
        memcpy(piece, p, n);
        piece[n] = '\0';
        jmap_put(one, "id", piece);
        free(piece);
        char *one_json = jmap_marshal(one);
        jmap_free(one);
        if (!first) sb_add(&c, ",", 1);
        sb_add(&c, one_json, strlen(one_json));
        free(one_json);
        first = 0;
        if (!end) break;
        p = end + 1;
    }
    sb_add(&c, "]", 1);

    char ids[8192];
    snprintf(ids, sizeof ids, "[%s]", ids_csv);

    jmap *data = jmap_new();
    jmap_put(data, "c", c.s);
    jmap_put(data, "ids", ids);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/v3/song/detail", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_PC);
    free(c.s);
    jmap_free(data);
    return r;
}

/* ── playlist_detail_service.go (linuxapi) ─────────────── */
ne_resp *ne_playlist_detail(const char *id, const char *s) {
    if (!s || !*s) s = "8";
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    jmap_put(data, "n", "100000");
    jmap_put(data, "s", s);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/v3/playlist/detail", ne_api_base());
    /* ne_call_linuxapi takes ownership of data */
    return ne_call_linuxapi(url, data, NULL);
}

/* ── user_playlist_service.go ──────────────────────────── */
ne_resp *ne_user_playlist(const char *uid, const char *limit,
                          const char *offset) {
    jmap *data = jmap_new();
    jmap_put(data, "uid", uid);
    jmap_put(data, "limit", limit && *limit ? limit : "30");
    jmap_put(data, "offset", offset && *offset ? offset : "0");
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/user/playlist", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── lyric_service.go (linuxapi) ───────────────────────── */
ne_resp *ne_lyric(const char *id) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    jmap_put(data, "lv", "-1");
    jmap_put(data, "kv", "-1");
    jmap_put(data, "tv", "-1");
    char url[640];
    snprintf(url, sizeof url, "%s/api/song/lyric", ne_api_base());
    /* ne_call_linuxapi takes ownership of data */
    return ne_call_linuxapi(url, data, COOKIES_OS_PC);
}

/* ── toplist_detail_service.go ─────────────────────────── */
ne_resp *ne_toplist_detail(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/toplist/detail", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── recommend_songs_service.go ────────────────────────── */
ne_resp *ne_recommend_songs(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/api/v3/discovery/recommend/songs",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_IOS);
    jmap_free(data);
    return r;
}

/* ── personalized_service.go ───────────────────────────── */
ne_resp *ne_recommend_playlists(const char *limit) {
    jmap *data = jmap_new();
    jmap_put(data, "limit", limit && *limit ? limit : "30");
    jmap_put(data, "order", "true");
    jmap_put(data, "n", "1000");
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/personalized/playlist",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_PC);
    jmap_free(data);
    return r;
}

/* ── user_account_service.go ───────────────────────────── */
ne_resp *ne_user_account(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/api/nuser/account/get", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── vip_info (original apiservice — not in the Go package) ──
 * Endpoint pinned from Binaryify NeteaseCloudMusicApi module/vip_info.js:
 * POST /weapi/music-vip-membership/front/vip/info with an empty body. */
ne_resp *ne_vip_info(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/music-vip-membership/front/vip/info",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── like_list_service.go ──────────────────────────────── */
ne_resp *ne_like_list(const char *uid) {
    jmap *data = jmap_new();
    jmap_put(data, "uid", uid);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/song/like/get", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ══ write family (phase 6) ═════════════════════════════ */

/* playlist_subscribe_service.go — t "1" → subscribe else unsubscribe */
ne_resp *ne_playlist_subscribe(const char *id, const char *t) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    const char *action = (t && strcmp(t, "1") == 0) ? "subscribe" : "unsubscribe";
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/playlist/%s", ne_api_base(), action);
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* playlist_tracks_service.go — the Go service doubles its own trackIds
 * (`append(x, x...)`), so one id marshals as ["<id>","<id>"] */
ne_resp *ne_playlist_tracks(const char *op, const char *pid,
                            const char *track_id) {
    jmap *data = jmap_new();
    jmap_put(data, "op", op);
    jmap_put(data, "pid", pid);

    sb b = {0};
    sb_add(&b, "[", 1);
    for (int round = 0; round < 2; round++) {          /* the doubling quirk */
        if (round) sb_add(&b, ",", 1);
        sb_add(&b, "\"", 1);
        sb_add(&b, track_id, strlen(track_id));
        sb_add(&b, "\"", 1);
    }
    sb_add(&b, "]", 1);
    jmap_put(data, "trackIds", b.s);
    free(b.s);

    jmap_put(data, "imme", "true");
    char url[640];
    snprintf(url, sizeof url, "%s/api/playlist/manipulate/tracks", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* playlist_create_service.go — privacy forced to "0" unless "10" */
ne_resp *ne_playlist_create(const char *name, const char *privacy) {
    if (!privacy || strcmp(privacy, "10") != 0) privacy = "0";
    jmap *data = jmap_new();
    jmap_put(data, "name", name);
    jmap_put(data, "privacy", privacy);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/playlist/create", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* playlist_delete_service.go — ids "[<id>]" */
ne_resp *ne_playlist_delete(const char *id) {
    jmap *data = jmap_new();
    char ids[128];
    snprintf(ids, sizeof ids, "[%s]", id);
    jmap_put(data, "ids", ids);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/playlist/remove", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* playlist_name_update_service.go — eapi over interface3.music.163.com.
 * NE_API_BASE override keeps the smoke test on the loopback server; in
 * production the hard-coded interface3 host matches the Go build. */
ne_resp *ne_playlist_update_name(const char *id, const char *name) {
    const char *ovr = getenv("NE_API_BASE");
    char url[640];
    if (ovr && *ovr)
        snprintf(url, sizeof url, "%s/eapi/playlist/update/name", ne_api_base());
    else
        snprintf(url, sizeof url,
                 "http://interface3.music.163.com/eapi/playlist/update/name");

    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    jmap_put(data, "name", name);
    /* ownership of data (with the merged header map) goes to the call */
    return ne_call_eapi(url, "/api/playlist/update/name", data);
}

/* ══ login family (phase 6) ═════════════════════════════ */

/* login_email_service.go — password md5-hex, extras os=ios appver=8.7.01 */
ne_resp *ne_login_email(const char *email, const char *password) {
    uint8_t dg[16];
    ne_md5_buf(password, strlen(password), dg);
    char *pw = ne_hex_lower(dg, 16);

    jmap *data = jmap_new();
    jmap_put(data, "username", email);
    jmap_put(data, "password", pw);
    jmap_put(data, "rememberLogin", "true");

    static const char *extras[] = {
        "os", "ios", "appver", "8.7.01", NULL
    };
    char url[640];
    snprintf(url, sizeof url, "%s/api/login", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, extras);
    jmap_free(data);
    free(pw);
    return r;
}

/* login_cellphone_service.go — CallWeapi (csrf_token = "" from the shell) */
ne_resp *ne_login_cellphone(const char *phone, const char *password) {
    uint8_t dg[16];
    ne_md5_buf(password, strlen(password), dg);
    char *pw = ne_hex_lower(dg, 16);

    jmap *data = jmap_new();
    jmap_put(data, "phone", phone);
    jmap_put(data, "countrycode", "86");
    jmap_put(data, "csrf_token", "");
    jmap_put(data, "password", pw);
    jmap_put(data, "rememberLogin", "true");
    jmap_put(data, "type", "1");
    jmap_put(data, "https", "true");
    jmap_put(data, "remember", "true");

    char url[640];
    snprintf(url, sizeof url, "%s/weapi/login/cellphone", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    free(pw);
    return r;
}

/* login_refresh_service.go — ApplyRequestStrategy + csrf, CallWeapi */
ne_resp *ne_login_refresh(void) {
    ne_apply_request_strategy();
    const char *csrf = ne_jar_get(ne_global_jar(), "__csrf");
    jmap *data = jmap_new();
    jmap_put(data, "csrf_token", csrf ? csrf : "");
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/login/token/refresh", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}
