/**
 * @file tcp.h
 * @author your name (you@domain.com)
 * @brief
 * @version 0.1
 * @date 2026-07-24
 *
 * @copyright Copyright (c) 2026
 *
 */

#ifndef TCP_TCP_H
#define TCP_TCP_H
#include "net_addr.h"
#include <rte_ether.h>
#include <rte_malloc.h>
#include <rte_mempool.h>
#include <rte_ring_core.h>
#include <rte_tcp.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define TCP_MAX_OPTIONS 10

typedef enum _TCP_STATUS {
        TCP_STATUS_CLOSED,
        TCP_STATUS_LISTEN,
        TCP_STATUS_SYN_SENT,
        TCP_STATUS_SYN_RECV,
        TCP_STATUS_ESTABLISHED,
        TCP_STATUS_CLOSE_WAIT,
        TCP_STATUS_LAST_ACK,
        TCP_STATUS_TIME_WAIT,
        TCP_STATUS_CLOSING,
        TCP_STATUS_FIN_WAIT_1,
        TCP_STATUS_FIN_WAIT_2,
} TCP_STATUS;

/**Used to maintain TCP state */
struct tcp_stream { // TODO: Merge tcp_stream and local addr in UDP into one.
        int fd;

        uint32_t src_ip;
        uint32_t dst_ip;

        uint16_t src_port;
        uint16_t dst_port;

        uint16_t proto;

        uint8_t local_mac[RTE_ETHER_ADDR_LEN];

        uint32_t sent_seq;
        uint32_t recv_ack;

        TCP_STATUS status;

        struct rte_ring *recv_buf;
        struct rte_ring *send_buf;

        struct tcp_stream *prev;
        struct tcp_stream *next;
};

/**Refer to the structure in rte_tcp.h */
struct tcp_fragment {
        uint16_t src_port;
        uint16_t dst_port;
        uint32_t sent_seq;
        uint32_t recv_ack;
        uint8_t data_off;
        uint8_t tcp_flags;
        uint16_t rx_win;
        uint16_t cksum;
        uint16_t tcp_urp;

        int opt_len;
        uint32_t options[TCP_MAX_OPTIONS];

        unsigned char *payload;
        size_t payload_len;
};

struct tcp_table {
        int count; // count of tcp streams in the table( currently unused, but
                   // it is kept for future expansion)
        struct tcp_stream *tcb_set; // tcb -> tcp control block
};

/**
 * TODO: This is a temporary implementation; it can be replaced with sock_ops
 * and UDP unification later.
 *
 */

struct tcp_table *tcp_table_instance(void);

struct tcp_stream *tcp_stream_search(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     __attribute__((unused)) uint8_t proto);
struct tcp_stream *tcp_stream_create(uint32_t src_ip, uint32_t dst_ip,
                                     uint16_t src_port, uint16_t dst_port,
                                     __attribute__((unused)) uint8_t proto);
int tcp_stream_handle_listen(struct tcp_stream *stream,
                             struct rte_tcp_hdr *tcphdr);

int tcp_stream_handle_syn_recv(struct tcp_stream *stream,
                               struct rte_tcp_hdr *tcp_hdr);

int tcp_handle(struct rte_mbuf *mbuf);

void tcp_out(struct rte_mempool *mp);
#endif