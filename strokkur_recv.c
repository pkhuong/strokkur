#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

#include "strokkur_recv.h"

int
strokkur_recv_chunk(int fd,
                    struct sockaddr_storage *source,
                    struct strokkur_chunk *chunk)
{
        struct iovec iov[1];
        struct msghdr header;
        ssize_t ret;

        memset(&header, 0, sizeof(header));
        memset(&iov, 0, sizeof(iov));

        iov[0].iov_base = chunk;
        iov[0].iov_len = sizeof(*chunk);

        header.msg_name = source;
        header.msg_namelen = sizeof(*source);
        header.msg_iov = iov;
        header.msg_iovlen = 1;

        memset(source, 0, sizeof(*source));
        memset(chunk, 0, sizeof(chunk->header));

        ret = recvmsg(fd, &header, 0);
        if (ret < 0) {
                return -1;
        }

        if ((header.msg_flags & MSG_TRUNC) != 0) {
                return -2;
        }

        {
                size_t expected_bytes = sizeof(chunk->header) + chunk->header.chunk_bytes;

                if ((size_t)ret != expected_bytes) {
                        return -3;
                }
        }

        if (chunk->header.message_bytes < ret) {
                return -3;
        }

        if (chunk->header.message_bytes < chunk->header.chunk_bytes) {
                return -4;
        }

        if (chunk->header.chunk_count == 0 || chunk->header.chunk_count > STROKKUR_CHUNK_MAX) {
                return -5;
        }

        if (chunk->header.message_bytes <= (chunk->header.chunk_count - 1) * STROKKUR_CHUNK_DATA_MAX) {
                return -6;
        }

        if (chunk->header.message_bytes > (chunk->header.chunk_count - 1) * STROKKUR_CHUNK_DATA_MAX + chunk->header.chunk_bytes) {
                return -7;
        }

        if ((size_t)ret < sizeof(*chunk)) {
                memset((char *)chunk + ret, 0, sizeof(*chunk) - ret);
        }

        return 0;
}

void
strokkur_recv_init(struct strokkur_recv_state *state,
                   const struct sockaddr_storage *source,
                   const struct strokkur_chunk *chunk)
{
        struct timeval now;

        memset(state, 0, sizeof(*state));
        if (gettimeofday(&now, NULL) != 0) {
                assert(0 && "gettimeofday failed");
                memset(&now, 0, sizeof(now));
        }

        state->first_received_us = ((uint64_t)now.tv_sec * 1000000UL) + now.tv_usec;
        memcpy(&state->source, source, sizeof(state->source));
        state->send_timestamp_us = chunk->header.send_timestamp_us;
        memcpy(&state->message_id, &chunk->header.message_id,
               sizeof(state->message_id));
        memcpy(&state->hash, &chunk->header.hash,
               sizeof(state->hash));
        state->message_bytes = chunk->header.message_bytes;
        state->chunk_count = chunk->header.chunk_count;
        return;
}

bool
strokkur_recv_initialised(const struct strokkur_recv_state *state)
{

        return (state->message_bytes != 0);
}

void
strokkur_recv_deinit(struct strokkur_recv_state *state)
{

        memset(state, 0, sizeof(*state));
        return;
}

static void
subtract_row(const struct strokkur_recv_state *state,
             struct strokkur_chunk *chunk,
             size_t row_index)
{
        const struct strokkur_chunk *base = state->chunks[row_index];

        strokkur_block_xor(chunk->header.mask, base->header.mask,
                           sizeof(chunk->header.mask));
        strokkur_block_xor(chunk->data, base->data, sizeof(chunk->data));
        return;
}

static struct strokkur_chunk *
process_basis(struct strokkur_recv_state *state,
              struct strokkur_chunk *chunk,
              size_t row_index)
{
        struct strokkur_chunk *ret;

        if (state->chunks[row_index] == NULL) {
                state->chunk_received++;
                state->chunks[row_index] = chunk;
                return NULL;
        }

        if (memcmp(chunk->header.mask, state->chunks[row_index]->header.mask,
                   sizeof(chunk->header.mask)) == 0) {
                return chunk;
        }

        ret = state->chunks[row_index];
        state->chunks[row_index] = chunk;
        subtract_row(state, ret, row_index);
        return ret;
}

static struct strokkur_chunk *
process_row(struct strokkur_recv_state *state,
            struct strokkur_chunk *chunk,
            size_t row_index)
{
        size_t word = row_index / 32;
        size_t shift = row_index % 32;

        if (state->chunks[row_index] == NULL) {
                state->chunk_received++;
                state->chunks[row_index] = chunk;
                return NULL;
        }

        /* XXX: We should do the popcount while we xor chunk out. */
        if (chunk->header.mask[word] == (1UL << shift)) {
                uint32_t base_mask[STROKKUR_CHUNK_MAX / 32];

                memset(base_mask, 0, sizeof(base_mask));
                base_mask[word] |= 1UL << shift;
                /* Are we there yet? */
                if (memcmp(base_mask, chunk->header.mask, sizeof(base_mask)) == 0) {
                        return process_basis(state, chunk, row_index);
                }
        }

        subtract_row(state, chunk, row_index);
        return chunk;
}

static struct strokkur_chunk *
process_rows(struct strokkur_recv_state *state,
             struct strokkur_chunk *chunk,
             size_t word_index)
{

        while (chunk != NULL && chunk->header.mask[word_index] != 0) {
                size_t row = 32 * word_index;

                row += __builtin_ctzl(chunk->header.mask[word_index]);
                chunk = process_row(state, chunk, row);
        }

        return chunk;
}

int
strokkur_recv_add_chunk(struct strokkur_recv_state *state,
                        const struct sockaddr_storage *source,
                        struct strokkur_chunk **chunk_p)
{
        struct strokkur_chunk *chunk = *chunk_p;
        size_t n_word = ((size_t)state->chunk_count + 31) / 32;

        if (memcmp(&state->source, source, sizeof(state->source)) != 0) {
                return -1;
        }

        if (state->send_timestamp_us != chunk->header.send_timestamp_us) {
                return -2;
        }

        if (uuid_compare(state->message_id, chunk->header.message_id) != 0) {
                return -3;
        }

        if (memcmp(state->hash, chunk->header.hash, sizeof(state->hash)) != 0) {
                return -4;
        }

        if (state->message_bytes != chunk->header.message_bytes) {
                return -5;
        }

        if (state->chunk_count != chunk->header.chunk_count) {
                return -6;
        }

        if (state->chunk_received > state->chunk_count) {
                return 0;
        }

        for (size_t word = 0; word < n_word; word++) {
                if (chunk->header.mask[word] == 0) {
                        continue;
                }

                chunk = process_rows(state, chunk, word);
                if (chunk == NULL) {
                        break;
                }
        }

        *chunk_p = chunk;
        if (state->chunk_count > state->chunk_received) {
                return state->chunk_count - state->chunk_received;
        }

        return 0;

}

bool
strokkur_recv_ready(const struct strokkur_recv_state *state)
{

        return (state->chunk_received > state->chunk_count);
}

static void
backsolve(struct strokkur_recv_state *state)
{
        size_t chunk_count = state->chunk_count;

        assert(state->chunk_received >= state->chunk_count);
        if (state->chunk_received != state->chunk_count) {
                return;
        }

        for (size_t i = chunk_count; i --> 0;) {
                size_t chunk_size = state->chunks[i]->header.chunk_bytes;
                size_t word = i / 32;
                size_t shift = i % 32;
                uint32_t mask = 1UL << shift;

                for (size_t j = 0; j < i; j++) {
                        if ((state->chunks[j]->header.mask[word] & mask) == 0) {
                                continue;
                        }

                        strokkur_block_xor(state->chunks[j]->data,
                                           state->chunks[i]->data,
                                           chunk_size);
                }
        }

        state->chunk_received = UINT16_MAX;
        return;
}

ssize_t
strokkur_recv_extract(struct strokkur_recv_state *state,
                      void *buf,
                      size_t bufsz)
{
        size_t written = 0;

        if (state->chunk_received < state->chunk_count) {
                return -1;
        }

        if (state->chunk_count * STROKKUR_CHUNK_DATA_MAX < state->message_bytes) {
                return -2;
        }

        if (bufsz == 0) {
                return state->message_bytes;
        }

        backsolve(state);
        if (bufsz > state->message_bytes) {
                bufsz = state->message_bytes;
        }

        for (size_t i = 0; i < state->chunk_count; i++) {
                size_t remaining = bufsz - written;
                size_t to_read = STROKKUR_CHUNK_DATA_MAX;

                if (remaining == 0) {
                        break;
                }

                if (to_read > remaining) {
                        to_read = remaining;
                }

                memcpy((char *)buf + written, state->chunks[i]->data, to_read);
                written += to_read;
        }

        if (bufsz >= state->message_bytes) {
                /* XXX: Check hash. */
                (void)state->hash;
        }

        return state->message_bytes;
}
