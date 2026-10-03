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
