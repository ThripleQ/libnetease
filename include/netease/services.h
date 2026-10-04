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

/* RecordRecentSongsService — weapi /play-record/song/list (create_weapi) */
ne_resp *ne_record_recent(const char *limit);

/* RecommendResourceService — 每日推荐歌单 */
ne_resp *ne_recommend_resource(void);

/* SongUrlV1Service — CallWeapi /weapi/song/enhance/player/url/v1;
 * level empty → "higher"; "sky" adds immerseType=c51 */
ne_resp *ne_song_url_v1(const char *id, const char *level);

/* SongUrlService — weapi /song/enhance/player/url, cookie os=pc. 2026-09: was linuxapi */
ne_resp *ne_song_url_old(const char *id, const char *br);

/* SongDownloadUrlService — CallWeapi /weapi/song/enhance/download/url/v1,
 * the official download endpoint (kept separate from the play URL). data
 * {id, level} — id is a single value, not an array; level empty → "standard".
 * Original apiservice: no corresponding Go service, ported from the Go
 * netease-cli shell's song-download-url command. */
ne_resp *ne_song_download_url(const char *id, const char *level);

/* SongMusicQualityService — weapi/song/music/detail/get {songId}: returns
 * the track's per-tier source table (l/m/h/sq/hr/je/sk/jm → {br,size}).
 * Authoritative answer for which levels actually have a source; a nil tier
 * means that level has no file. Pinned from chaunsin/netease-cloud-music. */
ne_resp *ne_song_music_quality(const char *id);

/* SongPurchasedService — api/single/mybought/song/list {limit, offset}:
 * the user's purchased single tracks. Endpoint from
 * Binaryify/NeteaseCloudMusicApi song_purchased. */
ne_resp *ne_song_purchased(const char *limit, const char *offset);

/* AlbumPurchasedService — api/digitalAlbum/purchased {limit, offset, total}:
 * the user's purchased digital albums. Endpoint from
 * Binaryify/NeteaseCloudMusicApi digitalAlbum_purchased. */
ne_resp *ne_album_purchased(const char *limit, const char *offset);

/* AlbumDetailService — weapi/v1/album/{id}: the album's tracks. */
ne_resp *ne_album_detail(const char *id);

/* SongDetailService — weapi/v3/song/detail, cookie os=pc;
 * ids_csv "1,2,3" → c=[{"id":"1"},..], ids=[1,2,3] */
ne_resp *ne_song_detail(const char *ids_csv);

/* PlaylistDetailService — weapi v6/playlist/detail, data
 * {id, n=100000, s} (s empty → "8"). 2026-09: was linuxapi v3. */
ne_resp *ne_playlist_detail(const char *id, const char *s);

/* UserPlaylistService — weapi/user/playlist, limit/offset empty → 30/0 */
ne_resp *ne_user_playlist(const char *uid, const char *limit,
                          const char *offset);

/* LyricService — weapi /song/lyric, cookie os=pc, lv/kv/tv/rv=-1, _nmclfl=1. 2026-09: was linuxapi */
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

/* VipInfoService — CallWeapi /weapi/music-vip-membership/front/vip/info
 * (Binaryify /vip/info, "获取 VIP 信息(app端)"; endpoint path pinned from
 * Binaryify NeteaseCloudMusicApi module/vip_info.js). Not wrapped by the Go
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

/* LoginCellphoneService — weapi (create_weapi) /login/cellphone with
 * {phone, countrycode(86), csrf_token, password=md5hex, rememberLogin,
 * type=1, https=true, remember=true} */
ne_resp *ne_login_cellphone(const char *phone, const char *password);

/* SendCaptchaService — weapi/captcha/sent {cellphone, ctcode}: send the SMS
 * login code (prerequisite of captcha login). Endpoint from Binaryify
 * /captcha/sent; no Go v1.6.0 counterpart (added for the SMS login flow). */
ne_resp *ne_send_captcha(const char *phone, const char *countrycode);

/* LoginCellphoneService captcha mode — /weapi/login/cellphone with
 * {phone, countrycode, captcha, rememberLogin, type, https, remember,
 * csrf_token}; logs in with the SMS code instead of the password. */
ne_resp *ne_login_cellphone_captcha(const char *phone, const char *captcha,
                                    const char *countrycode);

/* LoginRefreshService — ApplyRequestStrategy + csrf from jar, weapi (create_weapi)
 * /weapi/login/token/refresh */
ne_resp *ne_login_refresh(void);

/* ── artist family ───────────────────────────────────────────────────── */

/* ArtistDetailService — weapi/v1/artist/{id}: {artist, hotSongs, more}.
 * One call yields the artist profile plus its top 50 hot songs. */
ne_resp *ne_artist_detail(const char *id);

/* ArtistSongsService — weapi/v1/artist/songs {id, offset, limit, order};
 * order empty → "hot" (else "time"). Paged full song list. */
ne_resp *ne_artist_songs(const char *id, const char *offset,
                         const char *limit, const char *order);

/* ArtistAlbumsService — weapi/artist/albums/{id} {limit, offset, total}:
 * the artist's albums. total empty → "true". */
ne_resp *ne_artist_albums(const char *id, const char *limit,
                          const char *offset);

/* ArtistDescService — weapi/artist/introduction {id}: {introduction:[...],
 * briefDesc, ...}. */
ne_resp *ne_artist_desc(const char *id);

/* ── podcast / radio family ──────────────────────────────────────────── */

/* DjRadioDetailService — weapi/djradio/get {id} (v1; v2 returns 404). */
ne_resp *ne_radio_detail(const char *id);

/* DjProgramListService — weapi/dj/program/byradio {radioId, limit, offset,
 * asc} with NUMERIC radioId/limit/offset and a JSON boolean asc (string
 * values are rejected with code 400). Paged program list of a radio. */
ne_resp *ne_radio_programs(const char *radio_id, const char *limit,
                           const char *offset, const char *asc);

/* ── comment family ──────────────────────────────────────────────────── */

/* CommentService — weapi/v1/resource/comments/{thread_id} {rid, offset,
 * limit, beforeTime}. thread_id is the resource thread id:
 *   song     R_SO_4_<id>
 *   album    R_AL_3_<id>
 *   playlist A_PL_0_<id>   (R_SQ_2_ returns empty)
 *   program  R_VI_62_<id>  (server currently returns empty for these)
 * beforeTime empty → "0". */
ne_resp *ne_comments(const char *thread_id, const char *limit,
                     const char *offset, const char *before_time);

/* CommentHotService — weapi/v1/resource/hotcomments/{thread_id}, same data
 * shape; the "hot" comments tab of the same thread. */
ne_resp *ne_comments_hot(const char *thread_id, const char *limit,
                         const char *offset, const char *before_time);

/* ── explore / homepage family (added 2026-10-04) ─────────────────────
 * 第三方客户端「探索页」需要的两个官方数据源：首页龙珠入口 + 曲风(style-tag)
 * 体系。端点钉自 api-enhanced 现行 module（Binaryify 已归档）。
 * 全部 weapi；dragon ball 需登录态，未登录返回空数组 —— 调用方必须 fallback。 */

/* HomepageDragonBallService — /api/homepage/dragon/ball/static，空 data。
 * 官方客户端首页「发现」页顶部那排圆形入口（每日推荐 / 歌单 / 排行榜 /
 * 私人 FM…）。移动端接口，故 cookie 带 os=ios。未登录返回 {"data":[]}。 */
ne_resp *ne_dragon_ball(void);

/* StyleListService — /api/tag/list/get，空 data：曲风标签总表（tagId + 名称）。 */
ne_resp *ne_style_list(void);

/* StyleSongService — /api/style-tag/home/song {cursor,size,tagId,sort}。
 * 四个字段在 wire 上都是 JSON **数字**（传字符串会被服务端拒），所以走
 * jmap_put_int 而不是 jmap_put。 */
ne_resp *ne_style_song(const char *tag_id, const char *size,
                       const char *cursor);

/* StylePlaylistService — /api/style-tag/home/playlist {cursor,size,tagId,sort}，
 * 同样是数字字段；sort 固定 0。 */
ne_resp *ne_style_playlist(const char *tag_id, const char *size,
                           const char *cursor);

/* RadioService（私人漫游 / 私人 FM）— weapi /api/v1/radio/get。
 * mode/subMode 是官方客户端漫游页那排「听歌模式」；两者同样是 JSON 数字。
 * 传空 mode 表示「默认模式」，此时整包不带 mode/subMode，等价于上游
 * personal_fm.js 的空 data —— 官方 App 的默认值，比猜一个 mode 安全。
 * 每次调用返回一批不同的歌（服务端按口味随机出，无分页），需登录。 */
ne_resp *ne_radio_get(const char *mode, const char *sub_mode,
                      const char *limit);

/* PlaylistCatalogueService — /weapi/playlist/catalogue，空 data。歌单分类总表
 * （categories: 0 语种 / 1 风格 / 2 场景 / 3 情感 / 4 主题，sub[] 是标签）。
 * 标签没有封面（imgUrl 恒 null），卡片封面要另取 —— 见 ne_playlist_list。 */
ne_resp *ne_playlist_catalogue(void);

/* PlaylistListService — /weapi/playlist/list {cat, order, limit, offset, total}
 * （分类歌单；**不是** /top/playlist，那个路径已 404）。给一个标签名（清晨 /
 * 伤感 / 治愈…）返回该标签下的热门歌单，用来给「场景音乐」的标签卡配封面。 */
ne_resp *ne_playlist_list(const char *cat, const char *limit,
                          const char *offset);

/* HomepageBlockPageService — /api/homepage/block/page {refresh, cursor}，
 * 移动端首页「发现」页的整条 block 流。**「雷达歌单」就在这里**：
 *   data.blocks[].blockCode == "HOMEPAGE_BLOCK_MGC_PLAYLIST"
 * 的 `creatives[]` 是一批官方雷达歌单（私人雷达 / 新歌雷达 / 会员雷达 /
 * 乐迷雷达 / 宝藏雷达…，2026-10-04 探针实测稳定 6 张）；每张卡取
 * `creativeId` = 歌单 id、`uiElement.mainTitle.title` = 名称、
 * `uiElement.image.imageUrl` = 封面。
 *
 * 这是 weapi 侧**唯一**的雷达入口：任何路径里带 radar 字样的独立端点
 * （/api/radar/... 等 7 个候选）全部 404，api-enhanced 439 个 module 里
 * 也没有 radar —— 它只作为 block 流里的一个 block 存在，不单独暴露。
 * refresh 非 0 = 强制刷新（服务端重新出卡）；cursor 首次传 "-1"。 */
ne_resp *ne_homepage_block_page(const char *refresh, const char *cursor);

/* SimiArtistService — /weapi/discovery/simiArtist {artistid}：**相似歌手**。
 * 传入一个歌手 id，返回一批相似歌手（`artists[]`，每项含 id/name/picUrl/
 * albumSize 等）。kanade 主页那张「相似艺人 / 从你喜欢的艺人听起」卡
 * （它源码里的标识是 `artist_fm`）就走这条链路 —— 种子歌 → 歌手 → 相似歌手
 * → 相似歌手的热门歌。**这是真正对得上语义的接口**：以前我们拿「同一歌手的
 * 热门歌」冒充相似艺人，播出来永远是种子歌手自己的歌。 */
ne_resp *ne_simi_artist(const char *artist_id);
#endif
