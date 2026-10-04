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

uint32_t   sp_intern(strpool_t *sp, const char *s, size_t n);
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
} entry_table_t;

/* --------------------------------------------------------- directory tree */

typedef struct {
    eid_t    *items;
    uint32_t  n, cap;
} childvec_t;

typedef struct {
    childvec_t *child;     /* indexed by dir eid */
    uint32_t    child_cap;

    /* path -> eid open-addressing hash (Robin Hood variant, ref A12) */
    uint32_t   *ht_off;    /* strref off into names pool; 0 = empty */
    eid_t      *ht_val;
    uint32_t    ht_mask;   /* capacity-1, capacity is power of two */
    uint32_t    ht_count;
} dir_index_t;

/* ---------------------------------------------------------------- bitmap */

typedef struct {
    uint64_t *w;
    uint32_t  nbits;
} bitset_t;

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

typedef struct {
    int64_t v;
    eid_t   id;
} sidx_ent_t;

/* The high bit of `sidx_ent_t.id` marks a delta entry as a *retraction* of the
 * value in `v`, not an assertion of one. It exists because the main array is
 * append-and-sort: an id whose size or mtime changed still has its old row
 * there, and a range query that covers the old value would report it. */
#define SIDX_DEL 0x80000000u

/* Main sorted array, ascending by v. Incremental changes go to `delta`
 * (unsorted) and are merged periodically -- decision D3. */
typedef struct {
    sidx_ent_t *a;
    uint32_t    n, cap;

    sidx_ent_t *delta;
    uint32_t    dn, dcap;
} sidx_t;

void sidx_build(sidx_t *s, const int64_t *vals, uint32_t n);
void sidx_free(sidx_t *s);
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

/* ------------------------------------------------------- name trigram index */

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
 * thing, which is the same bargain ext_index_del() makes. */
typedef struct {
    eid_t   *ids;
    uint32_t n, cap;
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

/* One bitmap per extension (design §5.4). Extensions are few and low
 * cardinality, so a dense set per interned ext is the whole index -- there is
 * no need for a compressed structure until the ext count explodes.
 *
 * ext_id is the byte offset of the extension inside the exts pool plus one, so
 * it is sparse and cannot index an array directly; `tab` maps ext_id to a
 * dense slot. */
typedef struct {
    uint32_t   n;        /* distinct extensions */
    uint32_t  *ids;      /* slot -> ext_id */
    uint32_t  *counts;   /* slot -> entries carrying it, for the selectivity
                          * estimate in design §6.2 -- computing it here is what
                          * lets the optimiser cost ext: without a scan */
    bitset_t  *sets;     /* slot -> entries carrying it */
    uint32_t  *tab;      /* open addressing, value = slot + 1, 0 = empty */
    uint32_t   tab_mask;
} ext_index_t;

/* file: and folder: as whole-table bitmaps (design §2, L1) */
typedef struct {
    bitset_t all, dirs, files;
} type_index_t;

/* --------------------------------------------------------------- database */

/* Scan counters, kept so build/query reporting can show what the collector
 * actually saw (getdents volume, stat success rate, depth reached). */
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
    strpool_t     exts;
    entry_table_t et;
    dir_index_t   di;
    sidx_t        by_size;
    sidx_t        by_mtime;
    sidx_t        by_ctime;
    ext_index_t   ext;      /* ext: bitmap set, built in finalize */
    type_index_t  type;     /* file:/folder: bitmaps, built in finalize */
    tri_index_t   tri;      /* name trigrams, built in finalize (design §5.2) */
    /* One bit per entry id: clear for a tombstone (EF_DEAD), set for every live
     * row. Every query seeds its candidate set from here, which is what keeps a
     * removed entry out of every matcher without each matcher having to know
     * that removals exist. */
    bitset_t      live;
    /* Bumped by every mutation. The protocol layer's result cache compares it
     * (design §6.4), so a refresh between two identical QUERYs cannot serve the
     * previous id set -- which after a compaction would be a set of unrelated
     * rows rather than a stale subset. */
    uint64_t      epoch;
    /* When the last mutation happened, for D3's "merge once the delta has been
     * idle" rule. Monotonic microseconds; 0 means the index has never been
     * touched since it was built, i.e. there is nothing to merge. */
    uint64_t      last_change_us;
    /* finalize has run, so the derived indexes exist and esidx_add() has to keep
     * them in step. A full scan appends with no derived indexes present and lets
     * finalize build them; a reconcile appends into a live index. */
    bool          built;
    eid_t         root_eid; /* the scanned root; parent:"" and root: anchor here */
    scan_stats_t  scan;
} esidx_t;

void esidx_init(esidx_t *db);
void esidx_free(esidx_t *db);

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
int  esidx_update(esidx_t *db, const char *root, unsigned flags, update_stats_t *st);
/* Rebuild from scratch to reclaim tombstoned ids. Automatic once they dominate,
 * because a reconcile that rewrites a hot directory costs one id per child. */
int  esidx_compact(esidx_t *db);
void esidx_finalize(esidx_t *db);   /* build dir paths hash + sorted indexes */

/* child count of a directory entry (design §5.5 aggregate, derived not stored) */
uint32_t di_child_count(const esidx_t *db, eid_t dir);

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
int        di_remove_child(dir_index_t *di, eid_t dir, eid_t child);
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
const char *ext_of_str(const esidx_t *db, eid_t id);
/* full path of the directory containing `id` -- dirname(path_of()), so it is defined
 * for the root too (the directory above it), and empty only when the path has no
 * separator in it at all */
void       parent_path_of(const esidx_t *db, eid_t id, char *out, size_t outsz);
/* Win32 attribute bits, as the ETP ATTRIBUTES column reports them */
uint32_t   esidx_win_attributes(const esidx_t *db, eid_t id);

/* scan counters (zero after esidx_init, filled by esidx_scan) */
const scan_stats_t *esidx_scan_stats(const esidx_t *db);
void esidx_log_stats(const esidx_t *db, const char *phase);

/* ----------------------------------------------------------------- query */

/* The query surface lives in syntax.h: the AST, the parser, the ETP match
 * options and the executor. Kept out of here so that esidx.h stays the storage
 * contract and syntax.h is the layer above it. */
#include "syntax.h"

#endif /* ESIDX_H */
