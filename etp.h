/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
#ifndef ESIDX_ETP_H
#define ESIDX_ETP_H

/* Protocol layer (design §1, decision D1).
 *
 * ETP is FTP plus a `SITE EVERYTHING` extension carrying 32 subcommands. This is
 * the port of `etp_server_client_t`'s command layer from voidtools' Everything
 * Server 1.0.2.5, with the `everything_plugin_*` adapter replaced by our own
 * index engine (D1). The wire behaviour is deliberately identical, because the
 * ETP client is the consumer and it was written against that server.
 *
 * Two facts about the consumer shape this file, both observed on the wire by
 * pointing a real client at this server (AGENTS.md 1.4):
 *
 *   - Clients spell the extension both ways: the official Everything client sends
 *     `SITE EVERYTHING <sub> [param]`, others send the bare `EVERYTHING <sub>
 *     [param]`. etp_server treats the two identically (etp_server.c:2070 and
 *     :2128 dispatch both to the same function), and so does this.
 *   - No client opens a data connection. No PASV/EPSV/EPRT/PORT, no LIST/MLSD,
 *     no RETR -- browsing is `parent:"<path>" folder:` through QUERY. The data
 *     channel below exists for other FTP clients (design §1.3), not for these.
 */

#include "esidx.h"

typedef struct {
    const char *dbfile;     /* snapshot to load and serve */
    const char *bind_addr;  /* default "127.0.0.1" */
    int         port;       /* default 21 */
    const char *username;   /* NULL or empty = accept anonymous */
    const char *password;
    int         allow_download;  /* RETR / disk access */
    int         once;            /* serve one connection then exit (tests) */
    /* >0: reconcile the index in place every N seconds, so the process is
     * self-updating (design §7). One pass runs before the listener is up, because a
     * client that connects first must not be answered from a stale index. 0 = the
     * historical read-only server. */
    int         refresh_secs;
    /* >0: write the snapshot every N seconds, and once on a clean exit if anything
     * changed since the last write. Separate from refresh_secs on purpose: a snapshot
     * write is the whole file (design §4.1 -- D4 keeps no derived structure in it), so
     * it blocks the serve loop for as long as it takes to write, and how often that is
     * worth doing is a policy question about I/O, not about staleness. */
    int         save_secs;
    /* Embedded mode: open the fanotify group in this process instead of subscribing to a
     * proxy, and give the privilege back immediately (watch.c measures exactly what has to
     * survive the drop). `watch_sock` is ignored in this mode except as the name of the
     * socket sfa still creates for other clients; NULL takes a per-process default. */
    const char *watch_embed;
    /* "user" or "user:group" to become once the group is open. Required with
     * watch_embed, and refusing to be absent is the point: see watch.h. */
    const char *drop_to;
    /* Group for the socket sfa_srv_open() creates. NULL means none -- which is right for a
     * socket nobody connects to, and wrong for one something else might. */
    const char *watch_group;
    /* The sfa socket to subscribe to, or NULL for no watcher. --refresh and --watch are
     * two answers to the same question (what changed) and are independent: either one
     * makes the server self-updating, either one makes a --save timer meaningful, and
     * either one makes the startup repair pass necessary, because a change between the
     * snapshot on disk and the subscription is invisible to the subscription. */
    const char *watch_sock;
    /* Seconds between sweeps, or 0 for none. A sweep compares every directory's stamp
     * rather than the ones a walk reaches, which is the only thing that finds a change
     * below a directory whose ancestors never moved (design §7 "Sweep"); it costs one stat
     * per directory, so it is a separate coarse knob from --refresh rather than part of
     * it. Independent of --watch: a deployment with no proxy can still close this hole, and
     * one with a proxy may still want the belt to the braces. */
    int         sweep_secs;
   /* How long a client may have a reply queued and accept none of it before it is dropped, in
    * ms; 0 means STALL_MS. It is an option because the right value is a property of the link,
    * not of the server: 10 s is generous for a LAN and stingy for a phone on a bad cell
    * connection, and an operator is the only one who knows which they are serving. */
   uint64_t    stall_ms;
    /* Which derived indexes to leave unbuilt, already resolved by main.c: serve owns
     * its own esidx_t, so the flag cannot be re-read here without a second place that
     * decides. `no_index` is only read when `have_no_index` is set. */
    int         have_no_index;
    const char *no_index;        /* the list, possibly "" (= read the sidecar) */
} etp_opts_t;

/* Run until a signal or, with opts.once, until the first client disconnects.
 * Returns 0 on a clean shutdown, -1 if the listener could not be set up. */
int etp_serve(const etp_opts_t *opts);

/* Exposed for the test harness: the port actually bound, which matters when the
 * caller asked for port 0. */
int  etp_bound_port(void);

#endif /* ESIDX_ETP_H */
