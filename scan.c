/* Full scan (§7 / decision D6).
 *   - getdents64 + big buffer, no opendir/readdir          (ref A3 style)
 *   - openat relative descent, O_NOATIME with EPERM fallback (ref A1, A4)
 *   - d_type decides is_dir; stat only for size/mtime        (ref A3)
 *
 * D6 note: P0 is single-threaded. Adaptive parallelism (rotational media ->
 * 1-2 threads, SSD -> ncpu) lands with the work-stealing pool in P2.
 * A3 note: unlike plocate we MUST stat every entry, because we index
 * size/mtime (L1). plocate only needs paths, so it can skip stat entirely.
 * That is also why scan stats are tracked here: the stat success rate and the
 * getdents byte volume are the two numbers that tell us whether the planned
 * "batch stat by inode" optimisation (turn random I/O into near-sequential on
 * HDD) is worth building.
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

static void scan_dir(esidx_t *db, int dirfd, eid_t parent, uint16_t depth, char **pool)
{
    scan_stats_t *st = &db->scan;
    char *buf = buf_for_depth(pool, depth);
    if (!buf) {
        LOGE("cannot allocate getdents buffer at depth %u", depth);
        return;
    }

    st->dirs++;
    if (depth > st->depth_max) st->depth_max = depth;

    for (;;) {
        long n = syscall(SYS_getdents64, dirfd, buf, SCAN_BUF_SIZE);
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
            bool have_stat = (fstatat(dirfd, de->d_name, &sb, AT_SYMLINK_NOFOLLOW) == 0);
            if (have_stat) st->stat_ok++; else st->stat_fail++;

            bool is_dir = (de->d_type == DT_DIR);
            if (de->d_type == DT_UNKNOWN && have_stat) {
                /* A3: XFS with ftype=0 and some FSes report DT_UNKNOWN */
                is_dir = S_ISDIR(sb.st_mode);
            }

            uint16_t flags = 0;
            if (is_dir) flags |= EF_DIR;
            if (de->d_name[0] == '.') flags |= EF_HIDDEN;

            eid_t id = esidx_add(db, parent, de->d_name, depth, flags,
                                 have_stat ? (int64_t)sb.st_size : 0,
                                 have_stat ? (int64_t)sb.st_mtim.tv_sec : 0,
                                 have_stat ? (int64_t)sb.st_ctim.tv_sec : 0);
            if (id == EID_NONE) {
                LOGE("cannot append entry %s (out of memory)", de->d_name);
                return;
            }

            st->entries++;
            if (is_dir) st->dirs_found++; else st->files++;

            if (is_dir && depth < SCAN_MAX_DEPTH) {
                int cfd = open_dir(dirfd, de->d_name);
                if (cfd >= 0) {
                    LOGD("descend eid=%u depth=%u name=%s", id, depth + 1, de->d_name);
                    scan_dir(db, cfd, id, depth + 1, pool);
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

    eid_t root_id = esidx_add(db, EID_NONE, root, 0, EF_DIR,
                              (int64_t)sb.st_size,
                              (int64_t)sb.st_mtim.tv_sec,
                              (int64_t)sb.st_ctim.tv_sec);
    if (root_id == EID_NONE) return -1;

    int fd = open_dir(AT_FDCWD, root);
    if (fd < 0) {
        LOGE("cannot open root %s: %s", root, strerror(errno));
        return -1;
    }

    char *pool[SCAN_MAX_DEPTH + 1] = {0};

    LOGI("scan: root=%s", root);
    uint64_t t0 = ts_us();
    scan_dir(db, fd, root_id, 1, pool);
    close(fd);

    TSDONE2("scan: walk", t0,
            "(dirs=%llu entries=%llu files=%llu dirs_found=%llu depth_max=%llu)",
            (unsigned long long)db->scan.dirs,
            (unsigned long long)db->scan.entries,
            (unsigned long long)db->scan.files,
            (unsigned long long)db->scan.dirs_found,
            (unsigned long long)db->scan.depth_max);

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