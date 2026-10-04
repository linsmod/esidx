/* trigram.c -- the name trigram index of design §5.2.
 *
 * What this is for: a text leaf walks its candidate set and calls text_match() on
 * every row, which is the largest remaining cost in a real query (`path:/usr
 * *.conf size:>1k` spends 56 ms of a 57.7 ms query in eval on the r7000 host).
 * The name half of the fix is to intersect the candidate set with the posting
 * list of one trigram the pattern cannot match without, so the matcher runs on a
 * few hundred rows instead of a few hundred thousand.
 *
 * What this is not: a decision. Every trigram of a pattern's literal run must
 * occur in the name for the pattern to match at all, so the intersection is a
 * necessary condition and text_match() still decides every row that survives.
 * That is the property the whole design rests on, and it is why the caller in
 * query.c can switch this on for one query shape and leave it off for another.
 *
 * Deliberately *not* here, and why (design §5.2 lists the format, D4 says why the
 * format is not the one plocate uses):
 *   - block compression, PForDelta postings, a zstd dictionary. Those belong to a
 *     persisted index; this one is resident, so there is nothing to decompress on
 *     the query path and plain ascending arrays are the cheapest representation.
 *   - CRoaring. A posting list is an ascending id array, and the sets here are
 *     intersected by a merge, which a roaring bitmap does not accelerate.
 *   - the path index. `path:` reads a path rebuilt from the parent chain
 *     (design §12 risk 7), so it is a second index over materialised paths and a
 *     separate decision.
 *
 * Invariants worth stating, because both are load-bearing:
 *   - a posting list is strictly ascending, and holds an id at most once. Ascend-
 *     ing holds because finalize walks ids in order and esidx_add only ever hands
 *     out a larger one (D8); at-most-once because tri_index_add() drops a trigram
 *     a name already contributed. The filter's merge depends on both.
 *   - the index is keyed on ASCII-lowercased bytes and nothing else. wc_eq() folds
 *     case with tolower() in the C locale, which is ASCII-only, so folding a
 *     non-ASCII byte here would desynchronise the filter from the matcher it
 *     feeds: it would drop rows the matcher accepts.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esidx.h"
#include "timer.h"

#define TRI_TAB_MIN   1024u
/* NAME_MAX is 255 on ext4, so a name cannot yield more than 253 trigrams and this
 * holds them all. The overflow path below posts a duplicate rather than skipping
 * the trigram, because a dropped trigram loses a row and a duplicate cannot. */
#define TRI_SEEN_MAX  256u

/* --------------------------------------------------------------- the keys */

/* ASCII-only on purpose: see the invariant note above. */
static inline uint32_t tri_fold(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (uint32_t)(c - 'A' + 'a') : (uint32_t)c;
}

static inline uint32_t tri_key(const char *s, size_t i)
{
    return (tri_fold((unsigned char)s[i]) << 16) |
           (tri_fold((unsigned char)s[i + 1]) << 8) |
            tri_fold((unsigned char)s[i + 2]);
}

/* Fibonacci hashing on the *high* bits. The low bits of `key * C mod 2^32` depend
 * only on the low bits of `key` -- the multiplier is odd, so it cannot move
 * information downwards -- and a 24-bit trigram whose low 11 bits collide would
 * put every one of them in one probe chain. So the mask is applied to the top of
 * the product, which is what makes `tab_shift` worth carrying next to `tab_mask`. */
static inline uint32_t tri_hash(const tri_index_t *ti, uint32_t key)
{
    return (key * 2654435761u) >> ti->tab_shift;
}

static uint32_t tri_shift_for(uint32_t ncap)
{
    uint32_t s = 0;
    while ((1u << s) < ncap) s++;
    return 32u - s;
}

/* ------------------------------------------------------------ slot lookup */

static int tri_find(const tri_index_t *ti, uint32_t key)
{
    if (!ti->tab) return -1;
    uint32_t h = tri_hash(ti, key);
    for (;;) {
        uint32_t v = ti->tab[h];
        if (!v) return -1;
        if (ti->key[v - 1] == key) return (int)(v - 1);
        h = (h + 1) & ti->tab_mask;
    }
}

/* Find or create the slot for `key`. -1 only on allocation failure. */
static int tri_intern(tri_index_t *ti, uint32_t key)
{
    if (!ti->tab) {
        ti->tab = calloc(TRI_TAB_MIN, sizeof(uint32_t));
        if (!ti->tab) return -1;
        ti->tab_mask = TRI_TAB_MIN - 1;
        ti->tab_shift = tri_shift_for(TRI_TAB_MIN);
    }

    uint32_t h = tri_hash(ti, key);
    for (;;) {
        uint32_t v = ti->tab[h];
        if (!v) break;
        if (ti->key[v - 1] == key) return (int)(v - 1);
        h = (h + 1) & ti->tab_mask;
    }

    /* `list` is indexed by the same slot as `key`, so one growth covers both and
     * the new slot has to start as an empty posting list. */
    if (ti->n_slots == ti->cap_slots) {
        uint32_t ncap = ti->cap_slots ? ti->cap_slots * 2 : TRI_TAB_MIN;
        uint32_t *nk = realloc(ti->key, ncap * sizeof(uint32_t));
        if (!nk) return -1;
        tri_list_t *nl = realloc(ti->list, ncap * sizeof(tri_list_t));
        if (!nl) return -1;          /* ti->key grew; tri_index_free() releases both */
        memset(nl + ti->n_slots, 0, (ncap - ti->n_slots) * sizeof(tri_list_t));
        ti->key = nk; ti->list = nl; ti->cap_slots = ncap;
    }

    /* Keep the chains short rather than Robin Hood (ref A12, deliberately not
     * implemented -- see design §5.1 on why the dir hash measures instead). */
    if ((ti->n_slots + 1) * 4 >= (ti->tab_mask + 1) * 3) {
        uint32_t ncap = (ti->tab_mask + 1) * 2;
        uint32_t *nt = calloc(ncap, sizeof(uint32_t));
        if (!nt) return -1;
        free(ti->tab);
        ti->tab = nt;
        ti->tab_mask = ncap - 1;
        ti->tab_shift = tri_shift_for(ncap);
        for (uint32_t s = 0; s < ti->n_slots; s++) {
            uint32_t p = tri_hash(ti, ti->key[s]);
            while (ti->tab[p]) p = (p + 1) & ti->tab_mask;
            ti->tab[p] = s + 1;
        }
    }

    h = tri_hash(ti, key);
    while (ti->tab[h]) h = (h + 1) & ti->tab_mask;
    ti->key[ti->n_slots] = key;
    ti->tab[h] = ti->n_slots + 1;
    return (int)ti->n_slots++;
}

static int tri_post(tri_index_t *ti, uint32_t slot, eid_t id)
{
    tri_list_t *l = &ti->list[slot];
    if (l->n == l->cap) {
        uint32_t ncap = l->cap ? l->cap * 2 : 4;
        eid_t *ni = realloc(l->ids, ncap * sizeof(eid_t));
        if (!ni) return -1;
        l->ids = ni; l->cap = ncap;
    }
    l->ids[l->n++] = id;
    ti->n_postings++;
    return 0;
}

/* ------------------------------------------------------------------ build */

int tri_index_add(tri_index_t *ti, const char *name, eid_t id)
{
    uint32_t seen[TRI_SEEN_MAX];
    uint32_t nseen = 0;
    size_t len = strlen(name);
    if (len < 3) return 0;              /* no trigram, ref A9 */

    for (size_t i = 0; i + 3 <= len; i++) {
        uint32_t k = tri_key(name, i);
        if (nseen < TRI_SEEN_MAX) {
            uint32_t j = 0;
            while (j < nseen && seen[j] != k) j++;
            if (j < nseen) continue;   /* this name already contributed k */
            seen[nseen++] = k;
        }
        int slot = tri_intern(ti, k);
        if (slot < 0) return -1;
        if (tri_post(ti, (uint32_t)slot, id) != 0) return -1;
    }
    return 0;
}

int tri_index_build(tri_index_t *ti, const esidx_t *db)
{
    tri_index_free(ti);
    if (db->et.count == 0) return 0;

    uint32_t n = db->et.count;
    for (uint32_t i = 0; i < n; i++) {
        if (db->et.flags[i] & EF_DEAD) continue;
        /* display_name_of(), not name_of(): the root's stored name is the absolute
         * path it was indexed from, and the matcher reads the display name
         * (design §12.12). Indexing the stored name would put trigrams in the
         * index that no query ever asks for. */
        if (tri_index_add(ti, display_name_of(db, i), i) != 0) {
            LOGE("cannot build the name trigram index at entry %u", i);
            tri_index_free(ti);
            return -1;
        }
    }
    LOGD("name trigrams: %u distinct over %u live names, %u postings",
         ti->n_slots, n, ti->n_postings);
    return 0;
}

void tri_index_free(tri_index_t *ti)
{
    if (ti->list) {
        for (uint32_t i = 0; i < ti->n_slots; i++) free(ti->list[i].ids);
        free(ti->list);
    }
    free(ti->key);
    free(ti->tab);
    memset(ti, 0, sizeof(*ti));
}

/* ----------------------------------------------------------------- filter */

bool tri_index_filter(const tri_index_t *ti, const char *lit, bitset_t *out)
{
    if (!ti->tab || !out->w) return false;
    size_t len = strlen(lit);
    if (len < 3) return false;

    /* Any one trigram of the literal is a necessary condition, so the shortest
     * posting list is both the smallest answer and the cheapest to walk -- which
     * is why this picks one rather than intersecting them all. */
    const tri_list_t *best = NULL;
    for (size_t i = 0; i + 3 <= len; i++) {
        int slot = tri_find(ti, tri_key(lit, i));
        if (slot < 0 || ti->list[slot].n == 0) {
            /* Nothing in this index holds that trigram, so nothing holds the
             * literal. That is a narrowing all the way to zero, not a failure to
             * narrow -- and it is the shape that answers a query for a word that
             * has never been typed before without touching a single row. */
            bs_clear(out);
            return true;
        }
        const tri_list_t *l = &ti->list[slot];
        if (!best || l->n < best->n) best = l;
    }
    if (!best) return false;

    /* Both sides are ascending, so this is a merge: walk the set, advance a
     * cursor into the posting list, clear whatever the cursor passed. */
    uint32_t k = 0;
    for (uint32_t i = bs_next(out, 0); i < out->nbits; i = bs_next(out, i + 1)) {
        while (k < best->n && best->ids[k] < i) k++;
        if (k >= best->n || best->ids[k] != i) bs_clear_bit(out, i);
    }
    return true;
}