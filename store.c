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

/* ------------------------------------------------------------ entry table */

static int et_grow(entry_table_t *et, uint32_t ncap)
{
    eid_t    *p = realloc(et->parent, ncap * sizeof(eid_t));
    uint16_t *d = realloc(et->depth,  ncap * sizeof(uint16_t));
    uint16_t *f = realloc(et->flags,  ncap * sizeof(uint16_t));
    int64_t  *s = realloc(et->size,   ncap * sizeof(int64_t));
    int64_t  *m = realloc(et->mtime,  ncap * sizeof(int64_t));
    int64_t  *c = realloc(et->ctime,  ncap * sizeof(int64_t));
    uint16_t *e = realloc(et->ext_id, ncap * sizeof(uint16_t));
    strref_t *r = realloc(et->name,   ncap * sizeof(strref_t));
    if (!p || !d || !f || !s || !m || !c || !e || !r) return -1;
    et->parent = p; et->depth = d; et->flags = f; et->size = s;
    et->mtime = m;  et->ctime = c; et->ext_id = e; et->name = r;
    et->cap = ncap;
    return 0;
}

/* from the dir-tree section below */
int di_add_child(dir_index_t *di, eid_t dir, eid_t child);

uint16_t ext_intern(esidx_t *db, const char *name)
{
    /* extensions are few; linear scan is fine */
    const strpool_t *sp = &db->exts;
    uint32_t off = 0;
    while (off < sp->len) {
        const char *s = sp->buf + off;
        if (strcmp(s, name) == 0) return (uint16_t)(off + 1);
        off += (uint32_t)strlen(s) + 1;
    }
    uint32_t noff = sp_intern(&db->exts, name, strlen(name));
    return (uint16_t)(noff + 1);   /* 0 reserved for "none" */
}

static uint16_t ext_of(esidx_t *db, const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot[1] == '\0' || dot == name) return 0;
    char buf[32];
    size_t n = strlen(dot + 1);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)dot[1 + i]);
    buf[n] = '\0';
    return ext_intern(db, buf);
}

eid_t esidx_add(esidx_t *db, eid_t parent, const char *name, uint16_t depth,
                uint16_t flags, int64_t size, int64_t mtime, int64_t ctime)
{
    entry_table_t *et = &db->et;
    if (et->count == et->cap) {
        if (et_grow(et, et->cap ? et->cap * 2 : 1024) != 0) return EID_NONE;
    }
    eid_t id = et->count++;
    size_t nlen = strlen(name);

    et->name[id].off  = sp_intern(&db->names, name, nlen);
    et->name[id].len  = (uint32_t)nlen;
    et->parent[id]    = parent;
    et->depth[id]     = depth;
    et->flags[id]     = flags;
    et->size[id]      = size;
    et->mtime[id]     = mtime;
    et->ctime[id]     = ctime;
    et->ext_id[id]    = (flags & EF_DIR) ? 0 : ext_of(db, name);

    if (parent == EID_NONE) db->root_eid = id;
    if (parent != EID_NONE) di_add_child(&db->di, parent, id);
    return id;
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

eid_t di_lookup(const esidx_t *db, const char *path)
{
    const dir_index_t *di = &db->di;
    if (!di->ht_off) return EID_NONE;
    uint32_t i = hash_str(path) & di->ht_mask;
    while (di->ht_off[i] != 0) {
        if (strcmp(sp_get(&db->names, di->ht_off[i]), path) == 0) return di->ht_val[i];
        i = (i + 1) & di->ht_mask;
    }
    return EID_NONE;
}

const char *name_of(const esidx_t *db, eid_t id)
{
    if (id >= db->et.count) return "";
    return sp_get(&db->names, db->et.name[id].off);
}

const char *ext_of_str(const esidx_t *db, eid_t id)
{
    if (id >= db->et.count) return "";
    uint16_t e = db->et.ext_id[id];
    return e ? sp_get(&db->exts, e - 1) : "";
}

uint32_t di_child_count(const esidx_t *db, eid_t dir)
{
    if (dir < db->di.child_cap) return db->di.child[dir].n;
    return 0;
}

void parent_path_of(const esidx_t *db, eid_t id, char *out, size_t outsz)
{
    if (id >= db->et.count || !outsz) { if (outsz) out[0] = '\0'; return; }
    eid_t p = db->et.parent[id];
    if (p == EID_NONE || p >= db->et.count) { out[0] = '\0'; return; }
    path_of(db, p, out, outsz);
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
    /* delta (decision D3): linear scan, unsorted */
    for (uint32_t i = 0; i < s->dn; i++) {
        if (s->delta[i].v >= lo && s->delta[i].v <= hi) { bs_set(out, s->delta[i].id); cnt++; }
    }
    return cnt;
}

/* -------------------------------------------------------------- ext bitmaps */

/* ext_id is the byte offset in the exts pool plus one, so it is sparse. `tab` is
 * a small open-addressed map from ext_id to a dense slot; linear probing is fine
 * at a load factor of 0.5 (ref A12 considered and rejected for the trigram table,
 * where the probe distribution matters; here it does not). */
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
        if (!e) continue;
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
        if (bs_init(&sets[s], et->count) != 0) {
            for (uint32_t k = 0; k < s; k++) bs_free(&sets[k]);
            free(sets); free(counts); free(tab); free(ids);
            return -1;
        }
    }
    for (uint32_t i = 0; i < et->count; i++) {
        uint16_t e = et->ext_id[i];
        if (!e) continue;
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
    free(db->names.buf); free(db->exts.buf);
    free(db->et.parent); free(db->et.depth); free(db->et.flags);
    free(db->et.size); free(db->et.mtime); free(db->et.ctime);
    free(db->et.ext_id); free(db->et.name);
    for (uint32_t i = 0; i < db->di.child_cap; i++) free(db->di.child[i].items);
    free(db->di.child);
    free(db->di.ht_off); free(db->di.ht_val);
    sidx_free(&db->by_size); sidx_free(&db->by_mtime); sidx_free(&db->by_ctime);
    ext_index_free(&db->ext);
    bs_free(&db->type.all); bs_free(&db->type.dirs); bs_free(&db->type.files);
    memset(db, 0, sizeof(*db));
}

static void di_hash_build(esidx_t *db)
{
    /* size table to ~2x entry count (dirs only) */
    uint32_t ndirs = 0;
    for (uint32_t i = 0; i < db->et.count; i++)
        if (db->et.flags[i] & EF_DIR) ndirs++;

    uint32_t ncap = 1024;
    while (ncap < ndirs * 4) ncap *= 2;
    free(db->di.ht_off); free(db->di.ht_val);
    db->di.ht_off = calloc(ncap, sizeof(uint32_t));
    db->di.ht_val = calloc(ncap, sizeof(eid_t));
    db->di.ht_mask = ncap - 1;
    db->di.ht_count = 0;
    if (!db->di.ht_off || !db->di.ht_val) {
        LOGE("cannot allocate dir path hash (%u slots)", ncap);
        return;
    }

    uint32_t max_probe = 0;
    char *buf = malloc(65536);
    for (uint32_t i = 0; i < db->et.count; i++) {
        if (!(db->et.flags[i] & EF_DIR)) continue;
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
    bs_init(&ti->all, n);
    bs_init(&ti->dirs, n);
    bs_init(&ti->files, n);
    for (uint32_t i = 0; i < n; i++) {
        bs_set(&ti->all, i);
        if (db->et.flags[i] & EF_DIR) bs_set(&ti->dirs, i);
        else                        bs_set(&ti->files, i);
    }
    LOGD("type bitmaps: %u dirs, %u files", bs_count(&ti->dirs), bs_count(&ti->files));
}

void esidx_finalize(esidx_t *db)
{
    uint64_t t0 = ts_us();
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
}

void esidx_log_stats(const esidx_t *db, const char *phase)
{
    if (!log_enabled(LOG_INFO)) return;
    const entry_table_t *et = &db->et;
    uint64_t cols = (uint64_t)et->cap * (sizeof(eid_t) + 1 + 2 + 8 * 3 + 2 + sizeof(strref_t));
    LOGI("%s: entries=%u names_pool=%zu bytes ext_pool=%zu bytes columns~%llu bytes",
         phase, et->count, db->names.len, db->exts.len, (unsigned long long)cols);
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
#define ESIDX_VERSION 1

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
    if (r32(f, &ver) != 0 || ver != ESIDX_VERSION) {
        LOGE("%s: unsupported version %u (expected %u)", path, ver, ESIDX_VERSION);
        fclose(f); return -1;
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
    et->ext_id = rd(f, et->count * sizeof(uint16_t));
    if (!et->parent || !et->depth || !et->flags || !et->size ||
        !et->mtime || !et->ctime || !et->ext_id) { fclose(f); return -1; }

    et->name = malloc(et->count * sizeof(strref_t) + 1);
    for (uint32_t i = 0; i < et->count; i++) r32(f, &et->name[i].off);
    for (uint32_t i = 0; i < et->count; i++) r32(f, &et->name[i].len);
    fclose(f);

    TSDONE2("load: read snapshot", t0, "(%u entries)", et->count);

    /* rebuild derived structures: children lists, path hash, sorted indexes */
    uint64_t t1 = ts_us();
    db->root_eid = EID_NONE;
    for (uint32_t i = 0; i < et->count; i++) {
        if (et->parent[i] == EID_NONE) db->root_eid = i;
        else di_add_child(&db->di, et->parent[i], i);
    }
    TSDONE("load: rebuild children vectors", t1);

    esidx_finalize(db);
    TSDONE("load: total", t0);
    return 0;
}
