/**
 * @file socket_api.h(only for udp now)
 * @brief Minimal datagram socket API backed by DPDK rings.
 */
#ifndef NETARCH_SOCKET_API_H
#define NETARCH_SOCKET_API_H

#include <sys/socket.h>
#include <sys/types.h>

/** Create a userspace socket and its receive/send rings. */
int nsocket(int domain, int type, int protocol);

/** Bind a socket to a local IPv4 address and port. */
int nbind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
 * Block until a datagram is available and copy bytes into @p buf.
 *
 * The dequeued mbuf remains owned by this function and is freed after the
 * complete payload is consumed. The current partial-read behavior requeues the
 * unconsumed bytes.
 */
ssize_t nrecvfrom(int sockfd, void *buf, size_t len, int flags,
                  struct sockaddr *src_addr, socklen_t *addrlen);

/** Build a UDP datagram and transfer its mbuf to the socket send ring. */
ssize_t nsendto(int sockfd, const void *buf, size_t len, int flags,
                const struct sockaddr *dest_addr, socklen_t addrlen);

/** Remove a socket and release all queued packets and synchronization state. */
int nclose(int sockfd);

#endif /* NETARCH_SOCKET_API_H */