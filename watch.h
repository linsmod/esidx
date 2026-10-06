/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
#ifndef ESIDX_WATCH_H
#define ESIDX_WATCH_H

/* The watcher's job is narrow: turn "something changed" into the set of directories that
 * need listing again, which is the same answer `esidx_update()` computes from directory
 * stamps -- only obtained without polling.
 *
 * The events come from sfa (a submodule, its own repository) rather than from a fanotify
 * group of our own, and that is a decision about privilege rather than about code:
 * fanotify_init() needs CAP_SYS_ADMIN, so a server that called it directly could only be
 * a root process. esidx must not need root to serve a search index, so the capability
 * lives in one small separate daemon and this file is its client.
 *
 * What a kernel event can tell us and what the names pass can act on are not the same set,
 * and the difference decides the subscription (esidx_watch_open):
 *
 *   CREATE / DELETE / MOVED  a *name* appeared, vanished or changed side. Listing the
 *                            directory that holds the name finds it, which is what
 *                            reconcile_dir() does.
 *   CLOSE_WRITE / ATTRIB     an existing entry's *attributes* moved. Listing the parent
 *                            directory cannot see it -- the name is the same -- so there
 *                            is nothing for a names pass to do, and subscribing would buy
 *                            a directory listing per write and still leave size/mtime
 *                            stale. §12 risk 8 is therefore unchanged by this layer; the
 *                            per-file path is a separate one.
 *
 * So the guarantee this layer adds is narrow and worth stating exactly. While the server is
 * connected, every name change *the proxy reports* reaches the index within one event batch.
 * What the proxy could not report arrives as its loss signal instead of silence
 * (SFA_EV_UNRESOLVED, and SFA_EV_OVERFLOW for a kernel queue that ran over), and that
 * subscription is not optional: the proxy filters the signal on the subscriber's mask like
 * any other event, so a client that leaves those bits out is never told what it missed.
 *
 * The answer to a loss signal is a sweep -- esidx_sweep_dirs(), which compares every
 * directory's stamp instead of only the ones a walk reaches. That is what it takes,
 * because the obvious alternative is narrower than the signal: a mark of the root is a
 * pass that descends into a directory only when that directory's stamp moved, so it
 * repairs a bulk change (every directory on the way to it moved) and not a change buried
 * under directories that never did. The watcher asks for the sweep with
 * esidx_watch_wants_sweep() and the serve loop runs it in its apply step, because it is
 * the expensive half and belongs beside the reconcile it feeds.
 *
 * Attribute freshness remains a --refresh/--deep question, unchanged by any of this.
 */

#include <stddef.h>
#include <stdint.h>
#include "esidx.h"

typedef struct esidx_watch esidx_watch_t;

typedef struct {
    uint64_t events;     /* event messages read off the socket */
    uint64_t marked;     /* dirty marks made, before esidx_refresh_dirs() de-duplicates */
    uint64_t outside;    /* events for a path outside the index root (the mark covers a
                         * whole filesystem, so this is the normal case for a busy machine) */
    uint64_t unknown;    /* under the root, but the parent directory is not in the index:
                         * a subtree that was removed, or an event between the startup
                         * repair pass and the first reconcile (ref B4) */
uint64_t overflow;   /* the kernel queue overflowed; a full pass is the only honest answer */
    uint64_t unresolved; /* the proxy reported events it could not attribute to a path, or had
                          * to drop because this client was not draining fast enough. Treated like
                          * an overflow rather than ignored: an event we cannot name is an event
                          * we must not pretend to have handled, and one signal covers a whole
                          * read batch, so this counts batches rather than events. */
    uint64_t batches;    /* drain() calls that marked something */
} watch_stats_t;

/* Connect and subscribe. `root` is the index root and is compared against event paths
 * without a trailing slash; the reason esidx does its own prefix test is in
 * sfa/issues/closed/subscribe-prefix-filter.md: the mark is a whole filesystem, so an index
 * on /usr is sent every change made anywhere on it. The proxy grew a `--prefix` option for
 * that, and an operator should use it -- but the test stays here anyway, because one proxy
 * may serve several indexes with different roots and a proxy started with no prefix (or a
 * wider one) must not be able to mark a directory this index has never heard of. Measured on
 * a fixture that wrote 100 files inside the root and 100 beside it: 196 events, 96 of them
 * outside, i.e. half the stream is spent on paths `under_root()` throws away.
 *
 * `err` receives a one-line reason on failure. Returns NULL on failure. */
esidx_watch_t *esidx_watch_open(const char *sock_path, const char *root,
                                char *err, size_t errlen);

/* Embedded mode: one process, no proxy, no client socket.
 *
 * `sfa_srv_open()` opens the fanotify group *inside this process*, which needs
 * CAP_SYS_ADMIN -- so the process must start as root, and the only way that is not a
 * privilege escalation is to give the privilege back immediately. That is what `drop_to`
 * ("user" or "user:group") is for, and it is not optional: without it this function fails
 * rather than staying privileged, because a search server that keeps CAP_SYS_ADMIN is the
 * case the two-process form exists to avoid (sfa_server.h:18-21 says the same about
 * embedding, from the other side).
 *
 * What survives the drop is exactly CAP_DAC_READ_SEARCH, and that is measured rather than
 * assumed: sfa resolves an event's directory with open_by_handle_at(), which is gated on
 * that capability, and on r7000 sfa's own probe with the capability dropped reports
 * `open_by_handle_at: no (Operation not permitted)` plus its own warning that event paths
 * cannot be resolved. CAP_SYS_ADMIN is the other way round -- needed to open the group,
 * needed for nothing afterwards. So the process answers queries as an ordinary user holding
 * the one capability the event path needs.
 *
 * `db` is needed here and not in the socket form: events arrive inside sfa_srv_poll(), so
 * there is no drain() call for the caller to hand the index to, and the callback reaches it
 * itself. `sock_path` may be NULL (a per-process default) or "" (sfa's default path);
 * `group` is the socket's group and exists so the file is not left world-accessible.
 *
 * `err` receives a one-line reason on failure. Returns NULL on failure. */
esidx_watch_t *esidx_watch_open_embed(const char *root, esidx_t *db,
                                      const char *sock_path, const char *group,
                                      const char *drop_to, char *err, size_t errlen);

/* The socket, for the caller's poll set -- or, in embedded mode, the fanotify fd. Either
 * way it is one fd and it is the same "there is an event to read" signal, which is why the
 * serve loop needs no separate branch for the two forms: -1 only before a successful open. */
int esidx_watch_fd(const esidx_watch_t *w);

/* Read every message the socket has ready and mark the parent directory of each path.
 * Called when poll() says the fd is readable, and it must drain rather than take one
 * message: a batch of 10 000 events in a build tree is one reconcile per *directory*
 * after de-duplication, and taking one message per poll would pay the reconcile N times.
 *
 * Returns the number of marks made, 0 if there was nothing to do, -1 on a protocol or
 * socket error, and -2 if the proxy went away. -2 is not the same as 0 and must not be
 * folded into it: sfa_recv() reports "nothing to read" and "peer closed" identically, and
 * the difference between an index that is current and one that has quietly stopped being
 * current is exactly this return value (esidx_watch_drain peeks to tell them apart). */
int esidx_watch_drain(esidx_watch_t *w, esidx_t *db);

/* What the proxy says it is doing, as reported in its handshake (sfa's SFA_WF_* bits). The
 * one an operator needs is the mark mode: on a kernel where FAN_MARK_MOUNT rejects the
 * name events, sfa falls back to FAN_MARK_FILESYSTEM and the mark covers a whole
 * filesystem, which is the entire reason this client filters on the root at all -- so
 * without it in the log, "half my traffic is for paths I do not have" has no answer.
 * Returns 0 from a proxy too old to report anything, which is a fact and not an error. */
uint32_t    esidx_watch_work_mode(const esidx_watch_t *w);
const char *esidx_watch_work_mode_str(const esidx_watch_t *w);

/* Has the proxy told us it lost events since the last time this was cleared? A loss is the
 * one thing an event cannot describe, so the answer to it cannot be an event: the dirty set
 * has to be rebuilt from the filesystem, which is esidx_sweep_dirs(). Kept as a flag
 * rather than acted on here because the sweep costs one stat per directory (measured on
 * r7000: 77 ms for /usr's 34 810, 1.6 s for /work's 651 896, both with this code) and
 * belongs in the serve loop's apply step with the other expensive thing, not inside a poll
 * turn. */
int esidx_watch_wants_sweep(const esidx_watch_t *w);

/* Clear it. Called by whoever runs the sweep, so a sweep that fails to start is not
 * mistaken for one that has already happened. */
void esidx_watch_clear_sweep(esidx_watch_t *w);

const watch_stats_t *esidx_watch_stats(const esidx_watch_t *w);

void esidx_watch_close(esidx_watch_t *w);

#endif /* ESIDX_WATCH_H */