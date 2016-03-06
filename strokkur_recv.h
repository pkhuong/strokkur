#ifndef STROKKUR_RECV_H
#define STROKKUR_RECV_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <uuid/uuid.h>

#include "strokkur_common.h"

struct strokkur_chunk {
        struct strokkur_chunk_header header;
        uint8_t data[STROKKUR_CHUNK_DATA_MAX];
};

struct strokkur_recv_state {
        struct timeval first_received;
        struct sockaddr_storage source;

        uint64_t send_timestamp_us;
        uuid_t message_id;
        uint8_t hash[256 / 8];
        uint32_t message_bytes;
        uint16_t chunk_count;

        uint16_t chunk_received; /* UINT16_MAX when backsolved. */
        struct strokkur_chunk *chunks[STROKKUR_CHUNK_MAX];
};

_Static_assert(STROKKUR_CHUNK_MAX < UINT16_MAX,
               "STROKKUR_CHUNK_MAX must be < UINT16_MAX.");

/**
 * @brief Attempt to read a strokkur chunk from socket @a fd.
 * @param fd the socket to read from
 * @param source the source of the data if successful
 * @param chunk the chunk.
 *
 * @return 0 on success, negative on failure.
 */
int strokkur_recv_chunk(int fd, struct sockaddr_storage *source, struct strokkur_chunk *chunk);

/**
 * @brief Overwrite a strokkur recv state for @a source and first
 * chunk @a chunk.
 */
void strokkur_recv_init(struct strokkur_recv_state *, const struct sockaddr_storage *source, const struct strokkur_chunk *chunk);

/**
 * @brief Return whether the strokkur receive state is initialised (in
 * progress).
 */
bool strokkur_recv_initialised(const struct strokkur_recv_state *);

/**
 * @brief Reset the strokkur receive state to an uninitialised state.
 *
 * @note any strokkur chunk in the state must first be recycled.
 */
void strokkur_recv_deinit(struct strokkur_recv_state *);

/**
 * @brief Adjoin a chunk from @a source to a recv state.
 * @param source the source of the chunk as overwritten by strokkur_chunk_recv
 * @param chunk a pointer to the chunk received in
 * strokkur_chunk_recv.  A pointer to a chunk to recycle on exit, a
 * pointer to NULL if no recycling.
 *
 * @return negative on failure (the chunk should still be recycled),
 * positive if more chunks are needed to decode the message, 0 if the
 * message is ready for extraction.
 */
int strokkur_recv_add_chunk(struct strokkur_recv_state *, const struct sockaddr_storage *source, struct strokkur_chunk **chunk);

bool strokkur_recv_ready(const struct strokkur_recv_state *);

/**
 * @brief Flatten a message and write up to @a bufsz bytes of it in @a buf.
 * @return negative on failure, the size of the strokkur message on failure
 */
ssize_t strokkur_recv_extract(struct strokkur_recv_state *, void *buf, size_t bufsz);
#endif /* !STROKKUR_RECV_H */
