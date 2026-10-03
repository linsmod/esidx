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
#define EF_DIR     0x0001u
#define EF_HIDDEN  0x0002u

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

/* snapshot (decision D4: mmap load, plain write) */
int  esidx_save(const esidx_t *db, const char *path);
int  esidx_load(esidx_t *db, const char *path);

/* ----------------------------------------------------------------- query */

/* sort keys, aligned with ETP sort names (etp_server.c:472-494) */
typedef enum {
    SORT_NAME = 0,
    SORT_PATH,
    SORT_SIZE,
    SORT_MTIME,
    SORT_EXT
} sort_key_t;

typedef struct {
    int      parent;        /* eid of parent dir, or -1 for whole tree */
    int      type_filter;   /* 0 any, 1 dir only, 2 file only */
    const char *name_substr;/* P0: in-memory substring scan; trigram at P4 (§5.2) */
    int64_t  size_lo, size_hi;
    int64_t  mtime_lo, mtime_hi;
    uint16_t *ext_ids;      /* accepted ext ids; NULL/next==0 = any */
    uint32_t next;

    sort_key_t sort_key;
    int        sort_desc;
    uint32_t   offset, count;   /* count==0 -> all */
} query_t;

void query_init(query_t *q);
/* returns number of results; *out is malloc'd array of eid */
uint32_t query_exec(const esidx_t *db, const query_t *q, eid_t **out);

/* helpers exposed for main.c / future protocol layer */
eid_t di_lookup(const esidx_t *db, const char *path);
void  path_of(const esidx_t *db, eid_t id, char *out, size_t outsz);
uint16_t ext_intern(esidx_t *db, const char *name);

/* scan counters (zero after esidx_init, filled by esidx_scan) */
const scan_stats_t *esidx_scan_stats(const esidx_t *db);
void esidx_log_stats(const esidx_t *db, const char *phase);

#endif /* ESIDX_H */
