#ifndef NE_SERVICES_H
#define NE_SERVICES_H
#include "netease/request.h"

/* Read-service family — ports of the service .go definitions the Go
 * netease-cli shell invokes. Every function performs one HTTP round trip
 * through the request kernel and returns the raw ne_resp; CLI-level JSON
 * post-processing stays in the shell, exactly like Go. */

/* SearchService.Search — s/type/limit/offset; empty → "1"/"30"/"0";
 * type "2000" switches to /api/search/voice/get (keyword+scene) */
ne_resp *ne_search(const char *s, const char *type,
                   const char *limit, const char *offset);

/* CheckMusicService — ids=[id], br empty → "999000" */
ne_resp *ne_check_music(const char *id, const char *br);

/* RecordRecentSongsService — CallWeapi /api/play-record/song/list */
ne_resp *ne_record_recent(const char *limit);

/* RecommendResourceService — 每日推荐歌单 */
ne_resp *ne_recommend_resource(void);

/* SongUrlV1Service — CallWeapi /weapi/song/enhance/player/url/v1;
 * level empty → "higher"; "sky" adds immerseType=c51 */
ne_resp *ne_song_url_v1(const char *id, const char *level);

/* SongUrlService — linuxapi /api/song/enhance/player/url, cookie os=pc */
ne_resp *ne_song_url_old(const char *id, const char *br);

/* SongDownloadUrlService — CallWeapi /weapi/song/enhance/download/url/v1,
 * the official download endpoint (kept separate from the play URL). data
 * {id, level} — id is a single value, not an array; level empty → "standard".
 * Original apiservice: no corresponding Go service, ported from the Go
 * netease-cli shell's song-download-url command. */
ne_resp *ne_song_download_url(const char *id, const char *level);

/* SongDetailService — weapi/v3/song/detail, cookie os=pc;
 * ids_csv "1,2,3" → c=[{"id":"1"},..], ids=[1,2,3] */
ne_resp *ne_song_detail(const char *ids_csv);

/* PlaylistDetailService — linuxapi weapi/v3/playlist/detail,
 * data {id, n=100000, s} (s empty → "8") */
ne_resp *ne_playlist_detail(const char *id, const char *s);

/* UserPlaylistService — weapi/user/playlist, limit/offset empty → 30/0 */
ne_resp *ne_user_playlist(const char *uid, const char *limit,
                          const char *offset);

/* LyricService — linuxapi /api/song/lyric, cookie os=pc, lv/kv/tv=-1 */
ne_resp *ne_lyric(const char *id);

/* ToplistDetailService — weapi/toplist/detail */
ne_resp *ne_toplist_detail(void);

/* RecommendSongsService — weapi, cookie os=ios */
ne_resp *ne_recommend_songs(void);

/* PersonalizedService — weapi/personalized/playlist, cookie os=pc,
 * {limit, order=true, n=1000} */
ne_resp *ne_recommend_playlists(const char *limit);

/* UserAccountService — weapi rewrite of /api/nuser/account/get */
ne_resp *ne_user_account(void);

/* VipInfoService — CallWeapi /weapi/music-vip-membership/full/vip/info
 * (Binaryify /vip/info, "获取 VIP 信息(app端)"). Not wrapped by the Go
 * go-musicfox/netease-music package; added here for account-level
 * entitlement checks (redVipLevel / redVipExpireTime / musicPackage). */
ne_resp *ne_vip_info(void);

/* LikeListService — weapi/song/like/get {uid} */
ne_resp *ne_like_list(const char *uid);

/* ── write family (phase 6) ──────────────────────────────────────────── */

/* PlaylistSubscribeService — t "1" → weapi/playlist/subscribe, else
 * unsubscribe; data {id} */
ne_resp *ne_playlist_subscribe(const char *id, const char *t);

/* PlaylistTracksService — /api/playlist/manipulate/tracks (rewritten to
 * /weapi/) with {op, pid, trackIds, imme}. NOTE: the Go service does
 * `TrackIds = append(TrackIds, TrackIds...)` which DOUBLES the list — the
 * wire format for a single id is ["<id>","<id>"]; replicated here. */
ne_resp *ne_playlist_tracks(const char *op, const char *pid,
                            const char *track_id);

/* PlaylistCreateService — weapi/playlist/create {name, privacy}; privacy
 * != "10" is forced to "0" (Go behaviour; the shell always passes "0"). */
ne_resp *ne_playlist_create(const char *name, const char *privacy);

/* PlaylistDeleteService — weapi/playlist/remove {ids: "[<id>]"} */
ne_resp *ne_playlist_delete(const char *id);

/* PlaylistNameUpdateService — eapi via http://interface3.music.163.com,
 * options.Url=/api/playlist/update/name, data {id, name} */
ne_resp *ne_playlist_update_name(const char *id, const char *name);

/* ── login family (phase 6) ──────────────────────────────────────────── */

/* LoginEmailService — /api/login (rewritten /weapi/login), extras
 * os=ios/appver=8.7.01, {username, password=md5hex, rememberLogin} */
ne_resp *ne_login_email(const char *email, const char *password);

/* LoginCellphoneService — CallWeapi /weapi/login/cellphone with
 * {phone, countrycode(86), csrf_token, password=md5hex, rememberLogin,
 * type=1, https=true, remember=true} */
ne_resp *ne_login_cellphone(const char *phone, const char *password);

/* LoginRefreshService — ApplyRequestStrategy + csrf from jar, CallWeapi
 * /weapi/login/token/refresh */
ne_resp *ne_login_refresh(void);
#endif
