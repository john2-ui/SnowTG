/**
 * @file udp.c
 * @brief UDP packet construction, ingress delivery, egress, and the udp_ops
 *        vector consumed by the unified socket layer.
 *
 * Inbound:  udp_ingress -> find socket by (ip,port,proto) -> recv_buf
 * Outbound: udp_tx_flush -> arp resolve -> out ring -> NIC
 * App:      udp_sendto builds a datagram into send_buf; udp_recvfrom pulls one
 *           from recv_buf.
 */
#include "udp.h"

#include "arp.h"
#include "log.h"
#include "net_context.h"
#include "owner_io.h"
#include "pkt_frame.h"
#include "ring.h"
#include "socket.h"
#include "socket_owner_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_ring.h>
#include <rte_udp.h>
#include <string.h>

#define UDP_SK_FMT "sock=%u gen=%u"
#define UDP_SK_ARG(sk) (sk)->id, (sk)->generation

static struct rte_ipv4_hdr *udp_ipv4_header(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        return (struct rte_ipv4_hdr *)(eth + 1);
}

static struct rte_udp_hdr *udp_header(struct rte_ipv4_hdr *ip) {
        return (struct rte_udp_hdr *)(ip + 1);
}

static int mac_is_broadcast(const uint8_t *mac) {
        return memcmp(mac, g_broadcast_mac, RTE_ETHER_ADDR_LEN) == 0;
}

struct rte_mbuf *udp_build_pkt(struct rte_mempool *mp, const uint8_t *dst_mac,
                               uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               const uint8_t *data, uint16_t data_len) {
        const size_t l4_len = sizeof(struct rte_udp_hdr) + data_len;
        void *l4 = NULL;
        struct rte_mbuf *mbuf = eth_ipv4_build(mp, dst_mac, src_ip, dst_ip,
                                               IPPROTO_UDP, l4_len, &l4);
        if (mbuf == NULL) {
                LOG_ERROR("eth_ipv4_build failed");
                return NULL;
        }

        struct rte_ipv4_hdr *ip = udp_ipv4_header(mbuf);
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)l4;
        udp->src_port = src_port;
        udp->dst_port = dst_port;
        udp->dgram_len = rte_cpu_to_be_16((uint16_t)l4_len);
        if (data_len > 0 && data != NULL)
                rte_memcpy((uint8_t *)(udp + 1), data, data_len);
        udp->dgram_cksum = 0;
        udp->dgram_cksum = rte_ipv4_udptcp_cksum(ip, udp);
        return mbuf;
}

int udp_ingress(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = udp_ipv4_header(mbuf);
        struct rte_udp_hdr *udp = udp_header(ip);
        const uint16_t l2_l3_len =
            sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr);
        const uint16_t l4_len =
            rte_be_to_cpu_16(ip->total_length) - sizeof(*ip);

        /*
         * dispatch_packet() already validated the IPv4 header and length.
         * Validate UDP's own length before reading its fields, then verify its
         * checksum when present. RFC 768 permits an IPv4 UDP checksum of zero.
         */
        if (mbuf->data_len < l2_l3_len + sizeof(*udp) ||
            l4_len < sizeof(*udp)) {
                LOG_DEBUG("dropping invalid UDP datagram");
                rte_pktmbuf_free(mbuf);
                return -1;
        }

        const uint16_t udp_len = rte_be_to_cpu_16(udp->dgram_len);
        if (udp_len != l4_len ||
            (udp->dgram_cksum != 0 &&
             rte_ipv4_udptcp_cksum_mbuf_verify(mbuf, ip, l2_l3_len) != 0)) {
                LOG_DEBUG("dropping invalid UDP datagram");
                rte_pktmbuf_free(mbuf);
                return -1;
        }

#if ENABLE_UDP_DEBUG
        uint16_t payload_len =
            rte_be_to_cpu_16(udp->dgram_len) - sizeof(struct rte_udp_hdr);
        LOG_INFO("udp rx " IP_FMT ":%u -> " IP_FMT ":%u payload=%u",
                 IP_ARG(ip->src_addr), rte_be_to_cpu_16(udp->src_port),
                 IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                 payload_len);
#endif

        struct nsock *sk =
            nsock_from_ip_port(ip->dst_addr, udp->dst_port, ip->next_proto_id);
        if (sk == NULL) {
#if ENABLE_UDP_DEBUG
                LOG_WARN("no socket for " IP_FMT ":%u proto=%u",
                         IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                         ip->next_proto_id);
#endif
                /* ingress owns every mbuf, including unmatched datagrams. */
                rte_pktmbuf_free(mbuf);
                return -1;
        }

        arp_table_add(ip->src_addr, eth->src_addr.addr_bytes);

        if (rte_ring_mp_enqueue(sk->recv_buf, mbuf) != 0) {
                LOG_ERROR("recv_buf full for " UDP_SK_FMT ", dropping packet",
                          UDP_SK_ARG(sk));
                rte_pktmbuf_free(mbuf);
                return -1;
        }
        /*
         * UDP delivery and application commands execute on the same owner.
         * Retry parked RECVFROM requests directly instead of waking an
         * application thread that would dereference sk.
         */
        socket_owner_wake_recv(sk);
        socket_owner_ready_post(sk, OWNER_IO_EV_READ);
        return 0;
}

int udp_tx_flush(struct nsock *sk, struct rte_mempool *mp) {
        struct rte_mbuf *mbuf;
        if (rte_ring_sc_dequeue(sk->send_buf, (void **)&mbuf) < 0)
                return 0;

        /* A queue slot became available for a non-blocking sender. */
        socket_owner_ready_post(sk, OWNER_IO_EV_WRITE);

        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = udp_ipv4_header(mbuf);

        if (mac_is_broadcast(eth->dst_addr.addr_bytes)) {
                uint8_t *dst_mac = arp_lookup(ip->dst_addr);
                if (dst_mac != NULL) {
                        rte_memcpy(eth->dst_addr.addr_bytes, dst_mac,
                                   RTE_ETHER_ADDR_LEN);
                } else {
                        struct rte_mbuf *arp = arp_build_pkt(
                            mp, RTE_ARP_OP_REQUEST, eth->dst_addr.addr_bytes,
                            g_net.local_ip, ip->dst_addr);
                        if (arp == NULL) {
                                LOG_ERROR("arp_build_pkt() failed");
                                rte_pktmbuf_free(mbuf);
                                return 0;
                        }
                        struct inout_ring *ring = ring_instance();
                        rte_ring_mp_enqueue_burst(ring->out, (void **)&arp, 1,
                                                  NULL);
                        if (rte_ring_mp_enqueue(sk->send_buf, mbuf) != 0) {
                                LOG_ERROR("send_buf full while waiting ARP");
                                rte_pktmbuf_free(mbuf);
                        }
                        return 0;
                }
        }

        struct rte_ring *out_ring = ring_instance()->out;
        if (rte_ring_mp_enqueue_burst(out_ring, (void **)&mbuf, 1, NULL) == 0) {
                LOG_ERROR("out ring full, dropping reply");
                rte_pktmbuf_free(mbuf);
                return 0;
        }

        struct rte_udp_hdr *udp = udp_header(ip);
        uint16_t payload_len = rte_be_to_cpu_16(udp->dgram_len) -
                               (uint16_t)sizeof(struct rte_udp_hdr);
        const char *payload = (const char *)(udp + 1);
        LOG_INFO("udp tx " IP_FMT ":%u -> " IP_FMT ":%u payload=%u data=%.*s",
                 IP_ARG(ip->src_addr), rte_be_to_cpu_16(udp->src_port),
                 IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                 payload_len, payload_len, payload);
        return 0;
}

ssize_t udp_sendto(struct nsock *sk, const void *buf, size_t len,
                   __attribute__((unused)) int flags,
                   const struct sockaddr *dest_addr,
                   __attribute__((unused)) socklen_t addrlen) {
        if (g_net.mp == NULL) {
                LOG_ERROR("mbuf pool not initialized");
                return -1;
        }
        if (dest_addr == NULL) {
                LOG_ERROR("udp_sendto: " UDP_SK_FMT " missing dest",
                          UDP_SK_ARG(sk));
                return -1;
        }

        const struct sockaddr_in *daddr = (const struct sockaddr_in *)dest_addr;
        const uint8_t *dst_mac = arp_lookup(daddr->sin_addr.s_addr);
        if (dst_mac == NULL)
                dst_mac = g_broadcast_mac;
        /* TODO: fragment payloads larger than the path MTU (IP fragmentation)
         * and reassemble fragmented datagrams on receive. Today an oversized
         * send is encoded as a single frame and there is no reassembly path. */

        struct rte_mbuf *mbuf = udp_build_pkt(
            g_net.mp, dst_mac, sk->local_ip, daddr->sin_addr.s_addr,
            sk->local_port, daddr->sin_port, buf, (uint16_t)len);
        if (mbuf == NULL) {
                errno = ENOBUFS;
                return -1;
        }

        if (rte_ring_mp_enqueue(sk->send_buf, mbuf) != 0) {
                LOG_ERROR("send_buf full for " UDP_SK_FMT, UDP_SK_ARG(sk));
                rte_pktmbuf_free(mbuf);
                errno = EAGAIN;
                return -1;
        }
        LOG_INFO("udp_sendto " UDP_SK_FMT " " IP_FMT ":%u -> " IP_FMT
                 ":%u len=%zu data=%.*s",
                 UDP_SK_ARG(sk), IP_ARG(sk->local_ip),
                 rte_be_to_cpu_16(sk->local_port),
                 IP_ARG(daddr->sin_addr.s_addr),
                 rte_be_to_cpu_16(daddr->sin_port), len, (int)len,
                 (const char *)buf);
        return (ssize_t)len;
}

ssize_t udp_recvfrom(struct nsock *sk, void *buf, size_t len,
                     __attribute__((unused)) int flags,
                     struct sockaddr *src_addr,
                     __attribute__((unused)) socklen_t *addrlen) {
        struct rte_mbuf *mbuf = NULL;
        if (rte_ring_sc_dequeue(sk->recv_buf, (void **)&mbuf) != 0) {
                /*
                 * Transport callbacks are owner-side probes and must never
                 * block the packet worker.  socket_owner.c parks a blocking
                 * command and retries it after udp_ingress queues a datagram.
                 */
                errno = EAGAIN;
                return -1;
        }

        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);

        if (src_addr) {
                struct sockaddr_in *sin = (struct sockaddr_in *)src_addr;
                sin->sin_family = AF_INET;
                sin->sin_port = udp->src_port;
                sin->sin_addr.s_addr = ip->src_addr;
        }

        size_t payload_len =
            rte_be_to_cpu_16(udp->dgram_len) - sizeof(struct rte_udp_hdr);
        void *data = (void *)(udp + 1);

        if (len < payload_len) {
                rte_memcpy(buf, data, len);
                memmove(data, (uint8_t *)data + len, payload_len - len);
                udp->dgram_len = rte_cpu_to_be_16(sizeof(struct rte_udp_hdr) +
                                                  payload_len - len);
                if (rte_ring_mp_enqueue(sk->recv_buf, mbuf) != 0) {
                        LOG_ERROR("recv_buf full while truncating, dropping");
                        rte_pktmbuf_free(mbuf);
                }
                return (ssize_t)len;
        }
        rte_memcpy(buf, data, payload_len);
        rte_pktmbuf_free(mbuf);
        return (ssize_t)payload_len;
}

static void udp_drain_ring(struct rte_ring *ring) {
        struct rte_mbuf *m;
        while (rte_ring_sc_dequeue(ring, (void **)&m) == 0)
                rte_pktmbuf_free(m);
}

int udp_close(struct nsock *sk) {
        /*
         * UDP has no wire-level teardown.  Because this callback runs on the
         * owner after fd detachment, queued datagrams can be drained and the
         * object retired immediately without racing ingress or recvfrom.
         */
        LOG_INFO("udp_close socket=%u", sk->id);
        udp_drain_ring(sk->recv_buf);
        udp_drain_ring(sk->send_buf);
        nsock_free(sk);
        return 0;
}

const struct sock_ops udp_ops = {
    .name = "udp",
    .protocol = IPPROTO_UDP,
    .ingress = udp_ingress,
    .tx_flush = udp_tx_flush,
    .send = NULL,
    .recv = NULL,
    .sendto = udp_sendto,
    .recvfrom = udp_recvfrom,
    .close = udp_close,
    .connect = NULL,
    .listen = NULL,
    .accept = NULL,
};
