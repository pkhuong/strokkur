#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <emmintrin.h>

#include "strokkur_common.h"

static void
xor_cache_line(void *restrict acc, const void *restrict src)
{
        __m128i vacc[4];
        __m128i vsrc[4];
        uint8_t *acc8 = acc;
        const uint8_t *src8 = src;

        for (size_t j = 0; j < 4; j++) {
                vacc[j] = _mm_loadu_si128((void *)&acc8[j * 16]);
                vsrc[j] = _mm_loadu_si128((const void *)&src8[j * 16]);
        }

        for (size_t j = 0; j < 4; j++) {
                vacc[j] = _mm_xor_si128(vacc[j], vsrc[j]);
        }

        for (size_t j = 0; j < 4; j++) {
                _mm_storeu_si128((void *)&acc8[j * 16], vacc[j]);
        }

        return;
}

void
strokkur_block_xor(void *restrict acc, const void *restrict src, size_t n_bytes)
{
        uint8_t *acc8 = acc;
        const uint8_t *src8 = src;
        size_t i, round_bytes;

        round_bytes = n_bytes & ~63UL;
        for (i = 0; i < round_bytes; i += 64) {
                xor_cache_line(acc8 + i, src8 + i);
        }

        for (; i < n_bytes; i++) {
                acc8[i] ^= src8[i];
        }

        return;
}
