#include "icmp.h"

#include "log.h"
#include "net_context.h"

#include <netinet/in.h>
#include <rte_ether.h>
#include <rte_icmp.h>
#include <rte_ip.h>
#include <string.h>

/**
 * @brief RFC 1071 Internet checksum over @p count bytes at @p addr.
 */
static uint16_t icmp_checksum(uint16_t *addr, int count) {
        uint32_t sum = 0;

        while (count > 1) {
                sum += *addr++;
                count -= 2;
        }
        if (count > 0)
                sum += *(uint8_t *)addr;

        while (sum >> 16)
                sum = (sum & 0xFFFF) + (sum >> 16);

        return (uint16_t)~sum;
}

struct rte_mbuf *icmp_build_pkt(struct rte_mempool *mp, const uint8_t *dst_mac,
                                uint32_t src_ip, uint32_t dst_ip, uint16_t id,
                                uint16_t seqnb) {
        const unsigned int total_len = sizeof(struct rte_ether_hdr) +
                                       sizeof(struct rte_ipv4_hdr) +
                                       sizeof(struct rte_icmp_hdr);

        struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mp);
        if (mbuf == NULL) {
                LOG_ERROR("rte_pktmbuf_alloc() failed");
                return NULL;
        }

        mbuf->pkt_len = total_len;
        mbuf->data_len = total_len;

        uint8_t *msg = rte_pktmbuf_mtod(mbuf, uint8_t *);

        /* Ethernet header */
        struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
        rte_memcpy(eth->src_addr.addr_bytes, g_net.local_mac,
                   RTE_ETHER_ADDR_LEN);
        rte_memcpy(eth->dst_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
        eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

        /* IPv4 header */
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        ip->version_ihl = 0x45;
        ip->type_of_service = 0;
        ip->total_length = rte_cpu_to_be_16(sizeof(struct rte_ipv4_hdr) +
                                            sizeof(struct rte_icmp_hdr));
        ip->packet_id = 0;
        ip->fragment_offset = 0;
        ip->time_to_live = 64;
        ip->next_proto_id = IPPROTO_ICMP;
        ip->src_addr = src_ip;
        ip->dst_addr = dst_ip;
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);

        /* ICMP header */
        struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)(ip + 1);
        icmp->icmp_type = RTE_IP_ICMP_ECHO_REPLY;
        icmp->icmp_code = 0;
        icmp->icmp_ident = id;
        icmp->icmp_seq_nb = seqnb;
        icmp->icmp_cksum = 0;
        icmp->icmp_cksum =
            icmp_checksum((uint16_t *)icmp, sizeof(struct rte_icmp_hdr));

        return mbuf;
}

void icmp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                 struct rte_ring *out) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)(ip + 1);

        if (icmp->icmp_type != RTE_IP_ICMP_ECHO_REQUEST) {
                rte_pktmbuf_free(mbuf);
                return;
        }

        LOG_INFO("icmp echo request " IP_FMT " -> " IP_FMT ", replying",
                 IP_ARG(ip->src_addr), IP_ARG(ip->dst_addr));

        struct rte_mbuf *reply =
            icmp_build_pkt(mp, eth->src_addr.addr_bytes, ip->dst_addr,
                           ip->src_addr, icmp->icmp_ident, icmp->icmp_seq_nb);
        if (reply != NULL)
                rte_ring_mp_enqueue_burst(out, (void **)&reply, 1, NULL);

        rte_pktmbuf_free(mbuf);
}
