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
 * Two facts about the consumer shape this file, both verified against
 * app/src/main/java/.../transfer/EtpClient.java:
 *
 *   - It sends the *bare* `EVERYTHING <sub> [param]` form, never `SITE
 *     EVERYTHING`. etp_server treats the two identically (etp_server.c:2070 and
 *     :2128 dispatch both to the same function), and so does this.
 *   - It never opens a data connection. No PASV/EPSV/EPRT/PORT, no LIST/MLSD,
 *     no RETR -- browsing is `parent:"<path>" folder:` through QUERY. The data
 *     channel below exists for other FTP clients (design §1.3), not for this one.
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
} etp_opts_t;

/* Run until a signal or, with opts.once, until the first client disconnects.
 * Returns 0 on a clean shutdown, -1 if the listener could not be set up. */
int etp_serve(const etp_opts_t *opts);

/* Exposed for the test harness: the port actually bound, which matters when the
 * caller asked for port 0. */
int  etp_bound_port(void);

#endif /* ESIDX_ETP_H */
