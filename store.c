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

uint32_t sp_intern(strpool_t *sp, const char *s, size_t n)
{
    if (sp->len + n + 1 > sp->cap) {
        size_t ncap = sp->cap ? sp->cap : 65536;
        while (sp->len + n + 1 > ncap) ncap *= 2;
        char *nb = realloc(sp->buf, ncap);
        if (!nb) return 0;
        sp->buf = nb;
        sp->cap = ncap;
    }
    uint32_t off = (uint32_t)sp->len;
    memcpy(sp->buf + off, s, n);
    sp->buf[off + n] = '\0';
    sp->len += n + 1;
    return off;
}

const char *sp_get(const strpool_t *sp, uint32_t off)
{
    return sp->buf + off;
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

static uint32_t hash_bytes(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

/* Rank of a folded name, interning it into `folded` if it is new.
 *
 * The table is hashed on the *content*, not on a pool offset, because sp_intern()
 * appends unconditionally and so hands two identical names two different offsets.
 * One strcmp per probe on a matching string is the price; hashing the offset would
 * have needed a deduplicating intern to be correct. */
static uint32_t rk_intern(esidx_t *db, const char *folded)
{
    uint32_t h = hash_bytes(folded) & db->rk_mask;
    while (db->rk_tab[h]) {
        uint32_t rank = db->rk_tab[h] - 1;
        if (!strcmp(sp_get(&db->folded, db->rk_off[rank]), folded)) return rank;
        h = (h + 1) & db->rk_mask;
    }
    uint32_t rank = db->n_ranks++;
    db->rk_off[rank] = sp_intern(&db->folded, folded, strlen(folded));
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

static int et_grow(entry_table_t *et, uint32_t ncap)
{
    eid_t    *p = realloc(et->parent, ncap * sizeof(eid_t));
    uint16_t *d = realloc(et->depth,  ncap * sizeof(uint16_t));
    uint16_t *f = realloc(et->flags,  ncap * sizeof(uint16_t));
    int64_t  *s = realloc(et->size,   ncap * sizeof(int64_t));
    int64_t  *m = realloc(et->mtime,  ncap * sizeof(int64_t));
    int64_t  *c = realloc(et->ctime,  ncap * sizeof(int64_t));
    int64_t  *k = realloc(et->stamp,  ncap * sizeof(int64_t));
    uint16_t *e = realloc(et->ext_id, ncap * sizeof(uint16_t));
    strref_t *r = realloc(et->name,   ncap * sizeof(strref_t));
    if (!p || !d || !f || !s || !m || !c || !k || !e || !r) return -1;
    et->parent = p; et->depth = d; et->flags = f; et->size = s;
    et->mtime = m;  et->ctime = c; et->stamp = k;
    et->ext_id = e; et->name = r;
    et->cap = ncap;
    return 0;
}

/* from the dir-tree section below */
int di_add_child(dir_index_t *di, eid_t dir, eid_t child);

/* The id of an extension name: dense, 1-based, 0 meaning "none". The name goes into the
 * pool and its *offset* goes into ext_off, so the id cannot outgrow 16 bits the way a
 * pool offset did -- see esidx_t for what that cost.
 *
 * The scan is linear over the interned names. That is the same complexity the pool scan
 * it replaced had, so it is not a regression, but on /work it is not free either: 4 217 609
 * calls over 6 508 names is ~13 G strcmp calls, which is a real share of a 100 s build.
 * A hash table on the name would make it O(1); it is not in this commit because the
 * addressing bug it shares a function with had to be fixed on its own. */
uint16_t ext_intern(esidx_t *db, const char *name)
{
    for (uint32_t i = 0; i < db->n_ext; i++)
        if (strcmp(sp_get(&db->exts, db->ext_off[i]), name) == 0)
            return (uint16_t)(i + 1);

    if (db->n_ext == UINT16_MAX - 1) {      /* 0 and the wrap guard are both reserved */
        LOGE("ext: more than %u distinct extensions; ids are 16-bit", UINT16_MAX - 1);
        return 0;
    }
    uint32_t noff = sp_intern(&db->exts, name, strlen(name));
    uint32_t cap = db->n_ext + 1;
    if (cap > db->ext_off_cap) {
        uint32_t ncap = db->ext_off_cap ? db->ext_off_cap * 2 : 256;
        uint32_t *no = realloc(db->ext_off, (size_t)ncap * sizeof(uint32_t));
        if (!no) { LOGE("ext: cannot grow the id table"); return 0; }
        db->ext_off = no;
        db->ext_off_cap = ncap;
    }
    db->ext_off[db->n_ext] = noff;
    db->n_ext++;
    return (uint16_t)db->n_ext;
}

/* The name behind an id, or "" for 0 and for anything out of range -- an id that does not
 * resolve must not read past the table, because that is how the wrap it replaced turned
 * into a wrong answer rather than a wrong count. */
const char *ext_str(const esidx_t *db, uint16_t ext_id)
{
    if (!ext_id || ext_id > db->n_ext) return "";
    return sp_get(&db->exts, db->ext_off[ext_id - 1]);
}

/* Extensions longer than EXT_MAX are cut to it, which loses the tail: two extensions
 * sharing their first EXT_MAX characters become one id, so `ext:` matches both, and a
 * query for the real one matches neither (the query side keeps 63, so the two ends do
 * not even agree on where to cut). ext_list_ids() caps at 63.
 *
 * Nothing counts how often that happens, so nothing knows whether it is a curiosity or
 * a live answer being wrong. Counted here, at the one place the cut happens, and printed
 * by log_ext_lengths() -- which is also why the pool's own histogram cannot answer the
 * question: a cut string is already cut by the time it is in the pool, so the 32+ bucket
 * is empty by construction rather than by evidence. */
#define EXT_MAX 32
static uint64_t g_ext_truncated;      /* entries whose extension was cut to EXT_MAX-1 */

static uint16_t ext_of(esidx_t *db, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0' || dot == name) return 0;
    char buf[EXT_MAX];
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
    if (et->count == et->cap) {
        if (et_grow(et, et->cap ? et->cap * 2 : 1024) != 0) return EID_NONE;
    }
    eid_t id = et->count++;
    size_t nlen = strlen(in->name);

    et->name[id].off  = sp_intern(&db->names, in->name, nlen);
    et->name[id].len  = (uint32_t)nlen;
    et->parent[id]    = parent;
    et->depth[id]     = in->depth;
    et->flags[id]     = in->flags;
    et->size[id]      = in->size;
    et->mtime[id]     = in->mtime;
    et->ctime[id]     = in->ctime;
    et->stamp[id]     = in->stamp;
    et->ext_id[id]    = (in->flags & EF_DIR) ? 0 : ext_of(db, in->name);

    if (parent == EID_NONE) db->root_eid = id;
    else if (di_add_child(&db->di, parent, id) != 0) return EID_NONE;

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
    if (p != EID_NONE) di_remove_child(&db->di, p, id);

    et->flags[id] |= EF_DEAD;
    bump_epoch(db);
}

int esidx_remove(esidx_t *db, eid_t id)
{
    entry_table_t *et = &db->et;
    if (id >= et->count || (et->flags[id] & EF_DEAD)) return 0;

    /* Children first, so the tree is never left with a live row whose parent is
     * a tombstone -- that combination is what path_of() would walk into. */
    while (id < db->di.child_cap && db->di.child[id].n) {
        childvec_t *cv = &db->di.child[id];
        eid_t kid = cv->items[cv->n - 1];
        cv->n--;
        esidx_remove(db, kid);
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

int di_add_child(dir_index_t *di, eid_t dir, eid_t child)
{
    if (dir >= di->child_cap) {
        uint32_t ncap = di->child_cap ? di->child_cap : 1024;
        while (dir >= ncap) ncap *= 2;
        childvec_t *nc = realloc(di->child, ncap * sizeof(childvec_t));
        if (!nc) return -1;
        memset(nc + di->child_cap, 0, (ncap - di->child_cap) * sizeof(childvec_t));
        di->child = nc;
        di->child_cap = ncap;
    }
    childvec_t *cv = &di->child[dir];
    if (cv->n == cv->cap) {
        uint32_t ncap = cv->cap ? cv->cap * 2 : 8;
        eid_t *ni = realloc(cv->items, ncap * sizeof(eid_t));
        if (!ni) return -1;
        cv->items = ni;
        cv->cap = ncap;
    }
    cv->items[cv->n++] = child;
    return 0;
}

/* Swap-remove rather than preserve order: nothing depends on the order, and a
 * removal from a directory holding thousands of entries must not be O(n) memmove
 * on top of the O(n) search. */
int di_remove_child(dir_index_t *di, eid_t dir, eid_t child)
{
    if (dir >= di->child_cap) return -1;
    childvec_t *cv = &di->child[dir];
    for (uint32_t i = 0; i < cv->n; i++) {
        if (cv->items[i] != child) continue;
        cv->items[i] = cv->items[cv->n - 1];
        cv->n--;
        return 0;
    }
    return -1;
}

/* One child's name, without materialising a path. The reconcile matches a
 * getdents entry against the stored children of the same directory on every
 * single entry, and path_of() there would be O(depth) per lookup. */
eid_t di_lookup_name(const esidx_t *db, eid_t dir, const char *name)
{
    if (dir >= db->di.child_cap) return EID_NONE;
    const childvec_t *cv = &db->di.child[dir];
    for (uint32_t i = 0; i < cv->n; i++) {
        eid_t id = cv->items[i];
        if (strcmp(name_of(db, id), name) == 0) return id;
    }
    return EID_NONE;
}

/* HT_TOMB marks a slot whose directory is gone. It cannot be 0 (an empty slot)
 * because a linear probe has to walk *through* a removed entry to reach the
 * entries behind it -- treating it as empty would make every lookup for a
 * directory that hashed after the removed one miss. */
#define HT_TOMB 0xFFFFFFFFu

eid_t di_lookup(const esidx_t *db, const char *path)
{
    const dir_index_t *di = &db->di;
    if (!di->ht_off) return EID_NONE;
    uint32_t i = hash_str(path) & di->ht_mask;
    while (di->ht_off[i] != 0) {
        if (di->ht_off[i] != HT_TOMB &&
            strcmp(sp_get(&db->names, di->ht_off[i]), path) == 0)
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
    uint32_t off = sp_intern(&db->names, buf, strlen(buf));

    uint32_t i = hash_str(sp_get(&db->names, off)) & di->ht_mask;
    uint32_t reuse = UINT32_MAX;
    while (di->ht_off[i] != 0) {
        if (di->ht_off[i] == HT_TOMB) {
            if (reuse == UINT32_MAX) reuse = i;
        } else if (strcmp(sp_get(&db->names, di->ht_off[i]), buf) == 0) {
            di->ht_val[i] = dir;      /* same path, new id */
            return 0;
        }
        i = (i + 1) & di->ht_mask;
    }
    if (reuse != UINT32_MAX) i = reuse;
    else di->ht_count++;
    di->ht_off[i] = off;
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
            strcmp(sp_get(&db->names, di->ht_off[i]), buf) == 0) {
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
    if (dir < db->di.child_cap) return db->di.child[dir].n;
    return 0;
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

static int cmp_sidx(const void *a, const void *b)
{
    const sidx_ent_t *x = a, *y = b;
    if (x->v < y->v) return -1;
    if (x->v > y->v) return 1;
    return (x->id < y->id) ? -1 : (x->id > y->id);
}

void sidx_build(sidx_t *s, const int64_t *vals, uint32_t n)
{
    s->a = malloc(n ? n * sizeof(sidx_ent_t) : 1);
    s->n = n; s->cap = n;
    for (uint32_t i = 0; i < n; i++) { s->a[i].v = vals[i]; s->a[i].id = i; }
    qsort(s->a, n, sizeof(sidx_ent_t), cmp_sidx);
}

/* D3's O(1) writer. `del` marks the entry as a retraction of `v` rather than an
 * assertion of it -- see SIDX_DEL. */
static void sidx_push(sidx_t *s, int64_t v, eid_t id, bool del)
{
    if (s->dn == s->dcap) {
        uint32_t ncap = s->dcap ? s->dcap * 2 : 256;
        sidx_ent_t *nb = realloc(s->delta, ncap * sizeof(sidx_ent_t));
        if (!nb) { LOGE("sidx: cannot grow the delta buffer"); return; }
        s->delta = nb;
        s->dcap = ncap;
    }
    s->delta[s->dn].v = v;
    s->delta[s->dn].id = del ? (id | SIDX_DEL) : id;
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
 * per id survives, so the delta is grouped by id first. */
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

int sidx_merge(sidx_t *s)
{
    if (!s->dn) return 0;

    dsort_t *d = malloc((size_t)s->dn * sizeof(dsort_t));
    bitset_t touched;
    if (!d) return -1;
    if (bs_init(&touched, s->n ? s->n : 1) != 0) { free(d); return -1; }
    for (uint32_t i = 0; i < s->dn; i++) {
        d[i].id = s->delta[i].id & ~SIDX_DEL;
        d[i].v = s->delta[i].v;
        d[i].pos = i;
    }
    qsort(d, s->dn, sizeof(dsort_t), cmp_dsort);

    uint32_t nnew = s->n;
    for (uint32_t i = 0; i < s->dn; ) {
        uint32_t j = i + 1;
        while (j < s->dn && d[j].id == d[i].id) j++;
        const dsort_t *last = &d[j - 1];
        bool drop = (last->id < s->n);
        bool keep = !(s->delta[last->pos].id & SIDX_DEL);
        if (drop) { bs_set(&touched, last->id); nnew--; }
        if (keep) nnew++;
        i = j;
    }

    sidx_ent_t *na = malloc((nnew ? nnew : 1) * sizeof(sidx_ent_t));
    if (!na) { free(d); bs_free(&touched); return -1; }
    uint32_t k = 0;
    for (uint32_t i = 0; i < s->n; i++)
        if (!bs_test(&touched, s->a[i].id)) na[k++] = s->a[i];
    for (uint32_t i = 0; i < s->dn; ) {
        uint32_t j = i + 1;
        while (j < s->dn && d[j].id == d[i].id) j++;
        const dsort_t *last = &d[j - 1];
        if (!(s->delta[last->pos].id & SIDX_DEL)) {
            na[k].v = last->v;
            na[k].id = last->id;
            k++;
        }
        i = j;
    }
    free(d);
    bs_free(&touched);
    qsort(na, k, sizeof(sidx_ent_t), cmp_sidx);

    free(s->a);
    free(s->delta);
    s->a = na;
    s->n = s->cap = k;
    s->delta = NULL;
    s->dn = s->dcap = 0;
    return 0;
}

void sidx_free(sidx_t *s)
{
    free(s->a); free(s->delta);
    s->a = s->delta = NULL; s->n = s->cap = s->dn = s->dcap = 0;
}

uint32_t sidx_range_to_bitset(const sidx_t *s, int64_t lo, int64_t hi, bitset_t *out)
{
    uint32_t cnt = 0;
    /* binary search first index with v >= lo */
    uint32_t a = 0, b = s->n;
    while (a < b) {
        uint32_t m = a + (b - a) / 2;
        if (s->a[m].v < lo) a = m + 1; else b = m;
    }
    for (uint32_t i = a; i < s->n && s->a[i].v <= hi; i++) {
        bs_set(out, s->a[i].id);
        cnt++;
    }
    /* The delta is walked in append order, which is chronological, so a
     * retraction always comes after the assertion it undoes. A retraction clears
     * unconditionally rather than only inside [lo,hi]: it retracts *that value*
     * for that id, and the main array set the bit because that value was in the
     * range -- which says nothing about whether the id's current value is. */
    for (uint32_t i = 0; i < s->dn; i++) {
        eid_t id = s->delta[i].id;
        if (id & SIDX_DEL) {
            id &= ~SIDX_DEL;
            if (bs_test(out, id)) { bs_clear_bit(out, id); cnt--; }
        } else if (s->delta[i].v >= lo && s->delta[i].v <= hi) {
            if (!bs_test(out, id)) cnt++;
            bs_set(out, id);
        }
    }
    return cnt;
}

/* -------------------------------------------------------------- ext bitmaps */

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
    /* Only entries added by this process are counted, so on a load this reads 0 -- the
     * pool was written by whoever built the snapshot, and the tail it lost is gone. */
    LOGI("ext lengths: %llu entries had an extension cut to %d characters by ext_of()"
         " | 0 here on a load: the cut already happened when the snapshot was written",
         (unsigned long long)g_ext_truncated, EXT_MAX - 1);

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

int ext_index_build(ext_index_t *xi, const esidx_t *db)
{
    ext_index_free(xi);
    const entry_table_t *et = &db->et;
    if (et->count == 0) return 0;

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

    bitset_t *sets = calloc(n ? n : 1, sizeof(bitset_t));
    uint32_t *counts = calloc(n ? n : 1, sizeof(uint32_t));
    if (!sets || !counts) {
        free(sets); free(counts); free(tab); free(ids);
        return -1;
    }
    for (uint32_t s = 0; s < n; s++) {
        if (bs_init(&sets[s], et->count + 1) != 0) {
            for (uint32_t k = 0; k < s; k++) bs_free(&sets[k]);
            free(sets); free(counts); free(tab); free(ids);
            return -1;
        }
    }
    for (uint32_t i = 0; i < et->count; i++) {
        uint16_t e = et->ext_id[i];
        if (!e || (et->flags[i] & EF_DEAD)) continue;
        uint32_t h = (e * 2654435761u) & mask;
        while (ids[tab[h] - 1] != e) h = (h + 1) & mask;
        bs_set(&sets[tab[h] - 1], i);
        counts[tab[h] - 1]++;
    }

    xi->n = n;
    xi->ids = ids;
    xi->counts = counts;
    xi->sets = sets;
    xi->tab = tab;
    xi->tab_mask = mask;
    log_ext_cardinality(xi, et->count);
    log_ext_lengths(db, xi->n);
    LOGD("ext bitmaps: %u distinct extensions over %u entries", n, et->count);
    return 0;
}

void ext_index_free(ext_index_t *xi)
{
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

uint32_t ext_index_select(const ext_index_t *xi, const uint16_t *ids, uint32_t n,
                          bitset_t *out)
{
    bs_clear(out);
    uint32_t cnt = 0;
    for (uint32_t i = 0; i < n; i++) {
        int s = ext_slot(xi, ids[i]);
        if (s < 0) continue;             /* extension not present in this index */
        bs_or(out, &xi->sets[s]);
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
        if (!nids || !ncounts || !nsets) return -1;
        xi->ids = nids; xi->counts = ncounts; xi->sets = nsets;
        if (bs_init(&xi->sets[xi->n], db->et.count + 1) != 0) return -1;
        xi->ids[xi->n] = ext_id;
        xi->counts[xi->n] = 0;
        xi->n++;
        uint32_t h = (ext_id * 2654435761u) & xi->tab_mask;
        while (xi->tab[h]) h = (h + 1) & xi->tab_mask;
        xi->tab[h] = xi->n;
        slot = (int)xi->n - 1;
    }
    if (bs_reserve(&xi->sets[slot], db->et.count) != 0) return -1;
    if (!bs_test(&xi->sets[slot], id)) xi->counts[slot]++;
    bs_set(&xi->sets[slot], id);
    return 0;
}

int ext_index_del(ext_index_t *xi, uint16_t ext_id, eid_t id)
{
    int slot = ext_slot(xi, ext_id);
    if (slot < 0) return 0;
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
    free(db->names.buf); free(db->exts.buf); free(db->ext_off);
    free(db->et.parent); free(db->et.depth); free(db->et.flags);
    free(db->et.size); free(db->et.mtime); free(db->et.ctime); free(db->et.stamp);
    free(db->et.ext_id); free(db->et.name);
    for (uint32_t i = 0; i < db->di.child_cap; i++) free(db->di.child[i].items);
    free(db->di.child);
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
    /* size table to ~2x entry count (dirs only) */
    uint32_t ndirs = 0;
    for (uint32_t i = 0; i < db->et.count; i++)
        if ((db->et.flags[i] & EF_DIR) && !(db->et.flags[i] & EF_DEAD)) ndirs++;

    uint32_t ncap = 1024;
    while (ncap < ndirs * 4) ncap *= 2;
    di_hash_rehash(db, ncap);
    if (!db->di.ht_off) return;

    uint32_t max_probe = 0;
    char *buf = malloc(65536);
    for (uint32_t i = 0; i < db->et.count; i++) {
        if (!(db->et.flags[i] & EF_DIR) || (db->et.flags[i] & EF_DEAD)) continue;
        path_of(db, i, buf, 65536);
        uint32_t off = sp_intern(&db->names, buf, strlen(buf));
        uint32_t h = hash_str(sp_get(&db->names, off)) & db->di.ht_mask;
        uint32_t probe = 0;
        while (db->di.ht_off[h] != 0) { h = (h + 1) & db->di.ht_mask; probe++; }
        if (probe > max_probe) max_probe = probe;
        db->di.ht_off[h] = off;
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
    uint64_t t0 = ts_us();
    live_build(db);
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
    TSDONE2("finalize: ext bitmaps", t0, "(%u extensions)", db->ext.n);

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

    /* From here on esidx_add() has to keep every one of the above in step. */
    db->built = true;
}

void esidx_log_stats(const esidx_t *db, const char *phase)
{
    if (!log_enabled(LOG_INFO)) return;
    const entry_table_t *et = &db->et;
    uint64_t cols = (uint64_t)et->cap * (sizeof(eid_t) + 1 + 2 + 8 * 4 + 2 + sizeof(strref_t));
    LOGI("%s: entries=%u names_pool=%zu bytes ext_pool=%zu bytes columns~%llu bytes",
         phase, et->count, db->names.len, db->exts.len, (unsigned long long)cols);
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
 * standing between those two, which is exactly what it is for. */
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

    TSDONE2("load: read snapshot", t0, "(%u entries)", et->count);

    /* rebuild derived structures: children lists, path hash, sorted indexes */
    uint64_t t1 = ts_us();
    db->root_eid = EID_NONE;
    for (uint32_t i = 0; i < et->count; i++) {
        if (et->flags[i] & EF_DEAD) continue;   /* a tombstone is nobody's child */
        if (et->parent[i] == EID_NONE) db->root_eid = i;
        else di_add_child(&db->di, et->parent[i], i);
    }
    TSDONE("load: rebuild children vectors", t1);

    esidx_finalize(db);
    TSDONE("load: total", t0);
    return 0;
}
