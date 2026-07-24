/**
 * @file net_addr.h
 * @brief Socket address records and lookup helpers.
 */
#ifndef NETARCH_NET_ADDR_H
#define NETARCH_NET_ADDR_H

#include <pthread.h>
#include <rte_ether.h>
#include <rte_ring.h>
#include <stdint.h>

/**
 * State owned by one userspace socket.
 *
 * The worker lcore delivers packets through recv_buf_; the application lcore
 * consumes recv_buf_ and produces packets through send_buf_.
 */
struct local_addr {
        int fd_; /**< Socket descriptor exposed by socket_api. */

        uint32_t local_ip_; /**< Bound IPv4 address, network byte order. */
        uint8_t local_mac[RTE_ETHER_ADDR_LEN];
        uint16_t local_port_; /**< Bound transport port, network byte order. */

        uint8_t protocol_; /**< IPPROTO_* value. */

        struct rte_ring *recv_buf_; /**< Worker-to-application packet ring. */
        struct rte_ring *send_buf_; /**< Application-to-worker packet ring. */

        /** Used by blocking nrecvfrom(); ring occupancy is the condition. */
        pthread_cond_t cond_;
        pthread_mutex_t mutex_;

        struct local_addr *prev;
        struct local_addr *next;
};

/** Head of the intrusive list containing all open sockets. */
extern struct local_addr *g_local_addr;

/** Placeholder descriptor used until a real descriptor allocator is added. */
#define DEFAULT_FD_NUM 3

/** Return the current placeholder descriptor value. */
int get_fd_from_bitmap(void);

/** Find an open socket by descriptor, or NULL. */
struct local_addr *get_local_addr_from_fd(int sockfd);

/** Find a socket bound to an exact IP/port/protocol tuple, or NULL. */
struct local_addr *get_local_addr_from_ip_port(uint32_t ip, uint16_t port,
                                               uint8_t protocol);

#endif /* NETARCH_NET_ADDR_H */