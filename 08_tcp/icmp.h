/**
 * @file icmp.h
 * @brief ICMP echo (ping) build/handle helpers.
 */
#ifndef NETARCH_ICMP_H
#define NETARCH_ICMP_H

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <stdint.h>

/**
 * @brief Build an ICMP echo-reply packet.
 *
 * @param mp      Mempool used to allocate the mbuf.
 * @param dst_mac Destination Ethernet address.
 * @param src_ip  Source IPv4 (network order).
 * @param dst_ip  Destination IPv4 (network order).
 * @param id      ICMP identifier copied from the request.
 * @param seqnb   ICMP sequence number copied from the request.
 * @return Newly allocated mbuf, or NULL on allocation failure.
 */
struct rte_mbuf *icmp_build_pkt(struct rte_mempool *mp, const uint8_t *dst_mac,
                                uint32_t src_ip, uint32_t dst_ip, uint16_t id,
                                uint16_t seqnb);

/**
 * @brief Handle one inbound ICMP frame, replying to echo requests.
 *
 * The inbound mbuf is always freed before returning.
 *
 * @param mp   Mempool for the reply packet.
 * @param mbuf Inbound frame (consumed by this call).
 * @param out  Ring on which the reply is enqueued.
 */
void icmp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                 struct rte_ring *out);

#endif /* NETARCH_ICMP_H */
