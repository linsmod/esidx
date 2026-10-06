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
also be brought back in line with the filesystem without a rebuild, and a server
can do it to itself while it answers (`esidx serve --refresh=5`), so it no longer
goes stale while a server runs. Freshness can also be *immediate*:
`esidx serve --watch` subscribes to `sfa`, a separate small privileged proxy that
holds the fanotify group and rebroadcasts events as absolute paths, so a name
change is visible within one event batch instead of one `--refresh` interval — and
esidx itself needs no privilege, because the proxy is where that lives. The same
watcher can run *inside* this server instead (`--watch-embed`),
which opens the group itself and gives the privilege back before it answers
anything; one process, one fd, no socket in between. Both forms deliver the same
events through the same code, and the choice is about deployment, not semantics:
the split keeps the privilege in a process that answers nothing, the embedded form
keeps the tree and the index in one place to operate.

What is still missing is *attribute* immediacy: size and mtime still follow
`--deep`, because listing a directory cannot see them. And the proxy is not the
whole answer on its own — it drops events it cannot turn back into a path, so a
sweep that compares every directory's stamp (`serve --sweep=SECS`, and once at
startup) is what makes "up to date" true rather than nearly true.

| Phase | Scope | State |
|---|---|---|
| P0 | columnar store, directory tree, L0/L1 capability, sort, paging | done |
| P1 | FTP + `SITE EVERYTHING`, 32 subcommands, result cache | done |
| P2 | incremental collection: directory-mtime skip, fanotify | reconcile + mutation core done (`esidx update`, 0.1 ms idle on `/usr`), the serving process reconciles in place (`serve --refresh`), events arrive through the `sfa` proxy (`serve --watch`), and `serve --sweep=SECS` closes what a stamp-pruned pass cannot reach; attribute freshness still needs `--deep` |
| P3 | full query-language parser: 40+ functions, 12 comparisons, modifiers, constants, macros | done |
| P4 | name/path trigram index, prefix/suffix search, CRoaring, query optimiser | name trigram index done (`trigram.c`, byte trigrams over the display name); driver selection done; path half, name-sorted/reversed arrays and CRoaring not started |
| P5 | content inverted index, sparse media metadata, `dupe:` | not started |

Phase boundaries, per-design rationale and the closed technical decisions are in
[`docs/design.md`](docs/design.md). §10 of that document maps every component to
a file, states what is outstanding, and carries the current measurements.

## Build

```sh
git clone --recurse-submodules https://github.com/linsmod/esidx   # or:
git submodule update --init --recursive                           # in an existing clone

make            # both flavours: -O2 (log level = warn) and
                 # -O0 -g + AddressSanitizer/UBSan (log level = debug)
make opt        # just the optimised build
make dbg        # just the sanitiser one
make check      # the gate: both suites against both builds
make clean
```

**The `sfa` submodule is a dependency, not an option.** It is the privileged half of the
event source (`--watch`), its pointer is committed in the tree, and the build stops with one
line if `sfa/sfa.h` is missing rather than handing back a server that has no watcher and
does not say so. `make clean` still works without it — nothing that compiles needs it.

### Deploying somewhere else

```sh
make dist            # dist/esidx-<version>.tar.gz + .sha256
make dist-verify     # unpack it elsewhere, check it against both commits, build, run the suite
make deb             # dist/esidx_<version>_<arch>.deb -- binaries, systemd units, /etc/default
make deb-verify      # unpack the .deb and run what came out of it
```

`make dist` is the whole deployment in one file: esidx's sources, the **sfa submodule's
sources** (which a gitlink cannot carry — that is the reason the target exists), and
`install.sh`. Two properties it holds that a hand-rolled package does not:

- **The file list and the modes come from the two commits**, not from the build tree. This
  project is developed on a 9p/v9fs mount where `chmod` is a no-op and every file looks like
  `0777`, so a `cp`-based package would ship executable documentation and a non-executable
  `install.sh`. Both happened here before the modes were taken from git.
- **`make dist-verify` proves the package rather than trusting it**: it unpacks somewhere
  else, diffs the contents and every mode against `git ls-files -s`, builds it, and runs the
  watcher suite from the unpacked tree. It checks the suite's exit status instead of
  tailing its output, which is not a detail — the first version of it printed "passes" over a
  suite that had died with `Permission denied`.

`make deb` is the other deployment story: a package of **binaries** rather than sources, so
the target needs `dpkg` and not a compiler. It installs what `install.sh` installs by calling
the same two Makefile install targets — a file list written out a second time in a Makefile
would be one that eventually differs from the one in `install.sh` — and adds the two things a
tarball cannot carry:

- the **systemd unit**, which is now in the tree (`packaging/systemd/esidx.service`) rather
  than written on the machine it runs on. It starts the server as root with
  `--watch-embed --drop-to`, which is the single-process form: the fanotify group is opened
  and the privilege is handed back before the listener opens. `User=` is deliberately absent —
  it would either start the process unprivileged (and it cannot open a marked group) or leave
  it root for its whole life, which is the two-process answer and not this one. The unit is
  also where the capability ceiling is stated, narrower than "root", and
- the **maintainer scripts**, which create the `esidx` system user, its group, and
  `/var/lib/esidx`, retire the two units an earlier package installed, and enable this one.

It deliberately does not start it: the server refuses to run without a snapshot
(`main.c:247`), and building the first one is a full scan of `ESIDX_ROOT`, which does not
belong in a maintainer script. So `postinst` prints the two commands.

Two things the package decides, both in `/etc/default/esidx` rather than in the unit: the
bind address, and `RETR` — which returns file *contents* — is refused by the unit, so the
default is a search service and not an anonymous file server. The bind default is `0.0.0.0`,
because a service whose job is to answer searches from other machines cannot only answer on
its own host — and that is safe because the access control travels with the unit
(`IPAddressDeny=any` plus loopback and the private ranges, enforced by the kernel per unit)
rather than being left to a firewall someone has to remember. ETP clients here authenticate
anonymously, so that address list is the only control there is; set `ESIDX_BIND=127.0.0.1` to
take the service off the network instead. One process reads the whole file, and the tree it
marks comes from the snapshot itself (`--watch-embed`, no argument), so there is no second
tree to disagree with it.

**Updating** is `dpkg -i` (or `apt install ./esidx_*.deb`) over the installed package. The
unit and the binaries are replaced, `postinst` notices the service is running and restarts it
onto the new binary, and the snapshot is kept — an upgrade never rescans the tree, which is
what makes this the cheap operation rather than the install. The price is the restart itself:
the index is served from memory, so nothing answers while the snapshot loads (8.8 s for
`/work`'s 5 476 485 entries, measured on `r7000`). Without that restart an update would
install cleanly and the old process would keep answering — `Restart=always` is about the
process exiting, not about the file changing. A local edit to `/etc/default/esidx` is a
conffile edit, so dpkg wants a policy for it: with a terminal it asks, and **without one it
fails** — measured on `r7000`, where a non-interactive `dpkg -i` died on
`在 conffile 提示时读取标准输入时遭遇 EOF` and left the package unpacked. So an unattended
upgrade states the policy itself:

```sh
dpkg -i --force-confold esidx_*.deb          # keep what is on this machine
DEBIAN_FRONTEND=noninteractive dpkg -i esidx_*.deb   # same, and never prompt
apt install -o Dpkg::Options::=--force-confold ./esidx_*.deb
```

`--force-confnew` takes the package's file instead and discards the local values. Note what
triggers the prompt: dpkg compares whole files, so a change to *comments* in the shipped
`esidx.default` is enough — there is no "comments only" case.

**One snapshot, one server.** `serve` takes an advisory lock on `<snapshot>.lock` and refuses
to start when another process holds it, naming that pid. Two servers on one snapshot would each
answer from their own copy of the index and apply their own events, so the two answers would
drift apart with every change under the tree — and nothing on either side would report it. An
`flock` rather than a pid file, because the kernel releases it even after `kill -9`: a stale pid
file is how a service ends up refusing to start after a reboot, and the lock's job is to prevent
a second server, not to survive a crash. `build`, `update` and `query` are one-shot and do not
take it — which is deliberate and is also the limit of what it covers: rebuilding a snapshot
that a server has loaded is the operator's call. The server keeps answering from the copy in
memory, and a `--save` would write that copy back over the rebuild, so rebuild and then restart
(which is what an upgrade does), or build to another path.

The lock lives beside the snapshot, which makes its permissions part of the deployment: it is
created **before** the server gives its privilege back, and the unit's capability set does not
include `CAP_DAC_OVERRIDE` — so until it drops, the process is uid 0 that cannot ignore file
modes, and a directory owned by someone else at 0755 refuses it. Hence `/var/lib/esidx` is
`esidx:esidx` **2775** (package-owned, setgid, group-writable), the unit runs `Group=esidx`, and
`postinst` puts `ESIDX_USER` and the installing user in that group: three identities write in
there — the process before the drop, the user it becomes, and the admin building snapshots — and
a directory can belong to only one of them. The failure this replaces is worth recognising:
`serve: cannot open the lock file …` at startup on a directory that looks perfectly writable on
a shell, because an ordinary root shell *does* have `CAP_DAC_OVERRIDE` and the service does not.

`make deb-verify` unpacks the package and **runs what came out of it**: the packaged server
builds a snapshot, the packaged proxy probes fanotify, the unit's `ExecStart` names a binary
the package actually installs (a unit pointing at `/usr/local/bin` while the package installs
`/usr/bin` is a valid unit and a broken deployment), and `systemd-analyze` has nothing to say
about it beyond the missing-command lines it necessarily reports outside the install prefix.

On the target:

```sh
tar xzf esidx-<version>.tar.gz && cd esidx-<version>
./install.sh --start --root /work              # build, install, and run it
./install.sh --start --root /work --split      # the two-process form instead
./install.sh --start --root /work --prefix ~/.local    # same, no root for the install
./install.sh --stop
```

`install.sh` builds from sources and starts the **one-process** form, the same shape the
package runs: one server that opens the fanotify group as root, gives it back before it opens
the listener, and keeps only `CAP_DAC_READ_SEARCH`. `--split` asks for the two-process form
instead — the proxy as root, the index server as your user, and the socket between them
chgrp'ed to a group you name (`--group`, default: your own) — for a deployment where the index
server must not even *start* with a capability; `--watch=SOCK` stays available for the same
reason. `--drop-to=USER` (default: you, which is who built the snapshot) names the identity the
one-process server keeps, and `--group`/`--socket` are refused without `--split` rather than
ignored. `install.sh` handles the three things that made doing it by hand annoying: the
submodule is already inside the package (the embedded watcher is sfa's client code, compiled
into `esidx`), it refuses to continue rather than half-installing, and the split form's socket
group is arranged for you. Without `--start` it installs and prints the commands. Logs land in
`--logdir` (default `/tmp/esidx-logs`).

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

./test.sh              # index and query correctness   (337 assertions)
./test_etp.sh          # protocol acceptance           (229 assertions)
make test-all          # both, in that order, optimised build only
ESIDX_BUILD=dbg ./test.sh        # the sanitiser build

./round.sh             # one full round on a real tree, with timings
./round.sh /usr        # ...on a bigger one

./cmp_ref.sh           # re-measure every value quoted against the :21 server
./refresh.sh /usr      # what a refresh costs: idle pass, adding pass, snapshot write
./refresh.sh /work --add 2000
./ledger.sh /work      # the memory ledger + phase timings + the query shapes
./tri-skip.sh /usr     # what each derived index costs to skip, and what it buys
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
- **`tri-skip.sh`** is the same idea for the *derived indexes*: it answers the same
  queries against the same snapshot twice, once with a derived index built and once
  without, and prints the matched row count from both sides on every row — so a
  configuration that is faster *and* wrong cannot pass for a trade. What each index
  costs to skip is in design §5.3.1.

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
./esidx update /etc.idx --dir /etc/ssl # refresh only that directory, repeatable
#   updated /etc.idx: ... --dir resolves against the index and refuses anything else

# serve ETP -- what the ETP client speaks. 127.0.0.1:2121 is the default bind
./esidx serve /etc.idx
#   esidx serving 1622 entries from /etc.idx on 127.0.0.1:2121 (loaded in 0.3 ms)

# ...and keep that index current while serving it
./esidx serve /etc.idx --refresh=5 --save=300
#   esidx: reconciling in place every 5 s, snapshot written every 300 s

# ...or react to filesystem events as they happen
sudo ./sfa/sfa-server /                       # the privileged half, once
./esidx serve /etc.idx --watch --sweep=3600
#   esidx: watching /etc via /run/sfa.sock -- name changes become visible within one
#          batch; size/mtime still follow --refresh/--deep; an event the proxy cannot
#          place costs a full pass

# ...or with no proxy at all: this process opens the fanotify group and becomes esidx.
# The tree to mark defaults to the snapshot's root and the user to become defaults to
# the snapshot's owner, so --watch-embed needs no arguments.
sudo ./esidx serve /etc.idx --watch-embed --sweep=3600
#   watch: --drop-to not given: becoming esidx, the owner of /etc.idx
#          (pass --drop-to=USER to choose another)
#   watch: dropped to esidx (uid 998 gid 998); kept CAP_DAC_READ_SEARCH for event path
#          resolution, dropped the rest
#   esidx: watching /etc via the embedded fanotify group (this process), as esidx [(embedded)]
```

The banner names the user the process became, and not only the log line that reports the
drop: the drop is at INFO, the default level is higher, and "which identity is this service
running as" is not something an operator should have to raise the log level to find out.

`update` is the same walk in two modes. Without `--deep` it stats one directory
per changed subtree and notices name changes; on an unchanged `/usr` that is
15 stats and 0.1 ms. With `--deep` it stats every entry, which is what notices a
file whose *content* changed — that moves the file's own mtime and nothing its
parent can see — and costs 4.2 s on `/usr`. Both are safe to run repeatedly: a
pass that finds nothing writes nothing to any index.

**And one more mode, because the cheap pass has a reach worth knowing.** The names pass
descends into a directory only when that directory's own mtime moved, and it reaches a
directory only through its parent — so a file created or renamed at depth ≥ 2 moves the
stamp of the directory holding the name and nothing above it, and the pass never looks.
`--sweep` compares the stamp of *every* indexed directory instead, which is the only way
to ask that question, and costs one stat per directory: 77 ms on `/usr`, 1.6 s on `/work`
(both measured on real hardware). So it runs in exactly three places — once at startup,
whenever the event proxy reports that it lost events, and on `--sweep=SECS` if you ask for
it — and never as the default, because it would turn a 0.1 ms timer into a 1.6 s one to
fix a case that only a stopped daemon or a lossy proxy produces.

`serve --refresh=SECS` is that same names pass, run by the serving process: once
before the listener, then every SECS. The interval is a policy choice about
staleness; what it costs is in [design §10](docs/design.md) and `./refresh.sh`
measures it. An idle pass is 0.9 ms even over 5.5 M entries, because the walk
stops at the first unchanged directory stamp. A pass that *adds* rows used to pay
two O(n) rebuilds — a name rank and a children array, neither of which can be
appended to in the middle — which cost 2.38 s per pass on `/work` for any number of
new files; both are now drained on a threshold, and the same measurement is
**0.40 s**, of which 389 ms is the name-intern table's one-time rebuild on the first
add after a load. `--save=SECS` writes the snapshot on a timer and on a clean exit,
and skips the write entirely when nothing changed.

`update --dir PATH` refreshes only that directory (repeatable), which is the
**dirty set** of [design §7](docs/design.md) spelled on a command line: the same
reconcile, with a smaller set of directories to list. It is not a millisecond win on
an idle tree — a full pass is already 0.1-0.2 ms because it skips unchanged
subtrees, and naming one directory can cost *more* — it is the mechanism a
filesystem watcher will feed instead of pulling from the root, and the case it
actually serves is a change no stamp can see (§12 risk 8).

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

`folder:`, `file:`, the type macros, `root:` and `empty:` may be followed
**directly** by a term: `folder:abc` is that filter *and* the term `abc`, while
`folder:` alone is just the filter. The reference settles it and says so in
measurements — `folder:zzzznotfound` is 0 where `folder:` answers 1 594 989 — see
`docs/everything-syntax.md` ("what this document does not settle") and the rows
`cmp_ref.sh` re-measures.

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

### Choosing which derived indexes to build

The derived indexes are not in the snapshot (design D4), so which ones exist is a
decision about one process, not about the file — and a snapshot written by a process
that built everything answers queries for a process that built less. In fact the file is
**byte-identical** either way: on `/work`, `-o out.idx` gives 313 011 674 bytes and the
same md5 with every index on and with all five off.

```sh
./esidx options /work.idx                 # what would be left out, and why
./esidx options /work.idx --no-index=rank # ...if the flag were given

./esidx serve /work.idx --no-index=size,mtime,ctime    # this process builds fewer
./esidx serve /work.idx                                 # and this one does not
```

Three sources, and the order is the argument rather than an accident:

| source | example | |
|---|---|---|
| `--no-index[=LIST]` | `--no-index=trigram,rank` | this invocation's decision, so it wins |
| `<dbfile>.opts` | one name per line, `#` comments | the file's, so `x.idx` and `y.idx` can differ |
| `ESIDX_SKIP_INDEX` | `size,mtime,ctime` | the lowest; what the tests use |

`--no-index` with no value means "read the sidecar"; `--no-index=` means "ignore it and
build everything", which is only expressible because an empty list is a setting rather
than a missing argument. `-v 3` prints what was left out and which source decided it, and
the leaf that had to scan instead says so per query at `-v 4`.

**Every answer is identical either way** — that is an assertion in `test.sh`, not a
claim: the same snapshot, six queries, three configurations, identical rows *and*
identical paths. On `/work`:

| `--no-index=` | accounted | peak rss | saved |
|---|---|---|---|
| *(nothing: all five)* | 789.2 MiB | 862.1 MiB | |
| `size,mtime,ctime` | 601.2 | 669.4 | 188.0 MiB |
| `trigram` | 687.4 | 761.2 | 101.8 |
| `rank` | 725.4 | 749.2 | 63.8 |
| `trigram,rank` | 623.6 | 664.5 | 165.6 |
| `size,mtime,ctime,trigram,rank` | **435.6** | **455.2** | **353.6** |

**47 % of peak rss, and not one answer changed.** What it costs is in design §5.3.1, and
the short version is that `size:>10mb` is 10.5x slower without `by_size` while `size:>1k`
is free without it — a range index is worth its memory where the range is selective and
nowhere else, which is the trade this option exists to let somebody make.

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
| scan (walk) | 1 206 ms | **53 143 ms** (103 k/s) |
| finalize | 286 ms | 6 313 ms |
| save | 27 ms | 600 ms |
| **build, total** | **1 492 ms** | **59 456 ms** |
| load | 327 ms | 5 664 ms |
| snapshot | 20.8 MiB | 298 MiB |
| peak rss | 74.2 MiB | **862 MiB** |

The `/work` column is one run of `./ledger.sh /work`, so its walk figure moves with the
page cache by several seconds between runs — 53.1 s here against the 55.7 s the same
command printed before this change. The byte counts and the ledger below do not move: they
are what the code does to a tree of that shape, not what the disk was doing.

`find /work -xdev -printf '%y %b' | awk` — one `lstat` and one `readdir` per entry and
nothing else — takes **56.75 s** on the same tree, so the walk costs 6 % less than
`find` and the whole build, every derived index and a 298 MiB snapshot included, costs
5 % more than `find`'s single pass. Design §10 has the per-syscall split, and it says the
walk is 59 % `getdents64` (652 k calls, 392 B each, latency-bound) and 18 % `fstatat`
(1.81 µs a call, and no I/O at all, which is why "batch stat by inode" is not worth
building); the only lever left on that walk is D6's concurrency.

### Where the memory goes

`esidx -v 3 build <tree>` prints a per-structure ledger — allocated against used, one line
each — and it is the only way to tell a structure that is too big from one that is merely
sized by the wrong number. `./ledger.sh <tree>` prints it together with the snapshot size
and the query shapes, so a figure quoted from it can be re-measured with one command. On
`/work` (peak rss, **789 MiB accounted** — the total includes the 56.6 MiB of directory
paths the ledger spent two commits displaying without counting):

| | touched | address | |
|---|---|---|---|
| name trigram lists | 101.8 MiB | 104.1 | 83 507 488 postings, 1.28 bytes each: delta-varint, was 318.6 |
| entry columns | 282.0 | 282.0 | trimmed to the entry count, incl. the `nchild` aggregate |
| sorted arrays ×3 | 188.0 | 188.0 | 16 429 455 rows at 12 bytes, no padding |
| names pool | 37.3 | 64.0 | one copy per distinct name: 1 499 995 of them |
| name intern table | 5.7 | 8.0 | 2 097 152 slots for those names |
| dir range table | 8.0 | 8.0 | a start and a count per directory ordinal |
| dir children array | 20.9 | 20.9 | 5 476 484 ids in 5 476 484 slots — one allocation |
| eid → dir ordinal map | 6.4 | 6.4 | 2 097 152 slots for 630 472 directories |
| name rank | 63.8 | 122.6 | 1 493 203 distinct folded names |
| ext index | 15.1 | 15.5 | 16 384 slots for 6 765 extensions |
| dir path hash | 5.0 | 16.0 | load factor 0.31 |
| dir paths pool | 56.6 | 64.0 | 651 897 whole paths, 91 bytes a directory |

What is left, largest first: 56.6 MiB of directory paths sit in a pool at all when the
parent chain already rebuilds them. Two items that were on this list a few commits ago are
not — the trigram posting lists, which were 318.6 MiB and are 101.8 because the ids inside
a list ascend and so are stored as varint gaps, and the names pool's 96 MiB of duplicated
names (an intern table beside it). What they bought is in the table above, and the numbers
to re-run them with are in `docs/design.md` §10. The extension pool's own lookup was a scan
of every extension interned so far until recently: 519 string compares per intern on
`/work`, 2.19 G of them per build, now 1.16 per intern — 5.8 s of user time on a build
that is otherwise I/O-bound.

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
at compaction, never on the query path) and 5.9 MiB — the postings are delta-varint
encoded, which is 18.4 MiB as a plain array and 3.15x smaller for no measurable
query cost.

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
| `serve --refresh=SECS`, idle | **0.2-0.9 ms** (`/work`: 5.5 M entries, 48 dirs skipped) | one getdents of the root's own listing | proportional to the root's fanout, not to the tree |
| `serve --refresh=SECS`, adding ~2000 rows | — | **0.40 s** on `/work` (389 ms one-time name-intern rebuild + 12 ms of work); 2.38 s before the two overlays | the O(n) rebuilds are drained on a threshold, not skipped |
| `serve --save=SECS` | nothing written when nothing changed | 538-563 ms for a 313 MB snapshot | the whole file; no derived structure is persisted |
| `serve --watch` (event proxy) | one `poll()` wake per event, ~50 ms coalescing window | 1017 events → 932 marks → **6 directory listings, ~6 ms** for 1000 files written at once | one stat per directory the events named, de-duplicated per batch |
| `--sweep` (every directory's stamp) | 77 ms `/usr`, **1.6 s** `/work` (`r7000`, 651 896 dirs, 2.45 µs each) | same — it is linear in directories and nothing else | the price of knowing about a change at depth ≥ 2, which the pruned pass cannot reach |

A pass that finds nothing writes nothing to any index and does not move the index
epoch, so it is invisible to a connected client — the query costs above are the
query costs after a refresh. A pass that *does* change something moves the epoch,
and the result cache compares it, so a client that asks the same question twice
across a change is re-answered rather than served the previous id set.

`--refresh` runs the **names** pass, not the deep one: a deep pass inside the serve
loop would stat every entry on the tree while clients wait. So attributes
(`size:`, `dm:`, `dc:` on a file that was edited in place) are still only as fresh
as the last `update --deep` — design §12 risk 8, unchanged.


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
refresh.sh     what a refresh costs: idle pass, adding pass, snapshot write (design §10)
ledger.sh      the memory ledger, the snapshot size and the query shapes
tri-skip.sh    what each derived index costs to skip, and what it buys (design §5.3.1)
sortcmp.sh     per-sort-key cost for several builds at once (design §10)
```

## License

MIT, © 2026 linsmod — the full text is in `LICENSE`, and every source file carries the
`SPDX-License-Identifier` line, so a file that travels alone still says what it is.

Why MIT rather than something copyleft, given the choice was open: the protocol baseline this
tree implements (`../etp_server-1.0.2.5/`, MIT, © voidtools / David Carpenter) is *not* part of
the repository — it sits beside it and is cited by file and line in the comments as the
behavioural reference — so if any portion here was translated from it rather than rewritten, the
two licences agree and there is no relicensing question to settle. MIT is also DFSG-free, which
is what an upload to Debian needs. `sfa/` is MIT for the same reason and is a separate
repository with its own `LICENSE`.