/**
 * @file socket.h
 * @brief Unified userspace socket: one type for UDP and TCP, one registry, and
 *        the BSD-style socket API dispatchers.
 *
 * @ref nsock carries the transport-independent state every socket needs (fd,
 * local address, receive/send rings, blocking-wait synchronization, the
 * per-protocol @ref sock_ops, and list linkage). Transport-private state is
 * embedded in the @c u union -- TCP puts its @ref tcp_stream there; UDP has no
 * private peer state (the socket is the local 2-tuple only).
 *
 * Addressing model:
 *   - UDP: socket <-> (local_ip, local_port). Each datagram carries its peer
 *     via @c nsendto / @c nrecvfrom.
 *   - TCP: socket <-> 4-tuple after accept/connect. Use @c nsend / @c nrecv.
 *
 * The socket API entry points are thin dispatchers: they resolve the fd to a
 * @ref nsock and forward to @c sk->ops->... . The packet worker iterates
 * @ref g_sock_list and calls @c sk->ops->tx_flush per socket.
 */
#ifndef NETARCH_SOCKET_H
#define NETARCH_SOCKET_H

#include "sock_ops.h"
#include "tcp.h"

#include <pthread.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

/** Highest fd value the in-process fd table can hand out. */
#define NSOCK_FD_MAX 1024

/**
 * @brief One userspace socket, shared by every transport.
 *
 * The worker lcore delivers packets through @c recv_buf; the application lcore
 * consumes @c recv_buf and produces packets through @c send_buf. @c send_buf is
 * opaque to common code: each transport's @ref sock_ops interprets its contents
 * (UDP stores mbufs, TCP stores @ref tcp_fragment).
 */
struct nsock {
        int fd; /**< Descriptor from fd_alloc(), or -1 until tcp_accept(). */
        uint8_t protocol; /**< IPPROTO_UDP / IPPROTO_TCP. */

        uint32_t local_ip;   /**< Bound IPv4, network byte order. */
        uint16_t local_port; /**< Bound transport port, network byte order. */
        uint8_t local_mac[RTE_ETHER_ADDR_LEN]; /**< Local Ethernet address. */

        struct rte_ring *recv_buf; /**< Worker -> application packet ring. */
        struct rte_ring *send_buf; /**< Application -> worker packet ring. */

        /** Used by blocking recv(); ring occupancy is the condition. */
        pthread_cond_t cond;
        pthread_mutex_t mutex;

        const struct sock_ops *ops; /**< Per-protocol behavior. */

        /** Transport-private state. */
        union {
                struct tcp_stream tcp; /**< TCP connection state. */
        } u;

        struct nsock *prev; /**< Previous socket in @ref g_sock_list. */
        struct nsock *next; /**< Next socket in @ref g_sock_list. */
};

/** Head of the intrusive list containing every open socket. */
extern struct nsock *g_sock_list;

/**
 * @name fd table
 * @{
 */
/** Allocate and reserve an unused fd, or return -1 when the table is full. */
int fd_alloc(void);
/** Release a reserved fd back to the table. */
void fd_release(int fd);
/** @} */

/**
 * @name socket allocation
 * @{
 */
/**
 * @brief Allocate and register a @ref nsock with fresh, uniquely-named rings.
 *
 * @param fd       Descriptor from fd_alloc(), or -1 for an incomplete TCP
 *                 child that will receive an fd later in tcp_accept.
 * @param protocol IP protocol number; selects the @ref sock_ops to bind.
 * @return Initialized socket, or NULL on allocation failure.
 */
struct nsock *nsock_alloc(int fd, uint8_t protocol);
/** Remove @p sk from the registry and release its rings, lock, cond, and fd. */
void nsock_free(struct nsock *sk);
/** @} */

/**
 * @name registry lookups
 * @{
 */
struct nsock *nsock_from_fd(int fd);
struct nsock *nsock_from_ip_port(uint32_t ip, uint16_t port, uint8_t protocol);
struct nsock *nsock_from_4tuple(uint32_t remote_ip, uint32_t local_ip,
                                uint16_t remote_port, uint16_t local_port,
                                uint8_t protocol);
/** @} */

/**
 * @name BSD-style socket API (dispatchers)
 * @{
 */
int nsocket(int domain, int type, int protocol);
int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
/** Connected send (TCP). */
ssize_t nsend(int sockfd, const void *buf, size_t len, int flags);
/** Connected receive (TCP). */
ssize_t nrecv(int sockfd, void *buf, size_t len, int flags);
/** Datagram send (UDP); @p dest_addr is the peer for this packet. */
ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);
/** Datagram receive (UDP); @p src_addr receives the peer of this packet. */
ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen);
int nclose(int sockfd);
int nconnect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
int nlisten(int sockfd, int backlog);
int naccept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
/** @} */

#endif /* NETARCH_SOCKET_H */
