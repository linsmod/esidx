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
#include <unistd.h>

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  esidx [-v N] build <root> -o <dbfile> [--no-index[=LIST]]\n"
        "  esidx [-v N] update <dbfile> [root] [--deep] [--dir PATH]...\n"
        "                             [--no-index[=LIST]]\n"
        "  esidx [-v N] query <dbfile> [expr ...] [--no-index[=LIST]]\n"
        "  esidx [-v N] serve <dbfile> [-p port] [--bind addr]\n"
        "                             [-u user [-w pass]] [--no-download] [--once]\n"
        "                             [--refresh=SECS] [--save=SECS] [--watch[=SOCK]]\n"
        "                             [--sweep=SECS]\n"
        "                             [--no-index[=LIST]]\n"
        "  esidx options <dbfile> [--no-index[=LIST]]\n"
        "\n"
        "update: bring an index back in line with the filesystem. Without --deep\n"
        "        it stats one directory per subtree and notices name changes;\n"
        "        with --deep it stats every entry and notices size/mtime changes.\n"
        "        <root> defaults to the one the index was built from, and --dir\n"
        "        PATH refreshes only that directory (repeatable) -- the same\n"
        "        reconcile with a smaller set of directories to list.\n"
        "\n"
        "serve --refresh=SECS runs that same names pass inside the server, every SECS\n"
        "        seconds and once at startup, so a long-running process keeps its own\n"
        "        index current instead of serving a snapshot until it is restarted.\n"
        "        It answers at most one deep pass interval behind on attributes, which\n"
        "        is design 12 risk 8 stated for this configuration. --save=SECS writes\n"
        "        the snapshot on a timer and on a clean exit, and needs --refresh or\n"
        "        --watch.\n"
        "\n"
        "serve --watch[=SOCK] subscribes to sfa, a separate privileged proxy that turns\n"
        "        kernel filesystem events into absolute paths over a unix socket, and\n"
        "        marks the directory each change happened in. Name changes become visible\n"
        "        within one event batch instead of one --refresh interval; size and mtime\n"
        "        are unchanged, because listing a directory cannot see them. SOCK defaults\n"
        "        to /run/sfa.sock. esidx itself needs no privilege -- the proxy does, which\n"
        "        is why it is not part of this binary.\n"
        "\n"
        "        --sweep=SECS compares the mtime of EVERY indexed directory rather than\n"
        "        the ones a walk reaches, which is the only thing that finds a change below\n"
        "        a directory whose ancestors never moved. It costs one stat per directory\n"
        "        (94 ms for /usr's 34 811, 1.9 s for /work's 651 894, measured warm on\n"
        "        r7000), so it is a separate coarse knob: it also runs once at startup,\n"
        "        where it is what makes 'the server is up to date' true, and whenever the\n"
        "        event proxy reports that it lost events.\n"
        "\n"
        "derived indexes (design D4: none of them are in the snapshot, so this is a\n"
        "choice about this process and the same file serves both settings):\n"
        "  --no-index=size,mtime,ctime,trigram,rank   leave those unbuilt\n"
        "  --no-index                                 the same, from <dbfile>.opts\n"
        "  --no-index=                                build everything, ignore .opts\n"
        "  <dbfile>.opts  one name per line, '#' comments; --no-index wins over it,\n"
        "                 and ESIDX_SKIP_INDEX is the last resort (tests use it)\n"
        "  esidx options <dbfile>     print what would be left out, and why\n"
        "\n"
        "logging: ESIDX_LOG=error|warn|info|debug  or  -v / -v N / --verbose=N\n");
}

/* ------------------------------------------------------ the --no-index flag
 *
 * Removed from argv rather than read in place, for the reason log_strip_flags() removes
 * `-v`: a flag that the query loop can also see becomes part of the search string, and
 * `--no-index=size size:>1k` then answers zero rows -- the flag worked perfectly and its
 * own text was ANDed in as a term matching no filename, which is a much worse thing to
 * debug than "unknown option". Stripping it here means no other parser has to know the
 * name, and an argument the query loop does not recognise still becomes search text
 * exactly as before.
 *
 * `--no-index` with no value means "read the sidecar" and `--no-index=` means "ignore the
 * sidecar", which is why `have` is separate from the list: an empty list is a real
 * setting, not an absent argument.
 *
 * One flag for the whole command line, not one per subcommand: `esidx -v 3 --no-index=x
 * serve ...` and `esidx serve ... --no-index=x` have to mean the same thing, and an
 * argument order nobody has to remember is worth more than per-command parsing. */
typedef struct { int have; const char *list; } idx_flag_t;
static idx_flag_t g_flag;   /* stripped from argv in main(), before any subcommand */

static idx_flag_t idx_strip(int *argc, char **argv)
{
    idx_flag_t f = { 0, NULL };
    int w = 0;
    for (int i = 0; i < *argc; i++) {
        if (!strncmp(argv[i], "--no-index=", 11)) {
            f.have = 1; f.list = argv[i] + 11; continue;
        }
        if (!strcmp(argv[i], "--no-index")) {
            f.have = 1; f.list = ""; continue;
        }
        argv[w++] = argv[i];
    }
    for (int i = w; i < *argc; i++) argv[i] = NULL;
    *argc = w;
    return f;
}

/* Resolve and record, for a command that has just initialised `db` and knows its file. */
static void idx_configure(esidx_t *db, const char *dbfile, idx_flag_t f)
{
    const char *src = "default";
    uint32_t mask = esidx_index_resolve(dbfile, f.have, f.list, &src);
    esidx_index_apply(db, mask, src);
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
    /* The options belong to the file being written, so the sidecar that can turn an
     * index off is the one sitting next to it -- and `build` is the one command that
     * has to honour it, or the snapshot would be written by a process configured
     * differently from every other process that reads it. */
    idx_configure(&db, out, g_flag);

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

/* ------------------------------------------------------------------- update */

/* The offline half of the incremental path (design §7). `esidx serve --refresh=SECS`
 * runs the same walk in-process, between two poll() turns; this exists so an index
 * built by cron and served read-only can still be brought forward, and so the suites
 * can drive a refresh between two queries without a socket in the way. The two differ
 * in exactly one respect, and deliberately: a pass here compacts (it is about to exit,
 * and a small snapshot is what it leaves behind) where the server refuses to
 * (EU_NOCOMPACT, etp.c) because it cannot stop answering clients for a rescan.
 *
 * `--dir PATH` refreshes only that directory (repeatable), which is the dirty set of
 * design §7 spelled on a command line: the caller knows what changed, and the reconcile
 * is the same one a full pass runs -- only the set of directories to list is smaller. */
static int cmd_update(int argc, char **argv)
{
    const char *dbfile = NULL, *root = NULL;
    const char *dirs[128];
    int ndirs = 0;
    unsigned flags = 0;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--deep")) { flags |= EU_DEEP; continue; }
        if (!strcmp(a, "--sweep")) { flags |= EU_SWEEP; continue; }
        if (!strcmp(a, "--dir") && i + 1 < argc) {
            if (ndirs == (int)(sizeof(dirs) / sizeof(dirs[0]))) {
                fprintf(stderr, "update: too many --dir arguments (max %d)\n", ndirs);
                return 1;
            }
            dirs[ndirs++] = argv[++i];
            continue;
        }
        if (a[0] == '-') { fprintf(stderr, "update: unknown option %s\n", a); return 1; }
        if (!dbfile) { dbfile = a; continue; }
        if (!root)   { root = a; continue; }
        fprintf(stderr, "update: unexpected argument %s\n", a);
        return 1;
    }
    if (!dbfile) {
        fprintf(stderr, "usage: esidx update <dbfile> [root] [--deep] [--sweep] "
                        "[--dir PATH]...\n");
        return 1;
    }
    if (ndirs && root) {
        /* Both name a set of directories and they mean different things; refusing is
         * cheaper than deciding which one wins. */
        fprintf(stderr, "update: give either a root or --dir, not both\n");
        return 1;
    }

    esidx_t db;
    esidx_init(&db);
    /* Before the load, because the load is where finalize runs and therefore where the
     * decision has to have been made. */
    idx_configure(&db, dbfile, g_flag);
    uint64_t t0 = ts_us();
    if (esidx_load(&db, dbfile) != 0) {
        fprintf(stderr, "load failed: %s\n", dbfile);
        esidx_free(&db);
        return 1;
    }
    uint32_t before = esidx_live_count(&db);

    /* The dirty set, resolved against the index rather than trusted: a path that is not
     * one of its directories is refused by name, because a reconcile against the wrong
     * directory would delete every row it did not find -- the same reason the root
     * argument is checked rather than believed. */
    for (int i = 0; i < ndirs; i++) {
        eid_t e = di_lookup(&db, dirs[i]);
        if (e == EID_NONE) {
            fprintf(stderr, "update: %s is not a directory in this index\n", dirs[i]);
            esidx_free(&db);
            return 1;
        }
        if (esidx_mark_dirty(&db, e) != 0) {
            fprintf(stderr, "update: %s is not a directory\n", dirs[i]);
            esidx_free(&db);
            return 1;
        }
    }

    update_stats_t st;
    int rc = ndirs ? esidx_refresh_dirs(&db, flags, &st)
                   : esidx_update(&db, root, flags, &st);
    if (rc != 0) {
        esidx_free(&db);
        return 1;
    }

    uint32_t after = esidx_live_count(&db);
    fprintf(stderr,
            "updated %s: %u live entries (was %u, %+d), %u dirs (%u skipped, %u descended), "
            "%u added, %u removed, %u refreshed in %.1f ms\n",
            dbfile, after, before, (int)after - (int)before,
            st.dirs_skipped + st.dirs_reconciled,
            st.dirs_skipped, st.dirs_reconciled,
            st.added, st.removed, st.refreshed, (double)st.us / 1000.0);
    esidx_log_stats(&db, "update");

    if (esidx_save(&db, dbfile) != 0) {
        fprintf(stderr, "save failed: %s\n", dbfile);
        esidx_free(&db);
        return 1;
    }
    TSDONE2("update: total", t0, "(%u entries)", db.et.count);
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
    idx_configure(&db, dbfile, g_flag);

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
            /* Route through the one name table rather than a second switch. The CLI
             * used to spell the keys out here, and the two lists drifted: `attributes`
             * and `inverse_size` were missing, so `sort:attributes:asc` fell through
             * to `else sort.key = SORT_NAME` and answered with a *name* sort, silently,
             * with nothing in the output to say so. The ETP path has always gone
             * through sort_from_etp_name() and was right.
             *
             * An unknown key is now an error rather than a name sort: a query that
             * does not do what it says is worse than one that refuses (design §5.3). */
            char key[32] = {0}, ord[32] = {0};
            sscanf(a + 5, "%31[^:]:%31s", key, ord);
            const char *dir = (!strcasecmp(ord, "desc") ||
                               !strcasecmp(ord, "descending")) ? "_descending"
                                                              : "_ascending";
            char name[80];
            uint16_t k16 = 0;
            int asc = 1;
            snprintf(name, sizeof(name), "%s%s", key, dir);
            if (!strcasecmp(key, "ext"))          /* the CLI's spelling */
                snprintf(name, sizeof(name), "extension%s", dir);
            if (sort_from_etp_name(name, &k16, &asc) != 0) {
                fprintf(stderr, "unknown sort key '%s'\n", key);
                free(expr);
                esidx_free(&db);
                return 1;
            }
            sort.key = (sort_key_t)(k16 & SORT_KEY_MASK);
            sort.desc = !asc;
            continue;
        }
        if (!strncasecmp(a, "count:", 6))  { count  = (uint32_t)strtoul(a + 6, NULL, 10); continue; }
        if (!strncasecmp(a, "offset:", 7)) { offset = (uint32_t)strtoul(a + 7, NULL, 10); continue; }
        /* A `--` argument is this CLI's option syntax, never search text. Everything
         * else on this loop is a term, including something that merely looks like a
         * flag: the query language has no use for one, and swallowing a misspelled
         * option into the search string is how `--no-index=size size:>1k` came to
         * answer zero rows while the index really was configured correctly -- the
         * flag worked, and its own text was then ANDed into the query as a term that
         * matches no filename. Only `--` is claimed, not `-`, so a term may still
         * begin with a single dash. */
        if (a[0] == '-' && a[1] == '-') {
            fprintf(stderr, "query: unknown option %s\n", a);
            free(expr);
            esidx_free(&db);
            return 1;
        }
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

/* Parse a `--flag=N` interval. Zero is a real answer (off), and a value that is not a
 * number at all is an error rather than 0: `--refresh=soon` would otherwise be a server
 * that silently never reconciles, which is indistinguishable from a server with nothing
 * to do. */
static int secs_arg(const char *a, const char *what)
{
    const char *v = strchr(a, '=') + 1;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *end || n < 0 || n > 86400) {
        fprintf(stderr, "serve: %s needs a number of seconds (0..86400), not '%s'\n",
                what, v);
        return -1;
    }
    return (int)n;
}

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
        if (!strncmp(a, "--refresh=", 10)) {
            int v = secs_arg(a, "--refresh");
            if (v < 0) return 1;
            o.refresh_secs = v;
            continue;
        }
        if (!strncmp(a, "--save=", 7)) {
            int v = secs_arg(a, "--save");
            if (v < 0) return 1;
            o.save_secs = v;
            continue;
        }
        /* Bare --watch takes the default socket; --watch=PATH names one. The split from
         * the other flags is that this one has a usable default, so requiring a value
         * would be a worse spelling. */
        if (!strcmp(a, "--watch") || !strncmp(a, "--watch=", 8)) {
            /* Empty string rather than NULL for the bare form: NULL has to stay "no
             * watcher", since that is how a server without --watch is spelled. The empty
             * string then means "the default socket", which is what watch.c resolves. */
            o.watch_sock = a[7] == '=' ? a + 8 : "";
            continue;
        }
        if (!strncmp(a, "--sweep=", 8)) {
            int v = secs_arg(a, "--sweep");
            if (v < 0) return 1;
            o.sweep_secs = v;
            continue;
        }
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
                "                    [-u user [-w pass]] [--no-download] [--once]\n"
                "                    [--refresh=SECS] [--save=SECS] [--watch[=SOCK]]\n"
                "                    [--sweep=SECS]\n");
        return 1;
    }
    if (o.save_secs > 0 && o.refresh_secs == 0 && !o.watch_sock && !o.sweep_secs) {
        /* Refused rather than ignored: with nothing to reconcile, the epoch never moves
         * and every tick would find the snapshot already current. */
        fprintf(stderr, "serve: --save=%d needs --refresh, --watch or --sweep; there is "
                        "nothing to save\n", o.save_secs);
        return 1;
    }
    o.dbfile = dbfile;
    /* serve keeps its own esidx_t inside etp_serve(), so the flag travels as two plain
     * fields rather than being re-read from the environment over there: one place
     * decides what this process does not build. */
    o.have_no_index = g_flag.have;
    o.no_index = g_flag.list;
    return etp_serve(&o) == 0 ? 0 : 1;
}

/* ----------------------------------------------------------------- options */

/* Print what this invocation would leave unbuilt, without loading anything. The point is
 * that the answer is cheap: a 300 MB snapshot must not have to be read to be asked a
 * question about configuration, and a command that did would be one nobody runs before
 * changing a setting. */
static int cmd_options(int argc, char **argv)
{
    const char *dbfile = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        if (!dbfile) { dbfile = argv[i]; continue; }
        fprintf(stderr, "options: unexpected argument %s\n", argv[i]);
        return 1;
    }
    if (!dbfile) {
        fprintf(stderr, "usage: esidx options <dbfile> [--no-index[=LIST]]\n");
        return 1;
    }

    static const char *all[] = { "size", "mtime", "ctime", "trigram", "rank" };
    static const uint32_t bits[] = { ESIDX_IX_SIZE, ESIDX_IX_MTIME, ESIDX_IX_CTIME,
                                     ESIDX_IX_TRIGRAM, ESIDX_IX_RANK };

    const char *src = "default";
    uint32_t mask = esidx_index_resolve(dbfile, g_flag.have, g_flag.list, &src);

    char side[4096];
    esidx_index_sidecar_path(dbfile, side, sizeof(side));
    printf("dbfile:  %s\n", dbfile);
    printf("sidecar: %s (%s)\n", side, access(side, R_OK) == 0 ? "present" : "absent");
    printf("source:  %s\n", src);
    printf("skipped: %s\n", mask ? esidx_index_names(mask) : "none");
    printf("built:  ");
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++)
        if (!(mask & bits[i])) printf(" %s", all[i]);
    printf("\n");
    return 0;
}

int main(int argc, char **argv)
{
    log_init(argc, argv);
    argc = log_strip_flags(argc, argv);   /* so argv[1] is always the subcommand */
    g_flag = idx_strip(&argc, argv);      /* and the flag is out of every subcommand's way */
    LOGD("log level: %s", log_level_name(log_level()));

    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "build"))  return cmd_build(argc - 2, argv + 2);
    if (!strcmp(argv[1], "update")) return cmd_update(argc - 2, argv + 2);
    if (!strcmp(argv[1], "query"))  return cmd_query(argc - 2, argv + 2);
    if (!strcmp(argv[1], "serve"))  return cmd_serve(argc - 2, argv + 2);
    if (!strcmp(argv[1], "options")) return cmd_options(argc - 2, argv + 2);
    usage();
    return 1;
}
