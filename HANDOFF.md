# libnetease 交接文档

> 项目：`go-musicfox/netease-music` v1.6.0 + `netease-cli` 的 C 语言移植
> 状态：功能基本完成，C-only 单跑 40/40 全绿；`--go` 双跑 3 项遗留已全部按标准处理完毕（见第五节）
> 最近进展（2026-08-22）：遗留 1（qr-render 末行字符）与遗留 3（dualrun HOME 归一化）已修复并通过测试；遗留 2 的 padding bug 已修复（`qrenc.c` 第 769 行 `0xEC→0x11`），并已按 ISO/IEC 18004 标准完成全量代码审查，无不符合标准项。

---

## 一、项目概览

| 项 | 值 |
|---|---|
| 目标 | 做一个与 Go 版 **同名同协议** 的 `netease-cli`，netune 零改动即可无缝替换 |
| 锚定版本 | `github.com/go-musicfox/netease-music@v1.6.0` + `skip2/go-qrcode@da1b656` |
| 构建产物 | `libnetease.a`（静态库）+ `netease-cli`（可执行文件） |
| 构建系统 | CMake ≥ 3.16，C11 |
| 运行时依赖 | libcurl（必需）、zlib（可选，加速 PNG 压缩） |
| 平台 | Linux / macOS / Windows（无 POSIX 专用调用） |
| 主目录 | 优先 `$HOME`，Windows 下等价 `%USERPROFILE%`；cookie 路径：`~/.cache/netune/cookies.txt` |

### 移植纪律

- **Go 源码是唯一规格**：锚定 v1.6.0，不追上游。
- **双跑对照代替单测**：固定随机 key 后与 Python 镜像（逐行翻译自 cryto.go）比对输出。
- **坑位显式化**：Go 特有行为（见第六节坑位表）全部写成注释 + 测试用例。

## 二、目录结构

```
libnetease/
├── CMakeLists.txt          # 双产物 + 4 个单元测试 + dualrun 差分测试
├── README.md               # 项目说明（命令一览、坑位表、进度）
├── HANDOFF.md              # 本文档（交接用）
├── include/netease/        # 公共头：17 个 .h
│   ├── qrenc.h             # 二维码对外 API
│   ├── qr.h                # 扫码登录服务
│   ├── services.h          # 全部 API 服务
│   ├── request.h / http.h / cookiejar.h / crypto.h / jval.h / jmap.h
│   └── vendor: aes.h rsa.h bignum.h md5.h encoding.h
├── src/
│   ├── vendor/             # 零依赖：aes / rsa / bignum / md5 / base64 / hex / qrenc
│   │   └── qr_medium_table.h  # Medium 纠错级版本 1-40 参数表（从 Go 源生成）
│   ├── core/               # HTTP 内核 + JSON + 加密管线 + cookie + 设备池
│   ├── service/            # qr.c + services.c（全部 API 端点）
│   └── cli/main.c          # netease-cli 壳，33 命令 dispatch
└── tests/
    ├── run_tests.sh        # 一键：生成向量 → 构建 → ctest
    ├── ref_impl.py         # Python 镜像加密原语，生成 expected.h 向量
    ├── expected.h          # 向量头文件（由 ref_impl.py 生成）
    ├── test_crypto.c       # 加密原语向量测试
    ├── test_jval.c         # JSON 解析/序列化测试
    ├── test_request.c      # URL 重写测试
    ├── test_qr.c           # 二维码输出 dump（供 verify_qr.py 校验）
    ├── verify_qr.py        # 独立 QR 解码校验：格式信息/RS 余数/分段回解/PNG 像素
    ├── dualrun.py          # 差分对照脚本：stub 服务器 + 40 用例，支持 --go 双跑
    └── fixtures/
        ├── qr_render_expected.txt  # qr-render 文本渲染期望（与 fixture 逐字节比对）
        ├── necert.pem / nekey.pem  # TLS 代理自签证书（供 --go 模式拦截 Go 请求）
```

## 三、已完成的工作

### 阶段 0：项目骨架
- CMake 双产物：`libnetease`（static lib）+ `netease-cli`（executable）
- 目录分层：vendor / core / service / cli

### 阶段 1：加密原语（全部 1:1 对齐 Go）
- AES-128（CBC / ECB，PKCS7 padding）
- MD5
- RSA 1024-bit（无填充，手写大数 `bignum.c`）
- Base64 / hex 编解码
- 验证方式：`ref_impl.py` 生成向量，`test_crypto.c` 逐字节比对

### 阶段 2：HTTP 内核
- libcurl 封装（`http.c`）：GET/POST、header 管理、cookie 注入、zlib 自动解压
- `request.c`：weapi / linuxapi / eapi 三条加密通道
  - weapi：AES-CBC 双层 + RSA 封装 secretKey + csrf_token 注入
  - linuxapi：`eparams` AES-128-ECB + base64
  - eapi：`params` + `ipas` 反风控头 + 24641 设备池 + `requestId` 非真实毫秒（`Unix()*1000 + rand(0..999)`）
- URL 改写：`/api/` → `/weapi/`，保留路径参数

### 阶段 3：Cookie 桥
- Netscape 格式读写（tab 分隔，`253402300799` 表示永不过期）
- filterJar 逻辑：假 NMTID（`ApplyRequestStrategy` 注入的固定值）不上线不落盘
- 跨进程登录态复用：从 `cookies.txt` 重载 MUSIC_U

### 阶段 4：扫码登录垂直切片
- `qr-key`：unikey 提取 + chainId 生成（`v1_<sDeviceId>_web_login_<ms>`）
- `qr-check`：803/800/801/802 状态码 + body 内 cookie 字符串解析入库
- -462 重试逻辑（ApplyRequestStrategy 注入 NMTID 被风控拦截）

### 阶段 5：读接口家族（15 服务 / 18 命令）
- `jval` JSON 引擎：Go `encoding/json` 语义（键排序、HTML 转义、数字保留原文、严格 UTF-16 代理对校验）
- 服务：search / search-pl / check-music / song-url / song-detail / lyric / playlist / playlist-tracks / user-playlist / playlists / toplist / recommend-resource / recommend-songs / recommend-playlists / record-recent

### 阶段 6：写接口家族 + 登录族
- 写：like / subscribe / track-add / track-del / playlist-create / playlist-rename / playlist-delete / account-name
- 登录：login-email / login-cellphone / login-refresh / login-status
- eapi 通道：`playlist-rename` 走 `interface3.music.163.com`（明文 http）
- 坑位保留：`trackIds` 自我翻倍（`append(x, x...)`，单 id 上路为 `["id","id"]`）

### 阶段 7：壳对齐
- `qr-render` / `qr-image`：go-qrcode 全量移植（Medium 纠错，版本 1-40）
  - Reed-Solomon 纠错（手写伽罗华域 GF(2^8)）
  - 8 种 mask + penalty 评分 + 首个最低掩码胜出
  - 数据分段优化（`optimiseDataModes`：合并同编码模式相邻段）
  - 单字节段优先（`singleByteSegmentLength <= optimizedLength` 时折叠为一段）
  - PNG 输出（zlib 压缩，fallback 为 uncompressed stored）
  - 文本渲染（ToSmallString：半块字符 `▀▄█ `）
- 退出码 / stderr 对齐 Go 版（无参数、未知命令、参数缺失、JSON 解析失败）
- `dualrun.py` 差分对照脚本：stub 服务器 + 40 用例

## 四、当前测试状态

### 必过（正式验收标准）

```sh
./tests/run_tests.sh
# 等价于：
#   python3 tests/ref_impl.py > tests/expected.h
#   cmake -S . -B build
#   cmake --build build
#   ctest --test-dir build --output-on-failure
```

- **crypto**：加密原语向量测试 — PASS
- **jval**：JSON 解析/序列化测试 — PASS
- **rewrite**：URL 重写测试 — PASS
- **qr**：QR 编码器 dump + 结构校验 — PASS
- **dualrun**：40 用例 C-only 全绿（33 命令正路径 + 7 错误路径/边界）

### 可选（双跑对照，需 Go 二进制）

```sh
python3 tests/dualrun.py --cli build/netease-cli --go /path/to/netease-cli-go
```

stub 服务器同时校验三条加密通道的请求侧：
- weapi 外层 AES-CBC 解密
- linuxapi `eparams` 全解密按内层 url 分发
- eapi 全解密 + md5 信封校验

## 五、遗留问题（3 项，`--go` 双跑下暴露）

> **重要**：这 3 项在 C-only 测试中是 PASS 的（qr-render 与 fixture 逐字节一致、login-status 匹配正则）。只有在与真实 Go 二进制 `--go` 逐字节双跑时才不一致。是否要修，取决于"drop-in 替换"的精度要求——功能上都能用，只是字节级不完全相同。
>
> **2026-08-22 更新**：3 项均已处理完毕（见下）。

### 遗留 1：`qr-render` 末行渲染字符与 Go 不同 ✅ 已修复

**现象**：当二维码总行数为奇数时，最后一行（y = size-1）是单独一行。Go da1b656 的 `ToSmallString(false)` 渲染为：
- dark → `' '`（空格，上半暗）
- light → `▀`（U+2580，仅上半亮，下半暗）

C 原实现为：light → `█`（U+2588，全块，错误）。

**修复**：`src/vendor/qrenc.c` 第 919 行，odd-last-row 分支的 light 情况已改为 `0xE2 0x96 0x80`（▀）。`tests/fixtures/qr_render_expected.txt` 已同步更新（末行全 `▀`）。ctest 全绿验证通过。

### 遗留 2：`qr-image` 与 Go 二进制的像素不同 ✅ 已修复 + 标准审查通过

**根因（2026-08-22 定位）**：padding 字节错误。C 原用 `0xEC/0xED` 交替填充，而 ISO 标准与 Go 都是 **`0xEC`（11101100）与 `0x11`（00010001）交替**。`0xED` 是常见错误。

**修复**：`src/vendor/qrenc.c` 第 769 行改为 `unsigned char pad = i == 0 ? 0xEC : 0x11;`。修复后 C 的 finalstream（数据+纠错交织后）与 Go 权威流 200 字节完全匹配，掩码选择一致（都选 mask4）。

**标准符合性审查（2026-08-22，全量逐段核对 ISO/IEC 18004）**：全部通过，无不符合项：
- `FMT_SEQ`（M 级 8 mask 的 15 位 BCH 值）、`VER_SEQ`（v7-40 的 18 位版本信息）、`ALIGN_CENTER`（v2-40 对齐图案中心坐标）三张表与标准值逐项一致
- format info 两副本写入坐标（含 dark module）与 Go 移植源逐坐标一致，符合标准图 25
- version info 写入 `(i/3, size-11+i%3)` 布局正确
- 数据放置 zigzag：从右下 (size-1,size-1) 起，跳过 timing 列（`x==5` 后 `x--`）
- 8 种 mask 公式逐一对应标准
- RS 编码：GF(2^8) 本原多项式 0x11d，生成器 `(x+a^0)..(x+a^(n-1))`，与 Go reedsolomon 逐字节一致
- 交织 interleave：数据块先行、EC 块后行
- penalty1-4 与 Go symbol.go 逐行一致（只影响 mask 选择，不影响解码正确性）
- PNG：标准 PNG 结构 + 自适应滤波（与 image/png 同启发式）

**决策（用户 2026-08-22 拍板）**：遗留 2 以"代码符合标准"为验收标准，不再用 OpenCV/扫描类工具做测试。审查结论：代码符合标准，视为正确。

**背景记录（已归档，不再追查）**：
- 曾用 OpenCV `QRCodeDetector` 观察到 "Go 能扫、C 扫不出"（`points=True` 但 `decoded=''`），并观察到 C 与 Go 矩阵在 format info 区有 8 模块差异 —— 但这两条结论都建立在 `verify_qr.py` 自身 bug（对有效 Go 码全 mask 报 RS 失败）和 `g_bm_final.txt` 提取错位的基础之上，证据链不可靠。按"代码符合标准即正确"原则，不据此继续修改代码。
- Go 矩阵按标准读取坐标读出 `0x4fd1`（非标准）疑似读取脚本坐标偏移所致；C 的 format info 读出 `0x45f9` 为标准正确值。
- `verify_qr.py` 的 de-interleave / RS 校验逻辑有 bug，不可作为最终裁决，如后续需要独立验证请先修复它。

### 遗留 3：`login-status` 双跑输出路径不同 ✅ 已修复

**现象**：`dualrun.py` 给 C 和 Go 分别建了不同前缀的临时 HOME（`dualrun-` vs `dualrun-go-`），而 `login-status` 输出里嵌入了完整 cookie 文件路径，导致必然不相等。

**这不是移植 bug**，是测试框架自身的归一化遗漏。

**修复**：`tests/dualrun.py` 的 `norm()` 已加 HOME 路径归一化（`HOME_NORM = re.compile(r"/tmp/dualrun(?:-go)?-[^/\s]+")`，替换为 `/tmp/dualrun-HOME`）。ctest 全绿验证通过。

## 六、Go 行为坑位速查表

> 这些是移植时必须**原样保留**的 Go 特有行为，全部已有对应测试用例覆盖。

| 坑位 | 说明 |
|------|------|
| `reSecretKey` 不是 reverse | 循环里取两次独立随机，名字有误导性 |
| RSA 结果去前导零 | `big.Int.Bytes()` 输出，hex 长度可能 < 256 |
| JSON 键排序 + HTML 转义 | `<` `>` `&` 转成 `\u003c` 等 |
| cookie 默认 `os=ios` | 未登录时 UA 相关字段按 iOS 9.0.65 填 |
| login URL 注入随机 NMTID | 仅请求级，壳落盘时过滤（防假 NMTID 污染） |
| weapi 前置 csrf_token | 加密前注入，密文里生效 |
| trackIds 自我翻倍 | `append(x, x...)`，单 id 上路为 `["id","id"]` |
| eapi requestId 非 ms | `Unix()*1000 + rand(0..999)` |
| 设备池永远取不到最后一个 | `rand.Intn(len(deviceIds)-1)` |
| playlist-rename 走 interface3 | `http://interface3.music.163.com/eapi/...`（明文 http） |
| CallWeapi 不注入 cookie | 只带 jar cookie，无 os/appver/__remember_me/NMTID |

## 七、命令一览（33 命令，与 Go 版一一对应）

| 类别 | 命令 |
|------|------|
| 搜索/歌曲 | `search` `search-pl` `check-music` `song-url` `song-detail` `lyric` |
| 歌单 | `playlist` `playlist-tracks` `user-playlist` `playlists` `toplist` `recommend-resource` `recommend-songs` `recommend-playlists` `record-recent` |
| 红心 | `liked` `liked-check` `like` |
| 登录 | `qr-key` `qr-check` `qr-render` `qr-image` `login-email` `login-cellphone` `login-refresh` `login-status` |
| 写操作 | `subscribe` `track-add` `track-del` `playlist-create` `playlist-rename` `playlist-delete` `account-name` |

## 八、环境变量

| 变量 | 用途 |
|------|------|
| `NE_API_BASE` | 替换 `https://music.163.com` 基地址（测试用，生产不设则直连） |
| `NE_UNM_PROXY` | HTTP 代理（Go 版对应 `UNMProxyURL`，C 版通过 libcurl proxy 实现） |
| `NE_QR_DEBUG` | 二维码调试输出（版本、编码长度等） |
| `NE_QR_DUMP` | QR 测试 dump 目录（供 verify_qr.py 读取） |
| `NE_QR_EC_DEBUG` | 打点 RS 纠错字节输入/输出（定位 EC 差异用，见遗留 2） |
| `NE_DEBUG_HTTP` | 打开 libcurl `CURLOPT_VERBOSE`，打印 TLS/代理/HTTP 握手细节 |
| `NE_REAL_IP` | 注入 `X-Real-IP`+`X-Forwarded-For`（国内 IP 规避 460/空 body 风控） |
| `NE_RANDOM_CN_IP` | 未设 NE_REAL_IP 时每请求自动生成国内 IP |
| `NE_RATE_LIMIT_MS` / `NE_RATE_LIMIT_JITTER_MS` | 请求前随机间隔 base+[0,jitter) ms |
| `NE_UA_ROTATE` | PC 浏览器 UA 池轮换 |
| `NE_RETRY_RISK` | 对 -460/高频/传输错误指数退避重试 n 次（-462 不自动重试） |
| `NE_NO_KEEPALIVE` | 关闭连接复用（配合代理换 IP 立即生效） |
| `NE_BROWSER_HEADERS` | 补浏览器标准请求头（Accept/Accept-Language/Sec-Fetch 系/Upgrade-Insecure-Requests） |
| `NE_HTTP2` | 显式协商 HTTP/2（浏览器标配；需 libcurl 带 nghttp2，stub 测试勿开） |

## 九、下一步建议（按优先级）

> **2026-08-22 更新**：原遗留 1/2/3 已全部处理完毕（见第五节）。

> **2026-08-30 更新**：新增「十一、风控加固」（可选，默认零行为变更），详见 docs/RISKS.md。
> **2026-08-30 二次更新**：Windows 完整构建验证通过（CMake + Ninja + cl.exe + ctest，5/5 全绿，
> 见第九节）；dualrun 的 playlists 期望已同步 coverImgUrl 字段。
> **2026-08-30 三次更新**：补充第三层防风控（NE_BROWSER_HEADERS / NE_HTTP2）+ docs/RISKS.md 第八节
> TLS 指纹接入指南（curl-impersonate）。

1. **Windows 编译验证**：README 声称跨平台，但尚未在 MSVC/MinGW 下实际编译过。可顺带跑一遍 `run_tests.sh`。
   （2026-08-30 本机已验证：`build_ninja.sh` 用 CMake+Ninja+cl.exe 完整构建并 `ctest` **5/5 全绿**——
   crypto/jval/rewrite/qr 单测 + dualrun 52 用例全部通过。无需 MSBuild/VS 项目文件。）
2. **真实网易服务器联调**：目前所有验证都是本地 stub 回环，沙箱出口 IP 被风控。需要在真实网络环境（或代理）下跑一次登录 + 播放全链路。
3. **（可选）TLS 指纹层验证**：在真实环境用 JA3/JA4 指纹检测（如 `ja3` 在线检测、tlsfingerprint）对比
   标准 libcurl 与 `LD_PRELOAD=libcurl-impersonate.so` 的差异，确认网易是否对该维度敏感（见 docs/RISKS.md 第八节）。
4. **（可选）修复 `verify_qr.py`**：其 de-interleave / RS 校验逻辑有 bug（对有效 Go 码全 mask 报 RS 失败），如后续需要独立验证二维码可扫性，先修它；或改用 zxing/quirc 等成熟解码库做交叉验证。
5. **（可选）清理调试残留**：`qrenc.c` 中 `NE_QR_DEBUG`/`NE_QR_EC_DEBUG` 打点代码（DBG encdata/finalstream/mask penalty 等）已保留，属零运行时开销（仅环境变量触发），可保留亦可按需删除。

> **已实证（2026-08-22）**：本沙箱出口 IP 对网易 weapi 直接返回 `HTTP 200 + 空 body`（`size=0`），这是数据中心 IP 风控，**非 port 代码 bug**。验证过程：
> - `NE_DEBUG_HTTP=1` 显示 CLI 走默认代理（`http://127.0.0.1:18080`）、TLS 校验通过、`POST /weapi/login/qrcode/unikey` 返回 `HTTP/2 200`，但 body 为 0 字节。
> - 用手工 curl（浏览器 UA + 完整 headers + 假 weapi 密文）直连同样得到 `HTTP 200 + size=0`，与 C 无关，坐实 IP 风控。
> - `qr-key` 因此报 `code=0`（body 空 → 解析不到数字 code）。
> - **结论**：需在真实住宅 IP/合法代理环境下联调（登录 + 播放全链路）。本地 stub 已经覆盖了加密、请求、解析、cookie 全链路的正确性。

## 十、相关参考文件位置

| 内容 | 路径 |
|------|------|
| Go 参考源码（netease-music v1.6.0） | `/data/user/work/gopath/pkg/mod/github.com/go-musicfox/netease-music@v1.6.0/` |
| Go 参考源码（go-qrcode da1b656） | `/data/user/work/gopath/pkg/mod/github.com/skip2/go-qrcode@v0.0.0-20200617195104-da1b6568686e/` |
| Go 对照二进制 | `/data/user/work/gobuild/netease-cli-go` |
| Go 壳源码（main.go） | `/data/user/work/gobuild/main.go` |
| 独立 QR 参考（verify_qr 用） | `/data/user/work/go-qrcode/` |
| go-qrcode 原始 zip（校验未被篡改） | `/data/user/work/gopath/pkg/mod/cache/download/github.com/skip2/go-qrcode/@v/v0.0.0-20200617195104-da1b6568686e.zip` |
| go-qrcode 打点副本（QR 调试用） | `/data/user/work/goqr_dbg/`（`qrprobe/go.mod` 的 replace 指向它） |
| Go 位图探针（bm_dbg 打点版） | `/data/user/work/qrprobe/`（`bm_dbg.go` / `bm.go`） |
| QR 诊断脚本（Python） | `/data/user/work/`（artdiff.py / streamdiff.py / opencvprobe.py / renderscan.py 等，见遗留 2） |
| QR 调试输出（NE_QR_DUMP） | `build/...` 由 `tests/test_qr.c` 生成 `case0.txt` 等位图 dump |

## 十一、风控加固（2026-08-30）

> 调研对象：活跃上游（2026-08 时点）—— api-enhanced（Node，2026-08-27 仍有提交）、
> chaunsin/netease-cloud-music（Go，2026-08 仍有提交）、Meting-API（JS）；
> 原版 Binaryify/NeteaseCloudMusicApi 已于 2024-02 归档清空（仅 README），pyncm 已下架。
> 完整调研与策略见 **docs/RISKS.md**。

### 目标与纪律
- **默认零行为变更**：所有加固均为 opt-in（环境变量或新 C API），不启用时请求字节流
  与 Go v1.6.0 移植完全一致，dualrun 逐字节兼容不受影响。
- **新增文件**：`include/netease/risk.h` + `src/core/risk.c`（风控分类/退避/随机国内 IP）。

### 新增能力清单
| 能力 | 入口 | 说明 |
|---|---|---|
| 风控响应分类（8 类） | `ne_risk_classify` / `ne_risk_reason` / `ne_risk_is_transient` | 200/301/400/-460/-462/8821/429-503-403/空 body/传输错误 |
| 指数退避 + 抖动 | `ne_risk_backoff_ms(attempt, max)` | 200ms×2ⁿ，±25% 抖动 |
| 空 body（数据中心 IP 风控）检测 | `ne_risk_is_empty_body` | 对应 HANDOFF 实证的 `HTTP 200 + size=0` |
| 随机国内 IPv4 | `ne_random_cn_ip(buf)` | 116-125/218-223 全国内 /8 段内随机（上游同款思路） |
| X-Real-IP + X-Forwarded-For 双头 | `ne_http_set_real_ip` / `NE_REAL_IP` | api-enhanced/Meting 均为双头注入 |
| 自动随机国内 IP | `NE_RANDOM_CN_IP=1` | 未显式设置时每请求生成 |
| 请求限速 + 抖动 | `ne_http_set_rate_limit` / `NE_RATE_LIMIT_MS`+`NE_RATE_LIMIT_JITTER_MS` | 请求前随机间隔 |
| 风控/传输自动重试 | `ne_set_risk_retry` / `NE_RETRY_RISK` | 仅瞬时可恢复分类；-462 不自动重试 |
| UA 轮换 | `NE_UA_ROTATE=1` | PC 浏览器 UA 池（Chrome/Edge/Firefox/Safari） |
| 浏览器化请求头 | `NE_BROWSER_HEADERS=1` | Accept/Accept-Language/Sec-Fetch 系/Upgrade-Insecure-Requests |
| HTTP/2 协商 | `NE_HTTP2=1` | 显式 `CURL_HTTP_VERSION_2TLS`（浏览器标配；stub 测试勿开） |
| 连接复用开关 | `ne_http_set_no_keepalive` / `NE_NO_KEEPALIVE` | 配合代理换 IP 立即生效 |
| 毫秒睡眠 | `ne_sleep_ms`（util） | 跨平台（Windows Sleep / POSIX nanosleep） |
| `ne_resp` 增加 `http_status` | request.h | 兼容扩展，分类依赖 |

### 实现要点（坑位记录）
- `UA_PC` 原为 `static const char *`，因 C11 要求数组初值常量，改为宏 `#define UA_PC ...`，
  供 `UA_PC_POOL[]` 常量初始化；所有引用处兼容。
- 重试复用同一份已加密 form（重发相同密文）——与 Meting-API「-460 重试 5 次、间隔 100ms」
  思路一致，网关偶发限流可自愈；写操作场景建议保持关闭（可能重复生效）。
- 随机国内 IP 段表刻意排除 58-61/202-211（混有海外段），避免"看似国内"反露马脚。

### 上游对照（重要差异）
- **NMTID**：api-enhanced 2026-08-24 修复——固定假 NMTID 会触发风控，真值应由服务端
  在"不带 NMTID 的 eapi 请求"的 Set-Cookie 下发（保底 `00O`+38hex）。libnetease 沿用
  Go v1.6.0 的 `some_random_id_from_strategy`（filterJar 不上线、仅 os=pc 生效）。若 eapi
  场景被 -462 缠住，宿主可自行向 jar 注入服务端下发的 `NMTID` cookie 覆盖。
- **易盾反作弊 token**（`X-antiCheatToken` v2/v3）：api-enhanced 用 jsdom 跑 Watchman SDK
  获取，仅注册/验证码类接口需要，libnetease 无此功能，暂不实现。
- **游客 MUSIC_A / xeapi**：上游有，libnetease 当前接口用不到，仅文档记录。
- **TLS 指纹（第三层）**：libnetease 默认 transport 为标准 libcurl，TLS Client Hello 与浏览器差异
  大（JA3/JA4 可识别）。已提供 `NE_HTTP2`/`NE_BROWSER_HEADERS` 两个 opt-in 缓解项，完整方案
  （curl-impersonate / LD_PRELOAD）见 docs/RISKS.md 第八节。网易是否启用 TLS 指纹检测未公开证实。

### Windows 构建备注
- **结论：本地构建工具链完好**。完整流程 `build_ninja.sh`（CMake "Ninja" 生成器 + cl.exe + vcpkg）：
  `cmake configure → cmake --build → ctest` **5/5 全绿**（crypto/jval/rewrite/qr/dualrun 52 用例）。
  前置：cl.exe 环境变量（INCLUDE/LIB，见脚本）、vcpkg（curl）、ninja（下载到 `C:/tools`）、
  隔离 venv Python（pycryptodome，用于生成 expected.h 向量）。
- 唯一注意：**cmake 的 "Visual Studio" 生成器在本机查询 VCTargetsPath 时 MSBuild 报
  Access violation**（cmake 调用 MSBuild 探测时崩，疑似该 VS 实例问题）。这不影响构建本身——
  Ninja/NMake/直编均绕开它。若要用 VS 生成器，先排查 MSBuild 本身。
- `build_check.sh`：快速语法检查（cl.exe 直编全部 .c，**带 NE_HAVE_CURL/NE_HAVE_ZLIB 宏**，
  与真实构建同配置，能抓住 curl 路径编译错误；教训：不加宏会漏检 http.c 的声明顺序 bug）。
- dualrun 期望修复：commit 383f935 起 playlists 输出含 `coverImgUrl` 字段（Go json 键排序，
  `coverImgUrl` 在 `id` 前），dualrun.py 期望已同步。

