# esidx

A search index engine for ext4, speaking Everything's query language and
Everything's FTP extension protocol.

Written in C11, no runtime dependencies beyond libc.

---

## What this is for

The ETP client browses and searches a desktop machine over **ETP** —
voidtools' Everything Server. ETP is FTP plus a `SITE EVERYTHING` extension
carrying 32 subcommands for match options, column toggles, sorting, paging and
execution. The server side is Everything itself, which is Windows-only.

esidx is the Linux replacement for that server's index. The client is unchanged:
it keeps talking ETP to a machine that happens to run Linux. Everything's search
language is preserved in full, so existing queries, saved searches and UI
behaviour carry over.

## Status

Under active development. The index engine, the query language and the ETP server
are all built and tested; **a client can connect and search today.** An index can
also be brought back in line with the filesystem without a rebuild, so it no longer
goes stale while a server runs. What is still missing is *immediacy*: freshness
comes from a periodic pass rather than from filesystem events.

| Phase | Scope | State |
|---|---|---|
| P0 | columnar store, directory tree, L0/L1 capability, sort, paging | done |
| P1 | FTP + `SITE EVERYTHING`, 32 subcommands, result cache | done |
| P2 | incremental collection: directory-mtime skip, fanotify | reconcile + mutation core done (`esidx update`, 0.1 ms idle on `/usr`); fanotify not started |
| P3 | full query-language parser: 40+ functions, 12 comparisons, modifiers, constants, macros | done |
| P4 | name/path trigram index, prefix/suffix search, CRoaring, query optimiser | name trigram index done (`trigram.c`, byte trigrams over the display name); driver selection done; path half, name-sorted/reversed arrays and CRoaring not started |
| P5 | content inverted index, sparse media metadata, `dupe:` | not started |

Phase boundaries, per-design rationale and the closed technical decisions are in
[`docs/design.md`](docs/design.md). §10 of that document maps every component to
a file, states what is outstanding, and carries the current measurements.

## Build

```sh
make            # -O2, log level = warn: builds esidx and etp-probe
make DEBUG=1    # -O0 -g + AddressSanitizer/UBSan, log level = debug
make clean
```

One `make` leaves the tree ready for both test suites — `etp-probe` is the
acceptance client, not a user-facing tool, so `make install` installs only
`esidx`. Requires a Linux target (ext4), GCC or Clang with C11, and `make`.

## Test

Suites and harnesses, all runnable from a clean checkout:

```sh
./test.sh              # index and query correctness   (219 assertions)
./test_etp.sh          # protocol acceptance           (213 assertions)
make test-all          # both, in that order

./round.sh             # one full round on a real tree, with timings
./round.sh /usr        # ...on a bigger one

./cmp_ref.sh           # re-measure every value quoted against the :21 server
./sortcmp.sh -n 3 base=/path/to/old/esidx mine=./esidx -t /usr
                      # per-sort-key cost for several builds at once
```

- **`test.sh`** pins the *index*, with the expected numbers taken from `find(1)`
  rather than hand-written, so it detects regressions in the index rather than in
  the test. It also carries a 106-assertion *language* suite against a flat
  fixture, because a syntax regression and an index regression look identical from
  the outside.
  `./test.sh -v` echoes every query; `TEST_ROOT=/usr ./test.sh` indexes a bigger
  tree (slower, ~5 s).
- **`test_etp.sh`** pins the *wire*, driven by `tools/etp_probe.c` — a
  transcription of a real ETP client's parsing rules, so "the probe understood the
  reply" means "a client would understand the reply", fragile cases included. Its
  ten acceptance criteria are listed at the top of the file, each traced to the
  observation or to the `etp_server.c` line it comes from. The same shapes also run
  against voidtools' own server on `127.0.0.1:21` via `etp-probe 21`, so the rules
  are not merely self-consistent.
- **`round.sh`** is not a test; it is the demonstration that the three layers fit
  together on data nobody curated, and it is where the numbers below come from.
- **`cmp_ref.sh`** re-measures every expected value quoted against voidtools' own
  server, and **`sortcmp.sh`** is its counterpart for the sort: it times every sort
  key for as many builds as you hand it on one tree. Both exist because the
  alternative is a number produced by a throwaway script — which is exactly how the
  sort numbers in design §10 were first produced.

Both suites pass under `make DEBUG=1` (ASan + UBSan, zero leaks) and run entirely
inside a `mktemp` directory, leaving the source tree clean.

## Usage

```sh
# index a tree
./esidx build /etc -o /etc.idx
#   indexed 1622 entries in 5.4 ms  (scan 5.4 ms, finalize 0.4 ms)

# browse: direct children of a directory
./esidx query /etc.idx 'parent:/etc/ssh'

# filter and sort
./esidx query /etc.idx 'ext:conf' 'size:>1k' 'sort:size:desc' 'count:5'

# page
./esidx query /etc.idx 'ext:conf' 'count:20' 'offset:40'

# bring an existing index back in line with the filesystem
./esidx update /etc.idx
#   updated /etc.idx: 1622 live entries (was 1622, +0), 91 dirs (90 skipped,
#   1 descended), 0 added, 0 removed, 90 refreshed in 0.3 ms
./esidx update /etc.idx --deep     # also re-stat every entry (size, mtime, ctime)

# serve ETP -- what the ETP client speaks
./esidx serve /etc.idx -p 2121
#   esidx serving 1622 entries from /etc.idx on 127.0.0.1:2121 (loaded in 0.3 ms)
```

`update` is the same walk in two modes. Without `--deep` it stats one directory
per changed subtree and notices name changes; on an unchanged `/usr` that is
15 stats and 0.1 ms. With `--deep` it stats every entry, which is what notices a
file whose *content* changed — that moves the file's own mtime and nothing its
parent can see — and costs 4.2 s on `/usr`. Both are safe to run repeatedly: a
pass that finds nothing writes nothing to any index.

### Supported query language

The parser covers the language in
[`docs/everything-syntax.md`](docs/everything-syntax.md): the operators
(juxtaposition, `|`, `!`, `<>` and `()` grouping), the 12 comparison forms,
`;`-separated and bracketed value lists, wildcards, regex, the 22 modifiers, the
size/date constants, and the type macros. Highlights:

| Syntax | Meaning | Index used |
|---|---|---|
| `parent:"<path>"` | direct children | children vector, O(1) |
| `root:` `depth:` `child:<expr>` | structural | inline column / nested search |
| `folder:` `file:` `ext:jpg;png` `attrib:h` `empty:` | type / enum | bitmaps |
| `size:>1M` `size:10mb..50mb` `dm:today` `dm:>7d` | ranges | sorted arrays |
| `image:` `video: type:document` | extension-set macros | ext bitmaps |
| `path:` `name:` `stem:` `whole:` `ww:` `startwith:` `regex:` | text | in-memory scan |
| `sort:` `count:` `offset:` | CLI sugar for the ETP sort/OFFSET/COUNT | — |
| `<word>` | case-insensitive substring | in-memory scan |

Functions Everything has and ext4 cannot answer — `content:`, `dupe:`,
`si:`, the media metadata — parse and return no results, with one warning, rather
than failing the query. `si:` has no Linux counterpart at all (design §12, risk 2).

> **Quote every query argument.** `size:>1k` unquoted is a shell redirection, not
> a query — esidx then receives the literal string `size:` and silently matches
> zero-byte files. The stray `0`, `100` and `1m` files this leaves in the working
> directory were removed from this repo after it happened here.

### Diagnostics

Logging is on stderr; results are on stdout, so they can be piped safely.

```sh
./esidx build /usr -o /usr.idx                 # one-line timing summary
ESIDX_LOG=info ./esidx build /usr -o /usr.idx  # per-phase timings + scan stats
ESIDX_LOG=debug ./esidx query /usr.idx count:5 # per-directory trace
./esidx -v query /usr.idx count:5              # same as ESIDX_LOG=debug
./esidx --verbose=2 query /usr.idx count:5     # 0=off 1=error 2=warn 3=info 4=debug

./esidx -v 3 serve /usr.idx -p 2121            # per-query timings, served
```

## Architecture

```
collection   full scan: getdents64 + d_type + openat + O_NOATIME
storage      columnar attribute table + name/ext string pool + snapshot
index        dir tree │ name trigrams │ numeric sorted arrays │ bitsets │ ext bitmaps
execution    driver selection → bitmap intersect → matcher pass → sort → slice
syntax       Lexer → Parser → AST → executor
protocol     FTP control + SITE EVERYTHING (bare or SITE-prefixed)
```

Single process, single-threaded, index resident in memory. Persistence exists
only to avoid a full rescan on restart; a snapshot is written on demand and loaded
at startup.

Four design points worth knowing before reading the code — the first two are where
the performance comes from, the last two are where the interoperability risk was:

- **`parent:` is O(1).** Each directory owns a vector of its children's entry ids,
  and a hash maps a full path to its directory id. Both upstream projects lack
  this; FSearch stores only a parent pointer, which forces a full scan to list a
  directory — unacceptable when every navigation in the UI issues `parent:`.
- **The query has a driver index.** Of all the leaves in a query, the cheapest
  index-backed one seeds the candidate bitmap, and every other leaf filters *that*
  rather than the table (design §6.2). A browse request therefore costs one
  directory listing, not a table scan. Measured below; the choice is logged.
- **The PATH column uses backslash separators** even though the index is POSIX.
  The client joins `path + "\" + name`, so a `/`-separated PATH would come back to
  us spelled differently and no longer hash to the same directory. Separators are
  normalised on the way in, which is what makes `parent:` a hit rather than a miss.
- **A directory's SIZE is the sentinel `18446744073709551615`**, not the real
  4096. Everything does not index folder sizes by default; a client parses that
  into a signed 64-bit field, overflows, swallows the exception, and shows
  no size — which is what we want, since a 4096 in the size column of every
  directory means nothing.

## Measured

WSL 2, Ubuntu 22.04, ext4, `-O2`, single thread. Reproduce with `./round.sh /usr`.

| Tree | Entries | Scan | Finalize | Load | Snapshot |
|---|---|---|---|---|---|
| `/etc` | 1 622 | 5.4 ms | 0.4 ms | 0.3 ms | 95 KB |
| `/usr` | 116 888 | 4.23 s (27.6 k/s) | 37 ms | 55 ms | 6.9 MB |

Query cost on `/usr`, end to end through the protocol:

| Query | Candidates | Total |
|---|---|---|
| `parent:"/usr" folder:` — browse | 14 / 116 888 | **0.33 ms** |
| `ext:conf` — search | 608 / 116 888 | 0.37 ms |
| `image:` — category | 13 569 / 116 888 | 5.4 ms |
| `path:/usr *.conf size:>1k` | 78 296 / 116 888 | 24.3 ms |

The candidate column is the whole story. Before the driver index, an unfiltered
query spent 42 of its 46 ms in `qsort` over 116 888 rows; a browse request now
costs a third of a millisecond because it walks 14 rows. The 24.3 ms outlier is a
wildcard scan over 78 296 candidates — the in-memory text scan of design §5.2.

### Measured on real hardware

WSL2 is ~10x optimistic (§2.4 of `AGENTS.md`), so the numbers that decide anything
are from the `r7000` host: Ubuntu 22.04, x86_64, ext4 on NVMe, 16 cores, `-O2`,
single thread, `/usr` = **372 084 entries**. Reproduce with `./round.sh /usr` there.

| Query | Candidates | eval | total |
|---|---|---|---|
| `parent:"/usr" folder:` — browse | 16 | 0.04 ms | 0.22 ms |
| `ext:conf` — search | 1 206 | 0.06 ms | 1.1 ms |
| `conf` — a bare word, the client's default | 372 084 → **8 625** | 6.5 ms | 12.0 ms |
| `path:/usr *.conf size:>1k` | 233 021 → **908** | 33.9 ms | 35.3 ms |
| `image:` — category | 55 229 | 1.1 ms | 55.7 ms |

The two arrows are the name trigram index (`trigram.c`, design §5.2): a *filter*,
not a decision — the trigram set of a pattern's longest literal run is a necessary
condition for a match, so intersecting the candidate set can only drop rows the
matcher was going to reject. It costs +90 ms of `finalize` (paid again at load and
at compaction, never on the query path) and ~20 MB.

`image:` is now the largest cost on a real tree and ~53 of its 55.7 ms is the sort
over 55 229 rows — design §6.2's TopK, still not built. A `path:` term is still a
full scan; that is the unbuilt path half of §5.2.

Staying current costs this, measured on the same tree:

| Pass | Idle | Worst case | What it costs |
|---|---|---|---|
| `update` | **0.1 ms** | 110 ms | one stat per directory whose parent changed |
| `update --deep` | 4.23 s | 4.23 s | one stat per entry |

A pass that finds nothing writes nothing to any index and does not move the index
epoch, so it is invisible to a connected client — the query costs above are the
query costs after a refresh.


## Documentation

| File | Contents |
|---|---|
| [`docs/design.md`](docs/design.md) | architecture, capability → index mapping, implementation status, decisions D1-D8, risks |
| [`docs/everything-syntax.md`](docs/everything-syntax.md) | the full Everything query language |
| [`docs/upstream-notes.md`](docs/upstream-notes.md) | what plocate and FSearch do, with `file:line` citations, and what we took from each |
| [`docs/research-log.txt`](docs/research-log.txt) | raw research log, kept for provenance only |

## Environment

The upstream trees the survey cites live in WSL, not in this repository:

| Path | Contents |
|---|---|
| `\\wsl$\Ubuntu-22.04\root\src-dl\plocate-1.1.15` | plocate 1.1.15 (`apt-get source plocate`) |
| `\\wsl$\Ubuntu-22.04\root\src-dl\fsearch` | FSearch, master `d531eb3` (git clone; not packaged for Ubuntu) |

The WSL default user needs a password for `sudo`; those trees were fetched with
`-u root`, and `/root` is mode 700, so reading them also needs `-u root`.

The protocol baseline is in-repo at `../etp_server-1.0.2.5/`
(voidtools' Everything Server 1.0.2.5, MIT). Citations of the form
`etp_server.c:3951` refer to `../etp_server-1.0.2.5/src/etp_server.c`.

## Layout

```
esidx.h        public storage types and API
syntax.h       AST, parser, ETP match options, executor and regex contracts
etp.h          ETP server options
store.c        string pool, columnar table, directory tree, ext/type bitmaps, mutation, snapshot
trigram.c      name trigram inverted index (design §5.2): build / add / filter
scan.c         full scan, incremental reconcile
index.c        dense bitset and set algebra
lexer.c        the Everything token rules
parser.c       tokens -> AST
regex.c        backtracking regex + Everything wildcards
query.c        matcher table, driver selection, two-stage execution, sorting
etp.c          FTP control + SITE EVERYTHING
main.c         CLI: build / update / query / serve
log.c log.h    leveled logging, runtime-switchable
timer.h        monotonic phase timing helpers
tools/etp_probe.c   the acceptance client (built by `make`, and by `make etp-probe`)
tools/order_ref.c  the sort-order oracle: `strcasecmp`, in C, because `sort -f` is not
test.sh        index and language suite
test_etp.sh    protocol acceptance suite
round.sh       one full round, with timings
cmp_ref.sh     re-measures every number quoted against the reference server
sortcmp.sh     per-sort-key cost for several builds at once (design §10)
```

## License

Not yet declared. The protocol baseline it
implements (`../etp_server-1.0.2.5/`) is MIT, © voidtools / David Carpenter.