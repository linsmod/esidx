# esidx

A search index engine for ext4, speaking Everything's query language and
Everything's FTP extension protocol.

Written in C11, no runtime dependencies beyond libc.

---

## What this is for

ShareToPC's Android client browses and searches a desktop machine over **ETP** —
voidtools' Everything Server. ETP is FTP plus a `SITE EVERYTHING` extension
carrying 32 subcommands for match options, column toggles, sorting, paging and
execution. The server side is Everything itself, which is Windows-only.

esidx is the Linux replacement for that server's index. The client is unchanged:
it keeps talking ETP to a machine that happens to run Linux. Everything's search
language is preserved in full, so existing queries, saved searches and UI
behaviour carry over.

## Status

Under active development. The index engine works and is tested; the network
service does not exist yet, so **no client can connect to it today**.

| Phase | Scope | State |
|---|---|---|
| P0 | columnar store, directory tree, L0/L1 capability, sort, paging | index core done; protocol acceptance not met |
| P1 | FTP + `SITE EVERYTHING`, 32 subcommands, result cache | not started |
| P2 | incremental collection: directory-mtime skip, fanotify | full scan done, incremental not started |
| P3 | full query-language parser: 40+ functions, 12 comparisons, modifiers, constants, macros | not started |
| P4 | name/path trigram index, prefix/suffix search, CRoaring, query optimiser | not started |
| P5 | content inverted index, sparse media metadata, `dupe:` | not started |

Phase boundaries, per-design rationale and the closed technical decisions are in
[`docs/design.md`](docs/design.md). §10 of that document maps every component to
a file and states what is outstanding.

## Build

```sh
make            # -O2, log level = warn
make DEBUG=1    # -O0 -g + AddressSanitizer/UBSan, log level = debug
make clean
```

Requires a Linux target (ext4), GCC or Clang with C11, and `make`.

## Test

```sh
./test.sh           # or: make test
./test.sh -v        # echo every query and its diagnostics
TEST_ROOT=/usr ./test.sh   # index a bigger tree (slower, ~10 s)
```

47 assertions covering the directory tree, extension and size filtering,
substring search, all five sort keys, offset/count paging, error paths,
deep recursion (251 levels), and the logging switches. Correctness is pinned
against `find(1)` rather than against hand-written expectations, so the suite
detects regressions in the index rather than in the test.

The suite runs entirely inside a `mktemp` directory; the source tree is left
clean. It also passes under `make DEBUG=1` (ASan + UBSan).

## Usage

```sh
# index a tree
./esidx build /etc -o /etc.idx
#   indexed 1617 entries in 5.6 ms  (scan 5.2 ms, finalize 0.4 ms)

# browse: direct children of a directory
./esidx query /etc.idx "parent:/etc/ssh"

# filter and sort
./esidx query /etc.idx "ext:conf" "size:>1k" "sort:size:desc" "count:5"

# page
./esidx query /etc.idx "ext:conf" "count:20" "offset:40"
```

### Supported query subset

| Syntax | Meaning |
|---|---|
| `parent:"<path>"` | direct children of `<path>` |
| `folder:` / `file:` | type filter |
| `ext:jpg;png` | extension filter, `;`-separated |
| `size:>1M`, `size:100..2M`, `size:<1k` | size range |
| `dm:today`, `dm:>7d` | modification date |
| `sort:name\|path\|size\|mtime\|ext:asc\|desc` | sort key and direction |
| `count:N`, `offset:N` | paging |
| `<word>` | case-insensitive substring match on the name |

The full Everything language — operators, macros, 22 modifiers, 40+ functions —
is specified in [`docs/everything-syntax.md`](docs/everything-syntax.md) and
targeted by P3. Until then, an unrecognised token is treated as a substring to
search for, which is also how Everything treats a bare word.

### Diagnostics

Logging is on stderr; results are on stdout, so they can be piped safely.

```sh
./esidx build /usr -o /usr.idx                 # one-line timing summary
ESIDX_LOG=info ./esidx build /usr -o /usr.idx  # per-phase timings + scan stats
ESIDX_LOG=debug ./esidx query /usr.idx "ext:conf"   # per-directory trace
./esidx -v query /usr.idx count:5              # same as ESIDX_LOG=debug
./esidx --verbose=2 query /usr.idx count:5     # 0=off 1=error 2=warn 3=info 4=debug
```

> **Quote every query argument.** `size:>1k` unquoted is a shell redirection, not
> a query — esidx then receives the literal string `size:` and silently matches
> zero-byte files. The stray `0`, `100` and `1m` files this leaves in the working
> directory were removed from this repo after it happened here.

## Architecture

```
collection   full scan: getdents64 + d_type + openat + O_NOATIME
storage      columnar attribute table + name/ext string pool + snapshot
index        dir tree │ numeric sorted arrays │ dense bitsets
execution    candidate set → bitmap intersect → matcher pass → sort → slice
protocol     FTP + SITE EVERYTHING                    (P1)
syntax       Lexer → Parser → AST                     (P3)
optimiser    selectivity estimate → driver index      (P4)
```

Single process, single-threaded today, index resident in memory. Persistence
exists only to avoid a full rescan on restart; a snapshot is written on exit and
on a timer.

Two design points worth knowing before reading the code:

- **`parent:` is O(1).** Each directory owns a vector of its children's entry
  ids, and a hash maps a full path to its directory id. Both upstream projects
  lack this; FSearch stores only a parent pointer, which forces a full scan to
  list a directory — unacceptable when every navigation in the UI issues
  `parent:`.
- **Numeric filtering runs off sorted arrays, not scans.** Each numeric column
  keeps an ascending `(value, entry_id)` array; a range query binary-searches
  its bounds into a bitmap, which the matcher pass then intersects. Neither
  upstream project indexes structured filters at all.

See [`docs/design.md`](docs/design.md) for the full rationale, the capability →
index mapping, the reference tables with `file:line` citations, and the closed
decisions (D1-D7).

## Measured

WSL 2, Ubuntu 22.04, ext4, `-O2`, single thread:

| Tree | Entries | Scan | Load | Unfiltered query | Snapshot |
|---|---|---|---|---|---|
| `/etc` | 1 617 | 5-60 ms | 0.4 ms | < 1 ms | 95 KB |
| `/usr` | 116 888 | 5.2 s (22 k/s) | 38 ms | 46 ms | 7.3 MB |

The `/usr` numbers are the reason the optimiser is on the roadmap: an unfiltered
query spends 42 of its 46 ms in `qsort` over 116 888 rows.

## Documentation

| File | Contents |
|---|---|
| [`docs/design.md`](docs/design.md) | architecture, capability → index mapping, implementation status, decisions D1-D7, risks |
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
esidx.h        public types and API
store.c        string pool, columnar table, directory tree, snapshot
scan.c         full scan
index.c        dense bitset
query.c        query execution
main.c         CLI
log.c log.h    leveled logging, runtime-switchable
timer.h        monotonic phase timing helpers
test.sh        regression suite
```

## License

Not yet declared. Intended for the ShareToPC project; the protocol baseline it
implements (`../etp_server-1.0.2.5/`) is MIT, © voidtools / David Carpenter.