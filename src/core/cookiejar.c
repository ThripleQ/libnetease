/* Netscape cookies.txt jar + filterJar (see main.go) */
#include "netease/cookiejar.h"
#include "netease/util.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define NETEASE_HOST "music.163.com"
#define FAR_FUTURE   "253402300799"

typedef struct { char *name; char *value; } ne_cookie;

struct ne_jar {
    ne_cookie *items;
    size_t len, cap;
};

static int name_is_attr(const char *n) {
    /* RFC 6265 的属性名，外加早期草案里真在线上出现过的几个（version/comment/
     * discard/port/priority/partitioned）。Set-Cookie 是**线上格式**：值后面跟着
     * 一串属性，漏掉哪个名字，它就会被当成一个 cookie 存进 jar，之后又原样出现在
     * Cookie 请求头里（凭空多出一个假 cookie，对风控不是好事）。导入路径喂进来的
     * 是 document.cookie 形状（只有 k=v），碰不到这些。 */
    static const char *attrs[] = {
        "path", "domain", "expires", "max-age", "secure", "httponly", "samesite",
        "version", "comment", "commenturl", "discard", "port", "priority",
        "partitioned", NULL
    };
    for (int i = 0; attrs[i]; i++)
        if (strcasecmp(n, attrs[i]) == 0) return 1;
    return 0;
}

ne_jar *ne_jar_new(void) {
    ne_jar *j = ne_xmalloc(sizeof(ne_jar));
    j->items = NULL; j->len = 0; j->cap = 0;
    return j;
}

void ne_jar_free(ne_jar *j) {
    if (!j) return;
    for (size_t i = 0; i < j->len; i++) { free(j->items[i].name); free(j->items[i].value); }
    free(j->items);
    free(j);
}

static size_t jar_find(const ne_jar *j, const char *name) {
    for (size_t i = 0; i < j->len; i++)
        if (strcmp(j->items[i].name, name) == 0) return i;
    return (size_t)-1;
}

void ne_jar_set(ne_jar *j, const char *name, const char *value) {
    if (name_is_attr(name)) return;
    /* filterJar: the anti-fraud strategy's fixed fake NMTID never persists */
    if (strcasecmp(name, "NMTID") == 0 &&
        strcmp(value, "some_random_id_from_strategy") == 0) return;

    size_t idx = jar_find(j, name);
    if (idx != (size_t)-1) {
        free(j->items[idx].value);
        j->items[idx].value = ne_xstrdup(value);
        return;
    }
    if (j->len == j->cap) {
        j->cap = j->cap ? j->cap * 2 : 16;
        j->items = ne_xrealloc(j->items, j->cap * sizeof(ne_cookie));
    }
    j->items[j->len].name = ne_xstrdup(name);
    j->items[j->len].value = ne_xstrdup(value);
    j->len++;
}

const char *ne_jar_get(const ne_jar *j, const char *name) {
    size_t idx = jar_find(j, name);
    return idx == (size_t)-1 ? NULL : j->items[idx].value;
}

int ne_jar_load_file(ne_jar *j, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;
        /* host \t include \t path \t secure \t expiry \t name \t value */
        char *fields[7] = {0};
        int nf = 0;
        char *tok = strtok(p, "\t\n");
        while (tok && nf < 7) { fields[nf++] = tok; tok = strtok(NULL, "\t\n"); }
        if (nf >= 7) ne_jar_set(j, fields[5], fields[6]);
    }
    fclose(f);
    return 0;
}

int ne_jar_save_file(const ne_jar *j, const char *path) {
    /* 二进制模式不是可选项：文本模式下 MSVC 会把 \n 落成 \r\n，文件就不再是
     * cookiejar.h 承诺的 Go FileJar 字节格式（同一份 jar 在不同平台上不一致，
     * 跨平台读回时值里还会带上行尾残留）。 */
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "# Netscape HTTP Cookie File\n");
    for (size_t i = 0; i < j->len; i++)
        fprintf(f, "%s\tFALSE\t/\tFALSE\t%s\t%s\t%s\n",
                NETEASE_HOST, FAR_FUTURE, j->items[i].name, j->items[i].value);
    fclose(f);
    return 0;
}

char *ne_jar_cookie_header(const ne_jar *j) {
    size_t cap = 64, len = 0;
    char *buf = ne_xmalloc(cap);
    buf[0] = '\0';
#define APP(s) do { size_t _l = strlen(s); \
        while (len + _l + 2 > cap) { cap *= 2; buf = ne_xrealloc(buf, cap); } \
        memcpy(buf + len, s, _l); len += _l; buf[len] = 0; } while (0)
    for (size_t i = 0; i < j->len; i++) {
        if (i) APP("; ");
        APP(j->items[i].name); APP("="); APP(j->items[i].value);
    }
#undef APP
    return buf;
}

void ne_jar_merge_cookie_str(ne_jar *j, const char *cookie_str) {
    char *copy = ne_xstrdup(cookie_str);
    char *save = NULL;
    for (char *part = strtok_r(copy, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        while (isspace((unsigned char)*part)) part++;
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = '\0';
        ne_jar_set(j, part, eq + 1);   /* set() filters attrs + fake NMTID */
    }
    free(copy);
}
