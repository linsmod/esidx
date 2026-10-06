# AGENTS.md

How to work on esidx. Everything here was established by doing it; where a rule
exists because something went wrong, the reason is given so the rule can be
revised when the reason stops applying.

Target: Linux, ext4, C11, libc only. The development host is Windows and the code
runs in WSL2 — see **Environment**.

---

## 1. The loop

```
plan → layer → test → measure → commit
```

Each pass is one layer, one commit, one green suite. Do not start the next layer
until the current one is committed with its measurements.

### 1.1 Read the design before the code

`docs/design.md` is the authority on what a component is for and why it is shaped
that way; §2 maps every capability to an index mechanism, §9 cites the upstream
line each idea came from, §11 holds the closed decisions D1-D7. Every function in
`query.c` corresponds to a row in the matcher table, and every table row cites a
design section. Adding a capability means adding the row *and* the design entry,
not just the code.

Before changing a decision, read its entry in §11. They are closed for a reason
and the reason is written down.

### 1.2 Bottom-up, and one layer at a time

The layer order is fixed by the dependency direction and must not be inverted:

```
collection → storage → index → syntax → execution → protocol
```

A layer may only call downward. `esidx.h` is the storage contract and includes
`syntax.h` for convenience, but `store.c` must never reach into the query or
protocol layers — that inversion is how a storage change ends up needing a
protocol rebuild to test.

`regex.c` and `log.c`/`timer.h` are leaves: they depend on nothing but libc and
`log.h`, and nothing depends on their internals.

### 1.3 Independent tests per layer

Each layer has its own suite, and they are separate files on purpose:

| Suite | Pins | Fails when |
|---|---|---|
| `test.sh` | the **index**, against `find(1)`; plus a **language** suite against a flat fixture | scan, storage or parsing is wrong |
| `test_etp.sh` | the **wire**, against the ETP client's own parsing rules | a reply would not be understood by the client |
| `test_watch.sh` | the **watcher**: kernel events → dirty directories → visible rows | an event is mapped to the wrong directory, or the index is not current within one batch. Needs root and skips without it — see below |
| `round.sh` | nothing — it is the end-to-end demonstration and the source of the numbers in the docs | (prints timings; asserts nothing) |
| `cmp_ref.sh` | nothing — it re-measures every expected value quoted against the reference server (§1.4) | (prints a table; when the two indexes differ by more than 0 it prints the entries that make up the difference and stats each one, so a delta is classified rather than assumed; `--strict` exits 1 on a row the delta does not explain) |
| `test_deb.sh` | the **installed** package: postinst's effects, the unit under a real systemd, the bind and the address list it ships, the wire, event → served index, and what `dpkg -P` takes away | any of those disagree — or a step that needs a runnable `etp-probe` reports SKIPPED rather than passing. Needs root and installs/purges the package on the machine it runs on, so it is **not** in `make check`; `make deb-verify` covers the package's contents instead |
| `make dist` / `dist-verify` | nothing — they package the tree and then check the package | (dist builds a tarball of both repositories; dist-verify unpacks it elsewhere, diffs contents **and modes** against `git ls-files -s`, builds it, and runs the watcher suite from the unpacked tree) |

Run the index suite before the protocol suite. A parse regression shows up as a
protocol failure otherwise, and you will spend an hour in the wrong file.

`test_watch.sh` is **not** part of `make check`, on purpose: it needs `CAP_SYS_ADMIN`
(the `sfa` proxy is the privileged half) and every machine without that privilege would
fail the gate on a feature the protocol does not need. It skips with the reason printed
when `sfa-server --probe` says no, so "it did not run" and "it passed" are never
confused. Run it by hand after touching `watch.c`, `etp.c`'s poll loop, or the dirty
set — and both flavours:

```sh
make && ./test_watch.sh
ESIDX_BUILD=dbg ASAN_OPTIONS=detect_leaks=1 ./test_watch.sh
```

**A number in a comment is only worth something if the command that produced it is
in the repo.** Every expected value this project quotes against the reference —
in `test.sh`, in a commit message, in design §12 — comes from `./cmp_ref.sh`, which
asks both servers the same question over one directory they both index, with the
only difference being the spelling of the path. If you measure something against
`:21` and it is not in that script's term list, the next reader cannot re-check it
and the number decays into folklore. Add the term there in the same commit.


### 1.4 The official client as a test peer

**The specification is the wire, and the wire is observable.** Nothing here is
defined by reading somebody else's source: point a real ETP client at this
server, and whatever it does is the contract. Two peers are available on this
machine and neither substitutes for the suites; each answers a question the
other cannot:

| Peer | Question it answers |
|---|---|
| Everything, as an ETP client of ours | what does a client actually put on the wire, and does it read the reply back |
| `etp_server` on `127.0.0.1:21` | is this the reference's behaviour, or ours |

`tools/etp_probe.c` sits alongside them: same rules as a client, scriptable, so
`test_etp.sh` can assert 200-odd replies in a second. It is a transcription, so
by itself it only proves the server agrees with our reading of a client — which
is why the two peers above exist.

**Point the official client at this server.** Everything accepts `-instance`, so
the ETP instance lives beside the default one instead of replacing it. Put the
search on the command line: that is what makes the session *complete*, so the
log shows both what the client asked for and that it read the block back without
reconnecting. This is the exact invocation that works:

```powershell
# 1. a snapshot to serve (any tree; /etc is small and already indexed by the suites)
wsl -u root -e bash -lc 'cd /mnt/c/Users/linswin/AndroidStudioProjects/ShareToPC/esidx && ./esidx build /etc -o /tmp/etc.idx'

# 2. the server, detached. -v 4 IS the debug level -- do not also set ESIDX_LOG,
#    which -v 4 overwrites anyway (2.3).
wsl -u root -e bash -lc 'cd /mnt/c/Users/linswin/AndroidStudioProjects/ShareToPC/esidx && \
  setsid nohup ./esidx -v 4 serve /tmp/etc.idx -p 2121 --bind 127.0.0.1 \
    -u etpuser -w s3cret >/tmp/srv.err 2>&1 </dev/null &'

# 3. the client. ONE -ArgumentList string, so the instance name stays quoted.
Start-Process -FilePath 'C:\Program Files\Everything\Everything.exe' `
  -ArgumentList '-instance "esidx" -connect etpuser:s3cret@127.0.0.1:2121 -search "ext:conf"' `
  -WindowStyle Minimized
```

`-v 4` logs every `< command` and `> reply` (`etp.c:858`, `etp.c:214`), which is
the only way to see what the official client actually sends. Everything confirms
the connection in its window title: `ext:conf - 127.0.0.1 - Everything (ETP
esidx)`. Then just watch `/tmp/srv.err`; the session this produces is

```
< USER etpuser            > 331 Password required.
< PASS s3cret             > 230 Logged on.
< OPTS UTF8 ON            > 200 UTF8 mode enabled.
< SITE EVERYTHING SIZE_COLUMN 1          > 200 Size column set to (1).
< SITE EVERYTHING DATE_MODIFIED_COLUMN 1 > 200 Date modified column set to (1).
< SITE EVERYTHING PATH_COLUMN 1          > 200 Path column set to (1).
< SITE EVERYTHING COUNT 21               > 200 Count set to (21).
< SITE EVERYTHING SORT DATE_MODIFIED_DESCENDING > 200 Sort set to (...).
< SITE EVERYTHING SEARCH ext:conf        > 200 Search set to (ext:conf).
< SITE EVERYTHING QUERY   ->  query: 'ext:conf' -> N results | N of M candidates | ...
```

This is how the `OPTS UTF8 ON` bug was found: the trace shows the client log in,
send `OPTS UTF8 ON`, and then — because it never got a `200` — send nothing else
at all. There is no error anywhere, on either side; it just goes quiet. That is
the whole reason this peer exists: the probe only ever sent what it had been
taught to send, and the taught list had no `OPTS` in it.

Two things about the setup that each cost an hour to find:

- **The server must outlive the WSL invocation.** A plain `&` dies with the
  calling `wsl`, and then Everything simply reports nothing. Use
  `setsid nohup ... </dev/null &`.
- **Quote the instance name as one argument.** `Start-Process -ArgumentList
  '-instance','ETP Client',...` does not add quotes, so Everything receives
  `-instance ETP Client`, silently takes the instance name as `ETP` and treats
  `Client` as the search text. The symptom is a *working* connection to the wrong
  instance, so it is easy to miss.

And two that cost less but look like protocol failures:

- `pkill -f "esidx -v 4 serve"` kills the shell running it, because the pattern
  matches that shell's own command line. Use `pkill -x esidx`.
- If nothing appears in `/tmp/srv.err` at all, the client instance did not start.
  Check the process list; there is no message for this.

**What this cannot tell you, and the probe is the answer.** The client reads the
block in its own process and shows it in a window, so we can prove it asked and
that it did not hang — not what it made of the contents. For "would a client
understand this reply", run the probe with the sequence above verbatim:

```sh
./etp-probe 2121 <script>   # the script is the trace Everything produced
```

**`es.exe` is deliberately not here.** voidtools' command line interface looks
like the natural driver for a named instance, and it was tried first. It is
redundant: `-search` already completes the session, and the probe reads the block
better. It also cannot drive an ETP instance at all, which cost an hour to
establish and is worth recording so nobody retries it. Measured, and against the
reference server on `:21` as well:

| target instance | addressing | search |
|---|---|---|
| the default one (no `-instance`) | works | works |
| a plain named instance, index loaded | works | works |
| an **ETP** instance | works | **never returns** |

From the `cli.c` that ships in `es.zip`: `cli.c:4269-4274` builds
`EVERYTHING_TASKBAR_NOTIFICATION_(<name>)`, adding the parentheses itself, so
`-instance <name>` is the correct spelling; addressing works because the version
probe is an unconditional `EVERYTHING_WM_IPC` message; and the search goes out as
`WM_COPYDATA` + `EVERYTHING_IPC_COPYDATAQUERY` (`cli.c:477-481`) with the answer
due back as a *separate* message to es.exe's own window, which never arrives.
`-timeout` does not bound that wait (`cli.c:4021-4045` polls
`EVERYTHING_IPC_IS_DB_LOADED` only when `-timeout` is given, in a loop with no
counter), and `-ipc1` / `-ipc2` answer from the *default* instance instead, so a
six-figure count from an instance named here is a red flag, not a result.

The trap worth remembering from that hour: **a fresh instance looks exactly like a
broken one.** A plain `-instance <name>` whose database has not finished loading
also hangs, with no flag and nothing to wait on. It resolved by itself once the
index loaded. So never conclude anything from a hang until the plain instance
searches.

**Use the probe against `:21` too.** Running `etp-probe 21 <script>` is the only
check that the probe's rules are satisfied by an implementation we did not write.
It passes for every shape the suites cover. It also reproduces the one known
deviation: a client cannot classify ` MLSD` in the reference's own `211-` FEAT
reply and stops there, which is why no client sends FEAT (§5.1).

### 1.5 Commit per batch

One commit per layer, message written before the code is finished so the summary
is about what changed and why rather than what was typed. The message must
contain:

- what the layer is and what it replaced, if anything;
- the reasoning for any non-obvious decision, especially a judgement call;
- **the measurements**, including the before value when there was one;
- every bug found and fixed, with why the tests missed it earlier.

Measured numbers in a commit message are how a later reader tells whether a
regression has happened. `git log` is the only place that survives the next
refactor.

---

## 2. Environment

Development host is Windows; day-to-day work builds and runs in WSL2. WSL2 is not
the target, and its timings are not the machine's — see §2.4.

```sh
# from PowerShell
wsl -u root -e bash -lc "cd /mnt/c/$PWD && make && ./test.sh"
```

`-u root` is required: `/root` is mode 700 in this image, and the trees the
upstream survey cites live under it.

### 2.4 WSL2 is not the baseline

WSL2's virtual disk and CPU understate the target. Measured with `./round.sh /usr`
(the reference run), WSL2 scans at 27.6 k entries/s against **268 k/s** on real
ext4 NVMe — the `r7000` host, Ubuntu 22.04 x86_64, reachable as the `ssh` alias
`r7000`. Anything quoted as a *baseline* — the tables in design §10, a "before" in
a commit — must be re-run there before it is believed. The suites run unchanged on
it; `make install` puts the binary in `$(PREFIX)/bin` (default `/usr/local`; use
`PREFIX=$HOME/.local` to install without root).

Two environment traps on that box, both of which have cost an hour:

- its user `umask` is **002**, so files the fixtures create are group-writable —
  anything asserting a mode string must pin it (`test_etp.sh` `chmod 644`s its
  LIST fixture for exactly this reason).
- **its clocksource list is `hpet acpi_pm`** — there is no TSC on offer, so
  `clock_gettime` is a real syscall costing **1 222 ns**, against 20 ns for the
  vDSO call it is on WSL2. That is 60x, and it decides where instrumentation may
  live: a timer on a per-row or per-entry path is free on WSL2 and ruinous here.
  Phase timings are two reads for a whole phase, so they are unaffected, which is
  why every number in design §10 still stands; a per-comparison timer would have
  made every sort number a statement about the clock. Read
  `/sys/devices/system/clocksource/clocksource0/current_clocksource` before
  trusting any hot-path timing measured on a new host, and let the code print its
  own price — `-v 5` does (`scan: split: ... reads at 1222 ns`).

### 2.1 Compile-check without linking

```sh
for f in store index scan lexer parser regex query log etp main; do
  gcc -std=c11 -O2 -Wall -Wextra -D_GNU_SOURCE -c "$f.c" -o /tmp/"$f".o
done
```

`make` stops at the first error; this reports all of them, which matters when a
signature change touches one declaration and six call sites. Keep it in a script.

### 2.2 `-Wall -Wextra` clean is a gate, not a goal

The build has no `-Werror`, so a new warning is easy to miss. When a build emits
anything, fix it before committing — and if a warning is genuinely unavoidable,
suppress it with a comment saying why. Several warnings in this codebase are
already load-bearing (`-Wformat-truncation` on a deliberate snprintf).

### 2.3 `-v 3` is info, not debug

`log.h:18-21` numbers the levels from 1 (`LOG_ERROR=1 … LOG_DEBUG=4`) and
`log_init` passes `-v N` straight through as the enum value, so **`-v 3` is
`LOG_INFO`** and the wire trace only appears at **`-v 4`**. Two traps in one:

- every script in this repo says `-v 3`, which is why none of them ever showed a
  `< command` or `> reply` line;
- `log_init` reads `ESIDX_LOG` *first* and then lets any `-v N` overwrite it, so
  `ESIDX_LOG=debug ./esidx -v 3 …` silently runs at info. Pick one.

Debug is the level you want whenever the question is "what did the peer actually
send" — see §1.4.

**`-v 5` (`LOG_PERF`) is the scan walk's per-syscall split**, and it exists because of
§2.4's clocksource: four `clock_gettime` calls per entry are free against a TSC and
ruinous against HPET, where they cost 36 % of the walk they are splitting. It is not
INFO, because `round.sh` builds at INFO and every published number comes from there;
it is not DEBUG, because a sanitiser build logs at DEBUG by default and the gate would
pay for it on every build. The per-directory `descend` trace stays at *exactly* `-v 4`,
so `-v 5` does not drag 651 894 lines along on a large tree.

---

## 3. Tests

### 3.1 Both suites, both builds, every time

```sh
make check
```

That is the whole gate: it builds **both** flavours and runs both suites against each,
in the order below (index before protocol, so a parse regression is not read as a
protocol fault). It takes about 30 s.

```sh
make                      # optimised + sanitiser, no selector, no env var
./test.sh && ./test_etp.sh

ESIDX_BUILD=dbg ASAN_OPTIONS=detect_leaks=1 ./test.sh
ESIDX_BUILD=dbg ASAN_OPTIONS=detect_leaks=1 ./test_etp.sh
```

**There is no `DEBUG` variable and no `make clean` between the two.** The flavours used
to share object names, so switching meant `make clean && make DEBUG=1` and forgetting
the clean left the `-O2` objects in place — a run that *looked* sanitised and was not,
which cost an hour of "the ASan failure is not reproducible" (the run was the optimised
binary). The objects are named apart now (`%.o` and `%.dbg.o`), `make` builds both, and
`make opt` / `make dbg` name one. Do not reintroduce a selector that points the gate at
a single flavour: the whole reason the ASan build finds things is that it is not optional.

**And each suite refuses to measure a binary older than the code.** `make` that fails
part-way leaves the previous binary in place, and a suite run afterwards does not fail —
it re-measures the last build and reports it as this one. That is not hypothetical: it
happened while writing the watcher's work-mode assertion, where a botched edit broke the
build and `./test_watch.sh` still printed 20/20. All three suites now compare mtimes
against every `*.c`, `*.h`, the `Makefile` and `sfa/*.[ch]`, and exit 1 with the offending
file named. The suite cannot know what is inside the binary; the mtime is the cheapest
thing it can know. The check is repeated in each script rather than shared, on purpose —
they are independent files so one missing tool cannot take down the other two (§1.3).

One `make` also builds the two test peers (`all: opt dbg` covers `etp-probe` and
`order-ref` in both flavours), so there is no second step to forget. It used to be
`make && make etp-probe`, and forgetting it cost an hour's confusion:
`test_etp.sh` stops with `./etp-probe not built`, which reads like a broken checkout
rather than a missing prerequisite.

**A bare `make` must build something, and saying so is a rule.** `.DEFAULT_GOAL` is
named explicitly rather than left to be the first target in the file, because that is a
silent trap: one commit added a `require-sfa` rule above `all`, and from then on a bare
`make` checked one file and exited 0 on every machine. The check that missed it was
`make 2>&1 | grep -Ei 'error|warning'` — it printed nothing because nothing happened. A
deployment caught it (`make -j16` returned in 12 ms with no artefacts). **Ask whether the
artefact appeared, not whether make complained** — the same question as the mtime guard
above, one level up.

The ASan build is not optional. It is the only thing that catches:

- **use-after-free hidden by -O2.** The freed block still holds its bytes, so an
  optimised build passes while a use-after-free on a compiled regex sits in
  `text_match`. It was caught only because the sanitiser suite ran.
- **an uninitialised struct field read as a valid empty set.** `ext_index_add()` grew
  `sets[]` by `realloc` and left the new slot's `nbits` and `w` as whatever was in the
  heap; `bs_reserve()` read that as "big enough" and `bs_test()` dereferenced
  `0xbebebebe`. 35 assertions failed under the sanitiser build and **all 238 passed at
  `-O2`**, because fresh pages from the OS read as zero — the garbage happened to be a
  valid empty bitset.
- **leaks that swallow buffered stdout.** A 64 KiB path buffer leaked per query
  made three query tests fail with *empty output* rather than a leak report,
  because ASan aborts before stdout is flushed. If a query test suddenly returns
  nothing under `ESIDX_BUILD=dbg`, look for a leak before looking for a logic bug.
- **out-of-bounds and signed overflow**, which on this code are silent.

Run `./round.sh /etc` too. It is cheap and it is the only thing that exercises all
three layers against one another.

### 3.2 Pin correctness against something, not against yourself

`test.sh` takes its expected numbers from `find(1)`. That is deliberate: a
hand-written expectation only detects a change, whereas `find` detects a wrong
answer. When adding an index assertion, derive the number the same way.

For the language suite the fixture is flat and small on purpose, so the expected
counts are literals a reader can check by hand, and a wrong answer names the entry
it wrongly included. Do not add the deep tree to it — that is what `find`
cross-checking is for.

### 3.3 Protocol tests drive the real client's rules

`tools/etp_probe.c` is not a generic FTP client. It applies a real client's parsing
rules, including the parts that are fragile:

- a reply ends at `nnn` + space and continues on `nnn` + hyphen; anything shorter
  than four characters also terminates;
- the query block does **not** use that rule — it keys on the literal
  `200-Query results` and `200 End.`;
- `RESULT_COUNT` is parsed independently of every column toggle;
- a numeric field that overflows a signed 64-bit parse leaves the accumulator at
  its initial value, because the exception is swallowed.

Each of those is pinned against a live server *and* re-checked against voidtools'
own server on `:21`, so "the probe understood the reply" means a client would.
When you change anything on the wire, transcribe the corresponding rule rather
than writing a looser check — and if a rule is wrong, fix it against what a real
client does (§1.4), not against what the probe happens to do.

### 3.4 Write the failing test first, and make it fail for the right reason

The bug is not the interesting part; the reason the previous test missed it is.
Record that in the commit message. Examples from this codebase:

| Bug | Why it was missed |
|---|---|
| a quoted value after `fn:` became a separate term | no test sent `parent:"..."` over a socket — only the CLI, where the shell had already eaten the quotes |
| `dm:today` matched nothing | the test asserted "0 rows", which is what an empty result looks like |
| `SIZE` returned 550 for files | only directories were in `di_lookup`, and only LIST was tested |
| `COUNT` defaulted to 0 | the reference defaults to `0xffffffff` (`:1207`); no test omitted COUNT |
| the 503 guard fired on an armed PASV | the guard was copied from a reference whose state variable means something slightly different |
| `OPTS UTF8 ON` answered `501`, hanging the official client | the probe only ever sent what it had been taught to send, and that list had no `OPTS` — the verb was only on the wire when the real client was pointed at the server (§1.4). A rejection the client does not treat as fatal just stops it sending anything else, which looks like a hang with no error anywhere. |
| the indexed root printed its own path as its name, with an empty PATH column, and `name:` matched the directories *above* the root | every other row's parent is in the index, so the root was the only row where the two spellings could differ — and no term read it. Fixed by `display_name_of()` + `dirname(path_of())` (design §12.12); found by diffing both servers' full result sets, which `cmp_ref.sh` now does whenever the index delta is not 0. |
| a path sort allocated 64 KiB per row and, where malloc refused, silently became a **name** sort | every path-sort assertion sorted a *filtered* result set (the largest: three rows), and on WSL2's `/etc` the 104 MB of allocations succeed. It needs >26 000 rows in the result set, which is the regime §2.4 says WSL2 is not, and no `round.sh` drive sorted by path at all. The assertion for it runs the sort under `ulimit -v`, because without the cap the old code **passes** |
| the sort's folded-key arena was one realloc-doubling buffer, so every pointer already handed to an earlier row dangled | **`-O2` hid it and the DEBUG build caught it** — the freed block still holds its bytes, so the order assertions passed at `-O2` and failed under ASan (§3.1). Fixed by never moving a block. Worth recording as a rule: an arena that hands out pointers must not grow by `realloc`, because the caller has already stored the old addresses |
| `sort:attributes:` and `sort:inverse_size:` on the CLI silently sorted by **name** | `main.c` carried its own list of sort keys beside the 22-name table the ETP path uses, and the two drifted. The ETP wire was always right, so `test_etp.sh` could not see it; and a name sort and an attribute sort return the same *rows*, so a row count could not either. What found it was a measurement that made no sense — a numeric key 2.4x faster than the same key with an integer compare. The CLI now goes through `sort_from_etp_name()` and **refuses** an unknown key |
| `sort -f` is not a case-insensitive byte order, and neither is `tr A-Z a-z \| sort` | GNU sort folds for *equality* but orders by the original bytes; under `LC_ALL=C` it compares bytes **signed** while `strcasecmp` compares unsigned, so any byte >= 0x80 lands in the other half of the order. Both agree with `strcasecmp` on a lower-case fixture, so an assertion written against them passes for the wrong reason — which is how a `sort -f` oracle survived a commit. The oracle is `tools/order_ref.c`, which *is* `strcasecmp` |
| a name sort ordered two names that differ only in case by their raw bytes, not by id | the ranked path never copies the display name into the arena — that is what the rank is for — so `dn` stayed a pointer to the **unfolded** name and `cmp_folded` `memcmp`-ed the bytes as stored, which is not `strcasecmp`. Every order fixture had distinct names, so the tie-break was unreachable, and the no-rank fallback (which *does* fold `dn`) silently disagreed with the ordinary path about the same rows. A bug in one level of a multi-level sort is invisible until a fixture reaches that level — and `order-ref -c` cannot express it either, since it sorts the file it is given, so the case assertion compares against `find(1)`'s order (the id order) with `cmp` |
| a rename at depth 2 was invisible to the names pass | the reach of the stamp gate was never asked as a question: `reconcile_dir()` descends by comparing a **child's** stamp, so a directory is reached only through its parent, and an isolated change moves only the stamp of the directory holding the name. Every fixture change sat at depth 1 (`test.sh`'s `INC/one.txt`, `INC/a/three.txt`) or created a subtree whose *top* was a direct child of the root (which B5 descends into), and design §7's table asserted "correct for anything that changes a name" — so the claim and the fixtures agreed with each other and neither was checked against `find`. Found by the watcher's loss path: `SFA_EV_UNRESOLVED` answers with a mark of the root, a rename the proxy cannot resolve is invisible until a deep pass, and `test_watch.sh` asserted the convergence. Fixed by the **sweep** (§7 "Sweep", `esidx_sweep_dirs()`): compare every directory's stamp instead of the ones a walk reaches. The failing assertion came first, at both levels — `test.sh` a create and a rename at `a/b` with the depth-1 case beside it as the control that must keep working, `test_watch.sh` the convergence — and it is on demand rather than the default because it costs 77 ms on `/usr` and 1.6 s on `/work` against a pruned pass's 0.1-0.2 ms (r7000, measured) |
| a new row was invisible to `size:`/`dm:`/`dc:`, the result cache never compared `db->epoch`, and a skipped sorted array could be merged into existence | all three are on the path `esidx update` takes and on no other: it exits, so the next process rebuilds the sorted arrays from the columns (`esidx_add` never pushed them), the epoch could not move under a live connection (serve never called `esidx_update`), and the delta it left behind died with it. 337 + 217 green assertions, and two of the three were *promised in a header* — `esidx.h:485` and design §6.4. The reachability test needs a process that appends and answers in the same breath, so it needed `serve --refresh` to exist first: one connection, two identical QUERYs, the disk changed in between, and no `cache hit` in the log |

### 3.5 A test that cannot fail is worse than no test

The 503-while-transfering branch is unreachable in a single-threaded server,
because a transfer completes inside one command. It is documented in
`test_etp.sh` rather than asserted. Do not keep assertions that pass
unconditionally.

---

## 4. Performance and timing

### 4.1 Instrument at every layer boundary, and keep it

Every layer reports its own phases through `timer.h` and `log.h`:

| Layer | Reports |
|---|---|
| scan | walk time, µs/entry, entries/s, getdents volume, stat success rate |
| store | one line per derived index, in `finalize` |
| query | plan / eval / sort / total, **and the candidate count** |
| protocol | the query line, plus the candidate count and which leaf was the driver |

The candidate count is the important one. `N of M candidates` is what shows the
driver index working, and it is the number to look at first when a query is slow.
`test.sh` prints the four client query shapes with their timings and the driver
choice at DEBUG, so a regression shows up as a number rather than as a feeling.

### 4.2 Measure before and after, on the same tree

`./round.sh /usr` is the reference run. Two rules:

- **Compare like with like.** Same tree, same build flags, same machine state.
  The `/usr` numbers in `README.md` came from one run of `round.sh /usr`.
- **Report the candidate count with the time.** A time without it says nothing
  about whether the time is right.

### 4.3 Know what the measurements already settled

Do not re-litigate these; they were measured and the conclusions are recorded:

- **The driver index removed the sort problem.** An unfiltered query used to spend
  42 of 46 ms in `qsort` over 116 888 rows. A browse request now costs 0.33 ms
  because `parent:` seeds 14 candidates.
- **TopK (§6.2 step 4) is not worth building.** `RESULT_COUNT` must be the size of
  the whole matched set, so every id is collected and sorted anyway, and keeping
  the sorted array is what makes the protocol result cache free. Recorded in
  design §10.
- **The name trigram index is built, and the 10⁶-entry activation threshold it
  replaced was wrong by an order of magnitude.** A wildcard over 78 296 candidates
  cost 23.8 ms of a 24.3 ms query at 10⁵ entries, so the gate would never have been
  reached before the cost was already felt. P4 layer 1 (`trigram.c`) now measures,
  on `r7000` over 372 084 entries: a bare word 29.9 → 6.5 ms of eval,
  `path:/usr *.conf size:>1k` 64.7 → 33.9 ms (from its *other* leaf — `path:`
  itself is still a full scan, that is the unbuilt path half). It costs +90 ms of
  `finalize` and ~20 MB, and buys back 20 ms per query; `round.sh` carries the
  bare-word shape precisely because nothing measured it before.
- **The trigram postings are delta-varint encoded, and it is free on the query path.**
  318.6 → 101.8 MiB on /work (3.13x), 18.4 → 5.9 MiB on /usr, for +3 µs on a bare-word
  query and nothing measurable anywhere else. The reason it is free is structural and
  worth keeping: **a posting list's only reader walks one list front to back and never
  seeks**, which is the shape delta-1 exists for. The reason it is worth it is that
  "resident, so nothing to decompress" is the wrong question at 318 MiB — what decides
  is how many bytes a *query* touches. The full numbers are in design §10; the two
  traps are that `-O2` does not notice a one-byte over-read past a list (§3.1 caught
  it), and that a file's id is its position in readdir order, so **no fixture can place
  a wide posting by naming a file** — the reconcile path can, because `update` appends
  ids and never reuses one.
- **A filter must be provably a superset, and the allowlist is the proof.** The
  trigram set of a pattern's longest literal run is a *necessary* condition for a
  match, so intersecting can only drop rows `text_match()` rejects. That is why
  `tri_applies()` lists the shapes it may touch rather than the ones it may not: a
  refused prefilter is only slower, an accepted one that is wrong loses rows.
- **"Batch stat by inode" is not worth building, and the walk's only lever is D6.**
  Measured with `-v 5` on `r7000` over `/work` (5 476 485 entries, design §10): the
  walk is 55.7 s, of which **59 % is `getdents64`** (652 k calls, 392 B each, 50 µs a
  call — one directory block at a time, latency-bound) and **18 % `fstatat`**, which
  costs **1.81 µs and does no I/O at all**, because the inode the directory block just
  named is already resident. Three consecutive walks put `fstatat` at 16.6 / 16.6 /
  16.5 s while `getdents64` moved 63.1 → 50.6 → 31.9 s, which is what says the inodes
  were cached throughout. Batching by inode would remove path resolution and leave
  nothing; the counters that would disprove this on some other tree
  (`esidx_log_stats()`) stay. For scale: the same build costs 60.8 s against
  `find`'s 56.75 s on the same tree, so there is no fat left in the walk to trim —
  only cores to add.
- **`image:` is no longer the largest cost, and the sort was never the whole story.**
  Every sort key over an unfiltered `/usr` cost 143–240 ms, and the cause was
  `cmp_rec`'s comparator, not the sort's shape: ~6 M `strcasecmp` calls. Measured per
  key with `./sortcmp.sh` (the harness is in the repo for this reason):
  - folding each name per query and `memcmp`-ing it is **1.00×** on a name sort — a
    name is ~20 bytes, so `memcmp` is no cheaper than `strcasecmp` and the fold costs
    what it saves. A dense rank over the distinct display names, built in `finalize`,
    is **2.42×** (166 → 69 ms), because it removes the comparison rather than
    cheapening it.
  - the fold *does* pay on the four keys whose primary compare is an integer and whose
    tie-break is a string (extension 1.32×, date 1.50×, size 1.34×, attributes
    1.14×) — there it is amortised over ~17 comparisons per row.
  - `path` is **9 % slower** and recorded as such; both alternatives measured worse.
  - The comparison count is unchanged, so §6.2 step 4 (TopK) is not the lever here.
    What was, and is built: the **tie-break reads the name rank too**, so the four
    numeric keys stop reaching a string at all — 1.61–2.58× on `r7000`, at 10.3 ns
    per comparison against the 9.9 ns floor a name sort already had (design §10).
    `extension` is the one left, because its *primary* key is a string.
- **The in-memory text scan is no longer the remaining cost, but it is still a cost
  on paths.** `path:` has no index yet; `path_of()` is O(depth) per call (§12 risk
  7), which is what the path half of §5.2 would fix. Measured first: a prefilter can
  only narrow a path term that is *not* an ancestor of the index root, so on the
  tree every §10 measurement uses — rooted at `/usr` — `path:/usr` matches every row
  and a path trigram index would save nothing. The narrowing it does buy is on a
  term strictly inside the tree (`path:/etc/ssh`: 13 of 3 838).
- **`path_of()` is O(depth) and does not allocate** (§12 risk 7, corrected). It was
  the *sort* that allocated per row; there is no path-sort cache left, because there
  was never a second call to cache.
- **A dirty set is a shape change, not a constant-factor win, and the measurement says so.**
  "Which directories need a full listing" now has one answer for a full pass and an
  event-driven one: `esidx_mark_dirty()` appends to a transient set and
  `esidx_refresh_dirs()` reconciles exactly it, with `esidx_update()` being the case where
  the set holds the root — so a partial refresh cannot drift from a full one because they
  are the same code. Measured on `r7000`, and the numbers refuse to oversell it: a full pass
  is already **0.1 ms on `/usr` and 0.2 ms on `/work`** when idle, so refreshing one
  directory can be *more* work than refreshing the whole tree (6.8 ms for `--dir
  /usr/share/doc`, which owns 3 200 subdirectories). What the set buys is the case the
  stamps cannot cover — a file edited in place moves nothing its parent's mtime can see
  (§12 risk 8) — and there it is 3.1 ms to tombstone 2000 ids against a `/usr` worst case
  of 110 ms over 7 776 directories. Two rules learned the hard way: the set is **not**
  ancestor-collapsed (the parent's reconcile only descends on a moved stamp, which is
  exactly the change that cannot be relied on), and a directory marked and then tombstoned
  by an earlier apply in the same batch is skipped (ref B4).
- **A periodic pass is affordable; a periodic pass that *adds* is not, and the difference
  is not proportional to the change.** `esidx serve --refresh=SECS` runs the names pass in
  the serving process (design §7 "In place"), so freshness is a knob rather than a restart.
  Measured with `./refresh.sh` on `r7000` (real ext4 NVMe, `hpet`, so these are syscall
  times): an **idle** pass is **0.4-0.9 ms over 5 476 485 entries** — the walk stops at the
  first unchanged directory stamp, so it is proportional to the *root's* fanout, not to the
  tree — and a snapshot write is **538-563 ms** for a 313 MB file, which is why `--save` is
  a separate coarse knob and is skipped entirely when the epoch has not moved. A pass that
  adds rows cost **2.38 s** and now costs **0.40 s**, the difference being two O(n)
  rebuilds turned into thresholded drains; neither was proportional to the change (two
  runs of the same command added 1220 and 2001 rows and cost 2.38 s both times). What is
  left of it is 389 ms of name-intern table rebuild on the *first* add after a load — a
  once-per-process cost, and now the largest single item on the path. Removing stays cheap:
  2001 tombstoned ids cost 5-9 ms, because no name changed and the CSR swap-removes in
  place.
- **A structure that cannot be appended to in the middle costs O(n) per pass, and the fix
  is a threshold on a second structure rather than a cheaper rebuild.** The children array
  is a compressed sparse row, so an addition cannot go into it; it goes to an append-only
  overlay that `esidx_drain()` folds in once it passes **a twelfth of the entry count**
  (497 ms per 456 000 additions on `/work`, instead of per pass). The name rank is the same
  bargain for the same reason — a rank is a sorted position — and the fix has to be
  different, because a new name cannot be ranked into the middle of the table either: it is
  given `gap << 32` minus the width of the pending names sharing that gap, where `gap` is
  one binary search over the sorted rank table (query.c, `rank_pending`). Four things about
  that, all of them measured or refuted rather than argued:
  - **A comparator that falls back to a string for the rows it has no rank for is not a
    valid ordering.** Its answer would depend on which of two rows carries a rank, and
    qsort is entitled to answer anything at all to that. So the fallback is *arithmetic*
    and the comparator stays a pure integer compare — which is also why `srec_t.nrank`
    widened to 64 bits, rather than the primary key and the tie-break growing two rules
    that have to agree.
  - **A per-directory slack does not work, and the reason is the case that matters.** Slack
    at the end of each range keeps `di_children()` a single view and needs no overlay at
    all — but a directory created by a bulk copy is *empty*, so its slack is empty too, and
    the measured 2000-files-into-one-new-directory blew through any fixed slack and paid the
    full 497 ms anyway.
  - **Two views are the price of the overlay, so the accessors are the contract.**
    `di_children_n()` / `di_child_at()` exist because a caller reading `.items`/`.n` alone
    compiles and silently skips every child added since the last rebuild — the exact shape
    of bug §3.4 keeps recording. There are five call sites and all five were changed.
  - **`child-count:` is the invariant that catches it.** It reads the `nchild` column while
    `parent:` reads the two runs, the two are maintained by the same two functions, and
    test_etp.sh 11c asserts they agree at every step — which is the only assertion that
    notices a caller reading one run and missing the other.
- **A derived structure can hold a wrong value that every current reader is indifferent
  to, and that is not the same as being correct.** `rk_off` (rank → offset of its folded
  name) was filled with `noff[newr[i]]` where `newr` is indexed by *old rank*, so the table
  came out a permutation of the offsets — while `name_rank`, filled from `newr` separately,
  came out **right**. Every name sort was correct, because the only two readers were
  `rk_intern`'s probe and the intern rebuild, and neither requires the table to be
  *sorted*. `esidx_name_rank_gap()` is a binary search and the first reader that does, so
  the bug became reachable the moment the on-the-fly keys needed an insertion point, and
  test_etp.sh 11c's order assertion found it in one run. Two rules: a new reader of a
  derived structure is a new *test* of the invariant that structure documents; and when
  adding one, check what the old readers actually required rather than what the comment
  claims.
- **A promise in a header is not an implementation, and the process that runs daily can be
  why the gap stays invisible.** Three defects survived 337 index assertions and 217
  protocol ones because the only caller of the mutation path was `esidx update`, which
  exits: a new row never reached the numeric sorted arrays (`esidx_add` has nothing to link
  *from*, and the next process rebuilt them from the columns), the result cache compared no
  `db->epoch` although `esidx.h` and design §6.4 both said it did, and a `--no-index=size`
  process could merge a two-row delta into a deliberately unbuilt array and then read it as
  built. All three are reachable only from a process that appends and answers questions in
  the same breath, which is what `serve --refresh` is. Two rules: a test that exercises a
  layer through a caller that exits is testing the *caller*; and a documented invariant that
  no line reads is a comment, not a check.
- **A fixed threshold chosen for a batch tool can be wrong in a server, and the reason is
  latency rather than memory.** `esidx_compact()` at 25 % tombstones is right for a process
  about to exit and wrong for one that must answer: it is a full rescan plus a `finalize`
  (97 s on `/work`), and it replaces the whole `esidx_t`, so every cached result set becomes
  a set of unrelated ids. `EU_NOCOMPACT` is how a caller says "not now"; the offline
  `update` still compacts, so the id space is reclaimed on the next cron pass, not never.
- **`--refresh` does not fix attribute staleness and must not be documented as if it
  does.** It runs the *names* pass, because a deep pass inside the serve loop would stat
  every entry on the tree while clients wait. `size:`/`dm:` on a file edited in place are
  still only as fresh as the last `esidx update --deep` (§12 risk 8, unchanged).
- **A derived index that is not built must answer slower, never differently**, and the
  reason it is worth stating is that two halves disagreed: `est_leaf()` already refused
  to seed from an empty sorted array while `range_on()` went ahead and emptied the
  answer, so `size:`/`dm:`/`dc:` returned **zero rows** for a valid query with no error
  and exit status 0. Partial correctness reads as robustness. It is configured three
  ways deep — flag, `<dbfile>.opts` sidecar, `ESIDX_SKIP_INDEX` — and the **file is
  byte-identical in all of them** (same md5 with every index on and all five off), which
  is D4 saying out loud that the choice cannot reach the data. `./tri-skip.sh` exists so
  the cost is checkable rather than asserted: it prints the matched count from both
  configurations on every row, because a skip index is worth nothing if the two sides may
  disagree. Three things fall out of the numbers (design §5.3.1): a range index pays
  **only where the range is selective** (`size:>10mb` 10.5x slower without it, `size:>1k`
  free without it — a 41 ms sort dwarfs the 1.2 ms it saved); a missing index costs twice,
  since seeding is what keeps the *sort* small too; and the **name rank is a sort cost,
  not an eval cost** (2.8x, all of it in sort), so a harness that printed only
  plan/eval would have called it free.
- **A flag that a loop can also see becomes a search term.** `--no-index=size size:>1k`
  answered zero rows: the option worked, and its own text was ANDed into the query as a
  term matching no filename — a symptom pointing at the option being ignored, which is
  the opposite of what was wrong. `log_strip_flags()` already removed `-v` from `argv`
  for the same reason; `idx_strip()` now does the same for `--no-index`. The general
  rule: **a CLI flag is stripped once, centrally**, not read in place by each parser.
- **A returned pointer into a dead frame is right until it is not.** `esidx_index_
  resolve()` handed back `*source` pointing at a `char[4096]` local that the sidecar
  reading loop then reused. `-O2` printed the right path (the old copy sat in a register)
  and the sanitiser build's `-O0` printed an empty one. Found by `make check`, which is
  the whole argument for it (§3.1).

### 4.4 Do not optimise on a guess

`esidx_log_stats()` exists so the two numbers that decide whether "batch stat by
inode" is worth building — the stat success rate and the getdents byte volume —
are already being collected. If you are about to add an optimisation, check
whether the measurement it depends on is already available. If it is not, add the
counter first, in its own commit.

---

## 5. Interoperability

### 5.1 The client is the specification

The consumer is an ETP client, and the observable one is the way to ask it
questions (§1.4). When a protocol or language question arises, run a real client
against this server and look at what it does; the answer goes in a comment next
to the code. Its behaviours that are not obvious and that this server matches
deliberately:

| Behaviour | How we know | Consequence here |
|---|---|---|
| spells the extension `SITE EVERYTHING`; others send it bare | trace from the official client | both spellings route to one dispatcher |
| never opens a data connection | same trace | browsing is `parent:` + `folder:`, so `parent:` must be O(1) |
| joins `path + "\" + name` | observed round trip: a path we emit comes back mixed-separator | the PATH column uses backslash separators; `normalise_path()` accepts either on the way in |
| swallows an overflowing SIZE | the sentinel shows no size at all | the unknown-size sentinel is what we want |
| `RESULT_COUNT` is the total, not the page | observed: a page of 3 out of 8 reported 8 | the executor returns the full sorted set and the protocol layer slices |
| sends `OPTS UTF8 ON` first, and stops there unless it gets `200` | trace, after a `501` | see below |
| `COUNT` is only sent when positive | trace | the default must be "unlimited", matching `etp_server.c:1207` |
| closes the socket right after QUIT without reading | observed on `:21` | a clean EOF is normal, not an error |
| cannot classify ` MLSD` in the reference's own `211-` FEAT reply | the probe truncates at the same line | do not "fix" the server around it; a client never sends FEAT |
| echoes the whole search in `200 Search set to (...)` | `etp_server.c:4027` | the acknowledgement must be as long as the search: formatting it into a fixed 1 KB buffer loses the CRLF, and the client then waits for the rest of a line that is never coming — a hang with no error on either side, the same shape as the `OPTS` one above |

**A silent cut is worse than a refusal.** Three fixed buffers on the way in used to
truncate a long search without saying so — the parser's per-term value at 2047, the
protocol layer's search at 4096, and the acknowledgement above at 1023 — and the answer
was a query nobody asked. Found by asking the reference the question instead of asking
ours: a 451-name `ext:` list answers `167 658` on `:21`, the same as `ext:c` alone, and
ours used to hang. That term is in `./cmp_ref.sh`. The general rule this leaves behind:
**every fixed buffer between the socket and a term's value needs a reason to be smaller
than a control line**, and a test for it has to put the discriminator at the *end* of a
value past every bound — a value that happens to fit proves nothing.

The official Everything client is a second, independent reader, and it disagrees
with the one the probe was built from in one place that matters: it sends
`OPTS UTF8 ON` as the first command after login. A server that does not answer
that `200` never gets the column toggles or the QUERY, and the failure looks like
nothing at all. So `OPTS` is implemented to the reference's rules — `UTF8 ON`/
`UTF8 OFF` accepted, the bare `OPTS UTF8` rejected — and pinned in `test_etp.sh`
§11. Treat the clients as two specifications and satisfy both: where they differ,
the reference server's behaviour is the tie-breaker.

### 5.2 Match the reference byte for byte, then document the deviation

`../etp_server-1.0.2.5/src/etp_server.c` is the protocol baseline and decision D1
says to port its command layer rather than rewrite it. So: the reply grammar, the
32 subcommands, the acknowledgement wording and the 22 sort names are copied, not
reinvented. Cite the line in a comment (`etp_server.c:5189`) so a reader can check
the claim.

When a deviation is genuinely better, do both: make the change *and* record why in
the same place a future reader will look. The two that are left:

| Deviation | Why | Consequence |
|---|---|---|
| a top-level index root's PATH column is `/` | the reference's is `C:` — it indexes the drive root, we have no drive | POSIX's spelling of "the directory above `/etc`". A client joining `path + "\" + name` gets `\/etc`, which is the mixed-separator form §5.1 says it round-trips; an empty PATH instead would lose the leading `/` outright. Design §12.12 |
| a control line longer than 8 KB is refused, with a log line, rather than parsed | ours has one control buffer; the reference reallocs the search per `SEARCH` (`etp_server.c:4025`) and its only limit is its own line reader, which was not measured past 4 950 characters | Everything's search box takes tens of thousands of characters, so a client *can* send more than we take. The reference was asked with 4 950 and answered; we answer the same at 4 950 and refuse beyond 8 KB. Everything above the buffer — the parser's term value, the search itself, the acknowledgement — now holds anything that fits, so the limit is one number, in one place, and it is logged |

### 5.3 A parse error must yield no results, never everything

The client cannot distinguish them and both look like success. Returning the
whole index for a typo is the worst possible answer to something about to be
rendered. A malformed query produces a well-formed empty block plus a `LOGW`.

The same applies to functions ext4 cannot answer: `content:`, `dupe:`, `si:` and
the media metadata parse, warn once, and match nothing.

---

## 6. Coding conventions

Established in this codebase; match the surrounding code.

- **C11, `-Wall -Wextra`, four-space indent, opening brace on the same line.**
- **Comments explain why, not what.** The bar is: would a competent reader who
  does not know the history of this decision be likely to get it wrong? If yes,
  write the reason. If no, write nothing. Do not narrate the code.
- **Every non-obvious constant cites its source**: `etp_server.c:5229`,
  `everything-syntax.md L163`, `design §5.2`, ref `D3`.
- **One decision per place.** A judgement call belongs in one function with one
  comment. When a second place needs the same decision, call the first — the
  `bs_alloc_full` helper exists because three places had to agree that a subtree
  result starts as "everything", and getting it wrong in one of them made
  `!folder:` return the whole table.
- **`static` by default.** Non-static only for the API in a header.
- **No new dependency without a design entry.** D5 ("no SQLite, everything
  self-built") and D2 ("C11") both turn on this; the regex engine exists partly
  because pulling in libpcre2 would contradict them.
- **Failures are visible.** A `LOGW` the first time a condition is hit, not once
  per row. `qexec` returns -1 on allocation failure rather than a partial result.

### 6.1 Memory

- One allocation per query for the sorted id array, one per page for the slice.
  Free on every exit path — `qctx_done()` exists because a missed one leaked 64
  KiB per query and the symptom was three empty test results.
- `malloc` failures propagate; they do not fall back to a truncated answer.

### 6.2 Line endings

LF in the shell scripts (`*.sh`) — that is the only place it is load-bearing.
A Windows editor reintroduces CRLF and the symptom is `bash\r: No such file or
directory`, i.e. the script cannot run at all. Nothing else cares: the C sources
are compiled and the Markdown is read, so their line endings are whatever git
already has, and checking them is noise. If a script suddenly fails to run, look
here first.

---

## 7. Where to look

| Question | File |
|---|---|
| what is this project for | `README.md` |
| why is it shaped this way | `docs/design.md` — §2 capability map, §9 upstream citations, §11 decisions |
| the query language | `docs/everything-syntax.md` (Everything's own spec) |
| what plocate and FSearch do | `docs/upstream-notes.md` |
| storage contract | `esidx.h` |
| query surface | `syntax.h` |
| protocol options | `etp.h` |
| how to check the server against a real client | §1.4 |
| where a number quoted against the reference comes from | `./cmp_ref.sh` — it re-measures all of them in one run |
| what a refresh costs, and what it is allowed to cost | `./refresh.sh <root> [--add N]` — idle pass, adding pass, snapshot write |
| what is left | `docs/design.md` §10 — it carries the current measurements |

## 8. Before committing

- [ ] `make` clean of warnings, `-Wall -Wextra`
- [ ] `make check` green — both suites against both builds, which is the whole gate
- [ ] `./round.sh /etc` runs and its numbers are sane
- [ ] new assertions have an expected value that came from `find(1)` or a fixture
      a reader can check, not from running the code
- [ ] any number quoted against `:21` is in `./cmp_ref.sh`'s term list, and
      `./cmp_ref.sh` agrees with what the message claims
- [ ] measurements in the message, with the previous value if there was one
- [ ] `docs/design.md` §10 updated if a component changed state
- [ ] any deviation from the reference server or the client has its reason written
      next to the code
