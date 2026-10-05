#ifndef ESIDX_H
#define ESIDX_H

/* esidx -- Everything-compatible search index for ext4. Public types & API.
 *
 * Design reference: docs/design.md
 *   storage : columnar (§4.1) + string pool (§4.2)
 *   dir tree: §5.1 (self-developed; FSearch/plocate both lack a children index)
 *   sorted  : §5.3 + decision D3 (sorted array + delta buffer, LSM-style)
 *   bitmap  : §5.4 + decision D7 (dense bitset for P0, CRoaring at P4)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "log.h"

/* ------------------------------------------------------------------ ids */

typedef uint32_t eid_t;
#define EID_NONE ((eid_t)0xFFFFFFFFu)

/* entry flags */
#define EF_DIR      0x0001u
#define EF_HIDDEN   0x0002u
#define EF_READONLY 0x0004u
/* Tombstone. A removed entry keeps its row and its id; only this bit and the
 * `live` set say it is gone. Ids are never reused -- see design §11 D8. */
#define EF_DEAD     0x0008u

/* ----------------------------------------------------------- string pool */

typedef struct {
    uint32_t off;
    uint32_t len;
} strref_t;

typedef struct {
    char   *buf;
    size_t  len, cap;
} strpool_t;

/* Append `n` bytes plus a NUL, and hand back where they landed. Returns 0 on
 * allocation failure, which is why the offset comes out through the pointer: a failed
 * append used to return 0, and 0 is also where the pool's first string is, so a caller
 * that stored it got the *first* name in the pool rather than an error. Four pools call
 * this, and one of them interns (store.c's name table) -- where an ambiguous 0 is not a
 * theoretical question. */
int      sp_intern(strpool_t *sp, const char *s, size_t n, uint32_t *off);
const char *sp_get(const strpool_t *sp, uint32_t off);

/* ------------------------------------------------- entry table (columnar) */

typedef struct {
    uint32_t  count, cap;

    eid_t    *parent;   /* parent eid; a dir's own eid doubles as its did */
    uint16_t *depth;
    uint16_t *flags;    /* EF_* */
    int64_t  *size;
    int64_t  *mtime;
    int64_t  *ctime;
    /* mtime in nanoseconds, and only for directories: the reconcile compares it
     * against the filesystem to decide whether a subtree can be skipped (ref A2).
     * Seconds would not do -- a directory can be created, emptied and refilled
     * inside one second, and the answer would silently keep the old rows. */
    int64_t  *stamp;
    uint16_t *ext_id;   /* interned into exts pool; 0 = none */
    strref_t *name;
    /* design §5.5's first aggregate column, and the first thing in this codebase
     * that is stored rather than derived. It is here because the children vectors
     * are indexed by directory ordinal: `child-count:` and `empty:` ask the question
     * once per candidate row, and a count read out of a vector would mean an ordinal
     * probe per row -- measured at 2.4 -> 6.4 ms of eval over 372 084 rows, which is
     * a leaf the query language calls index-backed getting slower. Maintained in the
     * two functions that maintain the vector, so the two cannot disagree. */
    uint32_t *nchild;
} entry_table_t;

typedef struct {
    uint64_t *w;
    uint32_t  nbits;
} bitset_t;

/* --------------------------------------------------------- directory tree */

/* A directory's children, as up to two contiguous runs: the flat array's range for it,
 * and -- for a directory that has gained a child since the last esidx_build_children()
 * -- the overlay's block for it. Returned by value because there is nothing to own: the
 * storage is `dir_index_t`'s, and this type exists only so that a caller can iterate
 * without knowing that.
 *
 * Two runs, not one, because a shared array has no room in the middle (design §5.1).
 * The alternative shape -- reserved slack at the end of each range, so an addition
 * appends in place -- keeps a single run and needs no overlay at all, and it does not
 * work: a directory created by a bulk copy is empty, so its slack is empty too, and
 * measured on /work a pass that added 2 000 files to one new directory blew through any
 * fixed slack and paid the full 487 ms rebuild anyway (design §10). The overlay absorbs
 * a burst of any size in one directory for the price of the ids themselves.
 *
 * So the rule is that no caller reads `.items`/`.n` alone: `di_children_n()` and
 * `di_child_at()` are the accessors, and a loop written against `.n` would compile and
 * silently skip every child added since the last rebuild -- which is the shape of bug
 * this file keeps having to record. */
typedef struct {
    const eid_t *items;
    uint32_t     n;
    const eid_t *more;      /* the overlay block: NULL when there is nothing pending */
    uint32_t     nmore;
} children_t;

/* One directory's pending children, in `dir_index_t`'s overlay. In the header because
 * the overlay is part of the shape a caller has to know about, not an implementation
 * detail of store.c. */
typedef struct {
    uint32_t ord;        /* directory ordinal; OV_EMPTY in an unused slot */
    uint32_t n, cap;
    eid_t   *ids;
} ovslot_t;
#define OV_EMPTY 0xFFFFFFFFu

typedef struct {
    /* The children of every directory, in ONE array, grouped by *directory ordinal* --
     * a compressed sparse row. Two things were wrong with the shape this replaced and
     * both were measured on /work:
     *
     *   - it was indexed by *entry id*, which spends a vector header on every id in the
     *     table when 12 % of them are directories: 8 388 608 headers of which 651 897
     *     are live, and the growth path memsets every new range, so all 128 MiB of it
     *     was resident. `di_ord()` is the reverse map, and it is a hash rather than a
     *     column because a column would be four bytes on every row -- 22 MiB on /work,
     *     and 22 MiB more in the snapshot -- to serve the 12 % that are directories. It
     *     is a hash rather than rank/select because a reconcile inserts directories one
     *     at a time and every rank structure that is not a Fenwick tree costs O(n) to
     *     repair.
     *   - each directory's vector grew by doubling, so the array was at 53 %
     *     occupancy: 10 324 800 slots for 5 476 484 ids. One array with exact extents is
     *     100 % by construction, and it is one allocation rather than 651 897 of them.
     *
     * The cost of the shape is that a directory's children cannot be appended to: a
     * shared array does not have room in the middle. So additions go to an *overlay* --
     * an append-only per-directory block, drained into this array by
     * esidx_build_children() when it grows past ESIDX_CHILD_OVERLAY_DIV -- and
     * di_children() hands back both runs so no reader can see one without the other.
     * The same bargain esidx_build_name_rank() makes, and the threshold is a fraction of
     * the entry count for the same reason: both rebuilds are O(n), so the question is
     * never *whether* to rebuild but how many additions to let accumulate first. */
    eid_t      *citems;       /* every directory's children, grouped by ordinal */
    uint32_t    cn, ccap;     /* cn == ccap after a build: the extents are exact */
    uint32_t   *cstart;       /* ordinal -> first index into citems */
    uint32_t   *ccount;       /* ordinal -> length; 0 for a directory with none */
    uint32_t    ord_cap;      /* slots in cstart/ccount */
    /* Children added since the last build, one block per directory, keyed by ordinal, so
     * that a directory's pending children are contiguous and di_children() can return
     * them as a second run. */
    ovslot_t   *ov_slot;
    uint32_t    ov_mask, ov_count, ov_ids;   /* table; live slots; ids held in them */
    bool        c_dirty;       /* the overlay is non-empty: drain before serving */

    /* eid -> ordinal, open addressing, value = ordinal + 1 so that 0 means empty.
     * The same convention as the other three open-addressed tables here. Keys are
     * dense ids, so the id masks well enough on its own. */
    uint32_t   *ord_slot;
    uint32_t    ord_mask;     /* capacity-1, capacity is power of two */
    uint32_t    ord_count;
    eid_t      *ord_eid;      /* ordinal -> eid, so a lookup can hand back an id */

    /* path -> eid open-addressing hash (Robin Hood variant, ref A12) */
    uint32_t   *ht_off;    /* strref off into the dpaths pool; 0 = empty */
    eid_t      *ht_val;
    uint32_t    ht_mask;   /* capacity-1, capacity is power of two */
    uint32_t    ht_count;
} dir_index_t;

int  bs_init(bitset_t *b, uint32_t nbits);
void bs_free(bitset_t *b);
/* Widen to at least `nbits`, keeping the bits already set. A new entry id can
 * land past the end of a bitmap that finalize sized for the old entry count,
 * and every set operation silently ignores an out-of-range index -- so an
 * incremental add that skipped this would lose the row from `file:`/`ext:`
 * without any error. */
int  bs_reserve(bitset_t *b, uint32_t nbits);
void bs_set(bitset_t *b, uint32_t i);
bool bs_test(const bitset_t *b, uint32_t i);
void bs_clear(bitset_t *b);
void bs_clear_bit(bitset_t *b, uint32_t i);
uint32_t bs_count(const bitset_t *b);
/* Set algebra. The language's `space` / `|` / `!` map straight onto these
 * (design §5.4), which is why they live in the index layer and not in a query
 * helper. */
void bs_and(bitset_t *dst, const bitset_t *src);
void bs_or(bitset_t *dst, const bitset_t *src);
void bs_copy(bitset_t *dst, const bitset_t *src);
void bs_andnot(bitset_t *dst, const bitset_t *src);
void bs_not(bitset_t *b);
void bs_set_all(bitset_t *b, uint32_t nbits);
/* first set bit at or after `i`, or b->nbits when there is none */
uint32_t bs_next(const bitset_t *b, uint32_t i);

/* ---------------------------------------------------------- sorted index */

/* The high bit of a delta entry's *id* marks it as a *retraction* of the value beside
 * it, not an assertion of one. It exists because the main array is append-and-sort: an
 * id whose size or mtime changed still has its old row there, and a range query that
 * covers the old value would report it. */
#define SIDX_DEL 0x80000000u

/* Two arrays, not an array of {int64 v; eid_t id}. That struct is 16 bytes of which 4
 * are padding, and the padding is 62.7 MiB on /work -- the ledger's third-largest row,
 * and pure waste: 5 476 485 rows of a value and an id need 12 bytes each, not 16.
 *
 * The sort is what makes this affordable rather than merely smaller. `id` starts out as
 * 0..n-1 and the values come from a column, so the array can be built by sorting the
 * *ids* against that column with qsort_r and gathering the values afterwards: 12 bytes
 * per row resident and nothing transient at all. Sorting an array of 16-byte structs and
 * splitting it afterwards would have needed 88 MiB of scratch at the moment the trigram
 * index is also at its largest, which is the peak this is trying to reduce.
 *
 * Nothing outside store.c may read these: the query layer asks for a range or for a
 * count, and sidx_lower_bound() is the one place that knows the order.
 *
 * An empty array is legal and means "not built" -- and every reader has to say so out
 * loud, because the alternative is the worst failure this codebase knows:
 * sidx_range_to_bitset() over an empty array returns *no rows*, which is
 * indistinguishable from a query that matched nothing. ESIDX_IX_* below is what turns
 * one off, and query.c's range_on() is where the fallback that keeps it honest lives. */
typedef struct {
    int64_t *v;           /* ascending */
    eid_t   *id;
    uint32_t n, cap;

    int64_t *dv;          /* the delta: unsorted, append-only (D3) */
    eid_t   *did;
    uint32_t dn, dcap;
} sidx_t;

/* ------------------------------------------------- which derived indexes exist
 *
 * D4: the derived indexes are not in the snapshot, so "do not build this one" costs
 * nothing to express and nothing to undo -- the next load rebuilds whatever this
 * process did not ask for. That is a property a persisted index cannot have, and it is
 * why these are a bitmask on the open index rather than a field in the file: the file
 * is data, this is a choice about one process.
 *
 * The mask is a *skip* list, so 0 is everything on and a new bit defaults to built. */
enum {
    ESIDX_IX_SIZE    = 1u << 0,   /* by_size    -- size: ranges, 62.7 MiB on /work */
    ESIDX_IX_MTIME   = 1u << 1,   /* by_mtime   -- dm: ranges */
    ESIDX_IX_CTIME   = 1u << 2,   /* by_ctime   -- dc: ranges */
    ESIDX_IX_TRIGRAM = 1u << 3,   /* name trigrams -- the text prefilter */
    ESIDX_IX_RANK    = 1u << 4    /* name rank  -- SORT_NAME as an int compare */
};

/* Parse a skip list like "size,mtime" or "all". Returns 0 for nothing skipped. The same
 * parser serves the flag, the sidecar and the environment, so a name that works in one
 * works in all three. */
uint32_t esidx_index_skip(const char *list);
/* The bits in `mask` as the comma-separated names, for a log line or an error. */
const char *esidx_index_names(uint32_t mask);

/* Resolve which derived indexes this process will not build, and say where the answer
 * came from in `*source` (a static string; never NULL).
 *
 * Precedence is **flag > sidecar > environment > everything on**, and the order is the
 * argument rather than an accident: the flag is this invocation's decision and the
 * sidecar is the file's, so a stale sidecar must not silently override a flag someone
 * typed. `have_flag` with an empty list is therefore meaningful -- it is how a caller
 * says "ignore the sidecar and build everything" -- which is why it is a separate flag
 * rather than a NULL pointer. */
uint32_t esidx_index_resolve(const char *dbfile, int have_flag, const char *flag_list,
                             const char **source);
/* The path a sidecar for `dbfile` would have. */
void esidx_index_sidecar_path(const char *dbfile, char *out, size_t n);

void sidx_build(sidx_t *s, const int64_t *vals, uint32_t n);
void sidx_free(sidx_t *s);
/* first index with v >= lo -- the one binary search over the array's order */
uint32_t sidx_lower_bound(const sidx_t *s, int64_t lo);
/* rows whose value is in [lo,hi], main array and delta both */
uint32_t sidx_count_range(const sidx_t *s, int64_t lo, int64_t hi);
/* append [lo,hi] matching ids into bitset */
uint32_t sidx_range_to_bitset(const sidx_t *s, int64_t lo, int64_t hi, bitset_t *out);
/* D3's writer. update() asserts the new value; erase() retracts the old one.
 * Both are O(1) and both leave the main array alone until sidx_merge(). */
void sidx_update(sidx_t *s, int64_t v, eid_t id);
void sidx_erase(sidx_t *s, int64_t v, eid_t id);
/* Fold the delta into the main array (D3's 1% / 60 s rule). O(n + d log d):
 * every id the delta mentions is dropped from its old row and re-added from the
 * delta, so a merge never resurrects a retracted value. */
int  sidx_merge(sidx_t *s);

/* ------------------------------------------------------- name order (design §10) */

/* A dense rank over the *distinct* folded display names, so ordering by name is an
 * integer comparison instead of a string one.
 *
 * "Dense" is the load-bearing word and it is about the tie-break, not about space:
 * two rows with the same name must get the *same* rank, so that cmp_rec falls
 * through to the display-name tie-break and then to the id, exactly as it did when
 * the name itself was being compared. A rank assigned by position would order equal
 * names by insertion and silently change the result order, which is part of the
 * protocol contract (design §1.2 -- 22 sort names, and a client pages by OFFSET).
 *
 * The strings live in their own pool, folded once, because folding at query time
 * costs as much as the comparison it saves (measured: ~20 ms per sort over 372 084
 * rows, against ~26 ns saved per comparison).
 *
 * `folded` is kept even though only the rank is read on the sort path, because it is
 * what makes the rank reproducible and because §5.2's `startwith:`/`whole:`/`ww:`
 * want the same folded order. */

/* -------------------------------------------------------- name trigram index */

/* design §5.2, the name half of it: byte trigrams over the *display name*, used
 * only to narrow a candidate set before the matcher runs.
 *
 * It is a filter and never a decision. Every trigram of a pattern's longest
 * literal run has to occur in the name for the pattern to match at all, so the
 * intersection can only drop rows text_match() would have rejected anyway -- which
 * is what makes it safe to switch on for one query shape and leave off for
 * another (see query.c's tri_applies).
 *
 * Byte trigrams, not codepoint trigrams (ref A13): it is the only rule that keeps
 * a CJK filename from producing thousands of postings per name. Built on the
 * ASCII-folded name, because that is the only folding the matcher it feeds does
 * in the C locale.
 *
 * A posting list is ascending without being sorted: finalize walks ids in order
 * and esidx_add only ever hands out a larger one (design §11 D8), so the
 * intersection is a merge. Removals are *not* unpublished -- a dead id is stopped
 * by the `live` set every query seeds from, and esidx_compact() rebuilds the whole
 * thing, which is the same bargain ext_index_del() makes.
 *
 * The ids are delta-varint encoded rather than stored as an array of eid_t, which is
 * what `nb` is for: the list is a byte buffer, `n` counts postings and `nb` counts
 * the bytes they occupy. `last` is the id the next append differences against, so an
 * append is O(1) without decoding the buffer backwards. On /work that is 318.6 MiB of
 * ids in 2.83x fewer bytes, against the plain array this replaced -- see trigram.c,
 * which owns the encoding and is the only thing that reads it. */
typedef struct {
    uint8_t  *buf;
    uint32_t  n;      /* postings */
    uint32_t  nb;     /* bytes of buf in use */
    uint32_t  cap;    /* bytes allocated */
    uint32_t  last;   /* the id the next append differences against */
} tri_list_t;

typedef struct {
    tri_list_t *list;     /* slot -> posting list */
    uint32_t   *key;      /* slot -> the 24-bit trigram */
    uint32_t    n_slots, cap_slots;
    uint32_t   *tab;      /* open addressing, value = slot + 1, 0 = empty */
    uint32_t    tab_mask;
    uint32_t    tab_shift;/* hash shift, kept so the probe can take the high bits */
    uint32_t    n_postings;
} tri_index_t;

/* ------------------------------------------------------------- enum bitmap */

/* One bitmap per extension (design §5.4) -- except that a *narrow* extension is a posting
 * list, because the bitmap's cost is set by the table rather than by the extension and
 * the product does not scale: /work reserved 4 416 MB of address space for 6 765
 * extensions, of which 6 248 hold fewer than 4 096 rows. design §5.4 records the
 * measurements and the threshold.
 *
 * Which structure a slot has is `posts[s].ids`: non-NULL is a list, NULL is a bitmap.
 * One array rather than two, so the two kinds sit side by side in memory and a select
 * over a term naming several extensions touches one cache line per slot.
 *
 * `tab` maps ext_id to a dense slot. ext_id used to be a byte offset, so it was sparse;
 * it is dense now (see esidx_t), but the slot table stays rather than indexing by id
 * directly because the slots are renumbered by ext_index_build() and a term can name up
 * to 256 extensions at once, which is a set union either way. */
typedef struct {
    eid_t   *ids;
    uint32_t n, cap;
} ext_post_t;

typedef struct {
    uint32_t   n;        /* distinct extensions */
    uint32_t  *ids;      /* slot -> ext_id */
    uint32_t  *counts;   /* slot -> entries carrying it, for the selectivity
                          * estimate in design §6.2 -- computing it here is what
                          * lets the optimiser cost ext: without a scan */
    ext_post_t *posts;   /* slot -> posting list, or {NULL} when it has a bitmap */
    bitset_t  *sets;     /* slot -> entries carrying it, when it is broad enough */
    uint32_t  *tab;      /* open addressing, value = slot + 1, 0 = empty */
    uint32_t   tab_mask;
} ext_index_t;

/* file: and folder: as whole-table bitmaps (design §2, L1) */
typedef struct {
    bitset_t all, dirs, files;
} type_index_t;

/* --------------------------------------------------------------- database */

/* Scan counters, kept so build/query reporting can show what the collector
 * actually saw (getdents volume, stat success rate, depth reached).
 *
 * The four `_us` fields split the walk into the three syscalls it spends its time
 * in and the bookkeeping around them. They are accumulated only at LOG_INFO --
 * three clock reads per entry is a measurable fraction of a 10 us/entry walk, and
 * a counter nobody asked for should cost nothing (timer.h). The split is what
 * decides whether "batch stat by inode" (design §7, the note at the top of
 * scan.c) is worth building: if fstatat is 30% of the walk there is nothing on
 * the table, and if it is 70% there is. */
typedef struct {
    uint64_t dirs;            /* directories descended into */
    uint64_t entries;         /* entries appended (files + dirs) */
    uint64_t files;
    uint64_t dirs_found;
    uint64_t stat_ok;
    uint64_t stat_fail;
    uint64_t open_fail;       /* openat(O_DIRECTORY) failures -> subtree skipped */
    uint64_t getdents_calls;
    uint64_t getdents_bytes;
    uint64_t depth_max;
    uint64_t getdents_us;     /* inside SYS_getdents64 */
    uint64_t stat_us;         /* inside fstatat */
    uint64_t open_us;         /* inside openat(O_DIRECTORY); close() is in "other" */
    uint64_t add_us;          /* inside esidx_add: pool intern + the columns */
} scan_stats_t;

/* What one esidx_update() actually did. The counters a caller has to be able to
 * report, because "the index is up to date" is exactly the claim that is wrong
 * when it is wrong: a pass that skipped every directory it looked at has proved
 * nothing about the files inside them (design §7). */
typedef struct {
    uint32_t dirs_skipped;   /* stamp compared, unchanged, subtree not descended */
    uint32_t dirs_reconciled;/* descended into: its children were listed */
    uint32_t entries_seen;
    uint32_t added;
    uint32_t removed;
    uint32_t refreshed;      /* entries whose columns were compared */
    uint32_t stat_fail;      /* entries or directories that could not be read */
    uint32_t compacted;
    uint64_t us;
} update_stats_t;

typedef struct {
    strpool_t     names;
    /* The intern table for `names`: open addressing on the name, slot value = pool
     * offset + 1, 0 = empty. It is what keeps the pool at one copy per *distinct*
     * basename rather than one per entry -- 1 499 994 against 5 476 485 on /work, which
     * is 96 MiB of pool where 26 will do. The same shape as the ext table two fields
     * down, and derived like it (D4): a load rebuilds it from the offsets the entries
     * carry, and nothing in a snapshot depends on it.
     *
     * Built by esidx_add() as names arrive, and once on the first add after a load --
     * lazily, so a server that is only served from never pays for the pass. */
    uint32_t     *nm_tab;
    uint32_t      nm_mask;
    uint32_t      nm_count;    /* occupied slots = distinct names interned */
    strpool_t     exts;
    /* The dir hash's keys: one full path per directory, in interning order, and
     * deliberately NOT in `names`. A hash slot has to hold something stable to
     * compare a lookup against, and a path rebuilt from the parent chain is not
     * stable -- but a pool offset into the persisted pool is *too* stable: loading
     * a snapshot re-interned every path and an `esidx update` wrote them back, so
     * the file grew by 92 bytes per pass on a 7-entry fixture and 56.6 MiB per
     * pass on /work, without bound. This is derived data (D4 says derived indexes
     * are rebuilt, not stored), so it gets a pool of its own that no snapshot
     * mentions. Same 56.6 MiB resident -- a hash of the path instead of the path
     * would cost a rebuild per lookup and this is not that commit. */
    strpool_t     dpaths;
    /* An entry names its extension by *dense id*, not by a position in `exts`. It used
     * to be the byte offset, returned as uint16_t, and a pool past 64 KB wrapped one
     * extension's offset onto another's: two extensions shared an id and `ext:` answered
     * with rows carrying neither. Measured on /work (a 68 KB pool) as ~30 extensions, and
     * on a 2 200-extension fixture as 152. The offset lives here instead, in 32 bits,
     * where it cannot wrap; a dense id cannot realistically reach 65 535 distinct
     * extensions, so the entry column stays 16 bits wide and the snapshot's ext column
     * does not move -- only what an id *means* changed, hence ESIDX_VERSION 3.
     *
     * Rebuilt on load by walking `exts` in order, which is interning order, so the ids a
     * snapshot's entries carry are reproduced exactly. Nothing derived from the ids is
     * persisted (D4). */
    uint32_t     *ext_off;     /* ext_id (1-based) -> byte offset into `exts` */
    uint32_t      n_ext;       /* distinct extensions interned so far */
    uint32_t      ext_off_cap;
    /* The interning table for those names: open addressing on the name, slot value =
     * ext id + 1, 0 = empty. It is what makes ext_intern() O(1); the scan it
     * replaced was linear in the number of extensions interned *so far*, so a build
     * paid a price quadratic in its own extension count: 4 217 609 interns over
     * 6 765 names on /work, 2 189 959 017 string compares, now 4 895 892.
     *
     * Derived, so no snapshot mentions it (D4): esidx_load() refills it from `ext_off`,
     * which is interning order, which is the order the pool is walked in -- so an id
     * means the same string before and after a reload, and the ids a snapshot's entries
     * carry land on the names they were built against. */
    uint32_t     *ext_tab;
    uint32_t      ext_tab_mask;
    entry_table_t et;
    dir_index_t   di;
    sidx_t        by_size;
    sidx_t        by_mtime;
    sidx_t        by_ctime;
    ext_index_t   ext;      /* ext: bitmap set, built in finalize */
    type_index_t  type;     /* file:/folder: bitmaps, built in finalize */
    tri_index_t   tri;      /* name trigrams, built in finalize (design §5.2) */
    /* Folded display names and a dense rank over the distinct ones, built in
     * finalize: a name sort is an integer compare rather than a string one. */
    strpool_t     folded;
    uint32_t     *name_rank;  /* eid -> rank of its display name */
    uint32_t      n_ranks;    /* distinct folded names */
    uint32_t     *rk_off;     /* rank -> offset into `folded` */
    uint32_t     *rk_tab;     /* open addressing, value = rank + 1, 0 = empty */
    uint32_t      rk_mask;
    /* One bit per entry id: clear for a tombstone (EF_DEAD), set for every live
     * row. Every query seeds its candidate set from here, which is what keeps a
     * removed entry out of every matcher without each matcher having to know
     * that removals exist. */
    bitset_t      live;
    /* Bumped by every mutation, and read in exactly one place: the protocol layer's
     * `cache_matches()` (design §6.4), so a refresh between two identical QUERYs
     * cannot serve the previous id set -- which after a compaction would be a set of
     * unrelated rows rather than a stale subset. That comparison did not exist until
     * `esidx serve --refresh` made it reachable: before it, serve never called
     * esidx_update(), so nothing could go stale and nothing read this field outside
     * store.c. It also decides whether a snapshot needs writing at all, since a pass
     * that changed nothing must not rewrite the file (etp.c, serve_save). */
    uint64_t      epoch;
    /* When the last mutation happened, for D3's "merge once the delta has been
     * idle" rule. Monotonic microseconds; 0 means the index has never been
     * touched since it was built, i.e. there is nothing to merge. */
    uint64_t      last_change_us;
    /* finalize has run, so the derived indexes exist and esidx_add() has to keep
     * them in step. A full scan appends with no derived indexes present and lets
     * finalize build them; a reconcile appends into a live index. */
    bool          built;
    /* Which derived indexes esidx_finalize() must NOT build: the ESIDX_IX_* mask, from
     * esidx_index_resolve(). Every *reader* of a skipped structure has to fall back to
     * something slower and correct -- that is the whole contract, and `built` being
     * true says nothing about it, which is why the mask is checked where it is used
     * rather than being folded into it. */
    uint32_t      skip;
    const char   *skip_src;  /* which flag/sidecar/env decided it, for the log and for
                              * `esidx options`; a static string, never NULL after
                              * esidx_index_apply() */
    eid_t         root_eid; /* the scanned root; parent:"" and root: anchor here */
    scan_stats_t  scan;
} esidx_t;

void esidx_init(esidx_t *db);
void esidx_free(esidx_t *db);

/* Record the resolved mask on an open index, and print it at INFO. One line, at startup,
 * naming the source: a configuration that is believed to be off while it is on is the
 * whole failure this exists to prevent. */
void esidx_index_apply(esidx_t *db, uint32_t mask, const char *source);

/* One entry as the collector hands it over. A struct rather than nine positional
 * arguments because three of them are adjacent int64_t seconds/nanoseconds: a
 * swapped pair compiles, runs, and quietly makes every reconcile rescan. */
typedef struct {
    const char *name;
    uint16_t    flags;
    uint16_t    depth;
    int64_t     size;
    int64_t     mtime;
    int64_t     ctime;
    int64_t     stamp;     /* mtime in ns; only read for directories */
} entry_in_t;

/* append one entry; returns its eid. When db->built is set (a reconcile) the
 * derived indexes are updated for the new id, otherwise finalize builds them. */
eid_t esidx_add(esidx_t *db, eid_t parent, const entry_in_t *in);

/* refresh an existing entry's columns in place. Only the columns that actually
 * changed reach a sorted array, so a deep pass over a quiet tree writes nothing
 * to any delta. Not for a type change: flags are baked into the type and ext
 * bitmaps, so a file that became a directory has to be removed and re-added. */
int esidx_touch(esidx_t *db, eid_t id, const entry_in_t *in);

/* remove an entry and everything under it. Tombstones, not reuse -- see §11 D8. */
int esidx_remove(esidx_t *db, eid_t id);

uint32_t esidx_live_count(const esidx_t *db);

/* scan */
int  esidx_scan(esidx_t *db, const char *root);
/* Bring a built index back in line with the filesystem (design §7). */
#define EU_DEEP 0x1u   /* stat every entry, not just directories whose stamp moved */
/* Do not compact, even past the tombstone threshold below. A batch caller wants the
 * compact -- it is about to exit and a small snapshot is what it leaves behind. A
 * server does not: esidx_compact() is a full rescan plus a finalize, so inside a
 * single-threaded serve loop it is a multi-second stall in which no client is answered
 * at all, and it replaces the whole esidx_t, so every cached result set is a set of
 * unrelated ids. The offline `esidx update` still compacts, so the id space is
 * reclaimed on the next cron pass rather than never. */
#define EU_NOCOMPACT 0x2u
int  esidx_update(esidx_t *db, const char *root, unsigned flags, update_stats_t *st);
/* Rebuild from scratch to reclaim tombstoned ids. Automatic once they dominate,
 * because a reconcile that rewrites a hot directory costs one id per child. */
int  esidx_compact(esidx_t *db);
void esidx_finalize(esidx_t *db);   /* build dir paths hash + sorted indexes */

/* child count of a directory entry (design §5.5 aggregate, derived not stored) */
uint32_t di_child_count(const esidx_t *db, eid_t dir);
/* The children of a directory, as a view into the one flat array *and* the overlay;
 * `{NULL, 0, NULL, 0}` for an id that is not an indexed directory, or for one with no
 * children. Every reader goes through this so the two lookups live in one place, and
 * every iteration goes through di_children_n()/di_child_at() so that neither run can be
 * read without the other.
 *
 * The view is const, which is the point of it: a caller cannot shrink a range in place,
 * because the array is shared and a range's neighbours do not move. esidx_remove() walks
 * a subtree through di_remove_child() instead, which is the only thing that may shrink
 * one, and it says so. */
children_t di_children(const esidx_t *db, eid_t dir);
/* How many children that view holds, both runs. `nchild` says the same thing and is what
 * `child-count:` reads (design §5.5 measured the column against the vector), so
 * di_children_n() == di_child_count() is an invariant -- and the two are maintained by
 * the same two functions, which is the only reason it holds. */
static inline uint32_t di_children_n(children_t c) { return c.n + c.nmore; }
static inline eid_t     di_child_at(children_t c, uint32_t i)
{
    return i < c.n ? c.items[i] : c.more[i - c.n];
}
/* Rebuild the flat array from the columns: exact extents, one allocation, overlay
 * dropped. Called from esidx_finalize() (which covers a fresh build and a snapshot load)
 * and from esidx_drain() when the overlay has grown past its threshold. */
int  esidx_build_children(esidx_t *db);
/* How many additions may accumulate before that rebuild, as a fraction of the entry
 * count: a twelfth. The rebuild is O(n) and measured at 487 ms over 5.5 M entries, and
 * the overlay costs 4 bytes an id, so letting a twelfth of the index accumulate turns
 * 487 ms per pass into 487 ms per 456 000 additions -- while bounding the overlay at
 * 2.7 MiB there and at 1/12 of the CSR's own size everywhere. On a small index the
 * threshold is small too, which is why a 1 622-entry fixture still exercises both the
 * overlay path and the drain in one pass. */
#define ESIDX_CHILD_OVERLAY_DIV 12u
/* Drain whichever overlays are over their threshold. Called at the end of a reconcile
 * (scan.c), and the one place a future event-driven path will call after applying a
 * batch -- so "when do the O(n) rebuilds happen" has one answer. */
int  esidx_drain(esidx_t *db);
/* Is this directory empty? A bit, because `empty:` asks it per candidate row. */
bool di_is_empty(const esidx_t *db, eid_t dir);

/* snapshot (decision D4: mmap load, plain write) */
int  esidx_save(const esidx_t *db, const char *path);
int  esidx_load(esidx_t *db, const char *path);

/* ------------------------------------------------------------- ext bitmaps */

int      ext_index_build(ext_index_t *xi, const esidx_t *db);
void     ext_index_free(ext_index_t *xi);
/* OR every listed extension into `out`. Unknown extensions contribute nothing. */
uint32_t ext_index_select(const ext_index_t *xi, const uint16_t *ids, uint32_t n,
                          bitset_t *out);
/* selectivity estimate for one ext id, or UINT32_MAX when unknown */
uint32_t ext_index_count(const ext_index_t *xi, uint16_t ext_id);
/* incremental maintenance (design §7) */
int      ext_index_add(ext_index_t *xi, const esidx_t *db, uint16_t ext_id, eid_t id);
int      ext_index_del(ext_index_t *xi, uint16_t ext_id, eid_t id);

/* ------------------------------------------------------------ name order */

/* Fold every live display name and rank it by sorted position. Called from
 * esidx_finalize(), so a snapshot load and a compaction both get it -- and from
 * esidx_update(), once per pass that actually added a name, because a rank is a
 * sorted position and a name the index has never seen has nowhere to go until the
 * order is recomputed. A pass that added nothing does not rebuild, so the idle cost
 * design §7 reports is unchanged. */
int      esidx_build_name_rank(esidx_t *db);
void     esidx_free_name_rank(esidx_t *db);
/* rank of an entry's display name; 0 when the column has not been built */
uint32_t esidx_name_rank(const esidx_t *db, eid_t id);

/* -------------------------------------------------------- name trigram index */

/* Index every live display name. Called from esidx_finalize(), so a snapshot load
 * and a compaction both get it without persisting anything (decision D4: derived
 * indexes are rebuilt, not stored -- the same bargain the ext bitmaps make). */
int  tri_index_build(tri_index_t *ti, const esidx_t *db);
void tri_index_free(tri_index_t *ti);
/* Index one name under one id. Called from link_new() for a reconcile, so the
 * posting lists stay ascending across a live index too. */
int  tri_index_add(tri_index_t *ti, const char *name, eid_t id);
/* Narrow `out` in place to the ids that can possibly contain `lit` (at least 3
 * bytes; folded here, so the caller passes the pattern's literal as written).
 * Returns false when nothing could be narrowed -- too short, no index built, an
 * allocation failure -- and the caller scans as before. Returns true when it did
 * narrow, *including* to the empty set, which is a real answer: no name in the
 * index holds that trigram. */
bool tri_index_filter(const tri_index_t *ti, const char *lit, bitset_t *out);

/* ---------------------------------------------------------------- helpers */

/* exposed to the query and protocol layers */
eid_t      di_lookup(const esidx_t *db, const char *path);
eid_t      di_lookup_name(const esidx_t *db, eid_t dir, const char *name);
int        di_remove_child(esidx_t *db, eid_t dir, eid_t child);
/* Insert/erase a directory's path. Erasing marks the slot rather than backing
 * the probe chain up, because a linear-probe table with a hole in it cannot
 * distinguish "absent" from "present, further along". */
int        di_hash_insert(esidx_t *db, eid_t dir);
int        di_hash_erase(esidx_t *db, eid_t dir);
void       path_of(const esidx_t *db, eid_t id, char *out, size_t outsz);
uint16_t   ext_intern(esidx_t *db, const char *name);
const char *name_of(const esidx_t *db, eid_t id);
/* the name as the reference reports it: basename(name_of()). Differs for a
 * parentless entry, whose stored name is the path it was indexed from. store.c */
const char *display_name_of(const esidx_t *db, eid_t id);
/* The extension of an entry, or "" when it has none. Reads through the id table rather
 * than the pool, because an id is dense and an offset is 32-bit (see esidx_t). */
const char *ext_of_str(const esidx_t *db, eid_t id);
const char *ext_str(const esidx_t *db, uint16_t ext_id);

/* Longest extension this codebase will hold, NUL included: NAME_MAX is 255 on ext4 and
 * an extension is a suffix of a name, so nothing real reaches it. One constant because
 * the writer and the reader of a query's extension list have to agree on where a name
 * ends -- they kept 31 and 63 respectively, so `ext:` could not match what the index
 * held (store.c's ext_of, query.c's ext_list_ids). */
#define EXT_NAME_MAX 256
/* full path of the directory containing `id` -- dirname(path_of()), so it is defined
 * for the root too (the directory above it), and empty only when the path has no
 * separator in it at all */
void       parent_path_of(const esidx_t *db, eid_t id, char *out, size_t outsz);
/* Win32 attribute bits, as the ETP ATTRIBUTES column reports them */
uint32_t   esidx_win_attributes(const esidx_t *db, eid_t id);

/* scan counters (zero after esidx_init, filled by esidx_scan) */
const scan_stats_t *esidx_scan_stats(const esidx_t *db);
void esidx_log_stats(const esidx_t *db, const char *phase);
/* Per-structure memory ledger, allocated-vs-used. Called by esidx_log_stats, so
 * every phase that reports the index reports its footprint with it. */
void esidx_log_mem(const esidx_t *db, const char *phase);

/* ----------------------------------------------------------------- query */

/* The query surface lives in syntax.h: the AST, the parser, the ETP match
 * options and the executor. Kept out of here so that esidx.h stays the storage
 * contract and syntax.h is the layer above it. */
#include "syntax.h"

#endif /* ESIDX_H */
