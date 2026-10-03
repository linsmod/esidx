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
| `round.sh` | nothing — it is the end-to-end demonstration and the source of the numbers in the docs | (prints timings; asserts nothing) |
| `cmp_ref.sh` | nothing — it re-measures every expected value quoted against the reference server (§1.4) | (prints a table; `--strict` exits 1 on a row the index delta does not explain) |

Run the index suite before the protocol suite. A parse regression shows up as a
protocol failure otherwise, and you will spend an hour in the wrong file.

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

Development host is Windows; the project builds and runs in WSL2. There is no
Linux-native setup and none is needed.

```sh
# from PowerShell
wsl -u root -e bash -lc "cd /mnt/c/$PWD && make && ./test.sh"
```

`-u root` is required: `/root` is mode 700 in this image, and the trees the
upstream survey cites live under it.

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

---

## 3. Tests

### 3.1 Both suites, both builds, every time

```sh
make clean && make && make etp-probe
./test.sh && ./test_etp.sh

make clean && make DEBUG=1 && make etp-probe
ASAN_OPTIONS=detect_leaks=1 ./test.sh
ASAN_OPTIONS=detect_leaks=1 ./test_etp.sh
```

The ASan build is not optional. It is the only thing that catches:

- **use-after-free hidden by -O2.** The freed block still holds its bytes, so an
  optimised build passes while a use-after-free on a compiled regex sits in
  `text_match`. It was caught only because the DEBUG suite ran.
- **leaks that swallow buffered stdout.** A 64 KiB path buffer leaked per query
  made three query tests fail with *empty output* rather than a leak report,
  because ASan aborts before stdout is flushed. If a query test suddenly returns
  nothing under `DEBUG=1`, look for a leak before looking for a logic bug.
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
- **The in-memory text scan is the remaining cost, and §5.2's 10⁶-entry
  activation threshold is too optimistic.** A wildcard over 78 296 candidates
  cost 23.8 ms of a 24.3 ms query at 10⁵ entries. Revisit at P4 — the name-sorted
  and reversed-name arrays are cheaper than a trigram index and would serve
  `startwith:`/`endwith:` directly.
- **`path_of()` is O(depth) with an allocation per call** (design §12 risk 7). It
  is fine for display and for the current sort volume; the path-sort cache in
  `query.c` bounds the repeat cost when it is not.

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
the same place a future reader will look.

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
| what is left | `docs/design.md` §10 — it carries the current measurements |

## 8. Before committing

- [ ] `make` clean of warnings, `-Wall -Wextra`
- [ ] `./test.sh` and `./test_etp.sh` pass
- [ ] both pass again under `make DEBUG=1` with `ASAN_OPTIONS=detect_leaks=1`
- [ ] `./round.sh /etc` runs and its numbers are sane
- [ ] new assertions have an expected value that came from `find(1)` or a fixture
      a reader can check, not from running the code
- [ ] any number quoted against `:21` is in `./cmp_ref.sh`'s term list, and
      `./cmp_ref.sh` agrees with what the message claims
- [ ] measurements in the message, with the previous value if there was one
- [ ] `docs/design.md` §10 updated if a component changed state
- [ ] any deviation from the reference server or the client has its reason written
      next to the code
