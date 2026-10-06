/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 linsmod <linsmod@qq.com>
 */
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
 *   - block compression, PForDelta postings, a zstd dictionary. This says the index is
 *     resident so there is nothing to decompress in bulk, which is true and is also the
 *     wrong question at this size: on /work the posting lists were 318.6 MiB of a
 *     1 006 MiB index, and what decides whether they are worth compressing is how many
 *     bytes a *query* has to touch, not how many a load has to inflate. So the one
 *     encoding whose cost is paid a byte at a time by the caller is here, and the ones
 *     that need a block before they pay are not (design §10).
 *   - CRoaring. A posting list is an ascending id array, and the sets here are
 *     intersected by a merge, which a roaring bitmap does not accelerate.
 *   - the path index. `path:` reads a path rebuilt from the parent chain
 *     (design §12 risk 7), so it is a second index over materialised paths and a
 *     separate decision.
 *
 * Invariants worth stating, because both are load-bearing:
 *   - a posting list is strictly ascending, and holds an id at most once. Ascend-
 *     ing holds because finalize walks ids in order and esidx_add only ever hands out
 *     a larger one (D8); at-most-once because tri_index_add() drops a trigram a name
 *     already contributed. The filter's merge depends on both.
 *   - the index is keyed on ASCII-lowercased bytes and nothing else. wc_eq() folds
 *     case with tolower() in the C locale, which is ASCII-only, so folding a
 *     non-ASCII byte here would desynchronise the filter from the matcher it
 *     feeds: it would drop rows the matcher accepts.
 *   - the ids are delta-varint encoded, and *only* this file reads them. Ascending is
 *     what makes the encoding pay (a gap is usually one byte) and it is also what makes
 *     the decode a forward walk: tri_index_filter() reads the chosen list with a
 *     cursor that only moves forward, so it never has to seek into a list it cannot
 *     index directly. That is the property the whole encoding rests on, and it is why
 *     a varint decode is affordable here at all.
 */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "esidx.h"
#include "timer.h"

#define TRI_TAB_MIN   1024u
/* How many trigrams one name can contribute, and therefore how big the per-name
 * key buffer has to be. PATH_MAX is 4096 and the root's stored name is a whole
 * path (design §12.12), so the bound is PATH_MAX-2, not NAME_MAX-2.
 *
 * This used to be NAME_MAX (256), with the overflow path posting a duplicate
 * instead of skipping the trigram -- which broke the invariant the header states,
 * that a posting list holds an id at most once, on exactly the names long enough
 * to reach it. A buffer that cannot overflow removes the choice. 16 KB of stack
 * in a function that is not recursive, on a path that already carries a 64 KB
 * path buffer. */
#define TRI_MAX_KEYS  4096u

/* --------------------------------------------------------------- the keys */

/* ASCII-only on purpose: see the invariant note above.
 *
 * A table, because this runs three times per trigram per name and the build walks
 * every name twice. The two-pass counting build below needs the extraction twice
 * per name, and three compares per byte is a measurable share of the step: 1.88 s
 * on /work before this, against the same walk done twice.
 *
 * Sixteen rows of sixteen, generated rather than typed. The first version of this
 * table had seventeen, and the compiler's "excess elements" warnings scrolled past
 * while the row that folds A-Z was silently dropped -- so every mixed-case
 * prefilter stopped folding, and three suite assertions failed on it. Which is the
 * argument both for the assertions and against typing 256 numbers by hand. */
static const unsigned char tri_fold_tab[256] = {
      0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
     16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
     32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
     48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
     64,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122,  91,  92,  93,  94,  95,
     96,  97,  98,  99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
    128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
    144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
    160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
    192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
    208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
    224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
    240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
};

static inline uint32_t tri_fold(unsigned char c)
{
    return tri_fold_tab[c];
}

static inline uint32_t tri_key(const char *s, size_t i)
{
    /* Three independent loads the compiler can issue together, against three
     * shifts that each depend on the last. */
    return ((uint32_t)tri_fold_tab[(unsigned char)s[i]] << 16) |
           ((uint32_t)tri_fold_tab[(unsigned char)s[i + 1]] << 8) |
            (uint32_t)tri_fold_tab[(unsigned char)s[i + 2]];
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

/* -------------------------------------------------------- the posting codec */

/* LEB128: seven bits per byte, low group first, the high bit set on every byte but
 * the last. Chosen over the alternatives because the *length* is knowable without
 * decoding, which the two-pass build needs -- pass one has to size every list
 * exactly, and that is only possible if it can price a posting before writing it.
 *
 * A gap is never zero (ids ascend, and the first posting of a list is stored whole),
 * so a list of n postings is at least n bytes and the buffer can never be shorter
 * than the count it claims. */
static inline uint32_t tri_vlen(uint32_t v)
{
    uint32_t n = 1;
    while (v >= 0x80u) { v >>= 7; n++; }
    return n;
}

static inline uint32_t tri_vput(uint8_t *p, uint32_t v)
{
    uint32_t n = 0;
    while (v >= 0x80u) { p[n++] = (uint8_t)(v | 0x80u); v >>= 7; }
    p[n++] = (uint8_t)v;
    return n;
}

static inline uint32_t tri_vget(const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint32_t v = 0, s = 0;
    for (;;) {
        uint8_t b = *p++;
        v |= (uint32_t)(b & 0x7fu) << s;
        if (!(b & 0x80u)) { *pp = p; return v; }
        s += 7;
    }
}

static int tri_post(tri_index_t *ti, uint32_t slot, eid_t id)
{
    tri_list_t *l = &ti->list[slot];
    /* The first posting of a list has no predecessor, so it stores the id whole and
     * every later one stores the gap -- which is what makes a list of ids that
     * ascend by one cost one byte each. `last` is kept in the list rather than
     * decoded back out of the buffer, because an append that walked the buffer to
     * find its own tail would make the build quadratic in the length of a list. */
    uint32_t v = l->n ? id - l->last : id;
    uint32_t need = l->nb + tri_vlen(v);
    if (need > l->cap) {
        uint32_t ncap = l->cap ? l->cap * 2 : 16;
        while (ncap < need) ncap *= 2;
        uint8_t *nb = realloc(l->buf, ncap);
        if (!nb) return -1;
        l->buf = nb; l->cap = ncap;
    }
    l->nb += tri_vput(l->buf + l->nb, v);
    l->last = id;
    l->n++;
    ti->n_postings++;
    return 0;
}

/* ------------------------------------------------------------------ build */

/* The distinct trigrams of one name, folded, in first-occurrence order. Shared by
 * the two build passes so both see exactly the same keys -- a pass that disagreed
 * about a duplicate would size a list wrong and then overflow it. Returns the
 * count, which is 0 for a name shorter than a trigram. */
static uint32_t tri_keys_of(const char *name, uint32_t *keys)
{
    size_t len = strlen(name);
    if (len < 3) return 0;               /* no trigram, ref A9 */
    uint32_t n = 0;
    for (size_t i = 0; i + 3 <= len; i++) {
        uint32_t k = tri_key(name, i);
        uint32_t j = 0;
        while (j < n && keys[j] != k) j++;
        if (j < n) continue;             /* this name already contributed k */
        keys[n++] = k;
    }
    return n;
}

int tri_index_add(tri_index_t *ti, const char *name, eid_t id)
{
    uint32_t keys[TRI_MAX_KEYS];
    uint32_t n = tri_keys_of(name, keys);
    for (uint32_t i = 0; i < n; i++) {
        int slot = tri_intern(ti, keys[i]);
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
    uint32_t *keys = malloc(TRI_MAX_KEYS * sizeof(uint32_t));
    if (!keys) return -1;

    /* Two passes, and the reason is 131 MiB on /work.
     *
     * Posting as we go means every list grows by doubling, so the index ends up
     * resident at its high-water capacity: 450.4 MiB touched against 318.6 MiB of
     * ids, 71 % of the list capacity in use. The slack is not a constant factor
     * that a different growth policy would remove either -- it is whatever each
     * list's length happened to be modulo its power of two.
     *
     * So: price first, allocate exactly, then fill. The pricing pass interns the
     * same keys into the same slots, so pass two can reuse the slot table without
     * a lookup of its own; only the append is left, and it never has to grow.
     * ext_index_build() does this in the same function family, for the same
     * reason, with the comment "sized exactly" already written there.
     *
     * What pass one prices is *bytes*, not postings, which is the one thing the
     * encoding decides: a posting costs tri_vlen() of them, and pass one has to
     * reach the same number pass two will write or the exact sizing is a fiction
     * and tri_post() quietly doubles the buffer instead. So `prev` is carried here
     * too -- the gap is what gets priced, and a gap needs the previous id. */
    uint32_t *sz = NULL, *prev = NULL;
    uint32_t acc_n = 0;                   /* allocated slots, always == cap_slots */
    for (uint32_t i = 0; i < n; i++) {
        if (db->et.flags[i] & EF_DEAD) continue;
        uint32_t nk = tri_keys_of(display_name_of(db, i), keys);
        for (uint32_t k = 0; k < nk; k++) {
            int slot = tri_intern(ti, keys[k]);
            if (slot < 0) goto fail;
            /* Grown here rather than sized once up front: the pricing pass is what
             * interns the keys, so `cap_slots` keeps doubling underneath it and a
             * count array sized before the loop is a heap overflow on any tree with
             * more distinct trigrams than the first name contributes. */
            if ((uint32_t)slot >= acc_n) {
                uint32_t want = ti->cap_slots;
                uint32_t *nz = realloc(sz, (size_t)want * sizeof(uint32_t));
                if (!nz) goto fail;
                memset(nz + acc_n, 0, (size_t)(want - acc_n) * sizeof(uint32_t));
                sz = nz;
                uint32_t *np = realloc(prev, (size_t)want * sizeof(uint32_t));
                if (!np) goto fail;
                memset(np + acc_n, 0, (size_t)(want - acc_n) * sizeof(uint32_t));
                prev = np;
                acc_n = want;
            }
            /* `sz[slot] == 0` is the "no posting yet" test, and it is exact: a
             * posting is never zero bytes wide. The first one is the id itself,
             * which is what tri_post() writes for the first one too. */
            sz[slot] += tri_vlen(sz[slot] ? i - prev[slot] : i);
            prev[slot] = i;
        }
    }

    uint64_t priced = 0;
    if (sz) {
        for (uint32_t s = 0; s < ti->n_slots; s++) {
            tri_list_t *l = &ti->list[s];
            uint32_t want = sz[s] ? sz[s] : 1;
            l->buf = malloc(want);
            if (!l->buf) goto fail;
            l->cap = want;
            l->nb = 0;
            l->n = 0;
            l->last = 0;
            priced += sz[s];
        }
    }
    free(sz); sz = NULL;
    free(prev); prev = NULL;

    for (uint32_t i = 0; i < n; i++) {
        if (db->et.flags[i] & EF_DEAD) continue;
        /* display_name_of(), not name_of(): the root's stored name is the absolute
         * path it was indexed from, and the matcher reads the display name
         * (design §12.12). Indexing the stored name would put trigrams in the
         * index that no query ever asks for. */
        uint32_t nk = tri_keys_of(display_name_of(db, i), keys);
        for (uint32_t k = 0; k < nk; k++) {
            /* The slot is where pass one put it, and tri_post() does not have to
             * grow anything: cap is already the exact byte count. */
            int slot = tri_find(ti, keys[k]);
            if (slot < 0 || tri_post(ti, (uint32_t)slot, i) != 0) {
                LOGE("cannot build the name trigram index at entry %u", i);
                goto fail;
            }
        }
    }
    /* The two passes must agree on every byte, and the only way they can disagree
     * is if the pricing and the append stop using the same width function -- which
     * is a heap overflow, not a wrong answer, and so nothing would report it until
     * something else happened to run under a sanitiser. Counted here, once, where
     * the two numbers exist. */
    uint64_t wrote = 0;
    for (uint32_t s = 0; s < ti->n_slots; s++) wrote += ti->list[s].nb;
    if (wrote != priced) {
        LOGE("name trigram index: priced %llu posting bytes and wrote %llu",
             (unsigned long long)priced, (unsigned long long)wrote);
        goto fail;
    }

    LOGD("name trigrams: %u distinct over %u live names, %u postings in %llu bytes",
         ti->n_slots, n, ti->n_postings, (unsigned long long)wrote);
    free(keys);
    return 0;

fail:
    free(keys);
    free(sz);
    free(prev);
    tri_index_free(ti);
    return -1;
}

void tri_index_free(tri_index_t *ti)
{
    if (ti->list) {
        for (uint32_t i = 0; i < ti->n_slots; i++) free(ti->list[i].buf);
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
     * is why this picks one rather than intersecting them all.
     *
     * The `n == 0` branch below is a narrowing all the way to zero, and it is only
     * sound because *every* trigram a name can produce is indexed here. That is why a
     * "stop list" -- do not index the trigrams whose lists are so long that filtering on
     * them is nearly useless, 20 % of the volume on /work in 142 keys -- is not a
     * threshold away: it would make this branch answer zero rows for every pattern
     * containing a stopped trigram, which is the one failure this codebase keeps calling
     * the worst. Doing it needs the index to distinguish "no name has this trigram" from
     * "this trigram was stopped" -- a small key set beside the table -- and the filter to
     * skip the stopped ones instead of narrowing on them. Recorded here because the
     * reasoning is invisible from the threshold and load-bearing from the code. */
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
     * cursor into the posting list, clear whatever the cursor passed. The cursor
     * only moves forward, which is what lets the list be a delta-varint stream --
     * decoding is a byte-at-a-time walk and there is never a reason to go back to
     * one already read.
     *
     * `k` is the index of the posting `v` holds, so `v` is ids[k] and reading the
     * *next* one is a step that has to know there is one. The eid_t array this
     * replaced got that for free -- it tested `ids[k] < i` before it incremented,
     * so it never read ids[n] -- and a varint decode has to ask first. Skipping that
     * ask reads one byte past every list whose last id is below the largest candidate,
     * which -O2 does not notice: the byte past the end decodes to something, and every
     * row it is compared against is one the list had already rejected, so the answers
     * come out right. The sanitiser build is what found it. */
    const uint8_t *p = best->buf;
    uint32_t k = 0;                      /* the first posting is the id, read whole */
    uint32_t v = tri_vget(&p);
    for (uint32_t i = bs_next(out, 0); i < out->nbits; i = bs_next(out, i + 1)) {
        while (v < i) {
            if (k + 1u >= best->n) { k = best->n; break; }
            v += tri_vget(&p);
            k++;
        }
        if (k >= best->n || v != i) bs_clear_bit(out, i);
    }
    return true;
}