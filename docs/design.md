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
| Posting encoding | delta-1 + PForDelta 128 | [A10] |
| Block layout | interleaved for full blocks, plain for the tail | [A11] |
| Bit-width selection | histogram + byte-cost enumeration | [A14] |
| Trigram generation | 4-byte `memcpy`, take the low 3, advance 1 | [A9] |
| Hash table | Robin Hood | [A12] |
| Compression | zstd dictionary trained on the corpus | [A6][A8] |

**Activation threshold**: above 10⁶ entries, or when measured input latency
exceeds 100 ms. Below that, an in-memory array plus substring scan is enough —
FSearch demonstrates that at desktop scale.

**Mandatory fallback**: names shorter than 3 bytes produce no trigram [A9], so a
full-scan path must remain for short queries.

**Prefix / suffix** are cheaper as binary searches over sorted name and
reversed-name arrays than as trigram lookups. Build those alongside.

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

### 5.4 Enum bitmaps

`is_dir`, `ext_id`, each `attrib` bit, `index_type` each get a bitmap. The set
algebra (AND / OR / NOT) maps directly onto the language's `space` / `|` / `!`
operators.

Dense `uint64_t` words for now, swapped for CRoaring later behind an unchanged
`bs_init` / `bs_set` / `bs_test` / `bs_clear` ABI — decision D7.

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
plocate only stores paths and can skip stat entirely — so the stat success rate
and the getdents byte volume are the two numbers that decide whether the
planned "batch stat by inode" optimisation is worth building. Both are tracked
in `scan_stats_t` and logged at DEBUG.

**getdents buffers.** One buffer per recursion level, taken from a lazily grown
pool (`scan.c:61`). A per-frame local buffer would put
`depth × 128 KiB` on the stack — 64 MB at the 512-level cap. A single shared
static buffer would be clobbered by the recursive call while the parent frame is
still walking it. Both are wrong; the pool is neither.

**Depth cap.** 512 levels. Crossing it emits a warning naming the truncation
rather than silently dropping subtrees.

**Concurrency** (D6): adaptive to the medium, not yet implemented.

**Incremental.** Directory-mtime skip [A2] as the privilege-free baseline;
fanotify with `FAN_EVENT_ON_CHILD` [B1][B2][B3] when running as root, falling
back to inotify [B8]. New directories are recursively scanned on create [B5];
events are drained in batches [B7]; events under a deleted directory are
skipped [B4].

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
| B1 | `fsearch_folder_monitor_fanotify.c:275` | `fanotify_init(FAN_CLOEXEC\|FAN_NONBLOCK\|FAN_CLASS_NOTIF\|FAN_REPORT_DFID_NAME, O_RDONLY)` | as-is | `FAN_REPORT_DFID_NAME` is what yields a directory fid plus a name |
| B2 | `fsearch_folder_monitor_fanotify.c:369` | `fanotify_mark(fd, FAN_MARK_ADD\|FAN_MARK_ONLYDIR, MASK, AT_FDCWD, path)` | as-is | directories only |
| B3 | `fsearch_folder_monitor_fanotify.c:24` | mask includes **`FAN_EVENT_ON_CHILD`** | **key adoption** | one mark covers a whole subtree, sidestepping inotify's per-directory watch and `max_user_watches` |
| B4 | `fsearch_database_index.c:132-160` | `get_skippable_events()`: drop events under a deleted directory | as-is | saves tens of thousands of no-ops on `rm -rf`; the prefix test requires `str[len] == '/'` |
| B5 | `fsearch_database_index.c:581` | a create event that is a directory triggers a recursive scan | as-is | a new directory is usually a bulk copy in progress |
| B6 | `fsearch_database_index.c:571-576` | pull ancestors out of the index before mutating, put them back after | idea adopted | aggregates must bubble up the parent chain |
| B7 | `fsearch_database_index.c:199` | `g_async_queue_length()` batch draining | adopted | per-event handling jitters under bulk change |
| B8 | `fsearch_folder_monitor_*.c` | dual backend dispatched on `event->monitor_kind` | adopted | automatic degradation to inotify without root |

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
| 4.2 | `path_of()` parent-chain rebuild | `store.c` | done; path materialisation pending §4.2 |
| 5.1 | `dir_id → children`, `path → eid` hash | `store.c` | done |
| 5.2 | trigram index, sorted/reversed name arrays | — | not started (P4); the in-memory scan is measured below and is the reason |
| 5.3 | sorted array + delta buffer | `store.c` | structure done; the delta path has no writer until P2 |
| 5.4 | dense bitset | `index.c` | done; CRoaring at P4 (D7). Set algebra lives here, not in the executor |
| 5.4 | ext bitmaps, file:/folder: bitmaps | `store.c` | done — built in `finalize`, so the snapshot format is unchanged |
| 5.5 | aggregate columns + bubbling | — | not started (needs P2); `child-count:` is derived from the children vector instead |
| 5.6-5.8 | content, dupe, sparse metadata | — | not started (P6) |
| 6.1 | lexer → parser → AST | `lexer.c`, `parser.c` | **done** |
| 6.2 | optimiser: selectivity estimate, driver selection | `query.c` | **done** for step 1-2 (exact cardinality per leaf, no histogram yet). Step 3 ordering and step 4 TopK not started — see below |
| 6.3 | execution: candidates → bitmaps → matchers → sort → slice | `query.c` | **done**, including the second-stage FILTER_* pass |
| 6.4 | result cache | `etp.c` | **done** — the full sorted set is kept and re-sliced |
| 7 | full scan | `scan.c` | done; concurrency (D6) not started |
| 7 | incremental | — | not started (P2) |
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
  the dominant cost of a real query. The gate should be revisited at P4 — either
  lower the threshold or add the name-sorted and reversed-name arrays from §5.2,
  which are cheaper than a trigram index and would serve `startwith:`/`endwith:`
  directly.

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

---

## 12. Risks

1. **Histogram staleness** (§6.2) — after incremental changes the optimiser picks
   the wrong driver index. Mitigation: recompute on the same schedule as the D3
   delta merge.
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
7. **`path_of()` is O(depth) with an allocation per call** [upstream-notes §3.4].
   Fine for display; will need the materialised path column once `path:`
   queries or deep trees make it a hotspot.