# Upstream survey: plocate and FSearch

Distilled from the source reading recorded in [`research-log.txt`](research-log.txt).
Every claim below is anchored to `file:line` in the upstream trees; the raw
transcript is kept only for provenance.

## 1. Sources

| Project | Version | Location (WSL Ubuntu 22.04) | Obtained by |
|---|---|---|---|
| plocate | 1.1.15-1ubuntu2 | `/root/src-dl/plocate-1.1.15` | `apt-get source plocate` |
| FSearch | master `d531eb3` | `/root/src-dl/fsearch` | git clone — not packaged for Ubuntu |
| etp_server | 1.0.2.5 | `../../etp_server-1.0.2.5/src/etp_server.c` | in-repo |

Windows access: `\\wsl$\Ubuntu-22.04\root\src-dl\`.
The WSL default user (`linsmod`) needs a password for sudo; the trees were
fetched with `-u root` and `/root` is mode 700, so reading them also needs
`-u root`.

## 2. plocate

### 2.1 Scanning — adopted

```cpp
// updatedb.cpp:96
int fd = openat(dirfd, path, O_RDONLY | O_DIRECTORY | O_NOATIME);
```

- **A1** `openat` on a relative name. Avoids `PATH_MAX` and the TOCTOU window
  between path walk and use (`updatedb.cpp:526` comment).
- **A4** `O_NOATIME` returns `EPERM` for files you do not own; retry without it.
- **A3** `d_type` gives the type for free, `fstatat` only on `DT_UNKNOWN`
  (`updatedb.cpp:618-646`). The comment there names XFS with `ftype=0`
  explicitly. On ext4 `d_type` is always populated.
- **A2** Directory mtime (sec **and** nsec) unchanged since the last build →
  skip `readdir()` and reuse the stored entries
  (`updatedb.cpp:601-603`). This is the only privilege-free incremental
  mechanism available, and the data it needs is the directory-mtime region of
  the database (`database-builder.cpp`, `EncodingCorpus::add_file`).

Two things plocate does *not* do, which is why its scan is a useful but
incomplete model:

- **Scan is single-threaded** (`updatedb.cpp`, `scan()` recursion).
- **io_uring is used only by the query side** (`plocate.cpp:128`); the scan
  never uses it.

### 2.2 Index format — the parts worth copying

`db.h` `Header` is the entire layout:

| Field | Purpose |
|---|---|
| `hash_table_offset_bytes`, `hashtable_size`, `extra_ht_slots` | trigram hash table, open addressing + overflow slots |
| `num_docids`, `filename_index_offset_bytes` | docid → filename region |
| `zstd_dictionary_offset_bytes` | per-database zstd dictionary |
| `directory_data_offset_bytes`, `next_zstd_dictionary_*` | directory mtime region (drives A2) |
| `check_visibility` | permission filtering |

**Builder** (`database-builder.cpp`) is two passes:

1. Sample 1000 blocks of 32 filenames each (reservoir sampling, fixed seed
   1234 for reproducibility) and train a zstd dictionary with
   `ZDICT_trainFromBuffer` (`:160`), compiled to a `ZSTD_CDict` at level 6
   (`:544`). A dictionary trained on short strings is the single biggest reason
   the database is small.
2. Real pass: generate trigrams, encode postings, compress, write.

| # | Source | Design |
|---|---|---|
| **A5** | `conf.cpp:72` `conf_block_size = 32` | 32 filenames per block |
| **A7** | `database-builder.cpp:303` `docid = num_blocks` | **docid is a block number, not a file number.** A hit means "this block", so the whole block is decompressed and matched precisely. This trades block-level false positives for sequential I/O and amortised decompression. |
| **A9** | `:326-329` | `memcpy` 4 bytes, take the low 3, advance 1 byte. The trailing `\0` makes the over-read safe; termination is detected by `trgm <= 0xffffff`. Side effect: names shorter than 3 bytes produce no trigram, hence `scan_all_docids()` exists and short queries degrade to a full scan. |
| **A10** | `:56-70` | Postings are delta-1 encoded (guarantees non-negative), emitted in groups of 128 |
| **A11** | `:96-102` | Full blocks use `interleaved=true` (4-way, SIMD-friendly); a short tail block cannot |
| **A14** | `:267-340` | `decide_block_type()`: bit-width histogram, then enumerate candidate widths and take the cheapest byte cost. Not a fixed threshold. |
| **A12** | `:463` | trigram hash table: prime size, linear probing, Robin Hood swap to flatten the longest probe; overflow slots + sentinel at the end |
| **A8** | `:340-341` | each block compressed on write, `filename_blocks[]` records file offsets — that array *is* the query-side `offsets[]` |
| **A13** | `parse_trigrams.h:50-52` | **trigrams are byte-based, not codepoint-based.** Explicitly to keep tables uniform and prevent CJK combinatorial blowup. Non-negotiable for us. |

TurboPFor is vendored and rewritten (comment claims ~80% of upstream
performance, no SSE4.1/AVX requirement). Block layout is one header byte:
low 6 bits = bit width, high 2 bits = block type.

| Type | When | Layout |
|---|---|---|
| `CONSTANT` | all 128 deltas equal | the single value |
| `FOR` | no exceptions | width + 128 fixed-width values |
| `PFOR_BITMAP` | many exceptions | + exception width(8b) + bitmap + exceptions + base values |
| `PFOR_VB` | few exceptions | + exception count(8b) + base value + varbyte exceptions + exception offsets |

`PFOR_BITMAP` layout, verbatim from `turbopfor.cpp:491-497`:

```
//  - Bit width (6 bits) | type << 6
//  - Exception bit width (8 bits)
//  - Bitmap of which values have exceptions (<num> bits, rounded up to a byte)
//  - Exceptions (<num_exc> values of <bits_exc> bits, rounded up to a byte)
//  - Base values (<num> values of <bits> bits, rounded up to a byte)
```

The idea is fixed-width plus exceptions: pick a bit width *below* the maximum,
store the low bits inline, and push the overflow into a side list. The
exception test is one line (`turbopfor-encode.h:207`):

```cpp
bs.write((in[i] >> bit_width) != 0);
```

Cost model (`:292-312`): straight FOR first, then PFOR-with-bitmap for each
candidate exception width; the varbyte branch cost is an estimate from the
cumulative histogram (comment: ~0.1% effect on total database size).

Minimum viable subset if implementing from scratch: `write_baseval`,
`encode_pfor_single_block<128>`, `decode_pfor_delta1_128`. The two things that
actually bite are the bit packing order (little-endian, may straddle bytes) and
the delta-1 off-by-one (`prev = out[i] = bs.read() + prev + 1`).
Storing plain `uint32` arrays uncompressed is entirely adequate if the index
stays resident in memory below ~10⁷ entries — the compression exists so a
200 MB database can be `pread` quickly from disk.

### 2.3 Query pipeline — philosophy adopted, mechanism not

```
needle -> parse_trigrams() -> conjunctive trigram set
       -> hash lookup -> posting lists (PForDelta-compressed, sorted docids)
       -> intersect groups -> candidate docids   (a false-positive set)
       -> batched pread of the filename region -> decompress
       -> exact match inside each block -> emit
```

| # | Source | Finding |
|---|---|---|
| **E3** | `plocate.cpp:371-376` | On a **full** scan it deliberately does *not* use io_uring. Comment: sequential is faster because the kernel coalesces and readaheads for you; it assumes it is CPU-bound and starts one worker per spare core, the last doing I/O only. |
| **E4** | `plocate.cpp:444-455` | 32 blocks per `pread` of one contiguous byte range, into a bounded queue (depth 256 ≈ 2 MB), N workers decompress and match. |
| — | `plocate.cpp:128` | io_uring is used only for genuinely random small I/O (a hash bucket, one docid block). |

Net model: **sequential I/O + block-level compression to amortise
decompression + CPU-parallel decompression**. Correct conclusion for our case
too, but only because their index lives on disk. Ours is resident, so the
mechanism (E4) does not apply — the philosophy does.

## 3. FSearch

### 3.1 Corrections to earlier assumptions

- **Not SQLite.** `grep -rn sqlite src/` returns 0 hits. Storage is bespoke:
  `fsearch_database_chunked_array.c` (in-memory chunked array) +
  `fsearch_database_file.c` (own persistence format, mmap-loaded) +
  `fsearch_database_index.c`.
- The `max_threads=1` IO pool (`fsearch_database.c:1331`) means the scan is
  **not** parallel either.

### 3.2 Incremental — adopted

Collection: `fsearch_folder_monitor_fanotify.c` (419 lines) → event queue →
`fsearch_database_index.c` (1175 lines) → `process_event()` dispatching to
create / delete / attrib / move-self / rescan.

| # | Source | Design |
|---|---|---|
| **B1** | `fsearch_folder_monitor_fanotify.c:275` | `fanotify_init(FAN_CLOEXEC \| FAN_NONBLOCK \| FAN_CLASS_NOTIF \| FAN_REPORT_DFID_NAME, O_RDONLY)`. `FAN_REPORT_DFID_NAME` is required to get a directory fid plus a name. |
| **B2** | `:369` | `fanotify_mark(fd, FAN_MARK_ADD \| FAN_MARK_ONLYDIR, MASK, AT_FDCWD, path)` — marks directories only. |
| **B3** | `:24` | the mask includes **`FAN_EVENT_ON_CHILD`**, so one mark on a top-level directory reports the entire subtree. This is the key trick: plain inotify needs one watch per directory and hits `max_user_watches`. Cost: `FAN_CLASS_NOTIF` needs root, else fall back. |
| **B8** | `fsearch_folder_monitor_*.c` | dual backend (`_FANOTIFY` / `_INOTIFY`) dispatched on `event->monitor_kind`, automatic degradation when permissions are insufficient |
| **B4** | `fsearch_database_index.c:132-160` | `get_skippable_events()`: if an event's path is a strict `/`-delimited descendant of a deleted directory, drop it. `rm -rf` of a large tree saves tens of thousands of no-op updates. The strict prefix check is what keeps `/ab` from matching `/abc`. |
| **B5** | `:581` | a create event that is a directory triggers a full recursive `db_scan_folder()` of that subtree and registers it with the monitor — a newly created directory is usually a bulk copy in progress, and per-event collection is unreliable there |
| **B6** | `:571-576` | before mutating, broadcast `ENTRY_DELETED + FLAG_SIZE` up the whole ancestor chain to pull those entries out of the size-ordered index; after inserting, broadcast `ENTRY_CREATED` so they re-enter at their new position. **Directory size and child counts are aggregates; every mutation must bubble them up the parent chain.** This is the single easiest thing to get wrong in a self-built index. |
| **B7** | `:199` | events are drained from a `GAsyncQueue` in batches (`process_queued_events()`), not handled one at a time |

### 3.3 Scanning — rejected

`fsearch_database_scan.c:141,158,185` uses `opendir`/`readdir` then
`fstatat(AT_SYMLINK_NOFOLLOW|AT_NO_AUTOMOUNT)` on **every** entry, ignoring
`d_type` entirely. `scan.c:165` still carries
`TODO: we can test for hidden here to avoid stat call`. One order of magnitude
slower than the plocate approach.

### 3.4 Storage layout — partially adopted

```c
// fsearch_database_entry.c:23-30
typedef struct FsearchDatabaseEntry {
    FsearchDatabaseEntry *parent;
    uint32_t attribute_flags;
    uint16_t flags;
    // Make sure the attributes member is aligned to its largest data type
    alignas(int64_t) uint8_t attributes[];
} FsearchDatabaseEntry;
```

Fixed 16-byte header; `attributes[]` is a flexible array holding this entry's
attributes, and the NUL-terminated name immediately after them. One `malloc`
per entry, no secondary indirection.

| # | Source | Finding |
|---|---|---|
| **C1/C2** | `entry_get_size_for_flags()` `:825-842` | which attributes exist is a bitset in `attribute_flags`; `db_entry_get_attribute_offsets()` precomputes an offset table and values are fetched by offset. Saves memory for unindexed attributes; costs an indirection and means **entries are not a uniform struct array and must not be cast as one**. |
| **C3** | `:812-822` | the precomputed offset table is unnecessary for a columnar layout |
| **C4** | `fsearch_database_chunked_array.c:16-22,39,100-119,142-146` | `TARGET_CHUNK_SIZE 2048`, split at 2×. Insertion cost drops from O(n) to O(2048). On bulk insert it first estimates `num_new_entries * target_chunk_size` to decide between per-item insertion and wholesale rebuild. |
| — | `build_path_recursively()` `:36-48` | **only a `parent` pointer, no children list.** Paths are rebuilt by walking up and appending. Consequences: memory-efficient, but a full path is O(depth) and allocates, so it is the hot spot when displaying results; directory aggregates must be maintained along the chain (B6); and **listing a directory's direct children requires a full scan**. |

That last point is disqualifying for us: browsing a directory fires
`parent:` on every navigation, and we need it to be O(1) (design §5.1, F1).

### 3.5 Query semantics — partially adopted

Matchers are a table of uniform-signature function pointers,
`(FsearchQueryNode *node, FsearchQueryMatchData *match_data) -> uint32_t`.
Every numeric attribute (`size`, `date_modified`, `depth`, `childcount`) shares
one comparator (`fsearch_query_matchers.c:45-61`):

```c
case FSEARCH_QUERY_NODE_COMPARISON_RANGE:
    return node->num_start <= num && num < node->num_end;
```

and every one of them is called **per entry**. There are three comments about
"size sorted indexes" in `fsearch_database_index.c` but no such structure
anywhere in the source — historical leftovers.

Sorting exists only to present results, not to filter:
`fsearch_database_search_view.c:216` sorts an already-filtered set, and the
`chain.properties[]` array in `fsearch_database_sort.c:38-61` is a multi-level
comparison chain (size → name → path), i.e. a comparator definition, not an
index.

| | name substring | structured filter (`size:`, `dm:`, …) |
|---|---|---|
| plocate | trigram inverted index, O(candidates) | unsupported |
| FSearch | in-memory array + `strstr`/regex, O(n) | **O(n) full scan** |

FSearch feels fast because everything is resident in memory, it is C, and there
is no disk I/O — not because of the algorithm. At desktop scale that is
sufficient; for our selection-driven queries it is not (design §5.3, F2).

## 4. Summary: what we take from where

| Area | Source | Substance |
|---|---|---|
| Full scan | plocate | `openat` + `O_NOATIME` (EPERM retry) + `d_type` + directory-mtime skip |
| Scan concurrency | neither | both are single-threaded; we shard top-level directories and work-steal, with concurrency adapted to the medium (D6) |
| Incremental | FSearch | fanotify with `FAN_EVENT_ON_CHILD`; new directory ⇒ recursive scan; ancestor-chain bubbling; batched event drain |
| Index structure | plocate | byte trigrams, 32 files/block, delta-1 + PForDelta 128, Robin Hood table, zstd dictionary, bounded-queue parallel decompression |
| Query I/O | plocate | sequential large reads; do not reach for io_uring on a full scan |
| Storage | neither | plocate has paths only (no size/mtime — `size:`/`dm:` need them); FSearch's format is too tightly coupled. Ours: columnar + string pool + mmap snapshot |
| `dir_id → children` | neither | self-built (F1) — FSearch's parent-only design is the reason it cannot do interactive browsing |
| Filtered attribute index | neither | self-built (F2) |
| Filter/optimizer | neither | self-built (F4) |

**Standing warning carried over from the survey:** plocate's trigram index
solves exactly one thing — substring matching on names. Structured filters
(`size:`, `dm:`, `attrib:`) are a separate mechanism. The two must be designed
orthogonally; no single inverted table covers both.