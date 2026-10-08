/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
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
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <linux/capability.h>
#include <grp.h>
#include <pwd.h>
/* capset() lives behind __USE_GNU and is not in POSIX, so declaring it here is the
 * alternative to linking libcap -- and D5 says no new dependency for two calls. */
extern int capset(struct __user_cap_header_struct *hdr,
                  const struct __user_cap_data_struct *data);

#include "sfa/sfa.h"
#include "sfa/sfa_server.h"

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
    /* Every indexed location, each without a trailing slash so the prefix test is one
     * compare per root and a path under the second of three is not mistaken for one
     * under the first. Owned: the caller hands over argv, which is not ours to keep. */
    char      **roots;
    size_t     *rootlens;
    uint32_t    nroots;
    watch_stats_t st;
    int         gone;        /* the proxy closed the connection */
    int         want_sweep;  /* the proxy lost events: the dirty set cannot be trusted */
    uint32_t    work_mode;   /* SFA_WF_*, as the handshake reported it */
    char        work_str[96];

    /* Embedded mode (one process holds the fanotify group itself): one entry per mount
     * this index reaches, because a fanotify mark covers one mount and sfa opens one group
     * per `sfa_srv_open()`. `db` is needed in this mode because events arrive inside
     * sfa_srv_poll() -- there is no socket to hand them to a drain() call -- so the callback
     * has to reach the index itself, and `made` carries what one poll turn marked back out
     * to drain()'s caller. A socket is still created per instance, because sfa_srv_open()
     * has no "no socket" option yet and it is a separate project; nothing connects to them
     * in this mode, so each path only has to be unique and short enough for sockaddr_un,
     * and they are unlinked on close. */
    struct sfa_srv **srv;
    char          **sock_owned;
    uint32_t        nsrv;
    esidx_t       *db;
    int            made;
};

uint32_t esidx_watch_work_mode(const esidx_watch_t *w)
{
    return w ? w->work_mode : 0;
}

const char *esidx_watch_work_mode_str(const esidx_watch_t *w)
{
    return w ? w->work_str : "(no proxy)";
}

/* Take ownership of the root list. "/" is the one root whose trailing slash is part of the
 * name, so it is kept and everything else loses it; the prefix test below then works for
 * both without a special case anywhere else. Returns 0 on success. */
static int watch_take_roots(esidx_watch_t *w, char *const *roots, uint32_t nroots)
{
    if (nroots == 0) return -1;
    w->roots = calloc(nroots, sizeof(*w->roots));
    w->rootlens = calloc(nroots, sizeof(*w->rootlens));
    if (!w->roots || !w->rootlens) return -1;
    for (uint32_t i = 0; i < nroots; i++) {
        size_t rl = strlen(roots[i]);
        while (rl > 1 && roots[i][rl - 1] == '/') rl--;
        w->roots[i] = strndup(roots[i], rl);
        if (!w->roots[i]) return -1;
        w->rootlens[i] = rl;
        w->nroots = i + 1;   /* so a partial failure still frees what was taken */
    }
    return 0;
}

static void watch_free_roots(esidx_watch_t *w)
{
    for (uint32_t i = 0; i < w->nroots; i++) free(w->roots[i]);
    free(w->roots);
    free(w->rootlens);
    w->roots = NULL;
    w->rootlens = NULL;
    w->nroots = 0;
}

/* Group the roots by the mount they live on: one fanotify mark per mount, and the mount is
 * what a mark covers.
 *
 * The grouping key is the mount *id*, not `st_dev`, and the difference is not academic: a
 * bind mount shares its source's superblock, so it has the same st_dev and a different
 * vfsmount, and FAN_MARK_MOUNT is per vfsmount. Grouping by st_dev would put a bind-mounted
 * root in the same group as its source and quietly leave it unmarked -- which is the shape
 * of "a staging tree that watches one mount and indexes several".
 *
 * STATX_MNT_ID needs kernel 5.8 and a statx(2) that reports it; where it is unavailable the
 * fallback is st_dev, which is *wrong for bind mounts and right for everything else*. The
 * fallback says so in the log rather than being silent about it, because a watcher that
 * cannot see a bind mount is a deployment that looks fine until someone mounts one.
 *
 * `groups` receives, for each root, the group it belongs to; the number of groups is
 * returned. Groups are numbered in first-appearance order so the log is stable. */
static uint32_t mnt_id_of(const char *path, bool *ok)
{
    *ok = false;
#ifdef SYS_statx
    struct statx stx;
    memset(&stx, 0, sizeof(stx));
    if (syscall(SYS_statx, AT_FDCWD, path, AT_STATX_SYNC_AS_STAT, STATX_MNT_ID, &stx) == 0 &&
        (stx.stx_mask & STATX_MNT_ID)) {
        *ok = true;
        return (uint32_t)stx.stx_mnt_id;
    }
#endif
    struct stat sb;
    if (stat(path, &sb) != 0) return 0;
    *ok = true;
    /* Fold the device into the high half: the two spaces cannot collide in practice, and a
     * group that merges two roots wrongly is caught by the test above rather than here. */
    return ((uint32_t)sb.st_dev << 12) ^ (uint32_t)(sb.st_dev >> 20);
}

static uint32_t group_roots(char *const *roots, uint32_t nroots, uint32_t *groups,
                            bool *degraded)
{
    uint32_t ngroups = 0;
    *degraded = false;
    for (uint32_t i = 0; i < nroots; i++) {
        bool ok = false;
        uint32_t id = mnt_id_of(roots[i], &ok);
        if (!ok) { groups[i] = i; ngroups++; continue; }   /* its own group, loudly */
        groups[i] = UINT32_MAX;
        for (uint32_t g = 0; g < ngroups; g++) {
            bool ok2 = false;
            if (mnt_id_of(roots[groups[g]], &ok2) == id && ok2) { groups[i] = g; break; }
        }
        if (groups[i] == UINT32_MAX) groups[i] = ngroups++;
    }
    return ngroups;
}

esidx_watch_t *esidx_watch_open(const char *sock_path, char *const *roots,
                                uint32_t nroots, char *err, size_t errlen)
{
    if (!roots || nroots == 0) { snprintf(err, errlen, "no index root"); return NULL; }
    /* An empty path is the bare `--watch` spelling and means the default socket; sfa_connect
     * already spells that as NULL, so the distinction never has to leave this function. */
    if (sock_path && !*sock_path) sock_path = NULL;

    /* sfa_connect2 rather than sfa_connect, because the handshake is where the mark mode is
     * reported and that is the answer to "why is this client being sent events for a
     * filesystem it does not index". The submodule pointer names the version, so this is
     * the header's contract rather than something to negotiate at compile time. */
    int fd;
    struct sfa_welcome welcome;
    fd = sfa_connect2(sock_path, &welcome);
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
    if (watch_take_roots(w, roots, nroots) != 0) {
        snprintf(err, errlen, "out of memory taking %u root%s", nroots, nroots == 1 ? "" : "s");
        close(fd);
        watch_free_roots(w);
        free(w);
        return NULL;
    }
    /* One proxy, one mark, one mount. Several roots are fine when they share a mount --
     * the mark covers them all and under_root() does the rest -- and refused when they do
     * not, because the fix is another proxy and that is the operator's decision to make. */
    uint32_t groups[ESIDX_MAX_ROOTS];
    bool degraded = false;
    uint32_t ng = group_roots(roots, nroots, groups, &degraded);
    if (ng > 1) {
        snprintf(err, errlen,
                 "this index covers %u mounts and the proxy holds one mark on one of them; "
                 "run one sfa-server per mount and pass --watch=SOCK for each, or serve it "
                 "with --watch-embed", ng);
        close(fd);
        watch_free_roots(w);
        free(w);
        return NULL;
    }
    w->work_mode = welcome.flags;
    /* sfa's own formatter, so the names in our banner are the ones the proxy's own log and
     * documentation use; "(未报告)" comes back for a flags == 0 handshake, which is what an
     * older server sends. */
    sfa_work_flags_str(welcome.flags, w->work_str, sizeof(w->work_str));
    return w;
}

/* ---- embedded mode: this process holds the fanotify group itself -------------------
 *
 * sfa's sfa_srv_open() does the privileged half (probe, fanotify_init, fanotify_mark, and
 * a socket for any other client) and sfa_srv_poll() drives it, handing events to on_event
 * without a socket in between. So one process can be the whole event path -- at the price
 * the sfa header names plainly: fanotify_init needs CAP_SYS_ADMIN, so this process starts
 * privileged, and sfa_server.h:18-21 is right that a process which stays privileged has
 * given up the reason the split existed.
 *
 * So it does not stay privileged. sfa_srv_open() is the only step that needs CAP_SYS_ADMIN,
 * and once the group is marked the fd keeps delivering events to a process that has none:
 * reading a fanotify group is not a privileged operation. Measured on r7000 with sfa's own
 * probe, dropping one capability at a time (kernel 6.8, the probe is the same one the
 * deployment runs):
 *
 *   drop CAP_DAC_READ_SEARCH -> open_by_handle_at: no (Operation not permitted)
 *                               and sfa's own warning: "事件路径无法反解（将丢弃无路径事件）"
 *   drop CAP_SYS_ADMIN       -> fanotify unusable, but only at open time
 *   drop chown/dac_override/fowner/setuid/setgid -> both needed ones survive
 *
 * That is the whole design, and it is why exactly one capability is kept below: sfa resolves
 * an event's directory through open_by_handle_at(), which is gated on CAP_DAC_READ_SEARCH,
 * and a single process that had dropped it would see every event as SFA_EV_UNRESOLVED --
 * which is not a degraded watcher, it is a watcher that asks for a full sweep after every
 * batch (1.6 s over /work's 651 896 directories, measured). CAP_SYS_ADMIN goes the other
 * way: it is needed to *open* the group and for nothing afterwards.
 */

/* The one capability kept, as a bit mask for capset(). Raw syscalls rather than libcap:
 * D5 says no new dependency, and this needs two calls, not a library. */
#define ESIDX_KEEP_CAP_CAC_READ_SEARCH (1u << 2)

/* Become `spec` ("user" or "user:group"), keeping only CAP_DAC_READ_SEARCH.
 *
 * The order is not a style choice. Dropping the bounding set needs CAP_SETPCAP and is
 * irreversible; setuid() from 0 empties the permitted set unless PR_SET_KEEPCAPS is set
 * first, and once the uid is gone there is no way left to put a capability *into* the
 * permitted set. So: narrow the bounding set while still root, ask to keep what the
 * setuid would otherwise discard, change identity, and only then decide what the
 * effective set is.
 *
 * `err` receives a one-line reason on failure; the caller must not continue, because every
 * step after a partial failure leaves the process in a state the next step's assumptions
 * do not describe. */
static int drop_privilege(const char *spec, char *err, size_t errlen)
{
    char user[128], group[128];
    const char *colon = strchr(spec, ':');
    size_t ulen = colon ? (size_t)(colon - spec) : strlen(spec);
    if (ulen == 0 || ulen >= sizeof(user)) {
        snprintf(err, errlen, "--drop-to: no user in \"%s\"", spec); return -1;
    }
    memcpy(user, spec, ulen); user[ulen] = '\0';
    if (colon) {
        size_t glen = strlen(colon + 1);
        if (glen == 0 || glen >= sizeof(group)) {
            snprintf(err, errlen, "--drop-to: no group in \"%s\"", spec); return -1;
        }
        memcpy(group, colon + 1, glen + 1);
    }

    struct passwd *pw = getpwnam(user);
    if (!pw) { snprintf(err, errlen, "--drop-to: no such user: %s", user); return -1; }
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    if (colon) {
        struct group *gr = getgrnam(group);
        if (!gr) { snprintf(err, errlen, "--drop-to: no such group: %s", group); return -1; }
        gid = gr->gr_gid;
    }
    if (uid == 0) {
        /* Refused rather than allowed: "--drop-to=root" would start privileged and stay
         * privileged, which is the thing this function exists to prevent. A caller that
         * wants no drop at all has the two-process deployment. */
        snprintf(err, errlen, "--drop-to=root would leave this process privileged, which is "
                              "the case --watch-embed exists to avoid");
        return -1;
    }
    if (getuid() != 0) {
        snprintf(err, errlen, "--watch-embed needs to start as root (uid 0) to open the "
                              "fanotify group; this process is uid %u", (unsigned)getuid());
        return -1;
    }

    /* 1. Narrow the bounding set to what we intend to keep. Irreversible, so it happens
     *    while CAP_SETPCAP is still available and before anything depends on it. */
    for (int cap = 0; cap <= CAP_LAST_CAP; cap++) {
        if (cap == CAP_DAC_READ_SEARCH) continue;
        if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0) == 0) continue;
        /* ESRCH/ EINVAL simply means the capability was not in the set to begin with. */
        if (errno == EINVAL) continue;
        snprintf(err, errlen, "cannot drop capability %d from the bounding set: %s",
                 cap, strerror(errno));
        return -1;
    }

    /* 2. Keep the permitted set across the setuid below. Without this, setuid() from 0
     *    empties it and there is no capability left to raise afterwards. */
    if (prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) < 0) {
        snprintf(err, errlen, "prctl(PR_SET_KEEPCAPS): %s", strerror(errno));
        return -1;
    }

    /* 3. Change identity. Supplementary groups first, and they are the *target user's*
     *    groups rather than an empty set: what this process becomes has to be the account
     *    the operator configured, and an account is its group memberships, not just its
     *    primary gid.
     *
     *    Narrowing them to the single gid (which is what this did) is the bug measured on
     *    r7000: esidx belongs to `wahaha`, /home/wahaha is 0750 wahaha:wahaha, and
     *    `sudo -u esidx find /home/wahaha` walks 3 323 560 entries -- while the serving
     *    process, with Groups: 144 and nothing else, saw the directory and not one file in
     *    it. The index then came back short by three million entries with no error, which
     *    is exactly the failure "the index is a view of what that identity may read" is
     *    supposed to make impossible: the identity it was a view of was not the one anyone
     *    configured. `sudo -u`, `su -` and systemd's User= all resolve the same list from
     *    /etc/group, so this is what makes those three agree with what gets indexed.
     *
     *    It is not a widening of what the process was *given*: initgroups() replaces the
     *    inherited set (root's, which is empty, plus whatever the unit's Group= added)
     *    with the user's own, so no group the process was not entitled to survives -- which
     *    is the property the old comment was protecting. */
    if (initgroups(user, gid) < 0) {
        snprintf(err, errlen, "initgroups(%s): %s", user, strerror(errno));
        return -1;
    }
    if (setgid(gid) < 0) {
        snprintf(err, errlen, "setgid(%u): %s", (unsigned)gid, strerror(errno));
        return -1;
    }
    if (setuid(uid) < 0) {
        snprintf(err, errlen, "setuid(%u): %s", (unsigned)uid, strerror(errno));
        return -1;
    }

    /* 4. Now decide what this process may still do: one capability, and only the one the
     *    event path needs. */
    struct __user_cap_header_struct hdr = { .version = _LINUX_CAPABILITY_VERSION_3,
                                            .pid = 0 };
    struct __user_cap_data_struct data[2];
    memset(data, 0, sizeof(data));
    data[0].effective = data[0].permitted = data[0].inheritable =
        ESIDX_KEEP_CAP_CAC_READ_SEARCH;
    if (capset(&hdr, data) < 0) {
        snprintf(err, errlen, "capset: %s", strerror(errno));
        return -1;
    }

    /* 5. Report what actually happened, and fail if it is not what was asked for. A drop
     *    that silently did less is the failure mode worth being loud about: the process
     *    would go on answering queries with capabilities nobody chose. */
    unsigned long long eff = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "CapEff:", 7) == 0) { sscanf(line + 7, "%llx", &eff); break; }
        }
        fclose(f);
    }
    if (eff != ESIDX_KEEP_CAP_CAC_READ_SEARCH) {
        snprintf(err, errlen, "after dropping to %s the effective capability set is 0x%llx, "
                              "expected 0x%x", user, eff,
                 (unsigned)ESIDX_KEEP_CAP_CAC_READ_SEARCH);
        return -1;
    }
    LOGI("watch: dropped to %s (uid %u gid %u); kept CAP_DAC_READ_SEARCH for event path "
         "resolution, dropped the rest", user, (unsigned)uid, (unsigned)gid);
    return 0;
}

/* In-process delivery. sfa calls this synchronously inside sfa_srv_poll(), on the fanotify
 * read loop, and its own header is explicit that it must stay fast: a slow callback delays
 * the kernel read and moves the pressure to the kernel queue, which reproduces the stall and
 * the silent loss the socket path avoids by simply being asynchronous.
 *
 * handle_event() is a prefix test and a set insert per path, so it is that. It must not
 * grow into a directory listing -- that is the serve loop's apply step, and it is called
 * after this returns, with the marks counted. */
/* Defined below, next to mark_parent(); declared here because the callback that needs it is
 * defined first. */
static void handle_event(esidx_watch_t *w, esidx_t *db, const struct sfa_event *ev, int *made);

static void on_event(void *user, const struct sfa_event *ev)
{
    esidx_watch_t *w = user;
    handle_event(w, w->db, ev, &w->made);
}

/* sfa's log lines are the same ones the standalone proxy prints, and they are how an
 * operator tells which mark mode was negotiated -- so they go to our log rather than to
 * stderr, and they are prefixed so their origin is readable in a log that has both. */
static void on_srv_log(void *user, const char *msg)
{
    (void)user;
    LOGI("watch: sfa: %s", msg);
}

esidx_watch_t *esidx_watch_open_embed(char *const *roots, uint32_t nroots, esidx_t *db,
                                      const char *sock_path, const char *group,
                                      const char *drop_to, char *err, size_t errlen)
{
    if (!roots || nroots == 0) {
        snprintf(err, errlen, "--watch-embed needs an index root");
        return NULL;
    }
    if (!drop_to || !*drop_to) {
        snprintf(err, errlen, "--watch-embed needs --drop-to=USER[:GROUP]: it starts as root "
                              "because fanotify needs CAP_SYS_ADMIN, and leaving it "
                              "privileged is the case this mode exists to avoid");
        return NULL;
    }
    if (!db) { snprintf(err, errlen, "--watch-embed needs an index"); return NULL; }

    esidx_watch_t *w = calloc(1, sizeof(*w));
    if (!w) { snprintf(err, errlen, "out of memory"); return NULL; }
    w->fd = -1;
    w->db = db;
    if (watch_take_roots(w, roots, nroots) != 0) {
        free(w);
        snprintf(err, errlen, "out of memory taking %u root%s", nroots, nroots == 1 ? "" : "s");
        return NULL;
    }

    /* Which mount each root is on, and therefore how many fanotify groups this process is
     * about to hold. One root on one mount -- everything that worked before this change --
     * is one group, and every line below it takes the path it always took. */
    uint32_t groups[ESIDX_MAX_ROOTS];
    bool degraded = false;
    uint32_t ng = group_roots(roots, nroots, groups, &degraded);
    w->srv = calloc(ng, sizeof(*w->srv));
    w->sock_owned = calloc(ng, sizeof(*w->sock_owned));
    if (!w->srv || !w->sock_owned) {
        snprintf(err, errlen, "out of memory for %u mount group%s", ng, ng == 1 ? "" : "s");
        goto fail;
    }
    if (degraded)
        LOGW("watch: this kernel's statx does not report a mount id, so the roots were grouped "
             "by device: correct unless one of them is a bind mount, which would then be "
             "unmarked");

    /* Privileged from here to the drop below, and no further: sfa_srv_open() is the only step
     * that needs CAP_SYS_ADMIN, and once the groups are marked their fds keep delivering
     * events to a process that has none. */
    for (uint32_t g = 0; g < ng; g++) {
        /* A socket per instance, because sfa_srv_open() has no "no socket" option yet and it
         * is a separate project. Nothing connects to them in this mode -- the events arrive
         * through on_event -- so a path only has to be unique and short enough for
         * sockaddr_un. Per process and per group, so two servers and two mounts cannot
         * collide, and each is unlinked on close. */
        char sock[108];
        int n = snprintf(sock, sizeof(sock), "/tmp/esidx-self-%ld-%u.sock",
                         (long)getpid(), g);
        if (sock_path && *sock_path && g == 0) {
            n = snprintf(sock, sizeof(sock), "%s", sock_path);
        }
        if (n < 0 || (size_t)n >= sizeof(sock)) {
            snprintf(err, errlen, "embedded socket path is too long for sockaddr_un");
            goto fail;
        }
        w->sock_owned[g] = strdup(sock);
        if (!w->sock_owned[g]) {
            snprintf(err, errlen, "out of memory");
            goto fail;
        }

        /* The prefixes are passed to sfa as well as tested here. The mark covers a whole
         * mount, so an index rooted at /usr is otherwise sent every change made anywhere on
         * it -- measured on a fixture that wrote 100 files inside the root and 100 beside it:
         * 196 events, 96 of them outside. under_root() throws those away either way; this
         * stops them being produced at all.
         *
         * At most SFA_SRV_MAX_PREFIXES per instance, which is sfa's own cap. Passing more is
         * not an error there, it is a truncation -- and a truncation here would be silent,
         * except that under_root() is the real gate and a prefix is only an optimisation, so
         * the worst a dropped prefix can do is cost events that would have been discarded. */
        const char *prefixes[SFA_SRV_MAX_PREFIXES];
        uint32_t np = 0;
        for (uint32_t i = 0; i < nroots && np < SFA_SRV_MAX_PREFIXES; i++)
            if (groups[i] == g) prefixes[np++] = w->roots[i];

        struct sfa_srv_opts opts = {
            .mount        = prefixes[0],
            .sock         = sock,
            .group        = (group && *group) ? group : NULL,
            .prefix       = prefixes,
            .nprefix      = (int)np,
            .log          = on_srv_log,
            .on_event     = on_event,
            .on_event_user = w,
            /* The same mask the socket client subscribes to, so a loss signal reaches this
             * watcher too. Not SFA_EV_ALL: subscribing wider would ask for signals this layer
             * has no answer for, and the answer to a loss signal is the one thing in here that
             * must not be skipped. */
            .on_event_mask = SFA_WATCH_MASK,
        };
        if (sfa_srv_open(&w->srv[g], &opts) < 0) {
            snprintf(err, errlen, "sfa_srv_open(%s): %s", prefixes[0],
                     w->srv[g] ? sfa_srv_error(w->srv[g]) : "no instance");
            goto fail;
        }
        w->nsrv = g + 1;
        LOGI("watch: mount group %u/%u marked at %s, prefixes %u of the %u root%s",
             g + 1, ng, prefixes[0], np, nroots, nroots == 1 ? "" : "s");
    }

    /* Privileged from here to exactly one line below. */
    if (drop_privilege(drop_to, err, errlen) < 0) goto fail;

    /* There is no handshake to read in this mode -- the client half is not used -- so the
     * work mode comes from the server's own capability report instead. sfa prints it
     * through the log callback above; what is left here is the default a proxy too old to
     * report anything would have given, which is a fact and not an error (same rule as
     * esidx_watch_work_mode). Several groups may have negotiated *different* modes -- the
     * probe is per target -- so the first is reported and a difference is said out loud
     * rather than averaged into something that describes neither. */
    w->work_mode = 0;
    snprintf(w->work_str, sizeof(w->work_str), "(embedded%s)",
             ng > 1 ? ", one mark per mount" : "");
    return w;

fail:
    for (uint32_t i = 0; i < w->nsrv; i++) sfa_srv_close(w->srv[i]);
    for (uint32_t i = 0; i < w->nsrv; i++)
        if (w->sock_owned[i]) { unlink(w->sock_owned[i]); free(w->sock_owned[i]); }
    /* A group whose sfa_srv_open() failed may have created its socket before failing. */
    for (uint32_t g = w->nsrv; g < ng; g++)
        if (w->sock_owned[g]) { unlink(w->sock_owned[g]); free(w->sock_owned[g]); }
    free(w->sock_owned);
    free(w->srv);
    watch_free_roots(w);
    free(w);
    return NULL;
}

uint32_t esidx_watch_fds(const esidx_watch_t *w, int *out, uint32_t max)
{
    if (!w) return 0;
    if (w->nsrv) {
        for (uint32_t i = 0; out && i < w->nsrv && i < max; i++)
            out[i] = sfa_srv_fd(w->srv[i]);
        return w->nsrv;
    }
    if (w->fd < 0) return 0;
    if (out && max) out[0] = w->fd;
    return 1;
}

/* Is this path inside the index? The comparison is on a whole component boundary, so
 * "/usrlocal/x" is not under "/usr" -- and the root itself counts, because a mark of the
 * root is the full pass that overflow falls back to. */
static int under_root(const esidx_watch_t *w, const char *path)
{
    for (uint32_t i = 0; i < w->nroots; i++) {
        if (strncmp(path, w->roots[i], w->rootlens[i]) != 0) continue;
        if (path[w->rootlens[i]] == '\0' || path[w->rootlens[i]] == '/') return 1;
    }
    return 0;
}

/* Mark every root, and report how many took. Reached when the loss signals come in -- a
 * batch the proxy could not attribute to a path, or a queue overflow -- and the honest
 * answer to "I do not know what changed" is every root, not the first one: a partial
 * sweep would leave the other trees frozen and read exactly like "nothing is changing
 * over there". The caller adds to both counters, because it is the one that knows which
 * is the batch and which is the session.
 *
 * `only` is the root that has to be marked when the caller knows which tree lost the event,
 * which is the ordinary path for one tree's overflow; UINT32_MAX means all of them, which is
 * what a signal that cannot be attributed to a tree means. */
static uint32_t mark_roots(esidx_t *db, uint32_t only)
{
    uint32_t n = 0;
    for (uint32_t i = 0, m = esidx_nroots(db); i < m; i++) {
        if (only != UINT32_MAX && i != only) continue;
        if (esidx_mark_dirty(db, esidx_root(db, i)) == 0) n++;
    }
    return n;
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
        for (uint32_t i = 0; i < w->nroots; i++) {
            if (strcmp(path, w->roots[i]) != 0) continue;
            /* The root that matched is marked by *its* index, not by position in the
             * watcher's list: the two lists are in the same order (etp.c hands over the
             * snapshot's roots in order) but only the index means anything here, and
             * looking it up is what makes a second root work at all. */
            if (i < esidx_nroots(db) &&
                esidx_mark_dirty(db, esidx_root(db, i)) == 0) {
                w->st.marked++;
                (*made)++;
            } else {
                w->st.unknown++;
            }
            return;
        }
        w->st.unknown++;
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

/* One event, one answer. This is the whole of what a subscription means, and it is a
 * separate function because there are now two ways an event arrives -- off the proxy's
 * socket, or synchronously inside sfa_srv_poll() when this process is the proxy -- and the
 * two must not be able to disagree. The loss signals are where that would bite: SFA_EV_OVERFLOW
 * and SFA_EV_UNRESOLVED mean "the dirty set cannot be trusted to name what changed", and a
 * copy of this that forgot to set want_sweep would turn a lost event into a silent one.
 *
 * `db` is the index, `made` is this batch's mark count. */
static void handle_event(esidx_watch_t *w, esidx_t *db, const struct sfa_event *ev, int *made)
{
    w->st.events++;

    if (ev->mask & SFA_EV_OVERFLOW) {
        /* The kernel queue overflowed: there are events we will never see, so the
         * dirty set cannot be trusted to name what changed. A sweep is the answer --
         * it compares every directory's stamp rather than the ones a walk reaches --
         * and a mark of the root goes in beside it so this batch is applied even if the
         * sweep is declined or fails, which is the weaker but non-empty answer. */
        w->st.overflow++;
        w->want_sweep = 1;
        { uint32_t k = mark_roots(db, UINT32_MAX); w->st.marked += k; *made += k; }
        return;
    }
    if (ev->path_len == 0 || ev->path[0] == '\0') {
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
        { uint32_t k = mark_roots(db, UINT32_MAX); w->st.marked += k; *made += k; }
        return;
    }

    mark_parent(w, db, sfa_event_path(ev), made);
    const char *old = sfa_event_path2(ev);
    if (old) mark_parent(w, db, old, made);
}

int esidx_watch_drain(esidx_watch_t *w, esidx_t *db)
{
    if (!w) return -1;

    int made = 0;             /* marks made by *this* call, before de-duplication */

    if (w->nsrv) {
        /* Embedded: this process is the proxy, so there is nothing to read -- the events
         * arrive inside the poll, through the callback below. What drain() has to do is
         * give each poll a turn and report what they marked. Timeout 0: the caller has
         * already waited in poll() for one of these fds to become readable, and blocking
         * here would be a second wait for the same readiness.
         *
         * Every group is polled, not just the one that woke: which group an event belongs to
         * is not in the revents, and a poll with nothing ready costs a read() that returns
         * EAGAIN. */
        int before = w->made;
        for (uint32_t g = 0; g < w->nsrv; g++) {
            if (sfa_srv_poll(w->srv[g], 0) < 0) {
                LOGW("watch: embedded sfa poll failed on group %u of %u: %s",
                     g + 1, w->nsrv, sfa_srv_error(w->srv[g]));
                return -1;
            }
        }
        made = w->made - before;
        if (made) w->st.batches++;
        return made;
    }

    if (w->fd < 0) return -1;

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
        handle_event(w, db, &ev, &made);
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
    for (uint32_t g = 0; g < w->nsrv; g++) sfa_srv_close(w->srv[g]);
    if (w->fd >= 0) close(w->fd);
    /* sfa_srv_open() unlinks before bind, so a stale file cannot stop the next start; each is
     * removed here anyway because a socket left behind by a clean exit is a question an
     * operator should never have to ask. */
    for (uint32_t g = 0; g < w->nsrv; g++)
        if (w->sock_owned[g]) { unlink(w->sock_owned[g]); free(w->sock_owned[g]); }
    free(w->sock_owned);
    free(w->srv);
    watch_free_roots(w);
    free(w);
}

