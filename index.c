/* Bitmap index (§5.4).
 * Decision D7: dense bitset for P0-P3, zero dependency; swap to CRoaring at P4
 * by keeping this ABI (bs_init/bs_free/bs_set/bs_test/bs_clear).
 */

#include "esidx.h"

#include <stdlib.h>
#include <string.h>

int bs_init(bitset_t *b, uint32_t nbits)
{
    uint32_t nw = (nbits + 63) / 64;
    b->nbits = nbits;
    b->w = calloc(nw ? nw : 1, sizeof(uint64_t));
    return b->w ? 0 : -1;
}

void bs_free(bitset_t *b)
{
    free(b->w);
    b->w = NULL;
    b->nbits = 0;
}

/* Widen to at least `nbits`. Grown geometrically rather than to the exact
 * request: a reconcile of one hot directory appends every child at once, and
 * growing to `count+1` each time would make that O(k^2) in copied words. */
int bs_reserve(bitset_t *b, uint32_t nbits)
{
    if (nbits <= b->nbits) return 0;
    uint32_t want = b->nbits ? b->nbits * 2 : nbits;
    if (want < nbits) want = nbits;
    uint32_t nw = (want + 63) / 64;
    uint32_t ow = (b->nbits + 63) / 64;
    uint64_t *nw_w = realloc(b->w, (size_t)nw * sizeof(uint64_t));
    if (!nw_w) return -1;
    memset(nw_w + ow, 0, (size_t)(nw - ow) * sizeof(uint64_t));
    b->w = nw_w;
    b->nbits = want;
    return 0;
}

void bs_set(bitset_t *b, uint32_t i)
{
    if (i < b->nbits) b->w[i >> 6] |= (uint64_t)1 << (i & 63);
}

bool bs_test(const bitset_t *b, uint32_t i)
{
    if (i >= b->nbits) return false;
    return (b->w[i >> 6] >> (i & 63)) & 1u;
}

void bs_clear(bitset_t *b)
{
    if (!b->w) return;
    memset(b->w, 0, ((b->nbits + 63) / 64) * sizeof(uint64_t));
}

void bs_clear_bit(bitset_t *b, uint32_t i)
{
    if (i < b->nbits) b->w[i >> 6] &= ~((uint64_t)1 << (i & 63));
}

uint32_t bs_count(const bitset_t *b)
{
    if (!b->w) return 0;
    uint32_t nw = (b->nbits + 63) / 64, c = 0;
    for (uint32_t i = 0; i < nw; i++) c += (uint32_t)__builtin_popcountll(b->w[i]);
    return c;
}

/* The three set operations the language needs. They are the whole of the
 * boolean half of the query algebra (design §5.4), so they belong next to the
 * bitset rather than in the executor. */

void bs_and(bitset_t *dst, const bitset_t *src)
{
    uint32_t nw = (dst->nbits + 63) / 64;
    if (src->nbits < dst->nbits) nw = (src->nbits + 63) / 64;
    for (uint32_t i = 0; i < nw; i++) dst->w[i] &= src->w[i];
    for (uint32_t i = nw; i < (dst->nbits + 63) / 64; i++) dst->w[i] = 0;
}

/* Every set operation is bounded by min(dst, src). The index bitmaps are grown
 * geometrically by bs_reserve() and so can be wider than the scratch set the
 * executor allocated for the current entry count; bounding by the source alone
 * would read past the end of the destination. */

/* Overwrite dst with src. bs_and() intersects, which is the wrong verb for
 * seeding a fresh set from another one -- an empty set AND anything is empty. */
void bs_copy(bitset_t *dst, const bitset_t *src)
{
    uint32_t nw = (src->nbits + 63) / 64;
    uint32_t dw = (dst->nbits + 63) / 64;
    if (nw > dw) nw = dw;
    memcpy(dst->w, src->w, (size_t)nw * sizeof(uint64_t));
    for (uint32_t i = nw; i < dw; i++) dst->w[i] = 0;
}

void bs_or(bitset_t *dst, const bitset_t *src)
{
    uint32_t nw = (dst->nbits + 63) / 64;
    if (src->nbits < dst->nbits) nw = (src->nbits + 63) / 64;
    for (uint32_t i = 0; i < nw; i++) dst->w[i] |= src->w[i];
}

void bs_andnot(bitset_t *dst, const bitset_t *src)
{
    uint32_t nw = (dst->nbits + 63) / 64;
    if (src->nbits < dst->nbits) nw = (src->nbits + 63) / 64;
    for (uint32_t i = 0; i < nw; i++) dst->w[i] &= ~src->w[i];
}

void bs_not(bitset_t *b)
{
    uint32_t nw = (b->nbits + 63) / 64;
    for (uint32_t i = 0; i < nw; i++) b->w[i] = ~b->w[i];
    /* clear the padding bits so that bs_count and bs_next stay honest */
    uint32_t rem = b->nbits & 63;
    if (rem) b->w[nw - 1] &= ((uint64_t)1 << rem) - 1;
}

void bs_set_all(bitset_t *b, uint32_t nbits)
{
    bs_clear(b);
    for (uint32_t i = 0; i < nbits; i++) bs_set(b, i);
}

uint32_t bs_next(const bitset_t *b, uint32_t i)
{
    if (!b->w) return b->nbits;
    while (i < b->nbits) {
        uint32_t w = i >> 6;
        uint64_t v = b->w[w] & (~(uint64_t)0 << (i & 63));
        if (v) return (w << 6) + (uint32_t)__builtin_ctzll(v);
        i = (w + 1) << 6;
    }
    return b->nbits;
}
