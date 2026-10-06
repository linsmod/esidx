# esidx architecture design

Scope, structure and rationale for the ext4 search index engine. The protocol
and query-language baselines are [`everything-syntax.md`](everything-syntax.md)
and etp_server 1.0.2.5; the upstream source reading that justifies the
per-decision choices is [`upstream-notes.md`](upstream-notes.md).

Throughout, **§n** refers to a section of this document and `[An]`…`[Gn]` to the
reference tables in §9.

---

## 1. Protocol baseline

### 1.1 `SITE EVERYTHING` subcommands

Source: `../../etp_server-1.0.2.5/src/etp_server.c:3951-4246`
(`etp_server_client_everything()`).

| Group | Subcommands | Lines |
|---|---|---|
| Match options (11) | `case` `whole_word` `path` `diacritics` `prefix` `suffix` `ignore_punctuation` `ignore_whitespace` `regex` `hide_empty_search_results` `search` | 3953-4023 |
| Secondary filter (11) | `filter_search` `filter_case` `filter_diacritics` `filter_prefix` `filter_suffix` `filter_ignore_punctuation` `filter_ignore_whitespace` `filter_path` `filter_regex` `filter_whole_word` | 4030-4149 |
| Sort / paging (3) | `sort` `offset` `count` | 4163-4190 |
| Result columns (7) | `size_column` `attributes_column` `date_modified_column` `date_created_column` `path_column` `file_list_filename_column` `date_recently_changed_column` | 4197-4239 |
| Execute (1) | `query` | 4246 |

32 in total.

> **The filter group is a second, independent matcher layer**, not a variant of
> the primary search: `c->filter_flags` plus `c->filter_search` are set by their
> own subcommands and applied after the primary search produces its result set.
> The execution model must be two-stage: primary search → candidate set →
> filter re-match → sort → slice.

### 1.2 Sort keys

Source: `etp_server.c:472-494` (`etp_server_sort_name_to_ids[]`) — 22 entries,
11 properties × ascending/descending:

`name`, `path`, `size`, `inverse_size`, `extension`, `date_created`,
`date_modified`, `attributes`, `file_list_filename`, `date_recently_changed`.

`inverse_size` maps to the same property as `size` with the direction inverted
(`:480-481`): it is the "smallest first" alias. Implementation reuses one sort
key and flips the flag.

### 1.3 FTP command set

`SIZE` `MDTM` `LIST` `PASV` `CWD` `PWD` `XPWD` `OPTS` `FEAT` `SYST` `CDUP`
`EPSV` `EPRT` `MLSD` `MLST` `EVERYTHING` `SITE` — enumerated in the comment at
`etp_server.c:50-68`.

### 1.4 Query language

Full operators, wildcards, macros, 22 modifiers, 40+ functions, 12 comparison
forms, size/duration/date syntax and constants, 17 attribute constants, and the
filename components. Catalogued in
[`everything-syntax.md`](everything-syntax.md).

---

## 2. Capability → index requirement

Four tiers. **L0 and L1 must be index-backed** or interactive browsing is not
viable; L2 may scan in memory until the entry count justifies an inverted index;
L3 scans or uses a specialised index.

### L0 — structural, O(1) / O(log n)

| Capability | Mechanism | Complexity |
|---|---|---|
| `parent:"P"` | directory tree `dir_id → children[]` plus a `path → dir_id` hash | O(1) |
| `depth:N` | inline `depth` column | O(1) |
| `root:` | `parent_id == ROOT` | O(1) |
| `child:"X"` | resolve `X` to a candidate set, then take its parent set | depends on L2 |

### L1 — scalar / enum, via sorted arrays or bitmaps

| Capability | Mechanism | Notes |
|---|---|---|
| `size:` incl. ranges and constants | sorted array `(value, entry_id)`, binary-search the bounds | constants expand to numeric intervals first |
| `dc:` `dm:` `da:` `dr:` | one sorted array per column | date constants evaluate to `[start, end)` first |
| `file:` `folder:` | bitmap | cardinality 2, whole-table bitmap |
| `ext:jpg;png` | bitmap per extension | low-cardinality enum |
| `type:`, `audio:`, `image:`, … | macro expands to an extension set → bitmap OR | |
| `attrib:h` | bitmap per attribute bit | 17 bits |
| `index-type:` | bitmap | |
| `empty:` | `size == 0` (file) or `child_count == 0` (folder) | |
| `child-count:`, `child-file-count:`, `child-folder-count:` | inline aggregate on the directory entry | must bubble up the parent chain on every mutation |
| `run-count:`, `frn:` | inline column / direct equality | |

### L2 — text, in-memory scan first

| Capability | Mechanism | Notes |
|---|---|---|
| substring (default) | in-memory array + `strcasestr`; trigram index above the threshold (§5.2) | |
| `prefix:`, `startwith:` | binary search over a name-sorted array | a prefix is directly bisectable |
| `suffix:`, `endwith:` | binary search over a **reversed-name** array | one extra column of space, buys the query |
| `whole:`, `ww:` | exact-name hash | O(1) |
| `regex:` | none; PCRE2 per entry | can pre-filter on literal trigrams extracted from the pattern |
| `path:`, `path-part:` | trigram index over paths, separate from the name index | |
| `stem:`, `name:` | substring over the normalised form | |
| `len:` | inline `name_len` column | |
| `ignorepunc`, `ignorews`, `diacritics` | normalisation at match time; never affects the index | needs ICU for diacritic folding |

### L3 — content and metadata, scan or dedicated index

| Capability | Mechanism | Notes |
|---|---|---|
| `content:` | full-text inverted index (FTS5 trigram or self-built) | built offline, asynchronously |
| `dupe:` | size bucketing, then a content hash within each bucket | reuse §5.3 to narrow first |
| `filelist:`, `filelist-filename:` | external list → temporary set, intersected with the candidates | |
| media metadata: `album:` `artist:` `title:` `genre:` `comment:` `tag:` `track:` `length:` `width:` `height:` `bitdepth:` `orientation:` `star-rating:` | sparse attribute table, loaded on demand | only media files have entries; keeps them out of the main table |
| `si:` | no Linux equivalent — parse it, reject at execution | |

---

## 3. Layering

```
┌───────────────────────────────────────────────────────────────────────┐
│ protocol  FTP (USER/PASS/TYPE/PASV/EPSV/EPRT/LIST/MLSD/MLST/RETR/…)  │
│                          + SITE EVERYTHING <32 subcommands>          │
├───────────────────────────────────────────────────────────────────────┤
│ syntax     Lexer → Parser → AST (operators, functions, modifiers,     │
│                                    constants)                          │
├───────────────────────────────────────────────────────────────────────┤
│ optimiser  selectivity estimate (histograms) → choose driver index →  │
│            execution plan                                             │
├───────────────────────────────────────────────────────────────────────┤
│ execution  bitmap set ops + matcher table + multi-level sort + paging │
├───────────────────────────────────────────────────────────────────────┤
│ index      dir tree │ name/path trigram │ numeric sorted arrays │     │
│            enum bitmaps │ content index │ sparse metadata │ aggregates│
├───────────────────────────────────────────────────────────────────────┤
│ storage    columnar attribute table (fixed-width columns) + name/path │
│            string pool + sparse K/V table                             │
├───────────────────────────────────────────────────────────────────────┤
│ collection full scan (getdents64) + incremental (fanotify, dir mtime)│
└───────────────────────────────────────────────────────────────────────┘
```

Implemented bottom-up: `collection`, `storage`, `index`, `execution`. The top
three layers do not exist yet (§11).

---

## 4. Storage layer

### 4.1 Columnar attribute table

Rejected FSearch's variable-length entry record [C1] in favour of columns:
L1 needs to sort and scan per attribute, and columnar is cache-friendly for
that; sparse attributes are better expressed as a bitmap or a K/V side table
than as per-entry flag dispatch.

```c
/* esidx.h:43 — the main table. One dense entry_id, randomly addressable. */
typedef struct {
    uint32_t  count, cap;

    eid_t    *parent;   /* parent eid; a directory's own eid doubles as its dir_id */
    uint16_t *depth;    /* uint8_t was too narrow for the 512-level scan cap */
    uint16_t *flags;    /* EF_DIR | EF_HIDDEN */
    int64_t  *size;
    int64_t  *mtime;
    int64_t  *ctime;
    uint16_t *ext_id;   /* interned into the ext pool; 0 = none */
    strref_t *name;     /* {off,len} into the names pool */
} entry_table_t;
```

Still to add for full capability coverage: `atime`, `recently_changed`,
`child_count` / `child_file_count` / `child_folder_count`, `run_count`, `frn`
(inode number on Linux), and a `path` column if paths stop being rebuilt from
the parent chain (§4.2).

### 4.2 String pool

Names — and paths when they are materialised — live in one growable `char`
buffer addressed by `{offset, len}`. One `malloc` for the whole pool instead of
one per entry.

Following plocate's block-compression idea [A5][A6][A8][A11] is deferred: with
the index resident in memory there is nothing to decompress on the query path.
Compression only pays once the dataset no longer fits in RAM (P4, and only above
~10⁷ entries). See decision D4.

`path_of()` (`store.c:153`) currently rebuilds a full path by walking the parent
chain, the way FSearch does. For *display* that is acceptable and saves the
storage. For `path:` queries at scale it becomes the bottleneck and the path
column will have to be materialised — see §2 L2.

**Extension names are interned through a hash, not found by a scan.** The numbering is
a position in interning order, which is a position in the append-only pool, so a lookup
only has to answer "have I seen this name before" — and until now it answered that by
`strcmp`-ing every name interned so far. The cost therefore grew with the number of
*distinct extensions* rather than with the number of files, which is the wrong axis for
a scan that runs once per file. Measured on `/work` (`r7000`, `-O2`, the run
`./ledger.sh /work` prints): **4 217 609 interning calls, 2 189 959 017 string
compares — 519 per call — and now 4 895 892, 1.16 per call.** The estimate this
replaces said "~13 G strcmp"; the measurement is 2.19 G, so the number that had been
carried in a comment for three commits was 6x out, in the direction that made the fix
look more urgent than it was. `esidx_log_stats()` prints both counters, because a claim
about a loop should be re-measurable rather than remembered.

Two off-by-ones live in that function and both were written here first, which is why
the counters are in the log rather than only in this paragraph:

- the slot holds `ext_id + 1` and `ext_id` is 1-based, so the rehash writes `i + 2` for
  the `i`-th offset. Writing `i + 1` makes every lookup resolve one id low, which is
  *another extension's rows*, not an error.
- the lookup reads `ext_off[id - 1]`, because the slot is `id + 1` and the array is
  indexed from 0. Reading `ext_off[id]` never matches, so every lookup of a name that
  was already interned appended a second copy of it and gave the rows a second id:
  1 181 distinct extensions over 1 200 files on a fixture with 400 of them, and
  `ext:e1` matching nothing at all.

The table is derived and not persisted (D4): a load refills it from `ext_off`, which is
already the numbering, sized from the extension count so the rehashes the build paid on
the way up are not paid again on the way in. `ext:`'s id list is a separate matter and
has its own entry in §10.

**Extension names are addressed by a dense id, not by their offset.** They were
addressed by offset, as a `uint16_t`, and a pool past 64 KB wrapped one extension's
offset onto another's: two extensions shared an id, so `ext:` answered with rows
carrying neither, and `ext_index_count()` — the exact cost design §6.2 ranks leaves
by — was wrong for the affected ids. Measured on `/work` (a 68 KB pool) as ~30
extensions, and on a 2 200-extension fixture as 152, where `ext:e…005` matched both
`f5.e…005` and `f1477.e…001477`. `ext_off[]` now holds the 32-bit offsets and an id is
its position in interning order, so it cannot wrap; a dense id cannot realistically
exceed 65 535 distinct extensions, so the entry column stays 16 bits. The id table is
rebuilt on load by walking the pool, which *is* the numbering — a name's id is where it
sits in an append-only pool — so nothing derived from ids is persisted (D4).

Snapshot **version 3**. The ext column is the same width in the same place, so a
version-2 snapshot would load without complaint and then resolve extensions to the
wrong strings; the version check is the only thing separating the two, which is what
it is for.

Extensions are **not truncated**: an extension is a suffix of a name, so `NAME_MAX`
bounds it, and the one constant that says so (`EXT_NAME_MAX`) is shared by the writer
and the reader of a query's extension list. They kept 31 and 63 characters
respectively, which is worse than lossy — `ext:` for a real extension matched nothing,
and two extensions sharing their first 31 characters matched each other's rows. Real
trees hit it: `/usr` has one entry with a 34-character extension, `/work` has 709, and
the longest on `/work` is 74 characters. Before the fix the index held 6 508 distinct
extensions; it holds 6 765, because names that used to be merged by the cut are
separate extensions and 249 of them are singletons.

### 4.3 Sparse metadata table

Media metadata and content-index offsets live in a K/V table keyed by
`entry_id`:

```
entry_id -> { album?, artist?, title?, genre?, comment?, tag?, track?,
              length?, width?, height?, bitdepth?, orientation?, star_rating? }
```

Only media files have an entry. Open-addressed hash, or an `entry_id`-sorted
array with binary search.

---

## 5. Index layer

### 5.1 Directory tree — self-built

```
path_to_id : hash<const char*, eid>       open addressing (store.c:243)
children   : eid -> eid[]                 growable vector (store.c:118)
```

**Why self-built.** FSearch has a `parent` pointer and no children list, so
listing a directory is a full scan; plocate has no directory concept at all.
Interactive browsing issues `parent:` on every navigation, so O(1) is not
optional. See [F1].

The hash table is sized to 4× the directory count and probed linearly. Robin
Hood swap [A12] was considered; at this load factor (≤ 0.25) the longest probe
is logged instead (`store.c`, LOGD) and the swap is not yet implemented. Revisit
if the load factor rises.

### 5.2 Name / path trigram inverted index — from plocate

| Parameter | Value | Source |
|---|---|---|
| Block size | 32 filenames per block | [A5] |
| Trigram unit | **bytes**, not codepoints | [A13] |
| Posting encoding | delta-1 + PForDelta 128, **of which only delta-1 is used** | [A10] |
| Block layout | interleaved for full blocks, plain for the tail | [A11] |
| Bit-width selection | histogram + byte-cost enumeration | [A14] |
| Trigram generation | 4-byte `memcpy`, take the low 3, advance 1 | [A9] |
| Hash table | Robin Hood | [A12] |
| Compression | zstd dictionary trained on the corpus | [A6][A8] |

**Activation threshold**: ~~above 10⁶ entries~~ — **wrong by an order of
magnitude, and measured**. At 10⁵ entries the in-memory scan was already 23.8 ms of
a 24.3 ms query on WSL2 and 27.2 ms of a 32.3 ms one on real hardware, so the
threshold was never going to be reached before the cost was already felt. The name
half is built unconditionally now; §10 carries the numbers. The path half is not,
and §10 says what is left of that query without it.

**Mandatory fallback**: names shorter than 3 bytes produce no trigram [A9], so a
full-scan path must remain for short queries. Kept, and it is not a corner: a
two-byte term has no trigram to look up at all, so `on` and `on*` still scan.

**Prefix / suffix** are cheaper as binary searches over sorted name and
reversed-name arrays than as trigram lookups. Not built; §10.

#### 5.2.1 What the name half actually is

Everything in the table above belongs to a *persisted* index: a block of 32
filenames is the unit of I/O and of compression, `docid` is a block number [A7],
and the posting encodings exist to make a block cheap to store and to seek. D4 says
this index is memory-resident, so none of that has anything to do here and adopting
it would be cost without benefit:

| Kept | Why |
|---|---|
| **Byte** trigrams [A13] | the one rule with a correctness reason: it is what keeps a CJK filename from producing thousands of postings, and it is what makes a CJK term searchable at all |
| The longest literal run of the pattern | Everything anchors a wildcard to the whole filename, so every literal run has to occur in the name. The longest one is a necessary condition, which is all a filter may be |
| Full scan below 3 bytes [A9] | the fallback the threshold note above is about |
| **delta-1 postings [A10]** | added after the first version of this table, and the reason it was left out is worth keeping because it was wrong. "Resident, so there is nothing to decompress" is true and is the wrong question at this size: on /work the posting lists were 318.6 MiB of a 1 006 MiB index, and what decides whether they are worth compressing is how many bytes a *query* touches, not how many a load inflates. The delta half needs no block to be useful — the gaps inside a list ascend, so a gap is usually one byte — and the one reader walks a list front to back, which is the shape delta-1 is best at. Measured: 318.6 → 101.8 MiB, and no query slower. The PForDelta half of [A10] still needs a block, so it is still dropped |

| Dropped | Why |
|---|---|
| Blocks of 32 [A5], `docid` = block number [A7], interleaving [A11] | a unit of compression and of I/O; there is no I/O on the query path |
| PForDelta, bit-width selection [A10][A14] | both need a block of postings to be worth anything, and a block is on the dropped list above. delta-1 with a varint width is the same idea one posting at a time, which is the granularity that exists here |
| zstd dictionary [A6][A8] | deferred to P4 in the original table too, and D4 says why: nothing to decompress |
| Robin Hood [A12] | the table holds ≤ 34 k keys at 3.7 × 10⁵ entries, so the longest probe is not worth an implementation; linear probing at a load factor of 0.75 keeps it short. The dir hash measures its probe length for the same reason (§5.1) |
| CRoaring (D7) | a posting list is an ascending id array and the intersection is a merge; roaring accelerates set algebra on dense ids, not a sorted merge |

**It is a filter, never a decision.** That is the whole safety argument, and it is
why `query.c`'s `tri_applies()` is an allowlist rather than a denylist: the subject
must be the display name, the folding must be the C locale's, and the trigram must
be a necessary condition. A shape that cannot prove all three is scanned.

**Posting lists are ascending without being sorted**, because `finalize` walks ids
in order and `esidx_add` only ever hands out a larger one (D8). The intersection is
therefore a merge against the candidate bitmap rather than a sort. Removals are not
unpublished — a dead id is stopped by the `live` set every query seeds from, and
`esidx_compact()` rebuilds, which is the same bargain `ext_index_del()` makes.

**The ids are delta-varint encoded**, and ascending is what makes that pay: a gap
inside a list is usually one byte, so a posting costs 1.28 bytes against 4 on /work.
The first posting of a list has no predecessor and is stored whole, as a varint like
any other. The encoding is only affordable because the one reader never seeks —
`tri_index_filter()` walks the chosen list with a cursor that moves forward — and it is
also the only thing that reads the buffer; `esidx.h` holds the shape and `trigram.c`
owns the format. §10 carries the measurements, the query cost and the test that reaches
each varint width.

**The path half is a separate index and is not built.** `path:` reads a path
rebuilt from the parent chain, so materialising one trigram per path per entry is
O(n · depth) at build time and buys back `path_of()`'s per-row cost only if the
path term is common. §10 measures what is left without it.

### 5.3 Numeric sorted arrays

One `(value, entry_id)` ascending array per numeric column; a range query
binary-searches the bounds and yields the candidate interval directly.

Insertion into a sorted array is O(n) memmove, which fanotify-driven
incremental updates cannot tolerate [F2]. Three options were weighed:

| Option | Update | Query | Verdict |
|---|---|---|---|
| sorted array | O(n) | O(log n + k) | fine for batch rebuild, not for live updates |
| tiered bitmaps (size bucketed by powers of two) | O(1) | several bitmap ORs, then a second filter pass | cheaper updates, superset results, space grows with bucket count |
| sorted array + unsorted delta buffer (LSM-style) | O(1) | O(log n + \|delta\|) | **chosen** — smallest implementation, standard for read-heavy ordered ranges |

See decision D3.

#### 5.3.1 An empty sorted array is legal, and it used to answer zero rows

`by_size`, `by_mtime` and `by_ctime` are derived (D4), so "do not build this one" costs
nothing to express: the next load rebuilds whatever the process did not ask for, and the
snapshot never has to be rewritten. `ESIDX_SKIP_INDEX` does it today as a mask on the open
index — deliberately *not* a field in the file, because the file is data and this is a
choice about one process.

The price of that freedom is a contract: **every reader of a skipped structure must
degrade to something slower and correct.** `sidx_range_to_bitset()` over an empty array
sets no bits, so a valid `size:>1k` answered **zero rows**, with no error, no warning and
exit status 0 — indistinguishable from a search that matched nothing, which is the failure
class §5.3 of AGENTS.md calls the worst. It was worse than a plain bug because the two
halves disagreed: `est_leaf()` already refused to seed a candidate set from an empty array
(`if (!ok || !s->n) return UINT32_MAX`), so the optimiser knew the leaf was unusable while
the executor went ahead and emptied the answer. **Partial correctness reads as robustness.**

`range_on()` now falls back to the column, which is the same shape `depth:`/`len:`/
`child-count:` have always used — the value is in the column whatever the array did — and
logs the degradation per query at DEBUG, because an answer that is right for the wrong
reason is exactly what this section is about.

**How it is configured**, three sources deep, and the order is the argument rather than an
accident — the flag is this invocation's decision and the sidecar is the file's, so a
stale sidecar must not silently override a flag somebody typed:

| source | spelling | note |
|---|---|---|
| flag | `--no-index[=LIST]`, on either side of the subcommand | stripped from `argv` in `main()`, the way `log_strip_flags()` removes `-v` |
| sidecar | `<dbfile>.opts`, one name per line, `#` comments | beside the snapshot: options belong to a file, so two indexes of the same tree can differ |
| environment | `ESIDX_SKIP_INDEX` | lowest; what the test suite drives |

`--no-index` bare means "read the sidecar" and `--no-index=` means "ignore it", which is
only expressible because an empty list is a setting rather than a missing argument.
`esidx options <dbfile>` prints the resolved answer and its source **without opening the
snapshot** — a 300 MB file must not have to be read to be asked a question about
configuration, or nobody runs it before changing a setting.

**The flag is stripped from `argv` rather than read in place**, and that detail is a bug
this section had: the query loop treats every argument that is not `sort:`/`count:`/
`offset:` as search text, so `--no-index=size size:>1k` answered **zero rows** — the flag
worked perfectly and its own text was then ANDed into the query as a term matching no
filename. The symptom pointed at the option being ignored, which is the opposite of what
was wrong. One `idx_strip()` in `main()` now removes it before any subcommand sees it, and
an unrecognised `--` argument on the query line is an error rather than a term.

**What it saves, on `/work`** (5 476 485 entries, `./ledger.sh /work`, which now prints the
configuration next to the numbers because a memory figure whose configuration is not in
the same run is not reproducible):

| `--no-index=` | accounted | peak rss | saved |
|---|---|---|---|
| *(nothing: all five)* | 789.2 MiB | 862.1 MiB | |
| `size,mtime,ctime` | 601.2 | 669.4 | 188.0 MiB |
| `trigram` | 687.4 | 761.2 | 101.8 |
| `rank` | 725.4 | 749.2 | 63.8 |
| `trigram,rank` | 623.6 | 664.5 | 165.6 |
| all five | **435.6** | **455.2** | **353.6** |

47 % of peak rss, **and the snapshot is byte-identical throughout** — 313 011 674 bytes
and the same md5 with every index on and with all five off, which is D4 saying out loud
that the configuration cannot reach the file. It is also why `esidx_compact()` copies the
mask into its scratch index rather than re-resolving it: a compaction run by a process
that was told not to build the trigram index must not quietly put 100 MiB of it back.

**What it costs per query**, `./tri-skip.sh /usr 5`, one snapshot serving both
configurations, the matched row count printed from both sides on every line so a "faster"
row that answered something else could not pass for a trade:

| skipped | query on /usr | rows | eval | sort | total |
|---|---|---|---|---|---|
| — | `size:>10mb` | 426 | 0.029 → **1.273** | 0.065 → 0.064 | 0.13 → 1.37 ms |
| — | `size:<1k` | 138 984 | 0.619 → 1.949 | 23.54 → 23.74 | 24.5 → 25.7 ms |
| — | `size:>1k` | 233 021 | 1.169 → 2.342 | 41.36 → 40.49 | 43.1 → 42.9 ms |
| — | `dm:today` | 4 | 0.024 → **1.270** | — | 0.11 → 1.35 ms |
| — | `dc:>2000` | 372 084 | 1.783 → 2.079 | 68.46 → 67.66 | 70.3 → 69.8 ms |
| trigram | `conf` | 8 484 | 2.597 → **27.994** | 0.847 → 0.932 | 3.5 → 29.0 ms |
| trigram | `path:/usr *.conf size:>1k` | 466 | 20.661 → 44.762 | — | 21.3 → 45.4 ms |
| rank | `image: sort:name:asc` | 55 229 | 0.181 → 0.179 | 7.550 → **21.225** | 7.8 → 21.5 ms |

(r7000, best of 5, `-O2`.) Three things fall out of that table, and they are why the
trade is worth exposing rather than settling in the code:

- **A range index is worth its memory only where the range is selective.**
  `size:>10mb` is 10.5x without it; `size:>1k`, which matches 233 021 of 372 084 rows,
  is **free** without it — the 41 ms sort over the whole table dwarfs the 1.2 ms the
  range saved, and the total actually came out 0.2 ms *faster*. So "62.7 MiB for
  `by_size` on /work" is not a cost that has to be justified in general; it buys the
  selective shapes and nothing else.
- **A selective range pays twice.** Seeding is what keeps the *sort* small as well as the
  eval, so removing the array costs twice over on `size:>10mb` — though on a 426-row
  result the second term is only 0.065 ms.
- **The rank is a sort cost, not an eval cost**, so its 2.8x is invisible in eval
  (0.181 → 0.179) and all of it is in the sort (7.55 → 21.2 ms). A harness that printed
  only plan/eval would have reported this option as free.

### 5.4 Enum bitmaps

`is_dir`, `ext_id`, each `attrib` bit, `index_type` each get a bitmap. The set
algebra (AND / OR / NOT) maps directly onto the language's `space` / `|` / `!`
operators.

Dense `uint64_t` words for now, swapped for CRoaring later behind an unchanged
`bs_init` / `bs_set` / `bs_test` / `bs_clear` ABI — decision D7.

**The extension half of this is a posting list, not a bitmap, and only the broad
extensions kept one.** `ext_index` used to give every extension a bitmap sized by the
*table*, so the cost was a product — one bitmap per extension, `n` bits each — and the
extension count grows with the tree, so the product grows faster than the tree does. The
original argument was "extensions are few and low cardinality" (1 580 over 372 084 entries
when written) and never recorded where the product stops being affordable.

A slot is a posting list — 4 bytes per entry, ascending because ids are, the structure
§5.2 already uses for names — unless the extension is broad enough that *selecting* it as
a bitmap beats walking its ids. Two conditions, and both matter: `k > n/128` (a bitmap is
`n/64` word operations whatever the cardinality, a list is a call plus two bit operations
per id) and a bitmap of at least 512 bytes (a sub-page bitmap is not worth having, and the
floor is also what keeps a small tree on the list path — without it every fixture in the
suite would take the bitmap path and the list path would ship untested).

Measured on `r7000`, the tree both servers index, `VmSize` after `serve`:

| | `/usr` = 372 084 entries | `/work` = 5 476 485 entries |
|---|---|---|
| bitmaps | 16, 726 kB | 17, 11 MB |
| posting lists | 1 564, 272 kB | 6 748, 3 MB |
| `finalize: ext` | 2.6 ms | 51.6 ms (was 114.5) |
| `VmSize` serving | — | **1.77 GB, was 6.11 GB** |
| widest bitmap | `h`, 47 504 rows | `h`, 694 286 rows |
| widest list | `cmake`, 2 520 rows | `sha1`, 39 087 rows |

Select cost, both structures, three interleaved runs on `/work`: `ext:h` 2.389 → 2.391 ms
(it keeps its bitmap, so nothing moves) and `ext:sha1`, the widest list, 0.428 → 0.467 ms —
**9 % slower**, consistently, which says the true crossover is nearer `n/64` than `n/128`.
The threshold stays at `n/128` anyway: 0.04 ms on one term against 4.4 GB of address space,
and moving the constant needs a sweep over it rather than one point.

The old cost was never resident: a `calloc`'d bitmap only faults the pages its bits land on,
so 4 229 MB of bitmaps measured 213 MB resident against 6.1 GB of address space. It failed
where overcommit is refused or `ulimit -v` is set, which is the same failure mode as the
path sort in §10 that only appeared under a memory cap.

The cardinality is printed rather than assumed because it is the whole decision, and
because the first version of that print was wrong in a way worth recording: it bucketed
through a bounds table indexed one element past its end, `-O2` folded the last comparison
away, and `/work` reported `>=65536: 0` on the same line as `largest 694286`. The broad
extensions it had been hiding hold 3 019 986 rows — 55 % of everything with an extension —
which is exactly the mass a threshold has to be placed against.

A removal from a list slot is a count decrement and nothing else: the id stays in the list
because every query seeds from `live`, a removed id is not in it, and the intersection that
follows drops it. That is the bargain `tri_index` already makes, and the count has to stay
exact because §6.2 costs a driver leaf with it.

### 5.5 Aggregate columns

`child_count`, `child_file_count`, `child_folder_count` are stored inline on the
directory entry and **bubbled up the ancestor chain on every mutation** [B6].
This is the single easiest invariant to get wrong; FSearch's
broadcast-`ENTRY_DELETED`-then-`ENTRY_CREATED` pattern around the ancestors is
the reference.

### 5.6 Content inverted index

FTS5 with a trigram tokenizer, or self-built. Built offline and asynchronously;
must not block the primary index.

### 5.7 Duplicate detection

Narrow by `size` (reusing §5.3), then compare content hashes within each bucket.

### 5.8 Sparse metadata indexes

Numeric media attributes get sorted arrays; text attributes get trigram
inverted indexes.

---

## 6. Query layer

### 6.1 Parsing

`Lexer → Parser → AST`.

- Operators: AND (space), OR (`|`), NOT (`!`), grouping (`<` `>`).
- Functions: `f:value` plus the 12 comparison forms of §1.4.
- Modifiers attach to nodes as match parameters; `no` prefix disables, `?`
  prefix enables globally.
- Constants expand before planning: `tiny` → (0, 10240]; `today` → [today 00:00,
  tomorrow 00:00); `audio:` → an extension set.

### 6.2 Optimiser

1. **Selectivity estimate** — 128-bucket equi-height histogram plus a
   cardinality per column.
2. **Choose a driver index** — among all leaf conditions, take the most
   selective one that is index-backed and seed the bitmap from it.
3. **Order the remaining conditions** by (selectivity ↑, unit cost ↑); cheapest
   filters first.
4. **TopK** — when `offset + count` is far below the candidate count, maintain a
   bounded heap instead of a full sort.

**Implemented: step 2, and step 1 well enough to serve it.** No histogram is
needed yet, because for every leaf that can win the driver role the exact count
is already available in O(1) or O(log n), which is better than a 128-bucket
estimate would be:

| Leaf | Estimate | Cost |
|---|---|---|
| `parent:` | the children vector's length | O(1) |
| `root:` | the root's children length | O(1) |
| `file:` / `folder:` | the type bitmap's popcount | O(n/64) |
| `ext:` | the sum of the per-extension cardinalities, computed once in `finalize` | O(k) |
| `size:` / `dm:` / `dc:` | the same binary search the range query will do | O(log n + k) |
| text, `regex:`, `depth:`, `attrib:` | not index-backed, never a driver | — |

A `NOT` can never seed: its result is the complement, which is large. An `OR`
returns at least the sum of its operands, so it only wins when both operands are
poor. That is the whole of `pick_driver`, and the choice is logged at DEBUG so it
is inspectable rather than implicit.

Steps 3 and 4 are deliberately absent; §10 records why.

### 6.3 Execution

```
driver index → candidate bitmap
  → per-condition AND / OR / NOT over bitmaps
  → remaining non-indexable conditions via the matcher table
  → second-stage FILTER_* matchers over the surviving set
  → multi-level sort chain (primary attribute → name → path, so the order is total)
  → OFFSET / COUNT slice
  → emit the enabled *_COLUMN fields
```

Implemented today (`query.c:86`): candidate set from the directory tree or the
full table, bitmap intersection for `size:` / `dm:`, one matcher pass for type /
extension / substring, `qsort_r` with a multi-level comparator, then the slice.
No optimiser yet — conditions are applied in a fixed order.

### 6.4 Result cache

`etp_server.c:4250-4277` compares every query parameter against the previous
one and, on a match, re-sends the previous result set. Worth replicating: the
client repeats `QUERY` while paging, re-sorting and toggling columns.

Invalidate on any index mutation.

---

## 7. Collection layer

**Full scan.** `getdents64` into a large buffer instead of
`opendir`/`readdir` [A3]; `openat` on relative names to avoid `PATH_MAX` and
close the TOCTOU window [A1]; `O_NOATIME` with an `EPERM` retry for files we do
not own [A4]; `d_type` decides `is_dir`, with `fstatat` only on `DT_UNKNOWN`
[A3].

Unlike plocate we must `stat` every entry, because L1 indexes size and mtime.
plocate only stores paths and can skip stat entirely — so the stat success rate and
the getdents byte volume were tracked in `scan_stats_t` as the two numbers that would
decide whether the planned "batch stat by inode" optimisation is worth building. Both
are logged, the walk's time is now split per syscall at `-v 5`, and the answer is
**no**: on ext4 a stat costs 1.81 µs and no I/O, because the inode the directory block
just named is already resident — see §10, "Phase timings". The counters stayed, since
they are what would show a tree where that is not true.

**getdents buffers.** One buffer per recursion level, taken from a lazily grown
pool (`scan.c:61`). A per-frame local buffer would put
`depth × 128 KiB` on the stack — 64 MB at the 512-level cap. A single shared
static buffer would be clobbered by the recursive call while the parent frame is
still walking it. Both are wrong; the pool is neither.

**Depth cap.** 512 levels. Crossing it emits a warning naming the truncation
rather than silently dropping subtrees.

**Concurrency** (D6): adaptive to the medium, not yet implemented.

**Incremental.** Two passes over one walk, because a directory's mtime is a
statement about its *own* entries and nothing else:

| Pass | Stats | Correct for | `/usr`, idle | `/usr`, worst case |
|---|---|---|---|---|
| **names** | one per directory whose parent changed, plus every new file | a name change that moves a directory between the root and the name — **not** an isolated change at depth ≥ 2 (§12 risk 8) | **0.1 ms** (15 dirs) | 110 ms |
| **deep** | every entry | also size, mtime and ctime, and every name | 4.23 s (8 590 dirs) | 4.23 s |

Directory-mtime skip [A2] is the names pass: a directory whose mtime is unchanged
is not descended into. The deep pass exists because we store size, mtime and ctime
and plocate does not — those live on the *file*, so editing a file an hour ago
moves nothing its parent can see, and `dm:today` over that file has to be right.
Neither pass subsumes the other, and a build that ran only the cheap one would
answer every name query correctly and every date query wrongly.

Identity within a directory is the **name**, not the inode. Inode matching looks
cheaper and is wrong twice over: ext4 recycles inodes, so a deleted file and its
replacement merge into one row, and two hard links in one directory share an
inode and collapse into one row. Matching names costs a `strcmp` per entry against
children already in the index and makes both cases fall out correctly.

**Events.** The dirty set needs something to fill it, and the two candidates are
fanotify and inotify. What is built is neither directly: `sfa`, a separate privileged
proxy (a submodule), holds the fanotify group and rebroadcasts events as absolute paths
over a unix socket, and `watch.c` subscribes to it. The reason is privilege, not code —
`fanotify_init()` needs `CAP_SYS_ADMIN`, so a server that called it itself could only be
a root process, and an index server should not need to be root. `esidx serve
--watch[=SOCK]` is the client half.

The plan this replaces assumed three things that measurement contradicted, so the
mechanism was rewritten around what the kernel actually does (all of it re-derivable —
`sfa-server --probe` prints the negotiation for any machine):

- **One mark per directory is not needed, and `FAN_EVENT_ON_CHILD` does not mean what
  B3 assumed.** `ON_CHILD` is the immediate children of the marked object: a change two
  levels down is silent under an inode mark. What does cover a whole filesystem is
  `FAN_MARK_FILESYSTEM` *without* that bit, which reports every object on it.
- **`FAN_MARK_MOUNT` is not always available.** Measured on WSL2 (6.18): every name
  event is `EINVAL` on a mount mark, while a filesystem mark takes all of them
  (`--probe` prints `MOUNT 1/7, FILESYSTEM 7/7`). `sfa` negotiates per event bit and per
  mark type and uses whichever covers more, which is the only reason this works on a
  kernel where the upstream recipe fails.
- **`metadata_len` cannot be used to find the info record.** WSL2 under-reports it (24,
  the header size, while `event_len` is larger), so a reader that gates on it sees "no
  info record" and no file handle at all. Records must be walked by each record's own
  `len` (`sfa-server.c` `find_fid_info`), which is what the proxy does.

The rule the layer turns on is one sentence: **an event names an object, and the only
thing a reconcile can do about it is list the directory that holds its name**, so every
event marks `dirname(path)` — including a directory that has just appeared, because
`di_lookup()` resolves directories the index already has and a new one has no id to mark.
The parent's pass adds it and descends into it [B5]. Events under a deleted directory
resolve to nothing and are counted, not applied [B4]. A rename arrives as one event
with both paths (5.17+ `FAN_RENAME`), so both sides are marked and no cookie pairing is
needed.

What this layer does **not** cover is as much a part of its design as what it does.
`CLOSE_WRITE` and `ATTRIB` are deliberately not subscribed: listing a directory cannot
see an attribute change, so subscribing would buy a reconcile per write and still leave
size and mtime stale. **§12 risk 8 therefore stands unchanged** — the names become
current within one event batch, attributes still follow `--refresh`/`--deep` — and the
startup banner says so, because a first line that only mentioned freshness would be read
as more than it is.

Two facts about cost, measured (WSL2, `test_watch.sh`'s own fixture, 1000 files written
by 8 parallel processes into one directory):

- **A watcher that keeps up is a watcher that pays per event.** 1014 events arrived in
  1013 batches of ~1, because the drain loop is faster than the writer. The dirty set is
  de-duplicated per *batch*, so that shape costs one reconcile per event: 975 batches,
  975 directory listings, ~0.5 s of a serve loop that answers nobody.
- **So a batch waits for itself to finish growing**, 50 ms or 256 marks, whichever comes
  first (`WATCH_COALESCE_MS`). The same burst then costs 5 batches and 6 listings — 927
  marks, 154 per listing — about 6 ms of reconcile. The wait is invisible in a search
  box and the saving is two orders of magnitude under exactly the load that needs it;
  the cap is what stops the wait from becoming unbounded for a tree being written hard.

A proxy that goes away is reported and dropped, never treated as "no events": the index
is left as it stands and `--refresh`, if set, keeps repairing it. That is why
`esidx_watch_drain()` returns −2 for a dead peer and 0 for a drained batch — the same
`recv` reports both, and the difference between an index that is current and one that has
quietly stopped being current is that return value.

The periodic pass is kept, for two reasons that measurement gave rather than taste: it
covers whatever happened while the server was down (the subscription is created after
the snapshot is read, so a startup repair pass runs first), and it covers an event the
proxy could not attribute to a path.

**What the proxy loses is a signal now rather than silence, and the signal's answer is
narrower than the signal.** `sfa` reports two ways of not knowing: a kernel queue that ran
over (`SFA_EV_OVERFLOW`), and its own — a handle it could not turn back into a path, or
events it had to drop because one client was not draining (`SFA_EV_UNRESOLVED`, sfa issue
#2). Both arrive with an empty path and both are answered with a mark of the root.
Measured on the proxy that emits them: a 500-file `rm -rf` of a nested tree delivers 139
of 511 events — the rest are lost because the parent directory the event names no longer
existed when the event was read — and the client is told once per read batch instead of
never (`test_watch.sh`, "a rename the proxy cannot resolve is reported").

But a mark of the root is **a pass that follows moved stamps down from the root**, and
that is the whole of its reach: it repairs a bulk change, where every directory on the way
to it moved, and it does not repair a change buried under directories that never moved.
That is not a property of the watcher — it is the names pass's own limit (§12 risk 8).

### Sweep

So the loss signal, the startup repair pass and `--sweep=SECS` all answer with a **sweep**
instead: `esidx_sweep_dirs()` compares the stamp of **every** live directory against the
column, and marks the ones that differ. The walk then reconciles exactly those, so a
subtree nobody touched costs one stat and no `getdents` — which is what makes this
affordable where a full pass is not.

It is a comparison rather than a walk because that is the only way to ask the question. The
directory that holds a changed name is only *reached* through its parent, and its parent's
stamp did not move, so no amount of descending finds it; asking each directory directly is
the same question with the tree's shape taken out of the loop.

Cost, measured with this code on `r7000` (ext4 NVMe, `hpet`), warm, on an idle tree — the
number that decided the placement:

| Tree | Directories | Sweep | Pruned pass | Ratio |
|---|---|---|---|---|
| `/usr` | 34 810 | **76.6-77.1 ms** | 0.1 ms | ~770x |
| `/work` | 651 896 | **1593-1608 ms** | 0.2 ms | ~8000x |

Two things fall out of that table. The sweep is *linear in directories and nothing else*
(2.2-2.5 µs per directory on both trees), so it predicts rather than surprises: 1.6 s on
`/work` is what 651 896 directories cost, not a bad day. And the pruned pass is cheap
**because** it does not know, which is why the sweep cannot be the default — it would turn
a 0.2 ms timer into a 1.6 s one for every deployment, to fix a case that only a stopped
daemon or a lossy proxy produces. Hence the three places it runs, and no others:

- **once at startup**, before the listener opens, where nobody is waiting on a query yet.
  This is what makes "the server is up to date" true rather than approximately true, and it
  is the one place the cost is unconditionally worth paying.
- **after a proxy loss signal**, which is the only event that means "I do not know what
  changed". The coalescing wait is short-circuited for it: the lost events have no other
  route into the index.
- **`--sweep=SECS`**, for a deployment that wants the hole closed on a timer instead of at
  startup. Off unless asked for, and coarse by design — anything under a minute costs more
  than it can plausibly find on a tree this size.

Not built, and named because it is the obvious next question: a sweep that **yields** to
the serve loop instead of blocking it. 1.6 s is a 1.6 s stall in which no client is
answered, which is acceptable at startup and on a loss signal and is *not* what one wants
from a timer. The shape is known — the dirty set is already a set, so the sweep could mark
a bounded number of directories per poll turn and resume where it left off — and it is not
built because no configuration needs it yet.

**In place.** `esidx serve --refresh=SECS` runs the names pass inside the serving
process, once before the listener and then every SECS, so a long-running server
keeps its own index current instead of serving the snapshot it loaded. The serve
loop is single-threaded, which is what makes it safe rather than lucky — the pass
runs between two `poll()` turns, so no client is half-way through a command while
the index moves — and what bounds what a pass may cost: a pass that stalls the loop
stalls every client. So the serving pass refuses compaction (`EU_NOCOMPACT`,
`esidx_compact()` is a full rescan plus a `finalize`), and `--save=SECS` is a
separate knob from the refresh interval because a snapshot write is the whole file
and has nothing to do with how stale the index may be. `--watch` and `--refresh` are
independent answers to the same question, either one makes the server self-updating,
and either one makes `--save` meaningful. §10 has the three costs
separated, and they differ by three orders of magnitude.

**The dirty set.** "Which directories need a full listing" is a question, and
until now the only answer was "the root, and from there whatever a stamp says".
`esidx_mark_dirty()` appends a directory to a transient set in the index;
`esidx_refresh_dirs()` reconciles exactly that set and clears it; and
`esidx_update()` is the case where the set holds the root. There is no second
reconcile underneath, which is the property that matters: a partial refresh cannot
drift from a full one because they are the same code, and the test that pins it is a
byte comparison of the two snapshots (test.sh, "a dirty set of directories").

What it is for is not milliseconds and the measurement says so plainly. On an idle
tree the full pass is already the cheapest thing there is — 0.1 ms on `/usr`, 0.2 ms
on `/work` — because 16 of `/usr`'s 17 directories are skipped on their stamp, so
*refreshing one directory can be more work than refreshing the whole tree* (6.8 ms for
`--dir /usr/share/doc`, which has 3 200 subdirectories of its own, against 0.1 ms for
the full pass). The set earns its keep in the case the stamps cannot cover, which is
design §12 risk 8: a file edited in place moves nothing its parent's mtime can see,
so the only way to reach it is to have been told. Measured, `r7000`:

| | full pass | `--dir` one directory |
|---|---|---|
| `/work`, idle (5.5 M entries, 630 k dirs) | 0.2 ms — 49 dirs | 0.9 ms — 4 dirs |
| `/work`, add 2000 rows in one directory | 355 ms | 363 ms |
| `/work`, remove them again | — | **3.1 ms** — 2000 ids tombstoned |
| `/usr`, refresh one directory | 0.1 ms idle / **110 ms worst case** (§10) | 0.1-21 ms, by that directory's own size |

The two rows worth reading twice are the third and the last: a removal is the case a
full pass cannot do cheaply and a dirty set does (3.1 ms), and the `/usr` worst case
is the number an interval has to stay under — every writable directory touched, 7 776
directories, 110 ms — against a few ms for the one that changed. The add rows are
equal on both routes because both pay the same one-time name-intern rebuild, which is
§10's largest remaining item and not something the dirty set can help with.

Two rules the set is built by, both learned the hard way and both about *not* being
clever: it is **not** ancestor-collapsed, because the parent's reconcile only descends
into a child whose *stamp* moved and the whole reason to mark a directory by hand is a
change the stamp cannot see; and a directory marked and then tombstoned by an earlier
apply in the same batch is skipped rather than listed (ref B4).

`esidx update <db> --dir PATH` is the set on a command line, and the path is resolved
against the index and refused by name otherwise — a reconcile against the wrong
directory would delete every row it did not find, which is why the root argument has
always been checked rather than believed. A directory created since the last pass has
no row to mark, so a create names its *parent*, which is also what a watcher's create
event gives you.

Two things make a refresh visible to a client that asks the same question twice.
The index epoch: a mutation bumps it, and `cache_matches()` compares it, so a
cached result set cannot outlive the index it was computed from. And a new row
reaches the three numeric sorted arrays, which it did not while the only caller was
`esidx update` — a process that exits, so the next one rebuilt the arrays from the
columns and hid the gap. Both were promises in a header (`esidx.h`, §6.4) with no
line of code behind them, reachable only once a process could append and answer
questions in the same breath.

**Mutation.** A removed entry keeps its row and its id and gains `EF_DEAD`; a
`live` bitset is the single authority on what exists, and every query seeds its
candidate set from it. Numeric changes go to the D3 delta with the old value
retracted, `ext:`/`file:`/`folder:` bitmaps and the children vectors are maintained
in place, and the dir-path hash erases with a tombstone rather than a backward
shift. See §11 D8 for why ids are not reused.

---

## 8. Core data structures

```c
/* Numeric index: sorted main array + unsorted delta buffer (D3). */
typedef struct SortedIndex {
    Segment *segments;     /* each segment internally sorted by (value, entry_id) */
    uint32_t num_segments;
} SortedIndex;

typedef struct BitmapIndex {          /* §5.4 */
    RoaringBitmap *by_value;          /* value_id → bitmap */
    uint32_t       num_values;
} BitmapIndex;

typedef struct QueryPlan {            /* §6.2 */
    ASTNode        *ast;
    uint32_t        driver_index_id;
    RoaringBitmap  *candidates;
    CondOrder       filter_order;
    SortChain       sort;
    uint32_t        offset, count;
    int             use_topk;
} QueryPlan;
```

---

## 9. Reference tables

Each row: source → what was taken → how it lands here → why it changed.

### A. Adopted from plocate (scan + index format)

| # | Source | Taken | Landing here | Why changed |
|---|---|---|---|---|
| A1 | `updatedb.cpp:96` | `openat(dirfd, path, O_RDONLY\|O_DIRECTORY\|O_NOATIME)` | as-is | avoids `PATH_MAX` and TOCTOU (`updatedb.cpp:526`); `O_NOATIME` keeps atime unpolluted |
| A2 | `updatedb.cpp:601-603` | directory mtime (sec+nsec) unchanged → reuse stored entries instead of `readdir()` | as-is | the only privilege-free incremental mechanism |
| A3 | `updatedb.cpp:618-646` | `d_type` for the type; `fstatat` only on `DT_UNKNOWN` | as-is | ext4 always populates `d_type`, so `is_dir` is free |
| A4 | `updatedb.cpp:96,103-111` | retry without `O_NOATIME` on `EPERM` | as-is | non-owner files hit `EPERM` |
| A5 | `conf.cpp:72` | `conf_block_size = 32` | value only | too small compresses badly, too large wastes block-level matches |
| A6 | `database-builder.cpp:160` | `ZDICT_trainFromBuffer()` | deferred to P4 | a dictionary trained on short strings beats general-purpose compression by a wide margin |
| A7 | `database-builder.cpp:303` | `docid = num_blocks` — **docid is a block number** | adopted | trades block-level false positives for sequential I/O and amortised decompression |
| A8 | `database-builder.cpp:340-341` | compress per block, `filename_blocks[]` holds the offsets | deferred to P4 | on write it *is* the query-side `offsets[]` |
| A9 | `database-builder.cpp:326-329` | `memcpy` 4 bytes, keep 3, advance 1 | P4 | the trailing `\0` makes the over-read safe; faster than per-byte assembly |
| A10 | `database-builder.cpp:56-70` | delta-1 postings, 128 per group | P4 | delta-1 keeps values non-negative; 128 aligns with SIMD |
| A11 | `database-builder.cpp:96-102` | `interleaved=true` for full blocks, `false` for the tail | P4 | interleaving serves SIMD decode; a short block cannot be interleaved |
| A12 | `database-builder.cpp:463` | Robin Hood trigram hash table | P4 | flattens the longest probe distance |
| A13 | `parse_trigrams.h:50-52` | trigrams are **byte**-based | as-is | explicitly prevents CJK blowup; required for CJK filenames |
| A14 | `database-builder.cpp:267-340` | `decide_block_type()`: histogram + byte-cost model | P4 | adapts instead of using a fixed threshold |

### B. Adopted from FSearch (incremental)

| # | Source | Taken | Landing here | Why changed |
|---|---|---|---|---|
| B1 | `fsearch_folder_monitor_fanotify.c:275` | `fanotify_init(FAN_CLOEXEC\|FAN_NONBLOCK\|FAN_CLASS_NOTIF\|FAN_REPORT_DFID_NAME, O_RDONLY)` | **moved out of esidx** into `sfa` | that call needs `CAP_SYS_ADMIN`, and an index server should not have to be root. The flag set is kept as-is |
| B2 | `fsearch_folder_monitor_fanotify.c:369` | `fanotify_mark(fd, FAN_MARK_ADD\|FAN_MARK_ONLYDIR, MASK, AT_FDCWD, path)` | **replaced** by a negotiated `FAN_MARK_MOUNT` → `FAN_MARK_FILESYSTEM` fallback | a mount mark is `EINVAL` for every name event on WSL2, so a recipe that only tries the inode form cannot run there at all. `sfa_probe.c` tries both and takes whichever covers more event bits |
| B3 | `fsearch_folder_monitor_fanotify.c:24` | mask includes **`FAN_EVENT_ON_CHILD`** | **key adoption, corrected** | `ON_CHILD` is the *immediate* children of the marked object, not the subtree: measured, a change two levels down is silent under an inode mark. Coverage comes from a filesystem mark *without* that bit. The intent (no per-directory watch) is kept |
| B4 | `fsearch_database_index.c:132-160` | `get_skippable_events()`: drop events under a deleted directory | as-is | falls out of `di_lookup()` returning nothing, and is counted (`unknown`) rather than dropped silently |
| B5 | `fsearch_database_index.c:581` | a create event that is a directory triggers a recursive scan | as-is, and it is the same code as before | a new directory is marked via its **parent**, and the parent's reconcile descends — so B5 needed no new code, only the rule that the parent is what gets marked |
| B6 | `fsearch_database_index.c:571-576` | pull ancestors out of the index before mutating, put them back after | idea adopted | aggregates must bubble up the parent chain |
| B7 | `fsearch_database_index.c:199` | `g_async_queue_length()` batch draining | adopted, plus a **coalescing window** | per-event handling jitters under bulk change. Measured: draining per event costs 975 directory listings for 1000 files, because a watcher that keeps up sees ~1 event per batch. `WATCH_COALESCE_MS` = 50 ms or 256 marks brings that to 6 |
| B8 | `fsearch_folder_monitor_*.c` | dual backend dispatched on `event->monitor_kind` | **replaced** by "one backend, in another process" | inotify needs no privilege, so a fanotify client needs no fallback — it needs a proxy. The privilege is in `sfa-server`; `esidx` is only ever a client |

### C. Adopted from FSearch (storage layout)

| # | Source | Taken | Landing here | Why changed |
|---|---|---|---|---|
| C1 | `fsearch_database_entry.c:23-30` | variable-length record, flexible array, flag bitset | **replaced by columnar**; the flag-bitset idea survives for sparse attributes | full coverage needs per-column sort/scan/bitmap; variable-length records do not allow that |
| C2 | `fsearch_database_entry.c:825-842` | `entry_get_size_for_flags()` decides which attributes to store | → sparse K/V table | media metadata is sparse; K/V beats fixed-width columns |
| C3 | `fsearch_database_entry.c:812-822` | precomputed attribute offset table | dropped | unnecessary with fixed-width columns |
| C4 | `fsearch_database_chunked_array.c:16-22,39,100-119,142-146` | `ChunkedArray`, `TARGET_CHUNK_SIZE 2048`, split at 2× | adopted for `children[]` | insertion costs O(2048) instead of O(n) |

### D. Adopted from FSearch (query semantics)

| # | Source | Taken | Landing here | Why changed |
|---|---|---|---|---|
| D1 | `fsearch_query_matchers.c:7-341` | matcher table of uniform-signature function pointers | adopted, extended to 40+ functions | adding a filter condition is then one function |
| D2 | `fsearch_query_matchers.c:45-61` | `cmp_num()` covering 6 numeric comparisons | adopted, extended to 12 | every numeric attribute reuses it |
| D3 | `fsearch_database_sort.c:38-61` | multi-level sort chain (size → name → path) | adopted, extended to 22 keys | guarantees a total order, so results are reproducible |
| D4 | `fsearch_database_search_view.c:216` | sorting applies to the result set | adopted + TopK | a small `offset + count` should not force a full sort |

### E. Consulted but rejected

| # | Source | What it does | Why not |
|---|---|---|---|
| E1 | `fsearch_database_scan.c:141,158,185` | `opendir`/`readdir` + `fstatat` on **every** entry | ignores `d_type`, an order of magnitude slower; `scan.c:165` still carries `TODO: we can test for hidden here to avoid stat call`. Use A3. |
| E2 | `fsearch_database.c:1331` | IO pool with `max_threads=1` | parallelism in name only. We shard top-level directories and work-steal (D6). |
| E3 | `plocate.cpp:371-376` | deliberately no io_uring on a full scan | the conclusion holds, but only because their index is on disk. Ours is resident. |
| E4 | `plocate.cpp:444-455` | 32 blocks per `pread`, bounded queue, parallel decompress | same — needed only in the persisted mode. |
| E5 | FSearch's bespoke persistence | own chunked array + mmap, no SQLite | tightly coupled, no query capability. We use columnar + our own snapshot (D5). |
| E6 | plocate indexes paths only | no size, no mtime | full coverage needs every attribute column. |
| E7 | FSearch has no children index | listing a directory is a full scan | unacceptable for interactive browsing — see §5.1 |
| E8 | FSearch filters by full scan | per-entry `size:` comparison | selection-driven queries need real indexes |

### F. Neither project has these

| # | Content | See |
|---|---|---|
| F1 | `dir_id → children` index and `path → dir_id` hash | §5.1 |
| F2 | Numeric sorted array / delta buffer | §5.3 |
| F3 | Enum bitmaps and set algebra | §5.4 |
| F4 | Query optimiser: selectivity estimation, driver selection, TopK | §6.2 |
| F5 | `SITE EVERYTHING` protocol server, 32 subcommands | §1.1 |
| F6 | Query-language lexer/parser and constant expansion | §1.4, §6.1 |
| F7 | Sparse metadata table and media attribute indexes | §4.3 |
| F8 | Two-stage `FILTER_*` matching | §1.1, §6.3 |

### G. Adopted from etp_server (protocol baseline)

| # | Source | Taken | Landing here |
|---|---|---|---|
| G1 | `etp_server.c:3951-4246` | full semantics of the 32 `SITE EVERYTHING` subcommands | implemented one by one, including two-stage filtering |
| G2 | `etp_server.c:472-494` | 22 sort names → property mapping | same-named mapping table; `inverse_size` reuses the `size` key with the direction flipped |
| G3 | `etp_server.c:4250-4277` | full parameter comparison plus result reuse | replicate; invalidate on any index mutation |
| G4 | `etp_server.c:50-68` | FTP command set, including `MLSD`/`MLST`/`EPSV`/`EPRT`/`OPTS`/`FEAT` | implemented one by one |
| G5 | [`everything-syntax.md`](everything-syntax.md) | the full modifier and function set | the parsing target |

---

## 10. Implementation status

| § | Component | File | State |
|---|---|---|---|
| 4.1 | Columnar table, name pool, ext pool | `store.c` | done, 9 columns (`by_ctime` added for `dc:`); aggregates and `frn` still outstanding |
| 4.1 | name interning | `store.c` | **done** — the pool holds one copy per *distinct* name rather than one per entry: 96.0 → 37.3 MiB on /work for a 5.7 MiB table, and −61.6 MB of snapshot with it. One hash and one probe per entry in the walk, which measures as a wash on CPU (§10's ledger) |
| 4.2 | `path_of()` parent-chain rebuild | `store.c` | done; path materialisation pending §4.2 |
| 4.2 | extension-name interning | `store.c` | **done** — an open-addressed table over the name, so `ext_intern()` is O(1) instead of a scan of every name interned so far: 519 string compares per call down to 1.16 on /work, which is 5.8 s of user time on a build that is otherwise I/O-bound. Derived, so no snapshot mentions it (§4.2 for the two off-by-ones it took to get right) |
| 5.1 | `dir_id -> children`, `path -> eid` hash | `store.c` | **done** — the children are one flat array grouped by directory ordinal (a CSR): exact extents, 100 % occupancy, one allocation for 630 472 directories, where the per-directory vectors were at 53 % in 630 472 allocations. It is rebuilt from the columns once per build and once per reconcile that added something, because a shared array has no room in the middle |
| 5.2 | trigram index, sorted/reversed name arrays | `trigram.c` | **name half done** — `trigram.c`, byte trigrams over the display name, 33 727 keys / 4.83 M postings at 3.7 × 10⁵ entries; §5.2.1 for what was deliberately left out and why. **Postings are delta-varint encoded** (318.6 → 101.8 MiB on /work, §10). **Path half and the sorted/reversed name arrays not started** |
| 5.3 | sorted array + delta buffer | `store.c` | **done** — `sidx_update`/`sidx_erase` write the delta, D3's 1%/60 s merge is implemented, and the range read honours the retractions. Two arrays (`int64`, `eid`) rather than one of structs: 12 bytes a row against 16, of which 4 were padding — 62.7 MiB on /work. The build sorts the ids against the value column and gathers; the merge is a linear merge of two sorted runs. Nothing outside `store.c` reads the layout. **Empty is legal and means "not built"**, and `range_on()` scans the column instead — see §5.3.1 |
| 5.4 | dense bitset | `index.c` | done; CRoaring at P4 (D7). Set algebra lives here, not in the executor |
| 5.4 | ext bitmaps, file:/folder: bitmaps | `store.c` | done — built in `finalize`, so the snapshot format is unchanged |
| 5.5 | aggregate columns + bubbling | `store.c` | **first aggregate done** — `nchild`, the child count, stored rather than derived and maintained in the same two functions that maintain the children vectors. It exists because those vectors are indexed by directory ordinal and `child-count:` reads the value once per candidate row: measured over 372 084 rows, reading the count out of the vector cost 2.4 -> 6.4 ms of eval. Not persisted (the load path rebuilds the vectors and the count together, D4). The remaining aggregates, and bubbling them to ancestors, are not started |
| 5.6-5.8 | content, dupe, sparse metadata | — | not started (P6) |
| 6.1 | lexer → parser → AST | `lexer.c`, `parser.c` | **done**. A term's value is bounded by `SYNTAX_VALUE_MAX` (8192), which is the longest control line the ETP layer will hand over — deliberately *not* smaller, because a value that arrives cut is a query that answers something else, and three buffers on the way in used to cut one (this one at 2047; the other two in the protocol layer) |
| 6.2 | optimiser: selectivity estimate, driver selection | `query.c` | **done** for step 1-2 (exact cardinality per leaf, no histogram yet). Step 3 ordering and step 4 TopK not started — see below. The `size:`/`dm:`/`dc:` estimates are now upper bounds once the D3 delta is non-empty, because they count the main array without the retractions. A text leaf can be costed from its shortest trigram posting list (§5.2) but is **still not a driver**: the filter narrows eval, not the candidate count. An empty array reports itself unusable here and `range_on()` scans the column instead (§5.3.1) |
| 6.3 | execution: candidates → bitmaps → matchers → sort → slice | `query.c` | **done**, including the second-stage FILTER_* pass. The text matcher implements Everything's rule for *what a term reads* — the filename, or the path once the value carries a separator or says `path:` — verified shape by shape against voidtools' server; §12.10 has the table and the two shapes still open. An `ext:` term's id list is sized by the term, not by a fixed 256 (§6.3, and the reason is in the code): a longer list used to be cut with no complaint |
| 6.4 | result cache | `etp.c` | **done** — the full sorted set is kept and re-sliced, and invalidated by the index epoch |
| 7 | full scan | `scan.c` | done; concurrency (D6) not started |
| 7 | incremental | `scan.c`, `watch.c` | **done** — the two reconcile passes and the mutation core; `esidx update <db> [--deep] [--sweep]`, `esidx serve --refresh=SECS` (names pass in the serving process), `esidx serve --sweep=SECS`, and `esidx serve --watch[=SOCK]`, which subscribes to the `sfa` submodule's privileged fanotify proxy and marks the directory each event names (§7 "Events"). One limit is named rather than papered over: a stamp-pruned pass reaches a change at depth ≥ 2 only if some directory above it also moved (§12 risk 8), which is what the **sweep** (§7 "Sweep") is for — 77 ms on `/usr`, 1.6 s on `/work`, measured on `r7000`, and therefore on demand at startup / after a proxy loss signal / on `--sweep`. The inotify fallback (B8) is **not** built and is not needed: a proxy means esidx needs no privilege, so there is no second backend to fall back to |
| 1-3 | FTP + `SITE EVERYTHING` | `etp.c` | **done** — all 32 subcommands, 22 sort names, the data channel for other FTP clients |

**Two things in §6.2 deliberately not built**, with the reasoning recorded
rather than the code written on speculation:

- **Step 3, ordering the non-indexable conditions.** There is exactly one
  non-indexable leaf class today (the text scan), so ordering among them cannot
  pay. It becomes worth building when §5.2's trigram index lands and there are
  several text leaves to order.
- **Step 4, the bounded TopK heap.** `RESULT_COUNT` has to be the size of the
  *whole* matched set (etp_server.c:5190), so every id is collected and sorted
  regardless; and keeping the sorted array is what lets the protocol layer
  re-slice a page without re-running the query. A heap would replace the
  `O(n log n)` compare stage with `O(n log k)` — real, but on the key this client
  actually sorts by, the row set is already a directory listing. Measured below:
  sorting only becomes the dominant cost above ~10⁴ rows.

**Measured baseline** (WSL2, Ubuntu 22.04, ext4, `-O2`, single thread; reproduce
with `./round.sh /usr`):

| Tree | Entries | Scan | Finalize | Load | Snapshot |
|---|---|---|---|---|---|
| `/etc` | 1 622 | 5.4 ms (298 k/s) | 0.4 ms | 0.3 ms | 95 KB |
| `/usr` | 116 888 | 4.23 s (27.6 k/s) | 37-56 ms | 55 ms | 6.9 MB |

**Also measured on real hardware** (the `r7000` host: Ubuntu 22.04, x86_64, ext4
on NVMe, 16 cores, gcc 11.4; the same `./round.sh /usr`). This is the check that
WSL2 was not standing in for the machine: `/usr` there is a full desktop install,
**372 084 entries** against WSL2's 116 888, and it scans at **268 k entries/s**
against 27.6 k/s — **~10x** — so WSL2 timings understate throughput and are not
the baseline to tune against. The tree is bigger, so the absolute query times are
too: `ext:conf` 1.1 ms over 1 206 hits, `image:` 52.5 ms (**51.2 ms sort** over
55 229 rows), `path:/usr *.conf size:>1k` 57.7 ms (**56.1 ms eval** over 233 021
candidates), a `parent:` browse 0.40 ms. The two costs §5.2 and §6.2 name are the
same two the real machine shows, in the same proportion — so the P4 conclusion
does not change, only the confidence that it is about the machine and not WSL2.

**Incremental refresh**, same machine, same build, `-O2`, single thread. "Worst
case" is every writable directory under `/usr` touched, so every stamp moved:

| Pass | idle | worst case | what it costs |
|---|---|---|---|
| names | **0.1 ms** — 15 dirs (14 skipped, 1 descended), 14 entries seen | 110 ms — 7 776 dirs, 7 775 refreshed, 0 added/removed | one stat per directory whose parent changed |
| deep | **4.23 s** — 8 590 dirs descended, 116 887 entries seen | same | one stat per entry |

Four things these numbers settle:

- **The cheap pass is the one to run often.** 0.1 ms on an unchanged 117 k-entry
  tree means a 5 s interval costs 0.002 % of a core; the deep pass at 4.2 s means
  a 10 min interval costs 0.7 %. Neither is a reason not to schedule them.
- **A pass that finds nothing writes nothing to any index.** 0 added, 0 removed,
  and — the part that matters for query latency — 0 delta rows, because
  `esidx_touch` only reaches a sorted array when the column actually moved. So the
  costs after a refresh are the costs before it: measured, `path:/usr *.conf
  size:>1k` still spends 22.9 ms of eval over 78 296 candidates, `ext:conf`
  0.05 ms, a browse 0.01 ms.
- **The epoch does not move for a pass that found nothing** (`epoch=0` after four
  idle passes on `/usr`). A refresh is invisible to the protocol layer's result
  cache unless a query could actually return something different.
- **Tombstones do not accumulate from a busy directory.** Rewriting one file in
  `/usr/share` five times in a row costs 0.4-0.6 ms each and leaves one tombstone
  from the final delete: the row is matched by name and refreshed in place. Ids
  are only spent on names that genuinely appear or disappear, which is what makes
  the D8 compaction threshold a safety net rather than routine work.

**The same passes, in the serving process** (`./refresh.sh <root> --add 2000`,
`r7000` — real ext4 NVMe, `hpet` clocksource, so these are syscall times and not
vDSO ones). A refresh interval has to be shorter than the pass it is waiting for, so
these are the numbers that decide it, and they do not scale the way the table above
suggests: the walk is pulled from the root and stops at the first unchanged stamp.

| Tree | Entries | Startup repair | Idle pass | Passes that add ~2000 rows | Snapshot write |
|---|---|---|---|---|---|
| `/usr` | 372 084 | 0.2 ms | **0.2-0.5 ms** — 16 dirs, all skipped but the root | not run (no writable tree on this host) | *nothing written*: the epoch never moved |
| `/work` | 5 476 485 | 0.5 ms | **0.4-1.1 ms** — 48 dirs skipped, the root's 74 entries listed | **0.40 s** in three passes (see below) | 538-549 ms for a 313 MB file |

What the adding passes cost, same tree, same command, three builds of this tree — which is
the whole argument for the two overlays, in one table:

| | before the overlays | children overlay (P3a) | both overlays (P3b) |
|---|---|---|---|
| adding ~2000 rows, `/work` | **2.38 s** = 414 ms walk + 1469 ms name rank + 497 ms children array | **1.85 s** = 440 ms walk + 1413 ms name rank | **0.40 s** = 389 ms one-time name-intern rebuild + 12 ms of actual work |
| removing 2000 rows again | 5-9 ms | 5-9 ms | 5-9 ms |

The last row is in the table because it is the shape of the thing: **removal was never
the expensive direction, and it stays that way through every one of these builds** — a
removal needs neither rebuild, since no name changed and the CSR swap-removes in place.
What the overlays bought is the other direction.

- **Both O(n) rebuilds are now amortised rather than removed**, and the three builds say
  what each was worth: the children array 497 ms per pass, the name rank 1413 ms per pass.
  Neither is proportional to the change — two runs of the same command added 1220 and 2001
  rows and cost 2.38 s and 1.85 s — which is the flatness stated as a measurement rather
  than as an argument. After them, the 2001 rows cost **12 ms** and the pass that first
  adds a row after a load costs 389 ms for a reason that has nothing to do with either
  rebuild: the name intern table is rebuilt lazily on the first add, over 1 499 995 distinct
  names, so a serve-only process never pays for it and a refreshing one pays it once per
  process. **That is now the largest single item on the refresh path**, and the next thing
  worth measuring rather than the next thing worth building.
- **An idle refresh is free at any interval, on any tree measured.** 0.9 ms over 5.5 M
  entries, because 48 of the 49 directories were skipped on their stamp and the one
  that was walked is the root's own listing. It is proportional to the *root's
  fanout*, not to the tree — which is the cheapest possible answer and the reason the
  repair pass can run unconditionally at startup.
- **A name the rank has not seen is placed between its two neighbours by arithmetic, not
  by renumbering.** One binary search over the sorted rank table says how many distinct
  names sort before it; the key is then `gap << 32` minus the width of the pending names
  sharing that gap, so a name sort stays a pure integer comparison (query.c, `rank_pending`).
  The alternative — have the comparator fall back to a string when one side has no rank —
  was not written, because a comparator whose answer depends on which row carries a rank
  is not a valid ordering and `qsort` is entitled to answer anything to one (design §10
  records the same class of bug in the tie-break).
- **The children half is an append-only overlay** that `esidx_drain()` folds into the
  array once it passes a *twelfth of the entry count*: 497 ms per 456 000 additions
  instead of per pass, bounding the overlay at 2.7 MiB here. A fixed per-directory slack
  would have been simpler and does not work — a directory created by a bulk copy is
  empty, so its slack is empty too, and the measured case (2000 files into one new
  directory) blew through any slack and paid the rebuild anyway.
- **Removing is cheap and adding is expensive**, which is the asymmetry to design
  around: the pass that tombstoned 2001 ids cost 5-9 ms, because a removal needs
  neither the rank nor the children array — the rank does not change when no name
  changes, and the CSR swap-removes in place.
- **The delta a new row now writes costs a range query nothing measurable at this
  size.** `size:>1mb` over `/work` evaluated in 1.72 ms with an empty delta and
  1.25-1.78 ms with 2000 added rows in each of the three arrays — the same spread as the
  two runs with an empty delta, which is the point: it is noise (the sort on those runs
  was 12.7-14.5 ms). `sidx_range_to_bitset()` walks the delta in append order, so the
  cost is linear in it, and D3's 1 % rule bounds the delta at 54 762 rows per array on
  this tree. That bound is *not* measured, and it is the number to watch if the interval
  gets short.
- **A snapshot write is 538-549 ms here**, which is why `--save` is a separate knob
  from the refresh interval and not "every refresh": on this tree that is half a
  second in which the loop answers nobody. It is also skipped entirely when the epoch
  has not moved, so an idle server writes nothing at all.

`/usr` scan stats: 8 590 directories, 108 298 files, max depth 13, 567 distinct
extensions. Finalize now also builds the ctime sorted array, the ext bitmaps and
the type bitmaps, which is why it grew from ~24 ms to ~37 ms; the load path pays
the same ~20 ms extra.

Query cost on `/usr`, as the protocol layer reports it — the candidate count is
the whole point, because it is what the driver index bought:

| Query | Candidates | plan | eval | sort | total |
|---|---|---|---|---|---|
| `parent:"/usr" folder:` | **14** / 116 888 | 0.18 ms | 0.01 ms | 0.01 ms | **0.33 ms** |
| `ext:conf` | 608 / 116 888 | 0.17 ms | 0.01 ms | 0.13 ms | 0.37 ms |
| `image:` (category) | 13 569 / 116 888 | 0.27 ms | 0.10 ms | 4.98 ms | 5.4 ms |
| `path:/usr *.conf size:>1k` | 78 296 / 116 888 | 0.39 ms | **23.8 ms** | 0.03 ms | 24.3 ms |

Two things fall out of that table:

- The old implementation spent 42 of 46 ms in `qsort` over 116 888 rows on an
  *unfiltered* query. A browse request now costs 0.33 ms because `parent:` seeds
  the candidate bitmap and the matcher pass walks 14 rows, not the index. That was
  the reason §6.2 step 2 was on the roadmap; it is now the reason it is not.
- The remaining 23.8 ms is a `*.conf` wildcard scan over 78 296 candidates. That
  is §5.2's trigram index, and the measurement is what justifies its activation
  threshold (10⁶ entries) being optimistic: at 10⁵ the in-memory scan is already
  the dominant cost of a real query. **That gate is now gone** — the name half is
  built unconditionally, and the next block is what it bought. (The alternative
  this paragraph used to recommend, name-sorted and reversed-name arrays, was the
  weaker of the two fixes and is not what landed: they serve `startwith:` and
  `endwith:` and nothing else, while the trigram filter serves every text term
  there is.)

#### Name trigram index — before and after, on real hardware

`r7000` (Ubuntu 22.04, x86_64, ext4 on NVMe, 16 cores), `/usr` = **372 084
entries**, `-O2`, single thread. Both columns are the same `./round.sh /usr`
session against the same tree, one build differing only in this layer — the
harness is in the repo on purpose (AGENTS.md §1.3), so these are re-measurable
rather than remembered. Every query returns the identical row count, which is the
invariant the layer promises: it is a filter, and only the time moves.

| Query | Candidates | eval before | eval after | total before | total after |
|---|---|---|---|---|---|
| `conf` (bare word, the client's default search) | 372 084 → **8 625** by the prefilter | 29.9 ms | **6.5 ms** | 32.3 ms | **12.0 ms** |
| `path:/usr *.conf size:>1k` | 233 021 → **908** for the `*.conf` leaf | 64.7 ms | **33.9 ms** | 66.2 ms | **35.3 ms** |
| `ext:conf` | 1 206 (ext bitmap, unchanged) | 0.08 ms | 0.06 ms | 1.0–1.4 ms | 1.0–1.2 ms |
| `image:` | 55 229 (ext bitmaps, unchanged) | 0.94 ms | 1.08 ms | 54.3 ms | 55.7 ms |
| `parent:"/usr" folder:` | 16 (dir tree, unchanged) | 0.04 ms | 0.04 ms | 0.22 ms | 0.22 ms |

Four things this settles, and one it does not:

- **The bare word was the client's default and nothing measured it.** Every shape
  already in `round.sh` either reads a path or never reaches a text matcher, which
  is why the layer had no before-number to quote. `round.sh` now carries it.
- **`path:` is untouched, and the query it appears in still halved** — from the
  *other* leaf in the same query. `*.conf` has no separator, so it reads the name
  and the name index applies; `path:/usr` reads a rebuilt path and is refused. Half
  of that query's cost was never the part this layer was built for.
- **Candidates do not move for a text term.** `conf` still seeds all 372 084 rows,
  because §6.2 step 3 would have to make a text leaf a driver and this layer
  deliberately does not. What changed is that the matcher pass walks 8 625 rows
  instead of 372 084. The sort grew correspondingly (2.3 → 5.3 ms) because
  8 484 rows are now collected where before they were collected too — that one is
  sampling noise across an 11-drive session, not a cost of the filter.
- **A word no filename contains costs nothing.** `zzzzqqqqxxxx` narrows to 0
  candidates and returns without touching a row: the trigram has no posting list.
- **What is still open:** `image:` is now the largest single cost on this tree
  (54 ms, of which ~53 ms is the sort over 55 229 rows), which is §6.2 step 4's
  TopK and not this layer. And a `path:` term is still a full scan — §5.2's path
  half.

**What it costs.** `finalize` on this tree: **+90.4 ms** (90.4 ms to build 33 727
keys and 4 834 687 postings; the other six derived indexes together are ~141 ms),
and `esidx_load` pays the same because it re-runs `finalize` — 276 ms becomes
~367 ms. Memory: ~20 MB of postings on a 372 k-entry index, against a 6.9 MB
snapshot for 117 k entries on WSL2. Nothing is persisted (D4), so this is paid at
startup and at every compaction and never on the query path.

**What it costs, since the postings are encoded.** The 20 MB above was the plain
array; delta-varint makes it **5.9 MiB** on the same tree (18.4 before, 3.15x) and
101.8 MiB on /work against 318.6, for no measurable query cost — the numbers are in the
memory findings below. The build price is also unchanged, because the two-pass build that
sizes every list exactly had to learn to price *bytes* rather than postings, and pricing a
posting is the same walk either way.

#### A path sort was allocating 64 KiB per row, and failing silently

Found while measuring the above, and the more useful of the two findings.

`sort_string()` gave a path sort a fresh `malloc(65536)` per row and held every one
of them until the sort ended. It looked like it worked, because only the first page
of each buffer is ever touched: an unfiltered `path_ascending` over `/usr` cost
**1.7 GB of RSS** but still returned the right order. Where malloc *did* refuse —
23 GB of address space is beyond what a default overcommit heuristic grants, and a
`ulimit -v` reaches that instantly — it returned `""`, every comparison tied, and
`cmp_rec`'s tie-break quietly turned the **path sort into a name sort**, with no
error anywhere. Reachable from the wire: `path` is one of the 22 sort names, so it is
one click on a column header in the client.

| `./round.sh /usr`, unfiltered `SEARCH`, `SORT path_ascending`, 372 084 rows | sort | total | peak RSS (CLI) |
|---|---|---|---|
| before | 1 310.9 ms | 1 564.7 ms | 1 705 368 kB |
| after | **169.0 ms** | **196.3 ms** | **207 644 kB** |

The rows on the wire and their order are the same on both sides; only the cost
changed. The residual 27 MB is 372 084 copies of a ~70-byte path, and the residual
169 ms of sort is ~6 M `strcasecmp` calls — which is the same cost `image:` is
made of, so §10's two remaining line items are one line item.

The cache that wrapped this (`scache_init`/`ck_*`) could never hit: `sort_string()`
has exactly one call site, once per row, inside the extraction loop, and the
comparator only reads `srec_t.s`. It was a hash lookup per row and nothing else, so
it is deleted rather than fixed.

Why no test caught it: every path-sort assertion sorted a *filtered* result set —
the largest was three rows. On WSL2's `/etc` (1 622 entries) the allocations total
104 MB and succeed. It only bites above ~26 000 rows in the result set, which is
exactly the regime AGENTS.md §2.4 says WSL2 is not. The assertion added for it runs
the sort under `ulimit -v` so the old allocation path cannot be satisfied at all,
and compares against `order_ref`.

#### The sort, measured per key — and the two schemes that were tried

`sortcmp.sh` (in the repo, per AGENTS.md §1.3) is the harness: same tree, one
unfiltered sort per cell, interleaved, best of three. `r7000`, `/usr` = 372 084 rows,
`-O2`. Three builds:

- **base** — `d2cbd2b`, `strcasecmp` in the comparator
- **A** — fold each key once per row into an arena, then `memcmp`
- **B** — A, plus a dense rank over the distinct display names built in `finalize`,
  so a *name* sort is an integer compare

| sort key | base | A | B | gain/A | gain/B | comparisons |
|---|---|---|---|---|---|---|
| `name:ascending` | 166.1 | 166.9 | **68.6** | 1.00× | **2.42×** | 6 391 733 |
| `name:descending` | 165.7 | 166.0 | **67.9** | 1.00× | **2.44×** | 6 381 346 |
| `extension:ascending` | 239.5 | 182.1 | 181.9 | 1.32× | 1.32× | 6 341 436 |
| `date_modified:descending` | 168.5 | 112.6 | 112.7 | 1.50× | 1.49× | 6 272 914 |
| `size:descending` | 148.2 | 110.8 | 110.4 | 1.34× | 1.34× | 6 353 102 |
| `attributes:ascending` | 167.0 | 146.8 | 146.3 | 1.14× | 1.14× | 6 375 785 |
| `date_created:descending` | 172.2 | 114.0 | 114.3 | 1.51× | 1.51× | 6 243 046 |
| `path:ascending` | **143.1** | 157.7 | 157.6 | 0.91× | 0.91× | 5 893 420 |
| `path:descending` | **143.9** | 157.6 | 157.9 | 0.91× | 0.91× | 5 867 893 |

Four things this settles, and one it does not:

- **The two schemes are not competitors; only one of them does anything, and it is
  not the one that looked like the point.** Folding every name per query and
  `memcmp`-ing it (A) is **1.00×** on a name sort — no change at all. A name is ~20
  bytes, so `memcmp` on it is no cheaper than `strcasecmp` on it, and the fold it
  needs per query costs exactly what the vectorised compare saves. The rank (B) is
  what makes it 2.42×, because it removes the comparison instead of making it
  cheaper.
- **What A *is* good for is the tie-break.** Every non-name key is an integer compare
  that lands on `cmp_rec`'s display-name tie-break, and there the fold is amortised
  over ~17 comparisons per row instead of paid once. That is the 1.14–1.51× on the
  four keys above, and B keeps it.
- **`path` is 9 % slower and neither scheme fixes it.** It keeps the baseline's
  mechanism — an exact-sized copy per row, no arena — because both alternatives
  measured worse (fold into the arena 190 ms, raw into the arena 218 ms). What is
  left is the per-row `strrchr` for the tie-break pointer plus the copy itself.
  Recorded rather than explained away: a path's primary key is unique, so it almost
  never reaches that tie-break, which makes the `strrchr` pure overhead.
- **The comparison count is the same in all three builds** (5.9–6.4 M), so every
  number above is a per-comparison improvement and none of it is TopK. §6.2 step 4
  would attack the *count*; the count was never the problem. What attacks it is the
  *cost* of a comparison, and the next subsection is what that cost was made of.

**What it costs.** `finalize: name rank: 75 ms` (179 786 distinct names of 372 084),
paid again on every load: 276.6 ms → 352.7 ms. Memory ~1.4 MB of rank plus ~8 MB of
folded names. Nothing is persisted (D4). A *name* sort is what the ETP client asks
for by default — `SORT name_ascending` is in the trace in AGENTS.md §1.4 — so the
trade is 75 ms of startup for 97 ms on every name-sorted query, which breaks even on
a single page of an unfiltered search.

#### The tie-break was the rest of it: a rank per comparison, not per column

The table above reads as though the only key a rank could help is `name`. It helps
every key, because a rank is not a per-column structure — it is a per-*comparison*
one, and `cmp_rec` reaches the display-name tie-break on nearly every comparison of
a non-name key. Over an unfiltered `/usr`, a `size` or `date_modified` sort lands
there constantly: a whole directory shares one size and a whole minute shares one
mtime, so after the integer compare ties, the ~7 ns that follows is `cmp_folded` over
two ~20-byte folded names, and it is amortised over ~17 comparisons per row.

So the tie-break reads `name_rank` too, which is the same order by construction: the
rank is dense over sorted position, and two names `strcasecmp` calls equal share a
rank and therefore still fall through to the id. Nothing was added to build — the
rank already existed — and no row needs a folded name any more, so the numeric keys
stop copying 372 084 names into an arena per query as well.

| `./sortcmp.sh -n 3`, unfiltered `/usr` = 372 084 rows, `r7000`, `-O2` | base `abd1584` | tie-break by rank | gain | ns/comparison base → after |
|---|---|---|---|---|
| `name:ascending` | 73.1 ms | **63.3 ms** | 1.15× | 11.4 → **9.9** |
| `name:descending` | 71.8 ms | **62.3 ms** | 1.15× | 11.3 → **9.8** |
| `attributes:ascending` | 182.5 ms | **70.7 ms** | **2.58×** | 28.6 → **11.1** |
| `date_created:descending` | 137.8 ms | **64.0 ms** | 2.15× | 22.1 → **10.3** |
| `date_modified:descending` | 136.6 ms | **64.5 ms** | 2.12× | 21.8 → **10.3** |
| `extension:ascending` | 210.0 ms | **120.6 ms** | 1.74× | 33.1 → **19.0** |
| `size:descending` | 123.8 ms | **76.7 ms** | 1.61× | 19.5 → **12.1** |
| `path:ascending` | 157.2 ms | **154.8 ms** | 1.02× | 26.7 → **26.2** |
| `path:descending` | 158.4 ms | **155.4 ms** | 1.02× | 27.0 → **26.5** |

Both columns are from one interleaved run, which is the only way they are comparable.
The absolute numbers are ~20 % below the table above for the same keys and the same
comparison counts to the digit — a quieter or differently-clocked `r7000` than the run
that produced it — so the *gains* are what this subsection claims and the absolute
milliseconds are not a new baseline.

Three things it settles:

- **The four integer keys are now integer all the way down**, at 10.3–12.1 ns against
  the 9.9 ns floor a name sort already had. There is nothing left to win by making a
  numeric compare cheaper: what remains is `qsort`'s own memory traffic, and §6.2
  step 4 (TopK), which attacks the *count* — still untouched at 5.9–6.4 M.
- **`extension` is the one key a name rank does not finish.** Its *primary* key is a
  string, so 19.0 ns is a string compare over ~6 bytes plus the per-row extraction of
  the extension; the tie-break below it is already an integer. An extension rank would
  finish it, from the slot table `ext_index` already keeps — ~40 lines, and the last
  per-comparison win this shape of fix can give.
- **`path` did not move, and the `strrchr` is gone anyway.** The tie-break no longer
  reads a string, so the per-row `strrchr` that produced `dn` is not computed at all —
  1.02× is that, plus noise. The remaining 26 ns is `strcasecmp` over ~70-byte paths,
  which is the primary key and inherent to it.

**What it cost:** nothing to build. `finalize` is unchanged (name rank 83.1 ms base,
74.1 ms after — run-to-run spread, not an effect), `load` 359.9 → 338.8 ms for the same
reason, and no new memory: `srec_t` grew a `uint32_t` into the padding the `eid_t`
left, so a row is still 32 bytes.

#### A name sort's tie-break was comparing raw bytes

Found by making the tie-break an integer compare and asking what it had been doing.

On the ranked path a name sort never copied the display name into the arena — that is
the whole point of the rank — so `dn` was left as a pointer to the *unfolded* name,
and the tie-break ran `cmp_folded()` over it: `memcmp` on the bytes as stored. For two
names that `strcasecmp` calls equal that is a different rule from the one every other
key applies. On a fixture of `Foo`/`foo`/`FOO`/`fOo`:

| `sort:name:ascending`, four case-variants of one name | order |
|---|---|
| before | `FOO, Foo, fOo, foo` — raw byte order |
| after | id order — they tie, as `strcasecmp` says they do |

The no-rank fallback path folds `dn` like every other key, so the ordinary path and the
fallback disagreed about the same four rows, and the documented order — `strcasecmp`,
then the id (D3) — was neither. Nothing caught it because every order fixture had
distinct names, so the tie-break was unreachable: a bug in one level of a multi-level
sort is invisible until a fixture reaches that level. The assertions added for it also
check that the fixture *can* discriminate — `find(1)` must not already list the four in
raw byte order, or the assertion is vacuous and says so.

#### Phase timings, and what the walk spends them on

The phase numbers above (scan / finalize / load / save) were always in the log; what was
missing was any split *inside* the walk, which is the number that decides two open
questions: whether "batch stat by inode" (the note at the top of `scan.c`, and §4.4 of
`AGENTS.md`) is worth building, and whether D6's scan concurrency has anything to win.
`esidx -v 5 build <tree> -o <db>` now prints it, and what it costs to ask.

**Phase timings**, `r7000`, `-O2`, single thread, one warm run each. The `/work` row is
the tree the `find(1)` baseline in `AGENTS.md` was taken on:

| | `/usr` = 372 084 | `/work` = 5 476 485 |
|---|---|---|
| scan (walk) | 1 206 ms | **55 653 ms** |
| finalize | 286 ms | **5 194 ms** |
| save | 27 ms | 600 ms |
| build, total | 1 492 ms | **60 847 ms** |
| load | 327 ms | 5 664 ms |
| snapshot | 25.1 MiB | 414 MiB |
| peak rss | 82.2 MiB | **1 223 MiB** |
| entries/s, warm | 308 000 | 98 400 |

The `/usr` figure is the same measurement as the 268 k/s quoted above, run with a warm
metadata cache; the spread between the two is cache state, not code.

Against the same tree measured the other way — `find /work -xdev -printf '%y %b'`, which
is one `lstat` and one `readdir` per entry and nothing else: **56.75 s**. So the walk
costs 2 % less than `find`, and the *whole* build — every derived index and a 414 MiB
snapshot included — costs 7 % more than `find`'s single pass. The entry count agrees
exactly (5 476 485 both ways; `esidx` also reports `open_fail=3` for three unreadable
directories, which the `LOGW` names). The file/directory split does not agree, and
should not: `find`'s `%y` reports a symlink as `l`, so the `awk` that consumed it counted
26 240 entries in neither bucket, while esidx counts every non-directory as a file.

`finalize` at this size, from the `-v 5` run — the two indexes added after the original
design are now the majority of it:

| step | ms | share |
|---|---|---|
| name trigrams | 1 883.9 | 35.7 % |
| name rank | 1 331.6 | 25.2 % |
| sorted index size | 745.4 | 14.1 % |
| sorted index mtime | 498.8 | 9.4 % |
| sorted index ctime | 503.5 | 9.5 % |
| dir path hash | 232.5 | 4.4 % |
| ext sets | 51.5 | 1.0 % |
| type bitmaps | 22.0 | 0.4 % |
| live set | 9.5 | 0.2 % |
| **total** | **5 279.1** | |

`load` is 91 % `finalize` (5 167 of 5 664 ms): D4 persists the index to avoid a rescan,
and at this size the rescan it avoids is 55.7 s while the work it does *not* avoid is
5.2 s of derived indexes rebuilt on every start. Reading the snapshot itself is 375 ms.
That is the number to weigh before deciding whether the derived indexes belong in the
file.

**The walk, split four ways** — `getdents64` / `fstatat` / `openat` / `esidx_add`, with
the remainder as `other`:

| | walk | getdents64 | fstatat | openat | esidx_add | other |
|---|---|---|---|---|---|---|
| `r7000` `/work` | 83 009 ms | 41.5 % | 20.0 % | 2.5 % | 16.0 % | 20.1 % |
| `r7000` `/usr` | 3 433 ms | 8.8 % | 32.5 % | 4.9 % | 20.5 % | 33.2 % |
| WSL2 `/usr` | 4 157 ms | 12.7 % | 65.9 % | 13.9 % | 1.8 % | 5.7 % |

Four things fall out of it, and none of them was visible before:

- **The walk is filesystem-call bound, and which call depends on the medium.** On WSL2
  `fstatat` is 66 % of the walk and costs **23.4 µs a call**; on ext4 the same call is
  **1.81 µs** and `getdents64` takes over at 59 %. §2.4's "WSL2 is ~10× optimistic" is
  not one factor — on the two hosts it is a *different syscall* being the expensive one.
- **"Batch stat by inode" has nothing to batch, so it should not be built.** Three
  consecutive `/work` walks put `fstatat` at 16.6 / 16.6 / 16.5 s while `getdents64` moved
  63.1 → 50.6 → 31.9 s: the inode is already resident (the directory block was just read,
  and `find` had walked the tree minutes earlier), so a stat costs path resolution and
  `copy_to_user` and no I/O at all. Batching by inode would remove the path resolution and
  leave nothing. On a genuinely cold tree the split would move — that is the one condition
  under which the idea becomes worth revisiting, and it is measurable rather than arguable.
- **`getdents64` is 652 k syscalls returning 392 B each**, 50 µs a call: one directory
  block at a time, latency-bound, single-threaded. No algorithmic fix exists — every
  directory must be read — so D6's concurrency is the only lever, and the ledger says it
  is worth roughly 3× on this tree. `esidx_add` at 1.2 µs an entry (pool intern, eight
  column stores, one child-vector push, and the page faults of a doubling table) is the
  only part of the walk that is ours.
- **Reading the split costs more than most of what it splits, on this host.** Below.

**The price of the instrumentation, and the trap that makes it necessary.** The split is
four `clock_gettime` calls per entry, so it lives at `-v 5` and not at INFO — `round.sh`,
which produces every number in this section, builds at INFO, and a split walk is slower
than a plain one. It is not at DEBUG either, because a sanitiser build logs at DEBUG by
default and the gate would pay for it. The line the level gates prints what the
attribution cost, next to the buckets it distorted:

```
scan: split: 24513642 reads at 1222 ns -- the attribution cost 29963.0 ms, 36.1% of the walk above
```

`r7000`'s clocksource list is **`hpet acpi_pm`** — there is no TSC on offer, so
`clock_gettime` is a syscall costing **1 222 ns**, against **20 ns** for the vDSO call it
is on WSL2. That is 60×, and it is why the split needs a level of its own: at INFO it
turned a 1.21 s walk of `/usr` into 3.37 s, which makes the percentages a statement about
the measurement rather than about the walk. Two consequences worth keeping:

- **Phase timings are unaffected; per-row ones would not be.** Every phase number here is
  two clock reads for a whole phase, so 1.2 µs is nothing. Nothing on a query path calls
  `ts_us()` per row either — `query.c` takes six readings for an entire query — so the
  documented query costs stand. Had a timer existed per comparison, every sort number in
  this section would have been about the clock.
- **The buckets can be corrected, because the price is printed next to them.**
  Subtracting each bucket's own reads at the 1222 ns the same run measured — arithmetic,
  not a second measurement — gives the untimed walk on `/work`: `getdents64`
  **32.8 s (59 %)**, `fstatat` **9.9 s (18 %)**, `esidx_add` **6.6 s (12 %)**, `openat`
  0.5 s, `other` 3.3 s. That is 53.1 s against the 55.7 s an untimed walk actually took,
  so the correction is good to 5 % — which is the error bar to quote with it.

Reproduce, on a host where `/work` exists:

```sh
./esidx build /work -o /tmp/work.idx          # the wall clock nobody pays for
./esidx -v 5 build /work -o /tmp/work.idx     # every INFO line, the split, and its price
./esidx -v 5 query /tmp/work.idx count:5      # load, and the re-run of finalize
```

`count:5` against that index takes 1 189 ms: the answer is five rows, and the whole table
is sorted to produce them — §6.2 step 4's problem stated as a number on a real tree rather
than on a fixture.

#### The memory ledger, and what it says is oversized

`esidx_log_mem()` prints one line per structure, `touched` against `address`, from
`esidx_log_stats()` — so every phase that reports the index reports its footprint with it.
Two columns because they are different questions: address space becomes memory only under a
refused overcommit or a `ulimit -v`, which is exactly how §5.4's 4.2 GB of ext bitmaps failed
while measuring 213 MB resident. Which of the two a row reports depends on how that structure
is grown, and is stated per row: `realloc` without a write past the old length leaves the tail
untouched, `calloc` of a large block returns zero pages nobody touches, and a `memset` of a
new range makes every byte of it resident.

`/work`, 5 476 485 entries, `r7000`, at the time the ledger landed:

| | touched | address | |
|---|---|---|---|
| name trigram lists | 450.4 | 452.1 | 15.2 postings/entry, **71 % of the capacity in use** |
| entry columns | 261.1 | 400.0 | cap 8 388 608 vs count 5 476 485 |
| sorted arrays ×3 | 250.7 | 250.7 | 188.0 would be two arrays |
| names pool | 152.6 | 256.0 | 56.6 of it is dir path copies |
| dir vector headers | 128.0 | 128.0 | 651 897 live of 8 388 608 slots |
| name rank | 63.8 | 122.6 | 1 493 203 distinct folded names |
| dir children vectors | 39.4 | 39.4 | 5 476 484 ids in 10 324 800 slots |
| ext index | 15.1 | 79.4 | `tab`: 8 388 608 slots for 6 765 extensions |
| dir path hash | 5.0 | 32.0 | load factor 0.16 |
| **total** | **1 364.7** | 1 841 | peak rss 1 418.4 |

Six things it says that no document recorded, and what has been done about each:

- **`ext_index_build()` sized `tab` to the entry count** — 8 388 608 slots for 6 765
  extensions, load factor 0.0008, plus `ids` at the same capacity for 27 KB. Sized by the
  extension count now, which the first pass discovers: 16 384 slots.
- **`di_hash_build()` sized the dir hash at 4x the directory count**, running /work at load
  factor 0.16, under a comment that said "2x entry count" — a label wrong for what the code
  does, the same class the ext bitmap commit recorded. Now 2x, which is the smallest power of
  two that cannot reach the 3/4-load growth threshold before the next doubling.
- **The columns' `cap` is the next power of two above the entry count.** Harmless resident
  (realloc never writes past `count`) and fatal under a memory cap; trimmed once at the top of
  `finalize`, where mremap shrinks in place and before anything derived has been built.
- **The trigram posting lists were resident at their high-water capacity** — 450.4 MiB
  touched for 318.6 MiB of ids. Now counted first and sized exactly, at the cost of a second
  walk over the names: −85 MiB of peak rss for +21 % on `finalize`, which is +5 % of a /work
  build and is re-run by every load.
- **The trigram posting lists are a third of the index and nothing had measured their
  shape.** 318.6 MiB, a mean of 15.2 postings an entry, and a mean cannot decide anything.
  `esidx_log_mem()` now prints the length distribution and the gap distribution, because
  the two fixes anyone would reach for want opposite numbers and neither was available
  (`log_tri_shape()`, and the measurement is its own commit rather than a footnote to
  one). On /work: 62 476 lists over 83 507 488 postings, and the volume is not spread —
  **142 lists (0.2 % of the keys) hold 17 039 716 postings, 20 % of all of them**, while
  4 731 lists hold one posting each. Nothing reaches 10 % of the table, so a stop list has
  exactly one interesting threshold on this tree, and it is the 1 % one: dropping those
  142 keys would remove 63.7 MiB, and a pattern that used one of them as its filter would
  fall back to its other trigrams — a filter may always be dropped, never required
  (§5.2's allowlist argument, run in the other direction). The gaps say the other thing:
  ids inside a list ascend, so **1.27 bytes a gap against 4 for a raw id, 3.13x over the
  whole set — 318.6 MiB would become ~102 MiB**, at the cost of decoding every posting
  the prefilter reads. That is the larger prize and it is on the query path, so it is not
  a decision this paragraph makes; it is the decision the next one has to measure.

- **The posting lists are delta-varint encoded, and the estimate above was right.**
  **318.6 → 101.8 MiB on /work** (334 029 952 → 106 762 658 bytes, 3.13x, 1.28 bytes a
  posting), which is 216.8 MiB off the accounted total — 1 006.0 → **789.2 MiB** — and
  215.9 MiB off peak rss, **1 078.0 → 862.1 MiB**. On /usr, 18.4 → 5.9 MiB and
  64.4 → 51.8 MiB accounted, 74.2 → 62.3 MiB peak. The snapshot does not move
  (313 011 674 bytes on /work): the index is derived, rebuilt by every load, so there was
  never a serialized copy to shrink. `finalize` is unchanged (6 750 → 6 706 ms on /work,
  which is inside the run-to-run spread of a phase that walks 83 M postings).
  **It cost nothing on the query path**, which is what the paragraph above said had to be
  measured rather than argued: best of 7, alternating builds, on /usr —
  `conf` plan 0.024 → 0.027 ms and eval 2.538 → 2.561 ms, `path:/usr *.conf size:>1k`
  eval 20.296 → 20.104 ms, `ext:conf` 0.031 → 0.030 ms. The decode is free because the
  reader walks one list front to back and never seeks, so it is the shape delta-1 exists
  for; the +3 µs on `conf` is the whole cost of turning 4 800 000 array reads into
  4 800 000 varint decodes.

  On /work the same harness (best of 5) puts `conf`'s plan at 0.332 → 0.385 ms, which
  reads like +53 µs until the shapes that *never touch* the trigram index are looked at:
  browse 0.481 → 0.527, `ext:conf` 0.440 → 0.464, `image:` 0.527 → 0.544. So the noise
  floor of that run is 24-46 µs on a box this size and the decode is inside it. Neither
  tree shows a shape slower than it was, and eval is flat or better on both.

  Two things about the code, both of which the sanitiser build found and `-O2` did not:
  **`-O2` does not notice a read one byte past the end of a posting list.** The array
  version tested `ids[k] < i` before it incremented `k` and so never read `ids[n]`; the
  decode has to ask whether a next posting exists before reading it, and the first
  version did not. Every list whose last id is below the largest candidate over-reads,
  and the byte past the end decodes to *something* which is then compared only against
  rows the list had already rejected — so all 301 assertions passed at `-O2` and the
  sanitiser build reported a heap-buffer-overflow in `tri_vget`. And the same shape of
  mistake with the index off by one in the other direction: counting postings read rather
  than indexing them makes `k >= n` true on the first row of a one-entry list, and a
  filter that clears everything answers **zero** for every literal whose shortest list
  holds a single id. That one *was* visible at `-O2` — 38 assertions failed — because
  most lists hold one id.

  The test reaches the widths by construction rather than by luck, and the route to it is
  the part worth recording: **a file's id is its position in the walk, which is readdir
  order, so no fixture can place a wide posting by naming a file.** An id of 16 384 needs
  a 16 384-entry tree and *the right end of it*, which naming does not choose. The
  reconcile path does: `esidx update` appends ids and never reuses one (D8), so an entry
  added to a 17 000-entry index is necessarily above 16 384 however readdir ordered the
  directory. The fixture therefore builds 17 000 files and then adds `wibble.dat` (one
  posting, the first of its list, so stored whole — three bytes) and `markaaa.dat` /
  `markbbb.dat` with 200 files between them (a gap of 201, two bytes) in one update pass.
  The fourth width needs a value of 2²¹ and no fixture here has the id space, so no
  assertion claims to reach it.

  **And the stop list is not a threshold away, which reading the code settles.**
  `tri_index_filter()` treats "this literal's trigram has no posting list" as a narrowing
  all the way to zero (`trigram.c:331`), and that is sound only because *every* trigram a
  name can produce is indexed. Stopping one would make the branch answer **zero rows** for
  every pattern containing it — the failure this codebase keeps calling the worst — so
  §5.2's "a filter may be dropped, never required" does not hold here as written. It needs
  the index to tell "no name has this trigram" from "this trigram was stopped" (a small
  key set beside the table) and the filter to skip the stopped ones rather than narrow on
  them. The measurement says the prize is 63.7 MiB; the code says the price is a semantic
  change to the one place where a wrong answer is invisible.
- **`di.child` was indexed by entry id.** 651 897 of 8 388 608 slots were a directory's, and the
  growth path memsets every new range, so all 128 MiB was resident. It is indexed by a dense
  **directory ordinal** now, with an open-addressed eid → ordinal map (6.4 MiB) beside it:
  16 MiB of headers, −112 MiB of peak rss. A column would have cost 22 MiB on every row — and
  22 MiB in the snapshot — to serve the 12 % that are directories, and rank/select is O(n) to
  repair after a reconcile inserts one directory. **Ordinals go to directories that have
  children**, which on /work is 630 472 of 651 897: a childless directory needs no vector, and
  not giving it one costs nothing.
- **The names pool holds 3.65 copies of every name** — 1 499 994 distinct basenames against
  5 476 485 entries, and `sp_intern()` only appended despite the name. **Done**, and it is
  the largest single item the ledger ever named: the pool is 37.3 MiB where the distinct
  names need about 26, against 96.0 before, for a 5.7 MiB table (2 097 152 slots, 0.71
  load) beside it. The snapshot follows the pool down, 374 608 534 → 313 011 674 bytes,
  because the pool is written verbatim and deduped bytes are bytes not written twice.
  What it costs is one hash and one probe per entry *in the walk* — measured on /work,
  +0.5 s of user time over 5 476 485 entries, against −2.1 s of system time from a pool
  that no longer doubles to 128 MiB, so the CPU is a wash and the memory is not. It is
  built by `esidx_add()` as names arrive and once on the first add after a load, lazily,
  so a server that is only served from never pays for the pass.

**`sidx_ent_t`'s padding is gone too**, and the sort is why that was free rather than
merely smaller. The struct was `{int64 v; eid_t id}` — 16 bytes, 4 of them padding, 62.7
MiB of it across the three arrays on /work — and the array is now an `int64` and an `eid`
per row, 12 bytes. Sorting a 16-byte struct and splitting it afterwards would have needed
88 MiB of scratch at the moment the trigram index is also at its largest; instead `id`
starts out as 0..n-1 and the values come from a column, so the array is built by sorting
the *ids* against that column with `qsort_r` and gathering afterwards — nothing transient
at all. The merge became a linear merge of two sorted runs, which is both O(n) instead of
O(n log n) and 12 bytes a row of output while holding 12 a row of input, where the old
code held 16 and allocated 16. 250.7 → 188.0 MiB, peak rss 1 173.7 → 1 114.1.

That merge rewrite also fixed a bug the split introduced and the sanitiser build caught
within the hour: `cmp_id_by_val` reads a value *by id* out of the array it is handed,
which holds in `sidx_build` (id *is* the index) and does not hold in a merge, where the
ids in the array are not indices into it — a heap-buffer-overflow inside `qsort_r`, on a
tree of nine files, in the one path the index suite exercises hardest. It showed up as 33
assertions failing *downstream* of an `esidx update` that died, which is the shape
AGENTS.md §3.1 warns about: the abort leaves a stale snapshot and every assertion after it
is reading yesterday's index.

The 56.6 MiB of directory paths copied into the dir hash's pool is the next one, and it is
a different animal: derived data (D4) that should not be in a persisted pool at all, which
is why it lives in one of its own until it does not.

Where the ledger stands after the commits that acted on it, `/work` again:

| | touched | address | |
|---|---|---|---|
| name trigram lists | 318.6 | 320.3 | exact-sized; 20 % of the postings sit in 142 of the lists |
| entry columns | 282.0 | 282.0 | 261.1 + the `nchild` aggregate |
| sorted arrays ×3 | 188.0 | 188.0 | **split into two arrays** — was 250.7 with 62.7 of padding |
| names pool | 37.3 | 64.0 | **interned** — was 96.0 / 128.0 |
| name intern table | 5.7 | 8.0 | 2 097 152 slots for 1 499 995 distinct names |
| dir vector headers | 16.0 | 16.0 | indexed by directory ordinal |
| name rank | 63.8 | 122.6 | |
| dir children vectors | 39.4 | 39.4 | 5 476 484 ids in 10 324 800 slots |
| eid → dir ordinal map | 6.4 | 6.4 | 2 097 152 slots for 630 472 directories |
| dir path hash | 5.0 | 16.0 | load factor 0.31 |
| dir paths pool | 56.6 | 64.0 | 651 897 whole paths, 91 bytes a directory |
| **total** | **1 032.5** | 1 579 | **peak rss 1 114.1**, was 1 418.4 at the first ledger |

The children are one array now, so the last table's two rows above are history: the ledger
after the CSR reads

| | touched | address | |
|---|---|---|---|
| dir range table | 8.0 | 8.0 | 1 048 576 slots × 2 uint32, for 630 472 directories |
| dir children array | 20.9 | 20.9 | **5 476 484 ids in 5 476 484 slots — 100 %** |
| **total** | **1 006.0** | 1 547 | **peak rss 1 078.0** |

which is −26.5 MiB accounted and −36.1 MiB of peak: the extra 9.6 is the per-block
overhead of 630 472 allocations, which the accounted figure never had and the kernel
always did. It costs **+810 ms on `finalize` over 5 476 485 entries (+13 %, and +5.6 % of
a /work build's user time)**: the build is now three passes over the columns with two
`di_ord()` probes an entry, and the probes are the price of not keeping a 22 MiB ordinal
column. Half of it could be had by maintaining the per-range counts in `di_add_child()`
instead of recomputing them, which would leave two counters for one number and rely on the
ledger's cross-check to catch them disagreeing — measured, declined, and recorded here so
the next reader does not have to re-derive it.

The walk never allocates a per-directory vector at all now. That was not a goal; it is what
happened when the shape stopped being a vector, and it is why a fresh build's peak drops by
the same 36 MiB rather than only its steady state.

The encoding takes the first row of the last table, and it is the largest single thing the
ledger has ever named:

| | touched | address | |
|---|---|---|---|
| name trigram lists | **101.8** | 104.1 | **delta-varint** — was 318.6 / 320.3, 3.13x |
| everything else | as above | | unchanged; the snapshot is byte-identical |
| **total** | **789.2** | 1 330 | **peak rss 862.1**, was 1 078.0 |

Two thirds of one row and 216.8 MiB of the total. What it cost is 8 bytes a slot — the list
grew from 16 to 24 bytes for `nb` and `last`, which is 0.5 MiB on /work against 216.8
saved, and is why the *address* column falls by less than the *touched* one. The remaining
item on the list is still the dir paths pool at 56.6 MiB, which is derived data in a
persisted pool and a separate decision.

The `dir paths pool` row is new and the total moved with it, which is the point: the row was
already being printed, carrying the reason "it is part of the names pool above, so it does
not go in the total" — a reason that stopped being true when the dir hash was given a pool
of its own. **56.6 MiB of resident memory was being displayed on a line that said it was
counted somewhere else, and was counted nowhere.** Every "the index accounts for N MiB"
figure before this one is therefore 56.6 MiB short of the truth; peak rss was always right,
because peak rss is measured rather than summed. `test.sh` now asserts that the total is
the sum of the rows that feed it, in exact bytes, which is checkable because the rows print
bytes as well as MiB — and a row that is *not* in the total says `(--)` in its percentage
column instead of being indistinguishable from one that is.

---

## 11. Decisions

Closed. Each states the choice, the reason, and the evidence.

### D1 — Port `etp_server.c`; do not rewrite the FTP layer

`everything_plugin_proc()` (`etp_server.c:742-766`) resolves ~60 procs (the
whole `db_*` family) from the Everything host process at init. The FTP layer —
sockets, `PASV`/`EPSV`/`EPRT`, `MLSD`/`MLST` formatting, `SIZE`/`MDTM`, the
`SITE EVERYTHING` dispatch — is decoupled from all of it.

**Decision**: keep the FTP + `SITE EVERYTHING` command layer wholesale, replace
the `everything_plugin_*` adapter with our own index engine.

**Reason**: an FTP server is a few thousand lines of subtle state machines,
passive/active mode handling, encoding conversion and `MLSD` formatting.
Rewriting it buys nothing. Porting means replacing the Windows-specific parts:
`WSAStartup`, `SOCKET` → `int`, `__declspec(dllexport)` → plain export, and the
`everything_plugin_utf8_*` string API → ordinary `char*`. Budget 300-500 edit
sites; extract the needed subset of `everything_plugin.h` behind a shim first.

### D2 — C11

**Reason**: the D1 port only makes sense in the same ecosystem. zstd is a C
library; CRoaring has a usable single-header C form. The target is a Linux
daemon with no cross-platform UI, so manual `malloc`/arena management is an
asset rather than a burden. Rust or Go would turn the D1 port into a rewrite.

### D3 — Numeric index: sorted array + delta buffer

**Decision**: an ascending `(value, entry_id)` array stays resident; incremental
changes append to a small unsorted delta array; a query is
`binary search over the main array ∪ linear scan of the delta`; when the delta
exceeds 1% of the main array or has been idle 60 s it is merged in.

**Reason**: pure sorted-array insertion is O(n) memmove, which fanotify-driven
updates cannot absorb [F2]. Tiered bitmaps update in O(1) but return a superset
requiring a second filtering pass, and their space grows with the bucket count.
The LSM-style delta gives O(1) updates and O(log n + |delta|) queries with the
smallest implementation, and it slots straight into §5.3.

Merges persist alongside the snapshot strategy in D4.

### D4 — Persistence: memory-resident, snapshot to disk

**Decision**: the whole index lives in memory while the process runs; a
snapshot is written on exit and on a timer, and loaded at startup. **No
compression** during P0-P3 — a million names occupy roughly 100-200 MB, which
mmap maps directly. Block-level zstd [A5][A6][A8][A11] is deferred to P4 and
gated on the entry count exceeding ~10⁷.

**Reason**: both upstreams persist because their processes cannot stay resident.
We are a long-running daemon; the index must be in memory to answer in
milliseconds, and persistence exists only to avoid a full rescan on restart.
mmap loading is a technique FSearch has already validated
(`fsearch_database_file.c`).

*Implementation note*: `esidx_load` currently reads the snapshot with `fread`.
The mmap path is D4's intent and is still to do.

### D5 — No SQLite; everything self-built

**Reason**: columnar layout + bitmaps + a memory-mapped snapshot is the wrong
shape for SQLite's B-tree row store, and avoiding the dependency keeps deployment
to any Linux box trivial. The content index (P6) can be self-built too if it
turns out to be simple enough.

### D6 — Scan concurrency adapts to the medium

**Decision**: read `/sys/block/<dev>/queue/rotational`. HDD → concurrency 1-2,
because parallel seeks are slower than sequential reads [E3]. SSD/NVMe →
concurrency = core count, splitting top-level subdirectories with work stealing.

**Reason**: both upstreams scan single-threaded [E2], but for different reasons —
they are single-shot batch tools. We must be parallel on an SSD and must *not*
be parallel on an HDD, so the concurrency has to be measured, not fixed.

### D7 — P0 uses a dense bitset; P4 swaps in CRoaring

**Decision**: P0-P3 use `uint64_t`-word dense bitsets (no dependency, ~100 lines).
P4 introduces CRoaring's single-header build behind an unchanged
`bs_init`/`bs_free`/`bs_set`/`bs_test`/`bs_clear` interface.

**Reason**: at fewer than 10⁷ entries a dense bitset is ~1.25 MB — irrelevant
next to the string pool. Pulling in Roaring earlier only adds debugging surface.

### D8 — Removal tombstones the row; ids are never reused

**Decision**: `esidx_remove()` sets `EF_DEAD`, clears the id's `live` bit and
leaves the row in place. Once tombstones exceed a quarter of the entry count,
`esidx_compact()` rebuilds the index from the filesystem.

**Reason**: the obvious alternative — a free list, handing a dead id to the next
`esidx_add()` — looks free and is not. The id is referenced by the main sorted
array (with the old value), by the delta (with a retraction of it), by the ext and
type bitmaps, and by every `qset` the protocol layer is still holding. Reusing it
means the retraction left in the delta can clear the *new* entry's bit, because the
range reader walks the delta in order and a retraction always applies. The
ordering that makes that safe would have to be enforced by every future writer.

Tombstoning makes correctness local: `live` is the only authority on what exists,
and it is derived from a flag in the column dump, so a snapshot round trip needs no
reconciliation state at all — which is why the snapshot is still a plain dump of
the columns and only gained the directory stamp.

**Cost, measured**: memory grows with the tombstone ratio, and so does the
per-query `bs_next` walk over the candidate words. Both are why compaction is
automatic at 25 % rather than never — but §10 records that a tree whose *names*
change costs nothing, so in practice the threshold is reached only by a workload
that rewrites directories wholesale, and there a full rebuild is the cheaper
answer anyway.

---

## 12. Risks

1. **Histogram staleness** (§6.2) — after incremental changes the optimiser picks
   the wrong driver index. Mitigation: recompute on the same schedule as the D3
   delta merge. Partly already true: the `size:`/`dm:`/`dc:` estimates count the
   main array without its retractions, so they are upper bounds between merges.
2. **`si:`** has no Linux counterpart. Parse it, reject it at execution time.
3. **Regex cannot be indexed.** `regex:` is a full scan. Literal trigrams can be
   extracted from the pattern to pre-filter — plocate's `parse_trigrams` does
   something similar for globs via its `WILDCARD_UNIGRAM` handling.
4. **Short query terms degrade** — names under 3 bytes produce no trigram, so the
   full-scan fallback in §5.2 must stay.
5. **Sorting dominates unfiltered queries** — 42 ms of the 46 ms on `/usr`. The
   TopK path (§6.2, D4 in §9) is the fix; until then, an unfiltered `QUERY` is
   the worst case.
6. **D1 port size** — the Windows socket init and the
   `everything_plugin_utf8_*` string API must each be replaced site by site.
   Estimate 300-500 sites; do the `everything_plugin.h` shim first.
7. ~~**`path_of()` is O(depth) with an allocation per call.**~~ The allocation
   was never in `path_of()` — it fills a caller's buffer out of a `strref_t
   stack[256]` and returns; the 64 KiB buffer is allocated once per query, in
   `text_match`, and reused down the candidate set. What *was* per-row was in the
   **sort**: `sort_string()` handed a path sort a fresh 64 KiB buffer per row and
   held every one until the sort ended, so an unfiltered `/usr` asked for 23 GB of
   address space and, where malloc refused, silently sorted by name instead. Fixed
   by sizing each copy to its path and dropping the cache, which existed but could
   never hit (one `sort_string()` call per row, in the extraction loop). So the
   remaining cost really is just the O(depth) walk, per row, for display and for a
   path sort — and a materialised path column is still what would fix `path:`
   (design §2, L2).
8. **A periodic pass cannot see a change that happened *and was reverted*
   between two passes** — or, more practically, a file whose attributes moved
   while its parent's mtime did not is invisible to the names pass until the next
   deep one. That is the whole reason the deep pass exists and the reason its
   interval is a policy choice rather than a performance one. This is unchanged by
   `esidx serve --refresh=SECS` and worth being explicit about, because that flag
   looks like it changes the answer: it does not. It runs the *names* pass, and
   deliberately so — a deep pass stats every entry on the tree, which is 4.23 s on
   `/usr` and a minute on `/work`, inside a loop that must keep answering clients.
   So with `--refresh`, the index is at most `SECS` behind on *names* and still one
   deep-pass interval behind on attributes, and a serving deployment needs
   `esidx update --deep` on its own schedule for those.
   **`--watch` does not change it either, and that is a decision rather than an
   omission.** The watcher is told about `CLOSE_WRITE` and `ATTRIB` and does not
   subscribe to them: listing a directory cannot see an attribute change, so
   handling them would cost a reconcile per write and still leave size and mtime
   stale. What it removes is the *first* half of this risk — with `--watch`, a name
   change is visible within one event batch (~50 ms measured on WSL2) instead of one
   `SECS` interval, so the window in which a created file is invisible closes. The
   honest statement for `--watch` is therefore: **names lag by one event batch,
   attributes still lag by one deep-pass interval.** Closing the second half means a
   per-file stat on the event's own path (`stat` + `di_lookup_name` + `esidx_touch`),
   which is a separate layer with its own measurements — it is proportional to real
   changes rather than to the tree, but it is not free and it was not measured here.

   **The reach of a *stamp-pruned* pass is a separate risk, and it was found by the
   watcher's own loss path.** `reconcile_dir()` decides to descend into a child by
   comparing that child's stored stamp with a fresh stat (`scan.c:550`), and it only
   ever reaches a directory through its parent. A directory's stamp moves when *its own*
   entries move, so an isolated change at depth ≥ 2 moves the stamp of the directory
   holding the name — which is reached through a parent whose stamp did not move.
   Measured, fixture `root/a/b/` (`./esidx update`, no watcher involved):

   | change | names pass | names pass, one level up | deep pass |
   |---|---|---|---|
   | create `root/a/b/new.txt` | **not seen** | seen (`root/a/deep.txt`) | seen |
   | rename `root/a/b/f1.txt` → `f1-renamed.txt` | **not seen**, old row survives | seen | seen |

   So the honest statement about `esidx update` and `--refresh` is **not** "correct for
   anything that changes a name": it is *correct for a change that moves a directory on
   the path from the root to the name*, which for an isolated change means depth 1. On a
   real tree that is the whole tree above the top level, so a file created in
   `/usr/share/doc/…` is invisible to a names pass — this is why every measurement in
   §10 quotes the *idle* cost of a pass (0.4-1.1 ms, 48 of 49 directories skipped on
   their stamp) rather than its cost when something changed. It is also inherited by the
   startup repair pass and by the watcher's loss-signal response (§7 "Events").

Two ways out. One is the **sweep** (§7), which compares every directory's stamp instead of
only the ones a walk reaches: it closes this for the startup repair pass, for a proxy loss
signal and for `--sweep=SECS`, and it is the reason the claim above is now bounded by depth
rather than by nothing. The other is the watcher, which sidesteps the question entirely by
marking the directory the event *names* — so a deployment with `--watch` and a recent
`--sweep` does not have this hole at all, and one with neither still does.

What the table's numbers have to be read with: they are a **fixture**, and the fixture is
why the answer was missed rather than why it is right. Every change it makes sits at depth
1, where the directory holding the name is a direct child of the root and is therefore
always listed — the one place where the two spellings of "reachable" cannot differ. The
sweep's own assertions are the same fixture plus a second directory one level down, with
the depth-1 case kept beside it as the control that has to keep working.
9. **Two text-matching bugs, both pre-existing and both found while writing the
   incremental tests** — both fixed now, and both pinned against the reference
   rather than against our reading of the code:
   - `text_match()` retried every plain substring against the *backslash* path
     form (`wire_form()`), so `name:nm` matched every row under `/tmp/nm`: a term
     that names a file also matched every file below a directory of that name. The
     retry is right for `path:` (the client does send backslashes) and wrong for
     `name:`. It is now scoped to the terms that read a path, which is what
     Everything does — measured on one directory against voidtools' own server on
     :21: `esidx` answers 3 there, the three entries *named* esidx, where reading
     the path as well would answer 280.
   - `wc_match()`'s single-`*` loop (`regex.c:619`) exited on the end of the
     subject without retrying the empty tail, so `*foo*` never matched anything
     with characters after `foo`. `*.conf` worked because the pattern ends in a
     literal; the suite had no pattern ending in `*`.

10. **The scope of a term is `text_match()`'s decision, and it takes three rules,
    not one.** Measured on one directory, ref = voidtools' server on :21 and ours on
    the same tree — every number in the table is re-measurable with `./cmp_ref.sh`,
    which is also the term list. The counts are from the run of 2026-10-04 and move
    with every commit, because the tree is the repo; the rules are the point.

    Every row agrees, and the 12 that the path-scoped rows carry are **not ours**:
    `cmp_ref.sh` prints the entries the two indexes disagree on and stats each one,
    and all 12 are `.git/objects/<xx>/tmp_obj_XXXXXX` — git's loose-object temp
    names, renamed away long ago and still sitting in voidtools' NTFS index, which
    lags deletion. None of them is on disk (`Get-ChildItem -Force -Recurse -Filter
    tmp_obj_*` finds none), so our smaller number is the complete one. It is the
    same 12 in every path-scoped count, which is why the script prints the delta
    once and marks those rows `= delta`.

    | term | ref | ours | rule |
    |---|---|---|---|
    | `esidx`, `name:esidx`, `*esidx*`, `regex:esidx`, `ww:esidx` | 3 | 3 | no separator in the value: the **filename** |
    | `path:esidx`, `esidx\main.c` | 307 / 1 | 295 / 1 | `path:`, or a separator in the value: the **path** |
    | `esidx` + sep + `*` (either spelling) | 39 | 39 | `find -maxdepth 1` says 39, the tree not counted: the direct children |
    | `sidx` + sep + `*` | 0 | 0 | a wildcard may not begin inside a component |
    | `*esidx/main.c`, `path:*/main.c`, `folder: esidx` + sep + `*` | 1 / 1 / 3 | same | |
    | `path:*esidx*`, `path:*PC*`, `path:**esidx**` | 307 / 6617 / 307 | 295 / 6605 / 295 | `path:` + a leading star: **contains** |
    | `path:*esidx` | 2 | 2 | no trailing star: ends-with |
    | `path:*PC/esidx*` | 1 | 1 | a value with a separator is a fragment, and a fragment's trailing star cannot cross one |
    | `path:esidx*`, `path:**esidx**` | 3 / 307 | 3 / 295 | no leading star: anchored at a component |

    So: a leading star in an explicit `path:` value is what makes it a `contains`
    test, and that is the only place a single star crosses a separator. Everything
    else — a bare term with a separator in it, and a `path:` value without a
    leading star — is anchored at a component boundary with a star that stops at
    the next one. `*PC/esidx*` (bare) is the shape that keeps this honest: contains
    would be 267 there and the reference says 1.

11. **A wildcard over a path is a scan over several offsets of every path.** The
    candidate offsets are the component boundaries and the separators, so an
    *anchored* path term with a star costs O(path length × pattern) per row:
    1.8 ms of eval for `esidx/*` over 6577 entries, against 0.4 ms for the bare
    `esidx` on the same tree and 2.1 ms for the substring `path:esidx`. The
    contains form is not in that class at all — `path:*esidx*` is one
    `strcasestr` over the path, 1.0 ms, because a star on both ends of a value
    means "occurs anywhere" and nothing else.

12. **The indexed root is a row like any other, and one rule covers it.** Two shapes
    off voidtools' server on :21, both captured with `etp-probe 21`:

    ```
    ROW 0    FOLDER C:        path=
    ROW 854  FOLDER ShareToPC  path=C:\Users\linswin\AndroidStudioProjects
    ```

    Neither mentions a root, and one rule produces both — the name is the last
    component of the entry's own path, PATH is everything before the last separator
    in it. `dirname("C:")` has no separator, hence the empty PATH; cut the other
    path and the parent falls out. So the two accessors are `basename(name_of())`
    and `dirname(path_of())`, with **no branch on whether the entry has a parent**:
    §4.2 keeps no full-path column, so `path_of()` rebuilds the path from the parent
    chain, and the root's stored name is the absolute path it was indexed from
    (`scan.c:169`) precisely because that is the only place the absolute prefix
    exists — `path_of(root)` and `di_lookup(db, root)` in `update`/`compact` both
    read it.

    That is also where the bug was. The root's *stored* name had leaked into what
    the wire and `name:` matching call "the name": it printed its own path as its
    name, left PATH empty, and `name:<the parent directory>` matched 1 row here
    against 0 on `:21`. Both suites passed for a long time because **every other
    row's parent is in the index**, so the two spellings only ever differ on that
    one row, and no term in `test.sh` or `cmp_ref.sh` read it. Found by dumping both
    servers' whole result sets and diffing the paths, which is now `cmp_ref.sh`'s
    `explain_delta` — it named the row and flagged it `ON DISK`, which is the only
    reason it was noticed rather than guessed at.

