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
