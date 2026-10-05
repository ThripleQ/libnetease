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

/* ── song_url_service.go — 2026-09: linuxapi → weapi（与 check_music
 * 同端点同参数，check_music 在 weapi 上长期工作）────────── */
ne_resp *ne_song_url_old(const char *id, const char *br) {
    if (!br || !*br) br = "320000";
    char ids[256];
    snprintf(ids, sizeof ids, "[%s]", id);

    jmap *data = jmap_new();
    jmap_put(data, "ids", ids);
    jmap_put(data, "br", br);
    char url[640];
    snprintf(url, sizeof url, "%s/api/song/enhance/player/url", ne_api_base());
    /* ne_create_weapi 不接管 data（只有 ne_call_* 会），必须自行释放 */
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_PC);
    jmap_free(data);
    return r;
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

/* ── song_music_quality (original apiservice — no Go service) ──
 * Endpoint pinned from chaunsin/netease-cloud-music SongMusicQuality:
 * POST /weapi/song/music/detail/get with body {songId}. Returns the per
 * quality-tier source table (l/m/h/sq/hr/je/sk/jm → {br,size,...}), which
 * is the authoritative answer for "what levels does this track actually
 * have". A nil tier means that level has no source. */
ne_resp *ne_song_music_quality(const char *id) {
    jmap *data = jmap_new();
    jmap_put(data, "songId", id);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/song/music/detail/get",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── song_purchased (已购单曲列表) ──────────────────────────
 * Endpoint from Binaryify/NeteaseCloudMusicApi song_purchased:
 * POST /api/single/mybought/song/list with {limit, offset}. Returns the
 * user's purchased single tracks (the list itself is owned; no per-item
 * flag needed). */
ne_resp *ne_song_purchased(const char *limit, const char *offset) {
    if (!limit || !*limit) limit = "20";
    if (!offset || !*offset) offset = "0";
    jmap *data = jmap_new();
    jmap_put(data, "limit", limit);
    jmap_put(data, "offset", offset);
    char url[640];
    snprintf(url, sizeof url, "%s/api/single/mybought/song/list",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── album_purchased (已购数字专辑列表) ─────────────────────
 * Endpoint from Binaryify/NeteaseCloudMusicApi digitalAlbum_purchased:
 * POST /api/digitalAlbum/purchased with {limit, offset, total}. Returns the
 * user's purchased digital albums. */
ne_resp *ne_album_purchased(const char *limit, const char *offset) {
    if (!limit || !*limit) limit = "30";
    if (!offset || !*offset) offset = "0";
    jmap *data = jmap_new();
    jmap_put(data, "limit", limit);
    jmap_put(data, "offset", offset);
    jmap_put(data, "total", "true");
    char url[640];
    snprintf(url, sizeof url, "%s/api/digitalAlbum/purchased",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ── album_detail (专辑详情) ─────────────────────────────
 * Endpoint from Binaryify/NeteaseCloudMusicApi album:
 * POST /weapi/v1/album/{id}. Returns the album's tracks. */
ne_resp *ne_album_detail(const char *id) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/v1/album/%s", ne_api_base(), id);
    ne_resp *r = ne_create_weapi(url, data, NULL);
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

/* playlist_detail_service.go 复刻为 linuxapi+v3；2026-09 审计后平移 weapi+v6，
 * 对齐活跃上游 api-enhanced (playlist_detail.js: /api/v6/playlist/detail) ——
 * linuxapi 通道已弃用（上游生产模块零使用），weapi 为网页端原生路径。 */
ne_resp *ne_playlist_detail(const char *id, const char *s) {
    if (!s || !*s) s = "8";
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    jmap_put(data, "n", "100000");
    jmap_put(data, "s", s);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/v6/playlist/detail", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
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

/* ── lyric_service.go — 2026-09: linuxapi → weapi（上游 api-enhanced 同路径），
 * 补 rv（罗马音）与 _nmclfl（防空标记）字段 ─────────────── */
ne_resp *ne_lyric(const char *id) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    jmap_put(data, "lv", "-1");
    jmap_put(data, "kv", "-1");
    jmap_put(data, "tv", "-1");
    jmap_put(data, "rv", "-1");
    jmap_put(data, "_nmclfl", "1");
    char url[640];
    snprintf(url, sizeof url, "%s/api/song/lyric", ne_api_base());
    /* ne_create_weapi 不接管 data（只有 ne_call_* 会），必须自行释放 */
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_PC);
    jmap_free(data);
    return r;
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
    /* 2026 网易已迁移: /api/w/nuser/account/get (api-enhanced login_status.js
     * 现行路径); 旧路径 /api/nuser/account/get 会重定向到 HTML 登录页 */
    snprintf(url, sizeof url, "%s/api/w/nuser/account/get", ne_api_base());
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
    jmap_put(data, "secureCaptcha", "");

    char url[640];
    /* /api/w/login/cellphone 经 weapi rewrite 后为 /weapi/w/login/cellphone
     * (旧路径 /weapi/login/cellphone 已被网易下线) */
    snprintf(url, sizeof url, "%s/api/w/login/cellphone", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    free(pw);
    return r;
}

/* send_captcha_service.go (Binaryify /captcha/sent) — weapi {cellphone, ctcode}.
 * Sends the SMS login code; rate-limited per day, avoid spamming it. */
ne_resp *ne_send_captcha(const char *phone, const char *countrycode) {
    if (!countrycode || !*countrycode) countrycode = "86";
    jmap *data = jmap_new();
    jmap_put(data, "cellphone", phone);
    jmap_put(data, "ctcode", countrycode);
    /* 网易 2025+ 要求登录场景密钥, 缺失则验证码接口静默拒绝
     * (api-enhanced module/captcha_sent.js 现行字段) */
    jmap_put(data, "secrete", "music_middleuser_pclogin");

    static const char *extras[] = { "os", "pc", NULL };
    char url[640];
    /* /api/sms/captcha/sent 经 weapi rewrite 后为 /weapi/sms/captcha/sent
     * (旧路径 /weapi/captcha/sent 已被网易下线) */
    snprintf(url, sizeof url, "%s/api/sms/captcha/sent", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, extras);
    jmap_free(data);
    return r;
}

/* login_cellphone captcha mode (Binaryify) — /weapi/login/cellphone with
 * the SMS code instead of the password. */
ne_resp *ne_login_cellphone_captcha(const char *phone, const char *captcha,
                                    const char *countrycode) {
    if (!countrycode || !*countrycode) countrycode = "86";
    jmap *data = jmap_new();
    jmap_put(data, "phone", phone);
    jmap_put(data, "countrycode", countrycode);
    jmap_put(data, "captcha", captcha);
    jmap_put(data, "csrf_token", "");
    jmap_put(data, "rememberLogin", "true");
    jmap_put(data, "type", "1");
    jmap_put(data, "https", "true");
    jmap_put(data, "remember", "true");
    jmap_put(data, "secureCaptcha", "");

    char url[640];
    snprintf(url, sizeof url, "%s/api/w/login/cellphone", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
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

/* ══ artist family ════════════════════════════════════════════════════ */

/* artist_detail — weapi/v1/artist/{id}: {artist, hotSongs, more}. The v1
 * payload embeds per-song privilege objects carrying a nested "code":0;
 * the kernel's top-level code scan is immune (see parse_code). */
ne_resp *ne_artist_detail(const char *id) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/v1/artist/%s", ne_api_base(), id);
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* artist_songs — weapi/v1/artist/songs {id, offset, limit, order} */
ne_resp *ne_artist_songs(const char *id, const char *offset,
                         const char *limit, const char *order) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    jmap_put(data, "offset", offset && *offset ? offset : "0");
    jmap_put(data, "limit", limit && *limit ? limit : "50");
    jmap_put(data, "order", order && *order ? order : "hot");
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/v1/artist/songs", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* artist_albums — weapi/artist/albums/{id} {limit, offset, total} */
ne_resp *ne_artist_albums(const char *id, const char *limit,
                          const char *offset) {
    jmap *data = jmap_new();
    jmap_put(data, "limit", limit && *limit ? limit : "50");
    jmap_put(data, "offset", offset && *offset ? offset : "0");
    jmap_put(data, "total", "true");
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/artist/albums/%s", ne_api_base(), id);
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* artist_desc — weapi/artist/introduction {id} */
ne_resp *ne_artist_desc(const char *id) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/artist/introduction", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ══ podcast / radio family ═══════════════════════════════════════════ */

/* radio_detail — weapi/djradio/get {id}. The /v2 path 404s; v1 is live. */
ne_resp *ne_radio_detail(const char *id) {
    jmap *data = jmap_new();
    jmap_put(data, "id", id);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/djradio/get", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* radio_programs — weapi/dj/program/byradio. radioId/limit/offset must be
 * JSON numbers and asc a JSON boolean; string forms are rejected (code 400). */
ne_resp *ne_radio_programs(const char *radio_id, const char *limit,
                           const char *offset, const char *asc) {
    jmap *data = jmap_new();
    jmap_put_int(data, "radioId", strtol(radio_id, NULL, 10));
    jmap_put_int(data, "limit", (limit && *limit) ? strtol(limit, NULL, 10) : 50);
    jmap_put_int(data, "offset", (offset && *offset) ? strtol(offset, NULL, 10) : 0);
    jmap_put_bool(data, "asc", asc && strcmp(asc, "true") == 0);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/dj/program/byradio", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ══ comment family ═══════════════════════════════════════════════════ */

/* comments — weapi/v1/resource/comments/{thread_id} {rid, limit, offset,
 * beforeTime}. `rid` mirrors the path thread id. */
ne_resp *ne_comments(const char *thread_id, const char *limit,
                     const char *offset, const char *before_time) {
    jmap *data = jmap_new();
    jmap_put(data, "rid", thread_id);
    jmap_put(data, "limit", limit && *limit ? limit : "20");
    jmap_put(data, "offset", offset && *offset ? offset : "0");
    jmap_put(data, "beforeTime", before_time && *before_time ? before_time : "0");
    char url[768];
    snprintf(url, sizeof url, "%s/weapi/v1/resource/comments/%s",
             ne_api_base(), thread_id);
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* comments_hot — weapi/v1/resource/hotcomments/{thread_id}, same data */
ne_resp *ne_comments_hot(const char *thread_id, const char *limit,
                         const char *offset, const char *before_time) {
    jmap *data = jmap_new();
    jmap_put(data, "rid", thread_id);
    jmap_put(data, "limit", limit && *limit ? limit : "20");
    jmap_put(data, "offset", offset && *offset ? offset : "0");
    jmap_put(data, "beforeTime", before_time && *before_time ? before_time : "0");
    char url[768];
    snprintf(url, sizeof url, "%s/weapi/v1/resource/hotcomments/%s",
             ne_api_base(), thread_id);
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* ══ explore / homepage family (2026-10-04) ═══════════════════════════
 * 探索页补齐。端点是 2026 现行路径（钉自 api-enhanced module），不是 Go
 * v1.6.0 移植的一部分 —— 因此不参与 dualrun 的 Go 逐字节比对。 */

/* homepage_dragon_ball.js — /api/homepage/dragon/ball/static，空 data，
 * weapi。移动端接口 → cookie 附 os=ios。未登录时服务端返回空数组。 */
ne_resp *ne_dragon_ball(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/api/homepage/dragon/ball/static",
             ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_IOS);
    jmap_free(data);
    return r;
}

/* style_list.js — /api/tag/list/get，空 data，weapi。曲风标签总表。 */
ne_resp *ne_style_list(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/api/tag/list/get", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* style_song.js — /api/style-tag/home/song {cursor,size,tagId,sort}。
 * 四字段均为 JSON 数字：上游 query 直接传数字，传字符串服务端会拒
 * （与 ne_radio_programs 同样的坑），故用 jmap_put_int。 */
ne_resp *ne_style_song(const char *tag_id, const char *size,
                       const char *cursor) {
    jmap *data = jmap_new();
    jmap_put_int(data, "cursor", cursor && *cursor ? atol(cursor) : 0);
    jmap_put_int(data, "size", size && *size ? atol(size) : 20);
    jmap_put_int(data, "tagId", tag_id && *tag_id ? atol(tag_id) : 0);
    jmap_put_int(data, "sort", 0);
    char url[640];
    // 前缀必须是 /weapi 而不是 /api：上游 request.js 发 weapi 时会把 uri 的
    // `/api` 重写成 `/weapi`（`url = domain + '/weapi/' + uri.substr(5)`），
    // 而服务端只在 /weapi 下注册了 style-tag 系列 —— 直接打 /api 会返回
    // {"msg":"参数错误","code":400}（2026-10-04 探针实测）。
    snprintf(url, sizeof url, "%s/weapi/style-tag/home/song", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* style_playlist.js — /api/style-tag/home/playlist，字段同 style_song。 */
ne_resp *ne_style_playlist(const char *tag_id, const char *size,
                           const char *cursor) {
    jmap *data = jmap_new();
    jmap_put_int(data, "cursor", cursor && *cursor ? atol(cursor) : 0);
    jmap_put_int(data, "size", size && *size ? atol(size) : 20);
    jmap_put_int(data, "tagId", tag_id && *tag_id ? atol(tag_id) : 0);
    jmap_put_int(data, "sort", 0);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/style-tag/home/playlist", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* radio_get — 私人漫游 weapi /api/v1/radio/get {mode, subMode, limit}。
 * 移动端接口（App 的漫游页才有），故 cookie 带 os=ios。
 * mode 为空 → 不写 mode/subMode，走服务端默认模式。 */
ne_resp *ne_radio_get(const char *mode, const char *sub_mode,
                      const char *limit) {
    jmap *data = jmap_new();
    if (mode && *mode) {
        jmap_put_int(data, "mode", atol(mode));
        jmap_put_int(data, "subMode", sub_mode && *sub_mode ? atol(sub_mode) : 0);
    }
    jmap_put_int(data, "limit", limit && *limit ? atol(limit) : 3);
    char url[640];
    snprintf(url, sizeof url, "%s/api/v1/radio/get", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_IOS);
    jmap_free(data);
    return r;
}

/* playlist_catalogue.js — /api/playlist/catalogue，空 data，weapi。
 * 歌单分类总表：categories 0 语种 / 1 风格 / 2 场景 / 3 情感 / 4 主题，
 * sub[] 是标签（name/category/hot…）。注意标签**没有封面**（imgUrl 恒 null），
 * 所以「场景音乐」卡的封面得另取：见 ne_playlist_list。 */
ne_resp *ne_playlist_catalogue(void) {
    jmap *data = jmap_new();
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/playlist/catalogue", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* top_playlist.js — /api/playlist/list（**不是** /top/playlist，那个路径
 * 已经 404 了）{cat, order, limit, offset, total}。分类歌单：给一个标签名
 * （清晨 / 伤感 / 治愈…）返回该标签下的热门歌单。
 * limit/offset/total 走数字/布尔，与 style-tag 同样的规矩。 */
ne_resp *ne_playlist_list(const char *cat, const char *limit,
                          const char *offset) {
    jmap *data = jmap_new();
    jmap_put(data, "cat", cat && *cat ? cat : "全部");
    jmap_put(data, "order", "hot");
    jmap_put_int(data, "limit", limit && *limit ? atol(limit) : 6);
    jmap_put_int(data, "offset", offset && *offset ? atol(offset) : 0);
    jmap_put_int(data, "total", 1);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/playlist/list", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}

/* homepage_block_page.js — /api/homepage/block/page {refresh, cursor}，
 * 移动端接口（首页「发现」页整条 block 流）→ cookie 附 os=ios。
 *
 * 「雷达歌单」区（HOMEPAGE_BLOCK_MGC_PLAYLIST 那个 block）是 weapi 侧唯一的
 * 雷达入口 —— 服务端把 radar 藏在这条通用 block 流里，没有独立端点
 * （带 radar 字样的 7 个候选路径全 404）。refresh/cursor 是上游 module 的
 * 原始字段：refresh 为布尔、cursor 首次传字符串 "-1"。 */
ne_resp *ne_homepage_block_page(const char *refresh, const char *cursor) {
    jmap *data = jmap_new();
    /* refresh 是布尔，**只有显式真值算真**：原先写的是「不等于 "0" 就算真」，
     * 于是 Kotlin 侧传 "false"（本意是「走服务端当日缓存」）被判成 true ——
     * 每次进探索页都在强制服务端重新出卡。与 asc（只认 "true"）统一口径。 */
    jmap_put_bool(
        data, "refresh",
        refresh && (strcmp(refresh, "true") == 0 || strcmp(refresh, "1") == 0));
    /* cursor **只在调用方真的给了值时才带**。这条端点的 /weapi/ 路由只实现了
     * 无分页形态：cursor 只要非空（"-1" 哨兵、0、数字还是字符串、甚至服务端
     * 自己返回的那个真游标，全都一样）就固定返 HTTP 200 + {"code":50002}
     * （74 字节）—— 详见 ne_create_weapi_asis 的注释。首页只要第一屏 blocks，
     * 不带 cursor 即得全套（雷达块 6 张 + 猜你喜欢 4 组），这也正是上游
     * api-enhanced 的默认调用形态（data.cursor 为 undefined，序列化时被丢掉）。
     * 将来真要按 cursor 翻 block 流，只能改走 /api/ 老网关（asis）+ 非空 cursor。 */
    if (cursor && *cursor) jmap_put(data, "cursor", cursor);
    char url[640];
    snprintf(url, sizeof url, "%s/api/homepage/block/page", ne_api_base());
    /* 标准 ne_create_weapi（URL 里的 /api/ 段被重写成 /weapi/ = 主流网关）。
     * 这里不带 cursor，正好落在 /weapi/ 能正常服务的那个形态上。 */
    ne_resp *r = ne_create_weapi(url, data, COOKIES_OS_IOS);
    jmap_free(data);
    return r;
}

/* simi_artist — weapi/discovery/simiArtist {artistid}（相似歌手）。
 * 字段名是 `artistid`（全小写，没有下划线），与上游 simi_artist.js 一致；
 * 写成 artistId 服务端会当成没传参数。 */
ne_resp *ne_simi_artist(const char *artist_id) {
    jmap *data = jmap_new();
    jmap_put(data, "artistid", artist_id);
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/discovery/simiArtist", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, NULL);
    jmap_free(data);
    return r;
}
