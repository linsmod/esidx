/* The watcher: an sfa client whose whole output is a set of dirty directories.
 *
 * The shape of the answer is not ours to choose. A name changed somewhere, and the only
 * thing the reconcile can do about it is list the directory that holds the name, so an
 * event for /a/b/c.txt marks /a/b -- including when c.txt is a directory that has just
 * appeared, because di_lookup() resolves directories the index already has and a new one
 * has no id to mark. The parent's pass is what adds it, and it descends into it (ref B5),
 * so a directory that arrives with a full subtree needs nothing else.
 *
 * That is one rule for every event type, which is why there is no per-event branch below
 * except for the two cases where the event is about the whole index: the queue overflowing,
 * and a proxy that could not resolve a path. Both mean "something happened that I cannot
 * enumerate", and the only honest response to that is a full pass.
 */

#include "watch.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#ifdef ESIDX_HAVE_SFA
#include "sfa/sfa.h"

/* The four name events plus the two that mean "I cannot enumerate this". MOVED_FROM and
 * MOVED_TO are here for a proxy that negotiated a kernel without FAN_RENAME: they arrive
 * as two events for one rename, which costs one extra mark of the same directory and no
 * extra id, because the reconcile claims what it already has.
 *
 * SFA_EV_UNRESOLVED is not optional and not decoration. It goes out through the proxy's
 * broadcast(), which filters on the subscriber's mask, so a client that leaves the bit out
 * is never told that an event was dropped -- which is precisely the case where it needs
 * telling. Measured on the proxy that emits it: a 500-file `rm -rf` of a nested tree
 * delivers 139 of 511 events, the rest lost because the parent directory named by the event
 * no longer existed when the event was read (sfa issues/closed/silent-event-loss.md). With
 * the bit subscribed we get one signal per read batch and answer it with a full pass; the
 * same workload without it leaves up to 73% of the deletions invisible until the next
 * periodic pass, and invisible for good when --watch is configured without --refresh. */
#define SFA_WATCH_MASK (SFA_EV_CREATE | SFA_EV_DELETE | SFA_EV_MOVED |      \
                        SFA_EV_MOVED_FROM | SFA_EV_MOVED_TO | SFA_EV_OVERFLOW | \
                        SFA_EV_UNRESOLVED)

struct esidx_watch {
    int         fd;
    char       *root;        /* no trailing slash, so the prefix test is one compare */
    size_t      rootlen;
    watch_stats_t st;
    int         gone;        /* the proxy closed the connection */
    int         want_sweep;  /* the proxy lost events: the dirty set cannot be trusted */
#ifdef SFA_WF_MARK_MOUNT
    uint32_t    work_mode;   /* SFA_WF_*, as the handshake reported it */
    char        work_str[96];
#endif
};

uint32_t esidx_watch_work_mode(const esidx_watch_t *w)
{
#ifdef SFA_WF_MARK_MOUNT
    return w ? w->work_mode : 0;
#else
    (void)w;
    return 0;
#endif
}

const char *esidx_watch_work_mode_str(const esidx_watch_t *w)
{
#ifdef SFA_WF_MARK_MOUNT
    return w ? w->work_str : "(no proxy)";
#else
    /* An sfa older than the working-mode handshake. The Makefile treats a missing submodule
     * as a supported checkout, and a stale one is the same class of surprise: it must build
     * and lose this line, not fail to compile. Hence the guard on the macro rather than on
     * the SDK function, which has no feature-test macro of its own. */
    (void)w;
    return "(not reported by this sfa)";
#endif
}

esidx_watch_t *esidx_watch_open(const char *sock_path, const char *root,
                                char *err, size_t errlen)
{
    if (!root || !*root) { snprintf(err, errlen, "no index root"); return NULL; }
    /* An empty path is the bare `--watch` spelling and means the default socket; sfa_connect
     * already spells that as NULL, so the distinction never has to leave this function. */
    if (sock_path && !*sock_path) sock_path = NULL;

    /* sfa_connect2 when the proxy is new enough to have it, because the handshake is where
     * the mark mode is reported and that is the answer to "why is this client being sent
     * events for a filesystem it does not index". sfa_connect() remains the fallback and
     * costs nothing but the line in the banner. */
    int fd;
#ifdef SFA_WF_MARK_MOUNT
    struct sfa_welcome welcome;
    fd = sfa_connect2(sock_path, &welcome);
#else
    fd = sfa_connect(sock_path);
#endif
    if (fd < 0) {
        snprintf(err, errlen, "cannot reach the sfa proxy at %s: %s",
                 sock_path ? sock_path : SFA_SOCKET_PATH, strerror(errno));
        return NULL;
    }
    if (sfa_subscribe(fd, SFA_WATCH_MASK) < 0) {
        snprintf(err, errlen, "sfa subscribe failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }

    esidx_watch_t *w = calloc(1, sizeof(*w));
    if (!w) {
        snprintf(err, errlen, "out of memory");
        close(fd);
        return NULL;
    }
    w->fd = fd;
    /* "/" is the one root whose trailing slash is part of the name, so it is kept and
     * everything else loses it; the prefix test below then works for both. */
    size_t rl = strlen(root);
    while (rl > 1 && root[rl - 1] == '/') rl--;
    w->root = strndup(root, rl);
    if (!w->root) {
        snprintf(err, errlen, "out of memory");
        close(fd);
        free(w);
        return NULL;
    }
    w->rootlen = rl;
#ifdef SFA_WF_MARK_MOUNT
    w->work_mode = welcome.flags;
    /* sfa's own formatter, so the names in our banner are the ones the proxy's own log and
     * documentation use; "(not reported)" comes back for a flags == 0 handshake, which is
     * what an older server sends. */
    sfa_work_flags_str(welcome.flags, w->work_str, sizeof(w->work_str));
#endif
    return w;
}

int esidx_watch_fd(const esidx_watch_t *w)
{
    return w ? w->fd : -1;
}

/* Is this path inside the index? The comparison is on a whole component boundary, so
 * "/usrlocal/x" is not under "/usr" -- and the root itself counts, because a mark of the
 * root is the full pass that overflow falls back to. */
static int under_root(const esidx_watch_t *w, const char *path)
{
    if (strncmp(path, w->root, w->rootlen) != 0) return 0;
    return path[w->rootlen] == '\0' || path[w->rootlen] == '/';
}

/* Mark the directory that holds `path`. The parent, always -- see the header.
 * `made` is this batch's count and `st` is the lifetime total; they are separate because
 * the two are asked different questions: the log line says what this poll turn cost, and
 * the counters say what the session has done. Returning the running total from drain()
 * made the first batch's cost grow with every batch after it. */
static void mark_parent(esidx_watch_t *w, esidx_t *db, const char *path, int *made)
{
    if (!under_root(w, path)) { w->st.outside++; return; }

    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        /* The root itself, or something directly in it with no parent below the root to
         * name. Marking the root is the correct answer for the first case -- a change to
         * the root's own attributes is what a full pass is for -- and a no-op for the
         * second, since the root is already the top of the index. */
        if (strcmp(path, w->root) == 0) {
            if (esidx_mark_dirty(db, db->root_eid) == 0) { w->st.marked++; (*made)++; }
            else w->st.unknown++;
        } else {
            w->st.unknown++;
        }
        return;
    }

    char dir[PATH_MAX];
    size_t dl = (size_t)(slash - path);
    if (dl >= sizeof(dir)) { w->st.unknown++; return; }   /* deeper than PATH_MAX */
    memcpy(dir, path, dl);
    dir[dl] = '\0';

    /* Not being in the index is not an error: a subtree that was removed a moment ago is
     * reported as events under a directory that has since been tombstoned (ref B4), and
     * the parent's pass is what noticed the removal. Counting it separately from
     * `outside` is what tells those two apart when reading the numbers. */
    eid_t dirid = di_lookup(db, dir);
    if (dirid == EID_NONE) { w->st.unknown++; return; }
    if (esidx_mark_dirty(db, dirid) == 0) { w->st.marked++; (*made)++; }
    else w->st.unknown++;
}

int esidx_watch_drain(esidx_watch_t *w, esidx_t *db)
{
    if (!w || w->fd < 0) return -1;

    int made = 0;             /* marks made by *this* call, before de-duplication */
    struct sfa_event ev;
    for (;;) {
        ssize_t n = sfa_recv(w->fd, &ev, 0);
        if (n < 0) {
            LOGW("watch: cannot read the sfa proxy: %s", strerror(errno));
            return -1;
        }
        if (n == 0) {
            /* sfa_recv() answers "nothing there" and "the peer is gone" the same way,
             * because poll() reported POLLHUP and recv() then returned 0. One peek
             * separates them, and the separation is the whole point: 0 means the batch is
             * drained and the index is current; -2 means the events that follow are not
             * coming, and a server that read that as 0 would keep claiming to be current
             * for as long as it ran. */
            char c;
            if (recv(w->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0) {
                w->gone = 1;
                return -2;
            }
            break;
        }

        w->st.events++;

        if (ev.mask & SFA_EV_OVERFLOW) {
            /* The kernel queue overflowed: there are events we will never see, so the
             * dirty set cannot be trusted to name what changed. A sweep is the answer --
             * it compares every directory's stamp rather than the ones a walk reaches --
             * and a mark of the root goes in beside it so this batch is applied even if the
             * sweep is declined or fails, which is the weaker but non-empty answer. */
            w->st.overflow++;
            w->want_sweep = 1;
            if (esidx_mark_dirty(db, db->root_eid) == 0) { w->st.marked++; made++; }
            continue;
        }
        if (ev.path_len == 0 || ev.path[0] == '\0') {
            /* An event with no name, and the only honest reading of it is "something
             * happened that I cannot name". SFA_EV_OVERFLOW is the case we could always
             * expect (the kernel queue overflowed); SFA_EV_UNRESOLVED is the proxy telling
             * us its own -- either it could not turn a handle into a path, or it had to drop
             * events because this client was not draining fast enough. Both arrive with an
             * empty path on purpose, and both are answered the same way: guess which
             * directory an unnamed event belonged to and an index goes quietly stale in a
* way nobody can distinguish from "nothing happened".
             *
             * The answer is a sweep (esidx_watch_wants_sweep), because the alternative is
             * narrower than the signal: marking the root asks a pass that descends only
             * into directories whose stamp moved, which repairs a bulk change and not one
             * buried under a quiet tree. The root mark stays as the fallback and as what
             * makes this batch get applied at all. */
            w->st.unresolved++;
            w->want_sweep = 1;
            LOGW("watch: %llu event batch(es) the proxy could not attribute to a path; "
                 "asking for a sweep", (unsigned long long)w->st.unresolved);
            if (esidx_mark_dirty(db, db->root_eid) == 0) { w->st.marked++; made++; }
            continue;
        }

        mark_parent(w, db, sfa_event_path(&ev), &made);
        const char *old = sfa_event_path2(&ev);
        if (old) mark_parent(w, db, old, &made);
    }
    if (made) w->st.batches++;
    return made;
}

int esidx_watch_wants_sweep(const esidx_watch_t *w)
{
    return w ? w->want_sweep : 0;
}

void esidx_watch_clear_sweep(esidx_watch_t *w)
{
    if (w) w->want_sweep = 0;
}

const watch_stats_t *esidx_watch_stats(const esidx_watch_t *w)
{
    return w ? &w->st : NULL;
}

void esidx_watch_close(esidx_watch_t *w)
{
    if (!w) return;
    if (w->fd >= 0) close(w->fd);
    free(w->root);
    free(w);
}

#else   /* !ESIDX_HAVE_SFA -- the submodule was not cloned */

struct esidx_watch { int unused; };

esidx_watch_t *esidx_watch_open(const char *sock_path, const char *root,
                                char *err, size_t errlen)
{
    (void)sock_path; (void)root;
    snprintf(err, errlen,
             "this binary was built without the sfa submodule, so it has no watcher "
             "(clone the submodule, or run `git submodule update --init`)");
    return NULL;
}
int esidx_watch_fd(const esidx_watch_t *w) { return w ? w->fd : -1; }
int  esidx_watch_wants_sweep(const esidx_watch_t *w) { (void)w; return 0; }
void esidx_watch_clear_sweep(esidx_watch_t *w) { (void)w; }
uint32_t esidx_watch_work_mode(const esidx_watch_t *w) { (void)w; return 0; }
const char *esidx_watch_work_mode_str(const esidx_watch_t *w) { (void)w; return "(not built in)"; }
int  esidx_watch_drain(esidx_watch_t *w, esidx_t *db) { (void)w; (void)db; return -1; }
const watch_stats_t *esidx_watch_stats(const esidx_watch_t *w) { (void)w; return NULL; }
void esidx_watch_close(esidx_watch_t *w) { (void)w; }

#endif  /* ESIDX_HAVE_SFA */