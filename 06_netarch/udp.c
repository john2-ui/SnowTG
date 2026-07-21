#include "udp.h"

#include "log.h"
#include "net_context.h"

#include <netinet/in.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <string.h>

struct rte_mbuf *udp_build_pkt(struct rte_mempool *mp, const uint8_t *dst_mac,
                               uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               const uint8_t *data, uint16_t data_len) {
        const unsigned int udp_len = sizeof(struct rte_udp_hdr) + data_len;
        const unsigned int ip_len = sizeof(struct rte_ipv4_hdr) + udp_len;
        const unsigned int total_len = sizeof(struct rte_ether_hdr) + ip_len;

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
        ip->total_length = rte_cpu_to_be_16(ip_len);
        ip->packet_id = 0;
        ip->fragment_offset = 0;
        ip->time_to_live = 64;
        ip->next_proto_id = IPPROTO_UDP;
        ip->src_addr = src_ip;
        ip->dst_addr = dst_ip;
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);

        /* UDP header + payload */
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
        udp->src_port = src_port;
        udp->dst_port = dst_port;
        udp->dgram_len = rte_cpu_to_be_16(udp_len);
        if (data_len > 0 && data != NULL)
                rte_memcpy((uint8_t *)(udp + 1), data, data_len);
        udp->dgram_cksum = 0;
        udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);

        return mbuf;
}

void udp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf,
                struct rte_ring *out) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);

        uint16_t dgram_len = rte_be_to_cpu_16(udp->dgram_len);
        uint16_t payload_len = dgram_len - sizeof(struct rte_udp_hdr);

        LOG_INFO("udp " IP_FMT ":%u -> " IP_FMT ":%u len=%u",
                 IP_ARG(ip->src_addr), rte_be_to_cpu_16(udp->src_port),
                 IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                 payload_len);

        /* Echo the payload back to the sender with source/destination swapped.
         */
        struct rte_mbuf *reply = udp_build_pkt(
            mp, eth->src_addr.addr_bytes, ip->dst_addr, ip->src_addr,
            udp->dst_port, udp->src_port, (uint8_t *)(udp + 1), payload_len);
        if (reply != NULL)
                rte_ring_mp_enqueue_burst(out, (void **)&reply, 1, NULL);

        rte_pktmbuf_free(mbuf);
}
