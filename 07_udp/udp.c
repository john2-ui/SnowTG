#include "udp.h"
#include "arp.h"
#include "log.h"
#include "net_addr.h"
#include "net_context.h"
#include "ring.h"

#include <netinet/in.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_ring.h>
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

int udp_handle(struct rte_mempool *mp, struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);

        // uint16_t dgram_len = rte_be_to_cpu_16(udp->dgram_len);
        // uint16_t payload_len = dgram_len - sizeof(struct rte_udp_hdr);

        // LOG_INFO("udp " IP_FMT ":%u -> " IP_FMT ":%u len=%u",
        //          IP_ARG(ip->src_addr), rte_be_to_cpu_16(udp->src_port),
        //          IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
        //          payload_len);

        // /* Echo the payload back to the sender with source/destination
        // swapped.
        //  */
        // struct rte_mbuf *reply = udp_build_pkt(
        //     mp, eth->src_addr.addr_bytes, ip->dst_addr, ip->src_addr,
        //     udp->dst_port, udp->src_port, (uint8_t *)(udp + 1), payload_len);
        // if (reply != NULL)
        //         rte_ring_mp_enqueue_burst(out, (void **)&reply, 1, NULL);

        // rte_pktmbuf_free(mbuf);

        uint16_t payload_len =
            rte_be_to_cpu_16(udp->dgram_len) - sizeof(struct rte_udp_hdr);

        LOG_INFO("udp rx " IP_FMT ":%u -> " IP_FMT ":%u payload=%u",
                 IP_ARG(ip->src_addr), rte_be_to_cpu_16(udp->src_port),
                 IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                 payload_len);

        struct local_addr *addr = get_local_addr_from_ip_port(
            ip->dst_addr, udp->dst_port, ip->next_proto_id);
        if (addr != NULL)
                arp_table_add(ip->src_addr, eth->src_addr.addr_bytes);

        if (addr == NULL) {
                LOG_WARN("no socket for " IP_FMT ":%u proto=%u",
                         IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                         ip->next_proto_id);
                for (struct local_addr *s = g_local_addr; s != NULL;
                     s = s->next) {
                        LOG_WARN("  bound fd=%d " IP_FMT ":%u proto=%u", s->fd_,
                                 IP_ARG(s->local_ip_),
                                 rte_be_to_cpu_16(s->local_port_),
                                 s->protocol_);
                }
                rte_pktmbuf_free(mbuf);
                return -1;
        }

        if (rte_ring_sp_enqueue(addr->recv_buf_, mbuf) != 0) {
                LOG_ERROR("recv_buf full for fd=%d, dropping packet",
                          addr->fd_);
                rte_pktmbuf_free(mbuf);
                return -1;
        }

        LOG_DEBUG("delivered to fd=%d recv_buf", addr->fd_);

        pthread_mutex_lock(&addr->mutex_);
        pthread_cond_signal(&addr->cond_);
        pthread_mutex_unlock(&addr->mutex_);

        return 0;
}

int udp_out(struct rte_mempool *mp) {
        // send all packets in the send buffer
        struct local_addr *addr;
        for (addr = g_local_addr; addr != NULL; addr = addr->next) {
                struct rte_mbuf *mbuf;
                int nb_snd =
                    rte_ring_sc_dequeue(addr->send_buf_, (void **)&mbuf);
                if (nb_snd < 0)
                        continue;

                // LOG

                struct rte_ether_hdr *eth =
                    rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
                struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
                if (eth->dst_addr.addr_bytes[0] == 0xFF &&
                    eth->dst_addr.addr_bytes[1] == 0xFF &&
                    eth->dst_addr.addr_bytes[2] == 0xFF &&
                    eth->dst_addr.addr_bytes[3] == 0xFF &&
                    eth->dst_addr.addr_bytes[4] == 0xFF &&
                    eth->dst_addr.addr_bytes[5] == 0xFF) {
                        // broadcast mac
                        uint8_t *dst_mac = arp_lookup(ip->dst_addr);
                        if (dst_mac != NULL) {
                                rte_memcpy(eth->dst_addr.addr_bytes, dst_mac,
                                           RTE_ETHER_ADDR_LEN);
                        } else {
                                // send arp request to get mac
                                struct rte_mbuf *arp =
                                    arp_build_pkt(mp, RTE_ARP_OP_REQUEST,
                                                  eth->dst_addr.addr_bytes,
                                                  g_net.local_ip, ip->dst_addr);
                                if (arp == NULL) {
                                        LOG_ERROR("arp_build_pkt() failed");
                                        rte_pktmbuf_free(mbuf);
                                        continue;
                                }
                                struct inout_ring *ring = ring_instance();
                                rte_ring_mp_enqueue_burst(
                                    ring->out, (void **)&arp, 1, NULL);

                                if (rte_ring_mp_enqueue(addr->send_buf_,
                                                        mbuf) != 0) {
                                        LOG_ERROR(
                                            "send_buf full while waiting ARP");
                                        rte_pktmbuf_free(mbuf);
                                }
                                continue;
                        }
                }

                struct rte_ring *out_ring = ring_instance()->out;
                if (rte_ring_mp_enqueue_burst(out_ring, (void **)&mbuf, 1,
                                              NULL) == 0) {
                        LOG_ERROR("out ring full, dropping reply");
                        rte_pktmbuf_free(mbuf);
                        continue;
                }

                struct rte_udp_hdr *udp =
                    (struct rte_udp_hdr *)((struct rte_ipv4_hdr *)(eth + 1) +
                                           1);
                uint16_t payload_len = rte_be_to_cpu_16(udp->dgram_len) -
                                       (uint16_t)sizeof(struct rte_udp_hdr);
                const char *payload = (const char *)(udp + 1);
                LOG_INFO("udp tx " IP_FMT ":%u -> " IP_FMT ":%u payload=%u "
                         "data=%.*s",
                         IP_ARG(ip->src_addr), rte_be_to_cpu_16(udp->src_port),
                         IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                         payload_len, payload_len, payload);
        }
        return 0;
}
