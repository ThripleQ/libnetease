/* Set-Cookie → 落盘 的确定性回归。
 *
 * 为什么需要它：真机上这条路径**撞不到**。实测（2026-10-07，K40）冷启动 27 个
 * 请求 + 4 次写操作（song/like ×3、album/sub、album/unsub）响应里一条 Set-Cookie
 * 都没有 —— 服务端不轮转，于是"落盘"这段代码在真机上从未被执行过。只看真机
 * 结果无法区分「代码对但没被触发」和「代码错且永远不触发」。
 *
 * 这里装一个假传输层（http.h 明确允许 ne_http_set_transport 注入），把带
 * Set-Cookie 的响应直接喂进 request 管线，把每条判据都跑一遍：
 *   1. 服务端轮转 → 必须落盘，且属性行不能变成 cookie；
 *   2. 内容没变的重复 Set-Cookie → 不写盘（指纹挡重复 IO）；
 *   3. 普通响应（无 Set-Cookie）→ 不写盘（播放场景高频 GET 的 IO 底线）；
 *   4. 同一份内容在 ne_jar_reload() 之后会再写一次 → 证明第 2 步是被指纹挡的，
 *      而不是别的原因凑巧没写；
 *   5. 值变了 → 再写；
 *   6. 导入路径 force=1：内容没变也必须写（登录导入不能省）；
 *   7. 没配 cookie 路径 → 不写、不崩；
 *   8. 落盘 → reload 的文件往返，值逐字一致（顺带锁住 LF 行尾这个格式契约）；
 *   9. 写盘失败不能记指纹 —— 否则「盘上是旧值」这个被修的 bug 会以另一种形式
 *      回来（路径恢复后内容没变就再也不补写）。
 * 全程单线程，故可直接用 ne_global_jar() 读回（该句柄对多线程宿主不安全，
 * 见 request.h 的 thread contract）。
 */
#include "netease/request.h"
#include "netease/http.h"
#include "netease/jmap.h"
#include "netease/cookiejar.h"   /* ne_jar_get（漏了这个声明会被当成隐式 int，
                                  * 指针截断后 strcmp 直接崩 —— 见 C4013） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JAR_PATH "test_cookie_persist.tmp"

static int failures = 0;
static const char *g_set_cookies = NULL;

static void *dup_mem(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

/* 假传输层：不联网，直接把预设的 Set-Cookie 行塞进响应对象 —— 这正是 http.h
 * 要求的传输层契约（"name=value\n" 行，属性的过滤由 cookiejar 负责）。 */
static ne_http_resp *fake_request(const char *url, const char *method,
                                  const char *body, const char *content_type,
                                  const char *cookie_header,
                                  const char *user_agent) {
    (void)url; (void)method; (void)body; (void)content_type;
    (void)cookie_header; (void)user_agent;
    ne_http_resp *r = (ne_http_resp *)malloc(sizeof *r);
    memset(r, 0, sizeof *r);
    r->status = 200;
    r->body = (char *)dup_mem("{\"code\":200}");
    r->body_len = strlen(r->body);
    r->set_cookies = g_set_cookies ? (char *)dup_mem(g_set_cookies) : NULL;
    return r;
}

static const ne_http_transport FAKE = { fake_request };

static ne_resp *fire(const char *set_cookies) {
    g_set_cookies = set_cookies;
    jmap *m = jmap_new();
    ne_resp *r = ne_call_weapi("https://music.163.com/weapi/test", m);
    jmap_free(m);
    return r;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static void expect(int cond, const char *what) {
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void expect_contains(const char *buf, const char *needle, int want,
                            const char *what) {
    int got = buf && strstr(buf, needle) != NULL;
    expect(got == want, what);
}

static void preset_jar_file(void) {
    FILE *f = fopen(JAR_PATH, "wb");
    if (!f) {
        printf("FAIL 无法写入 %s（测试环境问题）\n", JAR_PATH);
        exit(2);
    }
    fputs("# Netscape HTTP Cookie File\n", f);
    fputs("music.163.com\tFALSE\t/\tFALSE\t253402300799\tMUSIC_U\tOLD\n", f);
    fputs("music.163.com\tFALSE\t/\tFALSE\t253402300799\t__csrf\tOLDCSRF\n", f);
    fclose(f);
}

/* 服务端轮转时真实会发的形状：值 + 若干属性（属性绝不能进 jar） */
static const char *ROTATE =
    "MUSIC_U=NEWU; Path=/; Domain=.music.163.com; "
    "Expires=Wed, 09 Jun 2027 10:18:14 GMT; HttpOnly; Priority=High\n"
    "__csrf=NEWCSRF; Path=/";

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);    /* 崩了也要能看到走到哪一步 */
    remove(JAR_PATH);
    preset_jar_file();

    ne_http_set_transport(&FAKE);
    ne_set_cookie_file(JAR_PATH);
    ne_jar_reload();                     /* 预置文件 → 内存 jar（模拟冷启动） */

    /* ── 1. 服务端轮转 → 落盘 ─────────────────────────────── */
    ne_resp *r = fire(ROTATE);
    expect(r != NULL && r->code == 200, "1 假响应被正常解析（code=200）");
    ne_resp_free(r);

    char *t = slurp(JAR_PATH);
    expect(t != NULL, "1 轮转后 cookie 文件仍存在");
    expect_contains(t, "\tMUSIC_U\tNEWU\n", 1, "1 MUSIC_U 落盘为服务端新值");
    expect_contains(t, "\t__csrf\tNEWCSRF\n", 1, "1 __csrf 落盘为新值");
    expect_contains(t, "\tMUSIC_U\tOLD\n", 0, "1 旧值是替换而非并列（jar 不重复）");
    expect_contains(t, "\tPath\t", 0, "1 属性 Path 没有被当成 cookie");
    expect_contains(t, "\tDomain\t", 0, "1 属性 Domain 没有被当成 cookie");
    expect_contains(t, "\tHttpOnly\t", 0, "1 属性 HttpOnly 没有被当成 cookie");
    expect_contains(t, "\tExpires\t", 0, "1 属性 Expires 没有被当成 cookie");
    expect_contains(t, "\tPriority\t", 0, "1 属性 Priority 没有被当成 cookie");
    /* 文件格式是契约：Go FileJar 是 LF。文本模式写盘在 Windows 上会落 CRLF，
     * 于是同一份 jar 在不同平台上字节不同 —— 这条断言只在 Windows 上有牙。 */
    expect_contains(t, "\r", 0, "1 落盘是 LF 行尾（文本模式会写成 CRLF）");
    free(t);

    /* ── 2. 内容没变的重复 Set-Cookie → 不写盘 ────────────── */
    remove(JAR_PATH);
    r = fire(ROTATE);
    ne_resp_free(r);
    expect(!file_exists(JAR_PATH), "2 内容未变的重复 Set-Cookie 不重复写盘");

    /* ── 3. 普通响应（无 Set-Cookie）→ 不写盘 ─────────────── */
    r = fire(NULL);
    expect(r != NULL && r->code == 200, "3 无 Set-Cookie 的响应照常解析");
    ne_resp_free(r);
    expect(!file_exists(JAR_PATH), "3 无 Set-Cookie 时零写盘（高频 GET 的 IO 底线）");

    /* ── 4. reload 重置指纹 → 同样的内容会再写一次 ─────────── */
    ne_jar_reload();
    r = fire(ROTATE);
    ne_resp_free(r);
    t = slurp(JAR_PATH);
    expect(t != NULL, "4 ne_jar_reload() 后同样的内容会重新写盘（第 2 步确由指纹所挡）");
    expect_contains(t, "\tMUSIC_U\tNEWU\n", 1, "4 reload + 重放后内容正确");
    free(t);

    /* ── 5. 值变了 → 再写一次 ─────────────────────────────── */
    r = fire("MUSIC_U=ROTATED2; Path=/");
    ne_resp_free(r);
    t = slurp(JAR_PATH);
    expect_contains(t, "\tMUSIC_U\tROTATED2\n", 1, "5 再次轮转写入了新值");
    expect_contains(t, "\tMUSIC_U\tNEWU\n", 0, "5 上一次的值已被覆盖");
    free(t);

    /* ── 6. 导入路径 force=1：内容没变也必须写 ────────────── */
    remove(JAR_PATH);
    ne_jar_import_cookies("MUSIC_U=ROTATED2");   /* 与内存 jar 内容一致 */
    t = slurp(JAR_PATH);
    expect(t != NULL, "6 导入路径 force=1：内容没变也落盘（登录导入不能省）");
    expect_contains(t, "\tMUSIC_U\tROTATED2\n", 1, "6 导入的内容正确");
    free(t);

    /* ── 7. 没配 cookie 路径 → 不写、不崩 ─────────────────── */
    ne_set_cookie_file("");
    remove(JAR_PATH);
    r = fire("MUSIC_U=SHOULD_NOT_WRITE");
    ne_resp_free(r);
    expect(!file_exists(JAR_PATH), "7 未配置 cookie 路径时不写盘");

    /* ── 8. 文件往返：写盘 → reload → 读回的值逐字一致 ─────── */
    ne_set_cookie_file(JAR_PATH);
    r = fire("MUSIC_U=ROUNDTRIP; __csrf=RT");
    ne_resp_free(r);
    ne_jar_reload();                     /* 这之后的值只可能来自磁盘 */
    const char *v = ne_jar_get(ne_global_jar(), "MUSIC_U");
    expect(v && strcmp(v, "ROUNDTRIP") == 0,
           "8 落盘 → reload 往返后值逐字一致（无行尾残留）");

    /* ── 9. 写盘失败不记指纹 → 路径恢复后同样的内容会补写 ──── */
    ne_set_cookie_file("no_such_dir_here/jar.tmp");   /* fopen 必失败 */
    r = fire("MUSIC_U=RETRY1");
    ne_resp_free(r);
    expect(!file_exists("no_such_dir_here/jar.tmp"), "9 路径不可写时确实没写出去");
    ne_set_cookie_file(JAR_PATH);
    remove(JAR_PATH);
    r = fire("MUSIC_U=RETRY1");          /* 内容与内存 jar 相同：靠指纹挡 */
    ne_resp_free(r);
    t = slurp(JAR_PATH);
    expect(t != NULL, "9 写失败后不记指纹：路径恢复后同样的内容会补写一次");
    expect_contains(t, "\tMUSIC_U\tRETRY1\n", 1, "9 补写的内容正确");
    free(t);

    remove(JAR_PATH);
    printf(failures ? "cookie_persist: %d FAILURES\n" : "cookie_persist: all ok\n",
           failures);
    return failures ? 1 : 0;
}
