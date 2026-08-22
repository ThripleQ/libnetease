/* netease-cli (C) — drop-in replacement for the Go shell (same name, same
 * stdout protocol). Commands are ported family by family; unported ones
 * print the same "unknown cmd" error the Go build prints for junk input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/stat.h>
#endif

#include "netease/cookiejar.h"
#include "netease/encoding.h"
#include "netease/jval.h"
#include "netease/qr.h"
#include "netease/qrenc.h"
#include "netease/request.h"
#include "netease/services.h"
#include "netease/util.h"

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    ne_jar_save_file(ne_global_jar(), ne_cookie_file());
    exit(1);
}

static void output(const char *body) {
    fputs(body ? body : "", stdout);
    fputc('\n', stdout);
}

/* recursive mkdir -p (os.MkdirAll) */
static void mkpath(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    size_t l = strlen(tmp);
    if (l > 0 && tmp[l - 1] == '/') tmp[l - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
#if defined(_WIN32)
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = '/';
        }
    }
#if defined(_WIN32)
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

/* main.go: ~/.cache/netune/cookies.txt
 * os.UserHomeDir(): $HOME on unix, %USERPROFILE% on Windows */
static const char *cookie_path(void) {
    static char path[1024];
    const char *home = getenv("HOME");
#if defined(_WIN32)
    if (!home || !*home) home = getenv("USERPROFILE");
#endif
    if (!home || !*home) home = ".";
    snprintf(path, sizeof path, "%s/.cache/netune/cookies.txt", home);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.cache/netune", home);
    mkpath(dir);
    return path;
}

/* ── small helpers shared by the read commands ─────────── */

/* strings.Join(os.Args[from:], " ") */
static char *join_args(int argc, char **argv, int from) {
    size_t total = 1;
    for (int i = from; i < argc; i++) total += strlen(argv[i]) + 1;
    char *out = ne_xmalloc(total);
    out[0] = '\0';
    for (int i = from; i < argc; i++) {
        if (i > from) strcat(out, " ");
        strcat(out, argv[i]);
    }
    return out;
}

/* json.Unmarshal into map[string]interface{} — 1 iff body is a JSON object.
 * On failure fills jval_err with the Go error text (%v of Unmarshal /
 * UnmarshalTypeError) for main.go's "parse account failed: %v". */
static char jval_err[192];

static const char *unmarshal_type_name(ne_jv_type t) {
    switch (t) {
        case NE_JV_STR:  return "string";
        case NE_JV_NUM:  return "number";
        case NE_JV_BOOL: return "bool";
        case NE_JV_ARR:  return "array";
        default:         return "value";
    }
}

static int parse_root(const char *body, ne_jval **out) {
    jval_err[0] = '\0';
    *out = ne_jval_parse(body ? body : "");
    if (!*out) {
        snprintf(jval_err, sizeof jval_err, "%s", ne_jval_last_error());
        return 0;
    }
    if (ne_jval_type(*out) != NE_JV_OBJ) {
        /* UnmarshalTypeError: body IS valid JSON, just the wrong shape */
        snprintf(jval_err, sizeof jval_err,
                 "json: cannot unmarshal %s into Go value of type map[string]interface {}",
                 unmarshal_type_name(ne_jval_type(*out)));
        ne_jval_free(*out);
        *out = NULL;
        return 0;
    }
    return 1;
}

/* die(fmt.Sprintf("parse account failed: %v", err)) */
static void die_parse_account(void) {
    char msg[256];
    snprintf(msg, sizeof msg, "parse account failed: %s",
             *jval_err ? jval_err : "unexpected end of JSON input");
    die(msg);
}

/* output(body) + cleanup — the plain passthrough commands */
static int pass(ne_resp *r) {
    output(r->body);
    ne_resp_free(r);
    return 0;
}

static void print_marshal(ne_jval *v) {
    char *b = ne_jval_marshal(v);
    printf("%s\n", b);
    free(b);
    ne_jval_free(v);
}

/* account.id from UserAccountService body (liked / liked-check / playlists);
 * returns 1 with *uid set, 0 on unparseable body (caller dies) */
static int account_uid(long long *uid) {
    ne_resp *r = ne_user_account();
    ne_jval *root = NULL;
    if (!parse_root(r->body, &root)) {
        ne_resp_free(r);
        return 0;
    }
    long long uid_v = 0;
    ne_jval *acc = ne_jval_get(root, "account");
    if (acc && ne_jval_type(acc) == NE_JV_OBJ) {
        ne_jval *idv = ne_jval_get(acc, "id");
        if (idv && ne_jval_type(idv) == NE_JV_NUM)
            uid_v = (long long)ne_jval_num(idv);
    }
    ne_jval_free(root);
    ne_resp_free(r);
    *uid = uid_v;
    return 1;
}

/* ── qr-key / qr-check (phase 4) ───────────────────────── */

static int cmd_qr_key(void) {
    double code;
    char *body; size_t body_len;
    char *unikey = ne_qr_get_key(&code, &body, &body_len);

    /* risk-control (-462) retry with a brand-new jar: stale login-fragment
     * cookies in the old file trigger it (main.go qr-key) */
    if (code != 200 || !*unikey) {
        remove(ne_cookie_file());
        ne_jar_reload();
        free(unikey); free(body);
        unikey = ne_qr_get_key(&code, &body, &body_len);
    }

    if (code != 200 || !*unikey) {
        char msg[512];
        const char *m = strstr(body ? body : "", "\"message\"");
        if (m) {
            char *end = strchr(m + 9, '"');
            if (end) {
                size_t n = (size_t)(end - m) < sizeof msg - 32 ? (size_t)(end - m) : sizeof msg - 32;
                snprintf(msg, sizeof msg, "get qr key failed: code=%.0f, %.*s", code, (int)n, m + 9);
            } else {
                snprintf(msg, sizeof msg, "get qr key failed: code=%.0f", code);
            }
        } else if (body && *body) {
            snprintf(msg, sizeof msg, "get qr key failed: code=%.0f, body=%s", code, body);
        } else {
            snprintf(msg, sizeof msg, "get qr key failed: code=%.0f", code);
        }
        free(unikey); free(body);
        die(msg);
    }

    char *url = ne_qr_build_url(unikey);
    /* hand-assembled JSON — the & in the URL must NOT become \u0026
     * (Go json.Marshal would escape it and break the QR payload) */
    printf("{\"unikey\":\"%s\",\"url\":\"%s\"}\n", unikey, url);
    free(url); free(unikey); free(body);
    return 0;
}

static int cmd_qr_check(const char *unikey) {
    double code;
    size_t body_len;
    char *body = ne_qr_check(unikey, &code, &body_len);
    if (!body) die("check failed");

    /* code 803 = confirmed: persist the server-issued cookie string */
    if ((long)code == 803) {
        /* "cookie":"..." or data.cookie in the body — extract raw values */
        const char *ck = strstr(body, "\"cookie\"");
        if (ck) {
            ck = strchr(ck + 8, ':');
            if (ck) {
                ck++;
                while (*ck == ' ' || *ck == '\t') ck++;
                if (*ck == '"') {
                    ck++;
                    char *end = strchr(ck, '"');
                    if (end) {
                        char *tmp = ne_xmalloc((size_t)(end - ck) + 1);
                        memcpy(tmp, ck, (size_t)(end - ck));
                        tmp[end - ck] = '\0';
                        ne_jar_merge_cookie_str(ne_global_jar(), tmp);
                        free(tmp);
                    }
                }
            }
        }
    }
    printf("{\"code\":%.0f,\"body\":%s}\n", code, body);
    free(body);
    return 0;
}

/* ── read commands (phase 5) ───────────────────────────── */

static int cmd_check_music(const char *id) {
    ne_resp *r = ne_check_music(id, NULL);
    int playable = 0;
    ne_jval *root = NULL;
    if (parse_root(r->body, &root)) {
        ne_jval *code = ne_jval_get(root, "code");
        ne_jval *data = ne_jval_get(root, "data");
        if (code && ne_jval_type(code) == NE_JV_NUM &&
            (long long)ne_jval_num(code) == 200 &&
            data && ne_jval_type(data) == NE_JV_ARR &&
            ne_jval_len(data) > 0) {
            ne_jval *url = ne_jval_get(ne_jval_at(data, 0), "url");
            if (url && ne_jval_type(url) == NE_JV_STR &&
                ne_jval_str(url)[0])
                playable = 1;
        }
    }
    ne_jval_free(root);
    ne_resp_free(r);
    /* Go: fmt.Sprintf — literal text, not json.Marshal */
    printf("{\"code\":200,\"playable\":%s}\n", playable ? "true" : "false");
    return 0;
}

static int cmd_song_url(const char *id, const char *level_in) {
    const char *level = level_in ? level_in : "standard";

    /* V1 first; restricted → fall back to the old linuxapi endpoint */
    ne_resp *v1 = ne_song_url_v1(id, level);
    int have_v1 = 0;
    ne_jval *root = NULL;
    if (v1->err == 0 && parse_root(v1->body, &root)) {
        ne_jval *items = ne_jval_get(root, "data");
        if (items && ne_jval_type(items) == NE_JV_ARR &&
            ne_jval_len(items) > 0) {
            ne_jval *item = ne_jval_at(items, 0);
            ne_jval *u = ne_jval_get(item, "url");
            if (u && ne_jval_type(u) == NE_JV_STR && ne_jval_str(u)[0])
                have_v1 = 1;
            /* item-level non-200 code or freeTrialInfo (non-null) forces
             * the fallback (main.go song-url) */
            ne_jval *ic = ne_jval_get(item, "code");
            int cleared = 0;
            if (ic && ne_jval_type(ic) == NE_JV_NUM &&
                ne_jval_num(ic) != 0 &&
                (long long)ne_jval_num(ic) != 200) {
                have_v1 = 0;
                cleared = 1;
            }
            if (!cleared) {
                ne_jval *ft = ne_jval_get(item, "freeTrialInfo");
                if (ft && ne_jval_type(ft) != NE_JV_NULL)
                    have_v1 = 0;
            }
        }
    }
    ne_jval_free(root);

    double final_code;
    char *final_body;
    if (have_v1) {
        final_code = v1->code;
        final_body = ne_xstrdup(v1->body);
        ne_resp_free(v1);
    } else {
        ne_resp_free(v1);
        const char *br = "320000";
        if (strcmp(level, "lossless") == 0 || strcmp(level, "hires") == 0)
            br = "999000";
        ne_resp *old = ne_song_url_old(id, br);
        final_code = old->code;
        final_body = ne_xstrdup(old->body);
        ne_resp_free(old);
    }

    /* re-marshal with the final code (map semantics: sorted keys) */
    ne_jval *out = NULL;
    if (!parse_root(final_body, &out)) {
        ne_jval_free(out);
        out = ne_jval_new(NE_JV_OBJ);
    }
    ne_jval_put(out, "code", ne_jval_new_num_d(final_code));
    print_marshal(out);
    free(final_body);
    return 0;
}

/* song-download-url <id> [level] — official download endpoint (original
 * apiservice). data comes back as a single object; wrap it in an array so
 * the C consumer parses it like the play-URL response. */
static int cmd_song_download_url(const char *id, const char *level_in) {
    const char *level = level_in ? level_in : "standard";
    ne_resp *r = ne_song_download_url(id, level);
    if (r->err) {
        char msg[256];
        snprintf(msg, sizeof msg, "download url failed: err=%d", r->err);
        ne_resp_free(r);
        die(msg);
    }
    ne_jval *root = NULL;
    if (!parse_root(r->body, &root)) {
        ne_resp_free(r);
        die("bad download url response");
    }
    ne_jval *d = ne_jval_get(root, "data");
    if (d && ne_jval_type(d) == NE_JV_OBJ) {
        ne_jval *arr = ne_jval_new(NE_JV_ARR);
        ne_jval_push(arr, ne_jval_clone(d));
        ne_jval_put(root, "data", arr);
    }
    ne_jval_put(root, "code", ne_jval_new_num_d(r->code));
    ne_resp_free(r);
    print_marshal(root);
    return 0;
}

/* liked / liked-check / playlists share account+likelist scaffolding */
static ne_jval *like_ids_of(long long uid, ne_resp **resp_out) {
    char uid_str[32];
    snprintf(uid_str, sizeof uid_str, "%lld", uid);
    ne_resp *r = ne_like_list(uid_str);
    ne_jval *root = NULL;
    parse_root(r->body, &root);   /* Go ignores the error here (liked-check) */
    ne_jval *ids = root ? ne_jval_get(root, "ids") : NULL;
    if (!ids || ne_jval_type(ids) != NE_JV_ARR) {
        ne_jval_free(root);
        ne_resp_free(r);
        *resp_out = NULL;
        return NULL;
    }
    /* detach the array from the tree, free the rest */
    ne_jval *ids_copy = ne_jval_clone(ids);
    ne_jval_free(root);
    if (resp_out) *resp_out = r; else ne_resp_free(r);
    return ids_copy;
}

static int cmd_liked(void) {
    long long uid = 0;
    if (!account_uid(&uid)) die_parse_account();
    if (uid == 0) die("failed to get uid, need login first");

    ne_jval *ids = like_ids_of(uid, NULL);
    if (!ids) {
        /* Go: parse failure → "parse liked failed: %v"; valid JSON but
         * ids missing/empty → "no liked songs or parse failed" */
        if (*jval_err) {
            char msg[256];
            snprintf(msg, sizeof msg, "parse liked failed: %s", jval_err);
            die(msg);
        }
        die("no liked songs or parse failed");
    }
    if (ne_jval_len(ids) == 0) die("no liked songs or parse failed");

    /* "%.0f" per element, then 200-id batches of SongDetail */
    size_t total = ne_jval_len(ids);
    char **id_strs = ne_xmalloc((total ? total : 1) * sizeof(char *));
    size_t n = 0;
    for (size_t i = 0; i < total; i++) {
        ne_jval *e = ne_jval_at(ids, i);
        if (e && ne_jval_type(e) == NE_JV_NUM) {
            char buf[32];
            snprintf(buf, sizeof buf, "%.0f", ne_jval_num(e));
            id_strs[n++] = ne_xstrdup(buf);
        }
    }
    ne_jval_free(ids);
    if (n == 0) die("no liked songs");

    ne_jval *all = ne_jval_new(NE_JV_ARR);
    for (size_t i = 0; i < n; i += 200) {
        size_t end = i + 200 < n ? i + 200 : n;
        size_t csv_len = 1;
        for (size_t k = i; k < end; k++) csv_len += strlen(id_strs[k]) + 1;
        char *csv = ne_xmalloc(csv_len);
        csv[0] = '\0';
        for (size_t k = i; k < end; k++) {
            if (k > i) strcat(csv, ",");
            strcat(csv, id_strs[k]);
        }
        ne_resp *dr = ne_song_detail(csv);
        free(csv);
        ne_jval *droot = NULL;
        if (parse_root(dr->body, &droot)) {
            ne_jval *songs = ne_jval_get(droot, "songs");
            if (songs && ne_jval_type(songs) == NE_JV_ARR)
                for (size_t k = 0; k < ne_jval_len(songs); k++)
                    ne_jval_push(all, ne_jval_clone(ne_jval_at(songs, k)));
        }
        ne_jval_free(droot);
        ne_resp_free(dr);
    }
    for (size_t i = 0; i < n; i++) free(id_strs[i]);
    free(id_strs);

    if (ne_jval_len(all) > 0) {
        ne_jval *out = ne_jval_new(NE_JV_OBJ);
        ne_jval_put(out, "code", ne_jval_new_num_d(200));
        ne_jval *res = ne_jval_new(NE_JV_OBJ);
        ne_jval_put(res, "songs", all);
        ne_jval_put(out, "result", res);
        print_marshal(out);
    } else {
        ne_jval_free(all);
        printf("\n");   /* Go: fmt.Println() on empty */
    }
    return 0;
}

static int cmd_liked_check(const char *song_id) {
    long long uid = 0;
    if (!account_uid(&uid)) die_parse_account();
    if (uid == 0) die("failed to get uid, need login first");

    int liked = 0;
    ne_jval *ids = like_ids_of(uid, NULL);
    if (ids) {
        for (size_t i = 0; i < ne_jval_len(ids); i++) {
            ne_jval *e = ne_jval_at(ids, i);
            if (e && ne_jval_type(e) == NE_JV_NUM) {
                char buf[32];
                snprintf(buf, sizeof buf, "%.0f", ne_jval_num(e));
                if (strcmp(buf, song_id) == 0) {
                    liked = 1;
                    break;
                }
            }
        }
        ne_jval_free(ids);
    }
    printf("{\"code\":200,\"liked\":%s}\n", liked ? "true" : "false");
    return 0;
}

static int cmd_playlists(void) {
    long long uid = 0;
    if (!account_uid(&uid)) die_parse_account();
    if (uid == 0) die("failed to get uid, need login first");

    char uid_str[32];
    snprintf(uid_str, sizeof uid_str, "%lld", uid);

    ne_jval *all = ne_jval_new(NE_JV_ARR);
    int offset = 0;
    for (;;) {
        char limit_str[16], offset_str[16];
        snprintf(limit_str, sizeof limit_str, "100");
        snprintf(offset_str, sizeof offset_str, "%d", offset);
        ne_resp *r = ne_user_playlist(uid_str, limit_str, offset_str);
        ne_jval *root = NULL;
        if (!parse_root(r->body, &root)) {
            output(r->body);       /* Go prints the raw body and returns */
            ne_resp_free(r);
            ne_jval_free(all);
            return 0;
        }
        ne_jval *pl = ne_jval_get(root, "playlist");
        if (!pl || ne_jval_type(pl) != NE_JV_ARR || ne_jval_len(pl) == 0) {
            ne_jval_free(root);
            ne_resp_free(r);
            break;
        }
        for (size_t i = 0; i < ne_jval_len(pl); i++) {
            ne_jval *pm = ne_jval_at(pl, i);
            if (!pm || ne_jval_type(pm) != NE_JV_OBJ) continue;
            long long pid = 0;
            const char *name = NULL;
            int subscribed = 0;
            ne_jval *idv = ne_jval_get(pm, "id");
            if (idv && ne_jval_type(idv) == NE_JV_NUM)
                pid = (long long)ne_jval_num(idv);
            ne_jval *nv = ne_jval_get(pm, "name");
            if (nv && ne_jval_type(nv) == NE_JV_STR)
                name = ne_jval_str(nv);
            ne_jval *sv = ne_jval_get(pm, "subscribed");
            if (sv && ne_jval_type(sv) == NE_JV_BOOL)
                subscribed = ne_jval_bool(sv);
            if (pid > 0) {
                ne_jval *o = ne_jval_new(NE_JV_OBJ);
                char idbuf[32];
                snprintf(idbuf, sizeof idbuf, "%lld", pid);
                ne_jval_put(o, "id", ne_jval_new_num(idbuf));
                ne_jval_put(o, "name", ne_jval_new_str(name ? name : ""));
                ne_jval_put(o, "subscribed", ne_jval_new_bool(subscribed));
                ne_jval_push(all, o);
            }
        }
        size_t got = ne_jval_len(pl);
        ne_jval_free(root);
        ne_resp_free(r);
        if ((int)got < 100) break;
        offset += 100;
    }

    ne_jval *out = ne_jval_new(NE_JV_OBJ);
    ne_jval_put(out, "code", ne_jval_new_num_d(200));
    ne_jval_put(out, "playlists", all);
    print_marshal(out);
    return 0;
}

static int cmd_lyric(const char *id) {
    ne_resp *r = ne_lyric(id);
    ne_jval *root = NULL;
    if (!parse_root(r->body, &root)) {
        output(r->body);
        ne_resp_free(r);
        return 0;
    }

    /* prefer tlyric (translated), fallback to lrc; klyric when present */
    const char *lyric_text = "", *klyric_text = "";
    ne_jval *t = ne_jval_get(root, "tlyric");
    if (t && ne_jval_type(t) == NE_JV_OBJ) {
        ne_jval *l = ne_jval_get(t, "lyric");
        if (l && ne_jval_type(l) == NE_JV_STR) lyric_text = ne_jval_str(l);
    }
    if (!*lyric_text) {
        ne_jval *lrc = ne_jval_get(root, "lrc");
        if (lrc && ne_jval_type(lrc) == NE_JV_OBJ) {
            ne_jval *l = ne_jval_get(lrc, "lyric");
            if (l && ne_jval_type(l) == NE_JV_STR) lyric_text = ne_jval_str(l);
        }
    }
    ne_jval *kl = ne_jval_get(root, "klyric");
    if (kl && ne_jval_type(kl) == NE_JV_OBJ) {
        ne_jval *l = ne_jval_get(kl, "lyric");
        if (l && ne_jval_type(l) == NE_JV_STR) klyric_text = ne_jval_str(l);
    }

    ne_jval *out = ne_jval_new(NE_JV_OBJ);
    ne_jval *code = ne_jval_get(root, "code");
    if (code && ne_jval_type(code) == NE_JV_NUM)
        ne_jval_put(out, "code", ne_jval_new_num(ne_jval_num_lexeme(code)));
    if (*lyric_text)
        ne_jval_put(out, "lyric", ne_jval_new_str(lyric_text));
    if (*klyric_text)
        ne_jval_put(out, "klyric", ne_jval_new_str(klyric_text));
    ne_jval_free(root);
    ne_resp_free(r);
    print_marshal(out);
    return 0;
}

static int cmd_playlist_tracks(const char *id) {
    ne_resp *r = ne_playlist_detail(id, "0");
    ne_jval *root = NULL;
    if (!parse_root(r->body, &root)) {
        output(r->body);
        ne_resp_free(r);
        return 0;
    }
    ne_jval *pl = ne_jval_get(root, "playlist");
    if (!pl || ne_jval_type(pl) != NE_JV_OBJ) {
        output(r->body);
        ne_jval_free(root);
        ne_resp_free(r);
        return 0;
    }
    ne_jval *tracks = ne_jval_get(pl, "tracks");
    if (!tracks || ne_jval_type(tracks) != NE_JV_ARR) {
        output(r->body);
        ne_jval_free(root);
        ne_resp_free(r);
        return 0;
    }

    ne_jval *out = ne_jval_new(NE_JV_OBJ);
    ne_jval_put(out, "code", ne_jval_new_num_d(200));
    ne_jval *res = ne_jval_new(NE_JV_OBJ);
    ne_jval_put(res, "songs", ne_jval_clone(tracks));
    ne_jval_put(out, "result", res);
    ne_jval_free(root);
    ne_resp_free(r);
    print_marshal(out);
    return 0;
}

static int cmd_account_name(void) {
    ne_resp *r = ne_user_account();
    ne_jval *root = NULL;
    if (!parse_root(r->body, &root)) {
        ne_resp_free(r);
        puts("error");
        return 0;
    }
    const char *name = NULL;
    ne_jval *prof = ne_jval_get(root, "profile");
    if (prof && ne_jval_type(prof) == NE_JV_OBJ) {
        ne_jval *n = ne_jval_get(prof, "nickname");
        if (n && ne_jval_type(n) == NE_JV_STR) name = ne_jval_str(n);
    }
    if (!name || !*name) {
        ne_jval *acc = ne_jval_get(root, "account");
        if (acc && ne_jval_type(acc) == NE_JV_OBJ) {
            ne_jval *n = ne_jval_get(acc, "userName");
            if (n && ne_jval_type(n) == NE_JV_STR) name = ne_jval_str(n);
        }
    }
    puts(name && *name ? name : "未登录");
    ne_jval_free(root);
    ne_resp_free(r);
    return 0;
}

/* ── write family (phase 6) — the {"code":%.0f,"body":%s} envelope ── */

/* output() for CreateRequest-style (code, body) pairs — body spliced RAW,
 * byte-identical to Go's fmt.Sprintf envelope (never re-marshalled) */
static int envelope(ne_resp *r) {
    printf("{\"code\":%.0f,\"body\":%s}\n", r->code, r->body ? r->body : "");
    ne_resp_free(r);
    return 0;
}

/* like <song_id> [true|false] — the shell bypasses LikeService and calls
 * CreateRequest on weapi/song/like directly with os=pc appver=2.7.1.198277 */
static int cmd_like(const char *id, const char *like) {
    jmap *data = jmap_new();
    jmap_put(data, "trackId", id);
    jmap_put(data, "like", like ? like : "true");
    static const char *extras[] = {
        "os", "pc", "appver", "2.7.1.198277", NULL
    };
    char url[640];
    snprintf(url, sizeof url, "%s/weapi/song/like", ne_api_base());
    ne_resp *r = ne_create_weapi(url, data, extras);
    jmap_free(data);
    return envelope(r);
}

static int cmd_subscribe(const char *id, const char *t) {
    return envelope(ne_playlist_subscribe(id, t ? t : "0"));
}

static int cmd_track(const char *op, const char *pid, const char *sid) {
    return envelope(ne_playlist_tracks(op, pid, sid));
}

int main(int argc, char **argv) {
#if defined(_WIN32)
    /* Go's os.Stdout is binary mode (no \n→\r\n translation); match it so
     * the C shell stays byte-identical on Windows */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    if (argc < 2) {
        fprintf(stderr, "usage: netease-cli <cmd> [args...]\n");
        return 1;
    }

    ne_set_cookie_file(cookie_path());
    ne_jar_reload();

    const char *cmd = argv[1];
    int rc = 0;

    if (strcmp(cmd, "qr-render") == 0) {
        if (argc < 3) die("usage: netease-cli qr-render <url>");
        const char *err = NULL;
        ne_qr *qr = ne_qr_new(argv[2], &err);
        if (!qr) {
            char msg[256];
            snprintf(msg, sizeof msg, "qr error: %s", err);
            die(msg);
        }
        char *art = ne_qr_small_string(qr);
        fputs(art, stdout);
        free(art);
        ne_qr_free(qr);
    } else if (strcmp(cmd, "qr-image") == 0) {
        if (argc < 3) die("usage: netease-cli qr-image <url>");
        const char *err = NULL;
        ne_qr *qr = ne_qr_new(argv[2], &err);
        if (!qr) {
            char msg[256];
            snprintf(msg, sizeof msg, "qr error: %s", err);
            die(msg);
        }
        size_t len = 0;
        unsigned char *png = ne_qr_png(qr, 480, &len);
        if (!png) die("qr png error: encode failed");
        char *b64 = ne_base64_encode(png, len);
        output(b64);
        free(b64);
        free(png);
        ne_qr_free(qr);
    } else if (strcmp(cmd, "qr-key") == 0) {
        rc = cmd_qr_key();
    } else if (strcmp(cmd, "qr-check") == 0) {
        if (argc < 3) die("usage: netease-cli qr-check <unikey>");
        rc = cmd_qr_check(argv[2]);
    } else if (strcmp(cmd, "login-status") == 0) {
        printf("{\"status\":\"check %s\"}\n", ne_cookie_file());
    } else if (strcmp(cmd, "search") == 0) {
        char *s = join_args(argc, argv, 2);
        rc = pass(ne_search(s, "1", "100", NULL));
        free(s);
    } else if (strcmp(cmd, "search-pl") == 0) {
        char *s = join_args(argc, argv, 2);
        rc = pass(ne_search(s, "1000", "20", NULL));
        free(s);
    } else if (strcmp(cmd, "check-music") == 0) {
        if (argc < 3) die("usage: netease-cli check-music <song_id>");
        rc = cmd_check_music(argv[2]);
    } else if (strcmp(cmd, "record-recent") == 0) {
        rc = pass(ne_record_recent(argc > 2 ? argv[2] : "100"));
    } else if (strcmp(cmd, "recommend-resource") == 0) {
        rc = pass(ne_recommend_resource());
    } else if (strcmp(cmd, "song-url") == 0) {
        if (argc < 3) die("usage: netease-cli song-url <id> [level]");
        rc = cmd_song_url(argv[2], argc > 3 ? argv[3] : NULL);
    } else if (strcmp(cmd, "song-download-url") == 0) {
        if (argc < 3) die("usage: netease-cli song-download-url <id> [level]");
        rc = cmd_song_download_url(argv[2], argc > 3 ? argv[3] : NULL);
    } else if (strcmp(cmd, "song-detail") == 0) {
        if (argc < 3) die("usage: netease-cli song-detail <ids>");
        rc = pass(ne_song_detail(argv[2]));
    } else if (strcmp(cmd, "playlist") == 0) {
        if (argc < 3) die("usage: netease-cli playlist <id>");
        rc = pass(ne_playlist_detail(argv[2], "0"));
    } else if (strcmp(cmd, "user-playlist") == 0) {
        if (argc < 3) die("usage: netease-cli user-playlist <uid>");
        rc = pass(ne_user_playlist(argv[2], NULL, NULL));
    } else if (strcmp(cmd, "liked") == 0) {
        rc = cmd_liked();
    } else if (strcmp(cmd, "liked-check") == 0) {
        if (argc < 3) die("usage: netease-cli liked-check <song_id>");
        rc = cmd_liked_check(argv[2]);
    } else if (strcmp(cmd, "toplist") == 0) {
        rc = pass(ne_toplist_detail());
    } else if (strcmp(cmd, "recommend-songs") == 0) {
        rc = pass(ne_recommend_songs());
    } else if (strcmp(cmd, "recommend-playlists") == 0) {
        rc = pass(ne_recommend_playlists("30"));
    } else if (strcmp(cmd, "lyric") == 0) {
        if (argc < 3) die("usage: netease-cli lyric <song_id>");
        rc = cmd_lyric(argv[2]);
    } else if (strcmp(cmd, "playlist-tracks") == 0) {
        if (argc < 3) die("usage: netease-cli playlist-tracks <id>");
        rc = cmd_playlist_tracks(argv[2]);
    } else if (strcmp(cmd, "account-name") == 0) {
        rc = cmd_account_name();
    } else if (strcmp(cmd, "playlists") == 0) {
        rc = cmd_playlists();
    } else if (strcmp(cmd, "login-email") == 0) {
        if (argc < 4) die("usage: netease-cli login-email <email> <password>");
        rc = pass(ne_login_email(argv[2], argv[3]));
    } else if (strcmp(cmd, "login-cellphone") == 0) {
        if (argc < 4) die("usage: netease-cli login-cellphone <phone> <password>");
        rc = pass(ne_login_cellphone(argv[2], argv[3]));
    } else if (strcmp(cmd, "login-refresh") == 0) {
        rc = pass(ne_login_refresh());
    } else if (strcmp(cmd, "like") == 0) {
        if (argc < 3) die("usage: netease-cli like <song_id> [true|false]");
        rc = cmd_like(argv[2], argc > 3 ? argv[3] : NULL);
    } else if (strcmp(cmd, "subscribe") == 0) {
        if (argc < 3) die("usage: netease-cli subscribe <playlist_id> [1|0]");
        rc = cmd_subscribe(argv[2], argc > 3 ? argv[3] : NULL);
    } else if (strcmp(cmd, "track-add") == 0) {
        if (argc < 4) die("usage: netease-cli track-add <playlist_id> <song_id>");
        rc = cmd_track("add", argv[2], argv[3]);
    } else if (strcmp(cmd, "track-del") == 0) {
        if (argc < 4) die("usage: netease-cli track-del <playlist_id> <song_id>");
        rc = cmd_track("del", argv[2], argv[3]);
    } else if (strcmp(cmd, "playlist-create") == 0) {
        if (argc < 3) die("usage: netease-cli playlist-create <name>");
        rc = envelope(ne_playlist_create(argv[2], "0"));
    } else if (strcmp(cmd, "playlist-rename") == 0) {
        if (argc < 4) die("usage: netease-cli playlist-rename <playlist_id> <new_name>");
        rc = envelope(ne_playlist_update_name(argv[2], argv[3]));
    } else if (strcmp(cmd, "playlist-delete") == 0) {
        if (argc < 3) die("usage: netease-cli playlist-delete <playlist_id>");
        rc = envelope(ne_playlist_delete(argv[2]));
    } else {
        fprintf(stderr, "unknown cmd: %s\n", cmd);
        rc = 1;
    }

    /* persist jar (filterJar already dropped the fake NMTID in-memory too) */
    ne_jar_save_file(ne_global_jar(), ne_cookie_file());
    return rc;
}
