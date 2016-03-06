# Strokkur: fairly reliable connectionless messaging

Strokkur implements (non-empty) binary messages of at most 4MB on top
of UDP.  The decoder supports simultaneous messages, so there is no
need to affine one port per peer: messages are framed and keyed on
both the remote IP endpoint and a per-message UUID.  The protocol
supplement UDP checksums with a 256 bit checksum (either SHA-2 or
SHA-3); thus, faulty networking may cause additional message loss, but
should not result in incorrect messages.  In addition, the protocol is
resilient to reordered and dropped datagrams: it supplement each
messages with a small number of redundant, dense, XOR summaries of the
content.  This trick, reminiscent of fountain codes, is the origin of
the project's name: Strokkur, in Iceland, is one of the world's
highest frequency, throughput, and reliability geyser.

N.B., Strokkur does not attempt to protect against spoofing, sniffing,
or against malicious actors in general.  The only protection comes
from the checksum: given a preshared secret, SHA-3 may double as
authentication.  Other than that, UUIDs are easily copied, and UDP is
basically insecure.

# Interface (Sending messages)

Strokkur is meant to be embedded within a larger event loop, but does
not take a stance on what that event base should be.  Rather than
explicitly registering callbacks, Strokkur depends on the programmer
to regularly attempt to advance each outgoing message's state machine.

The first step is to initialise that state machine.  That's what
`strokkur_send_init` does: it prepares a state machine to send bytes
to a destination over a given file descriptor.  The machine will also
pre-emptively send a few redundant messages to protect against missing
datagrams.  In the common case, no packet is dropped and decoding
incurs minimal overhead.

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

Assuming that state machines are always 0-filled on allocation,
`strokkur_send_initialised` returns true iff `strokkur_send_init` has
been initialised.  Conversely, `strokkur_send_deinit` will 0-fill
the state machine again.

Once a `strokkur_send` state machine is initialised, the programmer
can periodically attempt to push some bytes down the wire by calling
`strokkur_send_pump`.  Ideally, they only do so when the file
descriptor is ready for writes.  `strokkur_send_pump` will return a
negative value on failure, 0 when the machine has completely sent the
message, and 1 on partial success (it sent one chunk but more still
has to go out).

Strokkur curently uses `arc4random` to sample different redundant rows
for each message.  That function is strong enough for our use (we only
want to avoid consistently pathological choices), and is thread safe.

# Interface (Receiving messages)

The `strokkur_send` subsystems shows how easy it is to send multiple
messages while remaining oblivious to the parent event loop.
Demultiplexing multiple messages that may arrive in arbitrary order on
the same socket is a bit more complicated.

Strokkur relies on the programmer to route chunks to the appropriate
`strokkur_recv_state` (using a combination of the message UUID, the IP
source, and the message hash).

When a strokkur socket is ready for reads, `strokkur_recv_chunk` will
attempt to read a message chunk from the socket.  On success, `source`
is overwritten with the source of the chunk, and `chunk` with the
contents of that chunk.

At this point, the programmer has to use custom logic to find the
corresponding receive state, or create a fresh state (and evict an
older state if necessary).

If a new state must be created, `strokkur_recv_init` will overwrite a
state with the information provided by a successful call to
`strokkur_recv_chunk`.

Initialisation can be double-checked with `strokkur_recv_initialised`
(assuming 0-filled allocations), and `strokkur_recv_deinit` will
re-fill a receive state with zeros.

Once a state has been found (or initialised),
`strokkur_recv_add_chunk` will adjoin the new chunk to the receive
state machine.  That function will return a negative value on failure,
0 if the message has enough chunks to try and decode, and positive if
more chunks are still needed.  Regardless of the return value, if
`chunk` points to a non-NULL value on exit, that chunk is redundant
and should be freed or otherwise marked for recycling.

Eventually, the receive state machine will have enough matching chunks
to decode the whole message (when `strokkur_recv_add_chunk` returns 0
or `strokkur_recv_ready` returns true).  The programmer may then call
`strokkur_recv_extract` to complete the decoding and write the first
`bufsz` bytes of the message to `buf`.  `strokkur_recv_extract`
returns a negative value on failure, and the size of the message on
success.  The return value *may* be larger than `bufsz`, and it is
valid to call `strokkur_recv_extract` with a `bufsz` of 0 to allocate
a large enough buffer.  When the buffer is large enough to contain the
whole message, `strokkur_recv_extract` makes sure that the checksum
value matches; if the checksum fails, `strokkur_recv_extract` returns
a negative value.

# Memory management

Strokkur does not allocate dynamic memory itself, and only uses a few
fixed-size structs.

On the send side, the data buffer and the file descriptor should
remain alive until the state machine completes or is deinitialised.
The send state itself does not point to any internal storage and can
then be reused arbitrarily.

The receive side is more complex.  The receive state points to a
number of `strokkur_chunk`s; they should be recycled before
deinitialising or freeing any `strokkur_recv_state`.  In addition,
when `strokkur_recv_add_chunk` returns, it may yield a different chunk
for recycling than the chunk that was added.  Regardless of the return
value of `strokkur_recv_add_chunk`, its `chunk` pointer should be
recycled on exit if non-NULL.
