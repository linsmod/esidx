/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
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
/* the serve singleton: open()/flock()/ftruncate() on <dbfile>.lock */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/file.h>

static void usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  esidx [-v N] build <root>... -o <dbfile> [--no-index[=LIST]]\n"
        "                             [--roots-file=PATH]\n"
        "  esidx [-v N] update <dbfile> [root]... [--deep] [--dir PATH]...\n"
        "                             [--no-index[=LIST]]\n"
        "  esidx [-v N] query <dbfile> [expr ...] [--no-index[=LIST]]\n"
        "  esidx [-v N] serve <dbfile> [-p port] [--bind addr]\n"
        "                             [-u user [-w pass]] [--no-download] [--once]\n"
"                             [--refresh=SECS] [--save=SECS] [--watch[=SOCK]]\n"
"                             [--watch-embed[=ROOT]] [--drop-to=USER[:GROUP]]\n"
"                             [--sweep=SECS] [--stall-ms=MS]\n"
"                             [--reload-config=PATH]\n"
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
        "build and update take every root they are given, in the order given: one\n"
        "        snapshot can cover several trees, `parent:\"\"` and `root:` are the union\n"
        "        of their top levels, and a root set that would index the same bytes\n"
        "        twice (the same directory twice, or one root under another) is refused.\n"
        "        --roots-file=PATH reads one absolute path per line, '#' comments,\n"
        "        which is how a packaged install names several trees without putting a\n"
        "        list in one environment variable.\n"
        "\n"
        "serve --refresh=SECS runs that same names pass inside the server, every SECS\n"
        "        seconds and once at startup, so a long-running process keeps its own\n"
        "        index current instead of serving a snapshot until it is restarted.\n"
        "        It answers at most one deep pass interval behind on attributes, which\n"
        "        is design 12 risk 8 stated for this configuration. --save=SECS writes\n"
        "        the snapshot on a timer and on a clean exit, and needs --refresh or\n"
        "        --watch.\n"
        "\n"
        "serve --stall-ms=MS sets how long a client may have a reply queued and accept\n"
        "        none of it before it is dropped (default 10000). Replies are written to a\n"
        "        non-blocking socket and what does not fit stays queued, so a client that\n"
        "        stops reading costs it its slot rather than the whole loop; MS is how long the\n"
        "        server waits before deciding it is not coming back. The queue is also capped\n"
        "        at 32 MB, so a client that never reads cannot make the server grow without\n"
        "        bound.\n"
        "\n"
        "serve --watch[=SOCK] subscribes to sfa, a separate privileged proxy that turns\n"
        "        kernel filesystem events into absolute paths over a unix socket, and\n"
        "        marks the directory each change happened in. Name changes become visible\n"
        "        within one event batch instead of one --refresh interval; size and mtime\n"
"        are unchanged, because listing a directory cannot see them. SOCK defaults\n"
         "        to /run/sfa.sock. esidx itself needs no privilege -- the proxy does, which\n"
         "        is why it is not part of this binary.\n"
         "\n"
         "serve --watch-embed[=ROOT] [--drop-to=USER[:GROUP]] is the single-process form:\n"
         "        this process opens the fanotify group itself instead of subscribing to a\n"
         "        proxy, which removes the second daemon and the socket between them. fanotify\n"
         "        needs CAP_SYS_ADMIN, so the process starts as root and gives the privilege\n"
         "        back once the group is open. Both arguments have defaults that cannot be\n"
         "        wrong, so the usual invocation is just `--watch-embed`: ROOT defaults to the\n"
         "        snapshot's own root (naming it is allowed and then compared), and --drop-to\n"
         "        defaults to the snapshot's owner -- the user it was built as, which is the\n"
         "        one that must be able to read it. A derived drop is printed, and an owner of\n"
         "        root is refused rather than becoming a privileged service by default.\n"
         "        What survives the drop is CAP_DAC_READ_SEARCH and\n"
         "        nothing else -- sfa resolves an event's directory with open_by_handle_at(),\n"
         "        which needs exactly that capability, so a process that dropped it would see\n"
         "        every event as unattributable and ask for a full pass after every batch. Use\n"
         "        this when one unit is worth more than an index process that holds no\n"
         "        capability at all; keep --watch where that separation is the point.\n"
         "\n"
        "        --sweep=SECS compares the mtime of EVERY indexed directory rather than\n"
        "        the ones a walk reaches, which is the only thing that finds a change below\n"
        "        a directory whose ancestors never moved. It costs one stat per directory\n"
        "        (94 ms for /usr's 34 811, 1.9 s for /work's 651 894, measured warm on\n"
        "        r7000), so it is a separate coarse knob: it also runs once at startup,\n"
        "        where it is what makes 'the server is up to date' true, and whenever the\n"
        "        event proxy reports that it lost events.\n"
        "\n"
        "        --reload-config=PATH watches a --roots-file format file (one absolute\n"
        "        path per line, '#' comments) and keeps the indexed locations equal to\n"
        "        what it names: at startup, and then every 5 s, the file is compared\n"
        "        with the locations the snapshot covers, and whenever the two disagree\n"
        "        the index is rebuilt in a child process and swapped in -- so a\n"
        "        location can be added or removed without restarting the server, and\n"
        "        without a build command being run by anyone. A snapshot that does not\n"
        "        exist is one such disagreement: the server starts with an empty index\n"
        "        and builds it. (A snapshot that exists and does not load is still an\n"
        "        error -- that file is not an index.) The child is this same binary\n"
        "        via /proc/self/exe, forked after the privilege drop so the index is a\n"
        "        view of what the server's own identity may read, and the serve loop\n"
        "        keeps answering while it runs. The snapshot on disk is rewritten from\n"
        "        the new index, so a restart comes back up on the new locations and\n"
        "        finds nothing to do. A build that fails is logged at error level and\n"
        "        changes nothing: the server keeps the index it has until the file\n"
        "        changes again. It is not /etc/default/esidx -- systemd reads that\n"
        "        once before the process starts, and the port and the bind address in\n"
        "        it cannot move under a running one.\n"
        "\n"
        "derived indexes (design D4: none of them are in the snapshot, so this is a\n"
        "choice about this process and the same file serves both settings):\n"
        "  --no-index=size,mtime,ctime,trigram,rank   leave those unbuilt\n"
        "  --no-index                                 the same, from <dbfile>.opts\n"
        "  --no-index=                                build everything, ignore .opts\n"
        "  <dbfile>.opts  one name per line, '#' comments; --no-index wins over it,\n"
        "                 and ESIDX_SKIP_INDEX is the last resort (tests use it)\n"
        "  esidx options <dbfile>     print what would be left out, and why\n"
        "  esidx roots <dbfile>       print the locations the snapshot covers\n"
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

/* The roots a build was asked for. `--roots-file` is how a list arrives: one absolute
 * path per line, '#' comments and blank lines ignored -- the same shape as the
 * <dbfile>.opts sidecar, and the same reason (a list has to be readable by something
 * that is not a shell). Precedence is positional > file > the one path a bare `build
 * <path>` gives, and they compose: positional paths are walked first, in the order they
 * were typed, then the file's. Duplicates are caught by esidx_scan(), which refuses a set
 * that would index the same bytes twice.
 *
 * The cap is ESIDX_MAX_ROOTS and the count is enforced here as well as there: this is
 * where a command line is turned into an argument, so it is the only place that can say
 * "you typed too many" before the scan starts. */

/* The `--roots-file` spelling, shared: both subcommands take it, and a list that means
 * one thing must be spelled one way or the packaged cron line and the packaged postinst
 * line drift apart. `*slot` is left alone when the flag is absent. */
static int roots_file_opt(int argc, char **argv, int i, const char **slot)
{
    if (!strcmp(argv[i], "--roots-file")) {
        if (i + 1 >= argc) {
            fprintf(stderr, "--roots-file needs a path\n");
            return -1;
        }
        *slot = argv[i + 1];
        return 1;
    }
    if (!strncmp(argv[i], "--roots-file=", 13)) {
        if (!argv[i][13]) {
            fprintf(stderr, "--roots-file= needs a path\n");
            return -1;
        }
        *slot = argv[i] + 13;
        return 1;
    }
    return 0;
}

static int cmd_build(int argc, char **argv)
{
    const char *out = "esidx.db", *roots_file = NULL;
    char *roots[ESIDX_MAX_ROOTS];
    uint32_t nroots = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { out = argv[++i]; continue; }
        int rf = roots_file_opt(argc, argv, i, &roots_file);
        if (rf < 0) { esidx_roots_free(roots, nroots); return 1; }
        if (rf > 0) { if (i + 1 < argc && argv[i][0] != '-') i++; continue; }
        if (argv[i][0] == '-' && argv[i][1]) {
            fprintf(stderr, "build: unknown option %s\n", argv[i]);
            esidx_roots_free(roots, nroots);
            return 1;
        }
        if (nroots >= ESIDX_MAX_ROOTS) {
            fprintf(stderr, "build: more than %u roots\n", (unsigned)ESIDX_MAX_ROOTS);
            esidx_roots_free(roots, nroots);
            return 1;
        }
        /* strdup, not the pointer: every exit path below frees the list, and half of
         * these come from argv, which is not ours to free. */
        roots[nroots] = strdup(argv[i]);
        if (!roots[nroots]) {
            fprintf(stderr, "build: out of memory\n");
            esidx_roots_free(roots, nroots);
            return 1;
        }
        nroots++;
    }
    /* Every positional is kept now. `build /a /b` used to take /a and drop /b without a
     * word, which is the one behaviour here that could not be discovered by reading the
     * output: the index looked complete and was half of what was asked for. */
    if (roots_file) {
        uint32_t before = nroots;
        if (esidx_roots_read_file(roots_file, roots, ESIDX_MAX_ROOTS, &nroots) != 0) {
            esidx_roots_free(roots, nroots);
            return 1;
        }
        fprintf(stderr, "build: %u root%s from %s, %u on the command line\n",
                nroots - before, nroots - before == 1 ? "" : "s", roots_file, before);
    }
    if (nroots == 0) { usage(); esidx_roots_free(roots, nroots); return 1; }

    esidx_t db;
    esidx_init(&db);
    /* The options belong to the file being written, so the sidecar that can turn an
     * index off is the one sitting next to it -- and `build` is the one command that
     * has to honour it, or the snapshot would be written by a process configured
     * differently from every other process that reads it. */
    idx_configure(&db, out, g_flag);

    uint64_t t0 = ts_us();
    if (esidx_scan(&db, roots, nroots) != 0) {
        fprintf(stderr, "scan failed\n");
        esidx_roots_free(roots, nroots);
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
        esidx_roots_free(roots, nroots);
        esidx_free(&db);
        return 1;
    }
    TSDONE("build: total", t0);
    fprintf(stderr, "saved -> %s  (%u root%s)\n", out, nroots, nroots == 1 ? "" : "s");
    esidx_roots_free(roots, nroots);
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
    const char *dbfile = NULL, *roots_file = NULL;
    const char *dirs[128];
    int ndirs = 0;
    unsigned flags = 0;
    char *roots[ESIDX_MAX_ROOTS];
    uint32_t nroots = 0;
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
        int rf = roots_file_opt(argc, argv, i, &roots_file);
        if (rf < 0) return 1;
        if (rf > 0) { if (i + 1 < argc && argv[i][0] != '-') i++; continue; }
        if (a[0] == '-') { fprintf(stderr, "update: unknown option %s\n", a); return 1; }
        if (!dbfile) { dbfile = a; continue; }
        /* Every one of them is kept, for the reason cmd_build() keeps them. */
        if (nroots >= ESIDX_MAX_ROOTS) {
            fprintf(stderr, "update: more than %u roots\n", (unsigned)ESIDX_MAX_ROOTS);
            esidx_roots_free(roots, nroots);
            return 1;
        }
        roots[nroots] = strdup(a);
        if (!roots[nroots]) {
            fprintf(stderr, "update: out of memory\n");
            esidx_roots_free(roots, nroots);
            return 1;
        }
        nroots++;
    }
    /* The same list, the same spelling: a packaged install names its locations in a file
     * and the line that brings the index forward has to be able to read it too. */
    if (roots_file) {
        uint32_t before = nroots;
        if (esidx_roots_read_file(roots_file, roots, ESIDX_MAX_ROOTS, &nroots) != 0) {
            esidx_roots_free(roots, nroots);
            return 1;
        }
        fprintf(stderr, "update: %u root%s from %s, %u on the command line\n",
                nroots - before, nroots - before == 1 ? "" : "s", roots_file, before);
    }
    if (!dbfile) {
        fprintf(stderr, "usage: esidx update <dbfile> [root]... [--deep] [--sweep] "
                        "[--dir PATH]... [--roots-file=PATH]\n");
        esidx_roots_free(roots, nroots);
        return 1;
    }
    if (ndirs && nroots) {
        /* Both name a set of directories and they mean different things; refusing is
         * cheaper than deciding which one wins. */
        fprintf(stderr, "update: give either roots or --dir, not both\n");
        esidx_roots_free(roots, nroots);
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
        esidx_roots_free(roots, nroots);
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
            esidx_roots_free(roots, nroots);
            return 1;
        }
        if (esidx_mark_dirty(&db, e) != 0) {
            fprintf(stderr, "update: %s is not a directory\n", dirs[i]);
            esidx_free(&db);
            esidx_roots_free(roots, nroots);
            return 1;
        }
    }

    update_stats_t st;
    int rc = ndirs ? esidx_refresh_dirs(&db, flags, &st)
                   : esidx_update(&db, nroots ? roots : NULL, nroots, flags, &st);
    if (rc != 0) {
        esidx_free(&db);
        esidx_roots_free(roots, nroots);
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
        esidx_roots_free(roots, nroots);
        return 1;
    }
    TSDONE2("update: total", t0, "(%u entries)", db.et.count);
    esidx_free(&db);
    esidx_roots_free(roots, nroots);
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
    /* NULL cancel: a batch caller is never "a query the client stopped waiting for",
     * and there is no socket to ask (D12). */
    if (qexec(&db, ast, NULL, sort, &set, NULL, NULL) != 0) {
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

/* One snapshot, one server.
 *
 * `serve` is the only command that keeps the index in memory *and* marks the tree for events,
 * so two of them on one snapshot is the configuration that must not happen: each answers from
 * its own copy and each applies its own events, so the two answers drift apart with every
 * change under the tree -- and the second one's events are invisible to the first. Nothing else
 * in the system notices: both processes are healthy, both answer, and the disagreement is
 * silent.
 *
 * flock rather than a pid file, for one reason that decides it: the kernel drops the lock when
 * the process dies, `kill -9` included, so a crash cannot leave a file behind that makes the
 * next start refuse -- which is the failure mode a pid file has, and the reason a service "will
 * not start after a reboot". Non-blocking, because the right answer to "this snapshot is
 * already served" is to say so and exit rather than to wait for a server that may run for
 * months. The pid is written into the file for the message only; it decides nothing, and the
 * lock is what is authoritative.
 *
 * The fd is deliberately kept for the life of the process: releasing the lock is the kernel's
 * job at exit, and there is no path in this program that should release it earlier. */
static int g_serve_lock_fd = -1;

static int singleton_lock(const char *dbfile)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s.lock", dbfile) >= (int)sizeof(path)) {
        fprintf(stderr, "serve: %s: too long to name a lock file for\n", dbfile);
        return -1;
    }

    /* Write access is only needed for the pid in the message, and the file may well belong to
     * another user: the packaged service starts as root and so creates it root-owned, while a
     * hand-run `serve` of the same snapshot runs as the service user. Read-only is enough to
     * hold a lock, so a refused write-open falls back rather than refusing a legal start. */
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        /* The errno that matters is this one, not the fallback's. open(O_RDONLY) without
         * O_CREAT reports ENOENT for a file that does not exist however the creation failed,
         * and the difference is the whole diagnosis: "no such file" reads like the snapshot
         * moved, while the real cause -- EACCES on a directory this identity may not write,
         * read-only file system -- says what to change. Measured the hard way: the packaged
         * service reported ENOENT for a lock file it was refused permission to create. */
        int werr = errno;
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            fprintf(stderr, "serve: cannot open the lock file %s: %s\n", path, strerror(werr));
            fprintf(stderr, "serve: holding it needs a directory this identity may create the "
                            "file in, or an existing lock file it may read (a lock needs no "
                            "write access)\n");
            return -1;
        }
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        char buf[32];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            fprintf(stderr, "serve: %s is already being served by pid %s; refusing to start "
                            "a second one\n", dbfile, buf);
        } else {
            fprintf(stderr, "serve: %s is already being served (%s); refusing to start a "
                            "second one\n", dbfile, path);
        }
        close(fd);
        return -1;
    }

    char pid[32];
    int n = snprintf(pid, sizeof(pid), "%ld\n", (long)getpid());
    if (n > 0) {
        (void)!ftruncate(fd, 0);                    /* fails on the read-only fallback, fine */
        (void)!pwrite(fd, pid, (size_t)n, 0);
    }
    return fd;
}

/* ETP server (design §1). Loads a snapshot once and answers the client's
 * `EVERYTHING` sequence over a control connection; see etp.c for the protocol
 * notes and the decisions taken from the reference implementation. */
static int cmd_serve(int argc, char **argv)
{
    etp_opts_t o;
    memset(&o, 0, sizeof(o));
    o.bind_addr = "127.0.0.1";
    /* 2121, not ETP's conventional 21. The listener is opened *after* the watcher has given
     * the privilege back (see etp.c), so a privileged port could only ever fail here --
     * and `serve` without a port would then look like a broken build rather than a default
     * that cannot work. The banner prints the port either way. */
    o.port = 2121;
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
        /* Embedded mode is a separate flag rather than another --watch value, so that the
         * two forms cannot be confused for one another at the call site: --watch=SOMETHING
         * is always a socket to connect to, and only --watch-embed means "this process
         * holds the fanotify group". */
        if (!strcmp(a, "--watch-embed") || !strncmp(a, "--watch-embed=", 14)) {
            /* Bare form is the normal spelling, and the empty string is the sentinel for it
             * (same split as --watch above): the tree to mark is the snapshot's own root, so
             * naming it again is a second chance to disagree with the file, not information.
             * --watch-embed=ROOT stays accepted and is compared, for the caller who wants the
             * check to be performed against what they think they deployed. */
            o.watch_embed = a[13] == '=' ? a + 14 : "";
            continue;
        }
        if (!strncmp(a, "--reload-config=", 16)) {
            if (!a[16]) {
                fprintf(stderr, "serve: --reload-config= needs a path, e.g. "
                                "--reload-config=/etc/esidx/roots\n");
                return 1;
            }
            o.reload_config = a + 16;
            continue;
        }
        if (!strncmp(a, "--drop-to=", 10)) {
            o.drop_to = a + 10;
            continue;
        }
        if (!strcmp(a, "--drop-to")) {
            fprintf(stderr, "serve: --drop-to needs a user, e.g. --drop-to=esidx\n");
            return 1;
        }
        if (!strncmp(a, "--watch-group=", 14)) {
            o.watch_group = a + 14;
            continue;
        }
        if (!strncmp(a, "--sweep=", 8)) {
            int v = secs_arg(a, "--sweep");
            if (v < 0) return 1;
            o.sweep_secs = v;
            continue;
        }
        if (!strncmp(a, "--stall-ms=", 11)) {
            /* Not secs_arg(): that one parses seconds and says so, in both the accepted range
             * and the error -- "needs a number of seconds (0..86400)" under a flag whose unit
             * is milliseconds, with a day for a ceiling. A patience knob is not worth a
             * confidently wrong message. */
            const char *val = a + 11;
            char *end = NULL;
            long v = strtol(val, &end, 10);
            /* The 100 ms floor is not tidiness: this number is also how long a wedged client
             * stays in the table before the loop notices, so a value below a poll turn or two
             * would drop clients whose queue simply had not been reached yet. */
            if (!*val || (end && *end) || v < 100 || v > 3600000) {
                fprintf(stderr, "serve: --stall-ms needs 100..3600000 milliseconds (0.1 s to\n"
                                "       1 h), not '%s'\n", val);
                return 1;
            }
            o.stall_ms = (uint64_t)v;
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
                "                    [--sweep=SECS] [--stall-ms=MS]\n");
        return 1;
    }
    /* Checked here rather than in watch.c, because both of these are spelling mistakes a
     * reader makes long before a service is deployed: --drop-to with the two-process form
     * says nothing, and --watch-embed without --drop-to would start a root process and
     * discover the omission at the point where the only fix is to stop. */
    if (o.drop_to && !o.watch_embed) {
        fprintf(stderr, "serve: --drop-to only means something with --watch-embed; without it "
                        "this process never gains privilege to give back\n");
        return 1;
    }
    /* --drop-to is not required: etp.c defaults it to the snapshot's owner, which is the
     * user the snapshot was built as and is refused when that owner is root. Demanding the
     * flag here would make every deployment type a name that is already in the file's
     * ownership, and the point of the guard is the refusal, not the typing. */
    if (o.watch_embed && o.watch_sock) {
        fprintf(stderr, "serve: --watch-embed and --watch=SOCK are two different sources of "
                        "events; pass one or the other\n");
        return 1;
    }
    if (o.save_secs > 0 && o.refresh_secs == 0 && !o.watch_sock && !o.watch_embed && !o.sweep_secs) {
        /* Refused rather than ignored: with nothing to reconcile, the epoch never moves
         * and every tick would find the snapshot already current. */
        fprintf(stderr, "serve: --save=%d needs --refresh, --watch or --sweep; there is "
                        "nothing to save\n", o.save_secs);
        return 1;
    }
    o.dbfile = dbfile;
    /* One snapshot, one server -- and this is also what lets `build`/`update` be run next to a
     * live server without either of them finding out the hard way: the snapshot they replace is
     * not the one being served. Taken before the snapshot is read, so a second start costs
     * nothing. */
    g_serve_lock_fd = singleton_lock(dbfile);
    if (g_serve_lock_fd < 0) return 1;
    /* serve keeps its own esidx_t inside etp_serve(), so the flag travels as two plain
     * fields rather than being re-read from the environment over there: one place
     * decides what this process does not build. */
    o.have_no_index = g_flag.have;
    o.no_index = g_flag.list;
    return etp_serve(&o) == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------- roots */

/* Print the locations a snapshot covers, one per line, in index order.
 *
 * The question this answers is "what is in this index?", and it is asked by whoever changed
 * /etc/esidx/roots and has not rebuilt yet -- the answer they need is the *snapshot's* list,
 * not the file's, and those are two different lists the moment either of them changes. It
 * also has to load the snapshot to answer, because the roots are derived from the rows
 * (D9) and nothing stores them; `esidx options` avoids the load because it can, and this
 * one cannot.
 *
 * Exits 1 on an empty index rather than printing nothing: "this snapshot covers no
 * locations" is a fact worth a non-zero exit, because the reason a server would refuse to
 * start is that this file is absent. */
static int cmd_roots(int argc, char **argv)
{
    const char *dbfile = (argc > 0 && argv[0][0] != '-') ? argv[0] : NULL;
    if (!dbfile || (argc > 1 && argv[1][0] != '-')) {
        fprintf(stderr, "usage: esidx roots <dbfile>\n");
        return 1;
    }
    esidx_t db;
    esidx_init(&db);
    idx_configure(&db, dbfile, g_flag);
    if (esidx_load(&db, dbfile) != 0) {
        fprintf(stderr, "load failed: %s\n", dbfile);
        esidx_free(&db);
        return 1;
    }
    if (db.nroots == 0) {
        fprintf(stderr, "esidx: %s has no indexed location\n", dbfile);
        esidx_free(&db);
        return 1;
    }
    char path[PATH_MAX];
    for (uint32_t i = 0; i < db.nroots; i++) {
        path_of(&db, esidx_root(&db, i), path, sizeof(path));
        printf("%s\n", path);
    }
    esidx_free(&db);
    return 0;
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
    if (!strcmp(argv[1], "roots"))   return cmd_roots(argc - 2, argv + 2);
    usage();
    return 1;
}
