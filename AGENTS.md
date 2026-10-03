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
| `test_etp.sh` | the **wire**, against the Android client's own parsing rules | a reply would not be understood by the client |
| `round.sh` | nothing — it is the end-to-end demonstration and the source of the numbers in the docs | (prints timings; asserts nothing) |

Run the index suite before the protocol suite. A parse regression shows up as a
protocol failure otherwise, and you will spend an hour in the wrong file.

### 1.4 Commit per batch

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

`tools/etp_probe.c` is not a generic FTP client. It is a transcription of
`EtpClient.java`'s parsing, including the parts that are fragile:

- a reply ends at `nnn` + space and continues on `nnn` + hyphen; anything shorter
  than four characters also terminates (`:430-436`);
- the query block does **not** use that rule — it keys on the literal
  `200-Query results` and `200 End.` (`:302`, `:314`);
- `RESULT_COUNT` is parsed independently of every column toggle (`:320`);
- a numeric field that overflows `Long.parseLong` leaves the accumulator at its
  initial value, because the exception is swallowed (`:341`).

If the probe understands a reply, the client does. When you change anything on the
wire, transcribe the corresponding rule rather than writing a looser check.

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

The consumer is `ShareToPC`'s `EtpClient.java`. When a protocol or language
question arises, the answer is in that file, and the citation goes in a comment.
Its behaviours that are not obvious and that this server matches deliberately:

| Behaviour | Where | Consequence here |
|---|---|---|
| sends **bare** `EVERYTHING <sub>`, never `SITE EVERYTHING` | `:144-151` | both spellings route to one dispatcher |
| never opens a data connection | — | browsing is `parent:` + `folder:`, so `parent:` must be O(1) |
| joins `path + "\" + name` | `:522-528` | the PATH column uses backslash separators; `normalise_path()` accepts either on the way in |
| swallows `NumberFormatException` on SIZE | `:341` | the unknown-size sentinel makes it show no size, which is what we want |
| `RESULT_COUNT` is the total, not the page | `:321` | the executor returns the full sorted set and the protocol layer slices |
| `COUNT` is only sent when positive | `:260` | the default must be "unlimited", matching `etp_server.c:1207` |
| closes the socket right after QUIT without reading | `:460` | a clean EOF is normal, not an error |
| cannot parse the reference server's own FEAT reply | `:430-436` | do not "fix" the server around it; the client never sends FEAT |

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

LF, in every file including the shell scripts. Windows editors reintroduce CRLF
and the symptom is `bash\r: No such file or directory`. If a script suddenly
fails to run, check that first.

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
| what is left | `docs/design.md` §10 — it carries the current measurements |

## 8. Before committing

- [ ] `make` clean of warnings, `-Wall -Wextra`
- [ ] `./test.sh` and `./test_etp.sh` pass
- [ ] both pass again under `make DEBUG=1` with `ASAN_OPTIONS=detect_leaks=1`
- [ ] `./round.sh /etc` runs and its numbers are sane
- [ ] new assertions have an expected value that came from `find(1)` or a fixture
      a reader can check, not from running the code
- [ ] measurements in the message, with the previous value if there was one
- [ ] `docs/design.md` §10 updated if a component changed state
- [ ] any deviation from the reference server or the client has its reason written
      next to the code
