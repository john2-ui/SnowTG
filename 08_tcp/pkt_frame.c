/**
 * @file pkt_frame.c
 * @brief Ethernet + IPv4 frame construction shared by every L4 transport.
 */
#include "pkt_frame.h"

#include "net_context.h"

#include <rte_ether.h>
#include <rte_ip.h>
#include <string.h>

struct rte_mbuf *eth_ipv4_build(struct rte_mempool *mp, const uint8_t *dst_mac,
                                uint32_t src_ip, uint32_t dst_ip, uint8_t proto,
                                size_t l4_len, void **l4_out) {
        const size_t eth_len = sizeof(struct rte_ether_hdr);
        const size_t ip_len = sizeof(struct rte_ipv4_hdr);
        const size_t total_len = eth_len + ip_len + l4_len;

        struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mp);
        if (mbuf == NULL)
                return NULL;
        mbuf->pkt_len = (uint32_t)total_len;
        mbuf->data_len = (uint16_t)total_len;

        uint8_t *msg = rte_pktmbuf_mtod(mbuf, uint8_t *);

        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
        rte_memcpy(eth->src_addr.addr_bytes, g_net.local_mac,
                   RTE_ETHER_ADDR_LEN);
        rte_memcpy(eth->dst_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(msg + eth_len);
        ip->version_ihl = 0x45;
        ip->type_of_service = 0;
        ip->total_length = rte_cpu_to_be_16((uint16_t)(ip_len + l4_len));
        ip->packet_id = 0;
        ip->fragment_offset = 0;
        ip->time_to_live = 64;
        ip->next_proto_id = proto;
        ip->src_addr = src_ip;
        ip->dst_addr = dst_ip;
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);

        if (l4_out != NULL)
                *l4_out = msg + eth_len + ip_len;
        return mbuf;
}
