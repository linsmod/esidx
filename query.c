/* Query execution (§6.3).
 * P0 scope: parent:/file:/folder:/ext:/size:/dm: + sort + offset/count,
 * plus a fallback in-memory substring scan for bare words (§5.2, L2).
 *
 * Order of operations follows the design: candidate set from the directory
 * tree (§5.1, O(1)) -> bitmap intersection for scalar ranges (§5.4 + D3)
 * -> matcher pass -> multi-level sort chain (ref D3) -> slice.
 */

#include "esidx.h"
#include "timer.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

void query_init(query_t *q)
{
    memset(q, 0, sizeof(*q));
    q->parent    = -1;
    q->size_lo   = INT64_MIN;
    q->size_hi   = INT64_MAX;
    q->mtime_lo  = INT64_MIN;
    q->mtime_hi  = INT64_MAX;
    q->sort_key  = SORT_NAME;
    q->sort_desc = 0;
}

typedef struct {
    eid_t       id;
    const char *s;   /* name / ext / full path, depending on sort key */
    int64_t     v;   /* size / mtime */
} srec_t;

typedef struct {
    sort_key_t key;
    int        desc;
} sort_ctx_t;

static int cmp_rec(const void *pa, const void *pb, void *arg)
{
    const sort_ctx_t *c = arg;
    const srec_t *a = pa, *b = pb;
    int r = 0;

    switch (c->key) {
    case SORT_SIZE:
    case SORT_MTIME:
        r = (a->v < b->v) ? -1 : (a->v > b->v) ? 1 : 0;
        break;
    case SORT_EXT:
        r = strcasecmp(a->s ? a->s : "", b->s ? b->s : "");
        break;
    default:
        r = strcasecmp(a->s ? a->s : "", b->s ? b->s : "");
        break;
    }
    if (r == 0) r = (a->id < b->id) ? -1 : (a->id > b->id) ? 1 : 0;   /* stable */
    return c->desc ? -r : r;
}

static void fill_sort_key(const esidx_t *db, sort_key_t key, eid_t id, srec_t *r,
                          char *pathbuf, size_t pathsz)
{
    r->id = id;
    r->s  = NULL;
    r->v  = 0;

    switch (key) {
    case SORT_SIZE:  r->v = db->et.size[id];  break;
    case SORT_MTIME: r->v = db->et.mtime[id]; break;
    case SORT_EXT:
        r->s = db->et.ext_id[id] ? sp_get(&db->exts, db->et.ext_id[id] - 1) : "";
        break;
    case SORT_PATH:
        path_of(db, id, pathbuf, pathsz);
        r->s = pathbuf;   /* consumed immediately below */
        break;
    default:
        r->s = sp_get(&db->names, db->et.name[id].off);
        break;
    }
}

uint32_t query_exec(const esidx_t *db, const query_t *q, eid_t **out)
{
    *out = NULL;
    const uint32_t n = db->et.count;
    if (n == 0) return 0;

    uint64_t t_all = ts_us();

    const bool use_size  = (q->size_lo  != INT64_MIN || q->size_hi  != INT64_MAX);
    const bool use_mtime = (q->mtime_lo != INT64_MIN || q->mtime_hi != INT64_MAX);

    bitset_t bs_size = {0}, bs_mtime = {0};
    if (use_size) {
        if (bs_init(&bs_size, n) != 0) return 0;
        sidx_range_to_bitset(&db->by_size, q->size_lo, q->size_hi, &bs_size);
    }
    if (use_mtime) {
        if (bs_init(&bs_mtime, n) != 0) { bs_free(&bs_size); return 0; }
        sidx_range_to_bitset(&db->by_mtime, q->mtime_lo, q->mtime_hi, &bs_mtime);
    }
    uint64_t t_range = ts_us();

    /* ---- candidate set ---- */
    eid_t   *cand = NULL;
    uint32_t nc = 0;
    bool     borrowed = false;

    if (q->parent >= 0 && (uint32_t)q->parent < db->di.child_cap) {
        childvec_t *cv = &db->di.child[q->parent];
        cand = cv->items;
        nc = cv->n;
        borrowed = true;              /* §5.1: O(1), no scan */
    } else {
        cand = malloc(n * sizeof(eid_t));
        if (!cand) { bs_free(&bs_size); bs_free(&bs_mtime); return 0; }
        for (uint32_t i = 0; i < n; i++) cand[i] = i;
        nc = n;
    }
    uint64_t t_cand = ts_us();

    /* ---- matcher pass ---- */
    srec_t  *recs = malloc((nc ? nc : 1) * sizeof(srec_t));
    char    *pbuf = malloc(65536);
    uint32_t nr = 0;
    uint32_t n_dir = 0, n_file = 0;

    if (recs && pbuf) {
        for (uint32_t i = 0; i < nc; i++) {
            eid_t id = cand[i];
            if (id >= n) continue;

            uint16_t fl = db->et.flags[id];
            bool isdir = (fl & EF_DIR) != 0;
            if (q->type_filter == 1 && !isdir) continue;
            if (q->type_filter == 2 &&  isdir) continue;

            if (q->next && q->ext_ids) {
                bool ok = false;
                for (uint32_t k = 0; k < q->next; k++)
                    if (q->ext_ids[k] == db->et.ext_id[id]) { ok = true; break; }
                if (!ok) continue;
            }

            if (use_size  && !bs_test(&bs_size,  id)) continue;
            if (use_mtime && !bs_test(&bs_mtime, id)) continue;

            if (q->name_substr && q->name_substr[0]) {
                const char *nm = sp_get(&db->names, db->et.name[id].off);
                if (!strcasestr(nm, q->name_substr)) continue;
            }

            srec_t r;
            fill_sort_key(db, q->sort_key, id, &r, pbuf, 65536);
            if (q->sort_key == SORT_PATH) {
                /* path was written into the shared buffer -> own a copy */
                r.s = strdup(pbuf);
            }
            recs[nr++] = r;
            if (isdir) n_dir++; else n_file++;
        }
    }
    uint64_t t_match = ts_us();

    /* ---- sort ---- */
    sort_ctx_t ctx = { q->sort_key, q->sort_desc };
    if (nr > 1) qsort_r(recs, nr, sizeof(srec_t), cmp_rec, &ctx);
    uint64_t t_sort = ts_us();

    /* ---- slice ---- */
    uint32_t off = q->offset;
    if (off > nr) off = nr;
    uint32_t cnt = (q->count == 0) ? (nr - off) : q->count;
    if (cnt > nr - off) cnt = nr - off;

    eid_t *res = malloc((cnt ? cnt : 1) * sizeof(eid_t));
    if (res) {
        for (uint32_t i = 0; i < cnt; i++) res[i] = recs[off + i].id;
    }

    if (q->sort_key == SORT_PATH) {
        for (uint32_t i = 0; i < nr; i++) free((void *)recs[i].s);
    }
    free(recs);
    free(pbuf);
    if (!borrowed) free(cand);
    bs_free(&bs_size);
    bs_free(&bs_mtime);

    if (!res) return 0;
    *out = res;

    if (log_enabled(LOG_INFO)) {
        LOGI("query: range=%.3f cand=%.3f match=%.3f sort=%.3f total=%.3f ms",
             (double)(t_range - t_all) / 1000.0,
             (double)(t_cand - t_range) / 1000.0,
             (double)(t_match - t_cand) / 1000.0,
             (double)(t_sort - t_match) / 1000.0,
             (double)(ts_us() - t_all) / 1000.0);
        LOGI("query: candidates=%u matched=%u (dirs=%u files=%u) source=%s offset=%u count=%u returned=%u",
             nc, nr, n_dir, n_file, borrowed ? "dir children (O(1))" : "full table (O(n))",
             q->offset, q->count, cnt);
        if (nc && nr == nc && !borrowed)
            LOGD("query: every candidate matched -> a driver index would prune this faster");
        if (!borrowed && nc > 100000)
            LOGD("query: scanned %u entries without a driver index (parent: gives O(1))", nc);
    }
    return cnt;
}
