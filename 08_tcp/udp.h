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
#include <stdint.h>

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

/** UDP ops instance (defined in udp.c). */
extern const struct sock_ops udp_ops;

#endif /* NETARCH_UDP_H */
