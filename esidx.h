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
    eid_t         root_eid; /* the scanned root; parent:"" and root: anchor here */
    scan_stats_t  scan;
} esidx_t;

void esidx_init(esidx_t *db);
void esidx_free(esidx_t *db);

/* append one entry; returns its eid */
eid_t esidx_add(esidx_t *db, eid_t parent, const char *name, uint16_t depth,
                uint16_t flags, int64_t size, int64_t mtime, int64_t ctime);

/* scan */
int  esidx_scan(esidx_t *db, const char *root);
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

/* ---------------------------------------------------------------- helpers */

/* exposed to the query and protocol layers */
eid_t      di_lookup(const esidx_t *db, const char *path);
void       path_of(const esidx_t *db, eid_t id, char *out, size_t outsz);
uint16_t   ext_intern(esidx_t *db, const char *name);
const char *name_of(const esidx_t *db, eid_t id);
const char *ext_of_str(const esidx_t *db, eid_t id);
/* parent directory path of `id`; empty string for the root itself */
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
