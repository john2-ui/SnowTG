/**
 * @file pkt_frame.h
 * @brief Shared Ethernet + IPv4 frame builder for L4 transports.
 *
 * UDP, TCP, and any future transport share the same Ethernet/IPv4 header
 * layout. This helper allocates an mbuf, fills the L2/L3 headers, sizes the
 * mbuf, and hands back a pointer to the L4 area; the caller then fills in the
 * transport header and payload. Factoring this out removes the duplicated
 * eth+ip code that used to live in both udp_build_pkt and encode_tcp_pkt, and
 * makes adding a new transport a matter of filling L4 only.
 */
#ifndef NETARCH_PKT_FRAME_H
#define NETARCH_PKT_FRAME_H

#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Allocate an mbuf and fill its Ethernet + IPv4 headers.
 *
 * The source MAC is the local NIC MAC (@c g_net.local_mac). The IPv4
 * total_length and header checksum are computed from @p l4_len. The mbuf's
 * pkt_len and data_len are set to the full frame size.
 *
 * @param mp     Mempool to allocate from.
 * @param dst_mac Destination Ethernet address.
 * @param src_ip  Source IPv4 (network order).
 * @param dst_ip  Destination IPv4 (network order).
 * @param proto   IP protocol number for next_proto_id.
 * @param l4_len  Number of bytes that will follow the IPv4 header (L4 header +
 *                options + payload).
 * @param l4_out  Out-param: pointer to the L4 area inside the mbuf.
 * @return The mbuf, or NULL on allocation failure.
 */
struct rte_mbuf *eth_ipv4_build(struct rte_mempool *mp, const uint8_t *dst_mac,
                                uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                                size_t l4_len, void **l4_out);

#endif /* NETARCH_PKT_FRAME_H */
