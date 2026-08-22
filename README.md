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

依赖：CMake ≥ 3.16、libcurl。

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
