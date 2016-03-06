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

#define LOAD(I) do {                                                    \
                vacc[I] = _mm_loadu_si128((void *)&acc8[I * 16]);       \
                vsrc[I] = _mm_loadu_si128((const void *)&src8[I * 16]); \
        } while (0)

        LOAD(0);
        LOAD(1);
        LOAD(2);
        LOAD(3);
#undef LOAD

#define XOR(I) (vacc[I] = _mm_xor_si128(vacc[I], vsrc[I]))
        XOR(0);
        XOR(1);
        XOR(2);
        XOR(3);
#undef XOR

#define STORE(I) (_mm_storeu_si128((void *)&acc8[I * 16], vacc[I]))
        STORE(0);
        STORE(1);
        STORE(2);
        STORE(3);
#undef STORE

        return;
}

void
strokkur_block_xor(void *restrict acc, const void *restrict src, size_t n_bytes)
{
        uint8_t *acc8 = acc;
        const uint8_t *src8 = src;
        size_t i, round_bytes;

        i = 0;
        round_bytes = n_bytes & ~63UL;
        if (__builtin_expect(round_bytes > 0, 1)) {
                for (; i < round_bytes; i += 64) {
                        xor_cache_line(acc8 + i, src8 + i);
                }

                if (__builtin_expect(i >= n_bytes, 1)) {
                        return;
                }
        } else if (__builtin_expect(n_bytes == 0, 0)) {
                /* Help GCC 5 codegen just a little with edge probabilities. */
                return;
        }

        for (; i < n_bytes; i++) {
                acc8[i] ^= src8[i];
        }

        return;
}
