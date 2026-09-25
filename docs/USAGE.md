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
│ ne_search / ne_song_url_v1 / … 33 个服务函数           │
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
| linuxapi | `/api/`（内层） | 单层 AES-ECB，POST `/api/linux/forward` | ⚠️ 社区活跃项目已弃用（见 §8） |
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

## 4. 服务清单（33 个函数）

"通道"列：**W**=weapi(create_weapi) ｜ **W!**=weapi(call_weapi 严格) ｜ **L**=linuxapi ｜ **E**=eapi。
"登录"列：✓ 需要登录 cookie（MUSIC_U）；✗ 匿名可用；— 登录动作本身。

| 函数 | 端点（重写后实际路径） | 通道 | 参数 | 登录 |
|---|---|---|---|---|
| `ne_search(s,type,limit,offset)` | `/weapi/cloudsearch/pc`（type=2000 走 voice） | W | 空 type/limit/offset → "1"/"30"/"0" | ✗ |
| `ne_check_music(id,br)` | `/weapi/song/enhance/player/url` | W | br 空 → "999000" | ✗ |
| `ne_record_recent(limit)` | `/weapi/play-record/song/list` | W | | ✓ |
| `ne_recommend_resource(void)` | `/weapi/v1/discovery/recommend/resource` | W | | ✓ |
| `ne_song_url_v1(id,level)` | `/weapi/song/enhance/player/url/v1` | **W!** | level 空 → "higher"；"sky" 加 immerseType=c51；encodeType 恒 "flac" | ✗（匿名限低码率） |
| `ne_song_url_old(id,br)` | linuxapi 信封内 `/api/song/enhance/player/url` | **L** | | ✗ |
| `ne_song_download_url(id,level)` | `/weapi/song/enhance/download/url/v1` | **W!** | | ✓（已购） |
| `ne_song_music_quality(id)` | `/weapi/song/music/detail/get` | W | | ✓ |
| `ne_song_purchased(limit,offset)` | `/weapi/single/mybought/song/list` | W | | ✓ |
| `ne_album_purchased(limit,offset)` | `/weapi/digitalAlbum/purchased` | W | | ✓ |
| `ne_album_detail(id)` | `/weapi/v1/album/<id>` | W | body 同时带 id 字段 | ✗ |
| `ne_song_detail(ids_csv)` | `/weapi/v3/song/detail` | W | `c=[{"id":…},…]` | ✗ |
| `ne_playlist_detail(id,s)` | linuxapi 信封内 `/api/v3/playlist/detail` | **L** | n=100000；s 空 → "8"（订阅者数） | ✗ |
| `ne_user_playlist(uid,limit,offset)` | `/weapi/user/playlist` | W | 空 → 30/0 | ✗ |
| `ne_lyric(id)` | linuxapi 信封内 `/api/song/lyric` | **L** | lv/kv/tv=-1 | ✗ |
| `ne_toplist_detail(void)` | `/weapi/toplist/detail` | W | | ✗ |
| `ne_recommend_songs(void)` | `/weapi/v3/discovery/recommend/songs` | W | cookie os=ios | ✓ |
| `ne_recommend_playlists(limit)` | `/weapi/personalized/playlist` | W | | ✗ |
| `ne_user_account(void)` | `/weapi/w/nuser/account/get` | W | 903d337 起（旧路径已失效） | ✓ |
| `ne_vip_info(void)` | `/weapi/music-vip-membership/front/vip/info` | W | | ✓ |
| `ne_like_list(uid)` | `/weapi/song/like/get` | W | | ✓ |
| `ne_playlist_subscribe(id,t)` | `/weapi/playlist/subscribe`/`unsubscribe` | W | t="1" 收藏 "0" 取消 | ✓ |
| `ne_playlist_tracks(op,pid,track_id)` | `/weapi/playlist/manipulate/triacks`（拼错是服务端历史） | W | op=add/del；trackIds 双写为 Go 兼容 quirk | ✓ |
| `ne_playlist_create(name,privacy)` | `/weapi/playlist/create` | W | privacy "0" 公开 "10" 私密 | ✓ |
| `ne_playlist_delete(id)` | `/weapi/playlist/remove` | W | | ✓ |
| `ne_playlist_update_name(id,name)` | eapi `interface3`（HTTP 明文） | **E** | Go v1.6.0 行为复刻 | ✓ |
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

## 8. 已知限制与风险（2026-09 审计结论）

对照活跃权威项目（NeteaseCloudMusicApiEnhanced/api-enhanced，2026-09 仍活跃）：

1. **linuxapi 通道**（playlist_detail / lyric / song_url_old 三个服务）—— api-enhanced 生产模块已全部弃用该通道（仅调试工具保留），libnetease 是锚定 Go v1.6.0 的刻意选择。若 `/api/linux/forward` 被服务端下线，这三个接口最先断。**缓解预案**：playlist_detail 可平移到 weapi + `/api/v6/playlist/detail`（权威现行端点）；lyric 平移 weapi + 补 `rv:-1, _nmclfl:1` 字段。
2. **playlist_detail 版本落后**：libnetease 用 v3（Go 复刻），api-enhanced 已用 v6。
3. **NMTID**：libnetease 复刻 Go 的假值行为（内存中、不落盘）；api-enhanced 改为缓存服务端下发的真 NMTID 并在 eapi 请求里复用（2026-08 修复）。高风控环境下假值可能吃亏，宿主可通过 jar 注入真值覆盖。
4. **song_url_v1 通道**：libnetease 走 weapi；api-enhanced 升级到自研 xeapi（会话密钥协商）并建议高音质时补 android cookie。weapi 路径当前仍工作，属于进阶差异而非故障。
5. **c38c41f 前的双跑基线**：单跑 40/40 用例全绿 + `--go` 双跑逐字节对齐（锚定 Go v1.6.0），协议本身（密钥/加密/URL 重写/JSON 转义）经向量级验证无误。

维护纪律提醒：服务函数行为与 `services.h` 注释如有出入，**以 services.c 实现为准**（例如 `ne_login_refresh`/`ne_record_recent` 注释仍写 CallWeapi，实现已是 create_weapi）。
