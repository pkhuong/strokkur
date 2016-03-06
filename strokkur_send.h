#ifndef STROKKUR_SEND_H
#define STROKKUR_SEND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "strokkur_common.h"

/* At most 64 (+ 1) extra messages. */
#define STROKKUR_MAX_REDUNDANT 64

struct strokkur_send_state {
        struct strokkur_chunk_header header;
        int fd;
        struct sockaddr_storage dst;
        const void *data;
        size_t n_bytes;
        size_t n_base;
        size_t n_redundant;
        size_t progress;
        uint32_t masks[STROKKUR_MAX_REDUNDANT][STROKKUR_CHUNK_MAX / 32];
        uint8_t scratch[STROKKUR_CHUNK_DATA_MAX];
};

/**
 * @brief Initialise the send state machine in @a state to squirt @a
 * n_bytes in @a data to @a dst via socket @a fd.
 *
 * @param redundant_messages the number of redundant message to send.
 * At most 1 + STROKKUR_MAX_REDUNDANT are actually sent.  In most cases
 * one more redundant message is actually sent.  If the message is short,
 * fewer messages may be sent.
 * @return 0 on success, negative on failure.
 */
int strokkur_send_init(struct strokkur_send_state *state,
                       int fd, const struct sockaddr_storage *dst,
                       const void *data, size_t n_bytes,
                       size_t redundant_messages);

bool strokkur_send_initialised(const struct strokkur_send_state *state);
void strokkur_send_deinit(struct strokkur_send_state *state);

/**
 * @brief send one message chunk on behalf of the @a state send machine.
 * @return negative on failure, 0 if done, 1 if more work is necessary.
 */
int strokkur_send_pump(struct strokkur_send_state *state);
#endif /* !STROKKUR_SEND_H */
