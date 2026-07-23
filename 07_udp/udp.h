/**
 * @file udp.h
 * @brief UDP packet construction, socket delivery, and egress helpers.
 */
#ifndef NETARCH_UDP_H
#define NETARCH_UDP_H

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <stdint.h>

/**
 * @brief Build an Ethernet/IPv4/UDP packet carrying @p data.
 *
 * All addresses and ports are in network byte order. Lengths use payload-only
 * semantics: @p data_len is the number of UDP payload bytes, and the IPv4 and
 * UDP length fields plus checksums are derived from it.
 *
 * @param mp       Mempool used to allocate the mbuf.
 * @param dst_mac  Destination Ethernet address.
 * @param src_ip   Source IPv4 (network order).
 * @param dst_ip   Destination IPv4 (network order).
 * @param src_port Source UDP port (network order).
 * @param dst_port Destination UDP port (network order).
 * @param data     Payload bytes to copy (may be NULL when @p data_len is 0).
 * @param data_len Payload length in bytes.
 * @return Newly allocated mbuf, or NULL on allocation failure.
 */
struct rte_mbuf *udp_build_pkt(struct rte_mempool *mp, const uint8_t *dst_mac,
                               uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               const uint8_t *data, uint16_t data_len);

/**
 * @brief Deliver one inbound UDP datagram to its bound socket.
 *
 * On success ownership of @p mbuf transfers to the socket receive ring. If no
 * socket matches or the ring is full, this function frees the mbuf.
 *
 * @param mp   Reserved mempool argument retained for handler API consistency.
 * @param mbuf Inbound frame (consumed by this call).
 * @return 0 when delivered, -1 when dropped.
 */
int udp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf);

/**
 * @brief Move one pending datagram per socket toward the NIC output ring.
 *
 * If a destination MAC is unresolved, an ARP request is emitted and the UDP
 * mbuf is returned to its socket send ring.
 *
 * @param mp Mempool used for ARP request packets.
 * @return 0 after scanning the socket list.
 */
int udp_out(struct rte_mempool *mp);

#endif /* NETARCH_UDP_H */
