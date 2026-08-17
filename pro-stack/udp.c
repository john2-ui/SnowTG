/**
 * @file udp.c
 * @brief UDP packet construction, ingress delivery, egress, and the udp_ops
 *        vector consumed by the unified socket layer.
 *
 * Inbound:  udp_ingress -> find socket by (ip,port,proto) -> ring or local RX
 * Outbound: ring-backed UDP queues mbufs for udp_tx_flush; owner-local UDP
 *           resolves ARP and sends directly to the owner output ring.
 * App:      udp_sendto splits one application buffer into bounded independent
 *           datagrams; udp_recvfrom pulls one datagram from the selected
 *           receive queue.
 */
#include "udp.h"

#include "arp.h"
#include "config.h"
#include "log.h"
#include "net_context.h"
#include "owner_io.h"
#include "pkt_frame.h"
#include "ring.h"
#include "socket.h"
#include "socket_owner_internal.h"
#include "udp_memory.h"

#include <errno.h>
#include <netinet/in.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_ring.h>
#include <rte_udp.h>
#include <string.h>

#define UDP_SK_FMT "sock=%u gen=%u"
#define UDP_SK_ARG(sk) (sk)->id, (sk)->generation

static int udp_build_datagrams(struct nsock *sk,
                               const struct sockaddr_in *daddr,
                               const uint8_t *dst_mac, const void *buf,
                               size_t len, struct rte_mbuf **packets,
                               unsigned int *count_out);

static int udp_datagram_shape(size_t len, size_t *payload_limit_out,
                              unsigned int *count_out) {
        const size_t headers =
            sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr);
        size_t payload_limit;
        size_t count;

        if (g_net.ipv4_mtu <= headers) {
                errno = ENETDOWN;
                return -1;
        }
        payload_limit = g_net.ipv4_mtu - headers;
        count = len == 0 ? 1U
                         : len / payload_limit + (len % payload_limit != 0);
        if (count > UDP_SENDTO_MAX_DATAGRAMS) {
                errno = EMSGSIZE;
                return -1;
        }
        if (payload_limit_out != NULL)
                *payload_limit_out = payload_limit;
        if (count_out != NULL)
                *count_out = (unsigned int)count;
        return 0;
}

static struct rte_ipv4_hdr *udp_ipv4_header(struct rte_mbuf *mbuf) {
        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        return (struct rte_ipv4_hdr *)(eth + 1);
}

static struct rte_udp_hdr *udp_header(struct rte_ipv4_hdr *ip) {
        return (struct rte_udp_hdr *)(ip + 1);
}

static struct udp_owner_memory *udp_memory_current(void) {
        return socket_owner_udp_memory();
}

static void udp_local_rx_record_drop(void) {
        udp_memory_record_queue_drop(udp_memory_current());
}

/**
 * @brief Enqueue one mbuf in an owner-local UDP receive FIFO.
 *
 * Only the packet worker calls this function, so the queue needs no lock or
 * DPDK ring.  The node is acquired only while a datagram is actually retained.
 */
static int udp_local_rx_enqueue(struct nsock *sk, struct rte_mbuf *mbuf) {
        struct udp_owner_memory *memory = udp_memory_current();
        struct udp_rx_node *node;

        if (sk->u.udp.rx_queue_count >= UDP_RX_QUEUE_LIMIT) {
                errno = ENOBUFS;
                return -1;
        }
        node = udp_memory_rx_node_alloc(memory);
        if (node == NULL)
                return -1;

        node->mbuf = mbuf;
        if (sk->u.udp.rx_queue_tail != NULL)
                sk->u.udp.rx_queue_tail->next = node;
        else
                sk->u.udp.rx_queue_head = node;
        sk->u.udp.rx_queue_tail = node;
        sk->u.udp.rx_queue_count++;
        return 0;
}

/** Dequeue one mbuf and immediately release its now-unused queue node. */
static struct rte_mbuf *udp_local_rx_dequeue(struct nsock *sk) {
        struct udp_owner_memory *memory = udp_memory_current();
        struct udp_rx_node *node = sk->u.udp.rx_queue_head;
        struct rte_mbuf *mbuf;

        if (node == NULL)
                return NULL;
        sk->u.udp.rx_queue_head = node->next;
        if (sk->u.udp.rx_queue_head == NULL)
                sk->u.udp.rx_queue_tail = NULL;
        if (sk->u.udp.rx_queue_count > 0)
                sk->u.udp.rx_queue_count--;

        mbuf = node->mbuf;
        node->mbuf = NULL;
        node->next = NULL;
        udp_memory_rx_node_free(memory, node);
        return mbuf;
}

/** Release every queued local UDP datagram and its metadata. */
static void udp_local_rx_drain(struct nsock *sk) {
        struct rte_mbuf *mbuf;

        while ((mbuf = udp_local_rx_dequeue(sk)) != NULL)
                rte_pktmbuf_free(mbuf);
}

/** Release the owner-held datagram, including any short-read state. */
static void udp_rx_current_release(struct nsock *sk) {
        if (sk->u.udp.rx_current != NULL)
                rte_pktmbuf_free(sk->u.udp.rx_current);
        sk->u.udp.rx_current = NULL;
        sk->u.udp.rx_current_off = 0;
}

/**
 * @brief Build and submit a local UDP datagram without retaining a TX mbuf.
 *
 * The worker output ring is the existing bounded handoff to the main lcore,
 * which owns the NIC TX burst.  It is deliberately not a socket send buffer.
 */
static ssize_t udp_sendto_local(struct nsock *sk, const void *buf, size_t len,
                                const struct sockaddr_in *daddr) {
        struct inout_ring *ring = ring_instance();
        const uint8_t *dst_mac;
        struct rte_mbuf *packets[UDP_SENDTO_MAX_DATAGRAMS];
        unsigned int count;

        if (ring == NULL || ring->out == NULL) {
                errno = ENETDOWN;
                return -1;
        }
        /*
         * A cache miss only parks the socket for an application retry.  The
         * datagram itself is not built or retained until a MAC is available.
         */
        dst_mac = arp_resolve(g_net.mp, ring->out, daddr->sin_addr.s_addr,
                              rte_get_timer_cycles());
        if (dst_mac == NULL) {
                nsock_tx_arp_wait(sk, daddr->sin_addr.s_addr);
                errno = EAGAIN;
                return -1;
        }

        if (udp_build_datagrams(sk, daddr, dst_mac, buf, len, packets,
                                &count) != 0)
                return -1;

        unsigned int enqueued = rte_ring_sp_enqueue_burst(
            ring->out, (void *const *)packets, count, NULL);
        for (unsigned int i = enqueued; i < count; i++)
                rte_pktmbuf_free(packets[i]);
        if (enqueued != count) {
                nsock_tx_record_udp_queue_drops(count - enqueued);
                LOG_DEBUG("owner-local UDP queue drop " UDP_SK_FMT
                          " enqueued=%u dropped=%u",
                          UDP_SK_ARG(sk), enqueued, count - enqueued);
        }
        return (ssize_t)len;
}

static int mac_is_broadcast(const uint8_t *mac) {
        return memcmp(mac, g_broadcast_mac, RTE_ETHER_ADDR_LEN) == 0;
}

struct rte_mbuf *udp_build_pkt(struct rte_mempool *mp, const uint8_t *dst_mac,
                               uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               const uint8_t *data, uint16_t data_len) {
        const size_t total_len = sizeof(struct rte_ether_hdr) +
                                 sizeof(struct rte_ipv4_hdr) +
                                 sizeof(struct rte_udp_hdr) + data_len;
        uint32_t data_room;

        if (mp == NULL || dst_mac == NULL)
                return NULL;
        data_room = rte_pktmbuf_data_room_size(mp);
        if (data_room <= RTE_PKTMBUF_HEADROOM ||
            total_len > data_room - RTE_PKTMBUF_HEADROOM)
                return NULL;
        if (data_len > UINT16_MAX - sizeof(struct rte_udp_hdr))
                return NULL;
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

static int udp_build_datagrams(struct nsock *sk,
                               const struct sockaddr_in *daddr,
                               const uint8_t *dst_mac, const void *buf,
                               size_t len, struct rte_mbuf **packets,
                               unsigned int *count_out) {
        size_t payload_limit;
        unsigned int count;
        size_t offset = 0;

        if (sk == NULL || daddr == NULL || dst_mac == NULL || packets == NULL ||
            count_out == NULL || (buf == NULL && len != 0)) {
                errno = EINVAL;
                return -1;
        }
        if (udp_datagram_shape(len, &payload_limit, &count) != 0)
                return -1;

        for (unsigned int i = 0; i < count; i++) {
                size_t remaining = len - offset;
                size_t chunk =
                    remaining < payload_limit ? remaining : payload_limit;
                const uint8_t *data =
                    chunk == 0 ? NULL : (const uint8_t *)buf + offset;

                packets[i] = udp_build_pkt(
                    g_net.mp, dst_mac, sk->local_ip, daddr->sin_addr.s_addr,
                    sk->local_port, daddr->sin_port, data, (uint16_t)chunk);
                if (packets[i] == NULL) {
                        for (unsigned int j = 0; j < i; j++)
                                rte_pktmbuf_free(packets[j]);
                        errno = ENOBUFS;
                        return -1;
                }
                offset += chunk;
        }
        *count_out = count;
        return 0;
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

        arp_table_confirm(ip->src_addr, eth->src_addr.addr_bytes);

        if (sk->io_mode == NSOCK_IO_OWNER_LOCAL) {
                if (udp_local_rx_enqueue(sk, mbuf) != 0) {
                        LOG_DEBUG("local UDP RX full for " UDP_SK_FMT
                                  ", dropping packet",
                                  UDP_SK_ARG(sk));
                        udp_local_rx_record_drop();
                        rte_pktmbuf_free(mbuf);
                        return -1;
                }
        } else if (rte_ring_sp_enqueue(sk->recv_buf, mbuf) != 0) {
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

        if (sk->io_mode == NSOCK_IO_OWNER_LOCAL) {
                /*
                 * Local UDP has no retained TX mbuf.  This callback is only
                 * reached after an ARP waiter is woken; let the reactor retry
                 * its request template.
                 */
                socket_owner_ready_post(sk, OWNER_IO_EV_WRITE);
                return SOCK_TX_FLUSH_IDLE;
        }

        if (rte_ring_sc_dequeue(sk->send_buf, (void **)&mbuf) < 0)
                return SOCK_TX_FLUSH_IDLE;

        /* A queue slot became available for a non-blocking sender. */
        socket_owner_ready_post(sk, OWNER_IO_EV_WRITE);

        struct rte_ether_hdr *eth =
            rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        struct rte_ipv4_hdr *ip = udp_ipv4_header(mbuf);

        if (mac_is_broadcast(eth->dst_addr.addr_bytes)) {
                struct rte_ring *out_ring = ring_instance()->out;
                const uint8_t *dst_mac = arp_resolve(mp, out_ring, ip->dst_addr,
                                                     rte_get_timer_cycles());
                if (dst_mac != NULL) {
                        rte_memcpy(eth->dst_addr.addr_bytes, dst_mac,
                                   RTE_ETHER_ADDR_LEN);
                } else {
                        if (rte_ring_sp_enqueue(sk->send_buf, mbuf) != 0) {
                                LOG_ERROR("send_buf full while waiting ARP");
                                rte_pktmbuf_free(mbuf);
                                return SOCK_TX_FLUSH_IDLE;
                        }
                        nsock_tx_arp_wait(sk, ip->dst_addr);
                        return SOCK_TX_FLUSH_ARP_WAIT;
                }
        }

        struct rte_ring *out_ring = ring_instance()->out;
        if (rte_ring_sp_enqueue(out_ring, mbuf) != 0) {
                LOG_ERROR("out ring full, retrying datagram");
                if (rte_ring_sp_enqueue(sk->send_buf, mbuf) != 0) {
                        LOG_ERROR("send_buf full while retrying datagram");
                        rte_pktmbuf_free(mbuf);
                        return SOCK_TX_FLUSH_IDLE;
                }
                return SOCK_TX_FLUSH_RETRY;
        }

#if ENABLE_UDP_DEBUG
        struct rte_udp_hdr *udp = udp_header(ip);
        uint16_t payload_len = rte_be_to_cpu_16(udp->dgram_len) -
                               (uint16_t)sizeof(struct rte_udp_hdr);
        const char *payload = (const char *)(udp + 1);
        LOG_INFO("udp tx " IP_FMT ":%u -> " IP_FMT ":%u payload=%u data=%.*s",
                 IP_ARG(ip->src_addr), rte_be_to_cpu_16(udp->src_port),
                 IP_ARG(ip->dst_addr), rte_be_to_cpu_16(udp->dst_port),
                 payload_len, payload_len, payload);
#endif
        return rte_ring_count(sk->send_buf) == 0 ? SOCK_TX_FLUSH_IDLE
                                                 : SOCK_TX_FLUSH_RETRY;
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
                errno = EINVAL;
                LOG_ERROR("udp_sendto: " UDP_SK_FMT " missing dest",
                          UDP_SK_ARG(sk));
                return -1;
        }
        if ((buf == NULL && len != 0) ||
            udp_datagram_shape(len, NULL, NULL) != 0) {
                if (buf == NULL && len != 0)
                        errno = EINVAL;
                return -1;
        }

        if (sk->local_port == 0) {
                uint32_t local_ip =
                    sk->local_ip == 0 ? g_net.local_ip : sk->local_ip;
                int rc = nsock_udp_bind_ephemeral(sk, local_ip);

                if (rc != 0) {
                        errno = rc < 0 ? -rc : EIO;
                        LOG_ERROR("udp_sendto: " UDP_SK_FMT
                                  " ephemeral bind failed errno=%d",
                                  UDP_SK_ARG(sk), errno);
                        return -1;
                }
        }

        const struct sockaddr_in *daddr = (const struct sockaddr_in *)dest_addr;
        if (sk->io_mode == NSOCK_IO_OWNER_LOCAL)
                return udp_sendto_local(sk, buf, len, daddr);

        /*
         * Resolution happens in udp_tx_flush on the packet worker.  Building
         * with a broadcast destination marks this mbuf as unresolved without
         * reading the owner-local ARP cache from the application lcore.
         */
        struct rte_mbuf *packets[UDP_SENDTO_MAX_DATAGRAMS];
        unsigned int count;
        if (udp_build_datagrams(sk, daddr, g_broadcast_mac, buf, len, packets,
                                &count) != 0)
                return -1;

        if (rte_ring_sp_enqueue_bulk(sk->send_buf, (void *const *)packets,
                                     count, NULL) != count) {
                LOG_ERROR("send_buf full for " UDP_SK_FMT, UDP_SK_ARG(sk));
                for (unsigned int i = 0; i < count; i++)
                        rte_pktmbuf_free(packets[i]);
                errno = EAGAIN;
                return -1;
        }
        nsock_tx_mark_dirty(sk);
#if ENABLE_UDP_DEBUG
        LOG_INFO("udp_sendto " UDP_SK_FMT " " IP_FMT ":%u -> " IP_FMT
                 ":%u len=%zu data=%.*s",
                 UDP_SK_ARG(sk), IP_ARG(sk->local_ip),
                 rte_be_to_cpu_16(sk->local_port),
                 IP_ARG(daddr->sin_addr.s_addr),
                 rte_be_to_cpu_16(daddr->sin_port), len, (int)len,
                 (const char *)buf);
#endif
        return (ssize_t)len;
}

ssize_t udp_recvfrom(struct nsock *sk, void *buf, size_t len,
                     __attribute__((unused)) int flags,
                     struct sockaddr *src_addr,
                     __attribute__((unused)) socklen_t *addrlen) {
        struct rte_mbuf *mbuf = sk->u.udp.rx_current;

        if (mbuf == NULL) {
                if (sk->io_mode == NSOCK_IO_OWNER_LOCAL)
                        mbuf = udp_local_rx_dequeue(sk);
                else if (rte_ring_sc_dequeue(sk->recv_buf, (void **)&mbuf) != 0)
                        mbuf = NULL;
        }
        if (mbuf == NULL) {
                /*
                 * Transport callbacks are owner-side probes and must never
                 * block the packet worker.  socket_owner.c parks a blocking
                 * command and retries it after udp_ingress queues a datagram.
                 */
                errno = EAGAIN;
                return -1;
        }
        if (sk->u.udp.rx_current == NULL) {
                sk->u.udp.rx_current = mbuf;
                sk->u.udp.rx_current_off = 0;
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

        const uint8_t *pkt = rte_pktmbuf_mtod(mbuf, const uint8_t *);
        size_t pkt_len = rte_pktmbuf_pkt_len(mbuf);
        size_t udp_offset = (size_t)((const uint8_t *)udp - pkt);
        size_t udp_total = rte_be_to_cpu_16(udp->dgram_len);
        size_t payload_len;

        /*
         * Trust only datagrams whose UDP length fits the captured mbuf.  A
         * truncated or overstated dgram_len must not be readable past pkt_len,
         * especially across short-read retries that keep the original image.
         */
        if (udp_total < sizeof(struct rte_udp_hdr) || udp_offset > pkt_len ||
            udp_total > pkt_len - udp_offset) {
                udp_rx_current_release(sk);
                errno = EPROTO;
                return -1;
        }
        payload_len = udp_total - sizeof(struct rte_udp_hdr);

        /*
         * A short read must not rewrite the wire image in the mbuf.  In
         * particular, changing dgram_len or moving the payload makes the
         * packet no longer self-consistent while rx_current owns it.  Keep
         * the original datagram and carry only the consumed-payload offset in
         * socket state.
         */
        if (sk->u.udp.rx_current_off > payload_len) {
                udp_rx_current_release(sk);
                errno = EPROTO;
                return -1;
        }

        size_t remaining = payload_len - sk->u.udp.rx_current_off;
        size_t copied = len < remaining ? len : remaining;

        if (copied > 0) {
                if (buf == NULL) {
                        errno = EFAULT;
                        return -1;
                }
                size_t data_offset =
                    udp_offset + sizeof(*udp) + sk->u.udp.rx_current_off;
                const void *source = rte_pktmbuf_read(
                    mbuf, (uint32_t)data_offset, (uint32_t)copied, buf);
                if (source == NULL) {
                        udp_rx_current_release(sk);
                        errno = EPROTO;
                        return -1;
                }
                if (source != buf)
                        rte_memcpy(buf, source, copied);
        }
        if (copied < remaining) {
                sk->u.udp.rx_current_off += copied;
                return (ssize_t)copied;
        }

        udp_rx_current_release(sk);
        return (ssize_t)copied;
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
#if ENABLE_UDP_DEBUG
        LOG_INFO("udp_close socket=%u", sk->id);
#endif
        udp_rx_current_release(sk);
        if (sk->io_mode == NSOCK_IO_OWNER_LOCAL) {
                udp_local_rx_drain(sk);
        } else {
                udp_drain_ring(sk->recv_buf);
                udp_drain_ring(sk->send_buf);
        }
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
