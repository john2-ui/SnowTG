#ifndef NETARCH_OWNER_IO_H
#define NETARCH_OWNER_IO_H

/**
 * @file owner_io.h
 * @brief Non-blocking owner-lcore transport API for upper-layer reactors.
 *
 * Unlike the BSD-compatible n* API, these calls may be made only on the
 * packet worker that owns @ref nsock_handle.  They never submit a command,
 * wait on a condition variable, or expose a transport control block.
 */

#include "socket_owner.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

enum owner_io_events {
        OWNER_IO_EV_READ = 1u << 0,
        OWNER_IO_EV_WRITE = 1u << 1,
        OWNER_IO_EV_CONNECTED = 1u << 2,
        OWNER_IO_EV_ERROR = 1u << 3,
        OWNER_IO_EV_HUP = 1u << 4,
};

/** Invoked on the owner worker after owner-local socket destruction. */
typedef void (*owner_io_release_fn)(void *ctx);

struct owner_io_event {
        struct nsock_handle handle;
        uint32_t events;
};

/**
 * Create a UDP or TCP socket directly on its owning packet-worker lcore.
 *
 * The returned handle is owner-local and is never associated with a BSD fd.
 */
int owner_io_socket_create(uint8_t protocol, struct nsock_handle *out);
/**
 * Create an owner-local socket whose TCP queues are embedded lists rather than
 * per-socket DPDK rings. This is intended for same-lcore reactors only.
 */
int owner_io_socket_create_local(uint8_t protocol, struct nsock_handle *out);
/** Register a callback invoked only after this socket is finally destroyed. */
int owner_io_set_release_observer(struct nsock_handle handle,
                                  owner_io_release_fn fn, void *ctx);
/** Bind an owner-local socket to a valid IPv4 endpoint without blocking. */
int owner_io_bind(struct nsock_handle handle, const struct sockaddr *addr,
                  socklen_t addrlen);
/**
 * Start a non-blocking connection to a valid IPv4 endpoint; TCP returns
 * -1/EINPROGRESS while handshaking.
 */
int owner_io_connect(struct nsock_handle handle, const struct sockaddr *addr,
                     socklen_t addrlen);
/** Try a connected send; returns -1/EAGAIN instead of parking the reactor. */
ssize_t owner_io_send(struct nsock_handle handle, const void *buf, size_t len);
/** Try a connected receive; returns -1/EAGAIN when no payload is ready. */
ssize_t owner_io_recv(struct nsock_handle handle, void *buf, size_t len);
/**
 * Try a datagram send to a valid IPv4 endpoint; returns -1/EAGAIN when the
 * local queue is full.
 */
ssize_t owner_io_sendto(struct nsock_handle handle, const void *buf, size_t len,
                        const struct sockaddr *addr, socklen_t addrlen);
/**
 * Try a datagram receive; returns -1/EAGAIN when no datagram is queued.
 * When @p addr is non-NULL, @p addrlen must point to storage at least as
 * large as @c sockaddr_in and is updated with the peer address size.
 */
ssize_t owner_io_recvfrom(struct nsock_handle handle, void *buf, size_t len,
                          struct sockaddr *addr, socklen_t *addrlen);
/** Initiate owner-local transport teardown and invalidate future operations. */
int owner_io_close(struct nsock_handle handle);

/**
 * Drain up to @p max_events coalesced readiness events without blocking.
 * Stale events whose socket slot was retired are discarded internally.
 */
unsigned int owner_io_ready_burst(struct owner_io_event *events,
                                  unsigned int max_events);

#endif /* NETARCH_OWNER_IO_H */
