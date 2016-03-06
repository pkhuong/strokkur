#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>

/* Use arc4random for now. */
#ifdef __linux__
#include <bsd/stdlib.h>
#else
#include <stdlib.h>
#endif

#include "strokkur_send.h"

static void
init_extra_row_mask(struct strokkur_send_state *state,
                    size_t n_chunk, size_t redundant_messages)
{
        uint8_t rows[STROKKUR_MAX_REDUNDANT];

        /* Populate the selection table for redundant rows. */
        for (size_t i = 0; i < redundant_messages; i++) {
                rows[i] = i;
        }

        _Static_assert(sizeof(uint32_t) * STROKKUR_MAX_REDUNDANT <= sizeof(state->scratch),
                "Scratch must have enough space for one column's random bits.");
        for (size_t i = 0; i < n_chunk; i++) {
                size_t word = i / 32;
                size_t shift = i % 32;

                arc4random_buf(state->scratch,
                               sizeof(uint32_t) * redundant_messages);
                for (size_t j = 0; j < (redundant_messages + 1) / 2; j++) {
                        uint64_t choice;
                        uint32_t bits;
                        uint8_t temp;

                        memcpy(&bits, state->scratch + j * sizeof(uint32_t),
                               sizeof(uint32_t));
                        choice = (uint64_t)bits * (redundant_messages - j);
                        choice >>= 32;
                        temp = rows[j];
                        rows[j] = rows[j + choice];
                        rows[j + choice] = temp;

                        assert(rows[j] < redundant_messages);
                        state->masks[rows[j]][word] |= 1UL << shift;
                }
        }

        return;
}

int
strokkur_send_init(struct strokkur_send_state *state,
                   int fd, const struct sockaddr_storage *dst,
                   const void *data, size_t n_bytes,
                   size_t redundant_messages)
{
        size_t n_chunk;

        memset(state, 0, (char *)(&state->scratch) - (char *)state);
        if (n_bytes > STROKKUR_CHUNK_MAX * STROKKUR_CHUNK_DATA_MAX) {
                return -1;
        }

        if (n_bytes == 0) {
                return -2;
        }

        n_chunk = (n_bytes + STROKKUR_CHUNK_DATA_MAX - 1) / STROKKUR_CHUNK_DATA_MAX;

        if (redundant_messages > STROKKUR_MAX_REDUNDANT) {
                redundant_messages = STROKKUR_MAX_REDUNDANT;
        }

        state->fd = fd;
        memcpy(&state->dst, dst, sizeof(state->dst));
        state->data = data;
        state->n_bytes = n_bytes;
        state->n_base = n_chunk;
        state->n_redundant = redundant_messages;

        {
                struct timeval now;
                uint64_t now_us;

                if (gettimeofday(&now, NULL) != 0) {
                        assert(0 && "gettimeofday failed");
                        memset(&now, 0, sizeof(now));
                }

                now_us = (uint64_t)now.tv_sec * 1000000UL;
                now_us += now.tv_usec;
                state->header.send_timestamp_us = now_us;
        }

        uuid_generate(state->header.message_id);
        /* XXX: hash data. */
        (void)state->header.hash;
        state->header.message_bytes = n_bytes;
        state->header.chunk_count = n_chunk;
        init_extra_row_mask(state, n_chunk, redundant_messages);
        return 0;
}

bool
strokkur_send_initialised(const struct strokkur_send_state *state)
{

        return (state->n_bytes != 0);
}

void
strokkur_send_deinit(struct strokkur_send_state *state)
{

        memset(state, 0, sizeof(*state));
        return;
}

static int
xor_columns(struct strokkur_send_state *state)
{
        size_t chunk_count = state->n_base;
        bool initialised = false;

        for (size_t i = 0; i < chunk_count; i++) {
                const char *buf;
                size_t offset;
                size_t bytes;

                if ((state->header.mask[i / 32] & (1UL << (i % 32))) == 0) {
                        continue;
                }

                offset = i * STROKKUR_CHUNK_DATA_MAX;
                bytes = state->n_bytes - offset;
                if (bytes > STROKKUR_CHUNK_DATA_MAX) {
                        bytes = STROKKUR_CHUNK_DATA_MAX;
                }

                buf = (const char *)state->data + offset;
                if (initialised == false) {
                        initialised = true;
                        memcpy(state->scratch, buf, bytes);

                        if (bytes < STROKKUR_CHUNK_DATA_MAX) {
                                memset(state->scratch + bytes, 0,
                                       STROKKUR_CHUNK_DATA_MAX - bytes);
                        }
                } else {
                        strokkur_block_xor(state->scratch, buf, bytes);
                }
        }

        if (initialised == false) {
                return -1;
        }

        return 0;
}

static int
send_chunk(int fd, const struct sockaddr_storage *dst,
           const struct strokkur_chunk_header *header, const void *data)
{
        struct iovec iov[2];
        struct msghdr message;
        ssize_t ret;

        memset(&message, 0, sizeof(message));
        memset(&iov, 0, sizeof(iov));

        iov[0].iov_base = (void *)header;
        iov[0].iov_len = sizeof(*header);
        iov[1].iov_base = (void *)data;
        iov[1].iov_len = header->chunk_bytes;

        message.msg_name = (void *)dst;
        message.msg_namelen = sizeof(*dst);
        message.msg_iov = iov;
        message.msg_iovlen = 2;

        ret = sendmsg(fd, &message, 0);
        if (ret < 0) {
                return -1;
        }

        if ((size_t)ret != sizeof(*header) + header->chunk_bytes) {
                return -2;
        }

        return 0;
}

static int
pump_base(struct strokkur_send_state *state)
{
        size_t offset = state->progress * STROKKUR_CHUNK_DATA_MAX;
        size_t size = state->n_bytes - offset;
        size_t word = state->progress / 32;
        size_t shift = state->progress % 32;
        int r;

        assert(state->progress < STROKKUR_CHUNK_MAX);
        assert(offset < state->n_bytes);
        if (size > STROKKUR_CHUNK_DATA_MAX) {
                size = STROKKUR_CHUNK_DATA_MAX;
        }

        state->header.chunk_bytes = size;
        state->header.mask[word] = 1UL << shift;
        r = send_chunk(state->fd, &state->dst,
                       &state->header, (const char *)state->data + offset);
        state->header.mask[word] = 0;

        if (r == 0) {
                state->progress++;
        }

        return r;
}

static int
pump_singleton(struct strokkur_send_state *state)
{
        int r;

        state->header.mask[0] = 1UL;
        r = send_chunk(state->fd, &state->dst,
                       &state->header, state->data);

        if (r == 0) {
                state->progress += 2;
        }

        return r;
}

static int
pump_full_row(struct strokkur_send_state *state)
{
        size_t chunk_count = state->n_base;
        int r;

        if (state->progress == chunk_count) {
                for (size_t i = 0; i < chunk_count; i++) {
                        state->header.mask[i / 32] = 1UL << (i % 32);
                }

                xor_columns(state);
                state->progress++;
        }

        r = send_chunk(state->fd, &state->dst,
                       &state->header, state->scratch);

        if (r == 0) {
                state->progress++;
        }

        return r;
}

static int
pump_random_row(struct strokkur_send_state *state)
{
        size_t chunk_count = state->n_base;
        int r;

        if ((state->progress % 2) == 0) {
                size_t row = (state->progress - chunk_count - 2) / 2;

                memcpy(state->header.mask, state->masks[row],
                       sizeof(state->header.mask));
                r = xor_columns(state);

                if (r != 0) {
                        /* We got a nop row. Skip it. */
                        state->progress += 2;
                        return 1;
                }

                state->progress++;
        }

        r = send_chunk(state->fd, &state->dst,
                       &state->header, state->scratch);
        if (r == 0) {
                state->progress++;
        }

        return r;
}

int
strokkur_send_pump(struct strokkur_send_state *state)
{
        size_t chunk_count = state->n_base;
        size_t n_steps = chunk_count + 2 * (1 + state->n_redundant);
        int r;

        r = 0;
        if (state->progress >= n_steps) {
                goto out;
        }

        if (state->progress < chunk_count) {
                r = pump_base(state);
                goto out;
        }

        assert(chunk_count > 0);
        /* Send n + 1 copies of the one chunk. */
        if (chunk_count == 1) {
                r = pump_singleton(state);
                goto out;
        }

        if (state->progress <= chunk_count + 1) {
                r = pump_full_row(state);
                goto out;
        }

        /* Generate redundant rows. */
        assert(state->progress > chunk_count + 1);
        r = pump_random_row(state);
        if (r > 0) {
                return strokkur_send_pump(state);
        }

out:
        if (r != 0) {
                return r;
        }

        if (state->progress >= n_steps) {
                return 0;
        }

        return 1;
}
