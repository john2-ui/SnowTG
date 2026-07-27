/**
 * @file sock_ops.h
 * @brief Per-protocol operations vector shared by every transport.
 *
 * Each transport (UDP, TCP, ...) publishes a @ref sock_ops instance and
 * registers it by its IP protocol number. The socket API dispatchers and the
 * packet worker both indirect through this table, so adding a new transport
 * is a matter of providing a new @ref sock_ops instance plus one
 * @ref sock_ops_lookup entry -- no changes to the socket API or to the worker
 * loop.
 *
 * Lifecycle contract for @c ingress:
 *   - The handler always consumes @p mbuf. On a successful delivery it either
 *     hands the mbuf to the socket receive ring (ownership transfers to the
 *     ring) or frees it; on a drop it frees the mbuf itself. Callers must not
 *     touch @p mbuf after the call returns.
 */
#ifndef NETARCH_SOCK_OPS_H
#define NETARCH_SOCK_OPS_H

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <sys/socket.h>
#include <sys/types.h>

struct nsock; /* Forward declaration; full definition lives in socket.h. */

/**
 * @brief Per-protocol behavior.
 *
 * Pointer-typed hooks may be NULL when the protocol does not support the
 * operation (for example UDP has no @c connect / @c listen / @c accept); the
 * socket API dispatcher returns @c -1 / sets @c errno for those.
 */
struct sock_ops {
        const char *name; /**< Human-readable protocol tag for logging. */
        uint8_t protocol; /**< IP protocol number (IPPROTO_UDP, IPPROTO_TCP). */

        /**
         * @brief Inbound path: classify @p mbuf, find or create the matching
         *        socket, and run the protocol receive logic.
         *
         * Always consumes @p mbuf (see the file-level lifecycle contract).
         */
        int (*ingress)(struct rte_mbuf *mbuf);

        /**
         * @brief Outbound path: drain the socket send ring toward the NIC out
         *        ring, emitting ARP requests when a peer MAC is unresolved.
         */
        int (*tx_flush)(struct nsock *sk, struct rte_mempool *mp);

        /**
         * Connected send (TCP). Peer comes from the TCB; @p dest is unused.
         * NULL for connectionless transports.
         */
        ssize_t (*send)(struct nsock *sk, const void *buf, size_t len,
                        int flags);

        /**
         * Connected receive (TCP). NULL for connectionless transports.
         */
        ssize_t (*recv)(struct nsock *sk, void *buf, size_t len, int flags);

        /**
         * Datagram send (UDP). Socket is the local 2-tuple; @p dest is the
         * peer for this packet. NULL for connection-oriented transports.
         */
        ssize_t (*sendto)(struct nsock *sk, const void *buf, size_t len,
                          int flags, const struct sockaddr *dest,
                          socklen_t addrlen);

        /**
         * Datagram receive (UDP). Fills @p src with the peer of this packet.
         * NULL for connection-oriented transports.
         */
        ssize_t (*recvfrom)(struct nsock *sk, void *buf, size_t len, int flags,
                            struct sockaddr *src, socklen_t *addrlen);

        /** Release the socket and every queued packet it still owns. */
        int (*close)(struct nsock *sk);

        /**
         * Active open (TCP): may implicit-bind, send SYN, block until
         * ESTABLISHED or failure. NULL for connectionless transports.
         */
        int (*connect)(struct nsock *sk, const struct sockaddr *addr,
                       socklen_t addrlen);

        /** Passive open (TCP). NULL for connectionless transports. */
        int (*listen)(struct nsock *sk, int backlog);

        /** Accept one completed connection (TCP). NULL for connectionless. */
        int (*accept)(struct nsock *sk, struct sockaddr *addr,
                      socklen_t *addrlen);
};

/**
 * @brief Look up the ops vector for an IP protocol number.
 * @return Pointer to the registered @ref sock_ops, or NULL if unknown.
 */
const struct sock_ops *sock_ops_lookup(uint8_t protocol);

/** UDP ops instance (defined in udp.c). */
extern const struct sock_ops udp_ops;

/** TCP ops instance (defined in tcp.c). */
extern const struct sock_ops tcp_ops;

#endif /* NETARCH_SOCK_OPS_H */
