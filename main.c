/* CLI harness for P0: build an index, then query it.
 *   esidx [-v] build <root> -o <dbfile>
 *   esidx [-v] query <dbfile> [expr ...]
 *
 * expr (subset of Everything syntax, §1.3):
 *   parent:"<path>"     direct children of <path>
 *   folder:  file:      type filter
 *   ext:jpg;png         extension filter
 *   size:>1M  size:100..2M   dm:>7d
 *   sort:size:desc  count:20  offset:40
 *   <word>              substring match on name (case-insensitive)
 *
 * NOTE: quote every expr in the shell. `size:>1k` unquoted is a redirection,
 * not a query. test.sh shows the correct form.
 *
 * Diagnostics: ESIDX_LOG=error|warn|info|debug, or -v / -v N / --verbose=N.
 * All of it goes to stderr; stdout carries only results.
 */

#include "esidx.h"
#include "timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  esidx [-v N] build <root> -o <dbfile>\n"
        "  esidx [-v N] query <dbfile> [expr ...]\n"
        "\n"
        "logging: ESIDX_LOG=error|warn|info|debug  or  -v / -v N / --verbose=N\n");
}

/* ------------------------------------------------------------- arg parsing */

static int64_t parse_size(const char *s)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (end) {
        switch (*end) {
        case 't': case 'T': v *= 1024.0 * 1024.0 * 1024.0 * 1024.0; break;
        case 'g': case 'G': v *= 1024.0 * 1024.0 * 1024.0; break;
        case 'm': case 'M': v *= 1024.0 * 1024.0; break;
        case 'k': case 'K': v *= 1024.0; break;
        default: break;
        }
    }
    return (int64_t)v;
}

static int64_t parse_time_val(const char *s)
{
    time_t now = time(NULL);

    if (!strcasecmp(s, "today")) {
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        return (int64_t)mktime(&tm);
    }
    if (!strcasecmp(s, "yesterday")) {
        struct tm tm;
        localtime_r(&now, &tm);
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
        return (int64_t)mktime(&tm) - 86400;
    }
    if (*s == 'd' && s[1] && isdigit((unsigned char)s[1])) {
        /* relative: 7d = within last 7 days */
        long days = strtol(s + 1, NULL, 10);
        return now - days * 86400;
    }
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    return (int64_t)v;
}

/* parse "OP VALUE" where OP in {>, <, >=, <=} ; also "a..b" / "a-b" */
static void apply_cmp(const char *v, int64_t *lo, int64_t *hi, int is_time)
{
    int64_t (*pv)(const char *) = is_time ? parse_time_val : parse_size;

    const char *dots = strstr(v, "..");
    if (dots) {
        char a[64], b[64];
        size_t la = (size_t)(dots - v);
        if (la >= sizeof(a)) la = sizeof(a) - 1;
        memcpy(a, v, la); a[la] = '\0';
        snprintf(b, sizeof(b), "%s", dots + 2);
        *lo = pv(a); *hi = pv(b);
        LOGD("cmp: %s -> range [%lld, %lld]", v, (long long)*lo, (long long)*hi);
        return;
    }
    if (v[0] == '>') {
        if (v[1] == '=') *lo = pv(v + 2);
        else             *lo = pv(v + 1) + 1;
    } else if (v[0] == '<') {
        if (v[1] == '=') *hi = pv(v + 2);
        else             *hi = pv(v + 1) - 1;
    } else if (v[0] == '=') {
        int64_t x = pv(v + 1); *lo = x; *hi = x;
    } else {
        int64_t x = pv(v); *lo = x; *hi = x;
    }
    LOGD("cmp: %s -> range [%lld, %lld]", v, (long long)*lo, (long long)*hi);
}

/* ------------------------------------------------------------------- build */

static int cmd_build(int argc, char **argv)
{
    const char *root = NULL, *out = "esidx.db";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { out = argv[++i]; continue; }
        if (!root) root = argv[i];
    }
    if (!root) { usage(); return 1; }

    esidx_t db;
    esidx_init(&db);

    uint64_t t0 = ts_us();
    if (esidx_scan(&db, root) != 0) {
        fprintf(stderr, "scan failed: %s\n", root);
        esidx_free(&db);
        return 1;
    }
    uint64_t t_scan = ts_us();

    esidx_finalize(&db);
    uint64_t t_fin = ts_us();

    const scan_stats_t *st = esidx_scan_stats(&db);
    fprintf(stderr, "indexed %u entries in %.1f ms  (scan %.1f ms, finalize %.1f ms)\n",
            db.et.count, (double)(t_fin - t0) / 1000.0,
            (double)(t_scan - t0) / 1000.0, (double)(t_fin - t_scan) / 1000.0);
    esidx_log_stats(&db, "build");
    LOGI("build: %.0f entries/s overall", (double)db.et.count / ((double)(t_fin - t0) / 1e6));
    if (st->entries && db.et.count < st->entries)
        LOGW("build: indexed %u of %llu seen entries (truncated at depth %llu?)",
             db.et.count, (unsigned long long)st->entries,
             (unsigned long long)st->depth_max);

    if (esidx_save(&db, out) != 0) {
        fprintf(stderr, "save failed: %s\n", out);
        esidx_free(&db);
        return 1;
    }
    TSDONE("build: total", t0);
    fprintf(stderr, "saved -> %s\n", out);
    esidx_free(&db);
    return 0;
}

/* ------------------------------------------------------------------- query */

static int cmd_query(int argc, char **argv)
{
    if (argc < 1) { usage(); return 1; }
    const char *dbfile = argv[0];

    esidx_t db;
    esidx_init(&db);

    uint64_t t0 = ts_us();
    if (esidx_load(&db, dbfile) != 0) {
        fprintf(stderr, "load failed: %s\n", dbfile);
        esidx_free(&db);
        return 1;
    }
    uint64_t t_load = ts_us();
    double lms = (double)(t_load - t0) / 1000.0;

    query_t q;
    query_init(&q);
    uint16_t extbuf[64];
    uint32_t next = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncasecmp(a, "parent:", 7)) {
            eid_t id = di_lookup(&db, a + 7);
            if (id == EID_NONE) {
                fprintf(stderr, "no such dir: %s\n", a + 7);
                LOGD("parent lookup miss: %s", a + 7);
                esidx_free(&db);
                return 1;
            }
            q.parent = (int)id;
            LOGD("parent:%s -> eid %u", a + 7, id);
        } else if (!strcasecmp(a, "folder:")) {
            q.type_filter = 1;
        } else if (!strcasecmp(a, "file:")) {
            q.type_filter = 2;
        } else if (!strncasecmp(a, "ext:", 4)) {
            const char *p = a + 4;
            while (*p && next < 64) {
                const char *e = strchr(p, ';');
                size_t l = e ? (size_t)(e - p) : strlen(p);
                char buf[32];
                if (l >= sizeof(buf)) l = sizeof(buf) - 1;
                /* ext ids are interned lowercase (see ext_of), so fold here
                 * too -- Everything's ext: is case-insensitive */
                for (size_t i = 0; i < l; i++)
                    buf[i] = (char)tolower((unsigned char)p[i]);
                buf[l] = '\0';
                if (l)
                    extbuf[next++] = ext_intern((esidx_t *)&db, buf);
                if (!e) break;
                p = e + 1;
            }
        } else if (!strncasecmp(a, "size:", 5)) {
            apply_cmp(a + 5, &q.size_lo, &q.size_hi, 0);
        } else if (!strncasecmp(a, "dm:", 3)) {
            apply_cmp(a + 3, &q.mtime_lo, &q.mtime_hi, 1);
        } else if (!strncasecmp(a, "sort:", 5)) {
            char key[32] = {0}, ord[32] = {0};
            sscanf(a + 5, "%31[^:]:%31s", key, ord);
            if      (!strcasecmp(key, "size"))  q.sort_key = SORT_SIZE;
            else if (!strcasecmp(key, "mtime") ||
                     !strcasecmp(key, "date_modified")) q.sort_key = SORT_MTIME;
            else if (!strcasecmp(key, "path"))  q.sort_key = SORT_PATH;
            else if (!strcasecmp(key, "ext") ||
                     !strcasecmp(key, "extension")) q.sort_key = SORT_EXT;
            else q.sort_key = SORT_NAME;
            q.sort_desc = (!strcasecmp(ord, "desc") || !strcasecmp(ord, "descending"));
            LOGD("sort:%s -> key=%d desc=%d", a + 5, (int)q.sort_key, q.sort_desc);
        } else if (!strncasecmp(a, "count:", 6)) {
            q.count = (uint32_t)strtoul(a + 6, NULL, 10);
        } else if (!strncasecmp(a, "offset:", 7)) {
            q.offset = (uint32_t)strtoul(a + 7, NULL, 10);
        } else {
            q.name_substr = a;
            LOGD("bare word -> substring %s", a);
        }
    }
    q.ext_ids = extbuf;
    q.next = next;

    uint64_t tq = ts_us();
    eid_t *res = NULL;
    uint32_t nr = query_exec(&db, &q, &res);
    uint64_t t1 = ts_us();
    double qms = (double)(t1 - tq) / 1000.0;

    char *pbuf = malloc(65536);
    for (uint32_t i = 0; i < nr; i++) {
        path_of(&db, res[i], pbuf, 65536);
        char tstr[32] = "";
        time_t mt = (time_t)db.et.mtime[res[i]];
        struct tm tm;
        if (localtime_r(&mt, &tm)) strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M", &tm);
        printf("%c %12lld  %s  %s\n",
               (db.et.flags[res[i]] & EF_DIR) ? 'd' : '-',
               (long long)db.et.size[res[i]], tstr, pbuf);
    }
    free(pbuf);
    free(res);

    fprintf(stderr, "loaded in %.1f ms, %u results in %.1f ms (end-to-end %.1f ms)\n",
            lms, nr, qms, (double)(t1 - t0) / 1000.0);
    esidx_free(&db);
    return 0;
}

int main(int argc, char **argv)
{
    log_init(argc, argv);
    argc = log_strip_flags(argc, argv);   /* so argv[1] is always the subcommand */
    LOGD("log level: %s", log_level_name(log_level()));

    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "build")) return cmd_build(argc - 2, argv + 2);
    if (!strcmp(argv[1], "query")) return cmd_query(argc - 2, argv + 2);
    usage();
    return 1;
}
