# libnetease 使用说明

`libnetease` 是 `go-musicfox/netease-music` v1.6.0 请求内核 + `netease-cli` 壳的 C 语言移植（C11，零第三方运行时依赖；桌面构建可选 libcurl）。

- 产物：`libnetease.a`（静态库）+ `netease-cli`（可执行文件）
- 协议：weapi / linuxapi / eapi 三通道，加密原语自带（AES-128 / MD5 / RSA-1024 无填充 / base64 / hex）
- 定位：netune（桌面）/ nume（Android）共用的网易云数据网关
- 风控对策与上游生态调研见 [docs/RISKS.md](RISKS.md)，交接状态见 [../HANDOFF.md](../HANDOFF.md)

---

## 1. 架构总览

```
┌─ 服务层 (src/service/) ────────────────────────────────┐
│ ne_search / ne_song_url_v1 / … 52 个服务函数           │
│ 每个函数 = 一次 HTTP 往返，返回原始 ne_resp             │
├─ 请求内核 (src/core/request.c) ────────────────────────┤
│ 通道①weapi(create_weapi)  通道②linuxapi  通道③eapi      │
│ 加密 → csrf/jar 合成 → /\w*api/ URL 重写 → post_common  │
│ 全局 cookie jar（内部 mutex，HTTP 期间不持锁）           │
├─ HTTP 层 (src/core/http.c) ────────────────────────────┤
│ 默认传输 = libcurl（NE_USE_CURL=ON 桌面默认）           │
│ 无 curl 平台（Android NDK）：宿主注入 transport 回调     │
├─ 加密原语 (src/vendor/) ───────────────────────────────┤
│ AES / MD5 / RSA(noPadding) / bignum / base64 / hex     │
└────────────────────────────────────────────────────────┘
```

三条加密通道（URL 里第一个以 `api` 结尾的 path 段会被重写）：

| 通道 | 重写目标 | 加密 | 现状 |
|---|---|---|---|
| weapi | `/weapi/` | 双层 AES-CBC + RSA 加密随机 key | 主力通道 |
| linuxapi | `/api/`（内层） | 单层 AES-ECB，POST `/api/linux/forward` | ⚠️ 上游已弃用，2026-09 起**无服务在用**（仅保留通道代码，见 §8） |
| eapi | `/eapi/` | AES-ECB + header 反欺诈对象 | 仅 playlist_update_name 在用 |

## 2. 构建

### 2.1 桌面（CLI + 静态库）

```sh
./tests/run_tests.sh        # 重新生成向量 + cmake 构建 + ctest
# 或标准流程：
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

依赖：CMake ≥ 3.16、libcurl（默认 ON）、zlib（可选）。产物：`build/netease-cli`、`libnetease.a`。

### 2.2 嵌入式 / Android（transport 注入）

平台没有 curl 时（Android NDK），**编译期关掉 curl，运行时由宿主注入传输回调**，请求内核不变。nume 即此模式（参考 `nume/app/src/main/cpp/CMakeLists.txt` 与 `libnetease_jni.c`）：

```cmake
# 宿主的 CMakeLists.txt
set(NE_USE_CURL OFF CACHE BOOL "no curl" FORCE)   # 必须在 add_subdirectory 前设置
add_subdirectory(<path-to-libnetease> libnetease-build EXCLUDE_FROM_ALL)
add_library(my_jni SHARED my_jni.c)
target_link_libraries(my_jni PRIVATE netease)
```

传输回调签名（`include/netease/http.h`）：

```c
typedef ne_http_resp *(*ne_transport_req_fn)(
    const char *url, const char *method,          /* "POST"/"GET" */
    const char *body, const char *content_type,   /* 仅 POST 用 */
    const char *cookie_header,                    /* 可为 NULL */
    const char *user_agent);

typedef struct { ne_transport_req_fn request; } ne_http_transport;

ne_http_resp {                                    /* 回调必须 malloc 返回，永不返回 NULL */
    long   status;        /* HTTP 状态码；传输错误置 0 */
    char  *body;          /* malloc'd，NUL 结尾 */
    size_t body_len;
    char  *err;           /* malloc'd 错误串，成功为 NULL */
    char  *set_cookies;   /* Set-Cookie 按行 "name=value\n"，必须由回调捕获 */
};
```

注意两点：
- **回调没有 `user_data` 参数** —— 宿主状态只能通过全局变量传递（nume 的 JNI shim 用全局 `JavaVM*` attach 当前线程取 JNIEnv）。
- **Set-Cookie 必须捕获进 `resp->set_cookies`**，请求内核直接从响应对象消费，无全局回传通道。登录态（MUSIC_U / __csrf 等）全靠它回写 jar。

注册：`ne_http_set_transport(&my_transport)` —— **必须在发出任何请求之前调用**；传 NULL 恢复默认 curl。

## 3. C API 快速上手

最小桌面示例：

```c
#include <netease/services.h>

int main(void) {
    ne_set_cookie_file("~/.cache/netune/cookies.txt"); /* CLI 自动做；嵌入宿主自行调用 */
    ne_resp *r = ne_search("hello", "1", "30", "0");
    if (r->err == 0 && r->code == 200) {
        printf("%.*s\n", (int)r->body_len, r->body);
    }
    ne_resp_free(r);   /* body 同体释放 */
    return 0;
}
```

### 3.1 初始化契约（调用顺序）

| 顺序 | 调用 | 谁必须 | 说明 |
|---|---|---|---|
| 1 | `ne_http_set_transport(cb)` | 无 curl 平台 | 任何请求前；NULL 恢复 curl |
| 2 | `ne_set_api_base(base)` | **嵌入式宿主（Android 等）** | Android 无环境变量可用，`request.h` 要求启动时显式调用。默认 `https://music.163.com`；入参被内部复制（strdup），调用后可释放 |
| 3 | `ne_set_cookie_file(path)` | 长期进程 | 指定 Netscape cookies.txt 位置并 reload；CLI 默认 `~/.cache/netune/cookies.txt` |
| 4 | `ne_jar_import_cookies(str)` | 可选 | 合并浏览器导出的 `"MUSIC_U=…; __csrf=…"` 串并落盘（线程安全）；`ne_jar_reload()` 可再重读文件 |
| 5 | 风控开关（§6） | 可选 | setter / 环境变量 |

### 3.2 `ne_resp` 语义（重点，两条路径行为不同）

```c
typedef struct {
    double code;        /* API 业务 code，语义见下表 */
    long   http_status; /* 原始 HTTP 状态码，传输错误为 0 */
    char  *body;        /* malloc'd 响应体 */
    size_t body_len;
    int    err;         /* 0 成功；1 传输错误；2 body 无顶层数字 code 字段 */
} ne_resp;
```

| 场景 | code | err | 判定建议 |
|---|---|---|---|
| `ne_create_weapi` / linuxapi / eapi，成功且 body 有 `"code"` | 业务 code | 0 | `code==200` 成功 |
| 同上，成功但 body 无 `"code"` 字段 | **200（fallback）** | 0 | 按 err==0 处理，**别拿 code 判** |
| 同上，传输失败 | **520** | 1 | 网络错误，可重试 |
| `ne_call_weapi`（song_url_v1 / song_download_url 专用），传输失败 | **0** | 1 | 网络错误 |
| `ne_call_weapi`，成功但 body 无数字顶层 code | **0** | 2 | body 仍可能正常 |
| `ne_call_weapi`，成功且有 code | 业务 code | 0 | 常规 |

> **坑位提醒**：判断成败请优先用 `err`，其次才看 `code`。严格路径（song_url_v1 系）的失败 code=0 与"成功但无字段"的 code=0 无法用 code 区分 —— 这是 nume 早期踩过的坑（ProfileRepository 的 workaround 注释即源于此）。
> `code` 字段扫描取 body 中**第一个** `"code":` 出现（jsonparser 语义），极少数 code 排在嵌套对象之后的响应可能取到嵌套值 —— 依赖 err 而非 code 可规避。

### 3.3 内存所有权

| 对象 | 归属 | 释放 |
|---|---|---|
| `ne_resp`（含 body） | 调用方 | `ne_resp_free(r)` |
| `ne_http_resp`（transport 返回） | 请求内核 | `ne_http_resp_free`（内核消费） |
| `ne_qr_get_key` 的 unikey / body_out | 调用方 | `free()` |
| `ne_generate_chain_id()` | 调用方 | `free()` |
| `ne_set_api_base` / `ne_set_cookie_file` 入参 | 调用方 | 内部 strdup，调用后即可释放 |

### 3.4 线程契约（request.h 原文要点）

- 全局 cookie jar 由**内部 mutex** 保护；多线程并发调用服务函数**安全且并行**（HTTP I/O 不持锁，c38c41f 起）。
- 例外：`ne_global_jar()` 返回**活句柄**，仅供 CLI 这种单线程宿主在进程退出时持久化；多线程宿主不要在调用返回后继续使用该句柄。
- `ne_jar_import_cookies()` 线程安全（jar 锁 + 落盘在锁内），可随时调。
- Set-Cookie 消费发生在响应对象上，无共享回传通道 —— 并发请求各自的 cookie 回写互不干扰。

## 4. 服务清单（52 个函数，按家族分组）

"通道"列：**W**=weapi(create_weapi) ｜ **W!**=weapi(call_weapi 严格) ｜ **L**=linuxapi ｜ **E**=eapi。
"登录"列：✓ 需要登录 cookie（MUSIC_U）；✗ 匿名可用；— 登录动作本身。

新增的歌手 / 电台 / 评论 / 探索页四大家族端点（含 `services.h` 里的参数默认值）已按
`src/service/services.c` 实录；探索页一族端点钉自 api-enhanced 现行 module
（`ckylinmc/neteasecloudmusicapi`），与上游逐module核对过（见 §8 备注）。

### 4.1 搜索 / 歌曲（含已购）

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_search(s,type,limit,offset)` | `/weapi/cloudsearch/pc`（type=2000 走 voice） | W | 空 type/limit/offset → "1"/"30"/"0" | ✗ |
| `ne_check_music(id,br)` | `/weapi/song/enhance/player/url` | W | br 空 → "999000" | ✗ |
| `ne_song_url_v1(id,level)` | `/weapi/song/enhance/player/url/v1` | **W!** | level 空 → "higher"；"sky" 加 immerseType=c51；encodeType 恒 "flac" | ✗（匿名限低码率） |
| `ne_song_url_old(id,br)` | `/weapi/song/enhance/player/url` | W | 2026-09 起（原 linuxapi，与 check_music 同端点同参数） | ✗ |
| `ne_song_download_url(id,level)` | `/weapi/song/enhance/download/url/v1` | **W!** | level 空 → "standard" | ✓（已购） |
| `ne_song_music_quality(id)` | `/weapi/song/music/detail/get` | W | 单曲各音质档 source 表（l/…/jm），钉自 chaunsin | ✓ |
| `ne_song_purchased(limit,offset)` | `/weapi/single/mybought/song/list` | W | 已购单曲 | ✓ |
| `ne_album_purchased(limit,offset)` | `/weapi/digitalAlbum/purchased` | W | 已购数字专辑 | ✓ |
| `ne_song_detail(ids_csv)` | `/weapi/v3/song/detail` | W | ids "1,2,3" → c=[{"id":…}] | ✗ |
| `ne_lyric(id)` | `/weapi/song/lyric` | W | lv/kv/tv/rv=-1，_nmclfl=1；2026-09 起（原 linuxapi） | ✗ |

### 4.2 歌单 / 榜单 / 发现

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_playlist_detail(id,s)` | `/weapi/v6/playlist/detail` | W | n=100000；s 空 → "8" | ✗ |
| `ne_user_playlist(uid,limit,offset)` | `/weapi/user/playlist` | W | 空 → 30/0 | ✗ |
| `ne_toplist_detail(void)` | `/weapi/toplist/detail` | W | | ✗ |
| `ne_recommend_resource(void)` | `/weapi/v1/discovery/recommend/resource` | W | | ✓ |
| `ne_recommend_songs(void)` | `/weapi/v3/discovery/recommend/songs` | W | cookie os=ios | ✓ |
| `ne_recommend_playlists(limit)` | `/weapi/personalized/playlist` | W | {limit, order=true, n=1000} | ✗ |
| `ne_playlist_list(cat,limit,offset)` | `/weapi/playlist/list` | W | cat 空 → "全部"；order=hot；limit 空 → 6（=api-enhanced `top_playlist.js`，**非** /top/playlist） | ✗ |
| `ne_playlist_catalogue(void)` | `/weapi/playlist/catalogue` | W | 歌单分类总表（=api-enhanced `playlist_catlist.js`） | ✗ |

### 4.3 专辑 / 艺人

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_album_detail(id)` | `/weapi/v1/album/<id>` | W | 专辑详情（含曲目） | ✗ |
| `ne_album_subscribe(id,t)` | `/weapi/album/sub`/`unsub` | W | t="1" 收藏 "0" 取消（2026-10-06 新增） | ✓ |
| `ne_album_sublist(limit,offset)` | `/weapi/album/sublist` | W | 已收藏专辑；total 恒 JSON 布尔 true；limit 空 → "100"。**专辑详情里没有 subscribed 字段**，判收藏态只能靠它（2026-10-06 新增） | ✓ |
| `ne_artist_detail(id)` | `/weapi/v1/artist/<id>` | W | {artist, hotSongs, more}，一次取艺人+Top50热歌 | ✗ |
| `ne_artist_songs(id,offset,limit,order)` | `/weapi/v1/artist/songs` | W | order 空 → "hot"（else "time"） | ✗ |
| `ne_artist_albums(id,limit,offset)` | `/weapi/artist/albums/<id>` | W | total 恒 "true" | ✗ |
| `ne_artist_desc(id)` | `/weapi/artist/introduction` | W | | ✗ |

### 4.4 电台 / 播客

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_radio_detail(id)` | `/weapi/djradio/get` | W | **v1**（v2 返回 404） | ✗ |
| `ne_radio_programs(radio_id,limit,offset,asc)` | `/weapi/dj/program/byradio` | W | 首三参需 JSON **数字**、asc 需 JSON 布尔（字符串被拒 code 400） | ✗ |

### 4.5 评论

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_comments(thread_id,limit,offset,before_time)` | `/weapi/v1/resource/comments/<thread_id>` | W | thread 前缀：song `R_SO_4`/album `R_AL_3`/playlist `A_PL_0`/program `R_VI_62` | ✗ |
| `ne_comments_hot(thread_id,limit,offset,before_time)` | `/weapi/v1/resource/hotcomments/<thread_id>` | W | 同侪「热门」标签 | ✗ |
| `ne_comment_like(thread_id,comment_id,like)` | `/weapi/v1/comment/like`/`unlike` | W | like "1" 点赞，否则取消；threadId 与 `ne_comments` 同前缀（2026-10-06 新增） | ✓ |

### 4.6 探索页 / 首页（2026-10-04 新增，api-enhanced 现行 module）

> 这批端点钉自 `ckylinmc/neteasecloudmusicapi`（api-enhanced 一线，Binaryify 已归档）；**不参与**
> dualrun 的 Go 逐字节比对（Go v1.6.0 无此批）。全部 weapi。

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_dragon_ball(void)` | `/api/homepage/dragon/ball/static` | W | 首页「发现」顶部圆形入口；移动端 → cookie os=ios；未登录返回 data=[] | ✗ |
| `ne_style_list(void)` | `/api/tag/list/get` | W | 曲风标签总表（=api-enhanced `style_list.js`） | ✗ |
| `ne_style_song(tag_id,size,cursor)` | `/weapi/style-tag/home/song` | W | cursor/size/tagId/sort 全为 JSON **数字**（=`style_song.js`） | ✗ |
| `ne_style_playlist(tag_id,size,cursor)` | `/weapi/style-tag/home/playlist` | W | 同上，sort 恒 0（=`style_playlist.js`） | ✗ |
| `ne_radio_get(mode,sub_mode,limit)` | `/api/v1/radio/get` | W | 私人漫游；mode 空 → 整包不带 mode/subMode（=`personal_fm.js`）；需登录 | ✓ |
| `ne_homepage_block_page(refresh,cursor)` | `/api/homepage/block/page` | W | 首页 block 流；「雷达歌单」=DATA.blocks[] where blockCode`HOMEPAGE_BLOCK_MGC_PLAYLIST`；refresh 仅显式 "true"/"1" 为真；cursor 留空（其 /weapi/ 路由反 50002） | ✗ |
| `ne_simi_artist(artist_id)` | `/weapi/discovery/simiArtist` | W | 字段名小写 `artistid`（写 artistId 服务端不当参数）；相似歌手（=api-enhanced `simi_artist.js`） | ✗ |

### 4.7 红心 / 账户 / 写操作

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_like_list(uid)` | `/weapi/song/like/get` | W | 红心歌曲清单 | ✓ |
| `ne_song_like(track_id,like)` | `/weapi/song/like` | W | like 字符串 "true"/"false" + `os=pc appver=2.7.1.198277`，**与 CLI 的 `like` 命令同方言**（2026-10-06 新增）；成功返 `{"playlistId":<"我喜欢的音乐"id>}`，注意它不是 uid | ✓ |
| `ne_record_recent(limit)` | `/weapi/play-record/song/list` | W | limit 空 → "100" | ✓ |
| `ne_user_account(void)` | `/weapi/w/nuser/account/get` | W | 903d337 起（旧路径已失效） | ✓ |
| `ne_vip_info(void)` | `/weapi/music-vip-membership/front/vip/info` | **W!** | 钉自 Binaryify `vip_info.js`；redVipLevel/redVipExpireTime/musicPackage | ✓ |
| `ne_playlist_subscribe(id,t)` | `/weapi/playlist/subscribe`/`unsubscribe` | W | t="1" 收藏 "0" 取消 | ✓ |
| `ne_playlist_tracks(op,pid,track_id)` | `/weapi/playlist/manipulate/triacks`（拼错是服务端历史） | W | op=add/del；trackIds 双写为 Go 兼容 quirk | ✓ |
| `ne_playlist_create(name,privacy)` | `/weapi/playlist/create` | W | privacy "0" 公开 "10" 私密（其余强制 "0"，Go 行为） | ✓ |
| `ne_playlist_delete(id)` | `/weapi/playlist/remove` | W | | ✓ |
| `ne_playlist_update_name(id,name)` | eapi `interface3`（HTTP 明文） | **E** | Go v1.6.0 行为复刻 | ✓ |

### 4.8 登录

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_login_email(email,password)` | `/weapi/login` | W | 密码先 MD5；cookie os=ios | — |
| `ne_login_cellphone(phone,password)` | `/weapi/w/login/cellphone` | W | 023149c 起（含 secureCaptcha） | — |
| `ne_login_refresh(void)` | `/weapi/login/token/refresh` | W | csrf 取自 jar | — |
| `ne_send_captcha(phone,cc)` | `/weapi/sms/captcha/sent` | W | 023149c 起（含 secrete） | — |
| `ne_login_cellphone_captcha(phone,captcha,cc)` | `/weapi/w/login/cellphone` | W | cc 空 → "86" | — |

二维码登录（`include/netease/qr.h`）：

| 函数 | 说明 |
|---|---|
| `ne_qr_get_key(&code,&body,&len)` | 返回 malloc'd unikey；unikey 空 = 失败 |
| `ne_qr_check(unikey,&code,&len)` | 轮询：800 待扫码 / 801 已过期 / 802 待确认 / 803 登录成功（cookie 已进 jar） |
| `ne_qr_build_url(unikey)` | 拼二维码内容 `http://music.163.com/login?codekey=…&chainId=…` |

## 5. CLI 参考（42 命令，与 Go 版协议一致）

登录：`qr-key` `qr-check` `qr-render <key> <file.png>` `qr-image <key>` `login-email` `login-cellphone` `login-refresh` `login-status`
读：`search <kw> [type]` `search-pl <kw>` `check-music <id>` `record-recent [n]` `recommend-resource` `recommend-songs` `recommend-playlists [n]` `song-url <id> [level]` `song-download-url <id> [level]` `song-music-quality <id>` `check-quality <id> <level>` `song-purchased [limit offset]` `album-purchased [limit offset]` `album <id>` `song-detail <ids…>` `playlist <id>` `playlist-cover <id>` `user-playlist <uid>` `liked` `liked-check <song_id>` `toplist` `lyric <id>` `playlist-tracks <id>` `account-name` `account-info` `vip-info` `playlists`
写：`like <id> <like\|unlike>` `subscribe <id> <t>` `track-add <pid> <sid>` `track-del <pid> <sid>` `playlist-create <name>` `playlist-rename <pid> <name>` `playlist-delete <pid>`

CLI 行为：启动时 load `~/.cache/netune/cookies.txt`，退出时持久化（假 NMTID 被 filterJar 过滤不落盘）。stdout 输出协议与 Go 版逐字节对齐。

> 注意：**短信验证码登录（send_captcha / login_cellphone_captcha）只有 C API，CLI 未暴露命令**。

## 6. 运行时开关

环境变量（CLI / 桌面宿主）与 C setter 等价关系：

| 作用 | 环境变量 | C API | 默认 |
|---|---|---|---|
| API 基址 | `NE_API_BASE` | `ne_set_api_base()` | `https://music.163.com` |
| 伪造国内出口 IP | `NE_REAL_IP=<ip>` | `ne_http_set_real_ip()` | 关 |
| 每请求随机国内 IP | `NE_RANDOM_CN_IP=1` | `ne_http_set_random_cn_ip()` | 关（nume 在 JNI_OnLoad 里开了） |
| 请求节流 + 抖动 | `NE_RATE_LIMIT_MS` / `NE_RATE_LIMIT_JITTER_MS` | `ne_http_set_rate_limit()` | 关 |
| 关连接复用 | `NE_NO_KEEPALIVE=1` | `ne_http_set_no_keepalive()` | 开 keepalive |
| PC UA 轮换 | `NE_UA_ROTATE=1` | —（仅环境变量） | 固定 UA_PC |
| 补浏览器标准头 | `NE_BROWSER_HEADERS=1` | —（仅环境变量） | 关 |
| 显式 HTTP/2 | `NE_HTTP2=1` | —（仅 curl 构建） | 关 |
| 风控退避重试 | `NE_RETRY_RISK=<n>` | `ne_set_risk_retry()` | 关 |
| cookie 文件路径 | — | `ne_set_cookie_file()` | CLI: `~/.cache/netune/cookies.txt` |

风控分类辅助（`risk.h`）：`ne_risk_classify(r)` → -460 频控 / -462·8821 需行为验证 / 登录类；`ne_risk_is_transient()` 判断是否值得退避重试；`ne_risk_is_empty_body()` 判空 body。常用组合见 README"风控应对"节。

## 7. Cookie 与登录态

- 持久化格式：Netscape cookies.txt（`# Netscape HTTP Cookie File` 头）。
- 登录态核心 cookie：`MUSIC_U`（长效登录令牌）、`__csrf`；服务端下发的 `NMTID` 参与风控（见 §8）。
- 桌面：CLI 自动 load/save。嵌入式：`ne_set_cookie_file()` 指到应用私有目录，`ne_jar_import_cookies()` 从 WebView 导出的 cookie 串导入（nume 的网页登录即此方案）。
- `filterJar` 语义（cookiejar.c）：反欺诈策略写入的**假 NMTID**（`some_random_id_from_strategy`）与 cookie 属性段（`Expires=` 等带 = 的部分）不会进入 jar / 不会落盘。

## 8. 已知限制与风险（2026-10-05 审计，对照权威上游在线核对）

1. **通道现状**：linuxapi 已全部移出请求路径（playlist_detail → weapi+v6，lyric/song_url_old → weapi）；song_url_v1 仍走 weapi，api-enhanced 另有 xeapi（会话密钥协商）进阶方案，高音质场景可关注。
2. **NMTID**：反欺诈策略仅在 jar 无 NMTID 时注入假值（不落盘）；服务端下发的真值优先复用，对齐 api-enhanced 2026-08 行为。
3. **协议正确性**：四把密钥、RSA 公钥、URL 重写、JSON 转义经向量级测试 + 差分双跑验证（52 用例）。
4. **探索页一族端点核对**（2026-10-05 对 `ckylinmc/neteasecloudmusicapi` 逐 module 核对）：
   - `ne_dragon_ball` / `ne_homepage_block_page`：上游 module 声明 `crypto='eapi'`，本库用 **weapi**（`ne_create_weapi`）。实测 weapi 可正常取数；若未来遇 -462/空 body 风控增强，可对照上游改走 eapi 通道（linuxapi/eapi 通道代码已在核心，仅需换转发封装）。
   - `ne_style_song` / `ne_style_playlist`：上游 `crypto='weapi'`，与库一致；sort 在 `style_playlist.js` 上游写死 0，与本库一致。
   - `ne_playlist_list` / `ne_playlist_catalogue`：对应上游 `top_playlist.js` / `playlist_catlist.js`（均 weapi），与库一致。
   - `ne_comments` / `ne_comments_hot`：上游现行 module 已迁移到 **v2 eapi**（`/api/v2/resource/comments`，threadId 放 body、分页用 pageNo/pageSize/cursor）；本库仍是 **v1 weapi**（threadId 放路径、offset+beforeTime 分页）。v1 仍可正常返回，但对齐上游的长期方向是 v2。thread 前缀（`R_SO_4_`/`A_PL_0_` 等）两种版本一致，调用方无感。
   - `ne_radio_get`：无独立上游 module（上游把它塞进 `personal_fm.js` 的 `/api/v1/radio/get` 空 data）；本库模式（mode/subMode 扩展 + cookie os=ios）为 nume 侧增强，端点同源可行。
5. **点赞 / 收藏一族（2026-10-06 新增，登录态探针逐条实测）**：
   - `ne_song_like`：不带 `userid` 也能用（上游 uid 缺省时该字段被丢掉），所以本函数签名里没有 uid。
     成功返 `{"playlistId": 7238603648, "code":200}` —— **那个 playlistId ≠ uid**（实测 uid 6393271458 对应歌单 7238603648），
     别拿 uid 当「我喜欢的音乐」的 id。
   - **失败藏在 message 里**：对下架歌曲点赞，服务端回 HTTP 200 + `{"code":401,"message":"下架歌曲无法收藏"}`。
     调用方只看 `err` / 200 判不出任何东西 —— nume 侧统一由 `InteractionRepository` 把 message 抽出来给用户看。
   - `ne_album_sublist` / `ne_song_like` / `ne_comment_like` / `ne_album_subscribe` 四条都是**「设成目标态」而非「切换」**，
     重复调幂等（对已取消的歌再调 unlike 仍返 200），客户端可以乐观更新 + 失败回滚，不必先读后写。
   - `ne_album_sublist` 单页 limit 100：它是给「本地查表判收藏态」用的，不是收藏管理页；收藏专辑超过 100 张的账号会漏判。

服务行为与 `services.h` 注释有出入时**以 services.c 为准**。
