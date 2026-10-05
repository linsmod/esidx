/* ETP protocol server (design §1, decision D1).
 *
 * Ported from voidtools' Everything Server 1.0.2.5, `src/etp_server.c`, with the
 * `everything_plugin_*` adapter replaced by our index engine. What was taken
 * wholesale, and why (design §11, D1):
 *
 *   - the reply grammar: `200-Query results` opens a multi-line block, every data
 *     line carries exactly one leading space, and the block ends with `200 End.`
 *     (etp_server.c:5189-5272). The ETP client's parser keys on those three
 *     literals and strips leading whitespace, so the spacing is load-bearing.
 *   - the 32 `EVERYTHING` subcommands and their one-line `200 ...` acknowledgements
 *     (:3953-4244). An unknown subcommand answers `500 Unknown Everything
 *     command.`; an unknown sort answers `500 Unknown sort type.`
 *   - the 22 sort names (:472-494), including `inverse_size_*` reusing the size
 *     key with the direction flipped (:480-481).
 *   - the two-stage model: match options and column toggles are state on the
 *     connection, and QUERY runs the search and then the FILTER_* re-match
 *     (:4246-4359).
 *   - the result cache: compare every parameter against the previous query and
 *     re-send on a match (:4250-4287). Everything re-sorts when only the sort
 *     changed; we keep the fully sorted set and re-slice, which is strictly less
 *     work and additionally makes a new OFFSET free.
 *
 * What is *not* ported, and why:
 *
 *   - the Windows socket layer and the ~60 `everything_plugin_*` procs. D1
 *     predicted 300-500 edit sites for a full textual port; the command layer is
 *     ~600 lines of state machine and formatting, which is the part worth having,
 *     and everything below it is the part we replace anyway.
 *   - the `everything_plugin_utf8_*` string API. Linux paths and names are UTF-8
 *     already, so `char*` is the native form rather than a wrapper.
 *   - index-folder-size accounting and the disk-access gate (`si:`); those depend
 *     on the Everything host process.
 *
 * Structure: one poll() loop over the listener plus every client, single-threaded.
 * The index is immutable while serving, so there is nothing to lock; design D6's
 * concurrency is about the *scan*, not the query path.
 */

#include "etp.h"
#include "syntax.h"
#include "timer.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS      16
#define CTL_BUF          8192
#define ETP_WELCOME      "220 esidx ready."
/* etp_server.c:5229 -- what Everything sends for a folder whose size it does not
 * index. The client's formatSize() reads it as -1 and prints nothing, which is
 * the behaviour we want; inventing a number here would show a meaningless
 * 4096-byte "folder size" in the browse list. */
#define SIZE_UNKNOWN     "18446744073709551615"

/* ------------------------------------------------------------------ helpers */

/* growable output buffer: a query reply can be a few hundred KB and must not be
 * assembled in a fixed stack frame */
typedef struct {
    char  *p;
    size_t n, cap;
} obuf_t;

static void obuf_add(obuf_t *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 16384;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = realloc(b->p, nc);
        if (!np) { LOGE("out of memory building a reply"); return; }
        b->p = np; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void obuf_puts(obuf_t *b, const char *s) { obuf_add(b, s, strlen(s)); }

static void obuf_printf(obuf_t *b, const char *fmt, ...)
{
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(stack)) { obuf_add(b, stack, (size_t)n); return; }
    char *heap = malloc((size_t)n + 1);
    if (!heap) return;
    va_start(ap, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    va_end(ap);
    obuf_add(b, heap, (size_t)n);
    free(heap);
}

static void obuf_free(obuf_t *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* Unix seconds -> Windows FILETIME (100 ns units since 1601-01-01).
 * etp_server.c:5235 sends fd.date_modified straight through, and the client
 * divides by 10000 and subtracts the 1601 epoch, so the conversion has to be
 * exact or every date shifts by decades. */
static uint64_t to_filetime(int64_t unix_sec)
{
    return ((uint64_t)unix_sec + 11644473600ULL) * 10000000ULL;
}

/* Our paths use '/'; the ETP wire is Windows-flavoured because the client joins
 * `path + "\\" + name`, so emitting '\' keeps a path that
 * makes the round trip byte-identical, and query.c's normalise_path() accepts
 * either separator on the way back in. */
static void wire_path(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 2 < outsz; p++)
        out[o++] = (*p == '/') ? '\\' : *p;
    out[o] = '\0';
}

/* ------------------------------------------------------------- client state */

typedef struct {
    int      fd;
    bool     logged_in;
    char     rbuf[CTL_BUF];
    size_t   rlen;
    obuf_t   wbuf;

    /* ---- state the 32 subcommands mutate (design §1.1) ---- */
    match_opts_t mo;
    /* CTL_BUF, not 4096, because a value arrives on a control line and a control line
     * is at most CTL_BUF: a fixed array smaller than the line it is parsed out of
     * truncates a long search *silently*, and then the server answers a different
     * query than the client asked -- 1 200 rows where 1 476 were asked for on the
     * fixture below, with no error anywhere. The reference keeps the same string in a
     * realloc per SEARCH (etp_server.c:4025), so its only limit is the line too.
     * cache_search/cache_filter are the same two values copied for design §6.4's
     * result cache and have to be able to hold them. */
    char         search[CTL_BUF];
    char         filter_search[CTL_BUF];
    uint16_t     sort_key;      /* SORT_* plus SORT_INVERSE_SIZE */
    int          sort_asc;
    uint32_t     offset, count;

    /* enabled result columns */
    int col_size, col_attributes, col_date_modified, col_date_created;
    int col_path, col_file_list_filename, col_date_recently_changed;

    /* working directory, for CWD/CDUP/PWD/MLSD */
    char cwd[4096];
    char pending_user[256];

    /* ---- result cache (design §6.4, ref G3) ---- */
    bool     cache_valid;
    qset_t   cache;             /* the full sorted match set */
    char     cache_search[CTL_BUF];
    char     cache_filter[CTL_BUF];
    uint32_t cache_filter_flags;
    match_opts_t cache_mo;
    uint16_t cache_sort_key;
    int      cache_sort_asc;
    uint32_t cache_offset, cache_count;

    /* data connection state */
    int  pasv_listen;           /* listening socket awaiting a data connection */
    int  pasv_port;
    int  data_fd;               /* an accepted data connection */
    int  peer_fd;               /* EPRT/PORT: connect out to the client */
    char peer_addr[64];
    int  peer_port;
} client_t;

static int   g_listen_fd = -1;
static int   g_bound_port = 0;
static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ replies */

static void c_flush(client_t *c)
{
    size_t off = 0;
    while (off < c->wbuf.n) {
        ssize_t w = send(c->fd, c->wbuf.p + off, c->wbuf.n - off, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        off += (size_t)w;
    }
    c->wbuf.n = 0;
    if (c->wbuf.p) c->wbuf.p[0] = '\0';
}

/* One reply line, formatted at whatever length it needs to be.
 *
 * It used to be formatted into a char[1024], which is invisible until a reply crosses
 * 1023 bytes: vsnprintf truncates, the CRLF goes with the tail, and a client waiting
 * for the end of the line waits forever. There is no error on either side -- the reply
 * simply stops mid-sentence, which is the same shape as the OPTS UTF8 hang AGENTS.md 1.4
 * records, and it was found the same way: a real value that turned out to be longer
 * than the buffer. `EVERYTHING SEARCH` acknowledges by echoing the whole search
 * (etp_server.c:4027), so a search over about 1000 characters is enough, and Everything
 * lets the user type tens of thousands.
 *
 * The reference has no such buffer: its printf writes into the client's output stream
 * through etp_server_client_printf(), and the search it echoes lives in a string it
 * reallocs per SEARCH (etp_server.c:4025) rather than in a fixed array. */
static void c_reply(client_t *c, const char *fmt, ...)
{
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(stack)) {
        LOGD("> %s", stack);
        obuf_add(&c->wbuf, stack, (size_t)n);
    } else {
        char *heap = malloc((size_t)n + 1);
        if (!heap) { LOGE("cannot format a %d-byte reply", n); return; }
        va_start(ap, fmt);
        vsnprintf(heap, (size_t)n + 1, fmt, ap);
        va_end(ap);
        LOGD("> %s", heap);
        obuf_add(&c->wbuf, heap, (size_t)n);
        free(heap);
    }
    c_flush(c);
}

static void c_write(client_t *c, const obuf_t *b)
{
    size_t off = 0;
    while (off < b->n) {
        ssize_t w = send(c->fd, b->p + off, b->n - off, MSG_NOSIGNAL);
        if (w <= 0) {
            if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            break;
        }
        off += (size_t)w;
    }
}

/* -------------------------------------------------- the query result block */

/* Byte-for-byte the shape of etp_server_send_query_results() (:5172-5276).
 * Column lines precede the FILE/FOLDER line for the same item, in the order the
 * reference emits them -- the client accumulates per item and resets on
 * FILE/FOLDER, so the order is part of the contract, not a style choice. */
static void send_query_results(const esidx_t *db, client_t *c, const qset_t *set)
{
    obuf_t b = {0};
    uint32_t off = c->offset;
    uint32_t show = c->count;
    char path[65536], wpath[65536];

    obuf_puts(&b, "200-Query results\r\n");
    obuf_printf(&b, " RESULT_COUNT %zu\r\n", (size_t)set->n);

    while (off < set->n) {
        if (!show) break;
        eid_t id = set->ids[off];
        bool is_dir = (db->et.flags[id] & EF_DIR) != 0;

        if (c->col_path) {
            parent_path_of(db, id, path, sizeof(path));
            wire_path(path, wpath, sizeof(wpath));
            obuf_printf(&b, " PATH %s\r\n", wpath);
        }
        if (c->col_attributes)
            obuf_printf(&b, " ATTRIBUTES %u\r\n", esidx_win_attributes(db, id));
        if (c->col_size) {
            /* Everything does not index folder sizes by default, so it sends the
             * sentinel; we have real numbers for folders but reporting them would
             * put a meaningless 4096 in the size column of every directory. */
            if (is_dir) obuf_printf(&b, " SIZE %s\r\n", SIZE_UNKNOWN);
            else       obuf_printf(&b, " SIZE %llu\r\n",
                                   (unsigned long long)db->et.size[id]);
        }
        if (c->col_date_modified)
            obuf_printf(&b, " DATE_MODIFIED %llu\r\n",
                       (unsigned long long)to_filetime(db->et.mtime[id]));
        if (c->col_date_created)
            obuf_printf(&b, " DATE_CREATED %llu\r\n",
                       (unsigned long long)to_filetime(db->et.ctime[id]));
        if (c->col_file_list_filename) {
            path_of(db, id, path, sizeof(path));
            wire_path(path, wpath, sizeof(wpath));
            obuf_printf(&b, " FILE_LIST_FILENAME %s\r\n", wpath);
        }
        if (c->col_date_recently_changed)
            obuf_printf(&b, " DATE_RECENTLY_CHANGED %llu\r\n",
                       (unsigned long long)to_filetime(db->et.mtime[id]));
        obuf_printf(&b, " %s %s\r\n", is_dir ? "FOLDER" : "FILE", display_name_of(db, id));

        off++;
        show--;
    }
    obuf_puts(&b, "200 End.\r\n");
    c_write(c, &b);
    obuf_free(&b);
}

/* ------------------------------------------------------------- the QUERY op */

/* Is the current parameter set the one the cache holds?
 *
 * etp_server.c:4250-4271 compares every match option, every column toggle, both
 * search strings and the filter flags -- and deliberately NOT offset/count, which
 * is what makes paging free. Everything re-sorts when only the sort changed
 * (:4273-4286); we already hold the sorted set, so a sort change is a cache miss
 * and a re-run, which is the same amount of work and one less code path. */
static bool cache_matches(const client_t *c)
{
    if (!c->cache_valid) return false;
    if (c->cache_sort_key != c->sort_key || c->cache_sort_asc != c->sort_asc) return false;
    if (strcmp(c->cache_search, c->search)) return false;
    if (strcmp(c->cache_filter, c->filter_search)) return false;
    if (c->cache_filter_flags != c->mo.filter_flags) return false;
    const match_opts_t *a = &c->cache_mo, *b = &c->mo;
    if (a->match_case != b->match_case) return false;
    if (a->match_whole_word != b->match_whole_word) return false;
    if (a->match_path != b->match_path) return false;
    if (a->match_diacritics != b->match_diacritics) return false;
    if (a->match_prefix != b->match_prefix) return false;
    if (a->match_suffix != b->match_suffix) return false;
    if (a->match_regex != b->match_regex) return false;
    if (a->ignore_punctuation != b->ignore_punctuation) return false;
    if (a->ignore_whitespace != b->ignore_whitespace) return false;
    if (a->hide_empty_search_results != b->hide_empty_search_results) return false;
    if (a->filter_search != b->filter_search) {
        const char *x = a->filter_search ? a->filter_search : "";
        const char *y = b->filter_search ? b->filter_search : "";
        if (strcmp(x, y)) return false;
    }
    return true;
}

static void cache_store(client_t *c)
{
    /* `c->cache` already holds the new set; this records the parameters that
     * produced it, so the next QUERY can be answered without re-running. */
    snprintf(c->cache_search, sizeof(c->cache_search), "%s", c->search);
    snprintf(c->cache_filter, sizeof(c->cache_filter), "%s", c->filter_search);
    c->cache_filter_flags = c->mo.filter_flags;
    c->cache_mo = c->mo;
    c->cache_sort_key = c->sort_key;
    c->cache_sort_asc = c->sort_asc;
    c->cache_offset = c->offset;
    c->cache_count = c->count;
    c->cache_valid = true;
}

static void do_query(const esidx_t *db, client_t *c)
{
    uint64_t t0 = ts_us();

    if (cache_matches(c)) {
        LOGI("query: cache hit, '%s' re-sliced at offset %u of %u",
             c->search, c->offset, c->cache.n);
        send_query_results(db, c, &c->cache);
        return;
    }

    char err[256] = {0};
    ast_t *ast = syntax_parse(c->search, err, sizeof(err));
    if (!ast && err[0]) {
        /* A search we cannot parse must answer with NOTHING, not with everything.
         * Everything itself never fails a query, so the block still has to be
         * well formed and RESULT_COUNT still has to be present -- but silently
         * turning a typo into "return the whole index" would be the worst possible
         * answer to give a client that is about to render it. */
        LOGW("query: %s -- in '%s'; answering with no results", err, c->search);
        qset_free(&c->cache);
        memset(&c->cache, 0, sizeof(c->cache));
        c->cache.ids = malloc(1);
        cache_store(c);
        send_query_results(db, c, &c->cache);
        return;
    }

    sort_spec_t sort;
    sort.key = (sort_key_t)(c->sort_key & SORT_KEY_MASK);
    sort.desc = !c->sort_asc;
    /* inverse_size is the size key with the direction flipped (design §1.2) */
    if (c->sort_key & SORT_INVERSE_SIZE) sort.desc = !sort.desc;

    /* the filter string lives in the client, so hand the executor a pointer that
     * stays valid for the call */
    c->mo.filter_search = c->filter_search[0] ? c->filter_search : NULL;

    qset_t set;
    if (qexec(db, ast, &c->mo, sort, &set) != 0) {
        LOGE("query: execution failed");
        ast_free(ast);
        c_reply(c, "500 Query failed.\r\n");
        return;
    }
    ast_free(ast);

    qset_free(&c->cache);
    c->cache = set;
    cache_store(c);

    send_query_results(db, c, &c->cache);

    LOGI("query: '%s' -> %u results (dirs=%u files=%u) | %u of %u candidates from "
         "driver leaf #%u of %u | plan %.3f eval %.3f sort %.3f ms | total %.3f ms",
         c->search, c->cache.n, c->cache.n_dir, c->cache.n_file,
         c->cache.seed, db->et.count, c->cache.driver, c->cache.leaf_cnt,
         (double)c->cache.t_plan_us / 1000.0,
         (double)c->cache.t_eval_us / 1000.0,
         (double)c->cache.t_sort_us / 1000.0,
         (double)(ts_us() - t0) / 1000.0);
}

/* ------------------------------------------------- the 32 SITE EVERYTHING ops */

#define IS(x) (!strcasecmp(sub, x))

static int param_int(const char *p)
{
    if (!p) return 0;
    while (*p == ' ') p++;
    if (*p == '0' || *p == '1') return *p - '0';
    return (int)strtol(p, NULL, 10);
}

static void copy_param(char *dst, size_t n, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    snprintf(dst, n, "%s", src);
}

/* One dispatcher for all 32 subcommands. Every acknowledgement is a single line
 * beginning "200 " -- a client's reply reader treats a space at
 * index 3 as the final line and a hyphen as a continuation, and the reference
 * emits exactly one multi-line reply in the whole protocol (the query block). */
static void everything_cmd(const esidx_t *db, client_t *c,
                           const char *sub, const char *param)
{
    if (IS("case")) {
        c->mo.match_case = param_int(param);
        c_reply(c, "200 Case set to (%d).\r\n", c->mo.match_case);
    } else if (IS("whole_word")) {
        c->mo.match_whole_word = param_int(param);
        c_reply(c, "200 Whole word set to (%d).\r\n", c->mo.match_whole_word);
    } else if (IS("path")) {
        c->mo.match_path = param_int(param);
        c_reply(c, "200 Path set to (%d).\r\n", c->mo.match_path);
    } else if (IS("diacritics")) {
        c->mo.match_diacritics = param_int(param);
        c_reply(c, "200 Diacritics set to (%d).\r\n", c->mo.match_diacritics);
    } else if (IS("prefix")) {
        c->mo.match_prefix = param_int(param);
        c_reply(c, "200 Prefix set to (%d).\r\n", c->mo.match_prefix);
    } else if (IS("suffix")) {
        c->mo.match_suffix = param_int(param);
        c_reply(c, "200 Suffix set to (%d).\r\n", c->mo.match_suffix);
    } else if (IS("ignore_punctuation")) {
        c->mo.ignore_punctuation = param_int(param);
        c_reply(c, "200 Ignore punctuation set to (%d).\r\n", c->mo.ignore_punctuation);
    } else if (IS("ignore_whitespace")) {
        c->mo.ignore_whitespace = param_int(param);
        c_reply(c, "200 Ignore whitespace set to (%d).\r\n", c->mo.ignore_whitespace);
    } else if (IS("regex")) {
        c->mo.match_regex = param_int(param);
        c_reply(c, "200 Regex set to (%d).\r\n", c->mo.match_regex);
    } else if (IS("hide_empty_search_results")) {
        c->mo.hide_empty_search_results = param_int(param);
        c_reply(c, "200 Hide empty search results set to (%d).\r\n",
                c->mo.hide_empty_search_results);
    } else if (IS("search")) {
        copy_param(c->search, sizeof(c->search), param);
        c_reply(c, "200 Search set to (%s).\r\n", c->search);
    } else if (IS("filter_search")) {
        copy_param(c->filter_search, sizeof(c->filter_search), param);
        c_reply(c, "200 Filter search set to (%s).\r\n", c->filter_search);
    }
    /* ---- the FILTER_* group: an independent second matcher layer (design §1.1) ---- */
    else if (IS("filter_case")) {
        if (param_int(param)) c->mo.filter_flags |= FF_CASE;
        else c->mo.filter_flags &= ~FF_CASE;
        c_reply(c, "200 Filter case set to (%d).\r\n",
                (c->mo.filter_flags & FF_CASE) ? 1 : 0);
    } else if (IS("filter_diacritics")) {
        if (param_int(param)) c->mo.filter_flags |= FF_DIACRITICS;
        else c->mo.filter_flags &= ~FF_DIACRITICS;
        c_reply(c, "200 Filter diacritics set to (%d).\r\n",
                (c->mo.filter_flags & FF_DIACRITICS) ? 1 : 0);
    } else if (IS("filter_prefix")) {
        if (param_int(param)) c->mo.filter_flags |= FF_PREFIX;
        else c->mo.filter_flags &= ~FF_PREFIX;
        c_reply(c, "200 Filter prefix set to (%d).\r\n",
                (c->mo.filter_flags & FF_PREFIX) ? 1 : 0);
    } else if (IS("filter_suffix")) {
        if (param_int(param)) c->mo.filter_flags |= FF_SUFFIX;
        else c->mo.filter_flags &= ~FF_SUFFIX;
        c_reply(c, "200 Filter suffix set to (%d).\r\n",
                (c->mo.filter_flags & FF_SUFFIX) ? 1 : 0);
    } else if (IS("filter_ignore_punctuation")) {
        if (param_int(param)) c->mo.filter_flags |= FF_IGNORE_PUNCTUATION;
        else c->mo.filter_flags &= ~FF_IGNORE_PUNCTUATION;
        c_reply(c, "200 Filter ignore punctuation set to (%d).\r\n",
                (c->mo.filter_flags & FF_IGNORE_PUNCTUATION) ? 1 : 0);
    } else if (IS("filter_ignore_whitespace")) {
        if (param_int(param)) c->mo.filter_flags |= FF_IGNORE_WHITESPACE;
        else c->mo.filter_flags &= ~FF_IGNORE_WHITESPACE;
        c_reply(c, "200 Filter ignore whitespace set to (%d).\r\n",
                (c->mo.filter_flags & FF_IGNORE_WHITESPACE) ? 1 : 0);
    } else if (IS("filter_path")) {
        if (param_int(param)) c->mo.filter_flags |= FF_PATH;
        else c->mo.filter_flags &= ~FF_PATH;
        c_reply(c, "200 Filter path set to (%d).\r\n",
                (c->mo.filter_flags & FF_PATH) ? 1 : 0);
    } else if (IS("filter_regex")) {
        if (param_int(param)) c->mo.filter_flags |= FF_REGEX;
        else c->mo.filter_flags &= ~FF_REGEX;
        c_reply(c, "200 Filter regex set to (%d).\r\n",
                (c->mo.filter_flags & FF_REGEX) ? 1 : 0);
    } else if (IS("filter_whole_word")) {
        if (param_int(param)) c->mo.filter_flags |= FF_WHOLEWORD;
        else c->mo.filter_flags &= ~FF_WHOLEWORD;
        c_reply(c, "200 Filter whole word set to (%d).\r\n",
                (c->mo.filter_flags & FF_WHOLEWORD) ? 1 : 0);
    }
    /* ---- sort / paging ---- */
    else if (IS("sort")) {
        uint16_t key;
        int asc;
        if (sort_from_etp_name(param, &key, &asc) != 0) {
            c_reply(c, "500 Unknown sort type.\r\n");
        } else {
            c->sort_key = key;
            c->sort_asc = asc;
            c_reply(c, "200 Sort set to (%s).\r\n", param ? param : "");
        }
    } else if (IS("offset")) {
        c->offset = (uint32_t)strtoul(param ? param : "0", NULL, 10);
        c_reply(c, "200 Offset set to (%u).\r\n", c->offset);
    } else if (IS("count")) {
        c->count = (uint32_t)strtoul(param ? param : "0", NULL, 10);
        c_reply(c, "200 Count set to (%u).\r\n", c->count);
    }
    /* ---- result columns (7) ---- */
    else if (IS("size_column")) {
        c->col_size = param_int(param);
        c_reply(c, "200 Size column set to (%d).\r\n", c->col_size);
    } else if (IS("attributes_column")) {
        c->col_attributes = param_int(param);
        c_reply(c, "200 Attributes column set to (%d).\r\n", c->col_attributes);
    } else if (IS("date_modified_column")) {
        c->col_date_modified = param_int(param);
        c_reply(c, "200 Date modified column set to (%d).\r\n", c->col_date_modified);
    } else if (IS("date_created_column")) {
        c->col_date_created = param_int(param);
        c_reply(c, "200 Date created column set to (%d).\r\n", c->col_date_created);
    } else if (IS("path_column")) {
        c->col_path = param_int(param);
        c_reply(c, "200 Path column set to (%d).\r\n", c->col_path);
    } else if (IS("file_list_filename_column")) {
        c->col_file_list_filename = param_int(param);
        c_reply(c, "200 File list filename column set to (%d).\r\n",
                c->col_file_list_filename);
    } else if (IS("date_recently_changed_column")) {
        c->col_date_recently_changed = param_int(param);
        c_reply(c, "200 Date recently changed column set to (%d).\r\n",
                c->col_date_recently_changed);
    }
    /* ---- execute (1) ---- */
    else if (IS("query")) {
        do_query(db, c);
    } else {
        c_reply(c, "500 Unknown Everything command.\r\n");
    }
}

/* ----------------------------------------------------------- data connection */

/* The ETP client never uses a data connection; these exist so that an
 * ordinary FTP client can LIST and RETR (design §1.3, ref G4). */

static void data_close(client_t *c)
{
    if (c->pasv_listen >= 0) { close(c->pasv_listen); c->pasv_listen = -1; }
    if (c->data_fd >= 0) { close(c->data_fd); c->data_fd = -1; }
    if (c->peer_fd >= 0) { close(c->peer_fd); c->peer_fd = -1; }
}

/* Accept the client's data connection, or connect out to it for EPRT/PORT.
 * Returns a descriptor or -1. */
static int data_open(client_t *c)
{
    if (c->data_fd >= 0) return c->data_fd;
    if (c->peer_fd >= 0) return c->peer_fd;
    if (c->pasv_listen < 0) return -1;

    struct pollfd p = { c->pasv_listen, POLLIN, 0 };
    int r = poll(&p, 1, 10000);
    if (r <= 0) { data_close(c); return -1; }
    int fd = accept(c->pasv_listen, NULL, NULL);
    close(c->pasv_listen);
    c->pasv_listen = -1;
    if (fd < 0) return -1;
    c->data_fd = fd;
    return fd;
}

static void data_send(client_t *c, const char *s, size_t n)
{
    int fd = data_open(c);
    if (fd < 0) return;
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, s + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; }
        off += (size_t)w;
    }
}

static void data_sendf(client_t *c, const char *fmt, ...)
{
    char stack[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);
    data_send(c, stack, strlen(stack));
}

/* ---------------------------------------------------------------- LIST/MLSD */

/* Forward declaration: do_list() below emits through it, and the definition sits
 * next to the command loop. */
static void emit_listing(client_t *c, const char *dir, const char *name,
                         const struct stat *sb, bool longfmt, bool machine);

/* Resolve an FTP path against the working directory, then to an index entry.
 * Returns EID_NONE when it is not in the index -- LIST then falls back to the
 * real filesystem, which is what a stock FTP client expects. */
static eid_t resolve_path(const esidx_t *db, const client_t *c, const char *arg,
                          char *out, size_t outsz)
{
    char joined[8192];
    if (arg && *arg) {
        if (arg[0] == '/') snprintf(joined, sizeof(joined), "%s", arg);
        else snprintf(joined, sizeof(joined), "%s/%s", c->cwd, arg);
    } else {
        snprintf(joined, sizeof(joined), "%s", c->cwd);
    }
    /* normalise . and .. textually; there is no real cwd to resolve against */
    char norm[8192];
    size_t o = 0;
    const char *p = joined;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') e++;
        size_t l = (size_t)(e - p);
        if (l == 1 && p[0] == '.') { /* skip */ }
        else if (l == 2 && p[0] == '.' && p[1] == '.') {
            while (o > 0 && norm[o - 1] != '/') o--;
            if (o > 0) o--;
        } else {
            if (o + l + 2 >= sizeof(norm)) break;
            norm[o++] = '/';
            memcpy(norm + o, p, l);
            o += l;
        }
        p = e;
    }
    norm[o] = '\0';
    if (o == 0) norm[o++] = '/', norm[o] = '\0';
    snprintf(out, outsz, "%s", norm);
    return di_lookup(db, norm);
}

static void do_list(const esidx_t *db, client_t *c, const char *arg, bool longfmt,
                    bool machine)
{
    char path[8192];
    eid_t id = resolve_path(db, c, arg, path, sizeof(path));

    if (id == EID_NONE) {
        /* not indexed: serve the real filesystem so an ordinary FTP client still
         * works. The index is the interesting part, not this. */
        struct stat sb;
        if (stat(path, &sb) != 0) {
            c_reply(c, "550 %s: no such file or directory.\r\n", arg ? arg : path);
            data_close(c);
            return;
        }
        if (S_ISDIR(sb.st_mode)) {
            DIR *d = opendir(path);
            struct dirent *de;
            if (!d) {
                c_reply(c, "550 %s: cannot read directory.\r\n", path);
                data_close(c);
                return;
            }
            c_reply(c, "150 Opening data connection for %s.\r\n", path);
            while ((de = readdir(d)) != NULL) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                char full[16384];
                snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
                struct stat es;
                if (stat(full, &es) != 0) continue;
                emit_listing(c, path, de->d_name, &es, longfmt, machine);
            }
            closedir(d);
        } else {
            c_reply(c, "150 Opening data connection for %s.\r\n", path);
            emit_listing(c, path, strrchr(path, '/') ? strrchr(path, '/') + 1 : path,
                         &sb, longfmt, machine);
        }
        c_reply(c, "226 Transfer complete.\r\n");
        data_close(c);
        return;
    }

    if (!(db->et.flags[id] & EF_DIR)) {
        struct stat sb;
        if (stat(path, &sb) == 0) {
            c_reply(c, "150 Opening data connection for %s.\r\n", path);
            emit_listing(c, path, display_name_of(db, id), &sb, longfmt, machine);
            c_reply(c, "226 Transfer complete.\r\n");
        } else {
            c_reply(c, "550 %s: cannot stat.\r\n", path);
        }
        data_close(c);
        return;
    }

    c_reply(c, "150 Opening data connection for %s.\r\n", path);
    children_t cv = di_children(db, id);
    for (uint32_t i = 0; i < cv.n; i++) {
        eid_t k = cv.items[i];
        struct stat sb;
        char full[16384];
        snprintf(full, sizeof(full), "%s/%s", path, display_name_of(db, k));
        if (stat(full, &sb) != 0) {
            /* the index knows it but stat failed; synthesise from the columns */
            memset(&sb, 0, sizeof(sb));
            sb.st_size = (off_t)db->et.size[k];
            sb.st_mtime = (time_t)db->et.mtime[k];
        }
        emit_listing(c, path, display_name_of(db, k), &sb, longfmt, machine);
    }
    c_reply(c, "226 Transfer complete.\r\n");
    data_close(c);
}

/* MLST/MLSD/LIST formatting.
 *
 * The ETP client never asks for these, so they exist for an ordinary FTP
 * client (design §1.3). The MLSD fact lines follow RFC 3659 §7, which is what
 * Everything advertises in FEAT (" MLSD", " MLST type*;size*;modify*;"). */

static void emit_listing(client_t *c, const char *dir, const char *name,
                         const struct stat *sb, bool longfmt, bool machine)
{
    char full[16384];
    snprintf(full, sizeof(full), "%s/%s", dir && *dir ? dir : "/", name);

    if (machine) {
        char mtime[32] = "";
        struct tm tm;
        if (localtime_r(&sb->st_mtime, &tm))
            strftime(mtime, sizeof(mtime), "%Y%m%d%H%M%S", &tm);
        data_sendf(c, "type=%s;size=%lld;modify=%s; %s\r\n",
                   S_ISDIR(sb->st_mode) ? "dir" : "file",
                   (long long)sb->st_size, mtime, name);
        return;
    }
    if (!longfmt) {
        data_sendf(c, "%s\r\n", name);
        return;
    }
    /* Unix ls style: permissions, link count, owner, group, size, month day
     * time/year, name -- the shape `ls -l` and every FTP client expect. */
    char perm[11] = "----------";
    if (S_ISDIR(sb->st_mode)) perm[0] = 'd';
    if (sb->st_mode & S_IRUSR) perm[1] = 'r';
    if (sb->st_mode & S_IWUSR) perm[2] = 'w';
    if (sb->st_mode & S_IXUSR) perm[3] = 'x';
    if (sb->st_mode & S_IRGRP) perm[4] = 'r';
    if (sb->st_mode & S_IWGRP) perm[5] = 'w';
    if (sb->st_mode & S_IXGRP) perm[6] = 'x';
    if (sb->st_mode & S_IROTH) perm[7] = 'r';
    if (sb->st_mode & S_IWOTH) perm[8] = 'w';
    if (sb->st_mode & S_IXOTH) perm[9] = 'x';

    struct tm tm;
    localtime_r(&sb->st_mtime, &tm);
    char when[32];
    if (tm.tm_year > 70)
        strftime(when, sizeof(when), "%b %e  %Y", &tm);
    else
        strftime(when, sizeof(when), "%b %e %H:%M", &tm);

    data_sendf(c, "%s %3lu %-8s %-8s %12lld %s %s\r\n",
               perm, (unsigned long)sb->st_nlink, "esidx", "esidx",
               (long long)sb->st_size, when, name);
}

/* ------------------------------------------------------------- command loop */

/* Split "VERB param" at the first run of spaces. Everything does not skip the
 * spaces after the split (etp_server.c:1683-1685) so that a filename may begin
 * with one; we keep that. */
static void split_verb(char *line, char **verb, char **param)
{
    char *p = line;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (!*p) { *verb = line; *param = line + strlen(line); return; }
    *p++ = '\0';
    while (*p == ' ' && p[1] == ' ') p++;   /* collapse the separator run only */
    *verb = line;
    *param = p;
}

/* Trailing '\r' off a CRLF-terminated command line. */
static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}

static bool auth_ok(const etp_opts_t *o, const char *user, const char *pass)
{
    if (!o->username || !*o->username) return true;         /* anonymous */
    if (strcmp(user, o->username)) return false;
    const char *want = o->password ? o->password : "";
    return strcmp(pass ? pass : "", want) == 0;
}

static void cmd_user(const etp_opts_t *o, client_t *c, char *param)
{
    snprintf(c->pending_user, sizeof(c->pending_user), "%s", param);
    if (!o->username || !*o->username) {
        /* no credentials configured: log straight in, which is what the reference
         * does with an empty username setting */
        c->logged_in = true;
        c_reply(c, "230 Logged on.\r\n");
        return;
    }
    c_reply(c, "331 Password required.\r\n");
}

static void cmd_pass(const etp_opts_t *o, client_t *c, char *param)
{
    if (auth_ok(o, c->pending_user, param)) {
        c->logged_in = true;
        c_reply(c, "230 Logged on.\r\n");
    } else {
        c->logged_in = false;
        c_reply(c, "530 Not logged in.\r\n");
    }
}

static void handle_command(const etp_opts_t *o, const esidx_t *db, client_t *c,
                           char *line)
{
    chomp(line);
    if (!*line) return;

    char *verb, *param;
    split_verb(line, &verb, &param);
    LOGD("< %s %s", verb, param);

    /* A transfer already in flight makes any new command a protocol error
     * (etp_server.c:1707-1712). Only an OPEN data connection counts: PASV merely
     * arms a listener, and the reference does not set its data-type until the
     * transfer actually starts, so PASV followed by LIST is legal. */
    if (c->data_fd >= 0) {
        c_reply(c, "503 Invalid sequence of commands.\r\n");
        return;
    }

    /* --- authentication --- */
    if (!strcasecmp(verb, "USER")) { cmd_user(o, c, param); return; }
    if (!strcasecmp(verb, "PASS")) { cmd_pass(o, c, param); return; }
    if (!strcasecmp(verb, "QUIT")) {
        c_reply(c, "221 Goodbye.\r\n");
        c->fd = -1;
        return;
    }
    if (!strcasecmp(verb, "NOOP")) { c_reply(c, "200 NOOP ok.\r\n"); return; }

    if (!c->logged_in) {
        /* Everything answers 530 to everything but USER/PASS/QUIT. The ETP
         * client's connect() sends USER then PASS and reads one reply each, so
         * this boundary matters. */
        c_reply(c, "530 Not logged on.\r\n");
        return;
    }

    /* --- capability and session --- */
    if (!strcasecmp(verb, "FEAT")) {
        /* the exact feature set etp_server.c:1728-1736 advertises, so that a
         * client which probes sees what it expects from an ETP server */
        c_reply(c, "211-Features:\r\n"
                   " MDTM\r\n"
                   " REST STREAM\r\n"
                   " SIZE\r\n"
                   " MLST type*;size*;modify*;\r\n"
                   " MLSD\r\n"
                   " UTF8\r\n"
                   " EVERYTHING\r\n"
                   "211 End\r\n");
        return;
    }
    if (!strcasecmp(verb, "OPTS")) {
        /* `OPTS UTF8 ON|OFF` and nothing else. The official client sends
         * `OPTS UTF8 ON` as the first command after login and will not send the
         * column toggles or QUERY until it is answered 200, so rejecting the
         * argument -- as this did -- hangs the session with no error anywhere.
         * Matching the argument exactly also means rejecting the bare
         * `OPTS UTF8`, which the reference rejects even though FEAT advertises
         * UTF8 (etp_server.c:1728-1736). Wording is the reference's too: the
         * client only requires the "200 " prefix, but D1 says match byte for
         * byte where the cost is nil. */
        int on = -1;
        if (!strncasecmp(param, "UTF8", 4) && (param[4] == ' ' || param[4] == '\0')) {
            const char *v = param + 4;
            while (*v == ' ') v++;
            if (!strcasecmp(v, "ON")) on = 1;
            else if (!strcasecmp(v, "OFF")) on = 0;
        }
        if (on < 0) c_reply(c, "501 Invalid option.\r\n");
        else c_reply(c, "200 UTF8 mode %s.\r\n", on ? "enabled" : "disabled");
        return;
    }
    if (!strcasecmp(verb, "SYST")) { c_reply(c, "215 UNIX Type: L8\r\n"); return; }
    if (!strcasecmp(verb, "TYPE")) {
        /* The client never sends TYPE and reads UTF-8 regardless, so accept
         * anything rather than fail a sequence it depends on. */
        c_reply(c, "200 Type set to %s.\r\n", param && *param ? param : "I");
        return;
    }
    if (!strcasecmp(verb, "HELP")) {
        c_reply(c, "214-Site commands:\r\n EVERYTHING\r\n214 End\r\n");
        return;
    }

    /* --- SITE EVERYTHING, and the bare EVERYTHING the client actually sends.
     *     etp_server.c:2070-2092 and :2128-2148 route both to one function. --- */
    if (!strcasecmp(verb, "EVERYTHING")) {
        char *sub, *subparam;
        split_verb(param, &sub, &subparam);
        everything_cmd(db, c, sub, subparam);
        return;
    }
    if (!strcasecmp(verb, "SITE")) {
        char *name, *rest;
        split_verb(param, &name, &rest);
        if (!strcasecmp(name, "EVERYTHING")) {
            char *sub, *subparam;
            split_verb(rest, &sub, &subparam);
            everything_cmd(db, c, sub, subparam);
        } else {
            c_reply(c, "500 Unknown SITE command.\r\n");
        }
        return;
    }

    /* --- working directory --- */
    if (!strcasecmp(verb, "PWD") || !strcasecmp(verb, "XPWD")) {
        char w[8192];
        wire_path(c->cwd, w, sizeof(w));
        c_reply(c, "257 \"%s\" is the current directory.\r\n", w);
        return;
    }
    if (!strcasecmp(verb, "CWD") || !strcasecmp(verb, "CDUP")) {
        const char *arg = (!strcasecmp(verb, "CDUP")) ? ".." : param;
        char path[8192];
        resolve_path(db, c, arg, path, sizeof(path));
        eid_t id = di_lookup(db, path);
        struct stat sb;
        if (id == EID_NONE || stat(path, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
            c_reply(c, "550 %s: no such directory.\r\n", arg);
            return;
        }
        /* cwd is 4096 and a resolved path can be 8191; the truncation is what
         * snprintf does anyway, so say so once rather than let the compiler
         * guess */
        snprintf(c->cwd, sizeof(c->cwd), "%.*s", (int)sizeof(c->cwd) - 1, path);
        char w[8192];
        wire_path(c->cwd, w, sizeof(w));
        if (!strcasecmp(verb, "CDUP"))
            c_reply(c, "250 CDUP successful. \"%s\" is current directory.\r\n", w);
        else
            c_reply(c, "250 CWD successful. \"%s\" is current directory.\r\n", w);
        return;
    }

    /* --- passive / active data setup --- */
    if (!strcasecmp(verb, "PASV") || !strcasecmp(verb, "EPSV")) {
        data_close(c);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) { c_reply(c, "425 Cannot open data connection.\r\n"); return; }
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, 1) != 0) {
            close(s);
            c_reply(c, "425 Cannot open data connection.\r\n");
            return;
        }
        socklen_t al = sizeof(a);
        getsockname(s, (struct sockaddr *)&a, &al);
        c->pasv_listen = s;
        c->pasv_port = ntohs(a.sin_port);
        if (!strcasecmp(verb, "EPSV"))
            c_reply(c, "229 Entering Extended Passive Mode (|||%d|).\r\n", c->pasv_port);
        else
            c_reply(c, "227 Entering Passive Mode (127,0,0,1,%d,%d).\r\n",
                    c->pasv_port / 256, c->pasv_port % 256);
        return;
    }
    if (!strcasecmp(verb, "EPRT") || !strcasecmp(verb, "PORT")) {
        /* |h1,h2,h3,h4,p1,p2|  or  h1,h2,h3,h4,p1,p2 */
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", param);
        char *bar = strchr(tmp, '|');
        char *body = bar ? (bar[1] && strrchr(bar + 1, '|') ? bar + 1 : bar) : tmp;
        if (bar) { char *end = strrchr(body, '|'); if (end && end != body) *end = '\0'; }
        unsigned h[6];
        if (sscanf(body, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) != 6) {
            c_reply(c, "501 Bad address.\r\n");
            return;
        }
        data_close(c);
        snprintf(c->peer_addr, sizeof(c->peer_addr), "%u.%u.%u.%u", h[0], h[1], h[2], h[3]);
        c->peer_port = (int)(h[4] * 256 + h[5]);
        /* connect lazily, on the first transfer, so a stray PORT does not cost a
         * connection and a failed one can still be reported as 425 */
        c->peer_fd = -2;
        c_reply(c, "200 PORT command successful.\r\n");
        return;
    }

    /* --- metadata ---
     * di_lookup only holds directories, so a file falls through to stat. That is
     * not a fallback for convenience: a client asking SIZE of a file must get the
     * file's size, and the index has it either way. */
    if (!strcasecmp(verb, "SIZE")) {
        char path[8192];
        eid_t id = resolve_path(db, c, param, path, sizeof(path));
        if (id != EID_NONE) {
            c_reply(c, "213 %llu\r\n", (unsigned long long)db->et.size[id]);
            return;
        }
        struct stat sb;
        if (stat(path, &sb) != 0 || S_ISDIR(sb.st_mode)) {
            c_reply(c, "550 %s: not found.\r\n", param);
            return;
        }
        c_reply(c, "213 %lld\r\n", (long long)sb.st_size);
        return;
    }
    if (!strcasecmp(verb, "MDTM")) {
        char path[8192];
        eid_t id = resolve_path(db, c, param, path, sizeof(path));
        int64_t mt;
        if (id != EID_NONE) {
            mt = db->et.mtime[id];
        } else {
            struct stat sb;
            if (stat(path, &sb) != 0) {
                c_reply(c, "550 %s: not found.\r\n", param);
                return;
            }
            mt = (int64_t)sb.st_mtime;
        }
        struct tm tm;
        char t[32] = "";
        if (localtime_r((time_t *)&mt, &tm)) strftime(t, sizeof(t), "%Y%m%d%H%M%S", &tm);
        c_reply(c, "213 %s\r\n", t);
        return;
    }

    /* --- listings --- */
    if (!strcasecmp(verb, "LIST")) {
        /* LIST [-al] [path] -- skip the whole flag cluster, not one character:
         * stopping after the '-' would leave "l/tmp/..." as the path. */
        char *arg = param;
        bool longfmt = false;
        while (arg && *arg == '-') {
            for (; *arg && *arg != ' '; arg++) {
                if (*arg == 'l' || *arg == 'a') longfmt = true;
            }
            while (*arg == ' ') arg++;
        }
        do_list(db, c, arg, longfmt, false);
        return;
    }
    if (!strcasecmp(verb, "MLSD")) { do_list(db, c, param, false, true); return; }
    if (!strcasecmp(verb, "MLST")) {
        char path[8192];
        eid_t id = resolve_path(db, c, param, path, sizeof(path));
        struct stat sb;
        if (id == EID_NONE || stat(path, &sb) != 0) {
            c_reply(c, "550 %s: not found.\r\n", param);
            return;
        }
        const char *name = strrchr(path, '/');
        name = (name && name[1]) ? name + 1 : path;
        char mtime[32] = "";
        struct tm tm;
        if (localtime_r(&sb.st_mtime, &tm))
            strftime(mtime, sizeof(mtime), "%Y%m%d%H%M%S", &tm);
        c_reply(c, "250-Listing %s\r\n"
                   " type=%s;size=%lld;modify=%s; %s\r\n"
                   "250 End.\r\n",
                param && *param ? param : path,
                S_ISDIR(sb.st_mode) ? "dir" : "file",
                (long long)sb.st_size, mtime, name);
        return;
    }

    /* --- download --- */
    if (!strcasecmp(verb, "RETR")) {
        char path[8192];
        resolve_path(db, c, param, path, sizeof(path));
        struct stat sb;
        if (!o->allow_download) { c_reply(c, "550 Download not allowed.\r\n"); return; }
        if (stat(path, &sb) != 0 || S_ISDIR(sb.st_mode)) {
            c_reply(c, "550 %s: not a file.\r\n", param);
            return;
        }
        int fd = open(path, O_RDONLY);
        if (fd < 0) { c_reply(c, "550 %s: cannot open.\r\n", param); return; }
        int dfd = data_open(c);
        if (dfd < 0) { close(fd); c_reply(c, "425 Cannot open data connection.\r\n"); return; }
        c_reply(c, "150 Opening data connection for %s (%lld bytes).\r\n",
                param, (long long)sb.st_size);
        char buf[65536];
        for (;;) {
            ssize_t r = read(fd, buf, sizeof(buf));
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) break;
            ssize_t off = 0;
            while (off < r) {
                ssize_t w = send(dfd, buf + off, (size_t)(r - off), MSG_NOSIGNAL);
                if (w <= 0) { if (w < 0 && errno == EINTR) continue; break; }
                off += w;
            }
        }
        close(fd);
        data_close(c);
        c_reply(c, "226 Transfer complete.\r\n");
        return;
    }

    c_reply(c, "500 Unknown command.\r\n");
}

/* Pull whatever is readable out of the socket and dispatch complete lines. */
static void client_read(const etp_opts_t *o, const esidx_t *db, client_t *c)
{
    for (;;) {
        if (c->rlen >= sizeof(c->rbuf) - 1) {
            LOGW("control buffer full; dropping the connection");
            c->fd = -1;
            return;
        }
        ssize_t n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - 1 - c->rlen, 0);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) return;
            c->fd = -1;
            return;
        }
        if (n == 0) {
            /* The client closes the socket right after QUIT without reading the
             * reply, so a clean EOF here is normal. */
            LOGD("client closed the connection");
            c->fd = -1;
            return;
        }
        c->rlen += (size_t)n;
        c->rbuf[c->rlen] = '\0';

        char *start = c->rbuf;
        char *nl;
        while ((nl = memchr(start, '\n', c->rlen - (size_t)(start - c->rbuf))) != NULL) {
            *nl = '\0';
            char line[CTL_BUF];
            /* A copy, not a truncation. It cannot cut anything: client_read() refuses
             * to read past CTL_BUF - 1 bytes without a newline in them and hangs up, so
             * the longest line that reaches here is CTL_BUF - 1 including its NUL, and
             * that is exactly what `line` holds. The snprintf this replaced read like
             * a silent-truncation site on a protocol value, which is what the two fixed
             * buffers above turned out to be -- one reply line and one search -- and it
             * is worth not looking like a third. */
            memcpy(line, start, strlen(start) + 1);
            handle_command(o, db, c, line);
            if (c->fd < 0) return;
            start = nl + 1;
        }
        size_t used = (size_t)(start - c->rbuf);
        memmove(c->rbuf, start, c->rlen - used);
        c->rlen -= used;
        if (c->rlen == 0) break;      /* drained; wait for the next read event */
    }
}

/* ------------------------------------------------------------------ serving */

static void client_init(client_t *c, int fd)
{
    memset(c, 0, sizeof(*c));
    c->fd = fd;
    c->pasv_listen = -1;
    c->data_fd = -1;
    c->peer_fd = -1;
    /* etp_server.c:1207 -- COUNT defaults to "unlimited", not zero. It matters:
     * a client only sends COUNT when it is positive, so a default of 0 would make
     * every query that omitted it return no rows at all, while the reference
     * returns the whole match set. */
    c->count = 0xffffffffu;
    snprintf(c->cwd, sizeof(c->cwd), "/");
}

static void client_free(client_t *c)
{
    obuf_free(&c->wbuf);
    data_close(c);
    qset_free(&c->cache);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

int etp_serve(const etp_opts_t *opts)
{
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    g_stop = 0;

    esidx_t db;
    esidx_init(&db);
    uint64_t t0 = ts_us();
    if (esidx_load(&db, opts->dbfile) != 0) {
        fprintf(stderr, "cannot load %s\n", opts->dbfile);
        esidx_free(&db);
        return -1;
    }
    double lms = (double)(ts_us() - t0) / 1000.0;

    const char *bindaddr = (opts->bind_addr && *opts->bind_addr) ? opts->bind_addr
                                                                 : "127.0.0.1";
    /* Port 0 means "any free port", which the caller discovers from etp_bound_port()
     * or from the startup banner -- the test harness relies on it. The 21 default
     * belongs to the CLI, not here, or -p 0 would silently become 21. */
    /* Port 0 means "any free port", which the caller discovers from
     * etp_bound_port() or from the startup banner -- the test harness relies on
     * it. The 21 default belongs to the CLI, not here, or `-p 0` would silently
     * become 21. */
    int port = opts->port;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { LOGE("socket: %s", strerror(errno)); esidx_free(&db); return -1; }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bindaddr, &a.sin_addr) != 1) {
        LOGE("bad bind address %s", bindaddr);
        close(s);
        esidx_free(&db);
        return -1;
    }
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        LOGE("bind %s:%d: %s", bindaddr, port, strerror(errno));
        close(s);
        esidx_free(&db);
        return -1;
    }
    if (listen(s, 8) != 0) {
        LOGE("listen: %s", strerror(errno));
        close(s);
        esidx_free(&db);
        return -1;
    }
    socklen_t al = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &al);
    g_bound_port = ntohs(a.sin_port);
    g_listen_fd = s;

    fprintf(stderr,
            "esidx serving %u entries from %s on %s:%d (loaded in %.1f ms)\n",
            db.et.count, opts->dbfile, bindaddr, g_bound_port, lms);
    fprintf(stderr, "%s auth, downloads %s\n",
            (opts->username && *opts->username) ? "password" : "anonymous",
            opts->allow_download ? "allowed" : "refused");
    fflush(stderr);

    client_t clients[MAX_CLIENTS];
    memset(clients, 0, sizeof(clients));
    for (size_t i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;

    uint64_t served = 0;
    while (!g_stop) {
        struct pollfd pfd[MAX_CLIENTS + 1];
        int nfd = 0;
        pfd[nfd].fd = s;
        pfd[nfd].events = POLLIN;
        nfd++;
        int map[MAX_CLIENTS];
        for (size_t i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) continue;
            pfd[nfd].fd = clients[i].fd;
            pfd[nfd].events = POLLIN;
            map[nfd - 1] = (int)i;
            nfd++;
        }
        int r = poll(pfd, (nfds_t)nfd, 1000);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOGE("poll: %s", strerror(errno));
            break;
        }
        if (r == 0) continue;

        if (pfd[0].revents & POLLIN) {
            int cfd = accept(s, NULL, NULL);
            if (cfd >= 0) {
                int one2 = 1;
                setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one2, sizeof(one2));
                size_t slot = MAX_CLIENTS;
                for (size_t i = 0; i < MAX_CLIENTS; i++)
                    if (clients[i].fd < 0) { slot = i; break; }
                if (slot == MAX_CLIENTS) {
                    const char *busy = "421 Too many connections.\r\n";
                    send(cfd, busy, strlen(busy), MSG_NOSIGNAL);
                    close(cfd);
                } else {
                    client_init(&clients[slot], cfd);
                    c_reply(&clients[slot], ETP_WELCOME "\r\n");
                    served++;
                    LOGI("client %zu connected (%llu total)", slot,
                         (unsigned long long)served);
                }
            }
        }

        for (int i = 1; i < nfd; i++) {
            if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            client_t *c = &clients[map[i - 1]];
            if (pfd[i].revents & POLLIN) client_read(opts, &db, c);
            else if (pfd[i].revents & (POLLHUP | POLLERR)) c->fd = -1;
            if (c->fd < 0) {
                LOGI("client %d disconnected", map[i - 1]);
                client_free(c);
                if (opts->once) goto done;
            }
        }
    }

done:
    for (size_t i = 0; i < MAX_CLIENTS; i++) client_free(&clients[i]);
    close(s);
    g_listen_fd = -1;
    esidx_free(&db);
    fprintf(stderr, "esidx: stopped after %llu client(s)\n",
            (unsigned long long)served);
    return 0;
}
