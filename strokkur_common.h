#ifndef STROKKUR_COMMON_H
#define STROKKUR_COMMON_H
#include <stddef.h>
#include <stdint.h>
#include <uuid/uuid.h>

/* A strokkur message may be composed of at most 512 chunks. */
#define STROKKUR_CHUNK_MAX 512UL
/* A strokkur chunk may have at most 8K bytes of data. */
#define STROKKUR_CHUNK_DATA_MAX 8192UL

/* The header is little endian on the wire. */
struct strokkur_chunk_header {
        uint64_t send_timestamp_us;
        uuid_t message_id;
        uint8_t hash[256 / 8];
        uint32_t message_bytes;
        uint16_t chunk_count;
        uint16_t chunk_bytes;
        uint32_t mask[(STROKKUR_CHUNK_MAX + 31) / 32];
};

_Static_assert((sizeof(struct strokkur_chunk_header) % 64) == 0, "Strokkur chunk header should be aligned to a cache line.");

void strokkur_block_xor(void *acc, const void *src, size_t n_bytes);
#endif /* !STROKKUR_COMMON_H */
