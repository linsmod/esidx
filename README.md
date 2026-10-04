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
make            # both flavours: -O2 (log level = warn) and
                 # -O0 -g + AddressSanitizer/UBSan (log level = debug)
make opt        # just the optimised build
make dbg        # just the sanitiser one
make check      # the gate: both suites against both builds
make clean
```

One `make` leaves the tree ready for everything, and it always builds both
flavours: the gate needs both, and a selector that could point it at one of them
is how it used to go wrong — the two shared object names, so switching meant
`make clean && make DEBUG=1`, and forgetting the clean ran the "sanitised" suite
against optimised objects. `etp-probe` and `order-ref` are built in both flavours
too; they are test peers, not user-facing tools, so `make install` installs only
`esidx`. Requires a Linux target (ext4), GCC or Clang with C11, and `make`.

## Test

Suites and harnesses, all runnable from a clean checkout:

```sh
make check             # the gate: both suites x both builds, ~30 s

./test.sh              # index and query correctness   (276 assertions)
./test_etp.sh          # protocol acceptance           (217 assertions)
make test-all          # both, in that order, optimised build only
ESIDX_BUILD=dbg ./test.sh        # the sanitiser build

./round.sh             # one full round on a real tree, with timings
./round.sh /usr        # ...on a bigger one

./cmp_ref.sh           # re-measure every value quoted against the :21 server
./ledger.sh /work      # the memory ledger + phase timings + the query shapes
./sortcmp.sh -n 3 base=/path/to/old/esidx mine=./esidx -t /usr
                      # per-sort-key cost for several builds at once
```

- **`test.sh`** pins the *index*, with the expected numbers taken from `find(1)`
  rather than hand-written, so it detects regressions in the index rather than in
  the test. It also carries a language suite against a flat
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
- **`ledger.sh`** is the same idea for memory: one command that builds a tree at
  `-v 3` and prints the per-structure ledger, the snapshot size and the query
  shapes, so every "MiB touched" figure below has a command that reproduces it. It
  found its own reason to exist the moment a `/work` number was quoted from a
  script in `/tmp` that the next reader could not re-run.

`make check` runs both suites against both builds, and the sanitiser build is part
of the gate rather than an extra step: it is the only thing that catches a
use-after-free or an uninitialised field that `-O2` hides. Both suites run entirely
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

A term's value may be up to **8 191 characters** — a long `ext:` list is the shape
that gets there, and it is answered whole. That is the length of the longest control
line the ETP server accepts; anything longer is refused, with a line in the log.

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
./esidx --verbose=2 query /usr.idx count:5     # 0=off 1=error 2=warn 3=info 4=debug 5=perf

./esidx -v 3 serve /usr.idx -p 2121            # per-query timings, served
./esidx -v 5 build /usr -o /tmp/u.idx          # + the walk split per syscall, and its price
```

`-v 5` splits the scan walk into `getdents64` / `fstatat` / `openat` / `esidx_add`, and
prints what the attribution itself cost. It has a level of its own because that is four
`clock_gettime` calls per entry: free against a TSC clocksource (20 ns, WSL2), ruinous
against HPET (1 222 ns, measured on `r7000`, where it made the split cost 36 % of the walk
it was splitting). The per-directory trace stays at exactly `-v 4`, so `-v 5` does not drag
651 894 `descend` lines along on a large tree.

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

| Tree | Entries | Scan | Finalize | Load | Snapshot | Peak RSS |
|---|---|---|---|---|---|---|
| `/etc` | 1 622 | 5.4 ms | 2.3 ms | 2.3 ms | 108 KB | 3 MiB |
| `/usr` | 116 888 | 4.20 s (27.8 k/s) | 110 ms | 125 ms | 7.8 MB | 33 MiB |

`finalize` is the sum of the nine derived indexes, and two of them are recent: the name
trigram index and the name rank together are 61 % of it. `./esidx -v 5 build` prints the
breakdown line by line, and design §10 has it per step.

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

Building it, on the same host:

| | `/usr` | `/work` = **5 476 485** entries, 506 GiB |
|---|---|---|
| scan (walk) | 1 206 ms | **55 653 ms** (98 k/s) |
| finalize | 286 ms | 5 194 ms |
| save | 27 ms | 600 ms |
| **build, total** | **1 492 ms** | **60 847 ms** |
| load | 327 ms | 5 664 ms |
| snapshot | 25.1 MiB | 414 MiB |
| peak rss | 82.2 MiB | **1 223 MiB** |

`find /work -xdev -printf '%y %b' | awk` — one `lstat` and one `readdir` per entry and
nothing else — takes **56.75 s** on the same tree, so the walk costs 2 % less than
`find` and the whole build, every derived index and a 414 MiB snapshot included, costs
7 % more than `find`'s single pass. Design §10 has the per-syscall split, and it says the
walk is 59 % `getdents64` (652 k calls, 392 B each, latency-bound) and 18 % `fstatat`
(1.81 µs a call, and no I/O at all, which is why "batch stat by inode" is not worth
building); the only lever left on that walk is D6's concurrency.

### Where the memory goes

`esidx -v 3 build <tree>` prints a per-structure ledger — allocated against used, one line
each — and it is the only way to tell a structure that is too big from one that is merely
sized by the wrong number. `./ledger.sh <tree>` prints it together with the snapshot size
and the query shapes, so a figure quoted from it can be re-measured with one command. On
`/work` (1 223 MiB peak, 1 092 MiB accounted):

| | touched | address | |
|---|---|---|---|
| name trigram lists | 318.6 MiB | 320.3 | 99 % of the posting-list capacity in use |
| entry columns | 282.0 | 282.0 | trimmed to the entry count, incl. the `nchild` aggregate |
| sorted arrays ×3 | 250.7 | 250.7 | 62.7 MiB of that is `sidx_ent_t` padding |
| names pool | 96.0 | 128.0 | 1 499 994 distinct basenames over 5 476 485 entries |
| dir vector headers | 16.0 | 16.0 | indexed by directory ordinal, not by id |
| eid → dir ordinal map | 6.4 | 6.4 | 2 097 152 slots for 630 472 directories |
| name rank | 63.8 | 122.6 | 1 493 203 distinct folded names |
| dir children vectors | 39.4 | 39.4 | 5 476 484 ids in 10 324 800 slots |
| ext index | 15.1 | 11.5 | 16 384 slots for 6 765 extensions |
| dir path hash | 5.0 | 16.0 | load factor 0.31 |

What is left, largest first: the names pool holds 3.65 copies of every name, the three
sorted arrays spend 62.7 MiB on `sidx_ent_t` padding, and the children vectors are at 53 %
occupancy. The extension pool's own lookup was a scan of every extension interned so far
until this release: 519 string compares per intern on `/work`, 2.19 G of them per build,
now 1.16 per intern — 5.8 s of user time on a build that is otherwise I/O-bound.

Query cost on `/usr`:

| Query | Candidates | eval | total |
|---|---|---|---|
| `parent:"/usr" folder:` — browse | 16 | 0.04 ms | 0.39 ms |
| `ext:conf` — search | 1 206 | 0.10 ms | 0.73 ms |
| `conf` — a bare word, the client's default | 372 084 → **8 484** | 2.7 ms | 3.8 ms |
| `path:/usr *.conf size:>1k` | 233 021 → **466** | 41.3 ms | 43.1 ms |
| `image:` — category | 55 229 | 0.71 ms | 21.6 ms |

One run of `./round.sh /usr` on `r7000`, so the counts are that host's `/usr` as it
stands: they move as packages come and go, and a count that differs from an older
table is the tree, not the code — `esidx`'s own candidate counts are cross-checked
against `find(1)` in `test.sh`.

The two arrows are the name trigram index (`trigram.c`, design §5.2): a *filter*,
not a decision — the trigram set of a pattern's longest literal run is a necessary
condition for a match, so intersecting the candidate set can only drop rows the
matcher was going to reject. It costs +90 ms of `finalize` (paid again at load and
at compaction, never on the query path) and ~20 MB.

The largest cost on a real tree is now the wildcard scan: `path:/usr *.conf size:>1k`
spends 41 of its 43 ms in `eval` over 233 021 candidates, which is the in-memory text
scan of design §5.2 and the reason the *path* half of that section is worth building.
`image:` is second, and 20.4 of its 21.6 ms is the sort over 55 229 rows — the client's
default `name_ascending`, an integer compare on the rank `finalize` builds. Design
§6.2's TopK is still not built; what it would attack is the comparison *count*, which
none of the sort work has touched.

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
ledger.sh      the memory ledger, the snapshot size and the query shapes
sortcmp.sh     per-sort-key cost for several builds at once (design §10)
```

## License

Not yet declared. The protocol baseline it
implements (`../etp_server-1.0.2.5/`) is MIT, © voidtools / David Carpenter.