# libnetease

`go-musicfox/netease-music` v1.6.0 + 其 `netease-cli` 壳的 **C 语言移植**。
产物与 Go 版同名同协议：把 C 版 `netease-cli` 放到 netune 旁即可无缝替换，netune 零改动。

## 结构

```
include/netease/   公共头
src/vendor/        零依赖加密原语：AES-128 / MD5 / RSA(1024bit 无填充) / base64 / hex / 大数
src/core/          请求内核：weapi/eapi 加密管线、Go 语义 JSON map、随机数
src/service/       API 端点（按 Go service/ 家族对应）
src/cli/           netease-cli 壳，stdout 协议 1:1 对齐 Go 版
tests/             向量测试 + verify_qr.py（独立 QR 解码校验）+ dualrun.py（差分对照，40 用例）
```

## 构建 / 测试

```sh
./tests/run_tests.sh          # 重新生成向量 + cmake 构建 + ctest
```

依赖：CMake ≥ 3.16、libcurl（阶段 2 起）。

## 移植纪律

- **Go 源码是唯一规格**：锚定 v1.6.0，不追上游。
- **双跑对照代替单测**：固定随机 key 后与 Python 镜像（逐行翻译自 cryto.go）比对输出。
- **坑位显式化**：Go 特有行为（见下）全部写成注释 + 测试用例。

## 已知 Go 行为坑位（移植时必须保留）

| 坑位 | 位置 | 说明 |
|------|------|------|
| `reSecretKey` 不是 reverse | cryto.go NewLen16Rand | 循环里取两次独立随机，名字有误导性 |
| RSA 结果去前导零 | big.Int.Bytes() | hex 长度可能 < 256 |
| JSON 键排序 + HTML 转义 | json.Marshal(map) | `<` `>` `&` 转成 `\u003c` 等（shell 的 qr-key 手拼 JSON 就是为绕开它） |
| cookie 默认 `os=ios` | request.go | 未登录时 UA 相关字段按 iOS 9.0.65 填 |
| login URL 注入随机 NMTID | request.go | 仅请求级，壳落盘时过滤（防假 NMTID 污染） |
| weapi 前置 csrf_token | request.go | 加密前注入，密文里生效 |
| 响应 zlib 解压 | request.go | libcurl 用 CURLOPT_ACCEPT_ENCODING 等价处理 |
| trackIds 自我翻倍 | playlist_tracks_service.go | `append(x, x...)` 把列表拼两遍，单 id 上线路式为 `["id","id"]` |
| eapi requestId 非 ms | request.go | `Unix()*1000 + rand(0..999)`：秒×1000 拼随机数，不是真实毫秒 |
| 设备池永远取不到最后一个 | request.go | `rand.Intn(len(deviceIds)-1)`；24641 个 ID 按固定步长 52 嵌入 |
| playlist-rename 走 interface3 | playlist_name_update_service.go | `http://interface3.music.163.com/eapi/...`（明文 http，其余端点均 https） |
| CallWeapi 不注入 cookie | request.go | 与 CreateRequest 不同：只带 jar cookie，无 os/appver/__remember_me/NMTID |

## 进度

- [x] 阶段 0：项目骨架（CMake 双产物：libnetease.a + netease-cli）
- [x] 阶段 1：加密原语 + 向量测试全绿（md5/AES/jmap转义/RSA模数/weapi/eapi）
- [x] 阶段 2：HTTP 内核（libcurl 封装 + weapi 请求管线 + code 解析 + zlib 自动解压）
- [x] 阶段 3：Cookie 桥（Netscape 读写 + filterJar 假 NMTID 拦截 + 属性过滤 + 登录态跨进程复用）
- [x] 阶段 4：扫码登录垂直切片（qr-key/-check + chainId 生成 + ApplyRequestStrategy + -462 重试）
- [x] 阶段 5：读接口家族（15 服务 + jval JSON 引擎 + 18 命令，回环 3/3 全绿）
- [x] 阶段 6：写接口家族（like/subscribe/track/playlist CRUD）+ 登录族（email/cellphone/refresh）+ eapi 通道（24641 设备池 + 反风控 header）+ trackIds 翻倍坑位
- [x] 阶段 7：壳对齐完成 — go-qrcode 全量移植（qr-render/qr-image）、Go `encoding/json` 错误消息逐字对齐、退出码对齐、双跑对照脚本 `tests/dualrun.py`（40 用例全绿）

## 命令一览（与 Go 版 33 命令一一对应）

| 类别 | 命令 |
|------|------|
| 搜索/歌曲 | `search` `search-pl` `check-music` `song-url` `song-detail` `lyric` |
| 歌单 | `playlist` `playlist-tracks` `user-playlist` `playlists` `toplist` `recommend-resource` `recommend-songs` `recommend-playlists` `record-recent` |
| 红心 | `liked` `liked-check` `like` |
| 登录 | `qr-key` `qr-check` `qr-render` `qr-image` `login-email` `login-cellphone` `login-refresh` `login-status` |
| 写操作 | `subscribe` `track-add` `track-del` `playlist-create` `playlist-rename` `playlist-delete` `account-name` |

## 差分对照（dualrun.py）

```sh
# C 单跑：40 用例 = 33 命令正路径 + 5 错误路径（退出码/stderr 文本）
python3 tests/dualrun.py --cli build/netease-cli

# 双跑：同参数、同 HOME、同 stub 服务器，与 Go 版逐字节比对
# （唯一归一化项：qr-key 输出里 chainId 的毫秒时间戳）
python3 tests/dualrun.py --cli build/netease-cli --go /path/to/go/netease-cli
```

stub 服务器同时校验三条加密通道的请求侧：weapi 外层 AES-CBC 解密、
linuxapi `eparams` 全解密按内层 url 分发、eapi 全解密 + md5 信封校验。

## 退出码 / stderr 语义（与 Go 版一致）

- 无参数 → stderr `usage: netease-cli <cmd> [args...]`，exit 1
- 未知命令 → stderr `unknown cmd: <cmd>`，exit 1
- 参数缺失 → 各命令 usage 文本，exit 1
- JSON 解析失败 → Go `encoding/json` 逐字错误文本，如
  `parse account failed: invalid character '<' looking for beginning of value`

## Windows

- 主目录取 `%USERPROFILE%`（等价 `os.UserHomeDir()`），cookie 路径
  `%USERPROFILE%\.cache\netune\cookies.txt`
- 无 POSIX 专用调用；CMake + libcurl + zlib 可用 MSVC/MinGW 构建

## E2E 验证（离线回环）

沙盒出口 IP 被 netease CDN 风控（正确密文也返回 content-length: 0，
Python+curl 对照确认是环境限制而非代码问题）。因此用本地 echo 服务器
（`NE_API_BASE=http://127.0.0.1:18321` 测试钩子，生产不设则直连
music.163.com）完成全链路验证：

- qr-key：unikey 提取 + 二维码 URL（chainId = `v1_<52hex大写>_web_login_<ms>`）
- qr-check：803 判定 + body 内 cookie 字符串解析入库
- 请求头：UA(pc)/Referer/Content-Type(form) 与 Go requests.NewRequest 一致
- cookie：qr-key 只带 sDeviceId；qr-check 注入 os=pc；假 NMTID 不上线不落盘
- 落盘：Netscape 格式与 Go FileJar 逐字段一致（tab 分隔 + 253402300799）
- 复用：第二次进程从 cookies.txt 重载 MUSIC_U 并随请求发送
