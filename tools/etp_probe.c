/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
/* etp_probe -- the ETP acceptance client.
 *
 * Built only by `make etp-probe`; not part of the server. Its job is to answer one
 * question: *would an ETP client understand this server?*
 *
 * So it is a reimplementation of a client's own wire handling, not a generic FTP
 * client. Every parsing rule below is a rule real clients apply, and the acceptance
 * suite drives the probe with the command sequence the official Everything client
 * actually emits (AGENTS.md 1.4 has how to capture it).
 *
 * The rules are pinned twice over, which is what makes "the probe understood the
 * reply" worth anything:
 *
 *   - test_etp.sh asserts each one against a live instance of this server;
 *   - `./etp-probe 21 <script>` runs the same shapes against voidtools' own server
 *     on 127.0.0.1:21, so they are satisfied by an implementation we did not write.
 *
 * If this probe parses a reply, a client parses it, because it applies the same
 * rules -- including the fragile ones. In particular:
 *
 *   - the welcome is accepted only on a `220 ` prefix;
 *   - a reply ends at `nnn` + space, and continues on `nnn` + hyphen; anything
 *     shorter than four characters also terminates;
 *   - the query block is NOT read with that rule: it keys on the literal
 *     `200-Query results` to enter and the literal `200 End.` to leave, and
 *     every line is stripped of leading whitespace first. A server that answered
 *     `200 End` or `200-Query results 42` in the wrong place would hang a real
 *     client, so the probe reports that as a protocol error rather than quietly
 *     coping;
 *   - RESULT_COUNT is parsed independently of every column toggle -- it is the
 *     total match count, not the page size;
 *   - a per-item column line only counts if it arrived before that item's
 *     FILE/FOLDER line, because the client resets its accumulators there.
 *
 * Anything a client would silently drop, the probe reports as DROPPED, so the
 * suite can assert that a well-behaved server produces none.
 *
 * Usage:
 *   etp_probe <port> <script>
 *
 * Script directives, one per line:
 *   send <text>        send a command, read one reply with the client's rules,
 *                      print `REPLY <line>` per reply line received
 *   sendraw <text>     send without reading anything
 *   query              read a query block (after a sendraw EVERYTHING QUERY),
 *                      print `COUNT <n>` and one `ROW ...` line per result
 *   close              close the connection
 *
 * Exit status is 0 if the exchange completed without a protocol error, 1
 * otherwise -- so the suite can assert on "the client would not have hung".
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINE   65536

static int g_fd = -1;

/* ---------------------------------------------------------------- reporting */

static int g_errors;

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

static void proto_error(const char *fmt, ...)
{
    va_list ap;
    fprintf(stdout, "PROTOCOL-ERROR ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    g_errors++;
}

/* ------------------------------------------------------------------ reading */

static char g_line[MAX_LINE];

/* Read one CRLF/LF line, stripping the terminator. Returns 0 on EOF. */
static int read_line(void)
{
    size_t n = 0;
    for (;;) {
        char c;
        ssize_t r = read(g_fd, &c, 1);
        if (r <= 0) {
            if (n == 0) return 0;
            break;
        }
        if (c == '\n') break;
        if (n + 1 < sizeof(g_line)) g_line[n++] = c;
    }
    g_line[n] = '\0';
    size_t l = n;
    while (l && g_line[l - 1] == '\r') g_line[--l] = '\0';
    return 1;
}

static void send_line(const char *s)
{
    char buf[MAX_LINE + 4];
    int n = snprintf(buf, sizeof(buf), "%s\r\n", s);
    if (n <= 0) return;
    ssize_t off = 0;
    while (off < n) {
        ssize_t w = write(g_fd, buf + off, (size_t)(n - off));
        if (w <= 0) break;
        off += w;
    }
}

/* Collapse a single- or multi-line reply. Final line is `nnn` + space at index 3;
 * `nnn` + hyphen continues. */
static int read_reply(void)
{
    char acc[MAX_LINE * 4];
    size_t alen = 0;
    acc[0] = '\0';

    for (;;) {
        if (!read_line()) return 0;
        size_t l = strlen(g_line);
        out("REPLY %s", g_line);
        if (alen + l + 2 < sizeof(acc)) {
            memcpy(acc + alen, g_line, l);
            alen += l;
            acc[alen++] = '\n';
            acc[alen] = '\0';
        }
        if (l >= 4 && g_line[3] == ' ') return 1;      /* final */
        if (l < 4 || g_line[3] != '-') return 1;       /* defensive bail-out */
    }
}

/* Clients parse every numeric field into a signed 64-bit value inside a
 * try/catch that swallows the overflow. Everything sends
 * 18446744073709551615 for a folder whose size it does not index
 * (etp_server.c:5229); that overflows, so the client leaves the accumulator at
 * its initial value and shows no size at all. Mirror that exactly rather than
 * saturating -- the point of this probe is to reproduce the client's behaviour,
 * not a tidier one. */
static long long parse_ll(const char *s, long long fallback)
{
    errno = 0;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) return fallback;
    return v;
}

/* ------------------------------------------------- the query block, verbatim */

/* The query block, transcribed. Leading whitespace is stripped on every line,
 * enter on the literal header, leave on the literal terminator, accumulate per
 * item, emit on FILE/FOLDER.
 *
 * Blocks are numbered and delimited so a script that reads two blocks on one
 * connection can still be asserted on -- which is how the result cache is tested.
 */
static void read_query_block(void)
{
    static int block_no;
    bool in_results = false;
    long long result_count = -1;
    long long rows = 0;

    /* per-item accumulators, all reset on FILE/FOLDER (the client resets there) */
    char a_path[MAX_LINE] = "";
    long long a_size = -1, a_dm = -1, a_dc = -1, a_drc = -1;
    int a_attrib = -1;

    for (;;) {
        if (!read_line()) { proto_error("eof inside the query block"); return; }
        char raw[MAX_LINE];
        snprintf(raw, sizeof(raw), "%s", g_line);
        /* echo the raw line so the suite can assert on the wire format itself,
         * not only on the client's interpretation of it */
        if (in_results) out("WIRE %s", raw);

        /* the terminator is checked before the in_results guard, exactly as the
         * client does (:314 before :318) */
        if (!strcmp(raw, "200 End.")) {
            if (!in_results) proto_error("'200 End.' arrived before the header");
            out("BLOCK-END %d count=%lld rows=%lld", block_no,
                result_count < 0 ? 0 : result_count, rows);
            return;
        }
        if (!in_results) {
            if (!strcmp(raw, "200-Query results")) {
                in_results = true;
                block_no++;
                out("BLOCK-BEGIN %d", block_no);
            } else if (strstr(raw, "200-Query results")) {
                proto_error("header line is not exactly '200-Query results': %s", raw);
            }
            continue;
        }

        /* every data line carries exactly one leading space on the wire */
        const char *l = raw;
        while (*l == ' ') l++;
        if (l == raw) proto_error("data line has no leading space: %s", raw);

        /* the column lines the client recognises, in its own order */
        if (!strncmp(l, "RESULT_COUNT ", 13)) {
            result_count = parse_ll(l + 13, -1);
            continue;
        }
        if (!strncmp(l, "PATH ", 5)) {
            snprintf(a_path, sizeof(a_path), "%s", l + 5);
            continue;
        }
        if (!strncmp(l, "ATTRIBUTES ", 11)) {
            a_attrib = (int)parse_ll(l + 11, -1);
            continue;
        }
        if (!strncmp(l, "SIZE ", 5)) {
            a_size = parse_ll(l + 5, a_size);
            continue;
        }
        if (!strncmp(l, "DATE_MODIFIED ", 14)) {
            a_dm = parse_ll(l + 14, a_dm);
            continue;
        }
        if (!strncmp(l, "DATE_CREATED ", 13)) {
            a_dc = parse_ll(l + 13, a_dc);
            continue;
        }
        if (!strncmp(l, "FILE_LIST_FILENAME ", 20)) {
            /* the client stores this as a string and never puts it in a result */
            continue;
        }
        if (!strncmp(l, "DATE_RECENTLY_CHANGED ", 22)) {
            a_drc = parse_ll(l + 22, a_drc);
            continue;
        }

        const char *name = NULL;
        bool is_dir = false;
        if (!strncmp(l, "FILE ", 5))        { name = l + 5; is_dir = false; }
        else if (!strncmp(l, "FOLDER ", 7)) { name = l + 7; is_dir = true; }

        if (name) {
            out("ROW %lld %s %s path=%s size=%lld dm=%lld dc=%lld drc=%lld attrib=%d",
                rows, is_dir ? "FOLDER" : "FILE", name,
                a_path, a_size, a_dm, a_dc, a_drc, a_attrib);
            rows++;
            /* reset the accumulators (the client resets there) */
            a_path[0] = '\0';
            a_size = a_dm = a_dc = a_drc = -1;
            a_attrib = -1;
            continue;
        }

        if (l[0] && strcmp(l, "200 End.") != 0) {
            /* the client falls off the end of its if-chain and drops the line;
             * report it so a server cannot hide a mistake here */
            proto_error("DROPPED %s", raw);
        }
    }
}

/* ------------------------------------------------------------- data channel
 *
 * The ETP client never uses one, but PASV/EPSV + LIST/MLSD/RETR have to work
 * for any other FTP client, and "the reply said 150" is not evidence that the
 * transfer happened. So the probe can open a real second connection:
 *
 *   send PASV          read the 227 and remember the port
 *   connectdata        connect to it (the server is already listening)
 *   send LIST -l <dir> read the 150
 *   data               drain the data socket to EOF, one DATA line per line
 *   recv               read the trailing 226
 */

static int g_data_fd = -1;

static void connect_data(int port)
{
    g_data_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_data_fd < 0) { perror("data socket"); return; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(g_data_fd, (struct sockaddr *)&a, sizeof(a)) != 0)
        perror("data connect");
}

/* "227 Entering Passive Mode (127,0,0,1,p1,p2)." -> p1*256+p2 */
static int parse_pasv(const char *line, bool extended)
{
    if (extended) {
        const char *bar = strchr(line, '(');
        if (!bar) return -1;
        const char *p = strrchr(bar, '|');
        if (!p) return -1;
        /* |||port| */
        const char *q = p;
        while (q > bar && *(q - 1) != '|') q--;
        return atoi(q);
    }
    const char *op = strchr(line, '(');
    if (!op) return -1;
    unsigned h[6];
    if (sscanf(op + 1, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &h[4], &h[5]) != 6)
        return -1;
    return (int)(h[4] * 256 + h[5]);
}

static void drain_data(void)
{
    if (g_data_fd < 0) { proto_error("no data connection"); return; }
    char buf[8192];
    for (;;) {
        ssize_t n = read(g_data_fd, buf, sizeof(buf) - 1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) break;
        buf[n] = '\0';
        for (char *line = buf; line && *line; ) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            size_t l = strlen(line);
            if (l && line[l - 1] == '\r') line[l - 1] = '\0';
            if (*line) out("DATA %s", line);
            line = nl ? nl + 1 : NULL;
        }
    }
    close(g_data_fd);
    g_data_fd = -1;
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <port> <script>\n", argv[0]);
        return 2;
    }
    int port = atoi(argv[1]);

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0) { perror("socket"); return 2; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(g_fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        perror("connect");
        return 2;
    }
    int one = 1;
    setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* the welcome is read with the client's own rule (the client's own rule) */
    if (!read_line()) { fprintf(stderr, "no welcome\n"); return 2; }
    out("WELCOME %s", g_line);
    if (strncmp(g_line, "220 ", 4) != 0)
        proto_error("welcome does not start with '220 '");

    FILE *f = fopen(argv[2], "r");
    if (!f) { perror("open script"); return 2; }

    char line[MAX_LINE];
    char last_reply[MAX_LINE] = "";
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = '\0';
        if (!l || line[0] == '#') continue;

        char *sp = strchr(line, ' ');
        const char *arg = sp ? sp + 1 : line + strlen(line);
        if (sp) *sp = '\0';

        if (!strcmp(line, "send")) {
            send_line(arg);
            if (!read_reply()) { proto_error("eof while reading a reply"); break; }
            snprintf(last_reply, sizeof(last_reply), "%s", g_line);
        } else if (!strcmp(line, "sendraw")) {
            send_line(arg);
        } else if (!strcmp(line, "recv")) {
            /* read a reply that was not consumed by the directive that caused it,
             * e.g. the trailing 226 after a transfer */
            if (!read_reply()) { proto_error("eof while reading a reply"); break; }
            snprintf(last_reply, sizeof(last_reply), "%s", g_line);
        } else if (!strcmp(line, "query")) {
            read_query_block();
        } else if (!strcmp(line, "connectdata")) {
            /* the port came from the 227/EPSV reply the previous `send` read */
            bool ext = strncmp(last_reply, "229", 3) == 0;
            if (!ext && strncmp(last_reply, "227", 3) != 0) {
                proto_error("connectdata after '%s'", last_reply);
            } else {
                int p = parse_pasv(last_reply, ext);
                if (p <= 0) proto_error("cannot parse the passive port from '%s'", last_reply);
                else { connect_data(p); out("DATA-CONNECTED %d", p); }
            }
        } else if (!strcmp(line, "data")) {
            drain_data();
        } else if (!strcmp(line, "sleep")) {
            /* Wait, on one connection, between two commands. The only reason this
             * exists: the result cache lives on the connection, so the only way to
             * observe that a server which updates its own index does not serve a stale
             * cached set is a session where the disk changes *between* two QUERYs. A
             * real client would be doing something else while it waited, and waiting is
             * part of what it does. Milliseconds, and clamped, because a suite that
             * hangs here would rather fail than stop. */
            long ms = strtol(arg, NULL, 10);
            if (ms < 0) ms = 0;
            if (ms > 60000) ms = 60000;
            struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        } else if (!strcmp(line, "close")) {
            break;
        } else {
            fprintf(stderr, "unknown directive: %s\n", line);
            fclose(f);
            return 2;
        }
    }
    fclose(f);
    close(g_fd);
    return g_errors ? 1 : 0;
}
