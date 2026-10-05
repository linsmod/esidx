/* Storage layer: string pool, columnar entry table, directory tree, snapshot.
 * Refs: design §4.1/§4.2/§5.1, decisions D4/D5.
 */

#include "esidx.h"
#include "timer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------ string pool */

int sp_intern(strpool_t *sp, const char *s, size_t n, uint32_t *off)
{
    if (sp->len + n + 1 > sp->cap) {
        size_t ncap = sp->cap ? sp->cap : 65536;
        while (sp->len + n + 1 > ncap) ncap *= 2;
        char *nb = realloc(sp->buf, ncap);
        if (!nb) return -1;
        sp->buf = nb;
        sp->cap = ncap;
    }
    uint32_t at = (uint32_t)sp->len;
    memcpy(sp->buf + at, s, n);
    sp->buf[at + n] = '\0';
    sp->len += n + 1;
    *off = at;
    return 0;
}

const char *sp_get(const strpool_t *sp, uint32_t off)
{
    return sp->buf + off;
}

/* FNV-1a over the bytes of a NUL-terminated string. One hash function for every table
 * in this file, because three of them are keyed on a name and a reader comparing them
 * needs to know they are keyed the same way. */
static uint32_t hash_bytes(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

/* ------------------------------------------------- the names intern table
 *
 * `names` used to be append-only, so a tree with many directories holding the same
 * basenames stored one copy per *entry*: 1 499 994 distinct basenames over 5 476 485
 * entries on /work, and 96 MiB of pool where the distinct names need about 26. Interning
 * is the textbook answer and it is the same shape ext_intern() already has -- open
 * addressing on the string, slot value = pool offset + 1, 0 = empty, FNV over the
 * bytes. This is the fifth place that convention is written down and the second table
 * keyed on the name itself; rk_intern() above is the third and hashes content for a
 * different reason.
 *
 * Derived, so no snapshot mentions it (D4): a load rebuilds it from the offsets the
 * entries already carry, which is one pass and no pool writes. The *pool* is persisted
 * as it stands, deduped -- so a snapshot is smaller, and a v3 snapshot that predates this
 * (a pool with duplicates in it) loads unchanged and simply keeps the duplicates, because
 * two entries pointing at two copies of one name are two keys. Correctness does not
 * depend on the pool being clean; only the saving does.
 */
#define NM_TAB_MIN 1024

/* Fill the table from the entries -- the post-load path, where no table exists yet.
 *
 * One slot per *distinct* name, and the strcmp that costs is what makes it one: the
 * entries share names by design (that is the whole point of the pool), so a slot per
 * entry puts 5 476 485 keys into a table sized for 1 499 994 of them and the probe below
 * never finds a free slot. The first version of this function did exactly that and hung
 * the /work build at 100 % CPU; see nm_tab_grow() for why growth must not come through
 * here at all. Sized from the entry count, which is an upper bound on the distinct names
 * and therefore puts the load factor under 3/4 without knowing the real figure. */
static int nm_tab_build(esidx_t *db)
{
    uint32_t ncap = NM_TAB_MIN;
    while (ncap * 3 < db->et.count * 4) ncap *= 2;

    uint32_t *nt = calloc(ncap, sizeof(uint32_t));
    if (!nt) {
        LOGE("names: cannot allocate the intern table (%u slots)", ncap);
        return -1;
    }
    uint32_t mask = ncap - 1, count = 0;
    for (uint32_t i = 0; i < db->et.count; i++) {
        if (db->et.flags[i] & EF_DEAD) continue;    /* a tombstone owns no name */
        uint32_t off = db->et.name[i].off;
        const char *name = sp_get(&db->names, off);
        uint32_t h = hash_bytes(name) & mask;
        while (nt[h] && strcmp(sp_get(&db->names, nt[h] - 1), name) != 0)
            h = (h + 1) & mask;
        if (nt[h]) continue;                        /* already interned */
        nt[h] = off + 1;
        count++;
    }
    free(db->nm_tab);
    db->nm_tab = nt;
    db->nm_mask = mask;
    db->nm_count = count;
    return 0;
}

/* Double the table, rehashing the keys it already holds.
 *
 * The entries are deliberately not read: they are the wrong source here. Growth happens
 * once per 3/4 load, and re-inserting every entry on each of those passes is both O(n)
 * per doubling and -- because the entries outnumber the distinct names -- a table that
 * fills up. Keys in the table are unique by construction, so no strcmp is needed either:
 * a slot holds an offset and the string at that offset is the key. */
static int nm_tab_grow(esidx_t *db)
{
    uint32_t ncap = (db->nm_mask + 1) * 2;
    uint32_t *nt = calloc(ncap, sizeof(uint32_t));
    if (!nt) {
        LOGE("names: cannot grow the intern table to %u slots", ncap);
        return -1;
    }
    uint32_t mask = ncap - 1;
    for (uint32_t s = 0; s <= db->nm_mask; s++) {
        if (!db->nm_tab[s]) continue;
        uint32_t off = db->nm_tab[s] - 1;
        uint32_t h = hash_bytes(sp_get(&db->names, off)) & mask;
        while (nt[h]) h = (h + 1) & mask;
        nt[h] = off + 1;
    }
    free(db->nm_tab);
    db->nm_tab = nt;
    db->nm_mask = mask;
    return 0;
}

/* The pool offset of `name`, appending it only if it is not there yet.
 *
 * Returns offset + 1, and 0 on failure, which is the slot convention again: offset 0 is
 * the first name every index ever interns, so it cannot double as the error. */
static uint32_t name_intern(esidx_t *db, const char *name, uint32_t n)
{
    /* A load leaves the table empty, and building it is a pass over the entries. Doing
     * that lazily rather than in the load path means a server that is only served from
     * never pays for it -- the same bargain esidx_build_name_rank() makes. */
    if (!db->nm_tab && nm_tab_build(db) != 0) return 0;
    /* 3/4 load, so the table cannot fill before the next doubling. */
    if ((db->nm_count + 1) * 4 > (db->nm_mask + 1) * 3 && nm_tab_grow(db) != 0)
        return 0;

    uint32_t h = hash_bytes(name) & db->nm_mask;
    while (db->nm_tab[h]) {
        uint32_t off = db->nm_tab[h] - 1;
        if (strcmp(sp_get(&db->names, off), name) == 0) return db->nm_tab[h];
        h = (h + 1) & db->nm_mask;
    }

    uint32_t off;
    if (sp_intern(&db->names, name, n, &off) != 0) {
        LOGE("names: cannot append '%s' to the pool", name);
        return 0;
    }
    db->nm_tab[h] = off + 1;
    db->nm_count++;
    return off + 1;
}

static void nm_tab_free(esidx_t *db)
{
    free(db->nm_tab);
    db->nm_tab = NULL;
    db->nm_mask = 0;
    db->nm_count = 0;
}

/* ------------------------------------------------------- name order (design §10) */

/* Fold a name into `buf`. NAME_MAX is 255 on ext4, so this never truncates a real
 * filename; a longer one is still folded rather than copied, because a truncated
 * name would give two different ranks to one string. */
static void fold_name(const char *name, char *buf, size_t bufsz)
{
    size_t i = 0;
    for (; name[i] && i + 1 < bufsz; i++) {
        unsigned char c = (unsigned char)name[i];
        buf[i] = (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    }
    buf[i] = '\0';
}

/* Rank of a folded name, interning it into `folded` if it is new.
 *
 * The table is hashed on the *content*, not on a pool offset, because `folded` is
 * append-only and so hands two identical names two different offsets. One strcmp per
 * probe on a matching string is the price; hashing the offset would need a deduplicating
 * intern to be correct -- which the *names* pool now has (name_intern() above), and which
 * this pool deliberately does not: ranks are over distinct folded names, so a second copy
 * of one is a bug there rather than 70 MiB of waste. */
static uint32_t rk_intern(esidx_t *db, const char *folded)
{
    uint32_t h = hash_bytes(folded) & db->rk_mask;
    while (db->rk_tab[h]) {
        uint32_t rank = db->rk_tab[h] - 1;
        if (!strcmp(sp_get(&db->folded, db->rk_off[rank]), folded)) return rank;
        h = (h + 1) & db->rk_mask;
    }
    uint32_t rank = db->n_ranks++;
    if (sp_intern(&db->folded, folded, strlen(folded), &db->rk_off[rank]) != 0) {
        db->n_ranks--;
        return 0;
    }
    db->rk_tab[h] = rank + 1;
    return rank;
}

/* Rank has to mean *sorted position* or it is not a sort key at all -- interning in
 * scan order would hand out "the first name this directory walk saw", which is the
 * order the answer already has. So the distinct names are sorted once here and
 * renumbered. strcmp on the folded strings is the same order cmp_folded() produces,
 * because they are folded: memcmp over the common prefix then length. */
static int cmp_rank_str(const void *pa, const void *pb, void *arg)
{
    const esidx_t *db = arg;
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    return strcmp(sp_get(&db->folded, db->rk_off[a]),
                  sp_get(&db->folded, db->rk_off[b]));
}

int esidx_build_name_rank(esidx_t *db)
{
    esidx_free_name_rank(db);
    const entry_table_t *et = &db->et;
    if (et->count == 0) return 0;

    /* One slot per entry bounds the distinct names from above -- two entries in one
     * directory cannot share a name -- so these tables never have to grow. */
    uint32_t cap = 64;
    while (cap < et->count) cap <<= 1;
    db->rk_tab  = calloc(cap, sizeof(uint32_t));
    db->rk_off  = malloc((size_t)et->count * sizeof(uint32_t));
    db->name_rank = malloc((size_t)et->count * sizeof(uint32_t));
    if (!db->rk_tab || !db->rk_off || !db->name_rank) {
        esidx_free_name_rank(db);
        return -1;
    }
    db->rk_mask = cap - 1;

    uint64_t t0 = ts_us();
    char buf[1024];
    for (uint32_t i = 0; i < et->count; i++) {
        if (et->flags[i] & EF_DEAD) continue;
        fold_name(display_name_of(db, i), buf, sizeof(buf));
        db->name_rank[i] = rk_intern(db, buf);
    }

    /* sort the distinct names and renumber into sorted position */
    uint32_t n = db->n_ranks;
    uint32_t *order = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *newr  = malloc((size_t)n * sizeof(uint32_t));
    if (!order || !newr) { free(order); free(newr); esidx_free_name_rank(db); return -1; }
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    qsort_r(order, n, sizeof(uint32_t), cmp_rank_str, db);

    /* Equal names must land on equal ranks, or the tie-break would order them by
     * insertion instead of falling through to the id as it did before. */
    uint32_t m = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (i > 0 && strcmp(sp_get(&db->folded, db->rk_off[order[i - 1]]),
                            sp_get(&db->folded, db->rk_off[order[i]])) != 0)
            m++;
        newr[order[i]] = m;
    }
    uint32_t *noff = malloc((size_t)n * sizeof(uint32_t));
    if (!noff) { free(order); free(newr); esidx_free_name_rank(db); return -1; }
    for (uint32_t i = 0; i < n; i++) noff[newr[i]] = db->rk_off[order[i]];
    memcpy(db->rk_off, noff, (size_t)n * sizeof(uint32_t));
    for (uint32_t i = 0; i < et->count; i++)
        if (!(et->flags[i] & EF_DEAD)) db->name_rank[i] = newr[db->name_rank[i]];
    db->n_ranks = m + 1;
    free(order); free(newr); free(noff);

    /* the intern table indexes the *new* numbering */
    memset(db->rk_tab, 0, ((size_t)db->rk_mask + 1) * sizeof(uint32_t));
    for (uint32_t r = 0; r < db->n_ranks; r++) {
        uint32_t h = hash_bytes(sp_get(&db->folded, db->rk_off[r])) & db->rk_mask;
        while (db->rk_tab[h]) h = (h + 1) & db->rk_mask;
        db->rk_tab[h] = r + 1;
    }

    TSDONE2("finalize: name rank", t0, "(%u distinct of %u)", db->n_ranks, et->count);
    return 0;
}

void esidx_free_name_rank(esidx_t *db)
{
    free(db->folded.buf);
    free(db->rk_tab);
    free(db->rk_off);
    free(db->name_rank);
    db->folded.buf = NULL;
    db->folded.len = db->folded.cap = 0;
    db->rk_tab = NULL; db->rk_off = NULL; db->name_rank = NULL;
    db->rk_mask = 0; db->n_ranks = 0;
}

uint32_t esidx_name_rank(const esidx_t *db, eid_t id)
{
    if (!db->name_rank || id >= db->et.count) return 0;
    return db->name_rank[id];
}

/* ------------------------------------------------------------ entry table */

/* §5.5's aggregate, stored. Its width is the entry count like every other column,
     * and like every other column it is initialised by et_grow()'s caller: a new row
     * has no children, and the only two functions that change that are di_add_child()
     * and di_remove_child(). */
static void et_init_col(entry_table_t *et, uint32_t id)
{
    et->nchild[id] = 0;
}

static int et_grow(entry_table_t *et, uint32_t ncap)
{
    /* Each pointer is stored as soon as it is reallocated rather than at the end.
     * The originals are all live, so a failure half way through would otherwise
     * leave the table holding pointers that realloc has already freed -- and the
     * caller has no way to tell, because the return value is the only signal. */
    eid_t    *p = realloc(et->parent, ncap * sizeof(eid_t));      if (!p) return -1; et->parent = p;
    uint16_t *d = realloc(et->depth,  ncap * sizeof(uint16_t));   if (!d) return -1; et->depth = d;
    uint16_t *f = realloc(et->flags,  ncap * sizeof(uint16_t));   if (!f) return -1; et->flags = f;
    int64_t  *s = realloc(et->size,   ncap * sizeof(int64_t));    if (!s) return -1; et->size = s;
    int64_t  *m = realloc(et->mtime,  ncap * sizeof(int64_t));    if (!m) return -1; et->mtime = m;
    int64_t  *c = realloc(et->ctime,  ncap * sizeof(int64_t));    if (!c) return -1; et->ctime = c;
    int64_t  *k = realloc(et->stamp,  ncap * sizeof(int64_t));    if (!k) return -1; et->stamp = k;
    uint16_t *e = realloc(et->ext_id, ncap * sizeof(uint16_t));   if (!e) return -1; et->ext_id = e;
    strref_t *r = realloc(et->name,   ncap * sizeof(strref_t));   if (!r) return -1; et->name = r;
    uint32_t *k2 = realloc(et->nchild, ncap * sizeof(uint32_t)); if (!k2) return -1; et->nchild = k2;
    et->cap = ncap;
    return 0;
}

/* The scan appends by doubling, so `cap` is the next power of two above the entry
 * count: /work ends at 8 388 608 for 5 476 485 entries, 35 % of the columns'
 * address space reserved for rows that do not exist. realloc never writes past
 * `count`, so the slack costs nothing resident -- it is the ext-bitmap failure
 * mode (design §5.4: 4.2 GB of address space, 213 MB of it resident) rather than
 * a memory one, and it is what fails under a refused overcommit or a `ulimit -v`.
 *
 * Trimming once here rather than growing by less is the deliberate trade: a
 * growth factor of 1.25 would copy the columns five times over the life of a
 * build instead of twice, and the copy lands in esidx_add() -- 6.6 s of a 55.7 s
 * /work walk -- on a path that is already the slowest non-syscall part of it.
 * mremap shrinks a large block in place, so this costs a walk of the pages, not a
 * copy of them, and it happens before finalize builds anything, which is why the
 * transient peak does not move. */
static void et_trim(entry_table_t *et)
{
    if (et->count == 0 || et->count >= et->cap) return;
    uint64_t t0 = ts_us();
    uint32_t was = et->cap;
    if (et_grow(et, et->count) == 0)
        TSDONE2("finalize: columns trimmed", t0, "%u -> %u rows of address space", was, et->count);
    else
        LOGW("finalize: cannot trim the columns to %u rows; the slack stays", et->count);
}

/* from the dir-tree section below */
int di_add_child(esidx_t *db, eid_t dir, eid_t child);

/* --------------------------------------------------------- extension interning
 *
 * Two counters, because "the scan was O(n) and the hash is O(1)" is a claim about a
 * loop and not a measurement: how many times an extension name was looked up, and how
 * many string compares that took. They are what makes the before and the after
 * checkable, and the before was an estimate ("~13 G strcmp calls") that nothing could
 * confirm -- which is how it stayed in a comment through three commits, and how a number
 * 6x larger than the truth came to be the one everybody read.
 *
 * Per-process, not per-db, for the same reason g_ext_truncated is: one index at a time
 * in this process, and a server's number is the interesting one. */
static uint64_t g_ext_calls;      /* ext_intern() calls */
static uint64_t g_ext_compares;   /* strcmp calls made while probing */

/* The table starts here and doubles. 256 is not a tuned number: it is the smallest
 * power of two that keeps the 3/4-load growth threshold out of the way for a tree with
 * a few dozen extensions, and the growth threshold is what decides the rest. */
#define EXT_TAB_MIN 256

/* Allocate the table if it is not there yet. The only two callers are ext_intern() and
 * the load path, and both want the same invariant: a non-NULL table with a mask. */
static int ext_tab_ensure(esidx_t *db)
{
    if (db->ext_tab) return 0;
    db->ext_tab = calloc(EXT_TAB_MIN, sizeof(uint32_t));
    if (!db->ext_tab) { LOGE("ext: cannot allocate the intern table"); return -1; }
    db->ext_tab_mask = EXT_TAB_MIN - 1;
    return 0;
}

/* (Re)build the table at `ncap` slots from the ids in ext_off. Shared by the growth
 * path and the load path, so "the table holds every interned extension" is written
 * once. Each name is hashed again from the pool rather than carried in the slot,
 * because a slot holds an id and the id holds the offset -- there is no hash to keep. */
static int ext_tab_fill(esidx_t *db, uint32_t ncap)
{
    uint32_t *nt = calloc(ncap, sizeof(uint32_t));
    if (!nt) {
        LOGE("ext: cannot allocate the intern table (%u slots)", ncap);
        return -1;      /* the old table stays: a lookup degrades, it does not lie */
    }
    uint32_t mask = ncap - 1;
    for (uint32_t i = 0; i < db->n_ext; i++) {
        uint32_t h = hash_bytes(sp_get(&db->exts, db->ext_off[i])) & mask;
        while (nt[h]) h = (h + 1) & mask;
        /* i + 2, not i + 1. A slot holds ext_id + 1 and ext_id is 1-based, so the
         * first id lands in a slot as 2 -- and writing i + 1 here makes every lookup
         * resolve one id low, which is a wrong extension's rows rather than an
         * error. It is the same convention as the dir hash, the eid -> ordinal map
         * and the name rank, and the first version of this line was the fourth
         * place it had to be written down. */
        nt[h] = i + 2;
    }
    free(db->ext_tab);
    db->ext_tab = nt;
    db->ext_tab_mask = mask;
    return 0;
}

static int ext_tab_grow(esidx_t *db)
{
    return ext_tab_fill(db, (db->ext_tab_mask + 1) * 2);
}

/* The id of an extension name: dense, 1-based, 0 meaning "none". The name goes into the
 * pool and its id goes into ext_off, so the id cannot outgrow 16 bits the way a
 * pool offset did -- see esidx_t for what that cost.
 *
 * The lookup is a hash of the name rather than a scan of every name interned so far.
 * The scan was the same complexity the pool scan it replaced had, so it was not a
 * regression -- it was just never cheap, and the cost grew with the number of
 * extensions rather than with the number of files. */
uint16_t ext_intern(esidx_t *db, const char *name)
{
    g_ext_calls++;
    if (ext_tab_ensure(db) != 0) return 0;

    /* Grow before the probe, not after the insert: the mask the probe has to use is the
     * one the insert will land in, and a rehash then only walks ids that already exist.
     * 3/4 load, so the table cannot fill before the next doubling. */
    if ((db->n_ext + 1) * 4 > (db->ext_tab_mask + 1) * 3 && ext_tab_grow(db) != 0)
        return 0;

    uint32_t h = hash_bytes(name) & db->ext_tab_mask;
    while (db->ext_tab[h]) {
        uint16_t id = (uint16_t)(db->ext_tab[h] - 1);   /* the ext id, 1-based */
        g_ext_compares++;
        /* ext_off[id - 1], not ext_off[id]: the slot holds id + 1 and ext_off is
         * indexed from 0. It is the same arithmetic ext_str() does, and the first
         * version of this line indexed from 1 -- which never matched, so every lookup
         * of an extension that was already interned appended a second copy of it and
         * handed the row a second id. 1 181 distinct extensions over 1 200 files on a
         * fixture with 400 of them, and `ext:e1` matching nothing. */
        if (strcmp(sp_get(&db->exts, db->ext_off[id - 1]), name) == 0) return id;
        h = (h + 1) & db->ext_tab_mask;
    }

    if (db->n_ext == UINT16_MAX - 1) {      /* 0 and the wrap guard are both reserved */
        LOGE("ext: more than %u distinct extensions; ids are 16-bit", UINT16_MAX - 1);
        return 0;
    }
    uint32_t noff;
    if (sp_intern(&db->exts, name, strlen(name), &noff) != 0) return 0;
    uint32_t cap = db->n_ext + 1;
    if (cap > db->ext_off_cap) {
        uint32_t ncap = db->ext_off_cap ? db->ext_off_cap * 2 : 256;
        uint32_t *no = realloc(db->ext_off, (size_t)ncap * sizeof(uint32_t));
        if (!no) { LOGE("ext: cannot grow the id table"); return 0; }
        db->ext_off = no;
        db->ext_off_cap = ncap;
    }
    uint16_t id = (uint16_t)(db->n_ext + 1);
    db->ext_off[db->n_ext] = noff;
    db->ext_tab[h] = (uint32_t)id + 1;
    db->n_ext++;
    return id;
}

/* The name behind an id, or "" for 0 and for anything out of range -- an id that does not
 * resolve must not read past the table, because that is how the wrap it replaced turned
 * into a wrong answer rather than a wrong count. */
const char *ext_str(const esidx_t *db, uint16_t ext_id)
{
    if (!ext_id || ext_id > db->n_ext) return "";
    return sp_get(&db->exts, db->ext_off[ext_id - 1]);
}

/* An extension is the text after the last dot of a name, so its length is bounded by
 * NAME_MAX -- 255 on ext4, and there is nothing above that to allow for. The bound here is
 * that plus the NUL, and it is shared with ext_list_ids() so the two ends cannot disagree
 * about where a name ends: they used to (31 here, 63 there), which is how a query for a
 * real extension matched nothing at all.
 *
 * A cut is still bounded rather than trusted, because a caller with a longer string must
 * not overflow the buffer -- but it is unreachable from a filename, so the counter behind
 * it is a guard and not a statistic. */
static uint64_t g_ext_truncated;      /* entries whose extension did not fit */

static uint16_t ext_of(esidx_t *db, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0' || dot == name) return 0;
    char buf[EXT_NAME_MAX];
    size_t n = strlen(dot + 1);
    if (n >= sizeof(buf)) { n = sizeof(buf) - 1; g_ext_truncated++; }
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)dot[1 + i]);
    buf[n] = '\0';
    return ext_intern(db, buf);
}

/* ------------------------------------------------------------------ mutation */

/* The epoch answers one question: "can a query return something different now?".
 * Bumping it on every pass would make a scheduled refresh throw away the protocol
 * layer's cached result set every few seconds even when the filesystem did not
 * move, which is the difference between a refresh being invisible and it being a
 * performance bug. One place, so the three callers cannot disagree about what a
 * mutation is. */
static void bump_epoch(esidx_t *db)
{
    db->epoch++;
    db->last_change_us = ts_us();
}

/* Make a freshly appended id visible to everything derived from the columns.
 * finalize builds these from scratch for a batch build; a reconcile appends into
 * an index that already has them, and this is the one place that knows the
 * difference. */
static int link_new(esidx_t *db, eid_t id)
{
    const entry_table_t *et = &db->et;
    uint32_t n = et->count;

    /* A new id is past everything finalize sized for, and every set operation
     * silently drops an out-of-range index -- so without this the row would be
     * invisible to `file:`, `folder:` and `ext:` and visible to a text scan. */
    if (bs_reserve(&db->live, n) != 0) return -1;
    if (bs_reserve(&db->type.all, n) != 0) return -1;
    if (bs_reserve(&db->type.dirs, n) != 0) return -1;
    if (bs_reserve(&db->type.files, n) != 0) return -1;

    bs_set(&db->live, id);
    bs_set(&db->type.all, id);
    if (et->flags[id] & EF_DIR) bs_set(&db->type.dirs, id);
    else                       bs_set(&db->type.files, id);

    uint16_t e = et->ext_id[id];
    if (e && ext_index_add(&db->ext, db, e, id) != 0) return -1;

    /* The trigram posting lists are ascending because ids are, so appending here
     * keeps the property the intersection merge depends on (design §5.2). */
    if (tri_index_add(&db->tri, display_name_of(db, id), id) != 0) return -1;

    /* A new directory needs a path in the hash, or `parent:` on it misses. */
    if ((et->flags[id] & EF_DIR) && di_hash_insert(db, id) != 0) return -1;
    return 0;
}

eid_t esidx_add(esidx_t *db, eid_t parent, const entry_in_t *in)
{
    entry_table_t *et = &db->et;
    size_t nlen = strlen(in->name);

    /* The name first, before any column is touched: it is the one step here that can
     * fail on its own, and a failed intern used to be stored as offset 0 -- which is the
     * pool's *first* name, so the row came out carrying someone else's name. Interning
     * returns offset + 1 and 0 on failure, because offset 0 is a real answer. */
    uint32_t slot = name_intern(db, in->name, (uint32_t)nlen);
    if (!slot) return EID_NONE;

    if (et->count == et->cap) {
        if (et_grow(et, et->cap ? et->cap * 2 : 1024) != 0) return EID_NONE;
    }
    eid_t id = et->count++;

    et->name[id].off  = slot - 1;
    et->name[id].len  = (uint32_t)nlen;
    et_init_col(et, id);
    et->parent[id]    = parent;
    et->depth[id]     = in->depth;
    et->flags[id]     = in->flags;
    et->size[id]      = in->size;
    et->mtime[id]     = in->mtime;
    et->ctime[id]     = in->ctime;
    et->stamp[id]     = in->stamp;
    et->ext_id[id]    = (in->flags & EF_DIR) ? 0 : ext_of(db, in->name);

    if (parent == EID_NONE) db->root_eid = id;
    else if (di_add_child(db, parent, id) != 0) return EID_NONE;

    if (db->built) {
        if (link_new(db, id) != 0) { LOGE("cannot index new entry %s", in->name); return EID_NONE; }
        bump_epoch(db);
    }
    return id;
}

int esidx_touch(esidx_t *db, eid_t id, const entry_in_t *in)
{
    entry_table_t *et = &db->et;
    if (id >= et->count || (et->flags[id] & EF_DEAD)) return -1;

    /* Each column reaches its sorted array only when it actually moved. A deep
     * pass compares every entry on the tree, and pushing all three unconditionally
     * would put the whole index into the delta on every pass -- the merge in D3
     * exists precisely to keep that buffer small. */
    bool changed = false;
    if (et->size[id] != in->size) {
        et->size[id] = in->size;
        sidx_update(&db->by_size, in->size, id);
        changed = true;
    }
    if (et->mtime[id] != in->mtime) {
        et->mtime[id] = in->mtime;
        sidx_update(&db->by_mtime, in->mtime, id);
        changed = true;
    }
    if (et->ctime[id] != in->ctime) {
        et->ctime[id] = in->ctime;
        sidx_update(&db->by_ctime, in->ctime, id);
        changed = true;
    }
    /* The stamp is not a queried column -- it is what the *next* reconcile
     * compares -- so a directory whose stamp moved and whose size/mtime did not
     * does not move the epoch, and a refresh that finds nothing leaves the
     * protocol layer's result cache alone. */
    et->stamp[id] = in->stamp;
    if (changed) bump_epoch(db);
    return 0;
}

/* Tombstone one entry. The caller has already dealt with the children. */
static void kill_one(esidx_t *db, eid_t id)
{
    entry_table_t *et = &db->et;

    /* Retract the value each sorted array still holds for this id, then publish
     * the new one (EID_NONE) so a range query covering either value drops it. */
    sidx_erase(&db->by_size, et->size[id], id);
    sidx_erase(&db->by_mtime, et->mtime[id], id);
    sidx_erase(&db->by_ctime, et->ctime[id], id);

    uint16_t e = et->ext_id[id];
    if (e) ext_index_del(&db->ext, e, id);

    bs_clear_bit(&db->live, id);
    bs_clear_bit(&db->type.all, id);
    bs_clear_bit(&db->type.dirs, id);
    bs_clear_bit(&db->type.files, id);

    if (et->flags[id] & EF_DIR) di_hash_erase(db, id);
    eid_t p = et->parent[id];
    if (p != EID_NONE) di_remove_child(db, p, id);

    et->flags[id] |= EF_DEAD;
    bump_epoch(db);
}

int esidx_remove(esidx_t *db, eid_t id)
{
    entry_table_t *et = &db->et;
    if (id >= et->count || (et->flags[id] & EF_DEAD)) return 0;

    /* Children first, so the tree is never left with a live row whose parent is
     * a tombstone -- that combination is what path_of() would walk into. The
     * ordinal and the vector are re-read every turn because the recursion removes
     * from this very vector (di_remove_child swap-removes) and can reallocate it. */
    const childvec_t *cv = di_children(db, id);
    while (cv && cv->n) {
        eid_t kid = cv->items[cv->n - 1];
        di_children_mut(db, id)->n--;
        esidx_remove(db, kid);
        cv = di_children(db, id);
    }
    kill_one(db, id);
    return 0;
}

uint32_t esidx_live_count(const esidx_t *db)
{
    return bs_count(&db->live);
}

/* ---------------------------------------------------------- directory tree */

static uint32_t hash_str(const char *s)
{
    uint32_t h = 2166136261u;   /* FNV-1a */
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* The reverse map. EID_NONE for an id that is not an indexed directory -- which
 * includes every tombstone, whose ordinal is deliberately left in place rather
 * than renumbered (D8). */
uint32_t di_ord(const dir_index_t *di, eid_t dir)
{
    if (!di->ord_slot || dir == EID_NONE) return EID_NONE;
    uint32_t h = dir & di->ord_mask;
    for (;;) {
        uint32_t v = di->ord_slot[h];
        if (!v) return EID_NONE;
        if (di->ord_eid[v - 1] == dir) return v - 1;
        h = (h + 1) & di->ord_mask;
    }
}

/* Index `dir` under the next ordinal. Growth is by doubling on the table and on
 * the header array together, so both stay a power of two apart and neither can be
 * the smaller one's excuse for the other. */
static int di_ord_add(dir_index_t *di, eid_t dir)
{
    if (!di->ord_slot) {
        di->ord_slot = calloc(1024, sizeof(uint32_t));
        if (!di->ord_slot) return -1;
        di->ord_mask = 1023;
    }
    if ((di->ord_count + 1) * 4 >= (di->ord_mask + 1) * 3) {
        uint32_t ncap = (di->ord_mask + 1) * 2;
        uint32_t *ns = calloc(ncap, sizeof(uint32_t));
        if (!ns) return -1;
        for (uint32_t o = 0; o < di->ord_count; o++) {
            uint32_t h = di->ord_eid[o] & (ncap - 1);
            while (ns[h]) h = (h + 1) & (ncap - 1);
            ns[h] = o + 1;
        }
        free(di->ord_slot);
        di->ord_slot = ns;
        di->ord_mask = ncap - 1;
    }
    uint32_t ord = di->ord_count;
    eid_t *ne = realloc(di->ord_eid, ((size_t)ord + 1) * sizeof(eid_t));
    if (!ne) return -1;
    di->ord_eid = ne;
    di->ord_eid[ord] = dir;
    di->ord_count = ord + 1;

    uint32_t h = dir & di->ord_mask;
    while (di->ord_slot[h]) h = (h + 1) & di->ord_mask;
    di->ord_slot[h] = ord + 1;
    return (int)ord;
}

static int di_ord_grow(dir_index_t *di, uint32_t ord)
{
    if (ord < di->child_cap) return 0;
    uint32_t ncap = di->child_cap ? di->child_cap : 1024;
    while (ord >= ncap) ncap *= 2;
    childvec_t *nc = realloc(di->child, (size_t)ncap * sizeof(childvec_t));
    if (!nc) return -1;
    memset(nc + di->child_cap, 0, (size_t)(ncap - di->child_cap) * sizeof(childvec_t));
    di->child = nc;
    di->child_cap = ncap;
    return 0;
}

int di_add_child(esidx_t *db, eid_t dir, eid_t child)
{
    dir_index_t *di = &db->di;
    uint32_t ord = di_ord(di, dir);
    if (ord == EID_NONE) {
        int added = di_ord_add(di, dir);
        if (added < 0) return -1;
        ord = (uint32_t)added;
    }
    if (di_ord_grow(di, ord) != 0) return -1;

    childvec_t *cv = &di->child[ord];
    if (cv->n == cv->cap) {
        uint32_t ncap = cv->cap ? cv->cap * 2 : 8;
        eid_t *ni = realloc(cv->items, (size_t)ncap * sizeof(eid_t));
        if (!ni) return -1;
        cv->items = ni;
        cv->cap = ncap;
    }
    cv->items[cv->n++] = child;
    /* The aggregate column moves with the vector, here and in di_remove_child and
     * nowhere else: `child-count:` and `empty:` read it once per candidate row, and
     * a count read out of the vector would mean an ordinal probe per row. */
    if (dir < db->et.count) db->et.nchild[dir]++;
    return 0;
}

const childvec_t *di_children(const esidx_t *db, eid_t dir)
{
    uint32_t ord = di_ord(&db->di, dir);
    if (ord == EID_NONE || ord >= db->di.child_cap) return NULL;
    return &db->di.child[ord];
}

childvec_t *di_children_mut(esidx_t *db, eid_t dir)
{
    uint32_t ord = di_ord(&db->di, dir);
    if (ord == EID_NONE || ord >= db->di.child_cap) return NULL;
    return &db->di.child[ord];
}

bool di_is_empty(const esidx_t *db, eid_t dir)
{
    if (dir >= db->et.count) return true;
    return db->et.nchild[dir] == 0;
}

/* Swap-remove rather than preserve order: nothing depends on the order, and a
 * removal from a directory holding thousands of entries must not be O(n) memmove
 * on top of the O(n) search. */
int di_remove_child(esidx_t *db, eid_t dir, eid_t child)
{
    dir_index_t *di = &db->di;
    uint32_t ord = di_ord(di, dir);
    if (ord == EID_NONE || ord >= di->child_cap) return -1;
    childvec_t *cv = &di->child[ord];
    for (uint32_t i = 0; i < cv->n; i++) {
        if (cv->items[i] != child) continue;
        cv->items[i] = cv->items[cv->n - 1];
        cv->n--;
        if (dir < db->et.count && db->et.nchild[dir]) db->et.nchild[dir]--;
        return 0;
    }
    return -1;
}

/* One child's name, without materialising a path. The reconcile matches a
 * getdents entry against the stored children of the same directory on every
 * single entry, and path_of() there would be O(depth) per lookup. */
eid_t di_lookup_name(const esidx_t *db, eid_t dir, const char *name)
{
    const childvec_t *cv = di_children(db, dir);
    if (!cv) return EID_NONE;
    for (uint32_t i = 0; i < cv->n; i++) {
        eid_t id = cv->items[i];
        if (strcmp(name_of(db, id), name) == 0) return id;
    }
    return EID_NONE;
}

/* A slot holds its key's offset into `dpaths` **plus one**, because 0 means
 * "empty" and offset 0 is a legal offset. It did not have to be plus one while
 * the keys lived in the *names* pool: that pool already held every entry's name,
 * so a directory path could never land on offset 0 and the sentinel was safe by
 * accident. Giving the derived keys their own pool -- which is what stopped the
 * snapshot growing on every refresh -- made the first interned path offset 0,
 * which is the indexed root's, and `parent:` on the root stopped resolving while
 * every subdirectory kept working. The other three open-addressed tables in this
 * codebase already store value+1 for exactly this reason (ext_index.tab,
 * tri_index.tab, rk_tab).
 *
 * HT_TOMB marks a slot whose directory is gone. It cannot be 0 for the same
 * reason, and it cannot be treated as empty: a linear probe has to walk *through*
 * a removed entry to reach the entries behind it. */
#define HT_TOMB 0xFFFFFFFFu

eid_t di_lookup(const esidx_t *db, const char *path)
{
    const dir_index_t *di = &db->di;
    if (!di->ht_off) return EID_NONE;
    uint32_t i = hash_str(path) & di->ht_mask;
    while (di->ht_off[i] != 0) {
        if (di->ht_off[i] != HT_TOMB &&
            strcmp(sp_get(&db->dpaths, di->ht_off[i] - 1), path) == 0)
            return di->ht_val[i];
        i = (i + 1) & di->ht_mask;
    }
    return EID_NONE;
}

static void di_hash_rehash(esidx_t *db, uint32_t ncap);

int di_hash_insert(esidx_t *db, eid_t dir)
{
    dir_index_t *di = &db->di;
    if (!di->ht_off) return -1;
    /* Leave room: the table is built for the entry count finalize saw, and a
     * reconcile adds directories to it. At the old sizing a busy tree would run
     * the probe length up to where a full table turns every lookup into a scan. */
    if ((di->ht_count + 1) * 4 >= (di->ht_mask + 1) * 3) {
        di_hash_rehash(db, (di->ht_mask + 1) * 2);
        if (!di->ht_off) return -1;
    }

    char buf[65536];
    path_of(db, dir, buf, sizeof(buf));

    /* Hashed off `buf` rather than off an interned copy of it: a directory whose
     * path is already in the table returns below, and interning first would
     * strand those bytes in the pool. */
    uint32_t i = hash_str(buf) & di->ht_mask;
    uint32_t reuse = UINT32_MAX;
    while (di->ht_off[i] != 0) {
        if (di->ht_off[i] == HT_TOMB) {
            if (reuse == UINT32_MAX) reuse = i;
        } else if (strcmp(sp_get(&db->dpaths, di->ht_off[i] - 1), buf) == 0) {
            di->ht_val[i] = dir;      /* same path, new id */
            return 0;
        }
        i = (i + 1) & di->ht_mask;
    }
    if (reuse != UINT32_MAX) i = reuse;
    else di->ht_count++;
    uint32_t poff;
    if (sp_intern(&db->dpaths, buf, strlen(buf), &poff) != 0) return -1;
    di->ht_off[i] = poff + 1;
    di->ht_val[i] = dir;
    return 0;
}

int di_hash_erase(esidx_t *db, eid_t dir)
{
    dir_index_t *di = &db->di;
    if (!di->ht_off) return -1;
    /* Rebuild the path to find the probe start. It has to happen before the
     * caller sets EF_DEAD, because path_of() walks live parents -- an erase per
     * removed directory is a price worth paying to avoid a third parallel array
     * in the table. */
    char buf[65536];
    path_of(db, dir, buf, sizeof(buf));
    uint32_t i = hash_str(buf) & di->ht_mask;
    while (di->ht_off[i] != 0) {
        if (di->ht_off[i] != HT_TOMB &&
            di->ht_val[i] == dir &&
            strcmp(sp_get(&db->dpaths, di->ht_off[i] - 1), buf) == 0) {
            di->ht_off[i] = HT_TOMB;
            di->ht_count--;
            return 0;
        }
        i = (i + 1) & di->ht_mask;
    }
    return -1;
}

const char *name_of(const esidx_t *db, eid_t id)
{
    if (id >= db->et.count) return "";
    return sp_get(&db->names, db->et.name[id].off);
}

/* The name as Everything reports it: the last component of the entry's own path.
 *
 * A parentless entry is the one place the stored name is not already a bare name --
 * scan.c:169 stores the root as the absolute path it was indexed from, because
 * path_of() and di_lookup() both need that string and there is nowhere else to keep
 * it. So this is `basename(name_of())`, which is what makes the root row read like
 * every other row instead of printing its own path as its name.
 *
 * Measured against voidtools' server on :21, which prints both shapes this way and
 * mentions no root in either: `FOLDER ShareToPC` with `PATH
 * C:\Users\linswin\AndroidStudioProjects` for a folder inside the tree, `FOLDER C:`
 * with an empty PATH for the drive root (a stored name with no separator in it).
 *
 * Deliberately not basename(path_of()): that is O(depth) with a buffer per row, and
 * the `name:` matcher calls this once per candidate (query.c). One strrchr over a
 * name that is a single component for everything else is the same answer for free. */
const char *display_name_of(const esidx_t *db, eid_t id)
{
    const char *s = name_of(db, id);
    const char *slash = strrchr(s, '/');
    return slash ? slash + 1 : s;
}

const char *ext_of_str(const esidx_t *db, eid_t id)
{
    if (id >= db->et.count) return "";
    return ext_str(db, db->et.ext_id[id]);
}

uint32_t di_child_count(const esidx_t *db, eid_t dir)
{
    if (dir >= db->et.count) return 0;
    return db->et.nchild[dir];
}

/* The full path of the directory containing this entry -- what the wire's PATH column
 * carries, and what the client joins the name onto (AGENTS.md 5.1).
 *
 * Defined as dirname(path_of()) rather than path_of(parent), so it is one rule for
 * every entry and has no branch on "is this the root": a parentless entry's own path
 * already ends in its last component, and cutting at the last separator leaves the
 * directory above it. path_of(parent) would need a fallback for exactly that one row,
 * and the fallback is this. */
void parent_path_of(const esidx_t *db, eid_t id, char *out, size_t outsz)
{
    if (id >= db->et.count || !outsz) { if (outsz) out[0] = '\0'; return; }
    path_of(db, id, out, outsz);
    char *slash = strrchr(out, '/');
    if (!slash) { out[0] = '\0'; return; }   /* a single-component path has no parent */
    if (slash == out) out[1] = '\0';         /* directly under /: the parent is / */
    else *slash = '\0';
}

/* Win32 attribute bits, for the ATTRIBUTES column and for sorting by them.
 * Only H and D have an ext4 equivalent (everything-syntax.md L239-240); the rest
 * of the mask Everything reports has no filesystem counterpart, so it is left
 * clear rather than invented. R (read-only) is the one more that maps cleanly
 * onto a POSIX mode bit, and is derived from the entry's stored flags. */
uint32_t esidx_win_attributes(const esidx_t *db, eid_t id)
{
    if (id >= db->et.count) return 0;
    uint16_t f = db->et.flags[id];
    uint32_t a = 0;
    if (f & EF_DIR)    a |= 0x10u;   /* FILE_ATTRIBUTE_DIRECTORY */
    if (f & EF_HIDDEN) a |= 0x02u;   /* FILE_ATTRIBUTE_HIDDEN    */
    if (f & EF_READONLY) a |= 0x01u; /* FILE_ATTRIBUTE_READONLY  */
    /* FILE_ATTRIBUTE_ARCHIVE (0x20) is what Everything sets on every ordinary
     * file; the ETP client only masks off 0x02|0x04, so setting it is safe
     * and matches the reference server's output. */
    if (!(f & EF_DIR)) a |= 0x20u;
    return a;
}

void path_of(const esidx_t *db, eid_t id, char *out, size_t outsz)
{
    /* walk the parent chain (FSearch build_path_recursively, iterative here) */
    strref_t stack[256];
    int n = 0;
    eid_t cur = id;
    while (cur != EID_NONE && n < 256) {
        stack[n++] = db->et.name[cur];
        cur = db->et.parent[cur];
    }
    size_t pos = 0;
    out[0] = '\0';
    for (int i = n - 1; i >= 0; i--) {
        const char *s = sp_get(&db->names, stack[i].off);
        size_t l = stack[i].len;
        if (pos + l + 2 >= outsz) break;
        if (pos > 0 && out[pos - 1] != '/') out[pos++] = '/';
        memcpy(out + pos, s, l);
        pos += l;
    }
    out[pos] = '\0';
}

/* ---------------------------------------------------------------- sorting */

/* Order by value, then by id. The tie-break is not observable from outside -- a range
 * query answers with a bitset, and the executor's own order is the id order -- but a
 * merge has to produce a *deterministic* array or two merges of the same delta would
 * differ, so it stays. */
static int cmp_id_by_val(const void *pa, const void *pb, void *arg)
{
    const int64_t *vals = arg;
    eid_t a = *(const eid_t *)pa, b = *(const eid_t *)pb;
    if (vals[a] < vals[b]) return -1;
    if (vals[a] > vals[b]) return 1;
    return (a < b) ? -1 : (a > b);
}

void sidx_build(sidx_t *s, const int64_t *vals, uint32_t n)
{
    s->v  = malloc((n ? n : 1) * sizeof(int64_t));
    s->id = malloc((n ? n : 1) * sizeof(eid_t));
    if (!s->v || !s->id) { LOGE("sidx: cannot allocate %u rows", n); sidx_free(s); return; }
    s->n = s->cap = n;
    for (uint32_t i = 0; i < n; i++) s->id[i] = i;
    /* Sort the ids against the *column*, then gather: the values never need an array of
     * their own before this point, which is the whole reason the split is free. */
    qsort_r(s->id, n, sizeof(eid_t), cmp_id_by_val, (void *)vals);
    for (uint32_t i = 0; i < n; i++) s->v[i] = vals[s->id[i]];
}

/* D3's O(1) writer. `del` marks the entry as a retraction of `v` rather than an
 * assertion of it -- see SIDX_DEL. */
static void sidx_push(sidx_t *s, int64_t v, eid_t id, bool del)
{
    if (s->dn == s->dcap) {
        uint32_t ncap = s->dcap ? s->dcap * 2 : 256;
        int64_t *nv = realloc(s->dv, (size_t)ncap * sizeof(int64_t));
        if (!nv) { LOGE("sidx: cannot grow the delta values"); return; }
        s->dv = nv;
        eid_t *ni = realloc(s->did, (size_t)ncap * sizeof(eid_t));
        if (!ni) { LOGE("sidx: cannot grow the delta ids"); return; }
        s->did = ni;
        s->dcap = ncap;
    }
    s->dv[s->dn] = v;
    s->did[s->dn] = del ? (id | SIDX_DEL) : id;
    s->dn++;
}

void sidx_update(sidx_t *s, int64_t v, eid_t id) { sidx_push(s, v, id, false); }
void sidx_erase(sidx_t *s, int64_t v, eid_t id)  { sidx_push(s, v, id, true); }

/* D3's merge rule: past 1% of the main array the delta stops being a rounding
 * error in every range query, so fold it in. O(n + d log d) rather than a
 * rebuild, because a rebuild re-sorts the whole array on every pass and the
 * delta exists precisely to avoid that.
 *
 * The subtlety is that an id can appear in the delta several times before the
 * merge, so it is not enough to append every SET entry: id 7 updated twice has
 * two assertions, and keeping both would leave the intermediate value in the
 * array where a range query covering it would still match. Only the *last* action
 * per id survives, so the delta is grouped by id first.
 *
 * The array is already sorted and the surviving delta rows are sorted here, so the
 * two are *merged* rather than the result re-sorted. That is O(n) instead of
 * O(n log n), and it is what makes the split into two arrays affordable here: the old
 * code allocated a whole second array of 16-byte structs while the first was still
 * alive -- 32 bytes a row at the peak -- where this allocates 12 while holding 12.
 * It also removes the trap the first version of this function fell into, which is that
 * `cmp_id_by_val` reads a value *by id* out of the array it was given, and in a merge
 * that array is positional: the ids in it are not indices into it. The sanitiser build
 * found that as a heap-buffer-overflow inside qsort_r, on a tree of nine files, in the
 * one path the index suite exercises hardest. */
typedef struct {
    eid_t    id;
    int64_t  v;
    uint32_t pos;
} dsort_t;

static int cmp_dsort(const void *a, const void *b)
{
    const dsort_t *x = a, *y = b;
    if (x->id != y->id) return x->id < y->id ? -1 : 1;
    return x->pos < y->pos ? -1 : (x->pos > y->pos);
}

/* value order, then id -- the order the main array is in */
static int cmp_keep(const void *a, const void *b)
{
    const dsort_t *x = a, *y = b;
    if (x->v != y->v) return x->v < y->v ? -1 : 1;
    return x->id < y->id ? -1 : (x->id > y->id);
}

int sidx_merge(sidx_t *s)
{
    if (!s->dn) return 0;

    dsort_t *d = malloc((size_t)s->dn * sizeof(dsort_t));
    dsort_t *keep = malloc((size_t)s->dn * sizeof(dsort_t));
    bitset_t touched;
    if (!d || !keep) { free(d); free(keep); return -1; }
    if (bs_init(&touched, s->n ? s->n : 1) != 0) { free(d); free(keep); return -1; }
    for (uint32_t i = 0; i < s->dn; i++) {
        d[i].id = s->did[i] & ~SIDX_DEL;
        d[i].v = s->dv[i];
        d[i].pos = i;
    }
    qsort(d, s->dn, sizeof(dsort_t), cmp_dsort);

    uint32_t nnew = s->n, nk = 0;
    for (uint32_t i = 0; i < s->dn; ) {
        uint32_t j = i + 1;
        while (j < s->dn && d[j].id == d[i].id) j++;
        const dsort_t *last = &d[j - 1];
        bool drop = (last->id < s->n);
        bool keep_it = !(s->did[last->pos] & SIDX_DEL);
        if (drop) { bs_set(&touched, last->id); nnew--; }
        if (keep_it) {
            keep[nk].id = last->id;
            keep[nk].v = last->v;
            nk++;
            nnew++;
        }
        i = j;
    }
    qsort(keep, nk, sizeof(dsort_t), cmp_keep);

    int64_t *nv = malloc((nnew ? nnew : 1) * sizeof(int64_t));
    eid_t   *ni = malloc((nnew ? nnew : 1) * sizeof(eid_t));
    if (!nv || !ni) {
        free(d); free(keep); free(nv); free(ni); bs_free(&touched);
        LOGE("sidx: cannot allocate %u rows for a merge", nnew);
        return -1;
    }

    uint32_t i = 0, j = 0, k = 0;
    while (i < s->n || j < nk) {
        int take_main;
        if (j >= nk)      take_main = 1;
        else if (i >= s->n) take_main = 0;
        else {
            /* ids are unique across the two runs -- a delta id is either still in the
             * main array (and marked) or beyond it -- so this is a strict order */
            take_main = (s->v[i] < keep[j].v) ||
                        (s->v[i] == keep[j].v && s->id[i] < keep[j].id);
        }
        if (take_main) {
            if (bs_test(&touched, s->id[i])) { i++; continue; }
            nv[k] = s->v[i];
            ni[k] = s->id[i];
            i++;
        } else {
            nv[k] = keep[j].v;
            ni[k] = keep[j].id;
            j++;
        }
        k++;
    }
    free(d);
    free(keep);
    bs_free(&touched);

    free(s->v); free(s->id); free(s->dv); free(s->did);
    s->v = nv; s->id = ni;
    s->n = s->cap = k;
    s->dv = NULL; s->did = NULL;
    s->dn = s->dcap = 0;
    return 0;
}

void sidx_free(sidx_t *s)
{
    free(s->v); free(s->id); free(s->dv); free(s->did);
    s->v = NULL; s->id = NULL; s->dv = NULL; s->did = NULL;
    s->n = s->cap = s->dn = s->dcap = 0;
}

uint32_t sidx_lower_bound(const sidx_t *s, int64_t lo)
{
    uint32_t a = 0, b = s->n;
    while (a < b) {
        uint32_t m = a + (b - a) / 2;
        if (s->v[m] < lo) a = m + 1; else b = m;
    }
    return a;
}

/* The count a range would produce, without building the bitset. This is what the
 * optimiser asks (design §6.2 step 1) and it used to open-code the same binary search
 * over the old struct array -- two places that knew the layout, which is how the two
 * could drift. The delta contributes nothing here, exactly as it contributes no
 * *set* membership: it is a set of corrections, and a count of the main array is an
 * upper bound, which is what an estimate is allowed to be. */
uint32_t sidx_count_range(const sidx_t *s, int64_t lo, int64_t hi)
{
    uint32_t a = sidx_lower_bound(s, lo);
    uint32_t cnt = 0;
    for (uint32_t i = a; i < s->n && s->v[i] <= hi; i++) cnt++;
    return cnt;
}

uint32_t sidx_range_to_bitset(const sidx_t *s, int64_t lo, int64_t hi, bitset_t *out)
{
    uint32_t cnt = 0;
    uint32_t a = sidx_lower_bound(s, lo);
    for (uint32_t i = a; i < s->n && s->v[i] <= hi; i++) {
        bs_set(out, s->id[i]);
        cnt++;
    }
    /* The delta is walked in append order, which is chronological, so a
     * retraction always comes after the assertion it undoes. A retraction clears
     * unconditionally rather than only inside [lo,hi]: it retracts *that value*
     * for that id, and the main array set the bit because that value was in the
     * range -- which says nothing about whether the id's current value is. */
    for (uint32_t i = 0; i < s->dn; i++) {
        eid_t id = s->did[i];
        if (id & SIDX_DEL) {
            id &= ~SIDX_DEL;
            if (bs_test(out, id)) { bs_clear_bit(out, id); cnt--; }
        } else if (s->dv[i] >= lo && s->dv[i] <= hi) {
            if (!bs_test(out, id)) cnt++;
            bs_set(out, id);
        }
    }
    return cnt;
}

/* --------------------------------------------------------------- ext sets */

/* ext_id is the byte offset in the exts pool plus one, so it is sparse. `tab` is
 * a small open-addressed map from ext_id to a dense slot; linear probing is fine
 * at a load factor of 0.5 (ref A12 considered and rejected for the trigram table,
 * where the probe distribution matters; here it does not). */

/* What the per-extension cardinality looks like, and what the two candidate
 * structures would cost with it.
 *
 * This is here because the decision it feeds was made without it. design 5.4 argues
 * from "extensions are few and low cardinality" -- 1 580 of them over 372 084 entries
 * when it was written -- and each set is sized by the *table*, not by the extension, so
 * the cost is a product: one bitmap per extension, n bits each. Nothing recorded where
 * that product stops being affordable, and it grows faster than the tree does, because
 * the extension count grows with the tree too.
 *
 * So this prints the distribution rather than a verdict. The loop that fills
 * `counts[]` has already produced every number here; before this it discarded them.
 * Kept at INFO because that is the level a build or load log carries (design 4.4: if
 * the measurement a decision depends on is not available, collect it first, on its
 * own). The per-extension numbers are bucketed because a distribution is what decides
 * a threshold -- a mean would hide that one extension can be a million entries and the
 * other 6 000 are singletons.
 *
 * The two costs are not comparable on memory alone: a bitmap selects in n/64 word
 * operations whatever the cardinality, while a posting list walks k ids. So the line
 * that matters is the crossover, k below which the list is also the *smaller*
 * structure -- an extension that is both broad and above it is the only case where
 * the bitmap earns its address space.
 */
/* A byte count in whichever unit does not print it as "0". A log line whose number is
 * 0 MB tells a reader nothing on the tree they are looking at. */
static void fmt_bytes(char *out, size_t outsz, size_t b)
{
    if (b >= 1024 * 1024)
        snprintf(out, outsz, "%llu MB", (unsigned long long)(b / (1024 * 1024)));
    else if (b >= 1024)
        snprintf(out, outsz, "%llu kB", (unsigned long long)(b / 1024));
    else
        snprintf(out, outsz, "%llu B", (unsigned long long)b);
}

static void log_ext_cardinality(const ext_index_t *xi, uint32_t entries)
{
    /* Buckets: =1, 2-15, 16-255, 256-4095, 4096-65535, >=65536. The last two edges
     * are where a tree's bulk usually sits, so they are where a threshold would fall.
     *
     * Written as an if-chain rather than a bounds table on purpose. The table version
     * indexed `lo[k + 1]` for k == NB-1, one element past the end: undefined, and the
     * -O2 build folded the last comparison away, so a cardinality of 694 286 printed
     * as ">=65536: 0" next to "largest 694286" in the same line. A diagnostic that
     * cannot be wrong by construction is worth three lines of comparisons. */
    enum { NB = 6 };
    uint32_t bn[NB] = {0}, be[NB] = {0};
    uint32_t with_ext = 0, maxc = 0;

    for (uint32_t s = 0; s < xi->n; s++) {
        uint32_t c = xi->counts[s];
        int b = (c == 1) ? 0 : c < 16 ? 1 : c < 256 ? 2
              : c < 4096 ? 3 : c < 65536 ? 4 : 5;
        bn[b]++; be[b] += c; with_ext += c;
        if (c > maxc) maxc = c;
    }

    size_t bitset_bytes = ((size_t)entries + 63) / 64 * sizeof(uint64_t);
    size_t as_bitmaps  = (size_t)xi->n * bitset_bytes;
    size_t as_lists    = (size_t)with_ext * sizeof(eid_t) + (size_t)xi->n * 3 * sizeof(eid_t);
    char bmb[32], lmb[32];
    fmt_bytes(bmb, sizeof(bmb), as_bitmaps);
    fmt_bytes(lmb, sizeof(lmb), as_lists);

    /* `count/entries` per bucket: the extension count alone says how many keys there
     * are, the entry count says where the rows are, and a threshold needs both -- 57
     * extensions holding 3 M rows is a different problem from 57 holding 3 000. */
    LOGI("ext cardinality: %u extensions over %u entries, %u carry one (largest %u,"
         " mean %.1f) | extensions/entries per extension: =1:%u/%u  2-15:%u/%u"
         "  16-255:%u/%u  256-4095:%u/%u  4096-65535:%u/%u  >=65536:%u/%u",
         xi->n, entries, with_ext, maxc,
         xi->n ? (double)with_ext / (double)xi->n : 0.0,
         bn[0], be[0], bn[1], be[1], bn[2], be[2], bn[3], be[3], bn[4], be[4], bn[5], be[5]);
    LOGI("ext cardinality: one bitmap each is %s of address space, a posting list each"
         " is %s | below %u entries an extension is the smaller structure as a list",
         bmb, lmb, (uint32_t)(bitset_bytes / sizeof(eid_t)));
}

/* The shape of the extension *names*, as opposed to the shape of their sets (which is
 * log_ext_cardinality). Two things it settles that the cardinality line cannot:
 *
 *   - whether real extensions come anywhere near EXT_MAX, which is what decides whether
 *     cutting there is harmless in practice;
 *   - how many entries were actually cut, which is the only number that says whether the
 *     cut costs a wrong answer on *this* tree. The pool's own histogram cannot say it: a
 *     cut string is already cut when it lands in the pool, so the 32+ buckets are empty
 *     by construction and not by evidence.
 *
 * The buckets straddle both caps on purpose -- 16-31 is the last bucket before the cut,
 * 32-63 is the first after it -- so a tree that is *about* to hit the cut is visible as
 * mass at the top of 16-31 rather than as a surprise much later.
 */
static void log_ext_lengths(const esidx_t *db, uint32_t ext_in_use)
{
    /* =1, 2-3, 4-7, 8-15, 16-31, 32-63, >=64 */
    enum { NB = 7 };
    uint32_t bn[NB] = {0};
    uint64_t total = 0, longest = 0;
    uint32_t n = 0;

    for (uint32_t off = 0; off < db->exts.len; ) {
        const char *s = db->exts.buf + off;
        size_t l = strlen(s);
        int b = (l == 1) ? 0 : l < 4 ? 1 : l < 8 ? 2 : l < 16 ? 3
              : l < 32 ? 4 : l < 64 ? 5 : 6;
        bn[b]++; n++;
        total += l;
        if (l > longest) longest = l;
        off += (uint32_t)l + 1;
    }

    LOGI("ext lengths: %u distinct, longest %llu, mean %.1f, pool %llu bytes |"
         " characters per extension: =1:%u  2-3:%u  4-7:%u  8-15:%u  16-31:%u"
         "  32-63:%u  >=64:%u",
         n, (unsigned long long)longest, n ? (double)total / (double)n : 0.0,
         (unsigned long long)db->exts.len,
         bn[0], bn[1], bn[2], bn[3], bn[4], bn[5], bn[6]);
    /* A cut is meant to be unreachable -- EXT_NAME_MAX is NAME_MAX -- so this is a
     * warning rather than a statistic, and it says so by being silent when it is zero.
     * At INFO it was invisible without -v 3, which is how the assertion for it in
     * test.sh passed on a fixture that *was* being cut (AGENTS.md 3.5). */
    if (g_ext_truncated)
        LOGW("ext lengths: %llu entries had an extension too long for %d characters and"
             " were cut -- the index is lossy, ext: will not match the real name",
             (unsigned long long)g_ext_truncated, EXT_NAME_MAX - 1);

    /* Every string in the pool was interned by some entry, and ids are dense over that
     * pool -- so at build time the two counts must be equal, and when they are not, some
     * id has collided with another and `ext:` is answering with the wrong rows. It is
     * checked here rather than in a test because the fixture that triggers it needs a
     * pool past 64 KB, i.e. thousands of distinct extensions (AGENTS.md 3.4: only visible
     * above a size threshold), whereas this runs on every build of every tree.
     *
     * A query can also intern a string that is in no entry at all (ext_list_ids() on an
     * unknown extension), which makes the pool legitimately longer -- so this is only
     * valid because ext_index_build() runs during finalize, before anything has queried
     * this db. */
    if (n != ext_in_use)
        LOGE("ext pool holds %u distinct strings but only %u extensions are in use:"
             " %u interned id(s) address the wrong string, so ext: matches the wrong"
             " rows (ext ids are 16-bit offsets into a pool that has outgrown 64 KB)",
             n, ext_in_use, n - ext_in_use);
}

/* A bitmap is kept only when the extension is broad enough that *selecting* it as a
 * bitmap beats walking its ids, and when a bitmap is worth allocating at all. Both
 * halves matter:
 *
 *   - select cost. A bitmap is n/64 word operations whatever the extension's cardinality;
 *     a list is one call plus two bit operations per id, so by op count the bitmap should
 *     win above n/128. Measured, it wins later than that: the widest list-backed extension
 *     on /work is `sha1` at 39 087 rows against a 5 476 485-row table, and its select
 *     costs 0.467 ms as a list against 0.428 ms as a bitmap -- 9 % worse, consistently
 *     over three interleaved runs. So the true crossover is nearer n/64, the list walk
 *     loses to a word loop it was supposed to win against, and the threshold is left at
 *     n/128 anyway: the absolute difference is 0.04 ms on one term, and the memory the
 *     lower half of the range gives up is 4.4 GB. Moving the constant needs a sweep over
 *     it, not one point.
 *   - the floor. A bitmap smaller than a page is not worth having, and the floor is also
 *     what keeps a small tree on the list path -- without it every fixture in the suite
 *     would take the bitmap path and the list path would ship untested, which is the
 *     failure mode AGENTS.md 3.4 keeps producing.
 *
 * The consequence to be honest about: a narrow extension that grows past the threshold
 * keeps its list until the next build. Correct either way, just not optimal, and the same
 * bargain esidx_update() makes for the name rank (a rename cannot re-sort 5 M rows).
 */
#define EXT_BITMAP_MIN_BYTES 512

int ext_index_build(ext_index_t *xi, const esidx_t *db)
{
    ext_index_free(xi);
    const entry_table_t *et = &db->et;
    if (et->count == 0) return 0;

    /* The table is sized for the *extension* count, which the first loop below
     * discovers -- not for the entry count. It used to be the entry count, and on
     * /work that reserved 8 388 608 slots to hold 6 765 extensions (load factor
     * 0.0008) and a second 32 MiB for `ids` malloc'd at the same capacity to hold
     * 27 KB of them. Two passes cannot know n before the first one has run, so the
     * table is filled at the provisional sizing and rehashed once the count is
     * known; from then on the growth in ext_index_add() doubles it, which is
     * correct because it grows with n and not with the table.
     *
     * The growth threshold is 3/4 load, so 2x the count is not just "some headroom":
     * it is the smallest power of two that cannot reach the threshold before the
     * next doubling. */
    uint32_t cap = 64;
    while (cap < et->count) cap <<= 1;
    uint32_t *tab = calloc(cap, sizeof(uint32_t));
    uint32_t *ids = malloc(cap * sizeof(uint32_t));
    if (!tab || !ids) { free(tab); free(ids); return -1; }
    uint32_t mask = cap - 1, n = 0;

    for (uint32_t i = 0; i < et->count; i++) {
        uint16_t e = et->ext_id[i];
        if (!e || (et->flags[i] & EF_DEAD)) continue;
        uint32_t h = (e * 2654435761u) & mask;
        while (tab[h] && ids[tab[h] - 1] != e) h = (h + 1) & mask;
        if (tab[h]) continue;
        tab[h] = n + 1;
        ids[n++] = e;
    }

    /* Rehash once, now that n is known: a 6 765-entry table does not need 32 MiB
     * to look things up in, and the load factor the probe length depends on is
     * the one the growth threshold assumes. */
    uint32_t want = 64;
    while (want < n * 2) want <<= 1;
    if (want < cap) {
        uint32_t *nt = calloc(want, sizeof(uint32_t));
        if (!nt) { free(tab); free(ids); return -1; }
        uint32_t nmask = want - 1;
        for (uint32_t s = 0; s < n; s++) {
            uint32_t h = (ids[s] * 2654435761u) & nmask;
            while (nt[h]) h = (h + 1) & nmask;
            nt[h] = s + 1;
        }
        free(tab);
        tab = nt;
        mask = nmask;
        cap = want;
    }
    /* Same for `ids`, which outlives the build as xi->ids: it holds the interned
     * ids in slot order and nothing appends to it here. */
    if (n < cap) {
        uint32_t *ni = realloc(ids, (n ? n : 1) * sizeof(uint32_t));
        if (!ni) { free(tab); free(ids); return -1; }
        ids = ni;
    }

    /* Three passes, where it used to be two: the structure of a slot depends on how many
     * entries carry it, and the count is only known once every entry has been visited. */
    bitset_t *sets = calloc(n ? n : 1, sizeof(bitset_t));
    ext_post_t *posts = calloc(n ? n : 1, sizeof(ext_post_t));
    uint32_t *counts = calloc(n ? n : 1, sizeof(uint32_t));
    if (!sets || !posts || !counts) {
        free(sets); free(posts); free(counts); free(tab); free(ids);
        return -1;
    }

    for (uint32_t i = 0; i < et->count; i++) {
        uint16_t e = et->ext_id[i];
        if (!e || (et->flags[i] & EF_DEAD)) continue;
        uint32_t h = (e * 2654435761u) & mask;
        while (ids[tab[h] - 1] != e) h = (h + 1) & mask;
        counts[tab[h] - 1]++;
    }

    /* The choice, and the memory it comes to. Reported whether or not anyone looks,
     * because it is the number that says whether this index is 20 MB or 4 GB. */
    uint32_t n_bitmap = 0, n_list = 0;
    size_t mem_bitmap = 0, mem_list = 0;
    size_t set_bytes = ((size_t)et->count + 63) / 64 * sizeof(uint64_t);
    uint32_t k_min = (uint32_t)(et->count / 128);
    for (uint32_t s = 0; s < n; s++) {
        if (counts[s] > k_min && set_bytes >= EXT_BITMAP_MIN_BYTES) {
            if (bs_init(&sets[s], et->count + 1) != 0) goto fail;
            n_bitmap++;
            mem_bitmap += set_bytes;
        } else {
            /* Sized exactly: a build reads every entry once, so there is no append to
             * grow for, and over-allocating 6 765 lists to make room for a reconcile
             * would cost more than the lists do. */
            posts[s].ids = malloc((counts[s] ? counts[s] : 1) * sizeof(eid_t));
            if (!posts[s].ids) goto fail;
            posts[s].n = 0;
            posts[s].cap = counts[s];
            n_list++;
            mem_list += (size_t)counts[s] * sizeof(eid_t);
        }
    }

    for (uint32_t i = 0; i < et->count; i++) {
        uint16_t e = et->ext_id[i];
        if (!e || (et->flags[i] & EF_DEAD)) continue;
        uint32_t h = (e * 2654435761u) & mask;
        while (ids[tab[h] - 1] != e) h = (h + 1) & mask;
        uint32_t s = tab[h] - 1;
        if (posts[s].ids) posts[s].ids[posts[s].n++] = i;   /* ascending: i ascends */
        else bs_set(&sets[s], i);
    }

    xi->n = n;
    xi->ids = ids;
    xi->counts = counts;
    xi->posts = posts;
    xi->sets = sets;
    xi->tab = tab;
    xi->tab_mask = mask;
    log_ext_cardinality(xi, et->count);
    log_ext_lengths(db, xi->n);
    char bmb[32], lmb[32];
    fmt_bytes(bmb, sizeof(bmb), mem_bitmap);
    fmt_bytes(lmb, sizeof(lmb), mem_list);
    /* The widest extension of each kind, named. Their cardinalities are what decide
     * whether the two paths are worth having, and naming them makes the measurement
     * re-runnable: sortcmp.sh and cmp_ref.sh both take a term, and "the widest extension"
     * is not a term anyone can paste. Both are reported because the risk is asymmetric --
     * a bitmap path that got slower is a regression, a list path that got slower only
     * shows up on the extensions closest to the threshold. */
    uint32_t widest_b = 0, widest_l = 0;
    int have_l = 0;
    for (uint32_t s = 0; s < n; s++) {
        if (posts[s].ids) {
            if (!have_l || counts[s] > counts[widest_l]) { widest_l = s; have_l = 1; }
        } else if (counts[s] > counts[widest_b]) {
            widest_b = s;
        }
    }
    LOGI("ext structure: %u bitmaps (%s), %u posting lists (%s) | a bitmap is kept"
         " above %u entries and only when it is at least %d bytes | widest bitmap: %s (%u),"
         " widest list: %s (%u)",
         n_bitmap, bmb, n_list, lmb, k_min, EXT_BITMAP_MIN_BYTES,
         n_bitmap ? ext_str(db, (uint16_t)ids[widest_b]) : "-",
         n_bitmap ? counts[widest_b] : 0,
         have_l ? ext_str(db, (uint16_t)ids[widest_l]) : "-",
         have_l ? counts[widest_l] : 0);
    return 0;

fail:
    for (uint32_t k = 0; k < n; k++) { free(posts[k].ids); bs_free(&sets[k]); }
    free(sets); free(posts); free(counts); free(tab); free(ids);
    return -1;
}

void ext_index_free(ext_index_t *xi)
{
    if (xi->posts) {
        for (uint32_t i = 0; i < xi->n; i++) free(xi->posts[i].ids);
        free(xi->posts);
    }
    if (xi->sets) {
        for (uint32_t i = 0; i < xi->n; i++) bs_free(&xi->sets[i]);
        free(xi->sets);
    }
    free(xi->ids);
    free(xi->counts);
    free(xi->tab);
    memset(xi, 0, sizeof(*xi));
}

static int ext_slot(const ext_index_t *xi, uint16_t ext_id)
{
    if (!xi->tab || !ext_id) return -1;
    uint32_t h = (ext_id * 2654435761u) & xi->tab_mask;
    while (xi->tab[h]) {
        if (xi->ids[xi->tab[h] - 1] == ext_id) return (int)(xi->tab[h] - 1);
        h = (h + 1) & xi->tab_mask;
    }
    return -1;
}

/* A bitmap is kept only when the extension is broad enough that *selecting* it as a
 * bitmap beats walking its ids -- see EXT_BITMAP_MIN_BYTES above. A list slot is filled
 * by walking the ids in order, so the list comes out ascending without being sorted,
 * which is the invariant every merge over it depends on (the same one tri_index has). */
uint32_t ext_index_select(const ext_index_t *xi, const uint16_t *ids, uint32_t n,
                          bitset_t *out)
{
    bs_clear(out);
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        int s = ext_slot(xi, ids[i]);
        if (s < 0) continue;             /* extension not present in this index */
        const eid_t *list = xi->posts[s].ids;
        if (list) {
            for (uint32_t k = 0; k < xi->posts[s].n; k++) bs_set(out, list[k]);
        } else {
            bs_or(out, &xi->sets[s]);
        }
        cnt++;
    }
    return cnt;
}

/* Add one entry to the set for `ext_id`, creating the slot if the extension is
 * new to this index. Mirrors ext_index_build()'s slot map -- the map, not a
 * parallel structure, is what keeps a lookup O(1). */
int ext_index_add(ext_index_t *xi, const esidx_t *db, uint16_t ext_id, eid_t id)
{
    if (!xi->tab || !ext_id) return 0;
    int slot = ext_slot(xi, ext_id);
    if (slot < 0) {
        /* a new extension: the table was sized for the entry count finalize saw */
        if ((xi->n + 1) * 4 >= (xi->tab_mask + 1) * 3) {
            uint32_t ncap = (xi->tab_mask + 1) * 2;
            uint32_t *nt = calloc(ncap, sizeof(uint32_t));
            if (!nt) return -1;
            free(xi->tab);
            xi->tab = nt;
            xi->tab_mask = ncap - 1;
            for (uint32_t s = 0; s < xi->n; s++) {
                uint32_t h = (xi->ids[s] * 2654435761u) & xi->tab_mask;
                while (xi->tab[h]) h = (h + 1) & xi->tab_mask;
                xi->tab[h] = s + 1;
            }
        }
        uint32_t *nids = realloc(xi->ids, (xi->n + 1) * sizeof(uint32_t));
        uint32_t *ncounts = realloc(xi->counts, (xi->n + 1) * sizeof(uint32_t));
        bitset_t *nsets = realloc(xi->sets, (xi->n + 1) * sizeof(bitset_t));
        ext_post_t *nposts = realloc(xi->posts, (xi->n + 1) * sizeof(ext_post_t));
        if (!nids || !ncounts || !nsets || !nposts) return -1;
        xi->ids = nids; xi->counts = ncounts; xi->sets = nsets; xi->posts = nposts;
        /* A new extension starts as a list: it has one entry, so a bitmap for it would be
         * a page of address space to hold a single bit. It graduates on the next build if
         * it ever earns one.
         *
         * Both halves of the new slot are zeroed, and that is load-bearing rather than
         * tidiness. realloc leaves the grown tail uninitialised, and these two arrays are
         * grown by realloc, so without this the slot's `nbits` is whatever was in that
         * heap block: bs_reserve() then reads it as "already big enough", allocates
         * nothing, and bs_test() dereferences an `w` that is not a pointer. -O2 hid it
         * completely -- fresh pages from the OS read as zero, so the garbage happened to
         * be a valid empty bitset -- and the DEBUG build caught it as a SEGV on
         * 0xbebebebe (AGENTS.md 3.1). The invariant from here on: a slot is a list iff
         * posts[slot].ids is non-NULL, and otherwise sets[slot] is a live bitmap. */
        memset(&xi->sets[xi->n], 0, sizeof(bitset_t));
        memset(&xi->posts[xi->n], 0, sizeof(ext_post_t));
        xi->ids[xi->n] = ext_id;
        xi->counts[xi->n] = 0;
        xi->n++;
        uint32_t h = (ext_id * 2654435761u) & xi->tab_mask;
        while (xi->tab[h]) h = (h + 1) & xi->tab_mask;
        xi->tab[h] = xi->n;
        slot = (int)xi->n - 1;
    }
    if (xi->posts[slot].ids) {
        ext_post_t *p = &xi->posts[slot];
        if (p->n == p->cap) {
            uint32_t ncap = p->cap ? p->cap * 2 : 8;
            eid_t *ni = realloc(p->ids, (size_t)ncap * sizeof(eid_t));
            if (!ni) return -1;
            p->ids = ni;
            p->cap = ncap;
        }
        p->ids[p->n++] = id;      /* ascending: a reconcile only ever hands out larger ids */
        xi->counts[slot]++;
        return 0;
    }
    if (bs_reserve(&xi->sets[slot], db->et.count) != 0) return -1;
    if (!bs_test(&xi->sets[slot], id)) xi->counts[slot]++;
    bs_set(&xi->sets[slot], id);
    return 0;
}

/* A list slot keeps the id of a removed entry. Nothing has to unpublish it: every query
 * seeds its candidate set from `live`, a removed id is not in it, and the intersection
 * that follows drops the id again. This is the bargain tri_index already makes, and the
 * reason a removal here is a count decrement and nothing else -- the count has to stay
 * exact because design §6.2 costs a driver leaf with it. Ids are never reused (D8), so a
 * stale id cannot be mistaken for a live one either; esidx_compact() rebuilds the lot. */
int ext_index_del(ext_index_t *xi, uint16_t ext_id, eid_t id)
{
    int slot = ext_slot(xi, ext_id);
    if (slot < 0) return 0;
    if (xi->posts[slot].ids) {
        if (xi->counts[slot]) xi->counts[slot]--;
        return 0;
    }
    if (bs_test(&xi->sets[slot], id)) {
        bs_clear_bit(&xi->sets[slot], id);
        xi->counts[slot]--;
    }
    return 0;
}

uint32_t ext_index_count(const ext_index_t *xi, uint16_t ext_id)
{
    int s = ext_slot(xi, ext_id);
    if (s < 0) return UINT32_MAX;
    return xi->counts[s];
}

/* --------------------------------------------------------------- database */

void esidx_init(esidx_t *db)
{
    memset(db, 0, sizeof(*db));
    db->di.ht_off = calloc(1024, sizeof(uint32_t));
    db->di.ht_val = calloc(1024, sizeof(eid_t));
    db->di.ht_mask = 1023;
    db->root_eid = EID_NONE;
}

void esidx_free(esidx_t *db)
{
    free(db->names.buf); free(db->exts.buf); free(db->dpaths.buf); free(db->ext_off);
    free(db->ext_tab);
    nm_tab_free(db);
    free(db->et.parent); free(db->et.depth); free(db->et.flags);
    free(db->et.size); free(db->et.mtime); free(db->et.ctime); free(db->et.stamp);
    free(db->et.ext_id); free(db->et.name); free(db->et.nchild);
    for (uint32_t i = 0; i < db->di.child_cap; i++) free(db->di.child[i].items);
    free(db->di.child);
    free(db->di.ord_slot);
    free(db->di.ord_eid);
    free(db->di.ht_off); free(db->di.ht_val);
    sidx_free(&db->by_size); sidx_free(&db->by_mtime); sidx_free(&db->by_ctime);
    ext_index_free(&db->ext);
    tri_index_free(&db->tri);
    esidx_free_name_rank(db);
    bs_free(&db->type.all); bs_free(&db->type.dirs); bs_free(&db->type.files);
    bs_free(&db->live);
    memset(db, 0, sizeof(*db));
}

/* (Re)allocate the dir path hash. Used by finalize for the batch build and by
 * di_hash_insert when a reconcile pushes the directory count past what the table
 * was sized for. */
static void di_hash_rehash(esidx_t *db, uint32_t ncap)
{
    uint32_t *off = calloc(ncap, sizeof(uint32_t));
    eid_t   *val = calloc(ncap, sizeof(eid_t));
    if (!off || !val) {
        LOGE("cannot allocate dir path hash (%u slots)", ncap);
        free(off); free(val);
        return;   /* the old table stays; a reconcile only degrades to a slower lookup */
    }
    free(db->di.ht_off); free(db->di.ht_val);
    db->di.ht_off = off;
    db->di.ht_val = val;
    db->di.ht_mask = ncap - 1;
    db->di.ht_count = 0;
}

static void di_hash_build(esidx_t *db)
{
    uint32_t ndirs = 0;
    for (uint32_t i = 0; i < db->et.count; i++)
        if ((db->et.flags[i] & EF_DIR) && !(db->et.flags[i] & EF_DEAD)) ndirs++;

    /* 2x the directory count, and the growth threshold in di_hash_insert() is 3/4
     * load, so this is the smallest power of two that cannot reach it before the
     * next doubling. It used to be 4x, which put /work at load factor 0.16 and 32
     * MiB of address space for 5 MiB of table; the comment above this function
     * said "2x entry count", which the code has never done. */
    uint32_t ncap = 1024;
    while (ncap < ndirs * 2) ncap *= 2;
    di_hash_rehash(db, ncap);
    if (!db->di.ht_off) return;

    uint32_t max_probe = 0;
    char *buf = malloc(65536);
    for (uint32_t i = 0; i < db->et.count; i++) {
        if (!(db->et.flags[i] & EF_DIR) || (db->et.flags[i] & EF_DEAD)) continue;
        path_of(db, i, buf, 65536);
        uint32_t h = hash_str(buf) & db->di.ht_mask;
        uint32_t probe = 0;
        while (db->di.ht_off[h] != 0) { h = (h + 1) & db->di.ht_mask; probe++; }
        if (probe > max_probe) max_probe = probe;
        uint32_t poff;
        if (sp_intern(&db->dpaths, buf, strlen(buf), &poff) != 0) { free(buf); return; }
        db->di.ht_off[h] = poff + 1;
        db->di.ht_val[h] = i;
        db->di.ht_count++;
    }
    free(buf);
    LOGD("dir path hash: %u dirs, %u slots, longest probe %u (load %.2f)",
         db->di.ht_count, ncap, max_probe,
         ncap ? (double)db->di.ht_count / (double)ncap : 0.0);
}

/* file: / folder: as whole-table bitmaps (design §2, L1). Two bitmaps rather
 * than a scan because the type filter is on every request the client makes. */
static void type_index_build(type_index_t *ti, esidx_t *db)
{
    bs_free(&ti->all); bs_free(&ti->dirs); bs_free(&ti->files);
    uint32_t n = db->et.count;
    if (!n) return;
    bs_init(&ti->all, n + 1);
    bs_init(&ti->dirs, n + 1);
    bs_init(&ti->files, n + 1);
    for (uint32_t i = 0; i < n; i++) {
        if (db->et.flags[i] & EF_DEAD) continue;
        bs_set(&ti->all, i);
        if (db->et.flags[i] & EF_DIR) bs_set(&ti->dirs, i);
        else                        bs_set(&ti->files, i);
    }
    LOGD("type bitmaps: %u dirs, %u files", bs_count(&ti->dirs), bs_count(&ti->files));
}

/* The live set is derived from the tombstone flag, which is what lets a snapshot
 * stay a plain dump of the columns: loading one rebuilds this exactly as a fresh
 * build does, and no reconciliation state has to be persisted. */
static void live_build(esidx_t *db)
{
    bs_free(&db->live);
    uint32_t n = db->et.count;
    if (bs_init(&db->live, n + 1) != 0) { LOGE("cannot allocate the live set"); return; }
    uint32_t live = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (db->et.flags[i] & EF_DEAD) continue;
        bs_set(&db->live, i);
        live++;
    }
    LOGD("live set: %u of %u ids", live, n);
}

void esidx_finalize(esidx_t *db)
{
    /* Two totals, not one: `t_all` is the phase, `t0` is the step inside it. The
     * per-step lines used to start their clock before live_build(), so the live set
     * -- a bitset over every id, which is a real cost on a multi-million-entry tree --
     * was silently attributed to the directory hash below it. */
    uint64_t t_all = ts_us(), t0 = ts_us();
    et_trim(&db->et);

    t0 = ts_us();
    live_build(db);
    TSDONE("finalize: live set", t0);

    t0 = ts_us();
    di_hash_build(db);
    TSDONE2("finalize: dir path hash", t0, "(dirs=%u table=%u slots)",
            db->di.ht_count, db->di.ht_mask + 1);

    t0 = ts_us();
    sidx_build(&db->by_size, db->et.size, db->et.count);
    TSDONE("finalize: sorted index size", t0);

    t0 = ts_us();
    sidx_build(&db->by_mtime, db->et.mtime, db->et.count);
    TSDONE("finalize: sorted index mtime", t0);

    /* dc: needs its own sorted array; without it the leaf could only be a scan,
     * and the client asks for DATE_CREATED on every search */
    t0 = ts_us();
    sidx_build(&db->by_ctime, db->et.ctime, db->et.count);
    TSDONE("finalize: sorted index ctime", t0);

    t0 = ts_us();
    ext_index_build(&db->ext, db);
    TSDONE2("finalize: ext sets", t0, "(%u extensions)", db->ext.n);

    t0 = ts_us();
    type_index_build(&db->type, db);
    TSDONE("finalize: type bitmaps", t0);

    t0 = ts_us();
    if (tri_index_build(&db->tri, db) != 0)
        LOGE("finalize: continuing without the name trigram index; text queries "
             "stay correct and stay slow");
    TSDONE2("finalize: name trigrams", t0, "(%u distinct, %u postings)",
            db->tri.n_slots, db->tri.n_postings);

    /* The name rank has to exist before a query can sort by name as an integer, and
     * esidx_build_name_rank() reports its own timing, so nothing is wrapped here. */
    if (esidx_build_name_rank(db) != 0)
        LOGE("finalize: continuing without the name rank; a name sort falls back "
             "to comparing folded names");

    TSDONE("finalize: total", t_all);

    /* From here on esidx_add() has to keep every one of the above in step. */
    db->built = true;
}

/* Peak resident set, in kB, from /proc/self/status. 0 if it cannot be read.
 * VmHWM rather than VmSize: an index reserves address space it never touches (the
 * ext bitmaps in design §5.4 are the extreme case -- 4.2 GB of address space that
 * measured 213 MB resident), so the size figure overstates the cost by more than an
 * order of magnitude on some structures and is the number that decides whether an
 * allocation will fail under a ulimit. Peak rather than current because the peak is
 * what a build has to fit, and because nothing here frees as it goes. */
static uint64_t vm_hwm_kb(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    uint64_t kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmHWM:", 6) == 0) {
            kb = strtoull(line + 6, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

void esidx_log_stats(const esidx_t *db, const char *phase)
{
    if (!log_enabled(LOG_INFO)) return;
    const entry_table_t *et = &db->et;
    uint64_t cols = (uint64_t)et->cap * (sizeof(eid_t) + 1 + 2 + 8 * 4 + 2 + sizeof(strref_t));
    LOGI("%s: entries=%u names_pool=%zu bytes ext_pool=%zu bytes columns~%llu bytes",
         phase, et->count, db->names.len, db->exts.len, (unsigned long long)cols);
    LOGI("%s: ext intern: %llu calls, %llu name compares (%.2f per call), "
         "%u distinct, table %u slots",
         phase, (unsigned long long)g_ext_calls, (unsigned long long)g_ext_compares,
         g_ext_calls ? (double)g_ext_compares / (double)g_ext_calls : 0.0,
         db->n_ext, db->ext_tab_mask + 1);
    if (db->built) {
        uint32_t live = bs_count(&db->live);
        LOGI("%s: live=%u tombstones=%u (%.1f%%) epoch=%llu",
             phase, live, et->count - live,
             et->count ? 100.0 * (double)(et->count - live) / (double)et->count : 0.0,
             (unsigned long long)db->epoch);
    }
    const scan_stats_t *st = esidx_scan_stats(db);
    if (st->entries)
        LOGI("%s: scan dirs=%llu entries=%llu files=%llu dirs_found=%llu depth_max=%llu stat_ok=%llu stat_fail=%llu open_fail=%llu getdents=%llu calls/%llu bytes",
             phase,
             (unsigned long long)st->dirs, (unsigned long long)st->entries,
             (unsigned long long)st->files, (unsigned long long)st->dirs_found,
             (unsigned long long)st->depth_max,
             (unsigned long long)st->stat_ok, (unsigned long long)st->stat_fail,
             (unsigned long long)st->open_fail,
             (unsigned long long)st->getdents_calls,
             (unsigned long long)st->getdents_bytes);

    uint64_t hwm = vm_hwm_kb();
    if (hwm)
        LOGI("%s: peak rss %.0f MiB", phase, (double)hwm / 1024.0);
    esidx_log_mem(db, phase);
}

/* Per-structure memory ledger.
 *
 * Written because "the index uses too much memory" is not an actionable statement:
 * every fix for it is a claim about *which* structure is oversized, and the three
 * biggest here are not the ones the design's structure list suggests. Two of them
 * are pure overhead -- `di.child` is indexed by entry id when 12 % of ids are
 * directories, and every array that grows by doubling is resident at its high-water
 * capacity -- so the interesting column is allocated-vs-used, not just total.
 *
* `touched` is what the structure has actually written -- what the process is
 * charged for -- and `address` is what it has reserved. The two are different
 * questions with different fixes: address space only becomes memory under a
 * refused overcommit or a `ulimit -v`, which is how design §5.4's 4.2 GB of
 * bitmaps failed while measuring 213 MB resident. Which of the two a row reports
 * is stated per row, because it depends on how that structure is grown: `realloc`
 * without a write past the old length leaves the tail untouched, `calloc` of a
 * large block returns zero pages nobody touches, and a `memset` of a new range
 * makes every byte of it resident.
 *
 * `*total` accumulates the touched column so the last line can be compared with
 * the process's own peak: the difference is the allocator's overhead plus whatever
 * scratch the build has already freed. */
static void mem_row(const char *phase, const char *what, uint64_t touched,
                    uint64_t address, uint64_t *total)
{
    if (total) *total += touched;
    LOGI("%s: mem %-22s %8.1f MiB touched %8.1f MiB addr  (%3.0f%%)",
         phase, what, (double)touched / 1048576.0, (double)address / 1048576.0,
         address ? 100.0 * (double)touched / (double)address : 0.0);
}

/* ------------------------------------------------ the trigram lists' shape
 *
 * This is 318.6 MiB on /work -- a third of everything the index accounts for, and the
 * largest single structure in it -- and until now the ledger said one thing about it: a
 * mean of 15.2 postings per entry. A mean cannot decide anything. The two fixes anyone
 * would reach for are a *stop list* (do not index the trigrams whose lists are so long
 * that filtering on them costs more than scanning) and *delta-varint postings* (ids
 * inside a list ascend, so most gaps are small), and they want opposite numbers: the
 * first wants to know how much of the volume sits in the long lists, the second wants to
 * know how small the gaps are.
 *
 * Both come out of one pass, and both are printed as the question rather than as a
 * verdict -- the rule log_ext_lengths() and log_ext_cardinality() already follow. The
 * bands straddle the thresholds a stop list would plausibly use (1 % of the table, then
 * 10x that), because a distribution picks the threshold and a single point cannot.
 *
 * The gap figures are a *sample*: the first 64 gaps of each list, which on /work is 4 M
 * of the 83 M postings. That is stated because it is a sample, and the front of a list is not an ordinary place --
 * a list is built in id order, but its ids are packed more tightly at the start of
 * leading gap is an ordinary one. Walking all 318 MiB to avoid the word would cost more
 * than every build that prints it.
 */
static void log_tri_shape(const esidx_t *db, const char *phase)
{
    enum { NB = 6 };
    static const uint32_t lo[NB] = { 1, 2, 16, 256, 4096, 65536 };
    static const char   *hi[NB] = { "=1", "2-15", "16-255", "256-4k", "4k-65k", ">=65k" };
    uint64_t lists[NB] = {0}, posts[NB] = {0};
    uint64_t nlists = 0, nposts = 0, gaps = 0, varint = 0, sampled = 0;

    for (uint32_t i = 0; i < db->tri.n_slots; i++) {
        const tri_list_t *l = &db->tri.list[i];
        if (!l->n) continue;
        uint32_t n = l->n;
        size_t b = n == 1 ? 0 : n < 16 ? 1 : n < 256 ? 2 : n < 4096 ? 3
                : n < 65536 ? 4 : 5;
        lists[b]++;
        posts[b] += n;
        nlists++;
        nposts += n;

        /* What the same ids would weigh as varint deltas: gaps inside one list ascend
         * by construction, so a gap is usually one byte.
         *
         * Two things about the sample, both of which were wrong in the first version and
         * are visible in the numbers rather than in the code:
         *
         *   - a *fixed* count per list makes the sample list-uniform, and the
         *     population is gap-uniform: 64 gaps out of a 100 000-entry list weigh the
         *     same as 64 out of a 10-entry one, so the mean came out as the average
         *     list's rather than the average gap's (2.2x sampled against 3.5x actual on
         *     /etc). So the sample is a fixed *fraction* of each list, and every list
         *     contributes in proportion to its length.
         *   - the run has to start somewhere other than the front: the ids in one list
         *     are packed more tightly at the start of the table than at the end. The
         *     offset is the slot index hashed (Fibonacci), so it is spread, free and
         *     deterministic -- and clamped so the run cannot read past the list, which
         *     the first version did and the sanitiser build caught. */
        if (n > 1) {
            uint32_t span = n - 1;
            uint32_t take = n / 64;
            if (take < 1) take = 1;
            if (take > span) take = span;
            uint32_t room = span - take;                 /* off + take <= span = n-1 */
            uint32_t off = room ? (uint32_t)(((uint64_t)i * 2654435761u) % (room + 1)) : 0;
            uint32_t prev = l->ids[off];
            for (uint32_t k = 1; k <= take; k++) {
                uint32_t d = l->ids[off + k] - prev;
                varint += d < (1u << 7) ? 1 : d < (1u << 14) ? 2 : d < (1u << 21) ? 3 : 4;
                gaps++;
                prev = l->ids[off + k];
            }
            sampled += take;
        }
    }

    char band_line[512] = {0};
    size_t at = 0;
    for (int i = 0; i < NB; i++)
        at += (size_t)snprintf(band_line + at, sizeof(band_line) - at, "%s%s=%llu/%llu",
                               i ? "  " : "", hi[i], (unsigned long long)lists[i],
                               (unsigned long long)posts[i]);
    LOGI("%s: mem trigram shape: %llu lists, %llu postings | by length (lists/postings): %s",
         phase, (unsigned long long)nlists, (unsigned long long)nposts, band_line);

    /* The two questions, answered rather than left to be derived: how much a stop list
     * at each threshold would remove, and what the postings weigh delta-encoded. */
    uint32_t cut1 = db->et.count / 100, cut2 = db->et.count / 10;
    uint64_t l1 = 0, p1 = 0, l2 = 0, p2 = 0;
    for (int i = 0; i < NB; i++) {
        if (lo[i] >= cut1) { l1 += lists[i]; p1 += posts[i]; }
        if (lo[i] >= cut2) { l2 += lists[i]; p2 += posts[i]; }
    }
    LOGI("%s: mem trigram stop list: lists >= 1%% of the table (%u ids): %llu lists "
         "holding %llu postings (%.0f%% of all) | >= 10%%: %llu lists, %llu postings "
         "(%.0f%%)", phase, cut1, (unsigned long long)l1, (unsigned long long)p1,
         nposts ? 100.0 * (double)p1 / (double)nposts : 0.0,
         (unsigned long long)l2, (unsigned long long)p2,
         nposts ? 100.0 * (double)p2 / (double)nposts : 0.0);

    /* Two numbers, both stated as what they are. The first is the measurement: the mean
     * encoded size of one gap, from the sample. The second is that mean applied to the
     * whole set, and it carries the one term a gap cannot: the first id in a list has no
     * predecessor to difference against, so it is stored whole.
     *
     * Both of these were wrong in the first version of this line, in ways only the
     * numbers showed: it compared a *sample's* varint total against the *whole* table's
     * 4-byte total and claimed 60x, which no encoding of a 4-byte id can reach; and it
     * counted that first id as one byte. Checked exactly on /etc (every gap, no sample)
     * the mean is 1.18 bytes against the sampled 1.32, so the sample is 12 % high on a
     * 15 % sample -- close enough to decide with, and it says so.
     */
    uint64_t gaps_all = nposts - nlists;
    double per_gap = gaps ? (double)varint / (double)gaps : 0.0;
    uint64_t enc_all = (uint64_t)(per_gap * (double)gaps_all) + nlists * sizeof(eid_t);
    LOGI("%s: mem trigram gaps: sampled %llu of %llu, %.2f bytes a gap against 4 for a "
         "raw id (%.2fx) | the whole set would be ~%llu bytes against %llu (%.2fx), "
         "including %llu first-of-list ids stored whole",
         phase, (unsigned long long)sampled, (unsigned long long)gaps_all, per_gap,
         per_gap ? 4.0 / per_gap : 0.0,
         (unsigned long long)enc_all, (unsigned long long)(nposts * sizeof(eid_t)),
         enc_all ? (double)(nposts * sizeof(eid_t)) / (double)enc_all : 0.0,
         (unsigned long long)nlists);
}

void esidx_log_mem(const esidx_t *db, const char *phase)
{
    if (!log_enabled(LOG_INFO)) return;
    const entry_table_t *et = &db->et;
    uint64_t total = 0;

    /* The columns, allocated at cap and holding count. sizeof each, so a
     * changed column cannot silently keep the old sum. */
    enum { COL_N = 10 };
    struct { const void *p; size_t sz; } col[COL_N] = {
        { et->parent, sizeof(eid_t)    }, { et->depth,  sizeof(uint16_t) },
        { et->flags,  sizeof(uint16_t) }, { et->size,   sizeof(int64_t)  },
        { et->mtime,  sizeof(int64_t)  }, { et->ctime,  sizeof(int64_t)  },
        { et->stamp,  sizeof(int64_t)  }, { et->ext_id, sizeof(uint16_t) },
        { et->name,   sizeof(strref_t) }, { et->nchild, sizeof(uint32_t) },
    };
    uint64_t col_bytes = 0, col_used = 0;
    for (int i = 0; i < COL_N; i++) {
        if (!col[i].p) continue;
        col_bytes += (uint64_t)et->cap * col[i].sz;
        col_used  += (uint64_t)et->count * col[i].sz;
    }
    mem_row(phase, "entry columns", col_used, col_bytes, &total);
    mem_row(phase, "names pool", db->names.len, db->names.cap, &total);
    if (db->nm_tab) {
        /* calloc'd, and only the occupied slots are ever written, so the address column
         * is what a `ulimit -v` would see while the touched column is what the process
         * is charged for. Four bytes against a name of ~20: the pool this table exists to
         * shrink was three and a half times larger. */
        mem_row(phase, "name intern table",
                (uint64_t)db->nm_count * sizeof(uint32_t),
                ((uint64_t)db->nm_mask + 1) * sizeof(uint32_t), &total);
        LOGI("%s: mem names: %u distinct names over %u entries (%.2f copies each, was "
             "one per entry), table %u slots",
             phase, db->nm_count, et->count,
             et->count ? (double)db->nm_count / (double)et->count : 0.0,
             db->nm_mask + 1);
    }

    /* The header array is indexed by directory ordinal, so its capacity tracks the
     * number of directories rather than the number of ids -- which is the whole
     * point of the change, and the row states both so the ratio stays checkable. */
    if (db->di.child) {
        uint32_t ndirs = db->di.ord_count;
        uint64_t hdr = (uint64_t)db->di.child_cap * sizeof(childvec_t);
        mem_row(phase, "dir vector headers", hdr, hdr, &total);
        LOGI("%s: mem dir headers: %u directories, %u slots (%.0f%%), eid->ordinal "
             "map %u slots", phase, ndirs, db->di.child_cap,
             db->di.child_cap ? 100.0 * (double)ndirs / (double)db->di.child_cap : 0.0,
             db->di.ord_mask + 1);

        uint64_t items_used = 0, items_bytes = 0;
        for (uint32_t d = 0; d < db->di.child_cap; d++) {
            items_used  += (uint64_t)db->di.child[d].n * sizeof(eid_t);
            items_bytes += (uint64_t)db->di.child[d].cap * sizeof(eid_t);
        }
        mem_row(phase, "dir children vectors", items_bytes, items_bytes, &total);
        mem_row(phase, "eid -> dir ordinal map",
                ((uint64_t)db->di.ord_mask + 1 + db->di.ord_count) * sizeof(uint32_t),
                ((uint64_t)db->di.ord_mask + 1 + db->di.ord_count) * sizeof(uint32_t),
                &total);
        LOGI("%s: mem dir children: %llu of %llu ids in vectors (%.0f%%)",
             phase, (unsigned long long)(items_used / 4),
             (unsigned long long)(items_bytes / 4),
             items_bytes ? 100.0 * (double)items_used / (double)items_bytes : 0.0);
    }
    if (db->di.ht_off) {
        uint64_t slots = (uint64_t)db->di.ht_mask + 1;
        /* Every occupied slot holds an offset into the name pool and the string
         * there is the directory's whole path -- a second copy of something the
         * parent chain already rebuilds. Counted exactly, by walking the strings
         * the table points at, because "how much does the dir tree cost twice" is
         * not a number worth estimating. It is part of the names pool above, so it
         * does not go in the total.
         *
         * HT_TOMB is a *slot* marker, not an offset: a reconcile's removal leaves it
         * behind (di_hash_erase, and why it does), and reading the pool at
         * 0xFFFFFFFF is what the first version of this row did. The sanitiser
         * build caught it and the incremental suite is what surfaced it -- the
         * update that hit a tombstone died, the snapshot was never rewritten, and
         * every assertion after it was a stale index. */
        uint64_t stored = 0;
        for (uint64_t s = 0; s < slots; s++)
            if (db->di.ht_off[s] && db->di.ht_off[s] != HT_TOMB)
                stored += strlen(sp_get(&db->dpaths, db->di.ht_off[s] - 1)) + 1;
        /* calloc: only the occupied slots are ever written, so the rest is address
         * space and not memory -- until something runs under a ulimit -v. */
        mem_row(phase, "dir path hash table", (uint64_t)db->di.ht_count * 8,
                slots * 8, &total);
        mem_row(phase, "dir paths copied to pool", stored, stored, NULL);
        LOGI("%s: mem dir hash load %.2f of %.0f slots for %u dirs",
             phase, (double)db->di.ht_count / (double)slots, (double)slots,
             db->di.ht_count);
    }

    /* Two arrays of {int64} and {eid_t}: 12 bytes a row with no padding, and the array
     * is written end to end so all of it is resident. It used to be one array of
     * {int64 v; eid_t id} -- 16 bytes of which 4 were padding, 62.7 MiB of it on /work,
     * which is why this row now reports what it holds rather than what it would cost as
     * two arrays. The delta rides along here too, so the number is the whole footprint. */
    const sidx_t *si[3] = { &db->by_size, &db->by_mtime, &db->by_ctime };
    uint64_t sidx_bytes = 0, sidx_rows = 0, sidx_delta = 0;
    for (int i = 0; i < 3; i++) {
        sidx_bytes  += ((uint64_t)si[i]->n + si[i]->dn) * (sizeof(int64_t) + sizeof(eid_t));
        sidx_rows   += si[i]->n;
        sidx_delta  += si[i]->dn;
    }
    mem_row(phase, "sorted arrays x3", sidx_bytes, sidx_bytes, &total);
    LOGI("%s: mem sorted arrays: %llu rows in %.1f MiB, %.1f bytes a row, %llu in the "
         "delta", phase, (unsigned long long)(sidx_rows + sidx_delta),
         (double)sidx_bytes / 1048576.0,
         (sidx_rows + sidx_delta) ? (double)sidx_bytes / (double)(sidx_rows + sidx_delta) : 0.0,
         (unsigned long long)sidx_delta);

    if (db->ext.n) {
        uint64_t set_bytes = 0, list_bytes = 0, list_used = 0;
        uint32_t nset = 0, nlist = 0;
        for (uint32_t i = 0; i < db->ext.n; i++) {
            if (db->ext.posts[i].ids) {
                nlist++;
                list_bytes += (uint64_t)db->ext.posts[i].cap * sizeof(eid_t);
                list_used  += (uint64_t)db->ext.posts[i].n * sizeof(eid_t);
            } else {
                nset++;
                set_bytes += (db->ext.sets[i].nbits + 7u) / 8u;
            }
        }
        uint64_t fixed = (uint64_t)db->ext.n * (sizeof(ext_post_t) + 2 * sizeof(uint32_t));
        if (db->ext.tab) fixed += ((uint64_t)db->ext.tab_mask + 1) * sizeof(uint32_t);
        mem_row(phase, "ext bitmaps", set_bytes, set_bytes + fixed, &total);
        mem_row(phase, "ext posting lists", list_bytes, list_bytes + fixed, NULL);
        LOGI("%s: mem ext structures: %u bitmaps, %u lists, tab %llu slots for %u "
             "extensions", phase, nset, nlist,
             (unsigned long long)(db->ext.tab ? (uint64_t)db->ext.tab_mask + 1 : 0),
             db->ext.n);
    }

    if (db->tri.n_slots) {
        uint64_t post_bytes = 0, post_used = 0;
        for (uint32_t i = 0; i < db->tri.n_slots; i++) {
            post_bytes += (uint64_t)db->tri.list[i].cap * sizeof(eid_t);
            post_used  += (uint64_t)db->tri.list[i].n * sizeof(eid_t);
        }
        uint64_t fixed = (uint64_t)db->tri.cap_slots *
                         (sizeof(tri_list_t) + sizeof(uint32_t));
        if (db->tri.tab) fixed += ((uint64_t)db->tri.tab_mask + 1) * sizeof(uint32_t);
        /* Resident at the capacity, not at the length: tri_index_add() grows a
         * posting list by doubling, and realloc copies what was there, so the
         * headroom between n and cap was written at some point and stays mapped.
         * A counting pass before the fill is what would make these two columns
         * agree -- the ext index above already does it, in the same function. */
        mem_row(phase, "name trigram lists", post_bytes, post_bytes + fixed, &total);
        LOGI("%s: mem trigram slots=%u of %u cap, %.1f postings/entry, %.0f%% of the "
             "list capacity in use", phase, db->tri.n_slots, db->tri.cap_slots,
             et->count ? (double)db->tri.n_postings / (double)et->count : 0.0,
             post_bytes ? 100.0 * (double)post_used / (double)post_bytes : 0.0);
        log_tri_shape(db, phase);
    }

    if (db->name_rank) {
        uint64_t bytes = (uint64_t)et->count * sizeof(uint32_t) + db->folded.cap +
                         (uint64_t)db->n_ranks * sizeof(uint32_t);
        uint64_t used  = (uint64_t)et->count * sizeof(uint32_t) + db->folded.len +
                         (uint64_t)db->n_ranks * sizeof(uint32_t);
        if (db->rk_tab) bytes += ((uint64_t)db->rk_mask + 1) * sizeof(uint32_t);
        mem_row(phase, "name rank", used, bytes, &total);
        LOGI("%s: mem name ranks: %u distinct folded names over %u entries",
             phase, db->n_ranks, et->count);
    }

    if (db->live.w)
        mem_row(phase, "live bitset", (uint64_t)db->live.nbits / 8u,
                (uint64_t)db->live.nbits / 8u, &total);
    if (db->type.dirs.w) {
        uint64_t b = (uint64_t)(db->type.all.nbits + 7u) / 8u * 3;
        mem_row(phase, "type bitmaps", b, b, &total);
    }

    LOGI("%s: mem %-22s %8.1f MiB accounted", phase, "TOTAL",
         (double)total / 1048576.0);
    uint64_t hwm = vm_hwm_kb() * 1024u;
    if (hwm)
        LOGI("%s: mem peak rss %.1f MiB -- the difference is the allocator's own "
             "overhead and the scratch a build has already freed (qsort's temp "
             "buffer, the rank arena)", phase, (double)hwm / 1048576.0);
}

/* --------------------------------------------------------------- snapshot */

#define ESIDX_MAGIC   "ESIDX1"
/* v2 adds the directory `stamp` column, which the reconcile compares against the
 * filesystem (ref A2). A v1 snapshot has no stamps, so every update would treat
 * every directory as changed -- correct, but it silently degrades the feature to
 * a full rescan, which is not something a version check should let through.
 *
 * v3 changes what an ext_id *is*: a dense 1-based index into the extension names
 * rather than a byte offset into their pool. The column is the same width and the
 * same place in the file, so a v2 snapshot would load without complaint and then
 * resolve extensions to the wrong strings -- the version check is the only thing
 * standing between those two, which is exactly what it is for.
 *
 * A child-count column was tried here as v4 and is deliberately *not* here. The
 * load path rebuilds the children vectors entry by entry, so it recomputes the
 * count for free -- which means persisting it buys nothing, and persisting it
 * *and* recomputing it is how the first version of this change answered
 * `child-count:2` for a directory with four children: the file said 4, the rebuild
 * added 4, and no test noticed because every child-count assertion read a freshly
 * built index. One structure, written by one function (D4). */
#define ESIDX_VERSION 3

static int w64(FILE *f, uint64_t v) { return fwrite(&v, 8, 1, f) == 1 ? 0 : -1; }
static int w32(FILE *f, uint32_t v) { return fwrite(&v, 4, 1, f) == 1 ? 0 : -1; }
static int wr(FILE *f, const void *p, size_t n) { return n == 0 || fwrite(p, n, 1, f) == 1 ? 0 : -1; }

int esidx_save(const esidx_t *db, const char *path)
{
    uint64_t t0 = ts_us();
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fwrite(ESIDX_MAGIC, 6, 1, f) != 1) { fclose(f); return -1; }
    if (w32(f, ESIDX_VERSION) != 0) { fclose(f); return -1; }

    w64(f, db->names.len); wr(f, db->names.buf, db->names.len);
    w64(f, db->exts.len);  wr(f, db->exts.buf,  db->exts.len);

    const entry_table_t *et = &db->et;
    w32(f, et->count);
    wr(f, et->parent, et->count * sizeof(eid_t));
    wr(f, et->depth,  et->count * sizeof(uint16_t));
    wr(f, et->flags,  et->count * sizeof(uint16_t));
    wr(f, et->size,   et->count * sizeof(int64_t));
    wr(f, et->mtime,  et->count * sizeof(int64_t));
    wr(f, et->ctime,  et->count * sizeof(int64_t));
    wr(f, et->stamp,  et->count * sizeof(int64_t));
    wr(f, et->ext_id, et->count * sizeof(uint16_t));

    for (uint32_t i = 0; i < et->count; i++) w32(f, et->name[i].off);
    for (uint32_t i = 0; i < et->count; i++) w32(f, et->name[i].len);

    fclose(f);
    TSDONE2("save", t0, "(%u entries -> %s)", et->count, path);
    return 0;
}

static int r64(FILE *f, uint64_t *v) { return fread(v, 8, 1, f) == 1 ? 0 : -1; }
static int r32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1 ? 0 : -1; }
static void *rd(FILE *f, size_t n)
{
    if (n == 0) return malloc(1);
    void *p = malloc(n);
    if (!p || fread(p, n, 1, f) != 1) { free(p); return NULL; }
    return p;
}

int esidx_load(esidx_t *db, const char *path)
{
    uint64_t t0 = ts_us();
    FILE *f = fopen(path, "rb");
    if (!f) { LOGE("cannot open %s", path); return -1; }
    char magic[6];
    if (fread(magic, 6, 1, f) != 1 || memcmp(magic, ESIDX_MAGIC, 6) != 0) {
        LOGE("%s: bad magic, not an esidx snapshot", path);
        fclose(f); return -1;
    }
    uint32_t ver;
    if (r32(f, &ver) != 0) { fclose(f); return -1; }
    if (ver != ESIDX_VERSION) {
        LOGE("%s: snapshot version %u, this build writes %u -- rebuild it"
             " (v1 has no directory stamps, so every refresh would rescan;"
             " v2 numbers extensions by pool offset, which wraps past a 64 KB"
             " pool and matches the wrong rows)",
             path, ver, ESIDX_VERSION);
        fclose(f);
        return -1;
    }

    uint64_t nlen, elen;
    if (r64(f, &nlen) != 0) { fclose(f); return -1; }
    db->names.buf = rd(f, nlen); db->names.len = nlen; db->names.cap = nlen;
    if (r64(f, &elen) != 0) { fclose(f); return -1; }
    db->exts.buf = rd(f, elen); db->exts.len = elen; db->exts.cap = elen;
    if (!db->names.buf || !db->exts.buf) { fclose(f); return -1; }

    entry_table_t *et = &db->et;
    if (r32(f, &et->count) != 0) { fclose(f); return -1; }
    et->cap = et->count;

    et->parent = rd(f, et->count * sizeof(eid_t));
    et->depth  = rd(f, et->count * sizeof(uint16_t));
    et->flags  = rd(f, et->count * sizeof(uint16_t));
    et->size   = rd(f, et->count * sizeof(int64_t));
    et->mtime  = rd(f, et->count * sizeof(int64_t));
    et->ctime  = rd(f, et->count * sizeof(int64_t));
    et->stamp  = rd(f, et->count * sizeof(int64_t));
    et->ext_id = rd(f, et->count * sizeof(uint16_t));
    if (!et->parent || !et->depth || !et->flags || !et->size ||
        !et->mtime || !et->ctime || !et->stamp || !et->ext_id) { fclose(f); return -1; }

    et->name = malloc(et->count * sizeof(strref_t) + 1);
    for (uint32_t i = 0; i < et->count; i++) r32(f, &et->name[i].off);
    for (uint32_t i = 0; i < et->count; i++) r32(f, &et->name[i].len);

    /* §5.5's aggregate, zeroed and left to the children rebuild below to fill --
     * the same loop that rebuilds the vectors increments it, so persisting it would
     * buy nothing and reading it *and* incrementing it would double every count. */
    et->nchild = calloc(et->count ? et->count : 1, sizeof(uint32_t));
    if (!et->nchild) { fclose(f); return -1; }
    fclose(f);

    /* Number the extension names. Not persisted, because walking the pool in order *is*
     * the numbering: a name's id is its position in the order it was interned, and the
     * pool is append-only, so the ids the entries carry are reproduced exactly. A load
     * therefore lands on the same ids the build did, which is what lets the ext column
     * stay 16 bits wide in the snapshot. */
    uint32_t next = 0;
    for (uint32_t off = 0; off < db->exts.len; ) {
        uint32_t len = (uint32_t)strlen(db->exts.buf + off) + 1;
        uint32_t *no = realloc(db->ext_off, (size_t)(next + 1) * sizeof(uint32_t));
        if (!no) { LOGE("load: cannot allocate the extension id table"); return -1; }
        db->ext_off = no;
        db->ext_off[next++] = off;
        off += len;
    }
    db->n_ext = next;
    db->ext_off_cap = next;

    /* The intern table for those ids, filled from the ids themselves -- there is no
     * other source for it, and it is not persisted (D4). Sized from the count, so the
     * rehashes the build paid on the way up are not paid again on the way in. */
    uint32_t ncap = EXT_TAB_MIN;
    while (ncap * 3 < db->n_ext * 4) ncap *= 2;
    if (ext_tab_fill(db, ncap) != 0) return -1;

    TSDONE2("load: read snapshot", t0, "(%u entries)", et->count);

    /* rebuild derived structures: children lists, path hash, sorted indexes */
    uint64_t t1 = ts_us();
    db->root_eid = EID_NONE;
    for (uint32_t i = 0; i < et->count; i++) {
        if (et->flags[i] & EF_DEAD) continue;   /* a tombstone is nobody's child */
        if (et->parent[i] == EID_NONE) db->root_eid = i;
        else di_add_child(db, et->parent[i], i);
    }
    TSDONE("load: rebuild children vectors", t1);

    esidx_finalize(db);
    TSDONE("load: total", t0);
    return 0;
}
