/**
 * @file sock_ops.h
 * @brief Per-protocol operations vector shared by every transport.
 *
 * Each transport publishes a @ref sock_ops instance.  All hooks execute on
 * the socket's owner packet worker: application lcores submit commands and do
 * not call this table or dereference @ref nsock directly.  Hooks that cannot
 * make immediate progress return -1/EAGAIN; the owner parks the command and
 * retries it after packet/timer state changes.
 *
 * Lifecycle contract for @c ingress:
 *   - The handler always consumes @p mbuf. On a successful delivery it either
 *     hands the mbuf to the socket's configured receive queue (ring-backed or
 *     owner-local) or frees it; on a drop it frees the mbuf itself. Callers
 *     must not touch @p mbuf after the call returns.
 */
#ifndef NETARCH_SOCK_OPS_H
#define NETARCH_SOCK_OPS_H

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <sys/socket.h>
#include <sys/types.h>

struct nsock; /* Forward declaration; full definition lives in socket.h. */

/**
 * Result of one owner-side transmit flush.
 *
 * IDLE also covers work that is currently blocked by TCP flow control; a
 * future ACK/window update will mark the socket dirty again.  RETRY is for
 * transient output/mempool pressure or additional queued datagrams.  ARP_WAIT
 * removes the socket from the hot dirty FIFO until neighbour learning wakes it.
 */
enum sock_tx_flush_result {
        SOCK_TX_FLUSH_IDLE = 0,
        SOCK_TX_FLUSH_RETRY = 1,
        SOCK_TX_FLUSH_ARP_WAIT = 2,
        /** Transport destroyed the socket while handling terminal failure. */
        SOCK_TX_FLUSH_DESTROYED = 3,
};

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
         * @brief Outbound path: drain transport TX work toward the NIC out
         *        ring, emitting ARP requests when a peer MAC is unresolved.
         *
         * @return One of @ref sock_tx_flush_result.
         */
        int (*tx_flush)(struct nsock *sk, struct rte_mempool *mp);

        /**
         * Non-sleeping connected send probe (TCP).  Peer comes from the TCB.
         * Return EAGAIN instead of waiting for send/window space.
         */
        ssize_t (*send)(struct nsock *sk, const void *buf, size_t len,
                        int flags);

        /**
         * Non-sleeping connected receive probe.  Return EAGAIN when no bytes
         * are ready; the owner implements blocking API semantics.
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

        /**
         * Begin protocol close after the fd has been detached.  It must not
         * block for FIN/TIME_WAIT; terminal owner-side handlers reclaim later.
         */
        int (*close)(struct nsock *sk);

        /**
         * Start active open.  A successful asynchronous start reports
         * EINPROGRESS; handshake/timer paths complete the parked command.
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
