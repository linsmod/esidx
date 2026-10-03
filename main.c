/* CLI harness and ETP server entry point.
 *   esidx [-v] build <root> -o <dbfile>
 *   esidx [-v] query <dbfile> [expr ...]
 *   esidx [-v] serve <dbfile> [-p port] [--bind addr] [-u user [-w pass]]
 *
 * query takes the Everything search language, parsed by the real front end
 * (syntax.h) -- see design §6.1. Leading or trailing `sort:`, `count:` and
 * `offset:` are stripped as CLI sugar; everything else is the search string.
 * Quote every expression in the shell: `size:>1k` unquoted is a redirection, not
 * a query.
 *
 * serve speaks ETP: FTP plus `SITE EVERYTHING` (design §1). Diagnostics:
 * ESIDX_LOG=error|warn|info|debug, or -v / -v N / --verbose=N.
 */

#include "esidx.h"
#include "etp.h"
#include "timer.h"

#include <dirent.h>
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
        "  esidx [-v N] serve <dbfile> [-p port] [--bind addr]\n"
        "                             [-u user [-w pass]] [--no-download] [--once]\n"
        "\n"
        "logging: ESIDX_LOG=error|warn|info|debug  or  -v / -v N / --verbose=N\n");
}

/* ------------------------------------------------------------- arg parsing */

/* NOTE: the query string is parsed by the real front end (syntax.h), not here.
 * These helpers are gone; what remains of CLI parsing is the subcommand table
 * and the leading sort:/count:/offset: options, which exist only so the test
 * suite can drive the executor without a protocol layer. */

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

/* Everything past argv[1] is joined into one search string and handed to the real
 * parser. The old hand-rolled token loop could not express `a | b`, `!x`,
 * `size:<1m` or `<a b>` groups, which is exactly the set of forms the ETP client
 * sends.
 *
 * `sort:`, `count:` and `offset:` are pulled out wherever they appear rather than
 * only at the front, because they are the CLI's stand-ins for the ETP sort/OFFSET/
 * COUNT subcommands and the test suite interleaves them with search terms. They
 * are not search functions in the language, so nothing is lost by removing them. */
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

    sort_spec_t sort = { SORT_NAME, 0 };
    uint32_t offset = 0, count = 0;

    size_t need = 1;
    for (int i = 1; i < argc; i++) need += strlen(argv[i]) + 1;
    char *expr = malloc(need);
    if (!expr) { esidx_free(&db); return 1; }
    size_t elen = 0;
    expr[0] = '\0';

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncasecmp(a, "sort:", 5)) {
            char key[32] = {0}, ord[32] = {0};
            sscanf(a + 5, "%31[^:]:%31s", key, ord);
            if      (!strcasecmp(key, "size"))  sort.key = SORT_SIZE;
            else if (!strcasecmp(key, "mtime") ||
                     !strcasecmp(key, "date_modified")) sort.key = SORT_MTIME;
            else if (!strcasecmp(key, "ctime") ||
                     !strcasecmp(key, "date_created"))  sort.key = SORT_CTIME;
            else if (!strcasecmp(key, "path"))  sort.key = SORT_PATH;
            else if (!strcasecmp(key, "ext") ||
                     !strcasecmp(key, "extension")) sort.key = SORT_EXT;
            else sort.key = SORT_NAME;
            sort.desc = (!strcasecmp(ord, "desc") || !strcasecmp(ord, "descending"));
            continue;
        }
        if (!strncasecmp(a, "count:", 6))  { count  = (uint32_t)strtoul(a + 6, NULL, 10); continue; }
        if (!strncasecmp(a, "offset:", 7)) { offset = (uint32_t)strtoul(a + 7, NULL, 10); continue; }
        if (elen) expr[elen++] = ' ';
        size_t al = strlen(a);
        memcpy(expr + elen, a, al + 1);
        elen += al;
    }

    char err[256] = {0};
    ast_t *ast = syntax_parse(expr, err, sizeof(err));
    if (!ast && err[0]) {
        fprintf(stderr, "parse error: %s\n  in: %s\n", err, expr);
        free(expr);
        esidx_free(&db);
        return 1;
    }
    char dumped[1024];
    ast_dump(ast, dumped, sizeof(dumped));
    LOGD("query: ast = %s", dumped[0] ? dumped : "<match all>");

    uint64_t tq = ts_us();
    qset_t set;
    if (qexec(&db, ast, NULL, sort, &set) != 0) {
        fprintf(stderr, "query failed\n");
        free(expr);
        ast_free(ast);
        esidx_free(&db);
        return 1;
    }
    uint64_t t1 = ts_us();
    double qms = (double)(t1 - tq) / 1000.0;

    eid_t *page = NULL;
    uint32_t np = qset_slice(&set, offset, count, &page);

    char *pbuf = malloc(65536);
    for (uint32_t i = 0; i < np; i++) {
        path_of(&db, page[i], pbuf, 65536);
        char tstr[32] = "";
        time_t mt = (time_t)db.et.mtime[page[i]];
        struct tm tm;
        if (localtime_r(&mt, &tm)) strftime(tstr, sizeof(tstr), "%Y-%m-%d %H:%M", &tm);
        printf("%c %12lld  %s  %s\n",
               (db.et.flags[page[i]] & EF_DIR) ? 'd' : '-',
               (long long)db.et.size[page[i]], tstr, pbuf);
    }
    free(pbuf);
    free(page);

    fprintf(stderr, "loaded in %.1f ms, %u of %u results in %.1f ms (end-to-end %.1f ms)\n",
            lms, np, set.n, qms, (double)(t1 - t0) / 1000.0);
    if (log_enabled(LOG_INFO))
        LOGI("query: %u matched (dirs=%u files=%u) | plan %.3f ms, eval %.3f ms, sort %.3f ms"
             " | driver leaf #%u seeded %u of %u candidates from %u leaves",
             set.n, set.n_dir, set.n_file,
             (double)set.t_plan_us / 1000.0, (double)set.t_eval_us / 1000.0,
             (double)set.t_sort_us / 1000.0,
             set.driver, set.seed, db.et.count, set.leaf_cnt);

    qset_free(&set);
    free(expr);
    ast_free(ast);
    esidx_free(&db);
    return 0;
}

/* ------------------------------------------------------------------- serve */

/* ETP server (design §1). Loads a snapshot once and answers the client's
 * `EVERYTHING` sequence over a control connection; see etp.c for the protocol
 * notes and the decisions taken from the reference implementation. */
static int cmd_serve(int argc, char **argv)
{
    etp_opts_t o;
    memset(&o, 0, sizeof(o));
    o.bind_addr = "127.0.0.1";
    o.port = 21;
    o.allow_download = 1;

    const char *dbfile = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-p") && i + 1 < argc) { o.port = atoi(argv[++i]); continue; }
        if (!strcmp(a, "--bind") && i + 1 < argc) { o.bind_addr = argv[++i]; continue; }
        if (!strcmp(a, "-u") && i + 1 < argc) { o.username = argv[++i]; continue; }
        if (!strcmp(a, "-w") && i + 1 < argc) { o.password = argv[++i]; continue; }
        if (!strcmp(a, "--no-download")) { o.allow_download = 0; continue; }
        if (!strcmp(a, "--once")) { o.once = 1; continue; }
        if (a[0] == '-') {
            fprintf(stderr, "serve: unknown option %s\n", a);
            return 1;
        }
        if (!dbfile) { dbfile = a; continue; }
        fprintf(stderr, "serve: unexpected argument %s\n", a);
        return 1;
    }
    if (!dbfile) {
        fprintf(stderr,
                "usage: esidx serve <snapshot> [-p port] [--bind addr]\n"
                "                    [-u user [-w pass]] [--no-download] [--once]\n");
        return 1;
    }
    o.dbfile = dbfile;
    return etp_serve(&o) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    log_init(argc, argv);
    argc = log_strip_flags(argc, argv);   /* so argv[1] is always the subcommand */
    LOGD("log level: %s", log_level_name(log_level()));

    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "build")) return cmd_build(argc - 2, argv + 2);
    if (!strcmp(argv[1], "query")) return cmd_query(argc - 2, argv + 2);
    if (!strcmp(argv[1], "serve")) return cmd_serve(argc - 2, argv + 2);
    usage();
    return 1;
}
