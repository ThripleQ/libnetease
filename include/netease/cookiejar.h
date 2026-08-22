#ifndef NE_COOKIEJAR_H
#define NE_COOKIEJAR_H
#include <stddef.h>

/* In-memory cookie jar for https://music.163.com, persisted as Netscape
 * cookies.txt — byte-compatible with the Go shell's FileJar format:
 *   music.163.com\tFALSE\t/\tFALSE\t253402300799\t<name>\t<value>
 *
 * filterJar semantics (main.go): the anti-fraud strategy's FIXED fake NMTID
 * ("some_random_id_from_strategy") is never persisted — dropping it here
 * covers both the jar and the file. */

typedef struct ne_jar ne_jar;

ne_jar *ne_jar_new(void);
void ne_jar_free(ne_jar *j);

/* load/disk */
int ne_jar_load_file(ne_jar *j, const char *path);      /* tolerant */
int ne_jar_save_file(const ne_jar *j, const char *path);

/* update from a parsed "name=value" cookie; attribute names (Path, Domain,
 * Expires, ...) are rejected; fake NMTID rejected (filterJar). */
void ne_jar_set(ne_jar *j, const char *name, const char *value);

/* lookup; returns NULL if absent */
const char *ne_jar_get(const ne_jar *j, const char *name);

/* "k1=v1; k2=v2" Cookie header value (malloc'd) — jar order */
char *ne_jar_cookie_header(const ne_jar *j);

/* merge a "k=v; k2=v2" string into the jar (saveNeteaseCookies semantics:
 * filter attribute names + fake NMTID; here values are replaced in-memory) */
void ne_jar_merge_cookie_str(ne_jar *j, const char *cookie_str);
#endif
