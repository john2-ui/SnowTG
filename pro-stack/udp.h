/**
 * @file udp.h
 * @brief UDP ops vector and packet construction helper.
 *
 * UDP behavior is exposed to the unified socket layer through @ref udp_ops.
 * A UDP socket is the local 2-tuple only; peer addresses travel with each
 * datagram via @c sendto / @c recvfrom.
 */
#ifndef NETARCH_UDP_H
#define NETARCH_UDP_H

#include "sock_ops.h"

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <stddef.h>
#include <stdint.h>

struct udp_rx_node;

/**
 * @brief UDP-private state embedded in @ref nsock.
 *
 * Ring-backed sockets leave these fields unused.  Owner-local sockets use the
 * FIFO and @c rx_current instead of allocating socket recv/send rings.
 */
struct udp_stream {
        struct udp_rx_node *rx_queue_head;
        struct udp_rx_node *rx_queue_tail;
        uint32_t rx_queue_count;
        struct rte_mbuf *rx_current;
};

/**
 * @brief Build an Ethernet/IPv4/UDP packet carrying @p data.
 *
 * All addresses and ports are in network byte order. @p data_len is the number
 * of UDP payload bytes; the IPv4 and UDP length fields and checksums are
 * derived from it.
 */
struct rte_mbuf *udp_build_pkt(struct rte_mempool *mp, const uint8_t *dst_mac,
                               uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               const uint8_t *data, uint16_t data_len);

/** Consume one inbound UDP frame, including local queue ownership. */
int udp_ingress(struct rte_mbuf *mbuf);
/** Drain one owner-local or ring-backed UDP TX work item. */
int udp_tx_flush(struct nsock *sk, struct rte_mempool *mp);
/** Drain a UDP socket and release its final resources. */
int udp_close(struct nsock *sk);

/** UDP ops instance (defined in udp.c). */
extern const struct sock_ops udp_ops;

#endif /* NETARCH_UDP_H */
