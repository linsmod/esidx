/* Full scan (§7 / decision D6).
 *   - getdents64 + big buffer, no opendir/readdir          (ref A3 style)
 *   - openat relative descent, O_NOATIME with EPERM fallback (ref A1, A4)
 *   - d_type decides is_dir; stat only for size/mtime        (ref A3)
 *
* D6 note: P0 is single-threaded. Adaptive parallelism (rotational media ->
 *  1-2 threads, SSD -> ncpu) lands with the work-stealing pool in P2.
 * A3 note: unlike plocate we MUST stat every entry, because we index
 * size/mtime (L1). plocate only needs paths, so it can skip stat entirely.
 * That is also why scan stats are tracked here: the stat success rate and the
 * getdents byte volume were the two numbers that were going to tell us whether the
 * planned "batch stat by inode" optimisation (turn random I/O into near-sequential
 * on HDD) is worth building. Measured, at -v 5 (design 10, "Phase timings"): on ext4
 * a stat costs 1.81 us and does no I/O at all, because the inode the directory block
 * just named is already resident -- so there is nothing to batch, and the optimisation
 * is not worth building. The counters stayed, because they are what would show a tree
 * where that is not true.
 *
 * getdents buffer: one buffer *per recursion level*, taken from a lazily grown
 * pool. A per-frame local buffer would mean depth * SCAN_BUF_SIZE of stack
 * (128 MB at SCAN_MAX_DEPTH), and a single shared static buffer would be
 * clobbered by the recursive call while the parent frame is still walking it.
 *
 * Timing/logging: ESIDX_LOG=info for phase timings, =debug for a per-directory
 * trace.
 */

#include "esidx.h"
#include "timer.h"

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>   /* DT_DIR / DT_UNKNOWN / DT_LNK */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/stat.h>

/* our own dirent64: avoid clashing with kernel headers that may or may not
 * export struct linux_dirent64 to userspace */
struct es_dirent64 {
    uint64_t        d_ino;
    int64_t         d_off;
    unsigned short  d_reclen;
    unsigned char   d_type;
    char            d_name[];
};

#define SCAN_BUF_SIZE   (128u * 1024u)
#define SCAN_MAX_DEPTH  512

/* mtime as one comparable integer. Nanoseconds matter for a different reason than
 * precision: a directory can be emptied and refilled inside one second, and a
 * seconds-resolution stamp would then skip the subtree and keep the rows that
 * were deleted -- a stale answer that looks like a correct one (design §7,
 * ref A2 stores sec+nsec for the same reason). */
static int64_t stamp_of(const struct stat *sb)
{
    return (int64_t)sb->st_mtim.tv_sec * 1000000000LL + (int64_t)sb->st_mtim.tv_nsec;
}

static int open_dir(int dirfd, const char *name)
{
    int fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY | O_NOATIME);
    if (fd < 0 && errno == EPERM) {
        /* A4: not the owner -> retry without O_NOATIME */
        fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY);
    }
    return fd;
}

/* one getdents buffer per recursion depth, allocated on first use */
static char *buf_for_depth(char **pool, uint32_t depth)
{
    if (!pool[depth]) {
        pool[depth] = malloc(SCAN_BUF_SIZE);
        if (pool[depth]) memset(pool[depth], 0, 8);   /* keep 8-byte alignment */
    }
    return pool[depth];
}

static void scan_dir(esidx_t *db, int dirfd, eid_t parent, uint16_t depth, char **pool,
                     bool timing)
{
    scan_stats_t *st = &db->scan;
    char *buf = buf_for_depth(pool, depth);
    if (!buf) {
        LOGE("cannot allocate getdents buffer at depth %u", depth);
        return;
    }

    st->dirs++;
    if (depth > st->depth_max) st->depth_max = depth;

    /* The per-directory trace belongs to debug mode exactly, not to "debug and
     * above": at LOG_PERF it would be 651 894 lines on /work, and the cost of
     * writing them lands in the 'other' bucket of the very attribution that asked
     * for it. */
    bool trace = (log_level() == LOG_DEBUG);

    for (;;) {
        uint64_t g0 = timing ? ts_us() : 0;
        long n = syscall(SYS_getdents64, dirfd, buf, SCAN_BUF_SIZE);
        if (timing) st->getdents_us += ts_us() - g0;
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("getdents64 failed on eid %u: %s", parent, strerror(errno));
            return;
        }
        if (n == 0) break;

        st->getdents_calls++;
        st->getdents_bytes += (uint64_t)n;

        for (long off = 0; off < n; ) {
            struct es_dirent64 *de = (struct es_dirent64 *)(buf + off);
            off += de->d_reclen;

            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
                continue;

            struct stat sb;
            uint64_t s0 = timing ? ts_us() : 0;
            bool have_stat = (fstatat(dirfd, de->d_name, &sb, AT_SYMLINK_NOFOLLOW) == 0);
            if (timing) st->stat_us += ts_us() - s0;
            if (have_stat) st->stat_ok++; else st->stat_fail++;

            bool is_dir = (de->d_type == DT_DIR);
            if (de->d_type == DT_UNKNOWN && have_stat) {
                /* A3: XFS with ftype=0 and some FSes report DT_UNKNOWN */
                is_dir = S_ISDIR(sb.st_mode);
            }

            entry_in_t in;
            memset(&in, 0, sizeof(in));
            in.name = de->d_name;
            in.depth = depth;
            in.flags = (uint16_t)(is_dir ? EF_DIR : 0) |
                       (uint16_t)(de->d_name[0] == '.' ? EF_HIDDEN : 0);
            if (have_stat) {
                in.size = (int64_t)sb.st_size;
                in.mtime = (int64_t)sb.st_mtim.tv_sec;
                in.ctime = (int64_t)sb.st_ctim.tv_sec;
                in.stamp = stamp_of(&sb);
            }

            uint64_t a0 = timing ? ts_us() : 0;
            eid_t id = esidx_add(db, parent, &in);
            if (timing) st->add_us += ts_us() - a0;
            if (id == EID_NONE) {
                LOGE("cannot append entry %s (out of memory)", de->d_name);
                return;
            }

            st->entries++;
            if (is_dir) st->dirs_found++; else st->files++;

            if (is_dir && depth < SCAN_MAX_DEPTH) {
                uint64_t o0 = timing ? ts_us() : 0;
                int cfd = open_dir(dirfd, de->d_name);
                if (timing) st->open_us += ts_us() - o0;
                if (cfd >= 0) {
                    if (trace)
                        LOGD("descend eid=%u depth=%u name=%s", id, depth + 1, de->d_name);
                    scan_dir(db, cfd, id, depth + 1, pool, timing);
                    close(cfd);
                } else {
                    st->open_fail++;
                    LOGD("openat(%s) failed, subtree skipped: %s", de->d_name, strerror(errno));
                }
            }
        }
    }
}

int esidx_scan(esidx_t *db, const char *root)
{
    struct stat sb;
    if (stat(root, &sb) != 0) {
        LOGE("stat(%s) failed: %s", root, strerror(errno));
        return -1;
    }

    entry_in_t in;
    memset(&in, 0, sizeof(in));
    in.name = root;
    in.flags = EF_DIR;
    in.depth = 0;
    in.size = (int64_t)sb.st_size;
    in.mtime = (int64_t)sb.st_mtim.tv_sec;
    in.ctime = (int64_t)sb.st_ctim.tv_sec;
    in.stamp = stamp_of(&sb);

    eid_t root_id = esidx_add(db, EID_NONE, &in);
    if (root_id == EID_NONE) return -1;

    int fd = open_dir(AT_FDCWD, root);
    if (fd < 0) {
        LOGE("cannot open root %s: %s", root, strerror(errno));
        return -1;
    }

    char *pool[SCAN_MAX_DEPTH + 1] = {0};

    LOGI("scan: root=%s", root);
    uint64_t t0 = ts_us();
    /* LOG_PERF, not LOG_INFO: the attribution is four clock reads per entry, which
     * is noise against a TSC clocksource and ruinous against an HPET one (log.h,
     * LOG_PERF), and round.sh -- which produces the numbers in design §10 -- builds
     * at INFO. */
    bool timing = log_enabled(LOG_PERF);
    scan_dir(db, fd, root_id, 1, pool, timing);
    close(fd);

    TSDONE2("scan: walk", t0,
            "(dirs=%llu entries=%llu files=%llu dirs_found=%llu depth_max=%llu)",
            (unsigned long long)db->scan.dirs,
            (unsigned long long)db->scan.entries,
            (unsigned long long)db->scan.files,
            (unsigned long long)db->scan.dirs_found,
            (unsigned long long)db->scan.depth_max);

    if (timing) {
        const scan_stats_t *st = &db->scan;
        double walk = (double)(ts_us() - t0);
        double buckets[4] = { (double)st->getdents_us, (double)st->stat_us,
                              (double)st->open_us, (double)st->add_us };
        const char *what[4] = { "getdents64", "fstatat", "openat", "esidx_add" };
        double sum = 0.0;
        for (int i = 0; i < 4; i++) sum += buckets[i];
        char part[256];
        int n = 0;
        for (int i = 0; i < 4; i++)
            n += snprintf(part + n, sizeof(part) - (size_t)n, "%s%s %.1f ms (%.1f%%)",
                          i ? " | " : "", what[i], buckets[i] / 1000.0,
                          walk > 0 ? 100.0 * buckets[i] / walk : 0.0);
        /* "other" is stated rather than left to be inferred: it is the dirent loop,
         * close(), and the clock reads the attribution itself costs. */
        double other = walk - sum;
        snprintf(part + n, sizeof(part) - (size_t)n,
                 " | other %.1f ms (%.1f%%)", other / 1000.0,
                 walk > 0 ? 100.0 * other / walk : 0.0);
        LOGI("scan: split: %s", part);

        /* Price the attribution, or the percentages above are not evidence about the
         * walk but about this run. One read is measured, not assumed: clock_gettime is
         * a vDSO call against a TSC clocksource and a syscall against anything else,
         * and the two differ by two orders of magnitude. */
        enum { CAL = 20000 };
        uint64_t c0 = ts_us();
        for (int i = 0; i < CAL; i++) (void)ts_us();
        double ns = (double)(ts_us() - c0) * 1000.0 / (double)CAL;
        double reads = 4.0 * (double)st->entries +
                       2.0 * (double)(st->getdents_calls + st->dirs_found);
        double tax_us = reads * ns / 1000.0;    /* ns -> us: walk is in microseconds */
        LOGI("scan: split: %u reads at %.0f ns -- the attribution cost %.1f ms, "
             "%.1f%% of the walk above; 'other' is mostly it",
             (unsigned)reads, ns, tax_us / 1000.0,
             walk > 0 ? 100.0 * tax_us / walk : 0.0);
    }

    if (log_enabled(LOG_DEBUG)) {
        uint64_t ok = db->scan.stat_ok, bad = db->scan.stat_fail;
        LOGD("scan: stat ok=%llu fail=%llu (%.1f%% ok), getdents calls=%llu bytes=%llu (avg %.0f B/call), open_fail=%llu",
             (unsigned long long)ok, (unsigned long long)bad,
             (ok + bad) ? 100.0 * (double)ok / (double)(ok + bad) : 0.0,
             (unsigned long long)db->scan.getdents_calls,
             (unsigned long long)db->scan.getdents_bytes,
             db->scan.getdents_calls
                 ? (double)db->scan.getdents_bytes / (double)db->scan.getdents_calls : 0.0,
             (unsigned long long)db->scan.open_fail);
    }
    if (db->scan.open_fail)
        LOGW("scan: %llu directories could not be opened (permissions?)",
             (unsigned long long)db->scan.open_fail);

    uint64_t total = db->scan.stat_ok + db->scan.stat_fail;
    if (total) {
        double sec = ts_ms_since(t0) / 1000.0;
        LOGI("scan: %.1f us/entry, %.0f entries/s",
             sec * 1e6 / (double)total, (double)total / (sec > 0 ? sec : 1e-9));
    }

    uint32_t nbuf = 0;
    for (uint32_t d = 0; d <= SCAN_MAX_DEPTH; d++) {
        if (pool[d]) nbuf++;
        free(pool[d]);
    }
    if (nbuf > 1)
        LOGD("scan: %u getdents buffers in use (%.1f MiB), freed after the walk",
             nbuf, (double)nbuf * SCAN_BUF_SIZE / (1024.0 * 1024.0));

    if (db->scan.depth_max >= SCAN_MAX_DEPTH)
        LOGW("scan: hit the depth cap (%u) -- subtrees below that were NOT indexed",
             SCAN_MAX_DEPTH);
    return 0;
}

const scan_stats_t *esidx_scan_stats(const esidx_t *db) { return &db->scan; }

/* ==================================================================== update */

/* Incremental refresh (design §7).
 *
 * Two passes over the same walk, and the difference between them is the whole
 * reason this is not simply "rescan":
 *
 *   names  one stat per directory. A directory whose mtime is unchanged is not
 *          descended at all, and inside one that did change only the *new* names
 *          are stat'ed. Correct for everything that changes a name.
 *   deep   every entry is stat'ed and its columns compared, so a file whose
 *          content changed is noticed even though its parent's mtime did not.
 *
 * The cheap pass cannot be the only one, and the reason is worth writing down
 * because the design's own citation (A2, plocate) has no problem with it:
 * plocate stores paths, so a file whose *content* changed is still correctly
 * indexed. We store size, mtime and ctime, and those live on the file, not on
 * its parent. `dm:today` over a tree where somebody edited a file an hour ago is
 * exactly the query that has to be right.
 *
 * Identity within a directory is the name, not the inode. Inode matching looks
 * cheaper and is wrong in two ways: ext4 recycles an inode, so a deleted file
 * and its replacement would be merged into one row, and two hard links in the
 * same directory share one inode and would collapse into a single row. Matching
 * names costs a strcmp against the children already in the index and makes both
 * cases fall out correctly -- a rename is a remove plus an add, which is what it
 * is on disk anyway.
 */

typedef struct {
    uint32_t name_off;      /* offset into the name pool, +1 so 0 means empty */
    eid_t    id;
    uint8_t  seen;          /* claimed by a getdents entry during this pass */
} cslot_t;

/* One per recursion depth, for the same reason the getdents buffers are: the walk
 * descends while the parent frame is still part-way through its own listing, and
 * a single shared table would be overwritten by the child -- which then makes the
 * parent treat every remaining name as new and add a duplicate of each. */
typedef struct {
    cslot_t *tab;
    uint32_t *used;         /* slot indices currently occupied, so a directory
                             * with three children does not pay to walk the table
                             * sized for the largest one seen */
    uint32_t  used_n, cap, mask;
    bool      dead;         /* allocation failed */
} ctab_t;

typedef struct {
    esidx_t       *db;
    int            deep;
    update_stats_t *st;
    ctab_t         tabs[SCAN_MAX_DEPTH + 1];
    char          *pool[SCAN_MAX_DEPTH + 1];
} upd_t;

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;   /* FNV-1a, the same one the dir hash uses */
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* Load the claim table for `dir` with the children it already has, sized to this
 * directory rather than to the largest one: /usr has a handful of directories
 * with thousands of entries and tens of thousands with three. */
static void ctab_load(upd_t *u, uint32_t depth, eid_t dir)
{
    esidx_t *db = u->db;
    ctab_t *t = &u->tabs[depth];
    uint32_t nkids = di_child_count(db, dir);

    /* Drop the previous directory's entries. Both fields: `seen` survives in a
     * slot that is reused, and a slot that still claims to have been claimed makes
     * ctab_claim() miss a child that is really there -- which shows up as a
     * duplicate row for every such name, and as no removal at all, because the
     * stale `seen` is what the removal pass reads. */
    for (uint32_t i = 0; i < t->used_n; i++)
        memset(&t->tab[t->used[i]], 0, sizeof(cslot_t));
    t->used_n = 0;

    uint32_t want = 64;
    while (want < nkids * 2) want *= 2;
    if (want > t->cap) {
        cslot_t *nt = realloc(t->tab, want * sizeof(cslot_t));
        uint32_t *nu = realloc(t->used, want * sizeof(uint32_t));
        if (!nt || !nu) {
            LOGE("update: cannot allocate the claim table (%u slots)", want);
            free(nt); free(nu);
            t->tab = NULL; t->used = NULL; t->cap = 0; t->dead = true;
            return;
        }
        memset(nt, 0, want * sizeof(cslot_t));
        t->tab = nt; t->used = nu; t->cap = want;
    }
    t->dead = false;
    t->mask = t->cap - 1;

    children_t cv = di_children(db, dir);
    for (uint32_t i = 0, cn = di_children_n(cv); i < cn; i++) {
        eid_t kid = di_child_at(cv, i);
        uint32_t h = name_hash(name_of(db, kid)) & t->mask;
        while (t->tab[h].name_off) h = (h + 1) & t->mask;
        t->tab[h].name_off = db->et.name[kid].off + 1;
        t->tab[h].id = kid;
        t->used[t->used_n++] = h;
    }
}

/* Claim the stored child with this name, or EID_NONE if the name is new.
 * Claiming marks it seen, so the pass at the end knows exactly which stored
 * children the filesystem no longer has. */
static eid_t ctab_claim(ctab_t *t, const esidx_t *db, const char *name)
{
    if (t->dead) return EID_NONE;
    uint32_t i = name_hash(name) & t->mask;
    while (t->tab[i].name_off) {
        if (!t->tab[i].seen &&
            strcmp(sp_get(&db->names, t->tab[i].name_off - 1), name) == 0) {
            t->tab[i].seen = 1;
            return t->tab[i].id;
        }
        i = (i + 1) & t->mask;
    }
    return EID_NONE;
}

static void ctab_free(upd_t *u)
{
    for (uint32_t d = 0; d <= SCAN_MAX_DEPTH; d++) {
        free(u->tabs[d].tab);
        free(u->tabs[d].used);
    }
}

/* Reconcile one directory's children against the filesystem. The caller decides
 * whether to get here: a directory is only descended into when it is new or when
 * its stamp moved, so nothing in here re-tests the stamp. */
static void reconcile_dir(upd_t *u, int dirfd, eid_t dir, uint16_t depth)
{
    esidx_t *db = u->db;
    update_stats_t *st = u->st;
    char *buf = buf_for_depth(u->pool, depth);
    if (!buf) { LOGE("update: cannot allocate a getdents buffer"); return; }

    struct stat sb;
    if (fstat(dirfd, &sb) != 0) {
        st->stat_fail++;
        LOGW("update: cannot stat the directory being refreshed (eid %u): %s",
             dir, strerror(errno));
        return;
    }
    st->dirs_reconciled++;

    /* Claim table over the children already indexed for this directory. */
    ctab_t *t = &u->tabs[depth];
    ctab_load(u, depth, dir);

    for (;;) {
        long n = syscall(SYS_getdents64, dirfd, buf, SCAN_BUF_SIZE);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("update: getdents64 failed on eid %u: %s", dir, strerror(errno));
            return;
        }
        if (n == 0) break;

        for (long off = 0; off < n; ) {
            struct es_dirent64 *de = (struct es_dirent64 *)(buf + off);
            off += de->d_reclen;

            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' || (de->d_name[1] == '.' && de->d_name[2] == '\0')))
                continue;
            st->entries_seen++;

            /* A directory always needs its stamp; a file only when we are here to
             * compare its attributes or it is new. */
            bool maybe_dir = (de->d_type == DT_DIR);
            eid_t child = ctab_claim(t, db, de->d_name);
            bool want_stat = u->deep || maybe_dir || child == EID_NONE;

            struct stat esb;
            bool have = false;
            if (want_stat) {
                have = (fstatat(dirfd, de->d_name, &esb, AT_SYMLINK_NOFOLLOW) == 0);
                if (!have) st->stat_fail++;
            }
            bool is_dir = maybe_dir || (de->d_type == DT_UNKNOWN && have && S_ISDIR(esb.st_mode));

            entry_in_t in;
            memset(&in, 0, sizeof(in));
            in.name = de->d_name;
            in.depth = depth;
            in.flags = (uint16_t)(is_dir ? EF_DIR : 0) |
                       (uint16_t)(de->d_name[0] == '.' ? EF_HIDDEN : 0);
            if (have) {
                in.size = (int64_t)esb.st_size;
                in.mtime = (int64_t)esb.st_mtim.tv_sec;
                in.ctime = (int64_t)esb.st_ctim.tv_sec;
                in.stamp = stamp_of(&esb);
            }

            if (child != EID_NONE) {
                bool was_dir = (db->et.flags[child] & EF_DIR) != 0;
                if (was_dir != is_dir) {
                    /* A name changed hands. The flags are baked into the type and
                     * ext bitmaps, so this is a remove and an add, not a touch. */
                    esidx_remove(db, child);
                    child = EID_NONE;
                    st->removed++;
                }
            }

            if (child == EID_NONE) {
                eid_t id = esidx_add(db, dir, &in);
                if (id == EID_NONE) {
                    LOGE("update: cannot index %s (out of memory)", de->d_name);
                    return;
                }
                st->added++;
                /* B5: a directory that has just appeared is a bulk copy in
                 * progress far more often than not, so walk it now. Nothing is
                 * skipped under it -- there is nothing stored under it to skip. */
                if (is_dir && depth < SCAN_MAX_DEPTH) {
                    int cfd = open_dir(dirfd, de->d_name);
                    if (cfd >= 0) {
                        reconcile_dir(u, cfd, id, depth + 1);
                        close(cfd);
                    } else {
                        st->stat_fail++;
                        LOGW("update: openat(%s) failed, subtree not indexed: %s",
                             de->d_name, strerror(errno));
                    }
                }
                continue;
            }

            /* known name: refresh the row, and decide about descending */
            if (is_dir) {
                /* A directory's stamp is what says whether anything below it can
                 * have changed, and it moves only when *its own* entries move --
                 * which is why the root is always reconciled and why a directory
                 * three levels down is reached through its parent rather than by
                 * asking the root. plocate's updatedb reasons identically
                 * (updatedb.cpp:601-603). */
                bool sub_changed = u->deep || !have ||
                                   db->et.stamp[child] != in.stamp;
                if (!sub_changed) {
                    st->dirs_skipped++;
                    LOGD("update: eid=%u stamp unchanged, subtree skipped", child);
                } else if (depth < SCAN_MAX_DEPTH) {
                    int cfd = open_dir(dirfd, de->d_name);
                    if (cfd >= 0) {
                        reconcile_dir(u, cfd, child, depth + 1);
                        close(cfd);
                    } else {
                        st->stat_fail++;
                        LOGW("update: openat(%s) failed, subtree not refreshed: %s",
                             de->d_name, strerror(errno));
                    }
                } else {
                    LOGW("update: depth cap reached at %s -- subtree not refreshed",
                         de->d_name);
                }
                if (have) { esidx_touch(db, child, &in); st->refreshed++; }
                continue;
            }
            if (have) { esidx_touch(db, child, &in); st->refreshed++; }
        }
    }

    /* Whatever the filesystem did not mention is gone. */
    for (uint32_t i = 0; i < t->used_n; i++) {
        cslot_t *s = &t->tab[t->used[i]];
        if (s->seen) continue;
        esidx_remove(db, s->id);
        st->removed++;
        LOGD("update: eid=%u removed (no longer on disk)", s->id);
    }

    /* The directory's own row. Its stamp is what the next pass will compare, so
     * it is written last and unconditionally. */
    entry_in_t din;
    memset(&din, 0, sizeof(din));
    din.name = name_of(db, dir);
    din.flags = EF_DIR;
    din.depth = (uint16_t)(depth ? depth - 1 : 0);
    din.size = (int64_t)sb.st_size;
    din.mtime = (int64_t)sb.st_mtim.tv_sec;
    din.ctime = (int64_t)sb.st_ctim.tv_sec;
    din.stamp = stamp_of(&sb);
    esidx_touch(db, dir, &din);
}

/* D3's merge rule, kept in one place so the three sorted arrays cannot drift
 * apart on when they compact. */
static void update_merge(esidx_t *db)
{
    sidx_t *all[3] = { &db->by_size, &db->by_mtime, &db->by_ctime };
    uint64_t now = ts_us();
    for (int i = 0; i < 3; i++) {
        sidx_t *s = all[i];
        if (!s->dn) continue;
        bool big = (uint64_t)s->dn * 100 > s->n;
        bool idle = db->last_change_us && now - db->last_change_us > 60 * 1000000ULL;
        if (!big && !idle) continue;
        uint32_t was = s->dn;
        if (sidx_merge(s) == 0 && log_enabled(LOG_DEBUG))
            LOGD("update: merged %u delta rows into a %u row array (%s)",
                 was, s->n, big ? "past 1%" : "idle 60 s");
    }
}

int esidx_update(esidx_t *db, const char *root, unsigned flags, update_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    if (!db || !db->built) { LOGE("update: the index has not been finalized"); return -1; }
    if (db->root_eid == EID_NONE) { LOGE("update: the index has no root"); return -1; }

    /* The root argument is matched against the indexed root rather than trusted,
     * because a reconcile against a different tree would delete every row it did
     * not find -- a typo must not be able to empty the index. */
    eid_t rid = db->root_eid;
    if (root && *root) {
        rid = di_lookup(db, root);
        if (rid == EID_NONE) {
            char have[4096];
            path_of(db, db->root_eid, have, sizeof(have));
            LOGE("update: %s is not this index (it was built from %s)", root, have);
            return -1;
        }
    }

    char rbuf[4096];
    path_of(db, rid, rbuf, sizeof(rbuf));
    int fd = open_dir(AT_FDCWD, rbuf);
    if (fd < 0) {
        LOGE("update: cannot open %s: %s", rbuf, strerror(errno));
        return -1;
    }

    upd_t u;
    memset(&u, 0, sizeof(u));
    u.db = db;
    u.deep = (flags & EU_DEEP) != 0;
    u.st = st;

    uint64_t t0 = ts_us();
    LOGI("update: %s %s", u.deep ? "deep refresh of" : "name refresh of", rbuf);
    reconcile_dir(&u, fd, rid, 1);
    close(fd);
    for (uint32_t d = 0; d <= SCAN_MAX_DEPTH; d++) free(u.pool[d]);
    ctab_free(&u);
    st->us = ts_us() - t0;

    update_merge(db);

    /* Both O(n) rebuilds -- the name rank and the children array -- are the drain's, and
     * neither runs per pass any more.
     *
     * The rank is sorted position, so a name the index has never seen has nowhere to go
     * until the order is recomputed, and recomputing costs O(n log n): measured at 1413 ms
     * over 5.48 M rows on /work, against 1-3 us of one binary search for the name that a
     * query would otherwise do. Doing that per pass made every pass that added a single
     * file cost more than the whole walk. Instead the entries past the rank's extent keep
     * a key computed on the fly (query.c, rank_pending) and esidx_drain() recomputes the
     * order once a twelfth of the index has accumulated -- 1413 ms per 456 000 names
     * instead of per pass.
     *
     * The children are the same bargain for the same reason: an addition cannot go into
     * the flat array in the middle, so di_add_child() put it in the overlay, and without
     * either half of that the in-memory index would answer `parent:` without the rows this
     * pass just added. Every assertion in the index suite would still pass, because each
     * one reads the snapshot this pass writes and the load rebuilds the array: the same
     * blindness design §3.4 records for the aggregate column. */
    esidx_drain(db);

    /* Tombstones are not reclaimed in place (design §11 D8), so a tree that is
     * rewritten often enough would otherwise grow the id space without bound. At
     * a quarter of the table the memory and the per-query bitmap walk have grown
     * enough to matter, and a rebuild is the cheaper way back.
     *
     * A caller that passes EU_NOCOMPACT gets the threshold reported and nothing done
     * about it -- the serve path, which cannot afford the stall (esidx.h). */
    uint32_t live = bs_count(&db->live);
    if (db->et.count >= 1024 && (uint64_t)(db->et.count - live) * 4 > db->et.count) {
        if (flags & EU_NOCOMPACT) {
            LOGI("update: %u of %u ids are tombstones -- past the compaction threshold, "
                 "left to the offline update (EU_NOCOMPACT)",
                 db->et.count - live, db->et.count);
        } else {
            LOGI("update: %u of %u ids are tombstones -- compacting",
                 db->et.count - live, db->et.count);
            if (esidx_compact(db) == 0) st->compacted = 1;
        }
    }

    LOGI("update: %u dirs (%u skipped, %u descended), %u entries seen, "
         "%u added, %u removed, %u refreshed%s | %.1f ms",
         st->dirs_skipped + st->dirs_reconciled,
         st->dirs_skipped, st->dirs_reconciled,
         st->entries_seen, st->added, st->removed, st->refreshed,
         st->compacted ? ", compacted" : "", (double)st->us / 1000.0);
    return 0;
}

int esidx_compact(esidx_t *db)
{
    char root[4096];
    if (db->root_eid == EID_NONE) return -1;
    path_of(db, db->root_eid, root, sizeof(root));

    LOGI("compact: rebuilding from %s", root);
    esidx_t fresh;
    esidx_init(&fresh);
    /* The scratch index inherits the mask rather than re-resolving it: a compaction run
     * by a process that was told not to build the trigram index must not quietly put
     * 100 MiB of it back. The sidecar is beside the same dbfile and would resolve to the
     * same answer, but inheriting cannot disagree with the process that is running. */
    fresh.skip = db->skip;
    fresh.skip_src = db->skip_src;
    if (esidx_scan(&fresh, root) != 0) { esidx_free(&fresh); return -1; }
    esidx_finalize(&fresh);
    /* Every id the caller may still be holding is now a different row, so the
     * epoch has to move even though nothing about the filesystem did. */
    fresh.epoch = db->epoch + 1;
    esidx_free(db);
    *db = fresh;
    LOGI("compact: done, %u entries, epoch %llu", db->et.count,
         (unsigned long long)db->epoch);
    return 0;
}