/**
 * @file net_addr.h
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 2026-07-23
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef UDP_NET_ADDR_H
#define UDP_NET_ADDR_H

#include <pthread.h>
#include <rte_ether.h>
#include <rte_ring_core.h>
#include <stdint.h>
struct local_addr {
        int fd_;

        uint32_t local_ip_;
        uint8_t local_mac[RTE_ETHER_ADDR_LEN];
        uint16_t local_port_;

        uint8_t protocol_;

        struct rte_ring *recv_buf_;
        struct rte_ring *send_buf_;

        pthread_cond_t cond_;
        pthread_mutex_t mutex_;

        struct local_addr *prev;
        struct local_addr *next;
        // TODO: Add a flag to implement blocking/non-blocking behavior.
};

extern struct local_addr *g_local_addr;

// TODO: replace with a bitmap implementation
#define DEFAULT_FD_NUM 3

int get_fd_from_bitmap(void);

struct local_addr *get_local_addr_from_fd(int sockfd);

struct local_addr *get_local_addr_from_ip_port(uint32_t ip, uint16_t port,
                                               uint8_t protocol);

#endif /* UDP_NET_ADDR_H */